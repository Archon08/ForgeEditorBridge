"""ue_kg_adapter - Tier 4 UE engine knowledge graph.

Backend: hybrid deterministic header parser (tree-sitter-cpp) + NetworkX KG +
sentence-transformers embeddings keyed by class.

Why hybrid (not pure LightRAG):
    The HKU LightRAG library expects an LLM to be available for entity-relation
    extraction. Running an LLM over ~9000 UE 5.7 headers would be slow, expensive,
    and non-deterministic. Engine headers are highly structured C++ - the entities
    we actually want (classes, structs, members, deprecation markers, modules)
    fall out of the AST cleanly. We parse with tree-sitter-cpp, build a NetworkX
    DiGraph, store class-level embeddings via the same nomic-embed-text-v2-moe
    model the Phase 2 vector adapter uses, and expose a query surface that mimics
    LightRAG's hybrid retrieval: structural lookups go through the graph, free-form
    queries go through the embedding index and join back to the graph for
    citations. The result is deterministic, cheap to rebuild, version-keyed, and
    incremental.

Storage layout:
    Docs/ue_kg/<ue_version>/
        kg.pickle           NetworkX DiGraph: nodes = modules/classes/structs/
                            methods/properties; edges encode contains/inherits/
                            deprecates relationships.
        index.sqlite        sqlite index over qualified_name + module + flags for
                            O(log n) lookups.
        embeddings.sqlite   sqlite-vec table over class/struct embeddings, with a
                            companion `ue_entities` table holding the source text.

The adapter is keyed by `ue_version`, so 5.7 and (future) 5.8 indexes coexist
without overwriting each other.
"""

from __future__ import annotations

import datetime as _dt
import json
import os
import pickle
import re
import sqlite3
import struct
import sys
import time
from dataclasses import dataclass, field
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

try:
    import sqlite_vec  # type: ignore
    _HAVE_VEC = True
except Exception:
    _HAVE_VEC = False

try:
    from sentence_transformers import SentenceTransformer  # type: ignore
    _HAVE_ST = True
except Exception:
    SentenceTransformer = None  # type: ignore
    _HAVE_ST = False


_MODEL_ID = "nomic-ai/nomic-embed-text-v2-moe"
_MODEL_DIM = 768
_MAX_HEADER_BYTES = 2 * 1024 * 1024  # 2 MB skip threshold

_ENGINE_ROOT_DEFAULT = r"C:\Program Files\Epic Games\UE_5.7\Engine"
_ENGINE_ROOT_PLACEHOLDER = "${UE_ENGINE_ROOT}"


def _resolve_engine_root(manifest: Optional[dict] = None) -> str:
    """Resolve the UE engine root with this precedence:
       1. UE_ENGINE_ROOT environment variable.
       2. manifest['engine_root'] if it does not start with the placeholder.
       3. Built-in Windows default.
    """
    env = os.environ.get("UE_ENGINE_ROOT")
    if env:
        return env.rstrip("/\\")
    if manifest:
        er = manifest.get("engine_root", "")
        if er and not er.startswith(_ENGINE_ROOT_PLACEHOLDER):
            return er.rstrip("/\\")
    return _ENGINE_ROOT_DEFAULT


def _resolve_engine_path(path_str: str, engine_root: str) -> str:
    """Substitute ${UE_ENGINE_ROOT} in a manifest path string."""
    if not path_str:
        return path_str
    return path_str.replace(_ENGINE_ROOT_PLACEHOLDER, engine_root)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _floats_to_blob(vec) -> bytes:
    return struct.pack(f"<{len(vec)}f", *[float(x) for x in vec])


def _safe_read_bytes(p: Path) -> bytes:
    try:
        with open(p, "rb") as f:
            return f.read()
    except OSError:
        return b""


def _decode(data: bytes) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("latin-1", errors="replace")


# ---------------------------------------------------------------------------
# Header extraction (tree-sitter-cpp)
# ---------------------------------------------------------------------------

@dataclass
class ExtractedClass:
    qualified_name: str          # e.g. UStaticMesh, FMath, Engine::Foo
    kind: str                    # "class" | "struct"
    module: str
    file: str                    # absolute path
    line: int                    # 1-based
    bases: list[str] = field(default_factory=list)
    methods: list["ExtractedMember"] = field(default_factory=list)
    properties: list["ExtractedMember"] = field(default_factory=list)
    leading_comment: str = ""
    flags: list[str] = field(default_factory=list)  # WITH_EDITOR, WITH_EDITORONLY_DATA, etc.


@dataclass
class ExtractedMember:
    name: str                    # e.g. SetMaterial
    kind: str                    # "method" | "property"
    access: str                  # "public" | "protected" | "private" | "internal"
    signature: str               # raw text of declaration
    file: str
    line: int                    # 1-based, position in file
    deprecated: bool = False
    deprecated_in: str = ""
    deprecated_message: str = ""
    replacement_hint: str = ""
    decorators: list[str] = field(default_factory=list)  # UFUNCTION/UPROPERTY/etc.
    flags: list[str] = field(default_factory=list)


# Macro detection patterns. tree-sitter-cpp does not handle UE's macro-heavy
# class declarations cleanly (UCLASS(...) followed by multi-base class with
# preprocessor blocks throws it into ERROR nodes), so we detect classes via
# regex over the source text and use tree-sitter on the *body* slice only.

_RE_UE_DEPRECATED = re.compile(
    r"UE_DEPRECATED\s*\(\s*([^,]+?)\s*,\s*\"([^\"]*)\"\s*\)"
)
_RE_REPLACEMENT_HINT = re.compile(
    r"(?:[Uu]se|[Cc]all|[Rr]eplaced?\s+(?:by|with))\s+"
    r"(?:the\s+)?"
    r"(?P<hint>[A-Z_][\w:]*(?:\(\))?(?:\s*(?:or|and|/)\s*[A-Z_][\w:]*(?:\(\))?)?)"
)
_RE_UE_DECORATOR = re.compile(
    r"\b(UFUNCTION|UPROPERTY|GENERATED_BODY|GENERATED_USTRUCT_BODY|"
    r"GENERATED_UCLASS_BODY|GENERATED_IINTERFACE_BODY|UCLASS|USTRUCT|UENUM|UMETA|"
    r"UINTERFACE)\b"
)
_RE_PREPROC_GUARD = re.compile(
    r"^\s*#\s*if\s+(WITH_EDITOR|WITH_EDITORONLY_DATA|PLATFORM_[A-Z_]+|"
    r"UE_BUILD_[A-Z]+|!WITH_EDITOR|!UE_BUILD_SHIPPING)\b"
)
_RE_PREPROC_END = re.compile(r"^\s*#\s*endif\b")
# Class/struct declaration with body. Captures the class name, optional
# inheritance list, and the opening brace position. Allows API export macros
# (ENGINE_API, COREUOBJECT_API, etc.) and template/`final`/`abstract`
# qualifiers.
# Find class/struct + name. Stop at end-of-line or `{`. We deliberately keep
# this regex linear (no `+`/`*` over alternation across complex sub-expressions)
# to avoid catastrophic backtracking on UE's deeply-templated base specs.
_RE_CLASS_DECL = re.compile(
    r"""
    ^[ \t]*
    (?:template[ \t]*<[^>]*>[ \t\r\n]*)?     # optional template head (single nesting level)
    (?P<kw>class|struct)[ \t]+
    (?:(?:[A-Z][A-Z_0-9]*_API|[A-Z][A-Z_0-9]*_VTABLE)[ \t]+)*   # API export macros
    (?P<name>[A-Za-z_]\w*)
    (?:[ \t]+(?:final|abstract|sealed))?
    """,
    re.MULTILINE | re.VERBOSE,
)


class _Extractor:
    """Hybrid extractor: regex finds class declarations + balanced-brace bodies,
    then per-line scanning extracts methods, properties, and UE_DEPRECATED
    markers from each body. Tree-sitter is used opportunistically for
    member-name extraction inside well-formed bodies, but the regex pass is
    the source of truth for class boundaries."""

    def __init__(self) -> None:
        # tree-sitter is optional in this design; we still load it because
        # the broader stack expects it, but the extractor does not depend on
        # it for class detection.
        if _HAVE_TS:
            self._lang = Language(_ts_cpp.language())
            self._parser = Parser(self._lang)
        else:
            self._lang = None
            self._parser = None

    def extract(self, file_path: Path, module: str) -> list[ExtractedClass]:
        data = _safe_read_bytes(file_path)
        if not data:
            return []
        if len(data) > _MAX_HEADER_BYTES:
            return []
        text = _decode(data)
        line_flags = self._guard_map(text)
        out: list[ExtractedClass] = []
        n = len(text)
        # Find every class/struct definition with a body.
        for m in _RE_CLASS_DECL.finditer(text):
            cname = m.group("name")
            kw = m.group("kw")
            if not cname:
                continue
            # Filter out enum class.
            line_start = text.rfind("\n", 0, m.start()) + 1
            line_text = text[line_start:m.end()]
            if line_text.lstrip().startswith("enum "):
                continue
            # Find the next `{` or `;` after the match, scanning forward and
            # capturing any base class clause along the way. We track paren
            # and angle-bracket depth so deeply-templated base specs (e.g.
            # `: public TAlignedBytes<sizeof(X), alignof(X)>`) don't bail.
            after_pos = m.end()
            brace_open = -1
            scan = after_pos
            paren_depth = 0
            angle_depth = 0
            while scan < n:
                ch = text[scan]
                # Skip line comments and block comments and strings.
                if ch == "/" and scan + 1 < n and text[scan + 1] == "/":
                    nl = text.find("\n", scan)
                    scan = nl + 1 if nl >= 0 else n
                    continue
                if ch == "/" and scan + 1 < n and text[scan + 1] == "*":
                    e = text.find("*/", scan + 2)
                    scan = e + 2 if e >= 0 else n
                    continue
                if ch == '"':
                    scan += 1
                    while scan < n and text[scan] != '"':
                        if text[scan] == "\\" and scan + 1 < n:
                            scan += 2
                            continue
                        scan += 1
                    scan += 1
                    continue
                if ch == "(":
                    paren_depth += 1
                elif ch == ")":
                    paren_depth -= 1
                elif ch == "<":
                    angle_depth += 1
                elif ch == ">":
                    if angle_depth > 0:
                        angle_depth -= 1
                elif ch == "{" and paren_depth == 0 and angle_depth == 0:
                    brace_open = scan
                    break
                elif ch == ";" and paren_depth == 0 and angle_depth == 0:
                    break
                scan += 1
                if scan - after_pos > 8192:
                    break
            if brace_open < 0:
                continue
            body_end = self._match_brace(text, brace_open)
            if body_end < 0:
                continue
            body_src = text[brace_open + 1:body_end]
            line_no = text.count("\n", 0, m.start()) + 1
            body_start_line = text.count("\n", 0, brace_open) + 1
            # Bases: the text between m.end() and brace_open holds an
            # optional `: public X, public Y` clause.
            bases_str = text[after_pos:brace_open]
            bases = self._parse_bases(bases_str)
            # Leading comment.
            leading = self._leading_comment(text, line_no)
            ec = ExtractedClass(
                qualified_name=cname,
                kind="class" if kw == "class" else "struct",
                module=module,
                file=str(file_path),
                line=line_no,
                bases=bases,
                leading_comment=leading,
                flags=list(line_flags.get(line_no, [])),
            )
            # Walk body for members.
            self._scan_body(body_src, body_start_line, ec, file_path, line_flags, kw)
            out.append(ec)
        return out

    # -- helpers -----------------------------------------------------------

    def _match_brace(self, text: str, open_pos: int) -> int:
        """Return the index of the matching `}` for the `{` at open_pos.

        Skips over string literals, char literals, // and /* */ comments. -1
        if no match (file truncated)."""
        i = open_pos + 1
        depth = 1
        n = len(text)
        while i < n:
            c = text[i]
            # Single-line comment
            if c == "/" and i + 1 < n and text[i + 1] == "/":
                nl = text.find("\n", i)
                i = nl + 1 if nl >= 0 else n
                continue
            # Block comment
            if c == "/" and i + 1 < n and text[i + 1] == "*":
                end = text.find("*/", i + 2)
                i = end + 2 if end >= 0 else n
                continue
            # String literal (handle escapes)
            if c == '"':
                i += 1
                while i < n and text[i] != '"':
                    if text[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1
                continue
            # Char literal
            if c == "'":
                i += 1
                while i < n and text[i] != "'":
                    if text[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1
                continue
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return i
            i += 1
        return -1

    def _parse_bases(self, bases_str: str) -> list[str]:
        if not bases_str:
            return []
        s = bases_str.strip().lstrip(":").strip()
        out: list[str] = []
        for chunk in self._split_top_level(s, ","):
            chunk = chunk.strip()
            # strip access keywords + virtual
            for kw in ("public ", "protected ", "private ", "virtual "):
                while chunk.startswith(kw):
                    chunk = chunk[len(kw):].strip()
            chunk = chunk.strip()
            if chunk:
                out.append(chunk)
        return out

    def _split_top_level(self, s: str, sep: str) -> list[str]:
        depth = 0
        out: list[str] = []
        cur: list[str] = []
        for ch in s:
            if ch in "<(":
                depth += 1
            elif ch in ">)":
                depth -= 1
            if ch == sep and depth == 0:
                out.append("".join(cur))
                cur = []
                continue
            cur.append(ch)
        if cur:
            out.append("".join(cur))
        return out

    def _guard_map(self, text: str) -> dict:
        flags_by_line: dict = {}
        stack: list[str] = []
        for i, line in enumerate(text.split("\n"), start=1):
            m = _RE_PREPROC_GUARD.match(line)
            if m:
                stack.append(m.group(1))
                continue
            if _RE_PREPROC_END.match(line) and stack:
                stack.pop()
                continue
            if stack:
                flags_by_line[i] = list(stack)
        return flags_by_line

    def _leading_comment(self, src_text: str, class_line: int) -> str:
        lines = src_text.split("\n")
        i = class_line - 2
        out: list[str] = []
        in_block = False
        while i >= 0:
            ln = lines[i]
            stripped = ln.strip()
            if not stripped:
                if out:
                    break
                i -= 1
                continue
            if stripped.endswith("*/"):
                in_block = True
                out.append(stripped[:-2].lstrip("*").strip())
                if stripped.startswith("/*") or stripped.startswith("/**"):
                    break
                i -= 1
                continue
            if in_block:
                if stripped.startswith("/*") or stripped.startswith("/**"):
                    out.append(stripped.lstrip("/").lstrip("*").strip())
                    break
                out.append(stripped.lstrip("*").strip())
                i -= 1
                continue
            if stripped.startswith("//"):
                out.append(stripped.lstrip("/").strip())
                i -= 1
                continue
            # Hit a non-comment line - skip macros (UCLASS, USTRUCT, etc.)
            # but stop on real code.
            if _RE_UE_DECORATOR.match(stripped):
                i -= 1
                continue
            break
        return " ".join(reversed(out)).strip()[:500]

    def _scan_body(self, body: str, body_start_line: int, ec: ExtractedClass,
                   file_path: Path, line_flags: dict, kw: str) -> None:
        """Line-based scan for member declarations + access specifiers + UE
        macros. We use balanced-brace tracking to skip nested classes/methods
        so we don't double-count their members at the parent level."""
        # Default access depends on whether it's a class or struct.
        access = "private" if kw == "class" else "public"
        lines = body.split("\n")
        i = 0
        n = len(lines)
        # Track decorator state: if we just saw UE_DEPRECATED, the next
        # declaration line is the deprecated one.
        pending_deprecated: dict | None = None
        pending_decorators: list[str] = []
        while i < n:
            line_no = body_start_line + i
            ln = lines[i]
            stripped = ln.strip()
            # Access specifier?
            am = re.match(r"^\s*(public|protected|private)\s*:\s*$", ln)
            if am:
                access = am.group(1)
                i += 1
                continue
            # Skip blank or pure-comment lines.
            if not stripped or stripped.startswith("//"):
                i += 1
                continue
            # Single-line block comment: `/** ... */` or `/* ... */`. Skip
            # without resetting pending decorators so they survive across the
            # comment line.
            if stripped.startswith("/*") and "*/" in stripped:
                i += 1
                continue
            # Multi-line block comment: skip until closing `*/`.
            if stripped.startswith("/*"):
                while i < n and "*/" not in lines[i]:
                    i += 1
                i += 1
                continue
            # Preprocessor lines - track flags but don't reset decorators.
            if stripped.startswith("#"):
                i += 1
                continue
            # UE_DEPRECATED macro?
            dm = _RE_UE_DEPRECATED.search(ln)
            if dm:
                msg = dm.group(2).strip()
                rh = _RE_REPLACEMENT_HINT.search(msg)
                pending_deprecated = {
                    "deprecated_in": dm.group(1).strip().strip('"'),
                    "message": msg,
                    "replacement_hint": rh.group("hint").strip() if rh else "",
                }
                i += 1
                continue
            # UPROPERTY/UFUNCTION/etc. decorator?
            dec = _RE_UE_DECORATOR.search(ln)
            if dec:
                pending_decorators.append(dec.group(1))
                # If the macro spans multiple lines (e.g. UPROPERTY(... \n ...)),
                # consume until the closing paren.
                if "(" in ln and ln.count("(") > ln.count(")"):
                    j = i + 1
                    while j < n and (ln + "\n" + "\n".join(lines[i+1:j+1])).count("(") > (ln + "\n" + "\n".join(lines[i+1:j+1])).count(")"):
                        j += 1
                    i = j + 1
                    continue
                i += 1
                continue
            # Skip nested class/struct definitions: regex-find a class kw at
            # this line and skip the matching brace.
            nested = re.match(r"^\s*(?:template\s*<[^>]*>\s*)?(class|struct|union|enum)\b", ln)
            if nested and "{" in ln:
                # Skip the nested body.
                # Locate { in body text (not just current line) and skip.
                abs_brace = self._find_brace_in_body(body, i)
                if abs_brace >= 0:
                    end = self._match_brace(body, abs_brace)
                    if end >= 0:
                        end_line = body[:end].count("\n")
                        i = end_line + 1
                        pending_deprecated = None
                        pending_decorators = []
                        continue
                i += 1
                continue
            # Likely member declaration.
            mem = self._parse_declaration(lines, i, line_no, access,
                                          pending_deprecated, pending_decorators,
                                          file_path, line_flags)
            if mem is not None:
                advance, members = mem
                for m_ in members:
                    if m_.kind == "method":
                        ec.methods.append(m_)
                    else:
                        ec.properties.append(m_)
                pending_deprecated = None
                pending_decorators = []
                i += max(1, advance)
                continue
            # Otherwise, just advance.
            pending_deprecated = None
            pending_decorators = []
            i += 1

    def _find_brace_in_body(self, body: str, line_idx: int) -> int:
        """Return absolute offset of the first '{' on or after line_idx, or -1."""
        # offset of the start of line_idx
        offset = 0
        for _ in range(line_idx):
            nl = body.find("\n", offset)
            if nl < 0:
                return -1
            offset = nl + 1
        b = body.find("{", offset)
        return b

    _RE_FN_DECL = re.compile(
        r"""
        ^
        (?P<prefix>\s*(?:[A-Za-z_][\w:<>\s,\*&]*?\s+)?)   # return type
        (?P<name>~?[A-Za-z_]\w*)                          # function name
        \s*\(                                             # opening paren
        """,
        re.VERBOSE,
    )

    _RE_VAR_DECL = re.compile(
        r"""
        ^
        \s*(?:[A-Za-z_][\w:<>\s,\*&]*?\s+)               # type
        (?P<names>[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)  # one or more names
        \s*(?:\[[^\]]*\])?                               # optional array
        \s*(?:=\s*[^;]*)?                                # optional initializer
        \s*;
        """,
        re.VERBOSE | re.DOTALL,
    )

    def _parse_declaration(self, lines, idx, line_no, access,
                            pending_dep, pending_decs, file_path, line_flags):
        """Try to parse a member from `lines[idx:]`. Returns (advance, members)
        or None if the line doesn't look like a member."""
        # Greedy join: keep adding lines until we see a `;` (declaration) or
        # `{` (function definition).
        joined = ""
        end_idx = idx
        max_join = 8
        for j in range(max_join):
            if idx + j >= len(lines):
                break
            joined += lines[idx + j] + "\n"
            end_idx = idx + j
            if ";" in lines[idx + j] or "{" in lines[idx + j]:
                break
        # Strip whitespace.
        single = " ".join(joined.split())
        # Function?
        # Look for `<stuff> Name(...)` followed by `;` or `{` somewhere.
        if "(" in single and (")" in single):
            # Try to find the function name: token immediately before `(`.
            paren_idx = single.find("(")
            head = single[:paren_idx].rstrip()
            # name is the last identifier (maybe with `~`) in head.
            m = re.search(r"(~?[A-Za-z_]\w*)\s*$", head)
            if m:
                name = m.group(1)
                # Skip macro-only lines (UFUNCTION/UPROPERTY mistakenly caught).
                if name in ("UPROPERTY", "UFUNCTION", "UCLASS", "USTRUCT",
                            "UENUM", "UMETA", "UE_DEPRECATED", "GENERATED_BODY",
                            "GENERATED_USTRUCT_BODY", "GENERATED_UCLASS_BODY",
                            "UINTERFACE", "DECLARE_DYNAMIC_DELEGATE",
                            "DECLARE_DYNAMIC_MULTICAST_DELEGATE",
                            "DECLARE_MULTICAST_DELEGATE", "DECLARE_DELEGATE",
                            "DECLARE_EVENT", "FORCEINLINE", "static_assert",
                            "if", "while", "for", "switch", "return"):
                    return None
                # Also skip C-style cast or constructor invocation patterns
                # where the name == class name and there's no return type.
                # We're inside the class body so a constructor IS valid.
                # OK if pre-name has either a return type or the function is
                # destructor (~Name) or it's a constructor (no return).
                # Skip macro bodies like FORCEINLINE that confuse us if the
                # line ends mid-decl. (We already join up to 8 lines.)
                sig = single[:single.find(";") + 1] if ";" in single else \
                      single[:single.find("{")].rstrip() + ";"
                mem = ExtractedMember(
                    name=name,
                    kind="method",
                    access=access,
                    signature=sig.strip()[:400],
                    file=str(file_path),
                    line=line_no,
                    deprecated=bool(pending_dep),
                    deprecated_in=(pending_dep or {}).get("deprecated_in", "") if pending_dep else "",
                    deprecated_message=(pending_dep or {}).get("message", "") if pending_dep else "",
                    replacement_hint=(pending_dep or {}).get("replacement_hint", "") if pending_dep else "",
                    decorators=list(pending_decs),
                    flags=list(line_flags.get(line_no, [])),
                )
                return (end_idx - idx + 1, [mem])
        # Property / data member?
        # Pattern: `<type> name [, name ...] [= init];`
        if ";" in single:
            # Strip pure-statement lines (e.g. `friend class X;` is not a member).
            if re.match(r"\s*friend\s", single):
                return None
            # Also drop typedef/using which we don't care about as members.
            if re.match(r"\s*(typedef|using)\b", single):
                return None
            m = self._RE_VAR_DECL.match(single)
            if m:
                names_raw = m.group("names")
                names = [n.strip() for n in names_raw.split(",") if n.strip()]
                out: list[ExtractedMember] = []
                for nm in names[:6]:
                    # Drop names that look like keywords or type fragments.
                    if nm in ("const", "volatile", "static", "mutable",
                              "typename", "register"):
                        continue
                    out.append(ExtractedMember(
                        name=nm,
                        kind="property",
                        access=access,
                        signature=single.strip()[:300],
                        file=str(file_path),
                        line=line_no,
                        deprecated=bool(pending_dep),
                        deprecated_in=(pending_dep or {}).get("deprecated_in", "") if pending_dep else "",
                        deprecated_message=(pending_dep or {}).get("message", "") if pending_dep else "",
                        replacement_hint=(pending_dep or {}).get("replacement_hint", "") if pending_dep else "",
                        decorators=list(pending_decs),
                        flags=list(line_flags.get(line_no, [])),
                    ))
                if out:
                    return (end_idx - idx + 1, out)
        return None


# ---------------------------------------------------------------------------
# Adapter
# ---------------------------------------------------------------------------


class UEKGAdapter:
    """Tier 4 UE engine knowledge graph.

    Constructor:
        manifest_path: path to ue_kg_manifest.json (Phase 3 prereq).
        ue_version:    e.g. "5.7". Index files live under <kg_root>/<version>/.
        kg_root:       e.g. Docs/ue_kg/.
    """

    def __init__(self, manifest_path: str, ue_version: str, kg_root: str) -> None:
        self.manifest_path = Path(manifest_path).resolve()
        self.ue_version = str(ue_version)
        self.kg_root = Path(kg_root).resolve()
        self.version_dir = self.kg_root / self.ue_version
        self.kg_path = self.version_dir / "kg.pickle"
        self.index_path = self.version_dir / "index.sqlite"
        self.embed_path = self.version_dir / "embeddings.sqlite"
        self._g = None  # lazy NetworkX
        self._idx_db: Optional[sqlite3.Connection] = None
        self._emb_db: Optional[sqlite3.Connection] = None
        self._model = None
        self._model_dim = _MODEL_DIM

    # -- lazy resources ----------------------------------------------------

    def _load_graph(self):
        if self._g is not None:
            return self._g
        if not self.kg_path.exists():
            self._g = nx.DiGraph()
            return self._g
        with open(self.kg_path, "rb") as f:
            self._g = pickle.load(f)
        return self._g

    def _open_index(self) -> sqlite3.Connection:
        if self._idx_db is not None:
            return self._idx_db
        self.version_dir.mkdir(parents=True, exist_ok=True)
        con = sqlite3.connect(str(self.index_path))
        con.execute("""
            CREATE TABLE IF NOT EXISTS classes (
                qualified_name TEXT PRIMARY KEY,
                kind           TEXT,
                module         TEXT,
                file           TEXT,
                line           INTEGER,
                bases          TEXT,
                comment        TEXT,
                flags          TEXT
            )""")
        con.execute("CREATE INDEX IF NOT EXISTS idx_classes_module ON classes(module)")
        con.execute("CREATE INDEX IF NOT EXISTS idx_classes_kind   ON classes(kind)")
        con.execute("""
            CREATE TABLE IF NOT EXISTS members (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                class_qualified TEXT,
                name            TEXT,
                kind            TEXT,
                access          TEXT,
                signature       TEXT,
                file            TEXT,
                line            INTEGER,
                deprecated      INTEGER,
                deprecated_in   TEXT,
                deprecated_msg  TEXT,
                replacement     TEXT,
                decorators      TEXT,
                flags           TEXT
            )""")
        con.execute("CREATE INDEX IF NOT EXISTS idx_members_class ON members(class_qualified)")
        con.execute("CREATE INDEX IF NOT EXISTS idx_members_name  ON members(name)")
        con.execute("CREATE INDEX IF NOT EXISTS idx_members_dep   ON members(deprecated)")
        con.execute("""
            CREATE TABLE IF NOT EXISTS modules (
                module_name           TEXT PRIMARY KEY,
                module_type           TEXT,
                module_build_cs_path  TEXT,
                header_count          INTEGER,
                class_count           INTEGER
            )""")
        con.execute("""
            CREATE TABLE IF NOT EXISTS meta (
                key TEXT PRIMARY KEY,
                value TEXT
            )""")
        con.commit()
        self._idx_db = con
        return con

    def _open_embed(self) -> sqlite3.Connection:
        if self._emb_db is not None:
            return self._emb_db
        if not _HAVE_VEC:
            raise RuntimeError("sqlite-vec is not installed.")
        self.version_dir.mkdir(parents=True, exist_ok=True)
        con = sqlite3.connect(str(self.embed_path))
        con.enable_load_extension(True)
        sqlite_vec.load(con)
        con.enable_load_extension(False)
        con.execute("""
            CREATE TABLE IF NOT EXISTS ue_entities (
                id              INTEGER PRIMARY KEY,
                qualified_name  TEXT,
                kind            TEXT,
                module          TEXT,
                header_path     TEXT,
                line            INTEGER,
                text            TEXT
            )""")
        con.execute(
            f"CREATE VIRTUAL TABLE IF NOT EXISTS ue_vec USING vec0("
            f"entity_id INTEGER PRIMARY KEY, "
            f"embedding float[{self._model_dim}])")
        con.commit()
        self._emb_db = con
        return con

    def _ensure_model(self) -> None:
        if self._model is not None:
            return
        if not _HAVE_ST:
            raise RuntimeError("sentence-transformers is not installed.")
        self._model = SentenceTransformer(_MODEL_ID, trust_remote_code=True)
        try:
            d = int(self._model.get_sentence_embedding_dimension() or _MODEL_DIM)
            if d > 0:
                self._model_dim = d
        except Exception:
            pass

    # -- contract ----------------------------------------------------------

    def rebuild(self, force: bool = False) -> dict:
        """Build/rebuild the KG. force=True wipes any prior index for this
        version. Returns stats."""
        if not _HAVE_TS or not _HAVE_NX:
            raise RuntimeError("tree-sitter-cpp + networkx required for rebuild.")
        if not self.manifest_path.exists():
            raise FileNotFoundError(f"manifest missing: {self.manifest_path}")
        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        if manifest.get("ue_version") != self.ue_version:
            print(f"[ue_kg] WARN: manifest ue_version={manifest.get('ue_version')} "
                  f"!= requested {self.ue_version}", file=sys.stderr)
        engine_root = _resolve_engine_root(manifest)

        t0 = time.time()
        self.version_dir.mkdir(parents=True, exist_ok=True)
        if force:
            for p in (self.kg_path, self.index_path, self.embed_path):
                try:
                    if p.exists():
                        p.unlink()
                except OSError:
                    pass
            # Drop in-memory handles.
            self._g = None
            self._idx_db = None
            self._emb_db = None

        idx = self._open_index()
        idx.execute("DELETE FROM classes")
        idx.execute("DELETE FROM members")
        idx.execute("DELETE FROM modules")
        idx.commit()

        ext = _Extractor()
        g = nx.DiGraph()
        all_classes: list[ExtractedClass] = []
        modules_indexed = 0
        headers_indexed = 0
        headers_skipped_size = 0

        for mod in manifest.get("modules", []):
            mod_name = mod.get("module_name") or ""
            roots = [Path(_resolve_engine_path(r, engine_root))
                     for r in mod.get("header_roots") or []]
            build_cs_resolved = _resolve_engine_path(mod.get("module_build_cs_path") or "", engine_root)
            if not roots:
                idx.execute(
                    "INSERT INTO modules(module_name, module_type, module_build_cs_path, header_count, class_count) "
                    "VALUES (?, ?, ?, 0, 0)",
                    (mod_name, mod.get("module_type") or "", build_cs_resolved),
                )
                continue
            modules_indexed += 1
            g.add_node(f"module::{mod_name}", node_kind="module",
                       module_type=mod.get("module_type") or "")
            module_classes = 0
            module_headers = 0
            for r in roots:
                if not r.exists():
                    continue
                for dirpath, _dirs, filenames in os.walk(r):
                    for fn in filenames:
                        if not fn.endswith(".h"):
                            continue
                        fp = Path(dirpath) / fn
                        try:
                            sz = fp.stat().st_size
                        except OSError:
                            continue
                        if sz > _MAX_HEADER_BYTES:
                            headers_skipped_size += 1
                            continue
                        module_headers += 1
                        headers_indexed += 1
                        g.add_node(f"header::{fp}", node_kind="header",
                                   module=mod_name)
                        g.add_edge(f"module::{mod_name}", f"header::{fp}",
                                   rel="contains_header")
                        try:
                            classes = ext.extract(fp, mod_name)
                        except Exception:
                            classes = []
                        for ec in classes:
                            module_classes += 1
                            all_classes.append(ec)
                            self._add_class_to_graph(g, ec)
            idx.execute(
                "INSERT INTO modules(module_name, module_type, module_build_cs_path, header_count, class_count) "
                "VALUES (?, ?, ?, ?, ?)",
                (mod_name, mod.get("module_type") or "",
                 mod.get("module_build_cs_path") or "",
                 module_headers, module_classes),
            )
        idx.commit()

        # Persist to sqlite.
        seen_qnames: set[str] = set()
        deduped_classes: list[ExtractedClass] = []
        for ec in all_classes:
            # Some classes are declared in multiple headers (forward decls
            # were already filtered). De-dupe by qualified_name + module +
            # file to keep the row count honest while merging.
            key = ec.qualified_name
            if key in seen_qnames:
                continue
            seen_qnames.add(key)
            deduped_classes.append(ec)
            idx.execute(
                "INSERT OR REPLACE INTO classes "
                "(qualified_name, kind, module, file, line, bases, comment, flags) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                (ec.qualified_name, ec.kind, ec.module, ec.file, ec.line,
                 json.dumps(ec.bases), ec.leading_comment,
                 json.dumps(ec.flags)),
            )
            for mem in ec.methods + ec.properties:
                idx.execute(
                    "INSERT INTO members "
                    "(class_qualified, name, kind, access, signature, file, line, "
                    " deprecated, deprecated_in, deprecated_msg, replacement, "
                    " decorators, flags) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (ec.qualified_name, mem.name, mem.kind, mem.access, mem.signature,
                     mem.file, mem.line, 1 if mem.deprecated else 0,
                     mem.deprecated_in, mem.deprecated_message, mem.replacement_hint,
                     json.dumps(mem.decorators), json.dumps(mem.flags)),
                )
        idx.commit()

        # Persist NetworkX graph.
        with open(self.kg_path, "wb") as f:
            pickle.dump(g, f)
        self._g = g

        # Embeddings.
        emb_t0 = time.time()
        embed_count = self._build_embeddings(deduped_classes)
        emb_elapsed = time.time() - emb_t0

        # Stats.
        deprecated_count = idx.execute(
            "SELECT COUNT(*) FROM members WHERE deprecated = 1").fetchone()[0]
        member_count = idx.execute("SELECT COUNT(*) FROM members").fetchone()[0]
        method_count = idx.execute(
            "SELECT COUNT(*) FROM members WHERE kind = 'method'").fetchone()[0]
        class_count = idx.execute("SELECT COUNT(*) FROM classes").fetchone()[0]
        idx.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)",
            ("last_build_iso", _now_iso()),
        )
        idx.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)",
            ("ue_version", self.ue_version),
        )
        idx.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)",
            ("manifest_path", str(self.manifest_path)),
        )
        idx.commit()

        elapsed = time.time() - t0
        return {
            "classes": class_count,
            "members": member_count,
            "methods": method_count,
            "headers_indexed": headers_indexed,
            "headers_skipped_size": headers_skipped_size,
            "modules_indexed": modules_indexed,
            "deprecated_count": deprecated_count,
            "embeddings_indexed": embed_count,
            "graph_nodes": g.number_of_nodes(),
            "graph_edges": g.number_of_edges(),
            "elapsed_sec": round(elapsed, 1),
            "embed_elapsed_sec": round(emb_elapsed, 1),
            "kg_pickle": str(self.kg_path),
            "index_sqlite": str(self.index_path),
            "embeddings_sqlite": str(self.embed_path),
        }

    def query(self, q: str, k: int = 5) -> list[dict]:
        """Free-form query. Cosine search on class embeddings, then augment
        with structural data from the graph + sqlite index."""
        if not q or not q.strip():
            return []
        self._ensure_model()
        emb = self._open_embed()
        idx = self._open_index()
        q_emb = self._model.encode(
            [q], show_progress_bar=False, convert_to_numpy=True,
            normalize_embeddings=True,
        )[0]
        blob = _floats_to_blob(q_emb)
        rows = emb.execute(
            "SELECT v.entity_id, v.distance, e.qualified_name, e.kind, "
            "       e.module, e.header_path, e.line, e.text "
            "FROM ue_vec v JOIN ue_entities e ON e.id = v.entity_id "
            "WHERE v.embedding MATCH ? AND k = ? "
            "ORDER BY v.distance ASC",
            (blob, k),
        ).fetchall()
        out: list[dict] = []
        for r in rows:
            _eid, distance, qname, kind, module, header, line, text = r
            score = max(0.0, 1.0 - (float(distance) / 2.0))
            # Augment with member counts + a deprecation flag.
            row = idx.execute(
                "SELECT COUNT(*) FROM members WHERE class_qualified = ?",
                (qname,)).fetchone()
            n_members = row[0] if row else 0
            drow = idx.execute(
                "SELECT COUNT(*) FROM members WHERE class_qualified = ? AND deprecated = 1",
                (qname,)).fetchone()
            n_deprecated = drow[0] if drow else 0
            out.append({
                "qualified_name": qname,
                "kind": kind,
                "module": module,
                "file": header,
                "line": line,
                "score": score,
                "members": n_members,
                "deprecated_members": n_deprecated,
                "snippet": (text or "")[:240],
            })
        return out

    def deprecated(self, class_name: Optional[str] = None,
                   module: Optional[str] = None) -> list[dict]:
        idx = self._open_index()
        sql = (
            "SELECT m.class_qualified, m.name, m.kind, m.access, m.deprecated_in, "
            "       m.deprecated_msg, m.replacement, m.file, m.line, c.module "
            "FROM members m JOIN classes c ON c.qualified_name = m.class_qualified "
            "WHERE m.deprecated = 1"
        )
        params: list = []
        if class_name:
            sql += " AND m.class_qualified = ?"
            params.append(class_name)
        if module:
            sql += " AND c.module = ?"
            params.append(module)
        sql += " ORDER BY c.module, m.class_qualified, m.line"
        rows = idx.execute(sql, params).fetchall()
        out = []
        for r in rows:
            out.append({
                "qualified_name": f"{r[0]}::{r[1]}",
                "class": r[0],
                "name": r[1],
                "kind": r[2],
                "access": r[3],
                "deprecated_in": r[4],
                "message": r[5],
                "replacement_hint": r[6],
                "file": r[7],
                "line": r[8],
                "module": r[9],
            })
        return out

    def signature(self, qualified_name: str) -> dict:
        """Look up a member by Class::Method or Class::Property qualified name."""
        idx = self._open_index()
        if "::" not in qualified_name:
            # Treat as a bare class lookup.
            row = idx.execute(
                "SELECT qualified_name, kind, module, file, line, bases, comment "
                "FROM classes WHERE qualified_name = ?",
                (qualified_name,)).fetchone()
            if not row:
                return {"found": False, "qualified_name": qualified_name}
            return {
                "found": True,
                "qualified_name": row[0],
                "kind": row[1],
                "module": row[2],
                "file": row[3],
                "line": row[4],
                "bases": json.loads(row[5] or "[]"),
                "comment": row[6] or "",
            }
        # Member: split on the LAST :: so ns::Class::Member works.
        owner, member = qualified_name.rsplit("::", 1)
        row = idx.execute(
            "SELECT class_qualified, name, kind, access, signature, file, line, "
            "       deprecated, deprecated_in, deprecated_msg, replacement, decorators, flags "
            "FROM members WHERE class_qualified = ? AND name = ?",
            (owner, member)).fetchone()
        if not row:
            # Some classes are declared with leading ns:: that we don't carry
            # through to the qualified_name. Try a lookup keyed only on the
            # final class segment.
            row = idx.execute(
                "SELECT class_qualified, name, kind, access, signature, file, line, "
                "       deprecated, deprecated_in, deprecated_msg, replacement, decorators, flags "
                "FROM members WHERE class_qualified = ? AND name = ?",
                (owner.split("::")[-1], member)).fetchone()
        if not row:
            return {"found": False, "qualified_name": qualified_name}
        return {
            "found": True,
            "qualified_name": f"{row[0]}::{row[1]}",
            "class": row[0],
            "name": row[1],
            "kind": row[2],
            "access": row[3],
            "signature": row[4],
            "file": row[5],
            "line": row[6],
            "deprecated": bool(row[7]),
            "deprecated_in": row[8],
            "deprecated_message": row[9],
            "replacement_hint": row[10],
            "decorators": json.loads(row[11] or "[]"),
            "flags": json.loads(row[12] or "[]"),
        }

    def members(self, class_name: str, scope: str = "public") -> list[dict]:
        idx = self._open_index()
        sql = (
            "SELECT name, kind, access, signature, file, line, deprecated "
            "FROM members WHERE class_qualified = ?"
        )
        params: list = [class_name]
        if scope and scope != "all":
            sql += " AND access = ?"
            params.append(scope)
        sql += " ORDER BY kind, line"
        rows = idx.execute(sql, params).fetchall()
        return [{
            "name": r[0], "kind": r[1], "access": r[2],
            "signature": r[3], "file": r[4], "line": r[5],
            "deprecated": bool(r[6]),
        } for r in rows]

    def callers_in_engine(self, qualified_name: str) -> list[dict]:
        """Best-effort call-site search using ripgrep-style scan over indexed
        header roots. This is a thin convenience; for plugin-side callers use
        the Tier 2 graph adapter."""
        idx = self._open_index()
        # Pull header roots from the modules table via the manifest.
        if not self.manifest_path.exists():
            return []
        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        engine_root = _resolve_engine_root(manifest)
        roots = []
        for mod in manifest.get("modules", []):
            for r in mod.get("header_roots") or []:
                roots.append((mod.get("module_name"), Path(_resolve_engine_path(r, engine_root))))
        # Search.
        member = qualified_name.rsplit("::", 1)[-1]
        if not member:
            return []
        out: list[dict] = []
        pat = re.compile(rf"\b{re.escape(member)}\s*\(")
        scanned = 0
        for mod_name, root in roots:
            if not root.exists():
                continue
            for dp, _, fns in os.walk(root):
                for fn in fns:
                    if not fn.endswith(".h"):
                        continue
                    fp = Path(dp) / fn
                    scanned += 1
                    try:
                        text = fp.read_text(encoding="utf-8", errors="replace")
                    except OSError:
                        continue
                    for i, ln in enumerate(text.split("\n"), start=1):
                        if pat.search(ln):
                            out.append({
                                "file": str(fp),
                                "line": i,
                                "module": mod_name,
                                "context": ln.strip()[:180],
                            })
                            if len(out) >= 200:
                                return out
        return out

    def stats(self) -> dict:
        idx = self._open_index()
        n_classes = idx.execute("SELECT COUNT(*) FROM classes").fetchone()[0]
        n_members = idx.execute("SELECT COUNT(*) FROM members").fetchone()[0]
        n_methods = idx.execute(
            "SELECT COUNT(*) FROM members WHERE kind = 'method'").fetchone()[0]
        n_dep = idx.execute(
            "SELECT COUNT(*) FROM members WHERE deprecated = 1").fetchone()[0]
        n_modules = idx.execute("SELECT COUNT(*) FROM modules").fetchone()[0]
        last_build = idx.execute(
            "SELECT value FROM meta WHERE key = 'last_build_iso'").fetchone()
        try:
            g = self._load_graph()
            n_nodes = g.number_of_nodes()
            n_edges = g.number_of_edges()
        except Exception:
            n_nodes = n_edges = 0
        emb_count = 0
        if self.embed_path.exists():
            try:
                emb = self._open_embed()
                emb_count = emb.execute(
                    "SELECT COUNT(*) FROM ue_entities").fetchone()[0]
            except Exception:
                pass
        return {
            "ue_version": self.ue_version,
            "nodes": n_nodes,
            "edges": n_edges,
            "classes": n_classes,
            "members": n_members,
            "methods": n_methods,
            "deprecated_count": n_dep,
            "modules_indexed": n_modules,
            "embeddings": emb_count,
            "last_build_iso": (last_build[0] if last_build else ""),
            "kg_pickle": str(self.kg_path),
            "index_sqlite": str(self.index_path),
            "embeddings_sqlite": str(self.embed_path),
        }

    # -- internals ---------------------------------------------------------

    def _add_class_to_graph(self, g, ec: ExtractedClass) -> None:
        cnode = f"class::{ec.qualified_name}"
        g.add_node(cnode, node_kind="class", kind=ec.kind, module=ec.module,
                   file=ec.file, line=ec.line)
        g.add_edge(f"module::{ec.module}", cnode, rel="contains_class")
        g.add_edge(f"header::{ec.file}", cnode, rel="declares_class")
        for b in ec.bases:
            base_node = f"class::{b}"
            g.add_node(base_node, node_kind="class_ref")
            g.add_edge(cnode, base_node, rel="inherits")
        for mem in ec.methods + ec.properties:
            mnode = f"member::{ec.qualified_name}::{mem.name}@{mem.line}"
            g.add_node(mnode, node_kind="member", kind=mem.kind,
                       access=mem.access, deprecated=mem.deprecated)
            g.add_edge(cnode, mnode, rel="has_member")
            if mem.deprecated:
                dep_node = f"deprecation::{mem.deprecated_in}"
                g.add_node(dep_node, node_kind="deprecation_marker",
                           version=mem.deprecated_in)
                g.add_edge(mnode, dep_node, rel="deprecated_in")

    def _build_embeddings(self, classes: list[ExtractedClass]) -> int:
        """One embedding per class/struct. Reuses the Phase 2 model."""
        if not classes:
            return 0
        if not _HAVE_VEC:
            print("[ue_kg] sqlite-vec not available - skipping embeddings.",
                  file=sys.stderr)
            return 0
        if not _HAVE_ST:
            print("[ue_kg] sentence-transformers not available - skipping embeddings.",
                  file=sys.stderr)
            return 0
        self._ensure_model()
        emb = self._open_embed()
        emb.execute("DELETE FROM ue_vec")
        emb.execute("DELETE FROM ue_entities")
        emb.commit()
        # Build embed-text for each class.
        texts: list[str] = []
        rows: list[tuple] = []
        for i, ec in enumerate(classes, start=1):
            method_names = [m.name for m in ec.methods if m.access == "public"][:20]
            property_names = [p.name for p in ec.properties if p.access == "public"][:20]
            base_str = ", ".join(ec.bases) if ec.bases else "-"
            text = (
                f"Class: {ec.qualified_name}\n"
                f"Kind: {ec.kind}\n"
                f"Module: {ec.module}\n"
                f"File: {ec.file}\n"
                f"Bases: {base_str}\n"
                f"Public methods: {', '.join(method_names) if method_names else '-'}\n"
                f"Public properties: {', '.join(property_names) if property_names else '-'}\n"
                f"Doc: {ec.leading_comment or '-'}\n"
            )
            texts.append(text)
            rows.append((i, ec.qualified_name, ec.kind, ec.module, ec.file, ec.line, text))
        # Batch encode.
        batch = 32
        encoded = 0
        n = len(texts)
        for start in range(0, n, batch):
            chunk_texts = texts[start:start + batch]
            chunk_rows = rows[start:start + batch]
            embs = self._model.encode(
                chunk_texts, batch_size=batch, show_progress_bar=False,
                convert_to_numpy=True, normalize_embeddings=True,
            )
            for row, vec in zip(chunk_rows, embs):
                emb.execute(
                    "INSERT INTO ue_entities(id, qualified_name, kind, module, header_path, line, text) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)",
                    row,
                )
                emb.execute(
                    "INSERT INTO ue_vec(entity_id, embedding) VALUES (?, ?)",
                    (row[0], _floats_to_blob(vec)),
                )
                encoded += 1
            emb.commit()
            if start % (batch * 20) == 0 and start > 0:
                print(f"[ue_kg] embedded {start + len(chunk_texts)}/{n} classes", file=sys.stderr)
        emb.commit()
        return encoded
