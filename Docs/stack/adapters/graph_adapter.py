"""graph_adapter - Tier 2 code graph over the ForgeEditorBridge plugin source.

Backend: tree-sitter-cpp (chosen over Graphify's top-level CLI because the
graphify PyPI CLI is tuned for LLM-friendly natural-language graphs and writes
its own opinionated graph.json schema. We need a deterministic, narrow code
graph keyed on the same domain/action vocabulary that _inventory.json and the
recipes use. tree-sitter-cpp parses the same way Graphify does internally, but
lets us control the node/edge schema end-to-end.)

If tree-sitter-cpp is unavailable, falls back to a regex-only backend that
extracts dispatch tables, function definitions, and #includes. The fallback
is lossy (it cannot resolve scopes) but the dispatch-table and domain listing
it produces are the same - good enough for the Tier 0/1 consumers and the
"what calls action X" style lookup.

The adapter interface is the stable contract; the backend is swappable.
"""

from __future__ import annotations

import datetime as _dt
import json
import pickle
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

try:
    import tree_sitter_cpp as _ts_cpp  # type: ignore
    from tree_sitter import Language, Parser  # type: ignore
    _HAVE_TS = True
except Exception:
    _HAVE_TS = False

try:
    import networkx as nx  # type: ignore
    _HAVE_NX = True
except Exception:
    _HAVE_NX = False


# ---------------------------------------------------------------------------
# Regex fallback
# ---------------------------------------------------------------------------

# Matches: if (Action == TEXT("name"))   /   else if (Action == TEXT("name"))
_RE_DISPATCH = re.compile(
    r'(?P<ws>[ \t]*)(?:else\s+)?if\s*\(\s*Action\s*==\s*TEXT\("(?P<action>[^"]+)"\)\s*\)',
    re.MULTILINE,
)
# Matches: return Action_Foo(Params);  on dispatch-or-next-line
_RE_HANDLER_CALL = re.compile(r'\breturn\s+(Action_\w+)\s*\(')
_RE_FUNC_DEF = re.compile(
    r'^\s*(?:[A-Za-z_][\w:<>\s,*&]*?\s+)?'
    r'(?P<scope>[A-Za-z_]\w*)::(?P<fn>[A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:const\s*)?\{',
    re.MULTILINE,
)
_RE_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
_RE_CALL = re.compile(r'\b(?P<q>(?:[A-Za-z_]\w*::)+)?(?P<fn>[A-Za-z_]\w*)\s*\(')
_RE_DOMAIN_NAME = re.compile(
    r'GetDomainName\s*\(\s*\)\s*const\s*(?:override\s*)?\{\s*return\s*TEXT\("(?P<dom>[^"]+)"\)',
    re.MULTILINE,
)


@dataclass
class _FnDef:
    scope: str
    name: str
    file: str
    line: int
    body_start: int
    body_end: int
    body_text: str


# ---------------------------------------------------------------------------
# Adapter
# ---------------------------------------------------------------------------


class GraphAdapter:
    """Tier 2 graph interface. Wraps whichever backend is available.

    Backends (in preference order):
      - tree-sitter-cpp (preferred; full AST, accurate line numbers)
      - regex            (fallback; lossy but covers dispatch + basic callsites)

    All methods operate on cached data. Call rebuild() to refresh.
    """

    def __init__(self, plugin_src_root: str, index_cache_path: str) -> None:
        self.plugin_src_root = Path(plugin_src_root).resolve()
        self.cache_path = Path(index_cache_path).resolve()
        self._backend: str = "unknown"
        self._data: dict = {}
        # Adjacent cache for callsite lookup (pickle of nx.DiGraph).
        self._graph_pickle = self.cache_path.with_suffix(".graph.pkl")
        if self.cache_path.exists():
            try:
                self._data = json.loads(self.cache_path.read_text(encoding="utf-8"))
                self._backend = self._data.get("backend", "unknown")
            except Exception:
                self._data = {}

    # -- public contract ---------------------------------------------------

    def rebuild(self) -> dict:
        """Rebuild the graph from source. Writes cache. Returns stats."""
        files = self._collect_cpp_files()
        if _HAVE_TS:
            data = self._build_ts(files)
            self._backend = "tree-sitter"
        else:
            data = self._build_regex(files)
            self._backend = "regex"
        data["backend"] = self._backend
        data["last_build_iso"] = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self._data = data
        self._write_cache()
        return self.graph_stats()

    def query_caller(self, fn_name: str) -> list[dict]:
        """Return [{file, line, caller_fn, context}] for all callers of fn_name.

        Matches exact name or Class::Method qualified form. Case sensitive.
        """
        out: list[dict] = []
        target_q = fn_name if "::" in fn_name else None
        target_n = fn_name.rsplit("::", 1)[-1]
        for caller, hits in self._data.get("calls", {}).items():
            for h in hits:
                called_q = h.get("qualified")
                called_n = h.get("name")
                if target_q:
                    if called_q != target_q:
                        continue
                else:
                    if called_n != target_n:
                        continue
                out.append({
                    "file": h["file"],
                    "line": h["line"],
                    "caller_fn": caller,
                    "context": h.get("context", ""),
                })
        return out

    def query_action_line(self, domain: str, action: str) -> Optional[tuple[str, int]]:
        """Return (handler_file_abs, dispatch_line) for a given domain.action."""
        for d in self._data.get("domains", []):
            if d.get("domain") != domain:
                continue
            for a in d.get("actions", []):
                if a.get("name") == action or action in (a.get("aliases") or []):
                    return (d.get("handler_file_abs", d.get("handler_file", "")),
                            int(a.get("dispatch_line", 0)))
        return None

    def list_domains(self) -> list[str]:
        return sorted(d.get("domain", "") for d in self._data.get("domains", []) if d.get("domain"))

    def list_actions(self, domain: str) -> list[dict]:
        for d in self._data.get("domains", []):
            if d.get("domain") == domain:
                return [{
                    "action": a.get("name"),
                    "dispatch_line": a.get("dispatch_line"),
                    "handler_fn": a.get("handler_fn"),
                } for a in d.get("actions", [])]
        return []

    def who_uses_ue_api(self, api_symbol: str) -> list[dict]:
        """Return [{file, line, domain, action, caller}] for plugin call sites."""
        rows: list[dict] = []
        hits = self.query_caller(api_symbol)
        fn_to_action = self._build_fn_to_action_index()
        for h in hits:
            da = fn_to_action.get(h["caller_fn"])
            rows.append({
                "file": h["file"],
                "line": h["line"],
                "domain": da[0] if da else None,
                "action": da[1] if da else None,
                "caller": h["caller_fn"],
            })
        return rows

    def graph_stats(self) -> dict:
        return {
            "nodes": self._data.get("stats", {}).get("nodes", 0),
            "edges": self._data.get("stats", {}).get("edges", 0),
            "files_indexed": self._data.get("stats", {}).get("files", 0),
            "domains": len(self._data.get("domains", [])),
            "actions": sum(len(d.get("actions", [])) for d in self._data.get("domains", [])),
            "last_build_iso": self._data.get("last_build_iso", ""),
            "backend": self._backend,
        }

    # -- internals ---------------------------------------------------------

    def _write_cache(self) -> None:
        self.cache_path.parent.mkdir(parents=True, exist_ok=True)
        self.cache_path.write_text(
            json.dumps(self._data, indent=2, ensure_ascii=False),
            encoding="utf-8",
        )
        if _HAVE_NX:
            try:
                g = nx.DiGraph()
                for caller, hits in self._data.get("calls", {}).items():
                    g.add_node(caller)
                    for h in hits:
                        target = h.get("qualified") or h.get("name")
                        g.add_edge(caller, target, file=h["file"], line=h["line"])
                with open(self._graph_pickle, "wb") as f:
                    pickle.dump(g, f)
            except Exception:
                pass

    def _collect_cpp_files(self) -> list[Path]:
        exts = (".cpp", ".h", ".hpp", ".inl")
        out: list[Path] = []
        for p in self.plugin_src_root.rglob("*"):
            if p.suffix.lower() in exts and p.is_file():
                out.append(p)
        return sorted(out)

    def _build_fn_to_action_index(self) -> dict:
        idx: dict = {}
        for d in self._data.get("domains", []):
            dom = d.get("domain")
            for a in d.get("actions", []):
                fn = a.get("handler_fn")
                if fn:
                    idx[fn] = (dom, a.get("name"))
        return idx

    # -- tree-sitter backend ----------------------------------------------

    def _build_ts(self, files: list[Path]) -> dict:
        lang = Language(_ts_cpp.language())
        parser = Parser(lang)
        domains: dict = {}
        functions: dict = {}   # scope::name -> _FnDef
        calls: dict = {}        # scope::name -> [{file,line,name,qualified,context}]
        includes: dict = {}     # file -> [include strings]
        total_nodes = 0
        total_edges = 0

        # Build header -> domain_name map from GetDomainName() overrides.
        # Key the map by absolute header path's stem AND the full header path to
        # avoid collisions if filenames repeat in subfolders.
        header_to_domain: dict = {}
        handler_headers: set[str] = set()
        for f in files:
            if f.suffix != ".h":
                continue
            if not _is_under_handlers(f):
                continue
            txt = _safe_read(f)
            m = _RE_DOMAIN_NAME.search(txt)
            if m:
                header_to_domain[f.stem] = m.group("dom")
                handler_headers.add(f.stem)

        for f in files:
            src = _safe_read(f)
            if not src:
                continue
            src_bytes = src.encode("utf-8", errors="replace")
            try:
                tree = parser.parse(src_bytes)
            except Exception:
                continue
            # Pass bytes for tree-sitter (byte-offset indexing is correct).
            fn_defs = list(_ts_iter_functions(tree.root_node, src_bytes))
            inc_list: list[str] = []
            for m in _RE_INCLUDE.finditer(src):
                inc_list.append(m.group(1))
            includes[str(f)] = inc_list

            for fd in fn_defs:
                fd.file = str(f)
                key = f"{fd.scope}::{fd.name}" if fd.scope else fd.name
                functions[key] = fd
                total_nodes += 1
                body_calls = _ts_iter_calls_in_body(fd.body_text)
                calls.setdefault(key, [])
                for c in body_calls:
                    total_edges += 1
                    # Approx line within body:
                    line = fd.body_start + c["offset_line"]
                    calls[key].append({
                        "file": str(f),
                        "line": line,
                        "name": c["name"],
                        "qualified": c["qualified"],
                        "context": c["context"],
                    })

            # Dispatch tables: only handler .cpp files (under /Handlers/) whose
            # companion header declared GetDomainName(). This filters out
            # BlueprintGraphAssembler.cpp, BridgeHandlerBase.cpp, and other
            # helpers that live under /Handlers/ but are not UBridgeHandlerBase
            # subclasses.
            if f.suffix == ".cpp" and _is_under_handlers(f):
                stem = f.stem  # e.g. ActorHandler
                dom_name = header_to_domain.get(stem)
                if dom_name is None:
                    # Skip: not a handler subclass.
                    continue
                actions = _extract_actions(src)
                dom_entry = domains.setdefault(dom_name, {
                    "domain": dom_name,
                    "handler_file": f.name,
                    "handler_file_abs": str(f),
                    "actions": [],
                })
                existing = {a["name"] for a in dom_entry["actions"]}
                for a in actions:
                    if a["name"] in existing:
                        continue
                    dom_entry["actions"].append(a)
                    existing.add(a["name"])

        # Ensure every header-declared handler (even zero-action ones like
        # quarantine) shows up in the domain list.
        for stem, dom_name in header_to_domain.items():
            if dom_name in domains:
                continue
            # Locate the matching .cpp to produce a stable handler_file ref.
            cpp_matches = [f for f in files
                           if f.suffix == ".cpp" and f.stem == stem]
            cpp = cpp_matches[0] if cpp_matches else None
            domains[dom_name] = {
                "domain": dom_name,
                "handler_file": cpp.name if cpp else f"{stem}.cpp",
                "handler_file_abs": str(cpp) if cpp else "",
                "actions": [],
            }

        domain_list = sorted(domains.values(), key=lambda d: d["domain"])
        return {
            "domains": domain_list,
            "calls": calls,
            "includes": includes,
            "stats": {
                "nodes": total_nodes,
                "edges": total_edges,
                "files": len(files),
            },
        }

    # -- regex backend -----------------------------------------------------

    def _build_regex(self, files: list[Path]) -> dict:
        domains: dict = {}
        calls: dict = {}
        includes: dict = {}
        total_nodes = 0
        total_edges = 0
        header_to_domain: dict = {}
        for f in files:
            if f.suffix != ".h" or not _is_under_handlers(f):
                continue
            txt = _safe_read(f)
            m = _RE_DOMAIN_NAME.search(txt)
            if m:
                header_to_domain[f.stem] = m.group("dom")

        for f in files:
            src = _safe_read(f)
            if not src:
                continue
            includes[str(f)] = [m.group(1) for m in _RE_INCLUDE.finditer(src)]
            for m in _RE_FUNC_DEF.finditer(src):
                total_nodes += 1
                key = f"{m.group('scope')}::{m.group('fn')}"
                calls.setdefault(key, [])
            if f.suffix == ".cpp" and _is_under_handlers(f):
                stem = f.stem
                dom_name = header_to_domain.get(stem)
                if dom_name is None:
                    continue  # not a handler subclass
                actions = _extract_actions(src)
                dom_entry = domains.setdefault(dom_name, {
                    "domain": dom_name,
                    "handler_file": f.name,
                    "handler_file_abs": str(f),
                    "actions": [],
                })
                existing = {a["name"] for a in dom_entry["actions"]}
                for a in actions:
                    if a["name"] in existing:
                        continue
                    dom_entry["actions"].append(a)
                    existing.add(a["name"])

        for stem, dom_name in header_to_domain.items():
            if dom_name in domains:
                continue
            cpp_matches = [f for f in files
                           if f.suffix == ".cpp" and f.stem == stem]
            cpp = cpp_matches[0] if cpp_matches else None
            domains[dom_name] = {
                "domain": dom_name,
                "handler_file": cpp.name if cpp else f"{stem}.cpp",
                "handler_file_abs": str(cpp) if cpp else "",
                "actions": [],
            }

        domain_list = sorted(domains.values(), key=lambda d: d["domain"])
        return {
            "domains": domain_list,
            "calls": calls,
            "includes": includes,
            "stats": {
                "nodes": total_nodes,
                "edges": total_edges,
                "files": len(files),
            },
        }


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------


def _is_under_handlers(p: Path) -> bool:
    """True if the path is under any `Handlers` directory segment.

    We use this to exclude BridgeHttpServer.cpp, BridgeSessionStore.cpp, and
    other server-layer files whose `Action == TEXT(...)` patterns are routing
    logic, not per-domain actions.
    """
    parts = {x.lower() for x in p.parts}
    return "handlers" in parts


def _safe_read(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        try:
            return p.read_text(encoding="latin-1")
        except Exception:
            return ""
    except Exception:
        return ""


def _guess_domain_from_stem(stem: str) -> str:
    """Best-effort domain name from file stem when GetDomainName() missing.

    e.g. ActorHandler -> actor. PCGGraphHandler -> pcg_graph (snake_case).
    """
    if stem.endswith("Handler"):
        stem = stem[: -len("Handler")]
    # Insert underscores before capitals; special-case consecutive caps like PCG.
    out = []
    for i, ch in enumerate(stem):
        if ch.isupper():
            if i > 0 and not (stem[i - 1].isupper() and
                              (i + 1 >= len(stem) or stem[i + 1].isupper())):
                if i > 0 and (stem[i - 1].islower() or
                              (i + 1 < len(stem) and stem[i + 1].islower())):
                    out.append("_")
            out.append(ch.lower())
        else:
            out.append(ch)
    return "".join(out).lstrip("_")


def _extract_actions(src: str) -> list[dict]:
    """Extract dispatch-table actions from a handler .cpp.

    Returns [{name, dispatch_line, handler_fn, aliases}] in source order.
    """
    lines = src.split("\n")
    out: list[dict] = []
    for m in _RE_DISPATCH.finditer(src):
        action = m.group("action")
        line_no = src.count("\n", 0, m.start()) + 1
        # Look ahead up to 8 lines for the handler call.
        handler_fn = None
        for j in range(line_no - 1, min(line_no + 7, len(lines))):
            mh = _RE_HANDLER_CALL.search(lines[j])
            if mh:
                handler_fn = mh.group(1)
                break
        out.append({
            "name": action,
            "dispatch_line": line_no,
            "handler_fn": handler_fn,
            "aliases": [],
        })
    return out


# ---------------------------------------------------------------------------
# Tree-sitter helpers (no-op if tree-sitter-cpp absent)
# ---------------------------------------------------------------------------


def _ts_iter_functions(root, src_bytes: bytes):
    """Yield _FnDef for each top-level function_definition.

    Takes `bytes` because tree-sitter uses byte offsets; indexing the Python
    str would corrupt names when the source has non-ASCII characters.
    """
    if not _HAVE_TS:
        return
    for node in _ts_walk(root):
        if node.type != "function_definition":
            continue
        decl = _child_by_type(node, "function_declarator")
        if decl is None:
            ptr = _child_by_type(node, "pointer_declarator") or \
                  _child_by_type(node, "reference_declarator")
            if ptr is not None:
                decl = _child_by_type(ptr, "function_declarator")
        if decl is None:
            continue
        name_node = decl.child_by_field_name("declarator")
        if name_node is None:
            continue
        scope = ""
        fname = ""
        if name_node.type == "qualified_identifier":
            scope_parts = []
            name_sub = None
            for c in name_node.children:
                if c.type == "namespace_identifier":
                    scope_parts.append(_bytes_text(c, src_bytes))
                elif c.type == "qualified_identifier":
                    scope_parts.append(_bytes_text(c, src_bytes))
                elif c.type in ("identifier", "destructor_name", "operator_name"):
                    name_sub = c
            scope = "::".join(scope_parts) if scope_parts else ""
            fname = _bytes_text(name_sub, src_bytes) if name_sub is not None else ""
        elif name_node.type in ("identifier", "field_identifier"):
            fname = _bytes_text(name_node, src_bytes)
        else:
            fname = _bytes_text(name_node, src_bytes)
        if not fname:
            continue
        body = _child_by_type(node, "compound_statement")
        if body is None:
            continue
        body_text = _bytes_text(body, src_bytes)
        yield _FnDef(
            scope=scope,
            name=fname,
            file="",
            line=node.start_point[0] + 1,
            body_start=body.start_point[0] + 1,
            body_end=body.end_point[0] + 1,
            body_text=body_text,
        )


def _ts_iter_calls_in_body(body_text: str):
    """Extract calls from a function body via regex over its text.

    Tree-sitter would give more accurate scope resolution, but for our needs
    (does file X call symbol Y anywhere) a regex scan over the body text is
    accurate enough and sidesteps deep AST walking per function.
    """
    out = []
    for i, line in enumerate(body_text.split("\n")):
        for m in _RE_CALL.finditer(line):
            fn = m.group("fn")
            q = (m.group("q") or "") + fn
            # Strip trivial false positives like control-flow keywords.
            if fn in _CPP_KEYWORDS:
                continue
            out.append({
                "name": fn,
                "qualified": q if "::" in q else None,
                "offset_line": i,
                "context": line.strip()[:180],
            })
    return out


_CPP_KEYWORDS = {
    "if", "else", "for", "while", "switch", "case", "return", "sizeof",
    "typeid", "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
    "throw", "new", "delete", "decltype", "alignof", "noexcept", "catch",
    "do", "break", "continue", "default", "goto", "typedef", "template",
    "try", "TEXT", "FORCEINLINE",
}


def _ts_walk(node):
    stack = [node]
    while stack:
        n = stack.pop()
        yield n
        for i in range(n.child_count - 1, -1, -1):
            stack.append(n.children[i])


def _child_by_type(node, type_name: str):
    for c in node.children:
        if c.type == type_name:
            return c
    return None


def _text(node, src: str) -> str:
    return src[node.start_byte:node.end_byte]


def _bytes_text(node, src_bytes: bytes) -> str:
    """Decode a tree-sitter node's span from source bytes.

    Tree-sitter uses byte offsets; indexing a Python str would be wrong on
    files with non-ASCII characters. Decode as UTF-8 with replacement for any
    stray bad bytes.
    """
    return src_bytes[node.start_byte:node.end_byte].decode("utf-8", errors="replace")
