---
map_schema_version: 1.0
plugin_name: ForgeEditorBridge
plugin_version: 0.2.6
ue_version: 5.7
generated_at: 2026-05-12T00:00:00Z
shipped_tiers: [0, 1, 2]
graph_index: _bridge_index.json
graph_backend: tree-sitter
---

<!-- BEGIN:HAND_AUTHORED -->

# ForgeEditorBridge - Cognitive Map

## What this plugin is

ForgeEditorBridge is an editor-only HTTP bridge that exposes the Unreal Editor to external AI agents over `localhost:8765`. Every request carries an `X-Bridge-Token` header authenticated against `bridge-status.json` written by the plugin at startup. The bridge ships handlers covering every major UE editor domain: assets, levels, materials, animation, gameplay, PCG, rendering, source control, and more. All handlers are synchronous editor-thread calls, so PIE state and module availability are first-class concerns.

See DOMAIN_CATALOG below for the full surface.

## Routing table

| query type | tier | interface | example |
|---|---|---|---|
| "what can the plugin do" | Tier 0 (shipped) | this file, DOMAIN_CATALOG below | scan the table, pick the domain |
| "how do I <compound task>" | Tier 1 (shipped) | recipes | `ls Docs/recipes/*.yml` |
| "what calls function X" | Tier 2 (shipped) | code graph | `bridge-stack graph query-caller X` |
| "list actions in domain D" | Tier 2 (shipped) | code graph | `bridge-stack graph list-actions D` |
| "where is domain.action dispatched" | Tier 2 (shipped) | code graph | `bridge-stack graph query-action D A` |
| "how do I semantically do Y" | Tier 3 (rebuild required) | vector index | `bridge-stack rebuild --tier vector` then `bridge-stack search "Y"` |
| "what UE API does Z" | Tier 4 (rebuild required) | UE knowledge graph | `bridge-stack rebuild --tier ue_kg --ue-version 5.7` then `bridge-stack ue-kg query "Z"` |

Tier 3 (Vector) and Tier 4 (UE KG) are regenerable from source and not shipped pre-built; see `Docs/stack/README.md` for setup.

## Canonical query examples

- "List all actions in the `pcg_graph` domain." -> Tier 0, jump to DOMAIN_CATALOG row, then open `PCGGraphHandler.cpp`.
- "Give me a recipe for wiring a PBR material with channel-packed textures." -> Tier 1, open `Docs/recipes/wire_pbr_material_channel_packed.yml`.
- "Which handlers call `UAssetRegistryHelpers::GetAssetRegistry`?" -> Tier 2, `bridge-stack graph query-caller UAssetRegistryHelpers::GetAssetRegistry`.

## Reading order for a fresh AI session

1. This Map (schema-stable overview, routing, philosophy).
2. `_inventory.json` if you need action-level detail with aliases and dispatch lines.
3. The relevant recipe under `Docs/recipes/*.yml` if the task is compound.
4. The individual `*Handler.cpp` file only on deep dive (source of truth for parameter shapes and error cases).

## Error code quick reference

| code | meaning |
|---|---|
| 1000 | missing required parameter |
| 1001 | unknown action |
| 2000 | asset not found |
| 2003 | asset not loaded |
| 3000 | engine API failure |
| 3003 | module or world unavailable |
| 3004 | PIE required |

## Philosophy and conventions

- The Map is schema-stable. Indexes below it are disposable and rebuildable via `bridge-stack rebuild`.
- Recipes compose atomic actions. Recipes never bypass handlers; they orchestrate them.
- Each tier (graph, vector, UE-KG) has a swappable adapter, so a failing or slow tier can be replaced without touching recipes or the Map.
- Domains map one-to-one to handler files. If a new handler appears, it gets a row in the catalog after `bridge-stack map regen`.
- Hand-authored prose above this line is never regenerated. Everything below the `HAND_AUTHORED` marker is owned by the build pipeline.
- DOMAIN_CATALOG `purpose` column is curated. It lives in `_inventory.json` and should be edited there, not copied from source-file header comments.

<!-- END:HAND_AUTHORED -->

<!-- BEGIN:DOMAIN_CATALOG -->
| domain | handler file | actions | purpose |
|---|---|---:|---|
| actor | ActorHandler.cpp | 23 | Spawn, query, transform, and destroy level actors; resolves UClasses from names and asset paths |
| animation | AnimationHandler.cpp | 13 | Animation sequence, montage, and movie-scene animation-track operations |
| animation_asset | AnimationAssetHandler.cpp | 6 | Animation asset creation and inspection (sequences, montages, blend spaces) |
| animbp | AnimBlueprintHandler.cpp | 10 | Animation Blueprint graph and state-machine edits via AnimGraph editor module |
| asset | AssetHandler.cpp | 13 | Generic asset registry queries, save/load, rename, duplicate, and reference scans |
| asset_graph | AssetGraphHandler.cpp | 4 | Asset Registry graph queries: referencers, dependencies (recursive), validation, package size |
| audio | AudioHandler.cpp | 18 | SoundCue, SoundWave, and MetaSound document operations |
| behavior_tree | BehaviorTreeHandler.cpp | 14 | Behavior Tree asset authoring, task/service/decorator wiring, blackboard hookup |
| blueprint | BlueprintHandler.cpp | 59 | Blueprint creation, node graphs, variables, functions, components, compilation - the heavy hitter |
| camera | CameraHandler.cpp | 9 | Editor viewport camera placement, cinematic cameras, bookmarks |
| chaos | ChaosHandler.cpp | 7 | Chaos destruction, geometry collections, and fracture operations |
| cloth_lod | ClothLODHandler.cpp | 3 | Cloth asset LOD configuration on skeletal meshes |
| collision | CollisionHandler.cpp | 7 | Collision profiles, responses, trace channels, and primitive collision setup |
| console | ConsoleHandler.cpp | 6 | Execute and read UE console commands and cvars |
| context | ContextHandler.cpp | 11 | Plugin self-description: capabilities, schema, health, version introspection |
| control_rig | ControlRigHandler.cpp | 10 | Control Rig Blueprint authoring via ControlRigDeveloper module |
| cpp | CppHandler.cpp | 8 | C++ source/header read and write, template generation, Live Coding trigger, build log tail |
| data_table | DataTableHandler.cpp | 14 | Data Table row CRUD, CSV/JSON import-export, struct binding |
| debug | DebugHandler.cpp | 9 | Draw-debug primitives, on-screen messages, and debug visualization toggles |
| editor_prefs | EditorPrefsHandler.cpp | 6 | Editor preferences and per-user settings read/write |
| editor_utility | EditorUtilityHandler.cpp | 4 | Editor Utility Widget/Blueprint execution and discovery |
| environment | EnvironmentHandler.cpp | 6 | SkyAtmosphere, exponential height fog, volumetric clouds actor setup |
| eqs | EQSHandler.cpp | 6 | Environment Query System asset authoring and test configuration |
| foliage | FoliageHandler.cpp | 5 | Foliage type placement, instanced foliage actor operations |
| gameplay_tag | GameplayTagHandler.cpp | 6 | GameplayTag registration, INI management, tag query |
| gas | GASHandler.cpp | 27 | Gameplay Ability System: abilities, attributes, effects, cues, tags |
| git | GitHandler.cpp | 7 | Git status, diff, commit, and branch operations scoped to the project |
| ik_retargeter | IKRetargeterHandler.cpp | 4 | IK Retargeter and IK Rig Definition asset operations |
| input | InputHandler.cpp | 7 | Enhanced Input actions, mapping contexts, and legacy input bindings |
| landscape | LandscapeHandler.cpp | 17 | Landscape creation, sculpt/paint, layer weightmaps, component edits |
| level | LevelHandler.cpp | 15 | Level (world) load, save, new, streaming sublevels, and level utilities |
| lighting | LightingHandler.cpp | 10 | Light actor placement, Lumen settings, lightmass, and baked lighting controls |
| localization | LocalizationHandler.cpp | 7 | Localization targets, cultures, string tables, and gather/compile ops |
| mass_entity | MassEntityHandler.cpp | 9 | Mass Entity traits, fragments, processors, and entity template setup |
| material | MaterialHandler.cpp | 19 | Material graph authoring: nodes, expressions, connections, parameters |
| material_instance | MaterialInstanceHandler.cpp | 7 | Material Instance parameter overrides and parent swaps |
| media | MediaHandler.cpp | 4 | MediaSource, MediaPlayer, and MediaTexture asset operations |
| navmesh | NavMeshHandler.cpp | 7 | Navigation mesh volume, runtime generation, and nav-area setup |
| niagara | NiagaraHandler.cpp | 22 | Niagara system, emitter, module, and variable operations |
| packaging | PackagingHandler.cpp | 8 | Cook, package, and build-target orchestration |
| pcg | PCGHandler.cpp | 2 | Runtime PCG component control: trigger GenerateLocal and set component properties via reflection |
| pcg_graph | PCGGraphHandler.cpp | 27 | UPCGGraph asset authoring: create graph, add/remove nodes, connect pins, set settings, telemetry |
| physics_asset | PhysicsAssetHandler.cpp | 8 | Physics Asset body and constraint setup for skeletal meshes |
| post_process | PostProcessHandler.cpp | 6 | Post Process Volume and material-post-process settings |
| project | ProjectHandler.cpp | 14 | Project settings, module state, plugin enable/disable, uproject operations |
| python | PythonHandler.cpp | 5 | Python script execution (soft dependency on PythonScriptPlugin) |
| quarantine | QuarantineHandler.cpp | 0 | Renames suspect assets into /Game/Forge_Quarantine with timestamped name and reason log |
| reflection | ReflectionHandler.cpp | 6 | UClass/UStruct/UEnum reflection queries for schema introspection |
| rendering | RenderingHandler.cpp | 14 | Rendering feature toggles: Nanite, Lumen, virtual textures, scalability |
| replication | ReplicationHandler.cpp | 9 | Replication graph, condition, and property-replication configuration |
| runtime_capture | RuntimeCaptureHandler.cpp | 2 | Captures PIE world snapshot (actors, transforms, GAS tags) and runtime variable dumps to JSON |
| scene_query | SceneQueryHandler.cpp | 7 | World line/sphere/box traces and overlap queries |
| sequencer | SequencerHandler.cpp | 18 | Level Sequence, sections, tracks, keyframe editing for cinematics |
| skeletal_mesh | SkeletalMeshHandler.cpp | 6 | Skeletal Mesh asset operations: LODs, sockets, materials |
| skeleton | SkeletonHandler.cpp | 6 | Skeleton asset bone trees, virtual bones, and retarget chains |
| smart_object | SmartObjectHandler.cpp | 6 | Smart Object definitions, slots, and subsystem queries |
| source_control | SourceControlHandler.cpp | 5 | Source control provider status, checkout, revert, submit |
| spline | SplineHandler.cpp | 11 | Spline component point CRUD, tangents, length sampling |
| state_tree | StateTreeHandler.cpp | 5 | State Tree asset authoring: states, transitions, conditions |
| static_mesh | StaticMeshHandler.cpp | 5 | Static Mesh asset operations: LODs, collision, sockets, materials |
| string_table | StringTableHandler.cpp | 4 | String Table asset creation, key/value CRUD |
| struct | StructHandler.cpp | 6 | User-defined struct creation and property edits |
| subsystem_query | SubsystemQueryHandler.cpp | 2 | Engine/Editor/World subsystem existence and listing |
| testing | TestingHandler.cpp | 6 | Automation test discovery and run orchestration |
| texture | TextureHandler.cpp | 7 | Texture2D import, sRGB/compression settings, reimport |
| umg | UMGHandler.cpp | 18 | UMG (UserWidget) tree authoring and widget-to-JSON export |
| water | WaterHandler.cpp | 4 | Water body actor placement and water-plugin assets |
| world_partition | WorldPartitionHandler.cpp | 13 | World Partition cell load, HLOD, data layer, and grid operations |
| world_settings | WorldSettingsHandler.cpp | 8 | World Settings editing: game mode, physics, default pawn, kill Z |
<!-- END:DOMAIN_CATALOG -->

<!-- BEGIN:RECIPE_CATALOG -->
| id | title | goal |
|---|---|---|
| `build_behavior_tree_with_blackboard_and_decorators` | Build a behavior tree with blackboard, decorators, and a service | Author a behavior tree from scratch with a typed blackboard, root composite, decorators, task nodes, and an attached service. |
| `build_gas_ability_with_cooldown_and_tags` | Build a GAS ability with cooldown, cost, and tags | Construct a complete UGameplayAbility - blueprint asset, ability tags, cooldown GE, cost GE - and grant it on a target ASC. |
| `import_texture_set_with_srgb_rules` | Batch-import a texture set with suffix-driven PBR rules | Import a folder of textures and apply correct sRGB / compression / LOD-group settings based on filename suffix conventions. |
| `mass_spawn_with_collision_avoidance` | Mass-spawn actors with collision avoidance | Spawn N actors from a proposed transform list, skipping or offsetting any that would overlap existing geometry. |
| `niagara_emitter_from_static_mesh` | Niagara emitter spawning from a static mesh | Create a Niagara system + emitter that spawns particles sampled from a static mesh, with mesh sampling, a renderer, and GPU sim. |
| `pcg_foliage_scatter_from_landscape` | PCG foliage scatter sampled from landscape | Build a PCG graph that samples a landscape, filters by slope and altitude, and spawns a static mesh as scattered foliage. |
| `scan_mesh_bounds_and_place` | Scan mesh bounds and place on landscape | Place a static mesh at candidate XY positions such that its bounds sit on the landscape surface and do not intersect existing meshes. |
| `spline_distributed_placement` | Distribute actors along a spline | Sample N points along a spline and place an actor at each, optionally aligned to the spline tangent or terrain normal. |
| `umg_widget_with_bound_data_and_animation` | UMG widget with data binding and fade-in animation | Create a UMG widget blueprint, populate it with anchored TextBlock + Image children, bind their values to a data context, add a fade-in animation, then compile. |
| `wire_pbr_material_channel_packed` | Wire a channel-packed PBR material | Import up to 13 PBR source textures and wire them into a compiled UE5 material with channel-packed ORM. |
<!-- END:RECIPE_CATALOG -->

<!-- BEGIN:INDEX_FRESHNESS -->
| tier | last built | source |
|---|---|---|
| 0 Map | shipped | hand-authored + `bridge-stack map regen` |
| 1 Recipes | shipped | `recipes/*.yml` |
| 2 Graph | shipped | `bridge-stack rebuild --tier graph` (tree-sitter) |
| 3 Vector | not shipped (rebuild required) | `bridge-stack rebuild --tier vector` (sentence-transformers + sqlite-vec) |
| 4 UE KG | not shipped (rebuild required) | `bridge-stack rebuild --tier ue_kg --ue-version 5.7` (hybrid: tree-sitter + regex + networkx + sqlite-vec) |
<!-- END:INDEX_FRESHNESS -->

---

See `Docs/stack/README.md` for tier-upgrade playbooks and adapter contracts.
