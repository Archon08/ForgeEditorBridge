#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "WorldSettingsHandler.generated.h"

/**
 * WorldSettingsHandler — domain "world_settings"
 *
 * Read and modify AWorldSettings properties.
 *
 * Actions:
 *   get_world_settings → returns game_mode, kill_z, gravity, lightmass, etc.
 *   set_game_mode      → game_mode_class
 *   set_kill_z         → kill_z (float)
 *   set_global_gravity → gravity_z (float)
 *   set_lightmass      → num_bounces?, quality?, smoothness?, static_level_scale?, environment_color?
 *   set_world_property → property (name), value (generic reflection setter)
 *   set_default_map    → map_path (writes to DefaultEngine.ini)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UWorldSettingsHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("world_settings"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("get_world_settings"), TEXT("set_game_mode"), TEXT("set_kill_z"), TEXT("set_global_gravity"), TEXT("set_lightmass"), TEXT("set_world_property"), TEXT("set_default_map"), TEXT("set_world_settings") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
