#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeNiagaraCapture.generated.h"

/**
 * v1.1 — Niagara FX Capture
 *
 * Exports UNiagaraSystem asset configurations (emitters, user parameters) and
 * snapshots the runtime component pool state during PIE.
 *
 * Output directory: {ProjectRoot}/Forge/ue-context/niagara/
 *
 * Per-system files:   niagara/{SystemName}.json
 * Pool snapshot:      niagara/pool_state.json
 *
 * Trigger from Python:
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.niagara_capture.export_all_niagara_systems()
 *   sub.niagara_capture.capture_niagara_pool_state()
 *
 * Exported per-system fields:
 *   asset_path, emitters[]{id, name, enabled, max_particles_estimate},
 *   user_parameters[]{name, type},
 *   scalability[]{cull_by_distance, max_distance, max_system_instances},
 *   audit{total_issues, issues[]{issue_type, severity, detail}}
 *
 * Audit rules:
 *   HIGH_EMITTER_COUNT    — system has >8 emitters (expensive per-frame tick)
 *   UNBOUND_FIXED_BOUNDS  — bFixedBounds is false; bounds computed every frame
 *   DISABLED_EMITTER      — an emitter exists in the system but is disabled (dead weight)
 *   ZERO_MAX_PARTICLES    — enabled emitter estimates 0 particles (likely uncompiled)
 *   NO_SCALABILITY        — system has no per-platform scalability overrides
 *
 * Pool state fields (editor/development builds only):
 *   pool_enabled, total_active, peak_total,
 *   systems_in_use[]{system_path, active_count, risk_flag}
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeNiagaraCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Export a single UNiagaraSystem by asset path (e.g. "/Game/FX/NS_Fire").
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportNiagaraSystem(const FString& AssetPath);

    /**
     * Export all UNiagaraSystem assets found under /Game/ via the Asset Registry.
     * Returns the number of systems successfully exported.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAllNiagaraSystems();

    /**
     * Snapshot the Niagara component pool for the active world.
     * Only meaningful during PIE — pool entries are empty in editor-only mode.
     * Writes niagara/pool_state.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool CaptureNiagaraPoolState();

private:
    FString OutputDir;

    // READ-MERGE-WRITE index.json to add/update the "niagara" section
    void UpdateIndexFile(int32 SystemCount, bool bPoolCaptured);
};
