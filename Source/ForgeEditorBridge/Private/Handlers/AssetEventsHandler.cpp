#include "Handlers/AssetEventsHandler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("asset_events");

void UAssetEventsHandler::Subscribe()
{
    if (bSubscribed) return;
    FAssetRegistryModule& M = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = M.Get();
    AddedHandle = AR.OnAssetAdded().AddLambda([this](const FAssetData& D)
    {
        RecordEvent(TEXT("added"), D.GetSoftObjectPath().ToString());
    });
    RemovedHandle = AR.OnAssetRemoved().AddLambda([this](const FAssetData& D)
    {
        RecordEvent(TEXT("removed"), D.GetSoftObjectPath().ToString());
    });
    RenamedHandle = AR.OnAssetRenamed().AddLambda([this](const FAssetData& D, const FString& OldName)
    {
        RecordEvent(TEXT("renamed"), D.GetSoftObjectPath().ToString(), OldName);
    });
    UpdatedHandle = AR.OnAssetUpdated().AddLambda([this](const FAssetData& D)
    {
        RecordEvent(TEXT("updated"), D.GetSoftObjectPath().ToString());
    });
    bSubscribed = true;
}

void UAssetEventsHandler::Unsubscribe()
{
    if (!bSubscribed) return;
    FAssetRegistryModule* M = FModuleManager::GetModulePtr<FAssetRegistryModule>("AssetRegistry");
    if (M)
    {
        IAssetRegistry& AR = M->Get();
        if (AddedHandle.IsValid())   AR.OnAssetAdded().Remove(AddedHandle);
        if (RemovedHandle.IsValid()) AR.OnAssetRemoved().Remove(RemovedHandle);
        if (RenamedHandle.IsValid()) AR.OnAssetRenamed().Remove(RenamedHandle);
        if (UpdatedHandle.IsValid()) AR.OnAssetUpdated().Remove(UpdatedHandle);
    }
    AddedHandle.Reset(); RemovedHandle.Reset(); RenamedHandle.Reset(); UpdatedHandle.Reset();
    bSubscribed = false;
}

void UAssetEventsHandler::RecordEvent(const FString& Kind, const FString& Path, const FString& OldPath)
{
    FEventEntry E;
    E.Id = NextId++;
    E.Kind = Kind;
    E.AssetPath = Path;
    E.OldPath = OldPath;
    E.Time = FDateTime::UtcNow();
    Events.Add(MoveTemp(E));
    if (Events.Num() > MaxEvents)
    {
        Events.RemoveAt(0, Events.Num() - MaxEvents);
    }
}

FBridgeResult UAssetEventsHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("enable_subscriptions"))   return Action_Enable(Params);
    if (Action == TEXT("disable_subscriptions"))  return Action_Disable(Params);
    if (Action == TEXT("get_recent_events"))      return Action_GetRecent(Params);
    if (Action == TEXT("get_event_count"))        return Action_GetCount(Params);
    if (Action == TEXT("clear_event_log"))        return Action_ClearLog(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("enable_subscriptions, disable_subscriptions, get_recent_events, get_event_count, clear_event_log"));
}

FBridgeResult UAssetEventsHandler::Action_Enable(TSharedPtr<FJsonObject> Params)
{
    Subscribe();
    return MakeSuccess(DOMAIN, TEXT("enable_subscriptions"),
        bSubscribed ? TEXT("Subscribed to asset registry events") : TEXT("Subscribe failed"));
}

FBridgeResult UAssetEventsHandler::Action_Disable(TSharedPtr<FJsonObject> Params)
{
    Unsubscribe();
    return MakeSuccess(DOMAIN, TEXT("disable_subscriptions"), TEXT("Unsubscribed"));
}

FBridgeResult UAssetEventsHandler::Action_GetRecent(TSharedPtr<FJsonObject> Params)
{
    int32 Limit = 100;
    int64 Since = 0;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    if (TSharedPtr<FJsonValue> V = Params->TryGetField(TEXT("since")))
    {
        Since = (int64)V->AsNumber();
    }

    TArray<TSharedPtr<FJsonValue>> Arr;
    int32 Emitted = 0;
    for (int32 i = Events.Num() - 1; i >= 0 && Emitted < Limit; --i)
    {
        const FEventEntry& E = Events[i];
        if (E.Id <= Since) break;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), E.Id);
        O->SetStringField(TEXT("kind"), E.Kind);
        O->SetStringField(TEXT("asset_path"), E.AssetPath);
        if (!E.OldPath.IsEmpty()) O->SetStringField(TEXT("old_path"), E.OldPath);
        O->SetStringField(TEXT("time"), E.Time.ToIso8601());
        Arr.Add(MakeShared<FJsonValueObject>(O));
        ++Emitted;
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("events"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Data->SetBoolField(TEXT("subscribed"), bSubscribed);
    return MakeSuccess(DOMAIN, TEXT("get_recent_events"),
        FString::Printf(TEXT("%d event(s)"), Arr.Num()), Data);
}

FBridgeResult UAssetEventsHandler::Action_GetCount(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("total_recorded"), Events.Num());
    Data->SetNumberField(TEXT("next_id"), NextId);
    Data->SetBoolField(TEXT("subscribed"), bSubscribed);
    return MakeSuccess(DOMAIN, TEXT("get_event_count"),
        FString::Printf(TEXT("%d event(s) buffered"), Events.Num()), Data);
}

FBridgeResult UAssetEventsHandler::Action_ClearLog(TSharedPtr<FJsonObject> Params)
{
    const int32 N = Events.Num();
    Events.Empty();
    return MakeSuccess(DOMAIN, TEXT("clear_event_log"),
        FString::Printf(TEXT("Cleared %d event(s)"), N));
}
