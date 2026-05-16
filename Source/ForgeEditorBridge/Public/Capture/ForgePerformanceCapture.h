#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgePerformanceCapture.generated.h"

class UForgeAISubsystem;

/**
 * v1.15 — Performance Snapshot Capture
 *
 * Captures frame timing, draw calls, GPU time, and texture memory on demand
 * or automatically at the end of each PIE session.
 *
 * Ring buffer: writes perf/history/perf-NNN.json (10 slots) and perf/latest.json.
 * Sequence number increments every write for drift detection.
 *
 * Trigger from command channel: { "command": "capture_perf_snapshot" }
 * Manual trigger from Python:
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.performance_capture.export_performance_snapshot()
 *
 * Output schema (perf/latest.json):
 *   timestamp, source, sequence,
 *   frame { frame_ms, game_ms, render_ms, gpu_ms, fps_avg },
 *   rendering { draw_calls, triangles },
 *   memory { texture_pool_used_mb },
 *   hitches { sub_60fps, sub_30fps, worst_ms_last_second }
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgePerformanceCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(UForgeAISubsystem* InSubsystem);
    void Deinitialize();

    /** On-demand capture — called by command channel capture_perf_snapshot. */
    void CaptureSnapshot();

    /**
     * Legacy BlueprintCallable entry point (kept for Python/Blueprint compatibility).
     * Equivalent to CaptureSnapshot(). Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportPerformanceSnapshot();

private:
    void WriteSnapshot(const FString& Source);
    void UpdateIndexFile();

    void OnBeginPIE(bool bIsSimulating);
    void OnEndPIE(bool bIsSimulating);

    FString         OutputDir;
    FDelegateHandle BeginPIEHandle;
    FDelegateHandle EndPIEHandle;

    // UTC time when the most recent PIE session started
    FDateTime    PIEStartTime;
    bool         bPIEStarted = false;

    // GC stall tracking disabled in UE 5.7 — FCoreDelegates GC delegate accessors removed.
    // GC_STALL rule is inert; LastGCMs stays 0.
    float           LastGCMs = 0.f;

    // Ring buffer state
    int32                  RingCounter = 0;
    static constexpr int32 RING_SIZE   = 10;

    UPROPERTY()
    TObjectPtr<UForgeAISubsystem> Subsystem;
};
