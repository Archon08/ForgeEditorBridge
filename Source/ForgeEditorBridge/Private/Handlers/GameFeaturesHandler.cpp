#include "Handlers/GameFeaturesHandler.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"   // EGameFeaturePluginState public alias + UE::GameFeatures::ToString
#include "GameFeaturesProjectPolicies.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/Engine.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("gamefeatures");

namespace
{
    UGameFeaturesSubsystem* GetGFS()
    {
        return GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
    }

    /** Resolve a plugin name to its plugin URL (the path the subsystem expects). */
    FString ResolvePluginURL(const FString& PluginName)
    {
        if (!PluginName.Contains(TEXT(".uplugin")) && !PluginName.Contains(TEXT("/")))
        {
            // Bare plugin name — let GFS build the URL
            UGameFeaturesSubsystem* GFS = GetGFS();
            FString URL;
            if (GFS && GFS->GetPluginURLByName(PluginName, URL))
            {
                return URL;
            }
        }
        return PluginName;
    }

    // RequireName preserved with the original 4-arg signature so call sites stay unchanged.
    // MakeError is protected on UBridgeHandlerBase and can't be called from a free
    // function — build the error result inline instead.
    bool RequireName(const TSharedPtr<FJsonObject>& Params, const FString& Action,
                     FString& OutName, FBridgeResult& OutErr)
    {
        if (!Params->TryGetStringField(TEXT("plugin_name"), OutName) || OutName.IsEmpty())
        {
            OutErr = FBridgeResult{};
            OutErr.bSuccess = false;
            OutErr.Domain = DOMAIN;
            OutErr.Action = Action;
            OutErr.ErrorCode = 1000;
            OutErr.Message = TEXT("'plugin_name' is required");
            return false;
        }
        return true;
    }
}

FBridgeResult UGameFeaturesHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("register_plugin"))     return Action_RegisterPlugin(Params);
    if (Action == TEXT("load_plugin"))         return Action_LoadPlugin(Params);
    if (Action == TEXT("activate_plugin"))     return Action_ActivatePlugin(Params);
    if (Action == TEXT("deactivate_plugin"))   return Action_DeactivatePlugin(Params);
    if (Action == TEXT("unload_plugin"))       return Action_UnloadPlugin(Params);
    if (Action == TEXT("unregister_plugin"))   return Action_UnregisterPlugin(Params);
    if (Action == TEXT("get_plugin_state"))    return Action_GetPluginState(Params);
    if (Action == TEXT("list_game_features"))  return Action_ListGameFeatures(Params);
    if (Action == TEXT("is_active"))           return Action_IsActive(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("register_plugin, load_plugin, activate_plugin, deactivate_plugin, unload_plugin, unregister_plugin, get_plugin_state, list_game_features, is_active"));
}

FBridgeResult UGameFeaturesHandler::Action_RegisterPlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("register_plugin"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("register_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->LoadGameFeaturePlugin(URL, FGameFeaturePluginLoadComplete());
    return MakeSuccess(DOMAIN, TEXT("register_plugin"),
        FString::Printf(TEXT("Register/load requested for '%s'"), *Name));
}

FBridgeResult UGameFeaturesHandler::Action_LoadPlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("load_plugin"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("load_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->LoadGameFeaturePlugin(URL, FGameFeaturePluginLoadComplete());
    return MakeSuccess(DOMAIN, TEXT("load_plugin"),
        FString::Printf(TEXT("Load requested for '%s'"), *Name));
}

FBridgeResult UGameFeaturesHandler::Action_ActivatePlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("activate_plugin"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("activate_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->LoadAndActivateGameFeaturePlugin(URL, FGameFeaturePluginLoadComplete());
    return MakeSuccess(DOMAIN, TEXT("activate_plugin"),
        FString::Printf(TEXT("Activate requested for '%s'"), *Name));
}

FBridgeResult UGameFeaturesHandler::Action_DeactivatePlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("deactivate_plugin"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("deactivate_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->DeactivateGameFeaturePlugin(URL);
    return MakeSuccess(DOMAIN, TEXT("deactivate_plugin"),
        FString::Printf(TEXT("Deactivate requested for '%s'"), *Name));
}

FBridgeResult UGameFeaturesHandler::Action_UnloadPlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("unload_plugin"), Name, Err)) return Err;
    bool bKeepRegistered = false;
    Params->TryGetBoolField(TEXT("keep_registered"), bKeepRegistered);
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("unload_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->UnloadGameFeaturePlugin(URL, bKeepRegistered);
    return MakeSuccess(DOMAIN, TEXT("unload_plugin"),
        FString::Printf(TEXT("Unload requested for '%s' (keep_registered=%s)"),
            *Name, bKeepRegistered ? TEXT("true") : TEXT("false")));
}

FBridgeResult UGameFeaturesHandler::Action_UnregisterPlugin(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("unregister_plugin"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("unregister_plugin"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    GFS->UnloadGameFeaturePlugin(URL, /*bKeepRegistered=*/false);
    return MakeSuccess(DOMAIN, TEXT("unregister_plugin"),
        FString::Printf(TEXT("Unregister requested for '%s'"), *Name));
}

FBridgeResult UGameFeaturesHandler::Action_GetPluginState(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("get_plugin_state"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("get_plugin_state"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    const EGameFeaturePluginState State = GFS->GetPluginState(URL);
    const FString StateName = UE::GameFeatures::ToString(State);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("plugin_name"), Name);
    Data->SetStringField(TEXT("url"), URL);
    Data->SetStringField(TEXT("state"), StateName);
    return MakeSuccess(DOMAIN, TEXT("get_plugin_state"), StateName, Data);
}

FBridgeResult UGameFeaturesHandler::Action_ListGameFeatures(TSharedPtr<FJsonObject> Params)
{
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("list_game_features"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));

    TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetEnabledPlugins();
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TSharedRef<IPlugin>& P : AllPlugins)
    {
        const FString Name = P->GetName();
        FString URL;
        if (!GFS->GetPluginURLByName(Name, URL)) continue;
        const EGameFeaturePluginState State = GFS->GetPluginState(URL);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        Entry->SetStringField(TEXT("url"), URL);
        Entry->SetStringField(TEXT("state"), UE::GameFeatures::ToString(State));
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("game_features"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_game_features"),
        FString::Printf(TEXT("%d GameFeature plugin(s)"), Arr.Num()), Data);
}

FBridgeResult UGameFeaturesHandler::Action_IsActive(TSharedPtr<FJsonObject> Params)
{
    FString Name; FBridgeResult Err;
    if (!RequireName(Params, TEXT("is_active"), Name, Err)) return Err;
    UGameFeaturesSubsystem* GFS = GetGFS();
    if (!GFS) return MakeError(DOMAIN, TEXT("is_active"), 3000, TEXT("UGameFeaturesSubsystem unavailable"));
    const FString URL = ResolvePluginURL(Name);
    const EGameFeaturePluginState State = GFS->GetPluginState(URL);
    const bool bActive = (State == EGameFeaturePluginState::Active);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("active"), bActive);
    Data->SetStringField(TEXT("state"), UE::GameFeatures::ToString(State));
    return MakeSuccess(DOMAIN, TEXT("is_active"),
        bActive ? TEXT("active") : TEXT("inactive"), Data);
}
