#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "WaterHandler.generated.h"

/**
 * WaterHandler — domain "water"
 *
 * Water body creation and configuration.
 *
 * Actions:
 *   create_water_body  → type ("lake"|"river"|"ocean"), location {x,y,z}, size {x,y}?
 *   set_water_material → actor_path (label lookup), material (asset path)
 *   configure_waves    → actor_path, wave_amplitude, wave_length, wave_speed
 *   get_water_info     → actor_path — returns type, material, wave_amplitude, location, scale
 */
UCLASS()
class FORGEEDITORBRIDGE_API UWaterHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("water"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_water_body"), TEXT("set_water_material"), TEXT("configure_waves"), TEXT("get_water_info") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetWaterInfo(TSharedPtr<FJsonObject> Params);
};
