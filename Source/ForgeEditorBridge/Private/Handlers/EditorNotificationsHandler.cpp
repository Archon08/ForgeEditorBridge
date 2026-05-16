#include "Handlers/EditorNotificationsHandler.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("notify");

FBridgeResult UEditorNotificationsHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("show_notification")) return Action_ShowNotification(Params);
    if (Action == TEXT("show_progress"))     return Action_ShowProgress(Params);
    if (Action == TEXT("update_progress"))   return Action_UpdateProgress(Params);
    if (Action == TEXT("complete"))          return Action_Complete(Params);
    if (Action == TEXT("dismiss"))           return Action_Dismiss(Params);
    if (Action == TEXT("list_active"))       return Action_ListActive(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("show_notification, show_progress, update_progress, complete, dismiss, list_active"));
}

FBridgeResult UEditorNotificationsHandler::Action_ShowNotification(TSharedPtr<FJsonObject> Params)
{
    FString Text, Type = TEXT("info");
    double Duration = 5.0;
    if (!Params->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
        return MakeError(DOMAIN, TEXT("show_notification"), 1000, TEXT("'text' is required"));
    Params->TryGetStringField(TEXT("type"), Type);
    Params->TryGetNumberField(TEXT("duration_seconds"), Duration);

    FNotificationInfo Info(FText::FromString(Text));
    Info.ExpireDuration = (float)Duration;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = false;
    Info.bFireAndForget = true;
    TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (!Item.IsValid())
        return MakeError(DOMAIN, TEXT("show_notification"), 3000, TEXT("AddNotification returned null"));

    const FString T = Type.ToLower();
    if (T == TEXT("warning"))     Item->SetCompletionState(SNotificationItem::CS_Pending);
    else if (T == TEXT("error"))  Item->SetCompletionState(SNotificationItem::CS_Fail);
    else                          Item->SetCompletionState(SNotificationItem::CS_Success);

    return MakeSuccess(DOMAIN, TEXT("show_notification"),
        FString::Printf(TEXT("Shown: %s"), *Text));
}

FBridgeResult UEditorNotificationsHandler::Action_ShowProgress(TSharedPtr<FJsonObject> Params)
{
    FString Id, Text;
    if (!Params->TryGetStringField(TEXT("notification_id"), Id) || Id.IsEmpty())
        return MakeError(DOMAIN, TEXT("show_progress"), 1000, TEXT("'notification_id' is required"));
    if (!Params->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
        return MakeError(DOMAIN, TEXT("show_progress"), 1000, TEXT("'text' is required"));
    if (Active.Contains(Id))
        return MakeError(DOMAIN, TEXT("show_progress"), 2002,
            FString::Printf(TEXT("notification_id '%s' already in use"), *Id));

    FNotificationInfo Info(FText::FromString(Text));
    Info.ExpireDuration = 0.0f;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = true;
    Info.bFireAndForget = false;
    TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (!Item.IsValid())
        return MakeError(DOMAIN, TEXT("show_progress"), 3000, TEXT("AddNotification returned null"));
    Item->SetCompletionState(SNotificationItem::CS_Pending);
    Active.Add(Id, Item);
    return MakeSuccess(DOMAIN, TEXT("show_progress"),
        FString::Printf(TEXT("Started progress '%s'"), *Id));
}

FBridgeResult UEditorNotificationsHandler::Action_UpdateProgress(TSharedPtr<FJsonObject> Params)
{
    FString Id, Text;
    double Percent = -1.0;
    if (!Params->TryGetStringField(TEXT("notification_id"), Id) || Id.IsEmpty())
        return MakeError(DOMAIN, TEXT("update_progress"), 1000, TEXT("'notification_id' is required"));
    Params->TryGetStringField(TEXT("text"), Text);
    Params->TryGetNumberField(TEXT("percent"), Percent);

    TSharedPtr<SNotificationItem>* Item = Active.Find(Id);
    if (!Item || !Item->IsValid())
        return MakeError(DOMAIN, TEXT("update_progress"), 2000,
            FString::Printf(TEXT("notification_id '%s' not active"), *Id));

    if (!Text.IsEmpty())
    {
        FString Composed = Text;
        if (Percent >= 0.0)
        {
            Composed += FString::Printf(TEXT(" (%.0f%%)"), Percent * 100.0);
        }
        (*Item)->SetText(FText::FromString(Composed));
    }
    return MakeSuccess(DOMAIN, TEXT("update_progress"),
        FString::Printf(TEXT("Updated '%s'"), *Id));
}

FBridgeResult UEditorNotificationsHandler::Action_Complete(TSharedPtr<FJsonObject> Params)
{
    FString Id;
    bool bSuccess = true;
    if (!Params->TryGetStringField(TEXT("notification_id"), Id) || Id.IsEmpty())
        return MakeError(DOMAIN, TEXT("complete"), 1000, TEXT("'notification_id' is required"));
    Params->TryGetBoolField(TEXT("success"), bSuccess);

    TSharedPtr<SNotificationItem>* Item = Active.Find(Id);
    if (!Item || !Item->IsValid())
        return MakeError(DOMAIN, TEXT("complete"), 2000,
            FString::Printf(TEXT("notification_id '%s' not active"), *Id));

    (*Item)->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
    (*Item)->ExpireAndFadeout();
    Active.Remove(Id);
    return MakeSuccess(DOMAIN, TEXT("complete"),
        FString::Printf(TEXT("Completed '%s' (%s)"), *Id, bSuccess ? TEXT("success") : TEXT("fail")));
}

FBridgeResult UEditorNotificationsHandler::Action_Dismiss(TSharedPtr<FJsonObject> Params)
{
    FString Id;
    if (!Params->TryGetStringField(TEXT("notification_id"), Id) || Id.IsEmpty())
        return MakeError(DOMAIN, TEXT("dismiss"), 1000, TEXT("'notification_id' is required"));
    TSharedPtr<SNotificationItem>* Item = Active.Find(Id);
    if (!Item || !Item->IsValid())
        return MakeError(DOMAIN, TEXT("dismiss"), 2000,
            FString::Printf(TEXT("notification_id '%s' not active"), *Id));
    (*Item)->ExpireAndFadeout();
    Active.Remove(Id);
    return MakeSuccess(DOMAIN, TEXT("dismiss"),
        FString::Printf(TEXT("Dismissed '%s'"), *Id));
}

FBridgeResult UEditorNotificationsHandler::Action_ListActive(TSharedPtr<FJsonObject> Params)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, TSharedPtr<SNotificationItem>>& Pair : Active)
    {
        Arr.Add(MakeShared<FJsonValueString>(Pair.Key));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("active"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_active"),
        FString::Printf(TEXT("%d active notification(s)"), Arr.Num()), Data);
}
