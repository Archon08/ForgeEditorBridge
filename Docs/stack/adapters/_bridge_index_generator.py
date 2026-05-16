"""_bridge_index_generator - emits Docs/_bridge_index.json from the graph tier.

Called by `bridge-stack rebuild --tier graph`. Does NOT overwrite
_inventory.json (which is hand-curated with purpose strings). The graph tier
emits a separate file keyed on the same domain vocabulary so the Map can
cross-reference both.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_STACK = _HERE.parent
_DOCS = _STACK.parent
_PLUGIN = _DOCS.parent

# Make the adapters package importable whether we're run as a module or script.
if str(_STACK) not in sys.path:
    sys.path.insert(0, str(_STACK))

from adapters.graph_adapter import GraphAdapter  # noqa: E402


def _read_plugin_version(plugin_root: Path) -> str:
    uplugin = plugin_root / "ForgeEditorBridge.uplugin"
    if uplugin.exists():
        try:
            data = json.loads(uplugin.read_text(encoding="utf-8"))
            return str(data.get("VersionName") or data.get("Version") or "unknown")
        except Exception:
            pass
    return "unknown"


def _merge_inventory_aliases(domains: list[dict], inventory_path: Path) -> dict:
    """Graph extraction doesn't see aliases (they're hand-curated). Pull them
    from _inventory.json so the index keys match what recipes reference.

    Also returns a diff summary for logging.
    """
    diff = {"inv_only": [], "graph_only": [], "name_aliases": {}}
    if not inventory_path.exists():
        return diff
    try:
        inv = json.loads(inventory_path.read_text(encoding="utf-8"))
    except Exception:
        return diff
    inv_map: dict = {}
    for d in inv.get("domains", []):
        dname = d.get("domain")
        inv_map[dname] = {a.get("name"): a.get("aliases", []) for a in d.get("actions", [])}

    import re as _re
    def _norm(n: str) -> str:
        return _re.sub(r"_", "", n.lower())

    # Name-alias detection: graph "state_tree" vs inventory "statetree".
    graph_names = {d.get("domain") for d in domains}
    inv_names = set(inv_map.keys())
    by_norm_graph = {_norm(n): n for n in graph_names}
    by_norm_inv = {_norm(n): n for n in inv_names}
    for nk in set(by_norm_graph) & set(by_norm_inv):
        if by_norm_graph[nk] != by_norm_inv[nk]:
            diff["name_aliases"][by_norm_graph[nk]] = by_norm_inv[nk]

    diff["graph_only"] = sorted(
        {by_norm_graph[n] for n in set(by_norm_graph) - set(by_norm_inv)}
    )
    diff["inv_only"] = sorted(
        {by_norm_inv[n] for n in set(by_norm_inv) - set(by_norm_graph)}
    )

    # Apply aliases. Use the name_aliases map to cross-reference.
    for dom in domains:
        key = dom.get("domain")
        # Look up under graph name first, fall back to its inventory alias.
        alias_for = inv_map.get(key) or \
            inv_map.get(diff["name_aliases"].get(key, "")) or {}
        for act in dom.get("actions", []):
            aliases = alias_for.get(act.get("name"))
            if aliases:
                act["aliases"] = aliases
    return diff


def _carry_forward_inventory_domains(domains: list[dict], diff: dict, inventory_path: Path) -> None:
    """Add inventory-only domains (e.g. 'quarantine') as empty-action stubs so
    the index stays cross-referenceable with the Map's DOMAIN_CATALOG."""
    if not diff.get("inv_only") or not inventory_path.exists():
        return
    try:
        inv = json.loads(inventory_path.read_text(encoding="utf-8"))
    except Exception:
        return
    inv_by_name = {d.get("domain"): d for d in inv.get("domains", [])}
    existing = {d.get("domain") for d in domains}
    for name in diff["inv_only"]:
        if name in existing:
            continue
        inv_entry = inv_by_name.get(name, {})
        domains.append({
            "domain": name,
            "handler_file": inv_entry.get("handler_file", ""),
            "handler_file_abs": "",
            "actions": [
                {
                    "name": a.get("name"),
                    "dispatch_line": a.get("dispatch_line"),
                    "handler_fn": None,
                    "aliases": a.get("aliases", []),
                } for a in inv_entry.get("actions", [])
            ],
            "source": "inventory-carry-forward",
        })
    domains.sort(key=lambda d: d["domain"])


def generate(
    plugin_src_root: Path,
    plugin_root: Path,
    inventory_path: Path,
    out_path: Path,
    cache_path: Path,
) -> dict:
    adapter = GraphAdapter(str(plugin_src_root), str(cache_path))
    stats = adapter.rebuild()

    domains = adapter._data.get("domains", [])
    diff = _merge_inventory_aliases(domains, inventory_path)
    _carry_forward_inventory_domains(domains, diff, inventory_path)

    payload = {
        "generated_at": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "plugin_version": _read_plugin_version(plugin_root),
        "graph_backend": stats.get("backend", "unknown"),
        "stats": {
            "nodes": stats.get("nodes", 0),
            "edges": stats.get("edges", 0),
            "files": stats.get("files_indexed", 0),
            "domains": len(domains),
            "actions": sum(len(d.get("actions", [])) for d in domains),
        },
        "inventory_cross_check": {
            "name_aliases": diff.get("name_aliases", {}),
            "inventory_only_domains": diff.get("inv_only", []),
            "graph_only_domains": diff.get("graph_only", []),
        },
        "domains": domains,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="_bridge_index_generator")
    parser.add_argument("--plugin-src", default=None, help="Plugin source root (override).")
    parser.add_argument("--plugin-root", default=None, help="Plugin root (override).")
    parser.add_argument("--inventory", default=None, help="Inventory JSON (override).")
    parser.add_argument("--out", default=None, help="Output index JSON path (override).")
    parser.add_argument("--cache", default=None, help="Adapter cache path (override).")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    plugin_root = Path(args.plugin_root) if args.plugin_root else _PLUGIN
    plugin_src = Path(args.plugin_src) if args.plugin_src else \
        plugin_root / "Source" / "ForgeEditorBridge"
    inventory = Path(args.inventory) if args.inventory else _DOCS / "_inventory.json"
    out_path = Path(args.out) if args.out else _DOCS / "_bridge_index.json"
    cache_path = Path(args.cache) if args.cache else _STACK / ".cache" / "graph_cache.json"

    if not args.quiet:
        print(f"[graph] plugin_src : {plugin_src}")
        print(f"[graph] inventory  : {inventory}")
        print(f"[graph] out        : {out_path}")
        print(f"[graph] cache      : {cache_path}")

    payload = generate(plugin_src, plugin_root, inventory, out_path, cache_path)

    s = payload["stats"]
    backend = payload["graph_backend"]
    if not args.quiet:
        print(f"[graph] backend    : {backend}")
        print(f"[graph] nodes      : {s['nodes']}")
        print(f"[graph] edges      : {s['edges']}")
        print(f"[graph] files      : {s['files']}")
        print(f"[graph] domains    : {s['domains']}")
        print(f"[graph] actions    : {s['actions']}")
        print(f"[graph] wrote      : {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
