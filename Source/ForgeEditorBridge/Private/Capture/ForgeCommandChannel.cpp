#include "Capture/ForgeCommandChannel.h"
#include "ForgeAISubsystem.h"
#include "IO/ForgeContextWriter.h"

// ---- Capture dependencies (for delegate-through commands) ---------------
#include "Capture/ForgeScreenshotCapture.h"
#include "Capture/ForgeAssetRegistryCapture.h"
#include "Capture/ForgeDataTableCapture.h"
#include "Capture/ForgePerformanceCapture.h"  // v1.15: capture_perf_snapshot

// ---- PCG ----------------------------------------------------------------
#include "PCGComponent.h"
#include "PCGGraph.h"        // UPCGGraph — required for GetGraph() return type to be complete

// ---- Material -----------------------------------------------------------
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---- Weather actor types (apply_weather) --------------------------------
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/PostProcessVolume.h"

// ---- Actor / world utils ------------------------------------------------
#include "Editor.h"                          // GEditor, FEditorDelegates, UEditorEngine (includes EditorEngine.h)
#include "FileHelpers.h"                     // FEditorFileUtils::SaveDirtyPackages
#include "Engine/World.h"
#include "Engine/Engine.h"                   // GEngine->GetWorldContexts() — UE5.7 safe world access
#include "EngineUtils.h"                     // TActorIterator
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"              // FProperty, FindFProperty

// ---- Debug draw ---------------------------------------------------------
#include "DrawDebugHelpers.h"

// ---- Filesystem / JSON --------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

// ============================================================
// Debug-draw helpers (file-local)
// ============================================================

namespace
{
	// Parse a {x, y, z} JSON sub-object into an FVector.
	// Falls back to Default when the field is absent or malformed.
	FVector ReadVec3(const TSharedPtr<FJsonObject>& Obj,
	                 const TCHAR* Field,
	                 FVector Default = FVector::ZeroVector)
	{
		const TSharedPtr<FJsonObject>* Sub;
		if (Obj->TryGetObjectField(Field, Sub) && Sub->IsValid())
		{
			double X = 0, Y = 0, Z = 0;
			(*Sub)->TryGetNumberField(TEXT("x"), X);
			(*Sub)->TryGetNumberField(TEXT("y"), Y);
			(*Sub)->TryGetNumberField(TEXT("z"), Z);
			return FVector((float)X, (float)Y, (float)Z);
		}
		return Default;
	}

	// Parse a colour from a hex string ("#RRGGBB", "#RRGGBBAA") or a named
	// colour token (case-insensitive: Red, Green, Blue, White, Black, Yellow,
	// Cyan, Magenta, Orange, Purple).  Returns FColor::White on unknown input.
	FColor ParseColor(const FString& ColorStr)
	{
		if (ColorStr.IsEmpty()) return FColor::White;

		// Hex path — FColor::FromHex handles "#" prefix, 6- and 8-digit forms.
		const TCHAR First = ColorStr[0];
		if (First == TCHAR('#') || FChar::IsHexDigit(First))
		{
			return FColor::FromHex(ColorStr);
		}

		// Named colour map
		const FString Lower = ColorStr.ToLower();
		if (Lower == TEXT("red"))     return FColor::Red;
		if (Lower == TEXT("green"))   return FColor::Green;
		if (Lower == TEXT("blue"))    return FColor::Blue;
		if (Lower == TEXT("white"))   return FColor::White;
		if (Lower == TEXT("black"))   return FColor::Black;
		if (Lower == TEXT("yellow"))  return FColor::Yellow;
		if (Lower == TEXT("cyan"))    return FColor::Cyan;
		if (Lower == TEXT("magenta")) return FColor::Magenta;
		if (Lower == TEXT("orange"))  return FColor(255, 165,   0, 255);
		if (Lower == TEXT("purple"))  return FColor(128,   0, 128, 255);

		return FColor::White;
	}
}

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgeCommandChannel::Initialize(const FString& InOutputDir, UForgeAISubsystem* InSubsystem)
{
    OutputDir = InOutputDir;
    Subsystem = InSubsystem;

    // Create inbox, history, and failed directories up front
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("commands/inbox")));
    PF.CreateDirectoryTree(*(OutputDir / TEXT("commands/history")));
    PF.CreateDirectoryTree(*(OutputDir / TEXT("commands/failed")));

    // Suspend polling during PIE to avoid editor asset churn
    BeginPIEHandle = FEditorDelegates::BeginPIE.AddUObject(this, &UForgeCommandChannel::OnBeginPIE);
    EndPIEHandle   = FEditorDelegates::EndPIE.AddUObject(this, &UForgeCommandChannel::OnEndPIE);

    // 1-second tick — returns true to keep ticking
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UForgeCommandChannel::OnTick), 1.0f);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeCommandChannel: Initialized — polling commands/inbox/ every 1s (max %d/tick)"),
        MaxPerTick);
}

void UForgeCommandChannel::Deinitialize()
{
    if (TickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

    FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
}

// ============================================================
// PIE delegates
// ============================================================

void UForgeCommandChannel::OnBeginPIE(bool bIsSimulating)
{
    bPIEActive = true;
}

void UForgeCommandChannel::OnEndPIE(bool bIsSimulating)
{
    bPIEActive = false;
}

// ============================================================
// Tick
// ============================================================

bool UForgeCommandChannel::OnTick(float /*DeltaTime*/)
{
    if (!bPIEActive)
    {
        PollInbox();
    }
    return true; // Keep ticking
}

// ============================================================
// PollInbox
// ============================================================

int32 UForgeCommandChannel::PollInbox()
{
    const FString InboxDir = OutputDir / TEXT("commands/inbox");

    // Enumerate all .json files in the inbox
    TArray<FString> CommandFiles;
    IFileManager::Get().FindFiles(CommandFiles, *(InboxDir / TEXT("*.json")), true, false);

    if (CommandFiles.IsEmpty())
    {
        return 0;
    }

    int32 Processed = 0;
    for (const FString& Filename : CommandFiles)
    {
        if (Processed >= MaxPerTick)
        {
            break;
        }

        const FString FullPath = InboxDir / Filename;
        if (ProcessCommandFile(FullPath))
        {
            Processed++;
        }
    }

    return Processed;
}

// ============================================================
// ProcessCommandFile — core dispatch loop
// ============================================================

bool UForgeCommandChannel::ProcessCommandFile(const FString& FilePath)
{
    // Helper: quarantine a bad file so the ticker doesn't hammer it every second
    auto QuarantineFile = [&](const FString& Reason)
    {
        const FString FailedDir  = OutputDir / TEXT("commands/failed");
        IPlatformFile& PF        = FPlatformFileManager::Get().GetPlatformFile();
        PF.CreateDirectory(*FailedDir);

        const FString Basename   = FPaths::GetBaseFilename(FilePath);
        const FString DestPath   = FailedDir / (Basename + TEXT(".json"));
        IFileManager::Get().Move(*DestPath, *FilePath);

        UE_LOG(LogTemp, Warning,
            TEXT("ForgeCommandChannel: Quarantined '%s' — %s"), *Basename, *Reason);
    };

    // 1. Read
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *FilePath))
    {
        QuarantineFile(TEXT("file read failed"));
        return false;
    }

    // 2. Parse
    TSharedPtr<FJsonObject> Cmd;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
    if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
    {
        QuarantineFile(TEXT("invalid JSON"));
        return false;
    }

    FString CommandType;
    if (!Cmd->TryGetStringField(TEXT("command"), CommandType))
    {
        QuarantineFile(TEXT("missing 'command' field"));
        return false;
    }

    // 3. Archive the command BEFORE execution — prevents reprocessing on crash
    const FString Timestamp     = FForgeContextWriter::NowISO8601();
    const FString SafeTimestamp = Timestamp
        .Replace(TEXT(":"), TEXT("-"))
        .Replace(TEXT("T"), TEXT("_"))
        .Replace(TEXT("Z"), TEXT(""));

    const FString BaseName    = FPaths::GetBaseFilename(FilePath);
    const FString HistoryDir  = OutputDir / TEXT("commands/history");
    const FString ArchivePath = HistoryDir / (BaseName + TEXT("_") + SafeTimestamp + TEXT(".json"));

    IFileManager::Get().Move(*ArchivePath, *FilePath);

    // 4. Dispatch
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("timestamp"),    Timestamp);
    Result->SetStringField(TEXT("command"),      CommandType);
    Result->SetStringField(TEXT("source_file"),  BaseName);

    bool bSuccess = false;

    if      (CommandType == TEXT("trigger_pcg_regenerate"))   bSuccess = Cmd_TriggerPCGRegenerate  (Cmd, Result);
    else if (CommandType == TEXT("spawn_test_actor"))          bSuccess = Cmd_SpawnTestActor        (Cmd, Result);
    else if (CommandType == TEXT("set_actor_property"))        bSuccess = Cmd_SetActorProperty      (Cmd, Result);
    else if (CommandType == TEXT("apply_weather"))             bSuccess = Cmd_ApplyWeather          (Cmd, Result);
    else if (CommandType == TEXT("capture_screenshot"))        bSuccess = Cmd_CaptureScreenshot     (Cmd, Result);
    else if (CommandType == TEXT("export_asset_registry"))     bSuccess = Cmd_ExportAssetRegistry   (Cmd, Result);
    else if (CommandType == TEXT("create_material_instance"))  bSuccess = Cmd_CreateMaterialInstance(Cmd, Result);
    else if (CommandType == TEXT("import_datatable_rows"))     bSuccess = Cmd_ImportDataTableRows   (Cmd, Result);
    else if (CommandType == TEXT("set_actor_transform"))       bSuccess = Cmd_SetActorTransform     (Cmd, Result);
    else if (CommandType == TEXT("delete_actor"))              bSuccess = Cmd_DeleteActor           (Cmd, Result);
    else if (CommandType == TEXT("save_packages"))             bSuccess = Cmd_SavePackages          (Cmd, Result);
    else if (CommandType == TEXT("draw_debug_line"))           bSuccess = Cmd_DrawDebugLine         (Cmd, Result);
    else if (CommandType == TEXT("draw_debug_sphere"))         bSuccess = Cmd_DrawDebugSphere       (Cmd, Result);
    else if (CommandType == TEXT("draw_debug_box"))            bSuccess = Cmd_DrawDebugBox          (Cmd, Result);
    else if (CommandType == TEXT("draw_debug_text"))           bSuccess = Cmd_DrawDebugText         (Cmd, Result);
    else if (CommandType == TEXT("clear_debug_shapes"))        bSuccess = Cmd_ClearDebugShapes      (Cmd, Result);
    else if (CommandType == TEXT("capture_perf_snapshot"))     bSuccess = Cmd_CapturePerfSnapshot   (Cmd, Result);
    else
    {
        Result->SetStringField(TEXT("status"), TEXT("error"));
        Result->SetStringField(TEXT("error"),  FString::Printf(TEXT("unknown command type '%s'"), *CommandType));
        UE_LOG(LogTemp, Warning, TEXT("ForgeCommandChannel: Unknown command type '%s'"), *CommandType);
    }

    // 5. Write result JSON alongside archived command
    const FString ResultPath = HistoryDir / (BaseName + TEXT("_") + SafeTimestamp + TEXT("_result.json"));
    FForgeContextWriter::WriteJSON(HistoryDir, FPaths::GetCleanFilename(ResultPath), Result);

    UE_LOG(LogTemp, Log, TEXT("ForgeCommandChannel: %s '%s' — %s"),
        bSuccess ? TEXT("OK") : TEXT("FAIL"),
        *CommandType,
        *BaseName);

    if (bSuccess)
    {
        UpdateIndexFile();
    }

    return true; // File was consumed regardless of success/failure
}

// ============================================================
// Command: trigger_pcg_regenerate
// ============================================================
// Required fields: "actor_label" (string)

bool UForgeCommandChannel::Cmd_TriggerPCGRegenerate(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ActorLabel;
    if (!Cmd->TryGetStringField(TEXT("actor_label"), ActorLabel))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'actor_label'"));
        return false;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    AActor* Actor = FindActorByLabel(World, ActorLabel);
    if (!Actor)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("actor '%s' not found in current level"), *ActorLabel));
        return false;
    }

    UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
    if (!PCGComp)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("actor '%s' has no UPCGComponent"), *ActorLabel));
        return false;
    }

    // UE5.5+: pass bForce=true so Generate() doesn't early-out when the
    // component considers itself already up-to-date.
    PCGComp->Generate(true);

    FString GraphName = TEXT("(none)");
    if (UPCGGraph* Graph = PCGComp->GetGraph())
    {
        GraphName = Graph->GetName();
    }

    OutResult->SetStringField(TEXT("status"),     TEXT("ok"));
    OutResult->SetStringField(TEXT("actor_label"), ActorLabel);
    OutResult->SetStringField(TEXT("pcg_graph"),   GraphName);
    return true;
}

// ============================================================
// Command: spawn_test_actor
// ============================================================
// Required: "class_path" (string, e.g. "/Script/Engine.PointLight")
// Optional: "location" {x,y,z}, "label" (string)

bool UForgeCommandChannel::Cmd_SpawnTestActor(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ClassPath;
    if (!Cmd->TryGetStringField(TEXT("class_path"), ClassPath))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'class_path'"));
        return false;
    }

    UClass* SpawnClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
    if (!SpawnClass)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("could not load class '%s'"), *ClassPath));
        return false;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    // Parse location  — optional, defaults to world origin
    FVector Location = FVector::ZeroVector;
    const TSharedPtr<FJsonObject>* LocObj;
    if (Cmd->TryGetObjectField(TEXT("location"), LocObj))
    {
        double X = 0, Y = 0, Z = 0;
        (*LocObj)->TryGetNumberField(TEXT("x"), X);
        (*LocObj)->TryGetNumberField(TEXT("y"), Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), Z);
        Location = FVector((float)X, (float)Y, (float)Z);
    }

    // Parse rotation — optional {pitch, yaw, roll}, defaults to zero
    FRotator Rotation = FRotator::ZeroRotator;
    const TSharedPtr<FJsonObject>* RotObj;
    if (Cmd->TryGetObjectField(TEXT("rotation"), RotObj))
    {
        double Pitch = 0, Yaw = 0, Roll = 0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
        Rotation = FRotator((float)Pitch, (float)Yaw, (float)Roll);
    }

    // Spawn via GEditor::AddActor for proper editor-level transaction support
    AActor* Spawned = GEditor->AddActor(
        World->GetCurrentLevel(),
        SpawnClass,
        FTransform(Rotation, Location, FVector::OneVector));

    if (!Spawned)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("GEditor::AddActor failed for class '%s'"), *ClassPath));
        return false;
    }

    FString Label;
    if (Cmd->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
    {
        Spawned->SetActorLabel(Label);
    }

    OutResult->SetStringField(TEXT("status"),       TEXT("ok"));
    OutResult->SetStringField(TEXT("class"),        SpawnClass->GetName());
    OutResult->SetStringField(TEXT("actor_label"),  Spawned->GetActorLabel());
    OutResult->SetStringField(TEXT("location"),
        FString::Printf(TEXT("(%.1f, %.1f, %.1f)"),
            Spawned->GetActorLocation().X,
            Spawned->GetActorLocation().Y,
            Spawned->GetActorLocation().Z));
    return true;
}

// ============================================================
// Command: set_actor_property
// ============================================================
// Required: "actor_label", "property", "value" (all strings)

bool UForgeCommandChannel::Cmd_SetActorProperty(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ActorLabel, Property, Value;
    if (!Cmd->TryGetStringField(TEXT("actor_label"), ActorLabel) ||
        !Cmd->TryGetStringField(TEXT("property"),    Property)   ||
        !Cmd->TryGetStringField(TEXT("value"),       Value))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            TEXT("missing required fields: 'actor_label', 'property', 'value'"));
        return false;
    }

    // Optional: target a specific component by name to avoid ambiguity on
    // actors with multiple components sharing the same property name.
    FString ComponentName;
    Cmd->TryGetStringField(TEXT("component_name"), ComponentName);

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    const FString StatusStr = ApplySinglePropertyChange(World, ActorLabel, Property, Value, ComponentName);
    const bool bOk = StatusStr == TEXT("ok");

    OutResult->SetStringField(TEXT("status"),      bOk ? TEXT("ok") : TEXT("error"));
    OutResult->SetStringField(TEXT("actor_label"), ActorLabel);
    OutResult->SetStringField(TEXT("property"),    Property);
    OutResult->SetStringField(TEXT("value"),       Value);

    if (!ComponentName.IsEmpty())
    {
        OutResult->SetStringField(TEXT("component_name"), ComponentName);
    }
    if (!bOk)
    {
        OutResult->SetStringField(TEXT("error"), StatusStr);
    }

    return bOk;
}

// ============================================================
// Command: apply_weather
// ============================================================
// Required: "changes" — array of { "actor_label", "property", "value" }

bool UForgeCommandChannel::Cmd_ApplyWeather(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    const TArray<TSharedPtr<FJsonValue>>* ChangesArray;
    if (!Cmd->TryGetArrayField(TEXT("changes"), ChangesArray) || ChangesArray->IsEmpty())
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            TEXT("missing or empty 'changes' array — expected [{actor_label, property, value}]"));
        return false;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 SucceededCount = 0;
    int32 FailedCount    = 0;

    for (const TSharedPtr<FJsonValue>& ChangeVal : *ChangesArray)
    {
        const TSharedPtr<FJsonObject> Change = ChangeVal->AsObject();
        if (!Change.IsValid()) { FailedCount++; continue; }

        FString ActorLabel, Property, Value, ComponentName;
        Change->TryGetStringField(TEXT("actor_label"),   ActorLabel);
        Change->TryGetStringField(TEXT("property"),      Property);
        Change->TryGetStringField(TEXT("value"),         Value);
        Change->TryGetStringField(TEXT("component_name"), ComponentName);

        const FString StatusStr = ApplySinglePropertyChange(World, ActorLabel, Property, Value, ComponentName);
        const bool bOk = StatusStr == TEXT("ok");

        TSharedRef<FJsonObject> ChangeResult = MakeShared<FJsonObject>();
        ChangeResult->SetStringField(TEXT("actor_label"), ActorLabel);
        ChangeResult->SetStringField(TEXT("property"),    Property);
        ChangeResult->SetStringField(TEXT("value"),       Value);
        ChangeResult->SetStringField(TEXT("status"),      bOk ? TEXT("ok") : TEXT("error"));

        if (!bOk)
        {
            ChangeResult->SetStringField(TEXT("error"), StatusStr);
            FailedCount++;
        }
        else
        {
            SucceededCount++;
        }

        Results.Add(MakeShared<FJsonValueObject>(ChangeResult));
    }

    OutResult->SetStringField(TEXT("status"),    FailedCount == 0 ? TEXT("ok") : TEXT("partial"));
    OutResult->SetNumberField(TEXT("succeeded"), SucceededCount);
    OutResult->SetNumberField(TEXT("failed"),    FailedCount);
    OutResult->SetArrayField(TEXT("changes"),    Results);

    return SucceededCount > 0;
}

// ============================================================
// Command: capture_screenshot
// ============================================================

bool UForgeCommandChannel::Cmd_CaptureScreenshot(
    const TSharedPtr<FJsonObject>& /*Cmd*/, TSharedRef<FJsonObject> OutResult)
{
    if (!Subsystem || !Subsystem->ScreenshotCapture)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("ScreenshotCapture not available"));
        return false;
    }

    Subsystem->ScreenshotCapture->RequestCapture();

    OutResult->SetStringField(TEXT("status"), TEXT("ok"));
    OutResult->SetStringField(TEXT("note"),   TEXT("screenshot requested — check screenshot/latest.png"));
    return true;
}

// ============================================================
// Command: export_asset_registry
// ============================================================

bool UForgeCommandChannel::Cmd_ExportAssetRegistry(
    const TSharedPtr<FJsonObject>& /*Cmd*/, TSharedRef<FJsonObject> OutResult)
{
    if (!Subsystem || !Subsystem->AssetRegistryCapture)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("AssetRegistryCapture not available"));
        return false;
    }

    const int32 Count = Subsystem->AssetRegistryCapture->ExportAssetRegistry();

    OutResult->SetStringField(TEXT("status"),      Count >= 0 ? TEXT("ok") : TEXT("error"));
    OutResult->SetNumberField(TEXT("asset_count"), Count);

    if (Count < 0)
    {
        OutResult->SetStringField(TEXT("error"), TEXT("ExportAssetRegistry returned -1"));
        return false;
    }

    return true;
}

// ============================================================
// Command: create_material_instance
// ============================================================
// Required: "parent_path"  (string, e.g. "/Game/Materials/M_Base")
//           "output_path"  (string, e.g. "/Game/Materials/")
//           "instance_name" (string, e.g. "MI_Blue_Base")

bool UForgeCommandChannel::Cmd_CreateMaterialInstance(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ParentPath, OutputPath, InstanceName;
    if (!Cmd->TryGetStringField(TEXT("parent_path"),   ParentPath)  ||
        !Cmd->TryGetStringField(TEXT("output_path"),   OutputPath)  ||
        !Cmd->TryGetStringField(TEXT("instance_name"), InstanceName))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            TEXT("missing required fields: 'parent_path', 'output_path', 'instance_name'"));
        return false;
    }

    // Load parent material
    UMaterialInterface* ParentMaterial = Cast<UMaterialInterface>(
        StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *ParentPath));

    if (!ParentMaterial)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("could not load parent material '%s'"), *ParentPath));
        return false;
    }

    // Build the full package name (strip trailing slash from output path)
    FString CleanOutputPath = OutputPath;
    if (CleanOutputPath.EndsWith(TEXT("/")))
    {
        CleanOutputPath.RemoveFromEnd(TEXT("/"));
    }

    const FString PackageName = CleanOutputPath / InstanceName;
    UPackage* Package = CreatePackage(*PackageName);

    if (!Package)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("could not create package '%s'"), *PackageName));
        return false;
    }

    // Create the material instance constant
    UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
        Package, *InstanceName, RF_Public | RF_Standalone | RF_Transactional);

    if (!MIC)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("NewObject<UMaterialInstanceConstant> failed"));
        return false;
    }

    MIC->Parent = ParentMaterial;
    MIC->PostEditChange();

    // Register with the asset registry so it appears in the Content Browser
    FAssetRegistryModule::AssetCreated(MIC);
    Package->MarkPackageDirty();

    OutResult->SetStringField(TEXT("status"),        TEXT("ok"));
    OutResult->SetStringField(TEXT("package"),       PackageName);
    OutResult->SetStringField(TEXT("instance_name"), InstanceName);
    OutResult->SetStringField(TEXT("parent"),        ParentPath);
    OutResult->SetStringField(TEXT("note"),
        TEXT("package marked dirty — use File > Save All or Ctrl+Shift+S to persist"));
    return true;
}

// ============================================================
// Command: import_datatable_rows
// ============================================================
// Required: "asset_path" (string), "rows" (object)

bool UForgeCommandChannel::Cmd_ImportDataTableRows(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString AssetPath;
    if (!Cmd->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'asset_path'"));
        return false;
    }

    const TSharedPtr<FJsonObject>* RowsField;
    if (!Cmd->TryGetObjectField(TEXT("rows"), RowsField))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'rows'"));
        return false;
    }

    if (!Subsystem || !Subsystem->DataTableCapture)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("DataTableCapture not available"));
        return false;
    }

    TArray<FString> Added, Updated, Failed;
    const bool bOk = Subsystem->DataTableCapture->ImportRowsFromObject(
        AssetPath, *RowsField, Added, Updated, Failed);

    auto ToJsonArray = [](const TArray<FString>& In) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(In.Num());
        for (const FString& S : In) Out.Add(MakeShared<FJsonValueString>(S));
        return Out;
    };

    OutResult->SetStringField(TEXT("status"),       bOk ? TEXT("ok") : TEXT("error"));
    OutResult->SetStringField(TEXT("asset_path"),   AssetPath);
    OutResult->SetNumberField(TEXT("added"),        Added.Num());
    OutResult->SetNumberField(TEXT("updated"),      Updated.Num());
    OutResult->SetNumberField(TEXT("failed"),       Failed.Num());
    OutResult->SetArrayField(TEXT("added_rows"),    ToJsonArray(Added));
    OutResult->SetArrayField(TEXT("updated_rows"),  ToJsonArray(Updated));
    OutResult->SetArrayField(TEXT("failed_rows"),   ToJsonArray(Failed));

    if (!bOk && Added.IsEmpty() && Updated.IsEmpty())
    {
        OutResult->SetStringField(TEXT("error"), TEXT("no rows imported — check asset_path and row data"));
    }

    return bOk;
}

// ============================================================
// Shared helper: GetEditorWorld
// ============================================================
// GEditor->GetEditorWorldContext() triggers check(0) in UE5.7 when no editor
// context is active (e.g. during early startup). Iterate GEngine->GetWorldContexts()
// instead to safely find the editor world.

UWorld* UForgeCommandChannel::GetEditorWorld() const
{
    if (!GEngine) return nullptr;

    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
    {
        if (Ctx.WorldType == EWorldType::Editor && Ctx.World())
        {
            return Ctx.World();
        }
    }
    return nullptr;
}

// ============================================================
// Shared helper: FindActorByLabel
// ============================================================

AActor* UForgeCommandChannel::FindActorByLabel(UWorld* World, const FString& ActorLabel) const
{
    if (!World) return nullptr;

    const FString LabelLower = ActorLabel.ToLower();
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel().ToLower() == LabelLower)
        {
            return *It;
        }
    }
    return nullptr;
}

// ============================================================
// Shared helper: SetObjectPropertyFromString
// ============================================================

bool UForgeCommandChannel::SetObjectPropertyFromString(
    UObject* Obj, const FString& PropertyName, const FString& Value,
    const FString& ComponentNameFilter) const
{
    if (!Obj) return false;

    // 1. Try on the object itself (actor-level properties), but only when no
    //    component filter is specified — a filter means the caller is deliberately
    //    targeting a specific component rather than the actor root.
    if (ComponentNameFilter.IsEmpty())
    {
        FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *PropertyName);
        if (Prop)
        {
            void* PropData = Prop->ContainerPtrToValuePtr<void>(Obj);
            const TCHAR* Result = Prop->ImportText_Direct(*Value, PropData, Obj, PPF_None);
            if (Result)
            {
                FPropertyChangedEvent ChangeEvent(Prop);
                Obj->PostEditChangeProperty(ChangeEvent);
                Obj->MarkPackageDirty();
                return true;
            }
        }
    }

    // 2. Try components. If ComponentNameFilter is set, only the named component
    //    is checked; otherwise the first component owning the property wins.
    AActor* Actor = Cast<AActor>(Obj);
    if (Actor)
    {
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);

        for (UActorComponent* Comp : Components)
        {
            if (!ComponentNameFilter.IsEmpty() &&
                !Comp->GetName().Equals(ComponentNameFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }

            FProperty* CompProp = FindFProperty<FProperty>(Comp->GetClass(), *PropertyName);
            if (CompProp)
            {
                void* PropData = CompProp->ContainerPtrToValuePtr<void>(Comp);
                const TCHAR* Result = CompProp->ImportText_Direct(*Value, PropData, Comp, PPF_None);
                if (Result)
                {
                    // Fire property-change notification so the editor panel and
                    // any dependent systems (lights, fog, etc.) rebuild correctly.
                    FPropertyChangedEvent CompChangeEvent(CompProp);
                    Comp->PostEditChangeProperty(CompChangeEvent);
                    Comp->MarkRenderStateDirty();
                    Actor->MarkPackageDirty();
                    return true;
                }
            }
        }
    }

    return false;
}

// ============================================================
// Shared helper: ApplySinglePropertyChange
// ============================================================

FString UForgeCommandChannel::ApplySinglePropertyChange(
    UWorld* World, const FString& ActorLabel,
    const FString& Property, const FString& Value,
    const FString& ComponentName) const
{
    if (ActorLabel.IsEmpty()) return TEXT("'actor_label' is empty");
    if (Property.IsEmpty())   return TEXT("'property' is empty");

    AActor* Actor = FindActorByLabel(World, ActorLabel);
    if (!Actor)
    {
        return FString::Printf(TEXT("actor '%s' not found"), *ActorLabel);
    }

    if (!SetObjectPropertyFromString(Actor, Property, Value, ComponentName))
    {
        if (!ComponentName.IsEmpty())
        {
            return FString::Printf(
                TEXT("property '%s' not found on component '%s' of actor '%s'"),
                *Property, *ComponentName, *ActorLabel);
        }
        return FString::Printf(TEXT("property '%s' not found on actor or components of '%s'"),
            *Property, *ActorLabel);
    }

    return TEXT("ok");
}

// ============================================================
// ============================================================
// Command: set_actor_transform
// ============================================================
// Required: "actor_label"
// Optional: "location" {x,y,z}, "rotation" {pitch,yaw,roll}, "scale" {x,y,z}
// Any omitted component retains its current value.

bool UForgeCommandChannel::Cmd_SetActorTransform(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ActorLabel;
    if (!Cmd->TryGetStringField(TEXT("actor_label"), ActorLabel))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'actor_label'"));
        return false;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    AActor* Actor = FindActorByLabel(World, ActorLabel);
    if (!Actor)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("actor '%s' not found"), *ActorLabel));
        return false;
    }

    FVector   Location = Actor->GetActorLocation();
    FRotator  Rotation = Actor->GetActorRotation();
    FVector   Scale    = Actor->GetActorScale3D();

    // Parse location — apply each axis only if present
    const TSharedPtr<FJsonObject>* LocObj;
    if (Cmd->TryGetObjectField(TEXT("location"), LocObj))
    {
        double X = Location.X, Y = Location.Y, Z = Location.Z;
        (*LocObj)->TryGetNumberField(TEXT("x"), X);
        (*LocObj)->TryGetNumberField(TEXT("y"), Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), Z);
        Location = FVector((float)X, (float)Y, (float)Z);
    }

    // Parse rotation
    const TSharedPtr<FJsonObject>* RotObj;
    if (Cmd->TryGetObjectField(TEXT("rotation"), RotObj))
    {
        double Pitch = Rotation.Pitch, Yaw = Rotation.Yaw, Roll = Rotation.Roll;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
        Rotation = FRotator((float)Pitch, (float)Yaw, (float)Roll);
    }

    // Parse scale
    const TSharedPtr<FJsonObject>* ScaleObj;
    if (Cmd->TryGetObjectField(TEXT("scale"), ScaleObj))
    {
        double X = Scale.X, Y = Scale.Y, Z = Scale.Z;
        (*ScaleObj)->TryGetNumberField(TEXT("x"), X);
        (*ScaleObj)->TryGetNumberField(TEXT("y"), Y);
        (*ScaleObj)->TryGetNumberField(TEXT("z"), Z);
        Scale = FVector((float)X, (float)Y, (float)Z);
    }

    Actor->Modify();
    Actor->SetActorTransform(FTransform(Rotation, Location, Scale), false, nullptr,
                             ETeleportType::TeleportPhysics);
    Actor->MarkPackageDirty();

    OutResult->SetStringField(TEXT("status"),      TEXT("ok"));
    OutResult->SetStringField(TEXT("actor_label"), ActorLabel);
    OutResult->SetStringField(TEXT("location"),
        FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), Location.X, Location.Y, Location.Z));
    OutResult->SetStringField(TEXT("rotation"),
        FString::Printf(TEXT("(P=%.2f, Y=%.2f, R=%.2f)"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
    OutResult->SetStringField(TEXT("scale"),
        FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), Scale.X, Scale.Y, Scale.Z));
    return true;
}

// ============================================================
// Command: delete_actor
// ============================================================
// Required: "actor_label"
// Creates an editor undo entry via GEditor transaction system.

bool UForgeCommandChannel::Cmd_DeleteActor(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    FString ActorLabel;
    if (!Cmd->TryGetStringField(TEXT("actor_label"), ActorLabel))
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'actor_label'"));
        return false;
    }

    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    AActor* Actor = FindActorByLabel(World, ActorLabel);
    if (!Actor)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),
            FString::Printf(TEXT("actor '%s' not found"), *ActorLabel));
        return false;
    }

    // Use the selection-based deletion path so the editor creates an undo entry
    // and fires all the relevant notifications (OnLevelActorDeleted, etc.).
    GEditor->SelectNone(false, true);
    GEditor->SelectActor(Actor, true, true, true);
    GEditor->edactDeleteSelected(World);
    GEditor->SelectNone(false, true);

    OutResult->SetStringField(TEXT("status"),      TEXT("ok"));
    OutResult->SetStringField(TEXT("actor_label"), ActorLabel);
    OutResult->SetStringField(TEXT("note"),        TEXT("undo available via Ctrl+Z"));
    return true;
}

// ============================================================
// Command: save_packages
// ============================================================
// Optional: "paths" — array of package paths to save (e.g. ["/Game/Materials/MI_Blue"])
//           If omitted, saves all dirty content packages (map excluded by default).
// Optional: "save_map" — bool, default false. Set true to also save the current map.

bool UForgeCommandChannel::Cmd_SavePackages(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    bool bSaveMap = false;
    Cmd->TryGetBoolField(TEXT("save_map"), bSaveMap);

    const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
    const bool bHasPaths = Cmd->TryGetArrayField(TEXT("paths"), PathsArray)
                           && PathsArray && !PathsArray->IsEmpty();

    int32 SavedCount = 0;

    if (bHasPaths)
    {
        // Targeted save: resolve each package by path, collect dirty ones, save.
        TArray<UPackage*> ToSave;
        TArray<FString>   NotFound;

        for (const TSharedPtr<FJsonValue>& Val : *PathsArray)
        {
            const FString PkgPath = Val->AsString();
            UPackage* Pkg = FindPackage(nullptr, *PkgPath);
            if (!Pkg)
            {
                // Try loading it in case it hasn't been touched this session
                Pkg = LoadPackage(nullptr, *PkgPath, LOAD_None);
            }

            if (Pkg && Pkg->IsDirty())
            {
                ToSave.Add(Pkg);
            }
            else if (!Pkg)
            {
                NotFound.Add(PkgPath);
            }
        }

        if (ToSave.Num() > 0)
        {
            const bool bOk = FEditorFileUtils::PromptForCheckoutAndSave(
                ToSave, /*bCheckDirty=*/true, /*bPromptToSave=*/false) ==
                FEditorFileUtils::EPromptReturnCode::PR_Success;

            SavedCount = bOk ? ToSave.Num() : 0;
        }

        if (!NotFound.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> NotFoundJson;
            for (const FString& S : NotFound) NotFoundJson.Add(MakeShared<FJsonValueString>(S));
            OutResult->SetArrayField(TEXT("not_found"), NotFoundJson);
        }
    }
    else
    {
        // Bulk save: all dirty content packages, optionally the map too.
        // Args: bPromptUserToSave, bSaveMapPackages, bSaveContentPackages,
        //       bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined
        FEditorFileUtils::SaveDirtyPackages(
            /*bPromptUserToSave=*/   false,
            /*bSaveMapPackages=*/    bSaveMap,
            /*bSaveContentPackages=*/true,
            /*bFastSave=*/           false,
            /*bNotifyNoPackagesSaved=*/false,
            /*bCanBeDeclined=*/      false);

        // SaveDirtyPackages is void — assume success unless an error is logged
        SavedCount = -1; // -1 = "bulk save fired, exact count unavailable"
    }

    OutResult->SetStringField(TEXT("status"),      TEXT("ok"));
    OutResult->SetNumberField(TEXT("saved_count"), SavedCount);
    OutResult->SetBoolField  (TEXT("map_included"), bSaveMap);

    if (SavedCount == -1)
    {
        OutResult->SetStringField(TEXT("note"),
            TEXT("bulk save completed — exact package count not tracked"));
    }

    return true;
}

// ============================================================
// Command: draw_debug_line                               v1.14
// ============================================================
// Required: "start" {x,y,z}, "end" {x,y,z}
// Optional: "color" (default "Green"), "duration" (default 0 = persistent),
//           "thickness" (default 1.0)

bool UForgeCommandChannel::Cmd_DrawDebugLine(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    const FVector Start = ReadVec3(Cmd, TEXT("start"));
    const FVector End   = ReadVec3(Cmd, TEXT("end"),   FVector(100.f, 0.f, 0.f));

    FString ColorStr = TEXT("Green");
    Cmd->TryGetStringField(TEXT("color"), ColorStr);
    const FColor Color = ParseColor(ColorStr);

    double Duration  = 0.0;
    double Thickness = 1.0;
    Cmd->TryGetNumberField(TEXT("duration"),  Duration);
    Cmd->TryGetNumberField(TEXT("thickness"), Thickness);

    // duration == 0  →  persistent (survives until clear_debug_shapes)
    // duration  > 0  →  auto-expires after Duration seconds
    const bool  bPersistent = (Duration <= 0.0);
    const float LifeTime    = bPersistent ? -1.f : (float)Duration;

    DrawDebugLine(World, Start, End, Color, bPersistent, LifeTime,
                  /*DepthPriority=*/0, (float)Thickness);

    OutResult->SetStringField(TEXT("status"),    TEXT("ok"));
    OutResult->SetStringField(TEXT("color"),     ColorStr);
    OutResult->SetBoolField  (TEXT("persistent"), bPersistent);
    OutResult->SetNumberField(TEXT("duration"),  Duration);
    return true;
}

// ============================================================
// Command: draw_debug_sphere                             v1.14
// ============================================================
// Required: "center" {x,y,z}
// Optional: "radius" (default 50.0), "color", "duration", "thickness"

bool UForgeCommandChannel::Cmd_DrawDebugSphere(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    const FVector Center = ReadVec3(Cmd, TEXT("center"));

    double Radius    = 50.0;
    double Duration  = 0.0;
    double Thickness = 1.0;
    Cmd->TryGetNumberField(TEXT("radius"),    Radius);
    Cmd->TryGetNumberField(TEXT("duration"),  Duration);
    Cmd->TryGetNumberField(TEXT("thickness"), Thickness);

    FString ColorStr = TEXT("Green");
    Cmd->TryGetStringField(TEXT("color"), ColorStr);
    const FColor Color = ParseColor(ColorStr);

    const bool  bPersistent = (Duration <= 0.0);
    const float LifeTime    = bPersistent ? -1.f : (float)Duration;

    DrawDebugSphere(World, Center, (float)Radius,
                    /*Segments=*/16, Color, bPersistent, LifeTime,
                    /*DepthPriority=*/0, (float)Thickness);

    OutResult->SetStringField(TEXT("status"),     TEXT("ok"));
    OutResult->SetStringField(TEXT("color"),      ColorStr);
    OutResult->SetNumberField(TEXT("radius"),     Radius);
    OutResult->SetBoolField  (TEXT("persistent"), bPersistent);
    OutResult->SetNumberField(TEXT("duration"),   Duration);
    return true;
}

// ============================================================
// Command: draw_debug_box                                v1.14
// ============================================================
// Required: "center" {x,y,z}, "extent" {x,y,z}
// Optional: "rotation" {pitch,yaw,roll}, "color", "duration", "thickness"

bool UForgeCommandChannel::Cmd_DrawDebugBox(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    const FVector Center = ReadVec3(Cmd, TEXT("center"));
    const FVector Extent = ReadVec3(Cmd, TEXT("extent"), FVector(50.f, 50.f, 50.f));

    // Rotation — optional {pitch, yaw, roll}
    FRotator Rotation = FRotator::ZeroRotator;
    const TSharedPtr<FJsonObject>* RotObj;
    if (Cmd->TryGetObjectField(TEXT("rotation"), RotObj))
    {
        double Pitch = 0, Yaw = 0, Roll = 0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
        Rotation = FRotator((float)Pitch, (float)Yaw, (float)Roll);
    }

    double Duration  = 0.0;
    double Thickness = 1.0;
    Cmd->TryGetNumberField(TEXT("duration"),  Duration);
    Cmd->TryGetNumberField(TEXT("thickness"), Thickness);

    FString ColorStr = TEXT("Green");
    Cmd->TryGetStringField(TEXT("color"), ColorStr);
    const FColor Color = ParseColor(ColorStr);

    const bool  bPersistent = (Duration <= 0.0);
    const float LifeTime    = bPersistent ? -1.f : (float)Duration;

    DrawDebugBox(World, Center, Extent, Rotation.Quaternion(), Color,
                 bPersistent, LifeTime, /*DepthPriority=*/0, (float)Thickness);

    OutResult->SetStringField(TEXT("status"),     TEXT("ok"));
    OutResult->SetStringField(TEXT("color"),      ColorStr);
    OutResult->SetBoolField  (TEXT("persistent"), bPersistent);
    OutResult->SetNumberField(TEXT("duration"),   Duration);
    return true;
}

// ============================================================
// Command: draw_debug_text                               v1.14
// ============================================================
// Required: "text" (string), "location" {x,y,z}
// Optional: "color", "duration" (default 0 = persistent), "font_scale" (default 1.0)

bool UForgeCommandChannel::Cmd_DrawDebugText(
    const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    FString Text;
    if (!Cmd->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("missing required field 'text'"));
        return false;
    }

    const FVector Location = ReadVec3(Cmd, TEXT("location"));

    double Duration  = 0.0;
    double FontScale = 1.0;
    Cmd->TryGetNumberField(TEXT("duration"),   Duration);
    Cmd->TryGetNumberField(TEXT("font_scale"), FontScale);

    FString ColorStr = TEXT("White");
    Cmd->TryGetStringField(TEXT("color"), ColorStr);
    const FColor Color = ParseColor(ColorStr);

    // DrawDebugString LifeTime semantics: -1 = until FlushDebugStrings,
    // > 0 = auto-expires. We mirror the persistent/expiring split.
    const float LifeTime = (Duration <= 0.0) ? -1.f : (float)Duration;

    DrawDebugString(World, Location, Text,
                    /*TestBaseActor=*/nullptr, Color, LifeTime,
                    /*bDrawShadow=*/true, (float)FontScale);

    OutResult->SetStringField(TEXT("status"),     TEXT("ok"));
    OutResult->SetStringField(TEXT("text"),       Text);
    OutResult->SetStringField(TEXT("color"),      ColorStr);
    OutResult->SetBoolField  (TEXT("persistent"), Duration <= 0.0);
    OutResult->SetNumberField(TEXT("duration"),   Duration);
    return true;
}

// ============================================================
// Command: clear_debug_shapes                            v1.14
// ============================================================
// No required fields.
// Flushes all persistent debug lines, boxes, spheres, and text strings
// from the Editor world.

bool UForgeCommandChannel::Cmd_ClearDebugShapes(
    const TSharedPtr<FJsonObject>& /*Cmd*/, TSharedRef<FJsonObject> OutResult)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("no editor world available"));
        return false;
    }

    FlushPersistentDebugLines(World);   // clears DrawDebugLine/Sphere/Box with bPersistent=true
    FlushDebugStrings(World);           // clears DrawDebugString with LifeTime=-1

    OutResult->SetStringField(TEXT("status"), TEXT("ok"));
    OutResult->SetStringField(TEXT("note"),
        TEXT("Flushed all persistent debug lines and strings from the Editor viewport."));
    return true;
}

// ============================================================
// Command: capture_perf_snapshot (v1.15)
// ============================================================

bool UForgeCommandChannel::Cmd_CapturePerfSnapshot(
    const TSharedPtr<FJsonObject>& /*Cmd*/, TSharedRef<FJsonObject> OutResult)
{
    if (!Subsystem || !Subsystem->PerformanceCapture)
    {
        OutResult->SetStringField(TEXT("status"), TEXT("error"));
        OutResult->SetStringField(TEXT("error"),  TEXT("PerformanceCapture not initialized"));
        return false;
    }

    Subsystem->PerformanceCapture->CaptureSnapshot();

    OutResult->SetStringField(TEXT("status"), TEXT("ok"));
    OutResult->SetStringField(TEXT("file"),   TEXT("perf/latest.json"));
    OutResult->SetStringField(TEXT("note"),
        TEXT("Snapshot written to perf/latest.json and perf/history/perf-NNN.json"));
    return true;
}

// ============================================================
// UpdateIndexFile — READ-MERGE-WRITE
// ============================================================

void UForgeCommandChannel::UpdateIndexFile()
{
    const FString IndexPath = OutputDir / TEXT("index.json");
    const FString Timestamp = FForgeContextWriter::NowISO8601();

    TSharedPtr<FJsonObject> Root;
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *IndexPath))
    {
        TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExistingContent);
        FJsonSerializer::Deserialize(R, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
        Captures = *ExistingCaptures;
    else
        Captures = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> CmdSection = MakeShared<FJsonObject>();
    CmdSection->SetStringField(TEXT("inbox_dir"),   TEXT("commands/inbox/"));
    CmdSection->SetStringField(TEXT("history_dir"), TEXT("commands/history/"));
    CmdSection->SetNumberField(TEXT("max_per_tick"), MaxPerTick);
    CmdSection->SetStringField(TEXT("last_updated"), Timestamp);
    CmdSection->SetArrayField(TEXT("supported_commands"), []()
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        static const TCHAR* Types[] = {
            TEXT("trigger_pcg_regenerate"),
            TEXT("spawn_test_actor"),
            TEXT("set_actor_property"),
            TEXT("apply_weather"),
            TEXT("capture_screenshot"),
            TEXT("export_asset_registry"),
            TEXT("create_material_instance"),
            TEXT("import_datatable_rows"),
            TEXT("set_actor_transform"),
            TEXT("delete_actor"),
            TEXT("save_packages"),
            TEXT("draw_debug_line"),
            TEXT("draw_debug_sphere"),
            TEXT("draw_debug_box"),
            TEXT("draw_debug_text"),
            TEXT("clear_debug_shapes"),
        };
        for (const TCHAR* T : Types) Arr.Add(MakeShared<FJsonValueString>(T));
        return Arr;
    }());

    Captures->SetObjectField(TEXT("commands"), CmdSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), TEXT("0.2.6"));

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
