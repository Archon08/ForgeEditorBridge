#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ClothLODHandler.generated.h"

/**
 * ClothLODHandler — domain "cloth_lod"  (v0.9.0 / UE 5.7)
 *
 * Manages clothing simulation binding and skeletal mesh LOD screen sizes through the Bridge.
 *
 * Actions:
 *   bind_cloth         → mesh_path (string),
 *                        cloth_asset_path (string — content path to a UClothingAssetBase),
 *                        lod_index (int, default 0),
 *                        section_index (int, default 0)
 *                        Binds an existing clothing asset to a skeletal mesh LOD section.
 *
 *   set_cloth_config   → cloth_asset_path (string),
 *                        property (string — UProperty name on the cloth config object),
 *                        value (string — value to import via FProperty::ImportText)
 *                        Sets a named property on the cloth asset's config via reflection,
 *                        making it forward-compatible with any UChaosClothConfig field.
 *
 *   set_lod_screen_size → mesh_path (string), lod_index (int), screen_size (float 0..1)
 *                         Sets the LOD screen-size threshold on FSkeletalMeshLODInfo.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UClothLODHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("cloth_lod"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("bind_cloth"), TEXT("set_cloth_config"), TEXT("set_lod_screen_size") }; }
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_BindCloth        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetClothConfig   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLODScreenSize (TSharedPtr<FJsonObject> Params);
};
