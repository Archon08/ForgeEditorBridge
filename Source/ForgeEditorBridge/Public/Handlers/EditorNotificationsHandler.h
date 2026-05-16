#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "EditorNotificationsHandler.generated.h"

class SNotificationItem;

/**
 * EditorNotificationsHandler — domain "notify"  (UE 5.7)
 *
 * Wraps FSlateNotificationManager to surface AI-driven notifications to the
 * user inside the editor's bottom-right notification stream.
 *
 * Actions:
 *   show_notification    → text, type? ("info"|"warning"|"error", default "info"),
 *                          duration_seconds? (default 5)
 *   show_progress        → notification_id, text — creates a long-running notification
 *   update_progress      → notification_id, text?, percent? (0..1)
 *   complete             → notification_id, success? (bool, default true)
 *   dismiss              → notification_id
 *   list_active          → returns array of active notification_ids
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEditorNotificationsHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("notify"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("show_notification"), TEXT("show_progress"),
            TEXT("update_progress"), TEXT("complete"),
            TEXT("dismiss"), TEXT("list_active")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    TMap<FString, TSharedPtr<SNotificationItem>> Active;

    FBridgeResult Action_ShowNotification (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ShowProgress     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_UpdateProgress   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Complete         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Dismiss          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListActive       (TSharedPtr<FJsonObject> Params);
};
