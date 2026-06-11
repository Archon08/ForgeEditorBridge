# UE 5.8 Migration Recon — ForgeEditorBridge

> **Status of source:** Gathered from `EpicGames/UnrealEngine` branch **`5.8`** (only tag is
> `5.8.0-preview-1`). Treat everything here as **preview, not final** — names and APIs may still shift.
> **Method:** GitHub API navigation of plugin internals + a blobless sparse `git clone` of the `5.8`
> branch to grep real headers. No engine build was performed (no engine/toolchain available at capture
> time), so item 1 is the deprecation-marker fallback, not a compiler log.

Companion file: [`UE5.8_Deprecations.txt`](./UE5.8_Deprecations.txt) — 844 `UE_DEPRECATED(5.8, …)`
markers, sorted, with file path + message.

---

## 1. Deprecation inventory (build-log fallback)

No engine build was possible, so this is the concrete fallback: every `UE_DEPRECATED(5.8, …)` marker in
the relevant modules, grepped from a sparse checkout.

**Coverage caveat:** sparse set = `Runtime/{Engine, Core, CoreUObject, Landscape}` +
`Plugins/{PCG, GameplayAbilities, ControlRig, Metasound, Mover}`. It does **not** include
`Engine/Source/Developer/*` — so `UProjectPackagingSettings` packaging-settings deprecations are **not**
captured here yet.

| Module | `UE_DEPRECATED(5.8…)` count |
|---|---|
| `Runtime/Engine` | 381 |
| `Plugins/PCG` | 154 |
| `Runtime/CoreUObject` | 98 |
| `Plugins/Runtime/Metasound` | 89 |
| `Runtime/Core` | 76 |
| `Plugins/Animation/ControlRig` | 23 |
| `Runtime/Landscape` | 9 |
| `Plugins/Experimental/Mover` | 8 |
| `Plugins/Runtime/GameplayAbilities` | 5 |
| **Total** | **844** |

### Subsystem highlights

**Character movement** — `CharacterMovementComponent.h` (16), `Character.h` (16),
`CharacterMovementReplication.h` (3). Theme: movement-base functions taking `UPrimitiveComponent*` are
deprecated in favor of a new `FMovementBaseInterfaceData` struct.
- `"Movement functions using UPrimitiveComponents have been deprecated. Use functions using the FMovementBaseInterfaceData struct instead."` (×11)
- `GetMovementBase` → `GetMovementBaseObject`
- `GetLastServerMovementBase` → `GetLastServerMovementBaseInterfaceData`
- replication props → `MovementBasePhysicsObjectOwner` / `NewMovementBasePhysicsObjectOwner` / `StartMovementBaseInterfaceData` / `EndMovementBaseInterfaceData`
- New `Mover` plugin has its own 8 markers (`GroundMovementUtils`, `AirMovementUtils`, `FloorQueryUtils`, `RollbackBlackboard`).

**String tables** — `Core/Public/Internationalization/StringTableCore.h` (2):
- `ExportStrings` → `ExportStringsToCSVFile`
- `ImportStrings` → `ImportStringsFromCSVFile`

**Gameplay abilities** (5) — `ActiveGameplayEffectHandle.h`, `AbilitiesGameplayEffectComponent.h`:
- `"We no longer use a global TMap to get the Owning ASC. You can remove this function."` (×2)
- handle construction → `GenerateNewHandle` / `GetInstantExecutedHandle`
- `"Use the version that takes an FActiveGameplayEffect (because they may be PendingRemove by now, so the Handle won't resolve)"`

**Control Rig** (23) — headline `"Control Rig core no longer supports owning simulations"` (×3 in
`ControlRig.h`); plus editor churn (`SRigHierarchyTreeView`, `SModularRigHierarchyTreeView`,
`ControlRigEditor`, `ControlRigBlueprintLegacy`, `ControlRigIOMapping`).

**MetaSound** (89) — concentrated in `MetasoundFrontendDocumentBuilder.h` (17),
`Interfaces/MetasoundFrontendInterfaceRegistry.h` (8), `MetasoundFrontendController.h` (7),
`MetasoundFrontendNodeClassRegistry.h` (5), `MetasoundEditorModule.h` (5), `MetasoundAssetBase.h` (5).
Themes: `GetTemplateAs()` / `SetTemplate` replacing direct template access; thread-safety —
`"Direct access to IInterfaceRegistryEntry is deprecated as it is not thread safe…"`.

**PCG** (154) — hotspots `PCGSubsystem.h` (16), `PCGWorldData.h` (11), `PCGComponent.h` (10),
`PCGGenerateLandscapeTextures.h` (10), `PCGGraph.h` (9), `PCGSettingsHelpers.h` (8). Notables:
`PCGGraph.h` → `"Use ShouldCook instead"`; `PCGEditor.h` → `EPCGEditorPanel` enum → `PCGEditorTabs`,
`GetMainEditorGraph/GetFocusedEditorGraph` renames.

**Cross-cutting (will bite a bridge plugin):** `SoundWave.h` (37), `SceneView.h` (11), `App.h` (10),
`UnrealClient.h` / `SceneViewport.h` (viewport), `UObjectGlobals.h`
(`RequiresCookedData` → `FPlatformProperties::RequiresCookedData()`), `World.h`
(`ChangeFeatureLevel` → `ShaderPlatformChanged`).

---

## 2. Mesh Terrain

- **Editor plugin name:** **"Mesh Terrain Mode"** (`Engine/Plugins/Experimental/MeshTerrainMode`),
  *"a suite of interactive tools for creating and editing Mesh Partitions in the Editor."* Category
  **"Mesh Partition"**. Pairs with **"Mesh Partition"** (`Engine/Plugins/Experimental/MeshPartition`),
  the runtime data/actor system it authors.
- **Status:** **Experimental, OFF by default** (`EnabledByDefault: false`, `IsExperimentalVersion: true`).
  `MeshTerrainMode` module is `Type: Editor`.
- **What you create:** an **editor mode, not an asset.** Enter the **"Mesh Terrain"** mode
  (id `"EM_MeshTerrainMode"`); its tools produce a **"Mega Mesh"** — an `AMeshPartition` actor
  (`NotPlaceable`) with a `UMeshPartitionComponent`, configured by a `UMeshPartitionDefinition`
  (`UDataAsset`).
- **Workflow (exact UI labels):** submode tabs **Shapes / Create / Edit / Sculpt / Paint / Modifiers**.
  Create → *"Create Rectangle"* / *"Import Heightmap"*. Sculpt → *"Height Sculpt," "Height Smooth,"
  "Height Flatten," "Slope Erode."* Edit → *"Convert," "Split," "Merge," "Stitch," "Expand."* Modifiers →
  non-destructive *Mesh / Texture / Spline / Brush / Noise / Boolean / Remesh*. Stylus supported.
- **Replace Landscape?** **No — sits alongside.** No dependency on the `Landscape` module; `Runtime/Landscape`
  still ships. Built on the Geometry/Modeling stack + MeshPartition. Long-term intent vs Landscape is
  **not stated in source**.

---

## 3. MetaHuman Crowd

- **Editor plugin name:** **"MetaHuman Crowd"** (`Engine/Plugins/MetaHuman/MetaHumanCrowd`),
  *"Support for crowds of MetaHumans"*, category **"MetaHuman"**.
- **Status:** **Experimental, OFF by default** (`EnabledByDefault: false`, `IsExperimentalVersion: true`,
  `Installed: false`). Modules `MetaHumanCrowd` (Runtime) + `MetaHumanCrowdEditor` (Editor).
- **What you create — nuance:** this plugin defines **no standalone Crowd asset, factory, or editor
  toolkit, and no UI strings of its own.** It plugs into the existing **MetaHuman Collection** framework
  (`MetaHumanCharacterPalette` plugin). You create a **`UMetaHumanCollection`** whose pipeline is set to
  **`UMetaHumanCrowdPipeline`**; the object you configure is **`UMetaHumanCrowdEditorPipeline`** (LODs,
  actor vs instanced-mesh build options, validation). New data asset: **`UMetaHumanCrowdAnimationConfig`**
  (`UDataAsset`).
- **Workflow:** create/configure a MetaHuman Collection → assign `UMetaHumanCrowdPipeline` → fill wardrobe
  slots (`HeadSlotName`, `BodySlotName`, `OutfitsSlotName`, `TopGarmentSlotName`, `BottomGarmentSlotName`,
  `ShoesSlotName`) → set LOD/mesh-build options → assign `UMetaHumanCrowdAnimationConfig` → **Build** →
  drive at runtime via **Mass**: a Mass spawner whose entity config uses
  **`UMetaHumanMassCrowdVisualizationTrait`** (display name **"MetaHuman Crowd Visualization"** — the only
  user-facing string in the plugin).
- **Dependencies:** heavily **Mass** (`MassEntity`, `MassCrowd`, `MassRepresentation`, `MassLOD`…),
  **MetaHumanCharacter/Palette/DefaultPipeline**, **Mover**, **SmartObjects/StateTree**, and (editor)
  Hair/Cloth (`ChaosClothAsset`, `ChaosOutfitAsset`, `RigLogic`). **No PCG, no Niagara.**

---

## 4. Procedural Vegetation Editor

- **Editor tool name:** **"Procedural Vegetation Editor"**
  (`Engine/Plugins/Experimental/ProceduralVegetationEditor`), *"Node Graph based Editor that allows users
  to create Nanite Foliage ready vegetation directly in the engine…"*
- **Status:** **Experimental, OFF by default** (`EnabledByDefault: false`, `IsExperimentalVersion: true`).
  Modules `ProceduralVegetation` (Runtime, `PostConfigInit`) + `ProceduralVegetationEditor` (Editor).
  Win64/Linux/Mac only.
- **Asset you create:** **`UProceduralVegetation`** — which **wraps a PCG graph**
  (`UProceduralVegetationGraph : public UPCGGraph`). Created via `UProceduralVegetationFactory`
  (Content Browser **New**, **Foliage** category); registered via `UAssetDefinition_ProceduralVegetation`.
  Legacy `UProceduralVegetationPreset` is `[DEPRECATED]`. `UProceduralVegetationLink : UAssetUserData`
  back-links an exported mesh to its source.
- **Workflow (exact labels):** **New** → modal **"Create Procedural Vegetation"** → opens **`FPVEditor`**
  (PCG-graph editor + 3D preview; commands *"Export"* Ctrl+E, *"Mannequin"* Ctrl+M, *"Scale Visualization,"
  "Stats Overlay"*) → **"Export"** writes a real **Static Mesh or Skeletal Mesh**
  (`EPVExportMeshType { StaticMesh, SkeletalMesh }`, `bCreateNaniteFoliage = true`, optional wind/collision)
  → place that mesh in the level like any normal mesh. No dedicated placement mode.
- **Relationships:** built **on PCG** + Dataflow / GeometryScripting / DynamicWind. **Independent of the
  old `UProceduralFoliageSpawner`.**

---

## 5. PCG editable output ("Viewport Editing" / Data Overrides)

Real in 5.8, **Experimental**. Editor terms: **"Viewport Editing" / "Manual" / "Data Overrides."** UI
strings: `"Viewport Editing ({0})"`, command **"Edit in Viewport"** (hotkey **V**),
**"Mark for Viewport Editing"**, and a PCG editor-mode **"Manual"** context.

**Mechanism:** edits stored as **deltas** keyed by node/pin hash, kept on **`UPCGComponent`** and
re-applied every regeneration — that's what preserves the link to the graph:
```cpp
// PCGComponent.h
UPROPERTY() FPCGSourceDataContainer SourceDataContainer;
```

**API — apply / list / clear** (`PCGDataOverride.h`, `struct FPCGDeltaCollection`):
- apply/add: `void Add(const FPCGDeltaKey&, TInstancedStruct<FPCGDeltaBase>&&)` / `Add_GetRef(...)`
- list: `bool ForEachDelta(TFunctionRef<bool(const FPCGDeltaKey&, TInstancedStruct<FPCGDeltaBase>&)>)`
  (+ `Find`, `Contains`, `Num`, `IsEmpty`)
- clear: `bool Remove(const FPCGDeltaKey&)` / `void Empty()`

Storage container (`PCGSourceDataContainer.h`, `struct FPCGSourceDataContainer`, mutation `#if WITH_EDITOR`):
`Store(...)`, `GetMutable(...)`, `Get(...)`, `Num()`, `Remove(...)`, `Empty()`.

Apply at execution (`PCGDataOverrideHelpers.h`, namespace `PCG::DataOverride`):
`ApplyDataOverrides(FPCGContext*, TArrayView<FPCGTaggedData>)`, `HasNodeLevelDataOverrides(...)`,
`HasPinLevelDataOverrides(...)`, `GetMutableSourceDataContainer(IPCGGraphExecutionSource*)`, and editor-only
list-keys `CollectNodeStorageKeys(const UPCGNode*, const IPCGGraphExecutionSource*)`.

Per-node enablement (`UPCGSettings`): `IsMarkedForManualEditing()` / `SetMarkedForManualEditing(bool)` /
`SetTemporaryManualEditingEnabled(bool)`.

**Where it lives:** runtime in module **`PCG`** (`Source/PCG/Public/Graph/DataOverride/` +
`…Points.h`, `…Spline.h`, `…Polygon.h`); UI in module **`PCGEditor`** (`SPCGManualEditPanel`,
`PCGManualEditPanelManager`, tool `UPCGManualEditTool` — `UCLASS(Experimental)`).

**Preview caveats:** tool is `UCLASS(Experimental)`; mutation is `WITH_EDITOR`-only (runtime read-only);
several `@todo_pcg` markers remain (attribute-keyed deltas, local-space offsets incomplete). Point
transform/offset/delete/insert path is fully wired.

---

## 6. Programmer-facing class → module map

**Mesh Terrain** — plugin **`MeshTerrainMode`** (module `MeshTerrainMode`, Editor): `UMeshTerrainMode`
(the `UEdMode`), `FMeshTerrainModeToolkit`, `FMeshTerrainModeManagerCommands`, `FMeshTerrainModeStyle`.
Plugin **`MeshPartition`** (modules `MeshPartition` Runtime, `MeshPartitionEditor`, `MeshPartitionCompute`,
`MeshPartitionModelingToolset`): `AMeshPartition`, `UMeshPartitionComponent`, `UMeshPartitionDefinition`,
tool builders `UConvertToolBuilder` / `USplitToolBuilder` / `UMergeToolBuilder` / `UStitchToolBuilder` /
`UExpandToolBuilder` (namespace `UE::MeshPartition`).

**MetaHuman Crowd** — plugin **`MetaHumanCrowd`**. Module `MetaHumanCrowd` (Runtime):
`UMetaHumanCrowdPipeline`, `UMetaHumanMassCrowdVisualizationTrait`, `UMetaHumanMassRepresentationSubsystem`,
`UMetaHumanCrowdAppearanceProvider`, item pipelines
`UMetaHumanCrowd{Character,Head,Groom,Outfit,SkeletalClothing}Pipeline`, `UMetaHumanCrowdStatsLibrary`.
Module `MetaHumanCrowdEditor` (Editor): `UMetaHumanCrowdEditorPipeline`, `UMetaHumanCrowdAnimationConfig`,
`…EditorPipeline` item counterparts.

**Procedural Vegetation** — plugin **`ProceduralVegetationEditor`**. Module `ProceduralVegetation`
(Runtime): `UProceduralVegetation`, `UProceduralVegetationGraph : UPCGGraph`,
`UProceduralVegetationGraphInstance : UPCGGraphInstance`, `UProceduralVegetationLink : UAssetUserData`,
`UPVData`, export node `UPVExportSettings`. Module `ProceduralVegetationEditor` (Editor):
`UProceduralVegetationFactory`, `UAssetDefinition_ProceduralVegetation`, `FPVEditor : FPCGEditor`,
`FPVEditorCommands`, `PVExporter`.

**PCG editable output** — plugin **`PCG`**. Module `PCG` (Runtime): `FPCGSourceDataContainer`,
`FPCGSourceDataStorageKey`, `FPCGDeltaCollection`, `FPCGDeltaBase` / `FPCGDeltaKey`, point deltas
`FPCGPointTransformDelta` / `FPCGPointTransformOffsetDelta` / `FPCGPointDeletionDelta` /
`FPCGPointInsertionDelta`, helpers namespace `PCG::DataOverride`, flags on `UPCGSettings`, storage on
`UPCGComponent`. Module `PCGEditor` (Editor): `SPCGManualEditPanel`, `FPCGManualEditPanelManager`,
`UPCGManualEditTool`, `UPCGManualEditToolBuilder`, `IPCGDeltaViewportExtension`.

---

## Open gaps

- **Packaging settings** deprecations not captured (would need `Engine/Source/Developer` +
  `Engine/Source/Editor` added to the sparse checkout and re-grepped).
- A **targeted cross-reference** of ForgeEditorBridge's own API calls against the 844-marker list has not
  yet been generated — that would turn this from "engine-wide list" into "exact calls in this plugin that
  are now deprecated."
