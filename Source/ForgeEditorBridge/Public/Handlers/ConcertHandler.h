#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ConcertHandler.generated.h"

/**
 * ConcertHandler — domain "concert"  (UE 5.7)
 *
 * Multi-User Editing (Concert) session queries. Joining/leaving sessions
 * goes through UConcertSyncClient — not all of which is in the bridge's
 * dependency graph. This handler covers what's reachable via runtime
 * console commands + IConcertSyncClient where mounted.
 *
 * Actions:
 *   is_concert_loaded     → returns whether the Concert plugin is mounted
 *   list_sessions         → lists discoverable sessions on the active server
 *   get_active_session    → returns name + id of joined session, if any
 *   leave_session         → leaves current session via console
 *   open_concert_browser  → opens the Multi-User Browser editor tab
 */
UCLASS()
class FORGEEDITORBRIDGE_API UConcertHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("concert"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("is_concert_loaded"), TEXT("list_sessions"), TEXT("get_active_session"),
            TEXT("leave_session"), TEXT("open_concert_browser")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_IsConcertLoaded   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListSessions      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetActiveSession  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_LeaveSession      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_OpenConcertBrowser(TSharedPtr<FJsonObject> Params);
};
