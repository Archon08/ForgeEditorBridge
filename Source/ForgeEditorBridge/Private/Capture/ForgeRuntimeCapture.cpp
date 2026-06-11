#include "Capture/ForgeRuntimeCapture.h"
#include "ForgeBridgeVersion.h"
#include "IO/ForgeContextWriter.h"
#include "Debug/ForgeDebugInterface.h"

#include "Editor.h"               // FEditorDelegates, GEditor (transitive)
#include "Engine/World.h"
#include "EngineUtils.h"          // TActorIterator
#include "GameFramework/Actor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Default categories captured during PIE. Can be adjusted via Add/RemoveFilterCategory.
static const TArray<FName> DefaultFilterCategories = {
    // Generic UE log channels. Add your project log categories at runtime
    // via FForgeRuntimeLogInterceptor::AddFilterCategory.
    FName(TEXT("LogPCG")),
    FName(TEXT("LogTemp")),
    FName(TEXT("LogBlueprintUserMessages")),
};

// =============================================================================
// FForgeRuntimeLogInterceptor
// =============================================================================

FForgeRuntimeLogInterceptor::FForgeRuntimeLogInterceptor(const FString& InOutputDir)
    : OutputDir(InOutputDir)
{
    FilterCategories = DefaultFilterCategories;
}

FForgeRuntimeLogInterceptor::~FForgeRuntimeLogInterceptor()
{
    StopCapturing();
}

void FForgeRuntimeLogInterceptor::StartCapturing()
{
    FScopeLock Lock(&FileMutex);
    if (bCapturing) return;

    // Ensure the runtime/ subdirectory exists
    const FString RuntimeDir = OutputDir / TEXT("runtime");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*RuntimeDir);

    // Overwrite (clear) log file and write session-start marker
    const FString LogFilePath = RuntimeDir / TEXT("log-filtered.txt");
    FFileHelper::SaveStringToFile(
        FString::Printf(TEXT("--- PIE Session started %s ---") LINE_TERMINATOR,
                        *FForgeContextWriter::NowISO8601()),
        *LogFilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM
        // No FILEWRITE_Append - this creates/overwrites the file
    );

    LineCount = 1; // count the header line
    GLog->AddOutputDevice(this);
    bCapturing = true;

    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: log capture started -> %s"), *LogFilePath);
}

void FForgeRuntimeLogInterceptor::StopCapturing()
{
    FScopeLock Lock(&FileMutex);
    if (!bCapturing) return;

    GLog->RemoveOutputDevice(this);
    bCapturing = false;

    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: log capture stopped (%d lines written)"), LineCount);
}

FString FForgeRuntimeLogInterceptor::VerbosityToString(ELogVerbosity::Type Verbosity)
{
    switch (Verbosity)
    {
        case ELogVerbosity::Error:   return TEXT("Error");
        case ELogVerbosity::Warning: return TEXT("Warning");
        default:                     return TEXT("Log");
    }
}

void FForgeRuntimeLogInterceptor::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                                              const FName& Category)
{
    // Quick pre-lock checks (these reads are safe without the lock - bCapturing
    // is only written from the game thread via Start/StopCapturing, and we only
    // need approximate checks here before acquiring the mutex)
    if (!bCapturing) return;
    if (!FilterCategories.Contains(Category)) return;
    if (Verbosity > ELogVerbosity::Display) return; // Skip Verbose/VeryVerbose

    FScopeLock Lock(&FileMutex);

    // Re-check under lock in case StopCapturing raced with us
    if (!bCapturing) return;

    if (LineCount >= MaxLinesPerSession)
    {
        if (LineCount == MaxLinesPerSession)
        {
            // Write exactly one cap warning then stop appending
            FForgeContextWriter::AppendLine(OutputDir / TEXT("runtime"), TEXT("log-filtered.txt"),
                FString::Printf(TEXT("[%s] Warning: ForgeRuntime: line cap (%d) reached, further lines suppressed"),
                                *FForgeContextWriter::NowISO8601(), MaxLinesPerSession));
            ++LineCount; // prevent repeated warnings
        }
        return;
    }

    const FString Line = FString::Printf(TEXT("[%s] %s: %s: %s"),
        *FForgeContextWriter::NowISO8601(),
        *VerbosityToString(Verbosity),
        *Category.ToString(),
        V);

    FForgeContextWriter::AppendLine(OutputDir / TEXT("runtime"), TEXT("log-filtered.txt"), Line);
    ++LineCount;
}

// =============================================================================
// UForgeRuntimeCapture
// =============================================================================

void UForgeRuntimeCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    LogInterceptor = MakeUnique<FForgeRuntimeLogInterceptor>(OutputDir);

    // FEditorDelegates::BeginPIE / EndPIE
    // Signature: DECLARE_MULTICAST_DELEGATE_OneParam(FOnBeginPIE, bool)
    // Handler: void(bool bIsSimulating) - non-dynamic multicast, use AddUObject
    BeginPIEHandle = FEditorDelegates::BeginPIE.AddUObject(
        this, &UForgeRuntimeCapture::OnBeginPIE);
    EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(
        this, &UForgeRuntimeCapture::OnEndPIE);

    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: Initialize complete"));
}

void UForgeRuntimeCapture::Deinitialize()
{
    if (LogInterceptor)
    {
        LogInterceptor->StopCapturing();
        LogInterceptor.Reset();
    }

    FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
    BeginPIEHandle.Reset();
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
    EndPIEHandle.Reset();
}

void UForgeRuntimeCapture::OnBeginPIE(const bool bIsSimulating)
{
    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: OnBeginPIE (simulating=%d)"), bIsSimulating);
    if (LogInterceptor) { LogInterceptor->StartCapturing(); }
}

void UForgeRuntimeCapture::OnEndPIE(const bool bIsSimulating)
{
    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: OnEndPIE"));
    if (LogInterceptor)
    {
        // Append session-end marker before stopping (still inside lock via AppendLine)
        FForgeContextWriter::AppendLine(OutputDir / TEXT("runtime"), TEXT("log-filtered.txt"),
            FString::Printf(TEXT("--- PIE Session ended %s ---"),
                            *FForgeContextWriter::NowISO8601()));
        LogInterceptor->StopCapturing();
    }
    UpdateIndexFile();
}

void UForgeRuntimeCapture::AddFilterCategory(FName Category)
{
    if (LogInterceptor && !LogInterceptor->FilterCategories.Contains(Category))
        LogInterceptor->FilterCategories.Add(Category);
}

void UForgeRuntimeCapture::RemoveFilterCategory(FName Category)
{
    if (LogInterceptor)
        LogInterceptor->FilterCategories.Remove(Category);
}

void UForgeRuntimeCapture::CaptureVariableSnapshot()
{
    UWorld* World = GetPIEWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeRuntime: CaptureVariableSnapshot called outside PIE"));
        return;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("session"), TEXT("PIE"));
    Root->SetStringField(TEXT("map"), World->GetMapName());

    TArray<TSharedPtr<FJsonValue>> Snapshots;
    int32 SnappedCount = 0;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !Actor->ActorHasTag(FName(TEXT("ForgeDebug")))) continue;

        TSharedPtr<FJsonObject> Snapshot = BuildActorSnapshot(Actor);
        if (Snapshot.IsValid())
        {
            Snapshots.Add(MakeShared<FJsonValueObject>(Snapshot));
            ++SnappedCount;
        }
    }

    Root->SetArrayField(TEXT("snapshots"), Snapshots);

    FForgeContextWriter::WriteJSON(OutputDir / TEXT("runtime"), TEXT("variables.json"),
                                    Root.ToSharedRef());

    UE_LOG(LogTemp, Log, TEXT("ForgeRuntime: variable snapshot written (%d actors)"), SnappedCount);
}

TSharedPtr<FJsonObject> UForgeRuntimeCapture::BuildActorSnapshot(AActor* Actor)
{
    TSharedPtr<FJsonObject> Snap = MakeShared<FJsonObject>();
    Snap->SetStringField(TEXT("actor"), Actor->GetName());
    Snap->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

    // Position
    const FVector Pos = Actor->GetActorLocation();
    TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
    PosObj->SetNumberField(TEXT("x"), Pos.X);
    PosObj->SetNumberField(TEXT("y"), Pos.Y);
    PosObj->SetNumberField(TEXT("z"), Pos.Z);
    Snap->SetObjectField(TEXT("position"), PosObj);

    // Optional: structured variable data via IForgeDebugInterface
    // Actors that implement this interface in Blueprint control what gets exported
    if (Actor->Implements<UForgeDebugInterface>())
    {
        TMap<FString, FString> DebugData;
        IForgeDebugInterface::Execute_GetDebugData(Actor, DebugData);

        if (DebugData.Num() > 0)
        {
            TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
            for (const auto& KV : DebugData)
            {
                Vars->SetStringField(KV.Key, KV.Value);
            }
            Snap->SetObjectField(TEXT("variables"), Vars);
        }
    }

    return Snap;
}

UWorld* UForgeRuntimeCapture::GetPIEWorld() const
{
    if (!GEngine) return nullptr;

    // Safe iteration - avoids GEditor->GetEditorWorldContext() check(0) crash
    // Verified pattern from v0.2 PCGCapture. In multiplayer PIE, returns first PIE world.
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType == EWorldType::PIE && Context.World())
            return Context.World();
    }
    return nullptr;
}

void UForgeRuntimeCapture::UpdateIndexFile()
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    // READ-MERGE-WRITE: preserve all existing sections (PCG, screenshot, build)
    TSharedPtr<FJsonObject> Root;
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingContent);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
        Captures = *ExistingCaptures;
    else
        Captures = MakeShared<FJsonObject>();

    const FString Timestamp = FForgeContextWriter::NowISO8601();

    TSharedPtr<FJsonObject> RuntimeSection = MakeShared<FJsonObject>();
    RuntimeSection->SetStringField(TEXT("log_file"),  TEXT("runtime/log-filtered.txt"));
    RuntimeSection->SetStringField(TEXT("vars_file"),  TEXT("runtime/variables.json"));
    RuntimeSection->SetStringField(TEXT("last_session"), Timestamp);
    Captures->SetObjectField(TEXT("runtime"), RuntimeSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), FORGE_BRIDGE_VERSION);

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
