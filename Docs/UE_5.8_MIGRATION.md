# UE 5.8 Migration & Control-Coverage Plan

Status: **Recon complete — ready for compile pass.** Originally drafted against public
preview coverage; now updated with **confirmed facts from the actual `EpicGames/UnrealEngine`
`5.8` branch** (tag `5.8.0-preview-1`), delivered by an engine-access recon session. All
former **[verify]** tags are resolved below. Plugin currently targets **UE 5.7**
(`VersionName 0.2.6`).

**Companion data (authoritative, from engine source):**
- [`UE5.8_Migration_Handoff.md`](./UE5.8_Migration_Handoff.md) — execution playbook for the migration agent
- [`UE5.8_Migration_Recon.md`](./UE5.8_Migration_Recon.md) — new-system class/module names
- [`UE5.8_Deprecations.txt`](./UE5.8_Deprecations.txt) — 844 `UE_DEPRECATED(5.8, …)` markers
- [`UE5.8_Plugin_Engine_Includes.txt`](./UE5.8_Plugin_Engine_Includes.txt) — 696 plugin includes; 239 verified present in 5.8

> Still preview: names can shift before GA. The one thing recon could not produce is a real
> **compiler log** (no engine build was available) — that remains Phase 0's job.

---

## 0. Build baseline (the remaining blocker)

- [ ] Put the plugin under a 5.8 engine (source build of branch `5.8` or Launcher 5.8 Preview),
      regenerate, build the editor target, **capture the full log**.
- [ ] Triage **hard errors first** (removals/renames — leave no marker, fail the build), then
      warnings (the 844-marker list names every replacement).
- [ ] Bump `Docs/stack/build_manifest.py` `ue_version` `"5.7"`→`"5.8"`; regenerate KG manifest
      against the 5.8 header tree.

## 1. Compile migration — what is now KNOWN

### 1a. Confirmed touch-points (plugin code ↔ 5.8 source, verified)
- **GAS** — `GASHandler.cpp:820-1117`, `ForgeGASCapture.cpp:300-443`: pragma-suppressed
  `StackingType` / `Modifiers` **still compile** (still 5.7-deprecated, not removed);
  `GetInstancingPolicy()` is **no longer deprecated** in 5.8 (pragma now unnecessary).
  - [ ] Optional cleanup: migrate to `GetStackingType()`/`SetStackingType()`, drop pragmas.
- **Control Rig** — `ControlRigHandler.cpp`: `ControlRigBlueprintLegacy.h`,
  `UControlRigBlueprint`, `UControlRigBlueprintFactory` **all survive** in 5.8. The 5.8
  deprecations in that header are on methods we don't call.
  - [ ] Re-verify the `GetRigVMClient` C2385 ambiguity workaround (`:645`) on 5.8.
- **Hot Reload** — `ForgeBuildCapture.cpp:185-227`: `IHotReloadInterface::OnHotReload()` is the
  **top removal risk** (deprecated since 5.7; may be gone in 5.8 — outside recon's checkout).
  - [ ] If removed: drop the fallback; Live Coding path already wired at `:265`.
- **Material usage flags** — `ForgeMaterialCapture.cpp:199-212` reads 14 `bUsedWith*` members
  directly; `Material.h` carries a 5.8 marker *"Always use the GetUsageByFlag() accessor."*
  **(New finding — not in recon §5.)**
  - [ ] Migrate to `GetUsageByFlag(MATUSAGE_*)` during the compile pass.

### 1b. Warning watchlist (plugin-include ∩ 5.8-marker headers = 45 headers)
Cross-reference generated from the two data files. Top intersections the plugin includes:
`Sound/SoundWave.h` (37 markers), `UObject/UnrealType.h` (35 — FProperty ctor signature
changes; relevant to ReflectionHandler), `Materials/Material.h` (29), `PCGComponent.h` (10),
`PCGGraph.h` (9), `UnrealClient.h` (6 — `FViewportFrame`→`FSceneViewport`),
`Components/SkeletalMeshComponent.h` (4 — cloth-collision API → `CollisionSources`),
`Engine/World.h` (3 — `ChangeFeatureLevel`→`ShaderPlatformChanged`),
`Internationalization/StringTableCore.h` (2 — `ExportStrings`/`ImportStrings` →
`…ToCSVFile`/`…FromCSVFile`; check `StringTableHandler`/`LocalizationHandler`),
`ControlRigBlueprintLegacy.h` (2), `LandscapeProxy.h` (2), `LandscapeComponent.h` (1 —
`GetGrassTypes`→`GetNamedGrassTypes`). Grep of plugin source found **no current calls** to the
renamed symbols beyond §1a — these headers are watchlist, not work.

### 1c. Unverified surface (where compile errors will come from)
- [ ] **457 of 696 includes** live in modules outside recon's sparse checkout (`UnrealEd`,
      `Niagara`, `LevelSequence`, `MovieRenderPipeline*`, `Concert*`, `GeometryScripting*`,
      `TakeRecorder`, `Chooser`, `PoseSearch`, `Water`, `StateTree`, `SmartObjects`,
      `GameFeatures`, …). Verify at compile, or expand the sparse checkout
      (`git sparse-checkout add Engine/Source/Editor Engine/Source/Developer Engine/Plugins`).
- [ ] **Packaging settings gap**: `UProjectPackagingSettings` (`Engine/Source/Developer`) had no
      recon coverage — re-verify `DeveloperToolSettings` module + struct on 5.8.
- [ ] Re-validate every `UE 5.7:` / `VERIFIED` boundary comment in plugin source against 5.8.

### 1d. Version metadata
- [ ] `uplugin` `VersionName` → `0.3.0`; consider `"EngineVersion": "5.8.0"` at GA (not before).
- [ ] Update `"version"` in `BridgeHttpServer::Start` (`bridge-status.json`) + startup log.

---

## 2. New 5.8 systems → handlers (names now CONFIRMED from source)

All four systems are **Experimental, off by default** → every new handler must
plugin-present-check and degrade gracefully (soft class resolution, no hard `Build.cs` dep on
experimental plugins — same pattern as `ForgeAnimationCapture`'s soft `IKRig.IKRigComponent`).

### 2.1 Mesh Terrain → new `MeshTerrainHandler`
Plugins **`MeshTerrainMode`** (editor mode `UMeshTerrainMode`, id `EM_MeshTerrainMode`) +
**`MeshPartition`** (runtime: actor `AMeshPartition` (NotPlaceable), `UMeshPartitionComponent`,
data asset `UMeshPartitionDefinition`; tool builders `UConvertToolBuilder`/`USplitToolBuilder`/
`UMergeToolBuilder`/`UStitchToolBuilder`/`UExpandToolBuilder`, ns `UE::MeshPartition`).
**Sits alongside Landscape — does not replace it** (keep `LandscapeHandler` as-is).
- [ ] Actions: create-from-rectangle / import-heightmap, sculpt ops (Height Sculpt/Smooth/
      Flatten, Slope Erode), edit ops (Convert/Split/Merge/Stitch/Expand), add/list modifiers
      (Mesh/Texture/Spline/Brush/Noise/Boolean/Remesh), query partitions+bounds.

### 2.2 MetaHuman Crowd → new `MetaHumanCrowdHandler`
Plugin **`MetaHumanCrowd`**. No standalone crowd asset — it extends MetaHuman Collections:
`UMetaHumanCollection` + pipeline `UMetaHumanCrowdPipeline` / editor
`UMetaHumanCrowdEditorPipeline`; data asset `UMetaHumanCrowdAnimationConfig`; runtime via Mass
trait `UMetaHumanMassCrowdVisualizationTrait` (+ `UMetaHumanMassRepresentationSubsystem`).
- [ ] Actions: assign crowd pipeline to a collection, set wardrobe slots
      (`HeadSlotName`/`BodySlotName`/`OutfitsSlotName`/…), set LOD/build options, assign anim
      config, trigger Build, wire a Mass spawner entity config with the visualization trait.
- [ ] Heavy optional-dep surface (Mass, MetaHumanCharacter/Palette, Mover, Chaos cloth) —
      strictly soft-resolved.

### 2.3 Procedural Vegetation → new `ProceduralVegetationHandler`
Plugin **`ProceduralVegetationEditor`**. Asset **`UProceduralVegetation`** wrapping
`UProceduralVegetationGraph : UPCGGraph`; factory `UProceduralVegetationFactory`; editor
`FPVEditor : FPCGEditor`; export via `UPVExportSettings` /
`EPVExportMeshType{StaticMesh,SkeletalMesh}`, `bCreateNaniteFoliage`.
Independent of legacy `UProceduralFoliageSpawner` (keep `FoliageHandler` as-is).
- [ ] Actions: create asset, edit its PCG graph (reuse `PCGGraphHandler` machinery — it IS a
      PCG graph), export to static/skeletal mesh (Nanite foliage opt), place exported mesh.

### 2.4 PCG editable output → extend `PCGHandler` / `PCGGraphHandler`
Module `PCG`: deltas stored on `UPCGComponent` (`FPCGSourceDataContainer SourceDataContainer`),
keyed `FPCGDeltaKey` → `FPCGDeltaCollection` (`Add`/`Add_GetRef`/`ForEachDelta`/`Find`/
`Remove`/`Empty`); apply at exec `PCG::DataOverride::ApplyDataOverrides(...)`; per-node opt-in
`UPCGSettings::IsMarkedForManualEditing()/SetMarkedForManualEditing()`. Point
transform/offset/delete/insert deltas are wired; attribute deltas still `@todo_pcg`.
- [ ] Actions: mark/unmark node for manual editing, apply point transform/offset/delete/insert
      delta, list deltas (`ForEachDelta`), clear deltas, regenerate-preserving-edits.
- [ ] Mutation is `WITH_EDITOR`-only — fine (plugin is editor-only).

---

## 3. Capture-module review

- [ ] New: mesh-partition inventory (actors, definitions, modifier stacks) — pair with §2.1.
- [ ] `ForgePCGCapture`: surface per-component delta counts (`SourceDataContainer`).
- [ ] `ForgeMaterialCapture`: §1a flag migration; add Substrate fields if present in 5.8.
- [ ] `ForgePerformanceCapture`: Lumen GI mode (radiance cache) + Nanite-foliage counters.
- [ ] Crash-hardening follow-up (not 5.8-specific): 5 more unsafe
      `GEditor->GetEditorWorldContext().World()` sites found — `BridgeHttpServer.cpp:541`,
      `ForgeNiagaraCapture.cpp:334`, `ForgeCollisionCapture.cpp:293`,
      `ForgeInputCapture.cpp:130`, `ForgeWorldGenCapture.cpp:168` — same fix as PR #3.

## 4. Verification checklist (definition of done)

- [ ] Clean compile on 5.8, zero deprecation warnings (or each remaining one ticketed).
- [ ] Plugin loads on 5.8; all existing domains register; `bridge-status.json` updated.
- [ ] New handlers (§2) exercised end-to-end via `/command` on a 5.8 project **with and
      without** the experimental plugins enabled (graceful-degrade path tested).
- [ ] Full context capture on a 5.8 project; JSON diff reviewed.
- [ ] KG manifest regenerated for 5.8; new symbols searchable.
- [ ] Regression pass over 5.7-era domains.

## 5. Standing decisions

- **Branch strategy: permanent version branches.** `ue5.7` is the **forever home** of the
  UE 5.7 version — it is never migrated, only maintained (bug fixes may be cherry-picked to
  it). All 5.8 migration and new-system work happens on **`ue5.8`**. No version-straddling
  `#if ENGINE_MINOR_VERSION` guards — each branch stays clean for its engine.
- **Optional plugins degrade gracefully** — report "unavailable", never fail to load.
- Implement against the **`5.8` release branch**, not `ue5-main`; expect a touch-up pass at GA
  (all four new systems are experimental and may rename).
