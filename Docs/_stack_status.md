# ForgeEditorBridge Cognitive Stack - Status

## Plugin
- Version: 0.2.6
- UE version: 5.7

## Tiers in this release

| Tier | Name | Shipped? | How to use |
|---|---|---|---|
| 0 | Map | yes | Read `Docs/_bridge_map.md` directly |
| 1 | Recipes | yes | 10 compound-task playbooks under `Docs/recipes/*.yml` |
| 2 | Graph (tree-sitter-cpp) | yes | Query via `bridge-stack graph ...`; refresh via `bridge-stack rebuild --tier graph` |
| 3 | Vector (sentence-transformers + sqlite-vec) | no - rebuild required | `bridge-stack rebuild --tier vector --force` (downloads ~580 MB HF model on first run) |
| 4 | UE KG (hybrid: tree-sitter + regex + networkx + sqlite-vec) | no - rebuild required | `bridge-stack rebuild --tier ue_kg --ue-version 5.7` (~2 hours CPU on first build; needs local UE 5.7 install) |

## Rebuild contract

Every tier below Tier 0 is regenerable from source. Lose `.vectors/` or `ue_kg/` -> run the matching `bridge-stack rebuild --tier ...` and the index restores. See `stack/README.md` for the orchestrator command reference.

## Design notes

### Why hybrid for the UE knowledge graph (Tier 4), not pure LightRAG
LightRAG uses an LLM for entity-relation extraction. Running an LLM over ~9,000 in-scope UE 5.7 engine headers would be expensive, slow, and non-deterministic. UE engine code is highly structured C++ and the entities the graph needs (classes, structs, members, deprecation markers, modules) parse out cleanly with regex + balanced-brace scanning. Tree-sitter-cpp is loaded but optional - it trips into ERROR nodes around `UCLASS(...)` macros on multi-base inheritance, so the regex-based class boundary detection is authoritative. Result: deterministic, cheap, incrementally rebuildable, and version-keyed under `ue_kg/<ue_version>/`.

### Why nomic-embed-text-v2-moe for vector (Tier 3)
The plugin's chunk corpus is small (~750-1000 short structured chunks): one per action, one per recipe, one per domain. A 7B code embedding model is overkill; an MoE model with 305M active params at 768-dim hits the right cost/quality point and loads cleanly under sentence-transformers 5.x with `trust_remote_code=True` plus `einops`. Swap models by changing `_DEFAULT_MODEL_ID` in `stack/adapters/vector_adapter.py` then `bridge-stack rebuild --tier vector --force`.

## Known limitations

- The shipped Tier 2 graph index reflects the handler set at the time of the snapshot. Run `bridge-stack rebuild --tier graph` to re-index against the current `Source/` tree.
- Recipes may reference actions not yet present in the plugin (marked `pending_plugin_support`); `bridge-stack verify` surfaces mismatches.
- The UE KG class extractor is regex-driven (tree-sitter alone is not reliable across UCLASS-decorated types with multi-base inheritance). Multi-line `UE_DEPRECATED(...)` calls are joined; forward declarations are filtered.
