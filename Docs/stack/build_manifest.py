#!/usr/bin/env python3
"""
Build the UE module KG manifest for ForgeEditorBridge.

Reads ForgeEditorBridge.Build.cs, resolves each module name to its .Build.cs
under the UE 5.8 engine tree, then walks header roots to count headers and LOC.
Also scans the plugin's handlers for #include statements that resolve into each
module's header roots, so we know which handlers actually lean on which module.

Read-only on the engine. Writes only to Plugins/ForgeEditorBridge/Docs/stack/.
"""

from __future__ import annotations

import datetime as _dt
import json
import os
import re
import sys
from pathlib import Path

# ---------- Paths ----------
PLUGIN_ROOT = Path(__file__).resolve().parents[2]
PLUGIN_MODULE_ROOT = PLUGIN_ROOT / "Source" / "ForgeEditorBridge"
BUILD_CS = PLUGIN_MODULE_ROOT / "ForgeEditorBridge.Build.cs"
HANDLER_DIRS = [
    PLUGIN_MODULE_ROOT / "Private" / "Handlers",
    PLUGIN_MODULE_ROOT / "Public" / "Handlers",
]

_ENGINE_ROOT_DEFAULT = r"C:\Program Files\Epic Games\UE_5.8\Engine"
ENGINE_ROOT = Path(os.environ.get("UE_ENGINE_ROOT", _ENGINE_ROOT_DEFAULT))
ENGINE_SOURCE = ENGINE_ROOT / "Source"
ENGINE_PLUGINS = ENGINE_ROOT / "Plugins"

ENGINE_ROOT_PLACEHOLDER = "${UE_ENGINE_ROOT}"


def _to_template(path_str: str) -> str:
    """Replace the resolved engine root prefix with ${UE_ENGINE_ROOT} so the
    serialized manifest does not leak the local install path. The adapter
    re-resolves the placeholder at load time."""
    s = str(path_str)
    root = str(ENGINE_ROOT)
    if s.startswith(root):
        return ENGINE_ROOT_PLACEHOLDER + s[len(root):]
    return s


OUT_PATH = PLUGIN_ROOT / "Docs" / "stack" / "ue_kg_manifest.json"


# ---------- Step 1: Parse Build.cs ----------
def parse_build_cs(path: Path) -> tuple[list[str], list[str], list[str]]:
    """Return (public_deps, private_deps, dynamic_deps) lists, deduped, comments stripped."""
    text = path.read_text(encoding="utf-8")

    # Strip // line comments and /* */ block comments
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)

    def grab(field: str) -> list[str]:
        # Look for `<field>.AddRange(new string[] { ... });`
        pat = re.compile(
            re.escape(field) + r"\.AddRange\s*\(\s*new\s+string\s*\[\s*\]\s*\{(.*?)\}\s*\)\s*;",
            re.DOTALL,
        )
        m = pat.search(text)
        if not m:
            return []
        body = m.group(1)
        # Quoted string literals
        names = re.findall(r'"([^"]+)"', body)
        # Dedupe preserving order
        seen: set[str] = set()
        out: list[str] = []
        for n in names:
            if n not in seen:
                seen.add(n)
                out.append(n)
        return out

    public_deps = grab("PublicDependencyModuleNames")
    private_deps = grab("PrivateDependencyModuleNames")
    dynamic_deps = grab("DynamicallyLoadedModuleNames")
    return public_deps, private_deps, dynamic_deps


# ---------- Step 2: Resolve modules ----------
def index_engine_build_files(engine_root: Path) -> dict[str, list[Path]]:
    """Walk the engine and index every '<Module>.Build.cs' (and .Module.cs) by module name."""
    index: dict[str, list[Path]] = {}
    suffixes = (".Build.cs", ".Module.cs")
    # Restrict walk to known directories to keep this fast
    roots = [engine_root / "Source", engine_root / "Plugins"]
    for root in roots:
        if not root.exists():
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                for suf in suffixes:
                    if fn.endswith(suf):
                        mod = fn[: -len(suf)]
                        full = Path(dirpath) / fn
                        index.setdefault(mod, []).append(full)
                        break
    return index


def classify_module(build_cs_path: Path, engine_root: Path) -> str:
    rel = build_cs_path.relative_to(engine_root)
    parts = rel.parts
    # parts like ('Source', 'Runtime', ...) or ('Plugins', 'Runtime', ...) or ('Plugins', 'Experimental', ...)
    if parts[0] == "Source":
        if len(parts) >= 2:
            top = parts[1]
            if top in ("Runtime", "Editor", "Developer", "Programs"):
                return top
        return "Source"
    if parts[0] == "Plugins":
        return "Plugin"
    return "Unknown"


def header_roots_for(build_cs_path: Path) -> list[Path]:
    """A module's headers live in same-dir Public/ and Classes/ (and sometimes Internal/)."""
    base = build_cs_path.parent
    roots: list[Path] = []
    for sub in ("Public", "Classes", "Internal"):
        p = base / sub
        if p.exists() and p.is_dir():
            roots.append(p)
    return roots


def count_headers(roots: list[Path]) -> tuple[int, int, list[Path]]:
    """Return (header count, total LOC, list of header file paths)."""
    headers: list[Path] = []
    for r in roots:
        for dirpath, _dirnames, filenames in os.walk(r):
            for fn in filenames:
                if fn.endswith(".h"):
                    headers.append(Path(dirpath) / fn)
    loc = 0
    for h in headers:
        try:
            # Fast LOC count by counting newlines + 1 if non-empty
            with open(h, "rb") as f:
                data = f.read()
            if not data:
                continue
            nl = data.count(b"\n")
            # If file does not end with newline, count last line
            if not data.endswith(b"\n"):
                nl += 1
            loc += nl
        except OSError:
            pass
    return len(headers), loc, headers


# ---------- Step 3: Map handler includes -> modules ----------
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def gather_handler_files(handler_dirs: list[Path]) -> list[Path]:
    out: list[Path] = []
    for d in handler_dirs:
        if not d.exists():
            continue
        for fn in os.listdir(d):
            if fn.endswith((".cpp", ".h")):
                out.append(d / fn)
    return out


def extract_includes(handler_files: list[Path]) -> dict[Path, set[str]]:
    """Return {handler_path: set of include-path-strings}."""
    out: dict[Path, set[str]] = {}
    for f in handler_files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        out[f] = set(INCLUDE_RE.findall(text))
    return out


def build_header_basename_index(modules: list[dict]) -> dict[str, list[str]]:
    """
    Map header basename -> list of module names that own it (by exact filename).
    The basename is enough because UE includes are typically by filename only.
    """
    idx: dict[str, list[str]] = {}
    for m in modules:
        for hp in m.get("_header_paths", []):
            base = Path(hp).name
            idx.setdefault(base, []).append(m["module_name"])
            # Also map by relative subpath under Public/Classes for disambiguation
    return idx


def build_header_relpath_index(modules: list[dict]) -> dict[str, list[str]]:
    """Map any include-suffix that ends with the header rel path to its module(s)."""
    idx: dict[str, list[str]] = {}
    for m in modules:
        for root in m.get("_header_root_paths", []):
            root_p = Path(root)
            for hp in m.get("_header_paths", []):
                hp_p = Path(hp)
                try:
                    rel = hp_p.relative_to(root_p)
                except ValueError:
                    continue
                key = str(rel).replace("\\", "/")
                idx.setdefault(key, []).append(m["module_name"])
    return idx


# ---------- Main ----------
def main() -> int:
    if not BUILD_CS.exists():
        print(f"ERROR: Build.cs not found: {BUILD_CS}", file=sys.stderr)
        return 2

    pub, priv, dyn = parse_build_cs(BUILD_CS)
    # Combined deduped list
    all_mods: list[str] = []
    seen: set[str] = set()
    for src in (pub, priv, dyn):
        for n in src:
            if n not in seen:
                seen.add(n)
                all_mods.append(n)

    print(f"[parse] public={len(pub)} private={len(priv)} dynamic={len(dyn)} total_unique={len(all_mods)}")

    print("[index] indexing engine .Build.cs files...")
    bcs_index = index_engine_build_files(ENGINE_ROOT)
    print(f"[index] indexed {len(bcs_index)} unique module names across engine")

    modules_records: list[dict] = []
    unresolved: list[dict] = []

    total_headers = 0
    total_loc = 0

    # Build a case-insensitive lookup for fallback
    bcs_index_ci: dict[str, list[tuple[str, list[Path]]]] = {}
    for k, v in bcs_index.items():
        bcs_index_ci.setdefault(k.lower(), []).append((k, v))

    casing_mismatches: list[dict] = []

    for mod in all_mods:
        candidates = bcs_index.get(mod, [])
        ci_match: str | None = None
        if not candidates:
            # Case-insensitive fallback (UE on Windows is case-insensitive in practice;
            # the canonical module name is the .Build.cs basename)
            ci_hits = bcs_index_ci.get(mod.lower(), [])
            if ci_hits:
                # Pick the first (alphabetic by canonical name)
                ci_hits.sort(key=lambda x: x[0])
                ci_match = ci_hits[0][0]
                candidates = ci_hits[0][1]
                casing_mismatches.append({
                    "build_cs_name": mod,
                    "canonical_engine_name": ci_match,
                    "note": "Build.cs uses different casing than UE module file basename. UE/UBT on Windows is case-insensitive so it likely compiles, but should be normalized.",
                })
        if not candidates:
            unresolved.append({
                "module_name": mod,
                "reason": "no <Module>.Build.cs found under Engine/Source or Engine/Plugins (also tried case-insensitive)",
            })
            continue
        # Prefer non-Source/Programs candidates if multiple. In practice there's usually one.
        # If multiple, pick the shortest path (most canonical).
        candidates.sort(key=lambda p: (len(str(p)), str(p)))
        bcs_path = candidates[0]

        mtype = classify_module(bcs_path, ENGINE_ROOT)
        roots = header_roots_for(bcs_path)
        if not roots:
            # Some modules really do have no Public/Classes (e.g. private-only). Record but with 0.
            hcount, loc, hpaths = 0, 0, []
        else:
            hcount, loc, hpaths = count_headers(roots)

        total_headers += hcount
        total_loc += loc

        modules_records.append({
            "module_name": mod,
            "module_type": mtype,
            "module_build_cs_path": _to_template(str(bcs_path)),
            "header_roots": [_to_template(str(r)) for r in roots],
            "estimated_header_count": hcount,
            "estimated_total_loc": loc,
            "in_use_by_plugin": [],          # populated below
            # internal-only fields used for include matching, stripped before write
            "_header_paths": [str(p) for p in hpaths],
            "_header_root_paths": [str(r) for r in roots],
            "_alt_candidates": [_to_template(str(c)) for c in candidates[1:]] if len(candidates) > 1 else [],
        })

    print(f"[resolve] resolved={len(modules_records)} unresolved={len(unresolved)}")

    # ---- Map handler includes to modules ----
    handler_files = gather_handler_files(HANDLER_DIRS)
    print(f"[handlers] found {len(handler_files)} handler files")
    inc_map = extract_includes(handler_files)

    base_idx = build_header_basename_index(modules_records)
    rel_idx = build_header_relpath_index(modules_records)

    # For each handler, figure out which modules it pulls headers from
    handler_to_modules: dict[Path, set[str]] = {}
    for hf, includes in inc_map.items():
        mods_used: set[str] = set()
        for inc in includes:
            inc_norm = inc.replace("\\", "/")
            # Try full rel path match first (e.g. "MyDir/MyHeader.h")
            mods = rel_idx.get(inc_norm)
            if mods:
                mods_used.update(mods)
                continue
            # Fall back to basename match
            base = Path(inc_norm).name
            mods = base_idx.get(base)
            if mods:
                # If multiple modules own a header by the same basename, only count when unambiguous.
                # That is the common-and-correct case for UE; ambiguous basenames get skipped.
                if len(mods) == 1:
                    mods_used.update(mods)
        handler_to_modules[hf] = mods_used

    # Invert: module -> list of handler filenames
    mod_to_handlers: dict[str, list[str]] = {}
    for hf, mods in handler_to_modules.items():
        for m in mods:
            mod_to_handlers.setdefault(m, []).append(hf.name)

    for rec in modules_records:
        rec["in_use_by_plugin"] = sorted(set(mod_to_handlers.get(rec["module_name"], [])))

    # Strip internal fields before serializing
    clean_records: list[dict] = []
    for rec in modules_records:
        clean_records.append({
            "module_name": rec["module_name"],
            "module_type": rec["module_type"],
            "module_build_cs_path": rec["module_build_cs_path"],
            "header_roots": rec["header_roots"],
            "estimated_header_count": rec["estimated_header_count"],
            "estimated_total_loc": rec["estimated_total_loc"],
            "in_use_by_plugin": rec["in_use_by_plugin"],
            "alt_candidates": rec["_alt_candidates"],
        })

    manifest = {
        "manifest_schema_version": "1.0",
        "ue_version": "5.8",
        "engine_root": ENGINE_ROOT_PLACEHOLDER + "\\",
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "plugin_module": "ForgeEditorBridge",
        "plugin_build_cs": str(BUILD_CS),
        "module_count": len(clean_records),
        "header_count_total": total_headers,
        "loc_total": total_loc,
        "modules_unresolved": unresolved,
        "casing_mismatches": casing_mismatches,
        "modules": clean_records,
    }

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"[write] {OUT_PATH}")

    # ---- Step 4: Sanity report ----
    print()
    print("================ SANITY REPORT ================")
    print(f"Total modules listed in Build.cs:    {len(all_mods)}")
    print(f"Modules resolved:                    {len(clean_records)}")
    print(f"Modules unresolved:                  {len(unresolved)}")
    print(f"Total header files in scope:         {total_headers}")
    print(f"Total LOC in scope:                  {total_loc:,}")
    print()
    print("Top 5 modules by header count:")
    by_h = sorted(clean_records, key=lambda r: r["estimated_header_count"], reverse=True)[:5]
    for r in by_h:
        print(f"  {r['module_name']:<32} {r['estimated_header_count']:>5} headers  ({r['estimated_total_loc']:,} LOC)")
    print()
    print("Top 5 modules by in_use_by_plugin (handlers depending on them):")
    by_u = sorted(clean_records, key=lambda r: len(r["in_use_by_plugin"]), reverse=True)[:5]
    for r in by_u:
        print(f"  {r['module_name']:<32} {len(r['in_use_by_plugin']):>3} handlers")
    print()
    if unresolved:
        print("UNRESOLVED MODULES (Build.cs entries that did not match any engine .Build.cs):")
        for u in unresolved:
            print(f"  - {u['module_name']}: {u['reason']}")
    else:
        print("All modules resolved cleanly.")
    if casing_mismatches:
        print()
        print("CASING MISMATCHES (Build.cs name vs canonical UE module name):")
        for c in casing_mismatches:
            print(f"  - Build.cs has '{c['build_cs_name']}' -> canonical is '{c['canonical_engine_name']}'")
    print("===============================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
