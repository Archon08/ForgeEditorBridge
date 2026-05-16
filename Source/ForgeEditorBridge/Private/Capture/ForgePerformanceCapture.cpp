#include "Capture/ForgePerformanceCapture.h"
#include "ForgeAISubsystem.h"
#include "IO/ForgeContextWriter.h"

// --- Editor / PIE delegates ---
#include "Editor.h"

// --- Engine performance globals ---
//   GGameThreadTime  — game thread last-frame time (Engine module)
//   GRenderThreadTime — render thread last-frame time (RenderCore module)
//   FApp::GetDeltaTime() — last frame delta in seconds; derive frame_ms / fps_avg
//   GAverageFPS / GAverageMS were removed from Engine/Engine.h in UE 5.7.
#include "Engine/Engine.h"
#include "Misc/App.h"

// --- RHI globals ---
//   GNumDrawCallsRHI[MAX_NUM_GPUS]       — draw calls last frame
//   GNumPrimitivesDrawnRHI[MAX_NUM_GPUS] — triangles last frame
//   RHIGetGPUFrameCycles()               — GPU frame cycles (convert via FPlatformTime)
#include "RHI.h"

// --- Render thread timing ---
//   GRenderThreadTime
#include "RenderCore.h"

// --- Memory + platform ---
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

// --- Texture streaming budget ---
#include "ContentStreaming.h"

// --- World iteration (retained for scene context) ---
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// All performance globals are declared by the included headers:
//   Engine/Engine.h  -> GGameThreadTime
//   RenderCore.h     -> GRenderThreadTime
//   RHI.h            -> GNumDrawCallsRHI, GNumPrimitivesDrawnRHI, MAX_NUM_GPUS
// No manual externs needed — redeclaring conflicts with header-declared types.

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::Initialize(UForgeAISubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    OutputDir = InSubsystem ? InSubsystem->OutputDirectory : FString();
    bPIEStarted = false;

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("perf/history")));

    BeginPIEHandle = FEditorDelegates::BeginPIE.AddUObject(
        this, &UForgePerformanceCapture::OnBeginPIE);
    EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(
        this, &UForgePerformanceCapture::OnEndPIE);

    UE_LOG(LogTemp, Log, TEXT("ForgePerformance: Initialized (v1.15, ring buffer %d slots)"), RING_SIZE);
}

// ---------------------------------------------------------------------------
// Deinitialize
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::Deinitialize()
{
    FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
    BeginPIEHandle.Reset();
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
    EndPIEHandle.Reset();

    // GC delegates removed in UE 5.7 — nothing to unbind.
}

// ---------------------------------------------------------------------------
// PIE lifecycle
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::OnBeginPIE(bool /*bIsSimulating*/)
{
    PIEStartTime = FDateTime::UtcNow();
    bPIEStarted  = true;
}

void UForgePerformanceCapture::OnEndPIE(bool /*bIsSimulating*/)
{
    WriteSnapshot(TEXT("EndPIE"));
    bPIEStarted = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::CaptureSnapshot()
{
    WriteSnapshot(TEXT("Manual"));
}

bool UForgePerformanceCapture::ExportPerformanceSnapshot()
{
    WriteSnapshot(TEXT("Manual"));
    return true;
}

// ---------------------------------------------------------------------------
// WriteSnapshot — v1.15 schema with ring buffer
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::WriteSnapshot(const FString& Source)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("source"),    Source);
    Root->SetNumberField(TEXT("sequence"),  (double)RingCounter);

    // -------------------------------------------------------------------------
    // Frame timing
    //   frame_ms  — derived from FApp::GetDeltaTime() (reliable across editor/PIE)
    //   game_ms   — GGameThreadTime (Engine module, ms units in UE5)
    //   render_ms — GRenderThreadTime (RenderCore, ms units in UE5)
    //   gpu_ms    — RHIGetGPUFrameCycles() converted via FPlatformTime::ToMilliseconds
    //   fps_avg   — derived from frame_ms
    // -------------------------------------------------------------------------
    {
        const double FrameMS  = FApp::GetDeltaTime() * 1000.0;
        const double FpsAvg   = FrameMS > KINDA_SMALL_NUMBER ? 1000.0 / FrameMS : 0.0;
        const double GameMS   = (double)GGameThreadTime;
        const double RenderMS = (double)GRenderThreadTime;
        const double GPUMS    = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

        TSharedRef<FJsonObject> FrameObj = MakeShared<FJsonObject>();
        FrameObj->SetNumberField(TEXT("frame_ms"),  FrameMS);
        FrameObj->SetNumberField(TEXT("game_ms"),   GameMS);
        FrameObj->SetNumberField(TEXT("render_ms"), RenderMS);
        FrameObj->SetNumberField(TEXT("gpu_ms"),    GPUMS);
        FrameObj->SetNumberField(TEXT("fps_avg"),   FpsAvg);
        Root->SetObjectField(TEXT("frame"), FrameObj);
    }

    // -------------------------------------------------------------------------
    // Rendering stats
    //   draw_calls — GNumDrawCallsRHI[0] (GPU 0; 0 outside active render frame)
    //   triangles  — GNumPrimitivesDrawnRHI[0]
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> RenderingObj = MakeShared<FJsonObject>();
        RenderingObj->SetNumberField(TEXT("draw_calls"), (double)GNumDrawCallsRHI[0]);
        RenderingObj->SetNumberField(TEXT("triangles"),  (double)GNumPrimitivesDrawnRHI[0]);
        Root->SetObjectField(TEXT("rendering"), RenderingObj);
    }

    // -------------------------------------------------------------------------
    // Memory — texture streaming pool
    //   GetStreamingTexturesSizeInMemory() returns bytes used by streaming textures.
    //   Pool limit not easily available via public API; omitted (-1).
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> MemObj = MakeShared<FJsonObject>();
        MemObj->SetNumberField(TEXT("texture_pool_used_mb"), -1.0);  // UE 5.7: streaming texture size API removed
        Root->SetObjectField(TEXT("memory"), MemObj);
    }

    // -------------------------------------------------------------------------
    // Hitches — derived from frame_ms at snapshot time
    // -------------------------------------------------------------------------
    {
        const double FrameMS = FApp::GetDeltaTime() * 1000.0;
        TSharedRef<FJsonObject> HitchObj = MakeShared<FJsonObject>();
        HitchObj->SetBoolField  (TEXT("sub_60fps"),           FrameMS > 16.67);
        HitchObj->SetBoolField  (TEXT("sub_30fps"),           FrameMS > 33.33);
        HitchObj->SetNumberField(TEXT("worst_ms_last_second"), FrameMS);
        Root->SetObjectField(TEXT("hitches"), HitchObj);
    }

    // -------------------------------------------------------------------------
    // Audit rules: HIGH_DRAW_CALLS, TEXTURE_STREAMING_OVERBUDGET, GC_STALL
    // -------------------------------------------------------------------------
    {
        TArray<TSharedPtr<FJsonValue>> AuditArr;
        auto AddAlert = [&AuditArr](const TCHAR* Rule, const TCHAR* Severity, const FString& Detail)
        {
            TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
            A->SetStringField(TEXT("rule"),     Rule);
            A->SetStringField(TEXT("severity"), Severity);
            A->SetStringField(TEXT("detail"),   Detail);
            AuditArr.Add(MakeShared<FJsonValueObject>(A));
        };

        const int32 DrawCalls = (int32)GNumDrawCallsRHI[0];
        if (DrawCalls > 3000)
        {
            AddAlert(TEXT("HIGH_DRAW_CALLS"), TEXT("warning"),
                FString::Printf(
                    TEXT("%d draw calls this frame (threshold: 3000). Consider batching, instancing, or LOD reduction."),
                    DrawCalls));
        }

        {
            const int64 OverBudget =
                IStreamingManager::Get().GetTextureStreamingManager().GetMemoryOverBudget();
            if (OverBudget > 0)
            {
                AddAlert(TEXT("TEXTURE_STREAMING_OVERBUDGET"), TEXT("warning"),
                    FString::Printf(
                        TEXT("Texture streaming is %.1f MB over budget. Increase r.Streaming.PoolSize or reduce texture resolution/count."),
                        (double)OverBudget / (1024.0 * 1024.0)));
            }
        }

        if (LastGCMs > 5.0f)
        {
            AddAlert(TEXT("GC_STALL"), TEXT("warning"),
                FString::Printf(
                    TEXT("Last GC took %.1f ms (threshold: 5 ms). Consider increasing GC interval or reducing live UObject count."),
                    LastGCMs));
        }

        Root->SetArrayField (TEXT("audit"),             AuditArr);
        Root->SetNumberField(TEXT("audit_issue_count"), AuditArr.Num());
    }

    // -------------------------------------------------------------------------
    // Ring buffer write
    //   Slot name: perf-000 … perf-009, wrapping via modulo
    //   latest.json always reflects the newest snapshot
    // -------------------------------------------------------------------------
    const FString SlotName = FString::Printf(TEXT("perf-%03d"), RingCounter % RING_SIZE);
    FForgeContextWriter::WriteJSON(OutputDir / TEXT("perf/history"), SlotName, Root);
    FForgeContextWriter::WriteJSON(OutputDir / TEXT("perf"),         TEXT("latest"), Root);

    RingCounter++;

    UE_LOG(LogTemp, Log,
        TEXT("ForgePerformance: Snapshot [%s] → perf/history/%s.json (seq %d)"),
        *Source, *SlotName, RingCounter - 1);

    UpdateIndexFile();
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgePerformanceCapture::UpdateIndexFile()
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    // Retrieve or create captures_available sub-object
    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
    {
        if (Found->IsValid() && (*Found)->Type == EJson::Object)
            Captures = (*Found)->AsObject();
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    // Build the perf section with the new v1.15 fields
    TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
    Section->SetBoolField  (TEXT("available"),     true);
    Section->SetStringField(TEXT("last_snapshot"), FForgeContextWriter::NowISO8601());
    Section->SetNumberField(TEXT("frame_ms"),      FApp::GetDeltaTime() * 1000.0);
    Section->SetStringField(TEXT("file"),          TEXT("perf/latest.json"));
    Captures->SetObjectField(TEXT("perf"), Section);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
