# Forge Editor Bridge

**Local, authenticated AI control for Unreal Editor.**

![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128)
![Plugin Type](https://img.shields.io/badge/plugin-editor--only-blue)
![Bridge](https://img.shields.io/badge/bridge-localhost%20HTTP-green)
![Stack](https://img.shields.io/badge/cognitive%20stack-optional-purple)
![License](https://img.shields.io/badge/license-source--available-lightgrey)

ForgeEditorBridge is an editor-only Unreal Engine plugin that gives local AI tools a structured way to inspect and operate the Unreal Editor over HTTP. It exposes editor actions through domain handlers, writes context captures to disk, and ships with an optional documentation stack so humans and LLMs can quickly discover what the bridge can do.

The important bit: **you do not need to build graph data, vector indexes, or a UE knowledge graph to use the plugin.** Install the plugin, start Unreal Editor, read the session token, and call the localhost bridge.

![Forge Editor Bridge banner](Docs/assets/forge-editor-bridge-banner.png)

## Highlights

- Authenticated localhost HTTP bridge for Unreal Editor automation.
- `POST /command` for single actions and `POST /batch` for grouped edits.
- Auto-discovered handler domains for actors, assets, Blueprints, materials, PCG (Procedural Content Generation), Niagara, UMG (Unreal Motion Graphics), GAS (Gameplay Ability System), Sequencer, World Partition, and more.
- Runtime self-description through `system.capabilities` and `system.describe`.
- Read-side context captures for project state, assets, Blueprints, materials, input, networking, packaging, and other editor surfaces.
- Safety-oriented defaults: localhost-only access, per-session token, quarantine intercept for destructive action names, and editor transaction support.
- Optional LLM/human cognitive stack with maps, recipes, graph lookup, vector search, and UE API indexing.

![Forge Editor Bridge card](Docs/assets/forge-editor-bridge-card.png)

## Repository Layout

```text
ForgeEditorBridge/
  ForgeEditorBridge.uplugin          Unreal plugin descriptor
  Source/ForgeEditorBridge/          Editor module, server, handlers, captures
  Docs/_bridge_map.md                Human/LLM domain map
  Docs/_inventory.json               Machine-readable handler/action inventory
  Docs/_bridge_index.json            Shipped graph index
  Docs/recipes/                      Compound task playbooks
  Docs/stack/                        Optional cognitive stack tooling
  Docs/OPERATING_GUIDE.md            Human operator guide
  Docs/LLM_OPERATOR_GUIDE.md         LLM operator guide
  Docs/_stack_status.md              Stack status dashboard
  CONTRIBUTING.md                    Contribution guidelines
  SECURITY.md                        Security policy
```

## Project Compatibility

| Requirement | Detail |
|---|---|
| Unreal Engine | 5.7 (matches plugin headers; later 5.x is untested) |
| Platform | Windows host. Listens on `127.0.0.1` only. |
| Project type | Blueprint-only **and** C++ projects are both supported. See note below. |
| Editor mode | Editor-only plugin (`Type: Editor`, `LoadingPhase: PostEngineInit`). Not loaded in cooked/packaged builds. |
| Build chain | This release ships as **source only** - no precompiled binaries. UBT compiles the plugin into your project on first editor launch. |

**Blueprint-only projects**: yes, you can use this plugin. The catch is the one-time build step. Because the release ships as source, UBT needs to compile the plugin module the first time you open the project with it installed. That requires Visual Studio (or Rider/VS Code with the C++ workload) and effectively means your project gains a `Source/` folder, a `.sln`, and `Binaries/` outputs - i.e. it becomes a "C++ project" in UBT's bookkeeping. After that compile lands, **your game content can stay 100% Blueprint**. The plugin does not add `UCLASS` actors, BP nodes, or any gameplay-side surface you need to consume in Blueprints; it runs in the editor and exposes a localhost HTTP API for tooling. A truly zero-compile workflow (drop the plugin in, launch the editor) would require shipping precompiled binaries matching your exact engine version - that is a Marketplace-style distribution and is not part of this release.

If you want to install without touching a compiler, you can fork this repo, run the compile once on your machine, and commit the `Binaries/` folder to a private fork. But for normal use, plan on installing Visual Studio with the **Game development with C++** workload before the first editor launch.

## Quick Start

1. Copy the folder that contains `ForgeEditorBridge.uplugin` (this repo's root) into your Unreal project, renaming it to `ForgeEditorBridge` if it isn't already:

```text
<YourProject>/Plugins/ForgeEditorBridge/
  ForgeEditorBridge.uplugin
  Source/
  Docs/
```

2. Open the project in Unreal Editor.

3. Enable **Forge Editor Bridge** in the Plugins panel.

4. Restart the editor.

5. Check settings:

```text
Project Settings > Plugins > Forge Editor Bridge
```

Defaults:

- Port: `8765`
- Context directory: `Forge/ue-context`
- Auto-start: enabled

6. Start the editor and read:

```text
{ProjectDir}/Forge/ue-context/bridge-status.json
```

That file contains the live port, auth token, start time, and available domains.

## First Call

PowerShell example (run this from your Unreal project root - the directory that holds the `.uproject` file):

```powershell
$status = Get-Content "$PWD\Forge\ue-context\bridge-status.json" | ConvertFrom-Json
$headers = @{ "X-Bridge-Token" = $status.auth_token }
$body = @{
  domain = "system"
  action = "ping"
  params = @{}
} | ConvertTo-Json

Invoke-RestMethod `
  -Method Post `
  -Uri "http://127.0.0.1:$($status.port)/command" `
  -Headers $headers `
  -ContentType "application/json" `
  -Body $body
```

Expected result:

```json
{
  "ok": true,
  "message": "pong",
  "domain": "system",
  "action": "ping",
  "error_code": 0
}
```

## HTTP Contract

The bridge exposes two endpoints:

| Endpoint | Body | Use |
|---|---|---|
| `POST /command` | one command object | Single action |
| `POST /batch` | JSON array of command objects | Grouped operations |

Every request must:

- originate from localhost
- include `X-Bridge-Token`
- send JSON

Command shape:

```json
{
  "domain": "system",
  "action": "capabilities",
  "params": {}
}
```

Batch shape:

```json
[
  { "domain": "system", "action": "get_editor_state", "params": {} },
  { "domain": "system", "action": "health_check", "params": {} }
]
```

## Start Here As A User

Run these first in a live editor session:

```json
{ "domain": "system", "action": "ping", "params": {} }
```

```json
{ "domain": "system", "action": "health_check", "params": {} }
```

```json
{ "domain": "system", "action": "capabilities", "params": {} }
```

```json
{ "domain": "system", "action": "describe", "params": { "domain": "blueprint" } }
```

Useful system actions:

| Action | Purpose |
|---|---|
| `ping` | Verify the server is alive |
| `capabilities` | List registered domains and actions |
| `describe` | Return action schemas |
| `describe_all` | Return all available schemas |
| `health_check` | Check server, output directory, captures, and runtime state |
| `get_editor_state` | Read level, PIE (Play In Editor), selection, dirty package state |
| `export_all_captures` | Write context captures to disk |
| `save_all` | Save dirty maps and assets |
| `undo` / `redo` | Use editor transaction history |

## Recipes: Chained Workflows

A recipe is a YAML playbook under `Docs/recipes/` that turns a compound editor task - the kind that would otherwise take five-to-twenty `POST /command` calls in a specific order - into a single curated sequence. Each step names a canonical `domain/action` plus its inputs and the captured outputs that feed the next step. **The plugin itself does not parse recipes**; an LLM operator (or any scripted runner) reads the YAML, dispatches each step against `/command` or `/batch`, and threads captured values forward. Every step is a real plugin call - nothing is invented prose.

Ten recipes ship with `0.2.6`:

| Recipe | What it does |
|---|---|
| `build_behavior_tree_with_blackboard_and_decorators` | Stands up a typed blackboard plus a behavior tree with composites, decorators, and a service. |
| `build_gas_ability_with_cooldown_and_tags` | Authors a GAS ability Blueprint with cooldown GE, cost GE, gameplay tags, and grants it on a target ASC. |
| `import_texture_set_with_srgb_rules` | Batch-imports a texture folder and applies sRGB / compression / LOD-group settings by filename suffix. |
| `mass_spawn_with_collision_avoidance` | Spawns N actors from a transform list, skipping or offsetting overlaps. |
| `niagara_emitter_from_static_mesh` | Creates a Niagara system + emitter that samples particles from a static mesh. (See note below on partial wiring.) |
| `pcg_foliage_scatter_from_landscape` | Builds a PCG graph that samples a landscape, filters by slope and altitude, and scatters a mesh. |
| `scan_mesh_bounds_and_place` | Places a static mesh at candidate XYs, snapping bounds to the landscape and rejecting overlaps. |
| `spline_distributed_placement` | Samples N points along a spline and places an actor at each, optionally aligned to tangent or terrain normal. |
| `umg_widget_with_bound_data_and_animation` | Creates a UMG widget with anchored children, data binding, and a fade-in animation, then compiles. |
| `wire_pbr_material_channel_packed` | Imports up to 13 PBR source textures and wires them into a compiled channel-packed UE5 material. |

To dispatch one, read the YAML and replay its `steps` against `/command` (or stage them as a single `/batch`):

```powershell
Get-Content Docs\recipes\wire_pbr_material_channel_packed.yml
```

Schema reference: `Docs/recipes/_template.yml`. Authoring discipline: `Docs/stack/README.md`. LLM-side dispatch guidance: `Docs/LLM_OPERATOR_GUIDE.md`.

### Known Recipe Stubs

The Niagara recipe references four actions that return `error_code 3003` ("not implemented at handler level"). Three of these are **BLOCKED** by UE 5.7 architecture and require a Python fallback rather than a future C++ wiring; one is **DEFERRED** to a later plugin release.

| Action | Status | Reason |
|---|---|---|
| `niagara/add_module` | BLOCKED | NiagaraEditor stack-graph editing lives in `Internal/Private` headers Epic does not expose publicly. |
| `niagara/set_module_property` | BLOCKED | Same surface as above. |
| `niagara/add_renderer` | BLOCKED | `FVersionedNiagaraEmitterData::RendererProperties` is a private field with no public mutator in UE 5.7. |
| `niagara/set_renderer_property` | DEFERRED | Reachable via reflection (same pattern `set_particle_lights` already uses). Roughly hours of work. |

For BLOCKED steps, dispatch a `python_agent` snippet that drives the equivalent via the `unreal` Python API. The recipe's `failure_modes:` block has the recommended pattern.

## Optional Cognitive Stack

The cognitive stack is for discovery and maintenance. It is not a runtime dependency.

Shipped:

- Tier 0: map, `Docs/_bridge_map.md`
- Tier 1: recipes, `Docs/recipes/*.yml`
- Tier 2: graph index, `Docs/_bridge_index.json`

Regenerable:

- Tier 3: vector search, `Docs/.vectors/chunks.sqlite`
- Tier 4: UE knowledge graph, `Docs/ue_kg/<version>/`

Basic stack commands. The `status`, `verify`, `graph list-domains`, `graph list-actions`, and `graph query-action` commands only need a Python environment with the graph-tier dependencies installed - see `Docs/stack/README.md` for the one-time venv setup. After that:

```powershell
cd Docs\stack
.\.venv\Scripts\python bridge-stack status
.\.venv\Scripts\python bridge-stack verify
.\.venv\Scripts\python bridge-stack graph list-domains
.\.venv\Scripts\python bridge-stack graph list-actions blueprint
.\.venv\Scripts\python bridge-stack graph query-action blueprint create_blueprint
```

Tip: you can also `grep` `Docs/_bridge_index.json` directly without any venv - it ships pre-built and is plain JSON.

Build the UE knowledge graph only for local engine API analysis (Tier 4):

```powershell
cd Docs\stack
.\.venv\Scripts\python bridge-stack rebuild --tier ue_kg --ue-version 5.7
.\.venv\Scripts\python bridge-stack ue-kg sig UGameplayAbility::GetAssetTags
```

> **Rebuilding indexes inside a clone leaks absolute paths.** The graph index (`Docs/_bridge_index.json`), the vector store (`Docs/.vectors/`), and the UE KG (`Docs/ue_kg/<version>/`) all record per-file absolute paths for the machine that built them. If you regenerate any tier from your local clone, **do not re-commit the rebuilt files**. The shipped versions already cover the plugin's own surface; rebuild only when you are extending or testing locally. The UE KG manifest (`Docs/stack/ue_kg_manifest.json`) ships path-templatized using `${UE_ENGINE_ROOT}`; the engine root is resolved from the `UE_ENGINE_ROOT` environment variable at load time, with a Windows default fallback.

## Semantic Search (Optional Tier 3)

![Forge semantic search tier](Docs/assets/forge-semantic-search-tier.png)

The shipped stack lets an LLM operator look things up **by name** (handler dispatch, action inventory, recipe id). The optional vector tier adds **lookup by meaning** - useful when the operator phrases a task by intent rather than by API name.

If your operator already knows it needs `pcg_graph/add_pcg_node`, the shipped graph index is sufficient. If your operator asks "how do I scatter rocks on a slope?" or "what do I do when `configure_asc` returns 4002?", the vector tier closes that gap by returning the recipe or failure-mode that matches the intent. It is purely additive - skipping it does not block any plugin functionality.

What the vector tier indexes:

- **Action chunks** - the domain's natural-language purpose, the canonical name + aliases, and the handler file/line.
- **Recipe chunks** - the recipe id, goal, description, inputs, declared `action_dependencies`, the ordered `steps[].action` chain, every `variants[]` differentiator, and every `failure_modes[]` trigger / symptom.
- **Domain-overview chunks** - the domain purpose and the first 10 action names.

Embeddings use `nomic-ai/nomic-embed-text-v2-moe` (768-dim, MoE). First run downloads ~580 MB of model weights to your local HuggingFace cache; subsequent runs reuse the cache.

Enable it once:

```powershell
cd Docs\stack
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install tree-sitter tree-sitter-cpp networkx pyyaml pyparsing sentence-transformers sqlite-vec einops
.\.venv\Scripts\python bridge-stack rebuild --tier vector --force
.\.venv\Scripts\python bridge-stack search "wire channel packed textures into material"
```

After that, query by meaning with `bridge-stack search "<question>"`. The index lives at `Docs/.vectors/chunks.sqlite` and is rebuilt incrementally when recipes or inventory change. For LLM operator guidance on **when** to issue a vector search vs a direct name lookup, see `Docs/LLM_OPERATOR_GUIDE.md` (Using Semantic Search).

## Architecture

```text
Local AI tool / script
        |
        | POST /command or /batch
        | X-Bridge-Token
        v
FBridgeHttpServer
        |
        v
UForgeAISubsystem
        |
        +-- UBridgeHandlerBase domains   write/query editor actions
        +-- capture objects              export editor context
        +-- UBridgeResultWriter          persist command results
        +-- UQuarantineHandler           intercept destructive action names
```

Core classes:

| Class | Role |
|---|---|
| `UForgeAISettings` | Project Settings section for port, token, output path, auto-start |
| `UForgeAISubsystem` | Editor subsystem that owns handlers, captures, and server lifecycle |
| `FBridgeHttpServer` | Local HTTP routes, token validation, dispatch, batch execution |
| `UBridgeHandlerBase` | Base class for domain handlers |
| `FBridgeResult` | Standard command result object |

## Safety Notes

ForgeEditorBridge is powerful editor automation. Treat it like local developer tooling, not a public web service.

- Do not expose the port outside localhost.
- Keep `bridge-status.json` session-local.
- Confirm schemas before write operations.
- Use `/batch` for grouped edits.
- Inspect `ok`, `error_code`, `message`, and `recovery_hint` after every command.
- Avoid source-control, packaging, Python execution, C++ writing, and console commands unless they are clearly part of the requested workflow.

Common error codes:

| Code | Meaning |
|---:|---|
| `1000` | Missing required parameter |
| `1001` | Unknown action |
| `2000` | Asset not found |
| `2003` | Asset not loaded |
| `3000` | Engine API failure |
| `3003` | Module or world unavailable |
| `3004` | PIE (Play In Editor) required |
| `5000` | Domain not registered |

## License

See `LICENSE`.

The current license is proprietary/source-available by default. Replace it intentionally before publishing if this project should be released under an open-source license.
