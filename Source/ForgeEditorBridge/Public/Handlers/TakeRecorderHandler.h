#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "TakeRecorderHandler.generated.h"

/**
 * TakeRecorderHandler — domain "take"  (UE 5.7)
 *
 * Wraps UTakeRecorderBlueprintLibrary. Take Recorder records gameplay/sequencer
 * activity into a Level Sequence asset; this handler exposes the start/stop
 * lifecycle. Deeper take-source CRUD (per-source overrides, naming patterns)
 * is left to UI / Python until specifically requested.
 *
 * Actions:
 *   start_recording  → preset_path? (UTakePreset asset), slate? (string), take_number? (int)
 *   stop_recording   → (no params)
 *   is_recording     → returns bool
 *   get_active_take  → returns the current ULevelSequence asset path (if recording)
 *   list_takes_in_path → content_path (default "/Game/Takes") → recursively list takes
 */
UCLASS()
class FORGEEDITORBRIDGE_API UTakeRecorderHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("take"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("start_recording"), TEXT("stop_recording"), TEXT("is_recording"),
            TEXT("get_active_take"), TEXT("list_takes_in_path")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_StartRecording   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_StopRecording    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_IsRecording      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetActiveTake    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListTakesInPath  (TSharedPtr<FJsonObject> Params);
};
