#include "Capture/ForgeBuildCapture.h"
#include "ForgeBridgeVersion.h"
#include "IO/ForgeContextWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"

#include "Editor.h"                       // GEditor + FEditorDelegates (transitive)
#include "Engine/Blueprint.h"             // UBlueprint, EBlueprintStatus

#include "ILiveCodingModule.h"            // ILiveCodingModule, ELiveCodingResult
#include "Modules/ModuleManager.h"

// ============================================================
// FForgeLogInterceptor
// ============================================================

namespace
{
    // Anonymous namespace prevents unity-build symbol collisions
    static const TArray<FName>& GetBuildCategories()
    {
        static const TArray<FName> Categories = {
            TEXT("LogCompile"),
            TEXT("LogBlueprintUserMessages"),
            TEXT("LogHotReload"),
            TEXT("LogUObjectGlobals"),
            // LogLinker excluded - produces asset-version noise during load, not build errors
            // LogOutputDevice excluded - too broad
        };
        return Categories;
    }
}

FForgeLogInterceptor::FForgeLogInterceptor(const FString& InOutputDir)
    : OutputDir(InOutputDir)
{
}

FForgeLogInterceptor::~FForgeLogInterceptor()
{
    StopCapturing();
}

void FForgeLogInterceptor::StartCapturing()
{
    FScopeLock Lock(&DataLock);
    if (!bCapturing)
    {
        PendingErrors.Empty();
        PendingWarnings.Empty();
        GLog->AddOutputDevice(this);
        bCapturing = true;
    }
}

void FForgeLogInterceptor::StopCapturing()
{
    FScopeLock Lock(&DataLock);
    if (bCapturing)
    {
        GLog->RemoveOutputDevice(this);
        bCapturing = false;
    }
}

bool FForgeLogInterceptor::IsBuildCategory(const FName& Category)
{
    return GetBuildCategories().Contains(Category);
}

void FForgeLogInterceptor::ParseCompilerLine(const FString& Raw,
                                               FString& OutFile, int32& OutLine, FString& OutText)
{
    OutFile = TEXT("");
    OutLine = -1;
    OutText = Raw;

    // MSVC format: "C:\path\file.cpp(142): error C2065: message"
    int32 ParenOpen = INDEX_NONE;
    Raw.FindLastChar(TEXT('('), ParenOpen);
    if (ParenOpen == INDEX_NONE) return;

    int32 ParenClose = INDEX_NONE;
    Raw.FindChar(TEXT(')'), ParenClose);
    if (ParenClose == INDEX_NONE || ParenClose <= ParenOpen) return;

    OutFile = Raw.Left(ParenOpen);
    const FString LineStr = Raw.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutLine = FCString::Atoi(*LineStr);

    // Everything after "): " is the message
    const int32 MsgStart = ParenClose + 3; // skip ):
    if (MsgStart < Raw.Len())
    {
        OutText = Raw.Mid(MsgStart);
    }
}

void FForgeLogInterceptor::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                                       const FName& Category)
{
    if (!IsBuildCategory(Category)) return;
    if (Verbosity > ELogVerbosity::Warning) return;

    FString File;
    int32 Line;
    FString Text;
    ParseCompilerLine(FString(V), File, Line, Text);

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("severity"),
        Verbosity == ELogVerbosity::Error ? TEXT("Error") : TEXT("Warning"));
    Entry->SetStringField(TEXT("category"), Category.ToString());
    Entry->SetStringField(TEXT("file"), File);
    Entry->SetNumberField(TEXT("line"), static_cast<double>(Line));
    Entry->SetStringField(TEXT("message"), Text);

    FScopeLock Lock(&DataLock);
    if (Verbosity == ELogVerbosity::Error)
        PendingErrors.Add(Entry);
    else
        PendingWarnings.Add(Entry);
}

void FForgeLogInterceptor::FlushToDisk(const FString& Trigger)
{
    TArray<TSharedPtr<FJsonObject>> Errors;
    TArray<TSharedPtr<FJsonObject>> Warnings;
    {
        FScopeLock Lock(&DataLock);
        Errors = PendingErrors;
        Warnings = PendingWarnings;
        PendingErrors.Empty();
        PendingWarnings.Empty();
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("trigger"), Trigger);
    Root->SetStringField(TEXT("result"), Errors.Num() == 0 ? TEXT("Success") : TEXT("Failed"));
    Root->SetNumberField(TEXT("error_count"), static_cast<double>(Errors.Num()));
    Root->SetNumberField(TEXT("warning_count"), static_cast<double>(Warnings.Num()));

    TArray<TSharedPtr<FJsonValue>> ErrorValues;
    for (const auto& E : Errors)
        ErrorValues.Add(MakeShared<FJsonValueObject>(E));
    Root->SetArrayField(TEXT("errors"), ErrorValues);

    TArray<TSharedPtr<FJsonValue>> WarnValues;
    for (const auto& W : Warnings)
        WarnValues.Add(MakeShared<FJsonValueObject>(W));
    Root->SetArrayField(TEXT("warnings"), WarnValues);

    FForgeContextWriter::WriteJSON(OutputDir / TEXT("build"), TEXT("errors.json"),
                                    Root.ToSharedRef());
}

int32 FForgeLogInterceptor::GetErrorCount() const
{
    FScopeLock Lock(&DataLock);
    return PendingErrors.Num();
}

int32 FForgeLogInterceptor::GetWarningCount() const
{
    FScopeLock Lock(&DataLock);
    return PendingWarnings.Num();
}


// ============================================================
// UForgeBuildCapture
// ============================================================

void UForgeBuildCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;

    LogInterceptor = MakeUnique<FForgeLogInterceptor>(OutputDir);
    LogInterceptor->StartCapturing();

    // UE 5.8: legacy IHotReloadInterface::OnHotReload() fallback removed —
    // Live Coding (below) is the compile-event source.

    // -- Live Coding (UE5 default compile system - primary trigger) --
    if (FModuleManager::Get().IsModuleLoaded(TEXT("LiveCoding")))
    {
        ILiveCodingModule& LC =
            FModuleManager::GetModuleChecked<ILiveCodingModule>(TEXT("LiveCoding"));
        LiveCodingDelegateHandle = LC.GetOnPatchCompleteDelegate().AddUObject(
            this, &UForgeBuildCapture::OnLiveCodingPatchComplete);
    }

    // -- Blueprint compile (general - fires void() for any BP) --
    // VERIFIED: GEditor->OnBlueprintCompiled() is correct. FKismetEditorUtilities::OnBlueprintCompiled IS WRONG.
    if (GEditor)
    {
        BlueprintCompiledDelegateHandle = GEditor->OnBlueprintCompiled().AddUObject(
            this, &UForgeBuildCapture::OnAnyBlueprintCompiled);
    }
}

void UForgeBuildCapture::Deinitialize()
{
    if (LogInterceptor)
    {
        LogInterceptor->StopCapturing();
        LogInterceptor.Reset();
    }

    if (FModuleManager::Get().IsModuleLoaded(TEXT("LiveCoding")))
    {
        ILiveCodingModule& LC =
            FModuleManager::GetModuleChecked<ILiveCodingModule>(TEXT("LiveCoding"));
        LC.GetOnPatchCompleteDelegate().Remove(LiveCodingDelegateHandle);
    }
    LiveCodingDelegateHandle.Reset();

    if (GEditor)
    {
        GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledDelegateHandle);
    }
    BlueprintCompiledDelegateHandle.Reset();
}

void UForgeBuildCapture::OnLiveCodingPatchComplete()
{
    // FOnPatchCompleteDelegate is void() in UE 5.7 - no result param.
    // Use log error count to determine success.
    if (!LogInterceptor) return;

    const bool bSuccess = (LogInterceptor->GetErrorCount() == 0);
    LogInterceptor->FlushToDisk(TEXT("LiveCoding"));

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("trigger"), TEXT("LiveCoding"));
    Root->SetStringField(TEXT("result"), bSuccess ? TEXT("Success") : TEXT("Failed"));

    FForgeContextWriter::WriteJSON(OutputDir / TEXT("build"), TEXT("hot-reload.json"),
                                    Root.ToSharedRef());
    WriteIndexFile();
}

void UForgeBuildCapture::OnAnyBlueprintCompiled()
{
    // Intentionally minimal - the per-BP callback (OnBlueprintPostCompile) has more context.
    // This hook is primarily a signal that something compiled; errors come from the MessageLog.
}


void UForgeBuildCapture::WriteIndexFile()
{
    // Build the top-level index.json so the consumer knows what's available
    TSharedPtr<FJsonObject> BuildSection = MakeShared<FJsonObject>();
    BuildSection->SetStringField(TEXT("file"), TEXT("build/errors.json"));
    BuildSection->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());

    TSharedPtr<FJsonObject> Captures = MakeShared<FJsonObject>();
    Captures->SetObjectField(TEXT("build_errors"), BuildSection);
    Captures->SetField(TEXT("pcg_last_execute"), MakeShared<FJsonValueNull>());
    Captures->SetField(TEXT("screenshot_latest"), MakeShared<FJsonValueNull>());
    Captures->SetField(TEXT("runtime_log"), MakeShared<FJsonValueNull>());
    Captures->SetField(TEXT("heightmap"), MakeShared<FJsonValueNull>());

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("plugin_version"), FORGE_BRIDGE_VERSION);
    Root->SetObjectField(TEXT("captures_available"), Captures);

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
