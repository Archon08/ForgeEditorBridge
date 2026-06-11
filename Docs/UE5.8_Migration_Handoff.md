# UE 5.7 → 5.8 Migration Handoff — ForgeEditorBridge

**Audience:** the agent that will move this plugin to a UE 5.8 branch. **That agent cannot access the UE
5.8 engine source.** This document + its companion data files front-load every 5.8 fact gathered by an
agent that *did* have engine access (read of `EpicGames/UnrealEngine` @ branch `5.8`). Use it to minimize
your compile-fix iterations — it does not replace the compile, it shortcuts it.

## Companion files in this folder
- [`UE5.8_Migration_Recon.md`](./UE5.8_Migration_Recon.md) — new 5.8 systems (Mesh Terrain, MetaHuman
  Crowd, Procedural Vegetation, PCG editable output) with exact class/module names.
- [`UE5.8_Deprecations.txt`](./UE5.8_Deprecations.txt) — **844** `UE_DEPRECATED(5.8, …)` markers, sorted,
  with file path + message. This is your authoritative "what warns in 5.8" list.
- [`UE5.8_Plugin_Engine_Includes.txt`](./UE5.8_Plugin_Engine_Includes.txt) — the **696** engine headers
  this plugin `#include`s. This is your compile-checklist for the *removal* risk (see §6).

---

## 0. TL;DR

- **Target:** branch **`5.8`**, only tag is **`5.8.0-preview-1`**. This is a **preview build** — APIs and
  names can still shift before release. Treat all 5.8 facts as preview-accurate, not final.
- **Current state:** the plugin compiles on **UE 5.7**. It is **editor-only** (`Build.cs` throws if not an
  editor target), module type `Editor`, `LoadingPhase: PostEngineInit`. `VersionName 0.2.6`.
- **The good news (verified, §4):** the deprecated engine symbols this plugin *actually uses* mostly
  **survive** in 5.8-preview1 (deprecated-not-removed). No headers it includes were removed *within the
  engine modules that were checked and cross-referenced*. So the deprecation surface is **low-friction**.
- **The real risk (§6):** *removals*. A `UE_DEPRECATED` marker means "still exists, warns." Symbols
  **removed** between 5.7 and 5.8 leave **no marker** and cause hard compile errors. ~457 of the plugin's
  696 includes live in engine modules that were **not** verified here. Those, plus removed *symbols* inside
  verified headers, are what your compile pass will surface. This plugin's own history (it survived the
  brutal 5.6→5.7 removals — see the `UE 5.7:` notes littered through the source) shows removals, not
  deprecations, are where the work is.

---

## 1. How to switch the plugin to 5.8 (mechanics)

- The `.uplugin` has **no `EngineVersion` / `EngineAssociation` pin** — it is version-agnostic, so no edit
  is needed there to "allow" 5.8. Good.
- Put the plugin under a **5.8 engine** (source build of branch `5.8`, or an installed 5.8 preview) — either
  drop it in `<Project>/Plugins/ForgeEditorBridge` of a 5.8 project, or `<Engine>/Plugins/`. Regenerate
  project files and build the editor target.
- It is **editor-only** — there is no runtime/shipping path to worry about, but every dependency it lists is
  an editor or editor-adjacent module. Confirm each module name in `Build.cs` still resolves in 5.8 (module
  *renames* are a known failure mode — see §5).

---

## 2. Subsystem → handler map (what this plugin touches)

The plugin is 133 `.cpp` / 136 `.h`, organized as one handler per engine subsystem under
`Source/ForgeEditorBridge/Private/Handlers/` and capture probes under `…/Private/Capture/`. The handlers that
intersect the **heaviest 5.8 deprecation areas** (from `UE5.8_Deprecations.txt`):

| Handler / Capture file | Subsystem | 5.8 `UE_DEPRECATED` count in that module | Risk |
|---|---|---|---|
| `PCGHandler.cpp`, `PCGGraphHandler.cpp`, `ForgePCGCapture.cpp` | PCG | **154** | Medium — large surface, but see §4 |
| `MetaSoundBuilderHandler.cpp` | MetaSound | **89** | Medium — Frontend/Builder churn |
| `ControlRigHandler.cpp` | Control Rig | **23** | **Confirmed touch — §5.1** |
| `LandscapeHandler.cpp`, `ForgeHeightmapCapture.cpp` | Landscape | 9 | Low |
| `MoverHandler.cpp` | Mover | 8 | Low |
| `GASHandler.cpp`, `ForgeGASCapture.cpp` | GameplayAbilities | 5 (+ pre-5.8 stacking) | **Confirmed touch — §5.2** |
| `LocalizationHandler.cpp`, `ForgeLocalizationCapture.cpp` | StringTable (Core) | 2 | Low |
| `PackagingHandler.cpp`, `ForgePackagingCapture.cpp` | Packaging | *(not captured — §7)* | **Unknown — gap** |
| `ForgeBuildCapture.cpp` | HotReload/build | n/a (removal-class) | **Confirmed touch — §5.3** |

Engine-wide cross-cutting hotspots that any handler may hit (high marker counts in 5.8):
`SoundWave.h` (37), `SceneView.h` (11), `App.h` (10), `UnrealClient.h`/`SceneViewport.h` (viewport),
`World.h` (`ChangeFeatureLevel` → `ShaderPlatformChanged`), `UObjectGlobals.h`
(`RequiresCookedData` → `FPlatformProperties::RequiresCookedData()`; `CancelAsyncLoading` deprecated).

---

## 3. The deprecation-vs-removal distinction (read before you start)

`UE5.8_Deprecations.txt` answers **"what will emit a warning."** It does **not** answer **"what was
deleted."** Your build will fail on deletions, not on warnings (UE plugins typically don't treat
deprecation warnings as errors unless `bTreatWarningsAsErrors`/`-Werror` is set — check the plugin's
`Build.cs`; it does not appear to force that). Recommended order of operations:

1. Build once on 5.8. Triage the **hard errors first** (removed symbols/headers, signature changes,
   module renames). §5–§7 pre-empt the ones already known.
2. Then optionally clean up **deprecation warnings** using `UE5.8_Deprecations.txt` (each message names its
   replacement). These are non-blocking unless warnings are errors.

---

## 4. Verified findings — the deprecated symbols this plugin uses mostly SURVIVE in 5.8

Cross-referenced the plugin's actual usage against 5.8-preview1 source:

- **Control Rig legacy header survives.** `ControlRigHandler.cpp` does `#include "ControlRigBlueprintLegacy.h"`.
  That header **still exists** in 5.8 (`ControlRigDeveloper/Public/ControlRigBlueprintLegacy.h`).
  `UControlRigBlueprint` and `UControlRigBlueprintFactory` **still exist**
  (`ControlRigEditor/Public/ControlRigBlueprintFactory.h`). The 5.8 deprecations inside that header are on
  methods the plugin does **not** call (`GeneratePythonCommands(FString)` → `GeneratePythonCommands()`;
  `GetControlRigClass` → `GetControlRigAssetReference`). See §5.1 for the one nuance.
- **GAS stacking members survive.** The pragma-suppressed `UGameplayEffect::StackingType` is still only
  `UE_DEPRECATED(5.7, "…use GetStackingType.")` in 5.8 — **not removed**. So the existing
  `PRAGMA_DISABLE_DEPRECATION_WARNINGS` blocks still compile. `UGameplayAbility::GetInstancingPolicy()` is
  **no longer deprecated at all** in 5.8 (the pragma around it in `ForgeGASCapture.cpp` is now unnecessary
  but harmless).
- **Include surface (verified subset) intact.** Of the plugin's 696 engine includes, the 239 that fall in
  the engine modules checked out here **all exist** in 5.8. The only "misses" were `*.generated.h` (build
  artifacts) and `LandscapeEditorUtils.h` (in the `LandscapeEditor` module, simply not in the checked-out
  subset — not removed).

**Implication:** expect the 5.8 build to be far closer to "clean with warnings" than the 5.6→5.7 jump was.
Budget the real effort for §6/§7 (unverified modules) and §5 (the few confirmed touch-points).

---

## 5. Confirmed touch-points (exact file:line → 5.8 status → action)

### 5.1 Control Rig — `Private/Handlers/ControlRigHandler.cpp`
- `:8  #include "ControlRigBlueprintLegacy.h"` — **OK in 5.8** (header present).
- `:96  UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();` — **OK** (factory present).
- `:636 UControlRigBlueprint* CRBlueprint = Cast<UControlRigBlueprint>(Asset);` — **OK** (class present).
- `:645` author note: `UControlRigBlueprint::GetRigVMClient` ambiguity (C2385) in 5.7. **Re-verify in 5.8** —
  base-class layout around `IRigVMClientHost` may have changed; if the C2385 workaround was a cast, confirm
  it still resolves.
- **5.8 note:** Control Rig core "no longer supports owning simulations" (`ControlRig.h:824-831`,
  `GetPhysicsSimulation`/`RegisterPhysicsSimulation` now return null/deprecated). The plugin does not appear
  to call these; only relevant if a handler queries Control Rig physics sims.

### 5.2 GameplayAbilities — `Private/Handlers/GASHandler.cpp`, `Private/Capture/ForgeGASCapture.cpp`
- Existing `PRAGMA_DISABLE_DEPRECATION_WARNINGS` blocks wrap `UGameplayEffect::StackingType`
  (`GASHandler.cpp:820,929,1117`; `ForgeGASCapture.cpp:407`), `Modifiers` (`ForgeGASCapture.cpp:443`), and
  `GetInstancingPolicy` (`ForgeGASCapture.cpp:300`). **All still compile in 5.8** (StackingType still
  5.7-deprecated; GetInstancingPolicy no longer deprecated).
- **Optional cleanup:** migrate `CDO->StackingType` reads to `CDO->GetStackingType()` and writes to
  `CDO->SetStackingType(...)` (both exist in 5.8, `UE_API`), then delete the pragma. The plugin already does
  the equivalent for tags (`GetAssetTags()` per the 5.5 note at `ForgeGASCapture.cpp:565`).
- **New in 5.8 (only if the plugin adds these calls):** `FActiveGameplayEffectHandle` — the global TMap for
  resolving the owning ASC is gone (`"We no longer use a global TMap to get the Owning ASC…"`); handle
  construction moves to `GenerateNewHandle` / `GetInstantExecutedHandle`. Plugin does not currently call
  these.

### 5.3 Build / Hot Reload — `Private/Capture/ForgeBuildCapture.cpp`
- `:185-193` and `:221-227`: `IHotReloadInterface::OnHotReload()` wrapped in deprecation pragma (author note:
  "deprecated in UE5.7 but still functional; suppress until migrated"). **Verify in 5.8** whether `HotReload`
  module / `OnHotReload()` still exists or was finally removed. If removed, this is a hard error — the
  intended replacement is Live Coding (`ILiveCodingModule::GetOnPatchCompleteDelegate()`, already wired at
  `:265`), so the fallback can simply be dropped.

---

## 6. Removal-risk method (the part the compiler will drive)

457 of the plugin's 696 engine includes live in modules **not** verified here (Editor/Developer modules and
plugins outside the checked-out set: `UnrealEd`, `Niagara`, `LevelSequence`, `MovieRenderPipeline*`,
`Concert*`, `GeometryScripting*`, `TakeRecorder`, `Chooser`, `PoseSearch`, `Water`, `StateTree`,
`SmartObjects`, `GameFeatures`, etc.). To verify these without a compile, expand the sparse checkout that
produced this report:

```bash
# the report's sparse clone lives at C:\Temp\ue58sparse (branch 5.8, blobless)
cd /c/Temp/ue58sparse
git sparse-checkout add Engine/Source/Editor Engine/Source/Developer Engine/Plugins
git checkout
# then re-run the include check in UE5.8_Plugin_Engine_Includes.txt against the tree
```

The plugin's own source is the best removal-precedent guide — it documents prior removals inline, e.g.:
`MovieSceneDoubleVectorTrack.h`/`MovieSceneDoubleVectorSection.h` removed in 5.7
(`AnimationHandler.cpp:17,454`), `GAverageFPS`/`GAverageMS` removed (`ForgePerformanceCapture.cpp:12`),
`FReplaceActorHelper` removed in 5.4 (`ActorHandler.cpp:1196`), `UEdGraphNode::GetDeprecationMessage()`
removed in 5.7 (`ForgeBlueprintCapture.cpp:589`). Search the plugin for `UE 5.7:` and `VERIFIED` — every one
of those is a boundary to re-validate against 5.8.

---

## 7. Known gaps in this recon (be explicit with the agent)

- **Packaging settings deprecations not captured.** `UProjectPackagingSettings` lives in
  `Engine/Source/Developer/*`, which was outside the checked-out set, so `PackagingHandler.cpp` /
  `ForgePackagingCapture.cpp` have **no 5.8 data** here. The author already moved this include to the
  `DeveloperToolSettings` module per a 5.7 note in `Build.cs` — re-verify that module + the settings struct
  in 5.8. (Closing this gap is a one-line sparse-add of `Engine/Source/Developer`.)
- **No actual compiler log exists** — there was no engine build available at capture time. Everything here is
  from source reading, not a build.
- **Full 696-include verification was not completed** — only the 239 in checked-out modules were confirmed
  (all present). The rest is the §6 task.

---

## 8. If the agent is asked to ADD support for the new 5.8 systems

Exact plugin/module/class names are in `UE5.8_Migration_Recon.md`. Quick index of the new handlers it might
add and the real names to drive:
- **Mesh Terrain** → plugins `MeshTerrainMode` (editor mode `UMeshTerrainMode`) + `MeshPartition` (actor
  `AMeshPartition`, component `UMeshPartitionComponent`, asset `UMeshPartitionDefinition`). Experimental, off
  by default.
- **MetaHuman Crowd** → plugin `MetaHumanCrowd`; pipeline `UMetaHumanCrowdPipeline` /
  `UMetaHumanCrowdEditorPipeline` on a `UMetaHumanCollection`; Mass trait
  `UMetaHumanMassCrowdVisualizationTrait`. Experimental, off by default.
- **Procedural Vegetation** → plugin `ProceduralVegetationEditor`; asset `UProceduralVegetation`
  (wraps `UProceduralVegetationGraph : UPCGGraph`); factory `UProceduralVegetationFactory`. Experimental.
- **PCG editable output** ("Viewport Editing" / Data Overrides) → module `PCG`: `FPCGSourceDataContainer`
  on `UPCGComponent`, `FPCGDeltaCollection` (`Add`/`ForEachDelta`/`Remove`/`Empty`),
  `PCG::DataOverride::ApplyDataOverrides(...)`; editor module `PCGEditor`: `UPCGManualEditTool`
  (`UCLASS(Experimental)`). All experimental/in-progress.

---

## 9. Suggested execution order for the fixing agent

1. Drop plugin under a 5.8 engine, generate, **build editor target**, capture the full log.
2. Fix **hard errors**: removed headers/symbols (§6 method), module renames (§5/§7), signature changes.
3. Re-validate every `UE 5.7:` / `VERIFIED` boundary note in the source against 5.8 behavior.
4. Confirm the §5 touch-points compile; optionally migrate the GAS stacking pragmas to getters/setters.
5. Close the packaging gap (§7).
6. Optional: clear deprecation warnings using `UE5.8_Deprecations.txt` (each names its replacement).
7. Smoke-test in-editor: load the plugin, exercise a few handlers (PCG, GAS, ControlRig, Packaging).
