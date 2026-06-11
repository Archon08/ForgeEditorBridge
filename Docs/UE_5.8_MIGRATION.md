# UE 5.8 Migration & Control-Coverage Plan

Status: **Planning** — prepared June 2026 against the UE 5.8 **Preview** (released
mid-May 2026; final release expected mid-to-late 2026). The plugin currently
targets **UE 5.7** (`VersionName 0.2.6`). This document is the work list for
moving ForgeEditorBridge to 5.8 while extending "total editor control" to the
new 5.8 systems.

> ⚠️ 5.8 is in Preview. Exact module names, class names, and deprecation lists
> below are drawn from public preview coverage and the `ue5-main` branch and
> **must be re-verified against the actual 5.8 release branch headers** before
> implementation (see Phase 0). Items not yet confirmed against source are
> tagged **[verify]**.

---

## 0. Getting the 5.8 source & a build baseline

The engine is **not** vendored in this repo; the plugin builds against an
installed engine. To develop and test against 5.8:

- **GitHub source (private):** `EpicGames/UnrealEngine` — requires linking your
  Epic Games and GitHub accounts to gain access.
  - `ue5-main` — bleeding edge, **already bumped to 5.8**; may not compile.
  - `5.8` release branch — created when 5.8 ships; the target we build against.
  - `release` — currently 5.7; will roll to 5.8 at GA.
- **Launcher build:** install "Unreal Engine 5.8 Preview" via the Epic Games
  Launcher for a binary engine to smoke-test the plugin without compiling the
  engine.

**Phase 0 tasks**
- [ ] Install/checkout UE 5.8 Preview (Launcher binary + `5.8` source branch).
- [ ] Add a `5.8` engine entry to local build tooling (`Docs/stack/` manifest
      `ue_version` is hardcoded `"5.7"` in `build_manifest.py:364` — bump to
      `"5.8"` and regenerate the KG manifest).
- [ ] Do a clean compile of the plugin against 5.8 and **capture the full
      warning/error log** — that log is the authoritative input to Phase 1–2.
      Everything below is the *expected* delta; the compile log is *truth*.

---

## 1. Compile / build migration (blocking)

The plugin is editor-only with a wide dependency surface
(`ForgeEditorBridge.Build.cs` lists ~120 modules). Most 5.7→5.8 breakage shows
up as (a) module relocations and (b) deprecated-API warnings.

### 1a. Module dependency audit
Re-validate every entry in `Build.cs` against 5.8 — 5.7 already moved several
modules (the file is full of `// 5.7:` relocation notes), and 5.8 continues
that pattern. Highest-risk entries to re-check **[verify]**:

- [ ] `Chooser` internal include path (`Chooser/Internal/Chooser.h`) — internal
      headers move often.
- [ ] `ControlRigDeveloper` / `ControlRigEditor` (Control Rig is actively
      refactored every release; `ControlRigBlueprintLegacy.h` may move/rename).
- [ ] `PlacementHandler` deps (`EditorFramework`, `TypedElementFramework`,
      `TypedElementRuntime`) — TypedElement APIs still churning.
- [ ] `MetasoundFrontend` / `MetasoundEditor` — MetaSound API surface changes.
- [ ] PCG modules (`PCG`, `PCGEditor`) — large 5.8 PCG changes (see §2.4).
- [ ] Packaging deps (`DeveloperToolSettings`, `TargetPlatform`) — settings
      classes relocate between `DeveloperToolSettings` / `EngineSettings`.
- [ ] Concert module set (`Concert`, `ConcertClient`, `ConcertSyncClient`,
      `ConcertTransport`) — multi-user stack reorganizes periodically.

### 1b. Known/likely deprecations to clear **[verify against compile log]**
Reported deprecated in 5.8 (removed in 5.9) — fix now so the 5.9 jump is clean:

- [ ] `UCharacterMovementComponent::SetMovementMode` →
      `SetMovementModeWithCustomMode`. Check `MoverHandler`,
      `ForgeNetworkCapture`, any movement-mode code.
- [ ] `FText::FromStringTable` legacy overload → the `FStringTable&` overload.
      Check `StringTableHandler`, `LocalizationHandler`,
      `ForgeLocalizationCapture`.
- [ ] GAS legacy attribute-set initialization helpers deprecated → check
      `GASHandler`, `ForgeGASCapture`.
- [ ] Sweep for any `UE_DEPRECATED` hits across all handlers/captures and treat
      warnings-as-errors locally during the migration pass.

### 1c. Version metadata
- [ ] Bump `ForgeEditorBridge.uplugin` `VersionName` (e.g. `0.3.0`) and add an
      explicit `"EngineVersion": "5.8.0"` once locked to GA (omit while on
      Preview to keep the plugin loadable across 5.7/5.8 during transition).
- [ ] Update the `"version"` string emitted in
      `BridgeHttpServer::Start` (`bridge-status.json`, currently `"0.2.6"`) and
      the startup log in `ForgeAISubsystem::StartBridge`.
- [ ] Search-and-replace stale `// UE 5.7` / `5.7:` comments that no longer
      describe reality after the audit.

---

## 2. New 5.8 systems → new/extended handlers (control coverage)

"Total control" means every new authorable system in 5.8 gets a bridge handler
(write) and, where it carries inspectable state, a capture module (read). The
major net-new surfaces in 5.8:

### 2.1 Mesh Terrain  — **new handler** `MeshTerrainHandler`  **[verify]**
New experimental 3D-mesh-based terrain system (caves/overhangs/overhangs that
heightmap landscape can't represent). Distinct from the existing
`LandscapeHandler` / `ForgeHeightmapCapture` (which stay for classic Landscape).
- [ ] Identify the plugin/module + subsystem/actor classes in 5.8 source.
- [ ] New `MeshTerrainHandler`: create/spawn mesh terrain, edit/sculpt ops,
      query bounds & layers, convert/import mesh→terrain.
- [ ] Add `Build.cs` deps for the new module(s).
- [ ] Capture: extend or add a capture for mesh-terrain inventory + stats.
- [ ] Register the new domain in `ForgeAISubsystem` handler map.

### 2.2 MetaHuman Crowd — **new handler** `MetaHumanCrowdHandler`  **[verify]**
New experimental plugin: LOD assembly pipeline (hero actor ↔ instanced skeletal
mesh by camera distance), scales tens→thousands.
- [ ] Identify the MetaHuman Crowd plugin module + authoring subsystem.
- [ ] New handler: spawn/configure crowd, set density/LOD policy, assign source
      MetaHumans, bake/assemble.
- [ ] Gate behind plugin-present check (MetaHuman is an optional plugin; do
      **not** add a hard `Build.cs` dependency — resolve classes by soft path
      like `ForgeAnimationCapture` does for `IKRig.IKRigComponent`).

### 2.3 Lumen Radiance Cache & Substrate NPR — **extend** existing handlers
New medium-quality GI mode + experimental stylised Substrate shading.
- [ ] `LightingHandler` / `PostProcessHandler`: expose the new Lumen GI mode
      toggle + scalability/console vars (radiance cache on/off, quality).
- [ ] `MaterialHandler`: support the Substrate NPR shading model path
      (set shading model / blendable GBuffer mode on Substrate materials).
- [ ] Capture: add Lumen GI mode + Substrate mode to
      `ForgePerformanceCapture` / lighting context so the agent can read current
      render settings.

### 2.4 PCG editable procedural output — **extend** `PCGHandler` / `PCGGraphHandler`
5.8 lets artists make manual edits on generated PCG output while keeping the
graph link (no more one-way bake).
- [ ] Add actions for the persisted-edit workflow: apply manual edit, list
      overrides, clear/reset overrides, re-run keeping edits.
- [ ] Re-verify PCG API surface (large 5.8 changes) — likely renames in
      `PCG`/`PCGEditor`. Reconcile with §1a.

### 2.5 Procedural Vegetation Editor (PVE) — **new/extended handler**  **[verify]**
Significantly enhanced in 5.8: author biologically-accurate, Nanite-ready
vegetation in-editor; import meshes from external tools.
- [ ] Decide: extend `FoliageHandler` vs. new `ProceduralVegetationHandler`
      (lean new handler — PVE is its own authoring graph/asset, not classic
      foliage instances).
- [ ] Actions: create PVE asset, set species/growth params, import source mesh,
      bake to Nanite, scatter to level.

---

## 3. Capture-module review (read side)

Cross-check every capture in `Private/Capture/` against 5.8 reflection/API:
- [ ] `ForgeHeightmapCapture` — confirm classic Landscape still present and add
      a sibling note pointing at Mesh Terrain (§2.1).
- [ ] `ForgePerformanceCapture` — add Lumen GI mode / Substrate / Nanite-vegetation
      counters.
- [ ] `ForgeNiagaraCapture`, `ForgeMaterialCapture` — Substrate field changes.
- [ ] Re-run a full capture against a 5.8 project and diff the JSON for missing
      or renamed fields.

---

## 4. Tooling / KG stack (`Docs/stack/`)

- [ ] `build_manifest.py`: bump `ue_version` `"5.7"`→`"5.8"` and regenerate.
- [ ] Re-index engine headers from the 5.8 source tree (new modules: Mesh
      Terrain, MetaHuman Crowd) so the knowledge graph covers new symbols.
- [ ] Validate header-root resolution against the 5.8 engine layout.

---

## 5. Verification checklist (definition of done)

- [ ] Clean compile against UE 5.8 with **zero** deprecation warnings.
- [ ] Plugin loads in the 5.8 editor; `bridge-status.json` reports `5.8` + new
      version string; all existing domains register.
- [ ] Each new handler (§2) exercised end-to-end via `/command` against a 5.8
      project; results land in bridge results.
- [ ] Full context capture runs clean against a 5.8 project; JSON diff reviewed.
- [ ] KG manifest regenerated for 5.8; semantic search returns new symbols.
- [ ] Regression: existing 5.7-era domains still function on 5.8.

---

## 6. Risks & open questions

- **Preview churn:** experimental modules (Mesh Terrain, MetaHuman Crowd,
  Substrate NPR) will rename classes/modules before GA. Implement against the
  `5.8` release branch, not `ue5-main`, and expect a second pass at GA.
- **Optional-plugin coupling:** MetaHuman Crowd and some systems are optional
  plugins — prefer soft class resolution over hard `Build.cs` deps to keep the
  bridge loadable when a plugin is absent (degrade gracefully, report
  "unavailable" rather than fail to load).
- **5.9 horizon:** APIs deprecated in 5.8 are removed in 5.9 — clearing them now
  (§1b) is cheap insurance.
- **Engine-version pinning:** decide whether to support 5.7 **and** 5.8 from one
  branch (version-guarded `#if ENGINE_MINOR_VERSION`) or cut a hard 5.8 branch.
  Recommendation: hard-cut to 5.8 once GA lands; keep a `5.7-maintenance` tag.

---

## Appendix: sources

- Epic — "Unreal Engine 5.8 Preview" announcement (forums.unrealengine.com)
- Epic — Introduction to Mesh Terrain (dev.epicgames.com community knowledge base)
- 80.lv — "Unreal Engine 5.8 Preview Has Arrived" / Mesh Terrain coverage
- Digital Production — "Unreal Engine 5.8 Preview rolls in" (2026-05-14)
- gamegpu — "Main branch updated to Unreal Engine 5.8" (GitHub branch status)
- Epic — UE5 GitHub branch guidance (`ue5-main` vs release branches)

> Re-confirm all **[verify]** items against the 5.8 release-branch headers
> during Phase 0 before committing implementation work.
