#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "AssetEventsHandler.generated.h"

/**
 * AssetEventsHandler — domain "asset_events"  (UE 5.7)
 *
 * Subscribes to FAssetRegistry delegates so the bridge can react to
 * other tools' content changes. Events are recorded to an in-memory ring
 * buffer that callers poll via get_recent_events.
 *
 * Actions:
 *   enable_subscriptions    → (no params) — hooks delegates
 *   disable_subscriptions   → (no params) — unhooks delegates
 *   get_recent_events       → limit? (default 100), since? (event_id)
 *                             returns array of {id, kind, asset_path, old_path?, time}
 *   get_event_count         → returns total recorded
 *   clear_event_log         → clears the ring buffer
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAssetEventsHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("asset_events"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("enable_subscriptions"), TEXT("disable_subscriptions"),
            TEXT("get_recent_events"), TEXT("get_event_count"), TEXT("clear_event_log")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    struct FEventEntry
    {
        int64 Id = 0;
        FString Kind;        // "added" | "removed" | "renamed" | "updated"
        FString AssetPath;
        FString OldPath;
        FDateTime Time;
    };

    bool bSubscribed = false;
    int64 NextId = 1;
    TArray<FEventEntry> Events;
    static constexpr int32 MaxEvents = 4096;
    FDelegateHandle AddedHandle, RemovedHandle, RenamedHandle, UpdatedHandle;

    void Subscribe();
    void Unsubscribe();
    void RecordEvent(const FString& Kind, const FString& Path, const FString& OldPath = FString());

    FBridgeResult Action_Enable           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Disable          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetRecent        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetCount         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearLog         (TSharedPtr<FJsonObject> Params);
};
