# Maintainers Guide

Notes for maintainers of ForgeEditorBridge. Operator-facing material lives in `OPERATING_GUIDE.md` and `LLM_OPERATOR_GUIDE.md`; this file is internal release-engineering.

## Version Bump Checklist

The canonical plugin version lives in `ForgeEditorBridge.uplugin` under `VersionName`. On a version bump, update every file that carries a version string so the plugin, runtime metadata, docs, and capture outputs stay aligned. Current canonical value: `0.2.6`.

Files to update (in this order):

1. `ForgeEditorBridge.uplugin` - `VersionName` field
2. `Docs/_inventory.json` - `plugin_version` field
3. `Docs/_bridge_index.json` - `plugin_version` field
4. `Docs/_bridge_map.md` - YAML frontmatter `plugin_version`
5. `Docs/_stack_status.md` - `## Plugin` section
6. All 10 `Docs/recipes/*.yml` - `tested_on_plugin` field
7. `Source/ForgeEditorBridge/Private/ForgeEditorBridge.cpp` - module startup log
8. `Source/ForgeEditorBridge/Private/ForgeAISubsystem.cpp` - bridge-started log
9. `Source/ForgeEditorBridge/Private/Server/BridgeHttpServer.cpp` - status JSON `version` field
10. All 9 `Source/ForgeEditorBridge/Private/Capture/*.cpp` - `plugin_version` JSON output fields
11. Handler header docblocks that carry inline `v<x.y.z> / UE 5.7` tags:
    - `Source/ForgeEditorBridge/Public/Handlers/EQSHandler.h`
    - `Source/ForgeEditorBridge/Public/Handlers/RenderingHandler.h`
    - `Source/ForgeEditorBridge/Public/Handlers/StateTreeHandler.h`
    - `Source/ForgeEditorBridge/Public/Handlers/StructHandler.h`

After bumping, run `python Docs/stack/bridge-stack verify` to confirm the docs tier is consistent. A quick residue grep also catches stale strings: `Grep "<previous-version>" Source Docs` should return no hits.
