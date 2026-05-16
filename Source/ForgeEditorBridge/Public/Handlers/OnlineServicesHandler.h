#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "OnlineServicesHandler.generated.h"

/**
 * OnlineServicesHandler — domain "online"  (UE 5.7)
 *
 * Thin presence detection for OnlineSubsystem / Online Services 2.0. The
 * full surface is provider-specific (EOS / Steam / Null) and very large;
 * this handler reports availability and exposes the configured subsystem
 * name. Authoring/login flows go through provider-specific Python or C++.
 *
 * Actions:
 *   is_online_subsystem_loaded → bool + subsystem name
 *   get_subsystem_info         → returns identifier and instance name
 *   list_loaded_modules        → enumerates loaded online-related modules
 */
UCLASS()
class FORGEEDITORBRIDGE_API UOnlineServicesHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("online"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("is_online_subsystem_loaded"), TEXT("get_subsystem_info"),
            TEXT("list_loaded_modules")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_IsLoaded          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetSubsystemInfo  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListLoadedModules (TSharedPtr<FJsonObject> Params);
};
