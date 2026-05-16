#include "Handlers/OnlineServicesHandler.h"
#include "Modules/ModuleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("online");

namespace
{
    bool IsModLoaded(const FString& Name)
    {
        return FModuleManager::Get().IsModuleLoaded(*Name);
    }
}

FBridgeResult UOnlineServicesHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("is_online_subsystem_loaded")) return Action_IsLoaded(Params);
    if (Action == TEXT("get_subsystem_info"))         return Action_GetSubsystemInfo(Params);
    if (Action == TEXT("list_loaded_modules"))        return Action_ListLoadedModules(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("is_online_subsystem_loaded, get_subsystem_info, list_loaded_modules"));
}

FBridgeResult UOnlineServicesHandler::Action_IsLoaded(TSharedPtr<FJsonObject> Params)
{
    const bool bOSS  = IsModLoaded(TEXT("OnlineSubsystem"));
    const bool bOSS2 = IsModLoaded(TEXT("OnlineServices"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("online_subsystem"), bOSS);
    Data->SetBoolField(TEXT("online_services_v2"), bOSS2);
    return MakeSuccess(DOMAIN, TEXT("is_online_subsystem_loaded"),
        FString::Printf(TEXT("OSS=%s OSS2=%s"),
            bOSS ? TEXT("yes") : TEXT("no"),
            bOSS2 ? TEXT("yes") : TEXT("no")),
        Data);
}

FBridgeResult UOnlineServicesHandler::Action_GetSubsystemInfo(TSharedPtr<FJsonObject> Params)
{
    FString DefaultPlatformService;
    GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), DefaultPlatformService, GEngineIni);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("default_platform_service"), DefaultPlatformService);
    Data->SetStringField(TEXT("config_file"), GEngineIni);
    return MakeSuccess(DOMAIN, TEXT("get_subsystem_info"),
        DefaultPlatformService.IsEmpty() ? TEXT("(none)") : DefaultPlatformService, Data);
}

FBridgeResult UOnlineServicesHandler::Action_ListLoadedModules(TSharedPtr<FJsonObject> Params)
{
    static const TArray<FString> Candidates = {
        TEXT("OnlineSubsystem"), TEXT("OnlineSubsystemUtils"), TEXT("OnlineSubsystemEOS"),
        TEXT("OnlineSubsystemSteam"), TEXT("OnlineSubsystemNull"), TEXT("OnlineSubsystemEpic"),
        TEXT("OnlineServices"), TEXT("OnlineServicesEOS"), TEXT("OnlineServicesEpic"),
        TEXT("OnlineServicesNull"), TEXT("OnlineServicesEOSGS"),
        TEXT("EOSShared"), TEXT("EOSSDK")
    };
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FString& Name : Candidates)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("module"), Name);
        O->SetBoolField(TEXT("loaded"), IsModLoaded(Name));
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("modules"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_loaded_modules"),
        FString::Printf(TEXT("Checked %d candidate online module(s)"), Arr.Num()), Data);
}
