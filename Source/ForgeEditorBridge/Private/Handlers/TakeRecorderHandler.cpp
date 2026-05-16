#include "Handlers/TakeRecorderHandler.h"
#include "Recorder/TakeRecorderBlueprintLibrary.h"
#include "Recorder/TakeRecorderPanel.h"
#include "Recorder/TakeRecorderParameters.h"
#include "TakeRecorderSources.h"  // 5.7: in TakesCore module top-level Public/
#include "TakePreset.h"
#include "TakeMetaData.h"
#include "LevelSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("take");

FBridgeResult UTakeRecorderHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("start_recording"))     return Action_StartRecording(Params);
    if (Action == TEXT("stop_recording"))      return Action_StopRecording(Params);
    if (Action == TEXT("is_recording"))        return Action_IsRecording(Params);
    if (Action == TEXT("get_active_take"))     return Action_GetActiveTake(Params);
    if (Action == TEXT("list_takes_in_path"))  return Action_ListTakesInPath(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("start_recording, stop_recording, is_recording, get_active_take, list_takes_in_path"));
}

FBridgeResult UTakeRecorderHandler::Action_StartRecording(TSharedPtr<FJsonObject> Params)
{
    // 5.7 signature: StartRecording(ULevelSequence*, UTakeRecorderSources*, UTakeMetaData*, const FTakeRecorderParameters&)
    // Slate name + take number live on UTakeMetaData; the recorder-panel toggle is no longer
    // a parameter. Caller may supply asset_path (existing UTakePreset to seed) or omit for empty.
    FString PresetPath, Slate;
    int32 TakeNumber = -1;
    Params->TryGetStringField(TEXT("preset_path"), PresetPath);
    Params->TryGetStringField(TEXT("slate"), Slate);
    Params->TryGetNumberField(TEXT("take_number"), TakeNumber);

    // Build a transient ULevelSequence + sources + metadata from the (optional) preset.
    ULevelSequence* TargetSeq = nullptr;
    UTakeRecorderSources* Sources = nullptr;
    UTakeMetaData* MetaData = nullptr;

    if (!PresetPath.IsEmpty())
    {
        UTakePreset* Preset = LoadObject<UTakePreset>(nullptr, *PresetPath);
        if (!Preset)
        {
            const FString Suffix = PresetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(PresetPath);
            Preset = LoadObject<UTakePreset>(nullptr, *Suffix);
        }
        if (!Preset)
            return MakeError(DOMAIN, TEXT("start_recording"), 2000,
                FString::Printf(TEXT("TakePreset not found: %s"), *PresetPath));
        TargetSeq = Preset->GetLevelSequence();
    }
    if (!TargetSeq)
    {
        TargetSeq = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient);
        TargetSeq->Initialize();
    }

    Sources  = NewObject<UTakeRecorderSources>(GetTransientPackage(), NAME_None, RF_Transient);
    MetaData = NewObject<UTakeMetaData>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!Slate.IsEmpty())  MetaData->SetSlate(Slate);
    if (TakeNumber >= 0)   MetaData->SetTakeNumber(TakeNumber);

    FTakeRecorderParameters Parameters;

    UTakeRecorder* Recorder = UTakeRecorderBlueprintLibrary::StartRecording(
        TargetSeq, Sources, MetaData, Parameters);
    if (!Recorder)
        return MakeError(DOMAIN, TEXT("start_recording"), 3000,
            TEXT("StartRecording returned null"),
            TEXT("Verify the Take Recorder plugin is enabled and a take source is configured"));

    return MakeSuccess(DOMAIN, TEXT("start_recording"),
        FString::Printf(TEXT("Recording started (slate='%s' take=%d)"), *Slate, TakeNumber));
}

FBridgeResult UTakeRecorderHandler::Action_StopRecording(TSharedPtr<FJsonObject> Params)
{
    UTakeRecorderBlueprintLibrary::StopRecording();
    return MakeSuccess(DOMAIN, TEXT("stop_recording"), TEXT("Recording stopped"));
}

FBridgeResult UTakeRecorderHandler::Action_IsRecording(TSharedPtr<FJsonObject> Params)
{
    const bool bRecording = UTakeRecorderBlueprintLibrary::IsRecording();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("recording"), bRecording);
    return MakeSuccess(DOMAIN, TEXT("is_recording"),
        bRecording ? TEXT("recording") : TEXT("idle"), Data);
}

FBridgeResult UTakeRecorderHandler::Action_GetActiveTake(TSharedPtr<FJsonObject> Params)
{
    if (!UTakeRecorderBlueprintLibrary::IsRecording())
        return MakeSuccess(DOMAIN, TEXT("get_active_take"), TEXT("Not currently recording"));
    UTakeRecorder* Active = UTakeRecorderBlueprintLibrary::GetActiveRecorder();
    if (!Active)
        return MakeError(DOMAIN, TEXT("get_active_take"), 3000, TEXT("Active recorder unavailable"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("recorder_class"), Active->GetClass()->GetName());
    return MakeSuccess(DOMAIN, TEXT("get_active_take"), TEXT("Recording in progress"), Data);
}

FBridgeResult UTakeRecorderHandler::Action_ListTakesInPath(TSharedPtr<FJsonObject> Params)
{
    FString ContentPath = TEXT("/Game/Takes");
    Params->TryGetStringField(TEXT("content_path"), ContentPath);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> All;
    ARM.Get().GetAssetsByPath(FName(*ContentPath), All, /*bRecursive=*/true);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FAssetData& A : All)
    {
        // Filter to ULevelSequence (which is what Take Recorder writes)
        if (A.AssetClassPath.GetAssetName() != FName(TEXT("LevelSequence"))) continue;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), A.AssetName.ToString());
        O->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("takes"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Data->SetStringField(TEXT("content_path"), ContentPath);
    return MakeSuccess(DOMAIN, TEXT("list_takes_in_path"),
        FString::Printf(TEXT("%d take(s) under '%s'"), Arr.Num(), *ContentPath), Data);
}
