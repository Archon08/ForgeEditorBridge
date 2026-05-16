#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "GameFeaturesHandler.generated.h"

/**
 * GameFeaturesHandler — domain "gamefeatures"  (UE 5.7)
 *
 * Wraps UGameFeaturesSubsystem — the runtime activate/deactivate API for
 * GameFeature plugins. ProjectHandler::enable_plugin only flips the
 * .uproject reference; this domain does the actual register/load/activate
 * lifecycle.
 *
 * Plugin URLs may be the plugin name (e.g. "MyFeature") or the full
 * "PluginName.uplugin" path; the subsystem normalizes.
 *
 * Actions:
 *   register_plugin       → plugin_name
 *   load_plugin           → plugin_name (loads but does not activate)
 *   activate_plugin       → plugin_name (registers + loads + activates)
 *   deactivate_plugin     → plugin_name
 *   unload_plugin         → plugin_name (keep_registered? bool)
 *   unregister_plugin     → plugin_name
 *   get_plugin_state      → plugin_name → returns state name
 *   list_game_features    → returns array of {name, state}
 *   is_active             → plugin_name → returns bool
 */
UCLASS()
class FORGEEDITORBRIDGE_API UGameFeaturesHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("gamefeatures"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("register_plugin"), TEXT("load_plugin"), TEXT("activate_plugin"),
            TEXT("deactivate_plugin"), TEXT("unload_plugin"), TEXT("unregister_plugin"),
            TEXT("get_plugin_state"), TEXT("list_game_features"), TEXT("is_active")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_RegisterPlugin    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_LoadPlugin        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ActivatePlugin    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DeactivatePlugin  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_UnloadPlugin      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_UnregisterPlugin  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetPluginState    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListGameFeatures  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_IsActive          (TSharedPtr<FJsonObject> Params);
};
