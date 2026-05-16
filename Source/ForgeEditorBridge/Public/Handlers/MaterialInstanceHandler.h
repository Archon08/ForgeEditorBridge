#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "MaterialInstanceHandler.generated.h"

/**
 * MaterialInstanceHandler — domain "material_instance"  (v0.8.0 / UE 5.7)
 *
 * Creates and edits UMaterialInstanceConstant assets through the Bridge.
 *
 * Actions:
 *   create_instance  → asset_path (string), parent_path (string)
 *                      Creates a new UMaterialInstanceConstant parented to the specified material.
 *
 *   set_scalar       → asset_path (string), param_name (string), value (float)
 *                      Override a scalar parameter on the MIC.
 *
 *   set_vector       → asset_path (string), param_name (string),
 *                      r (float), g (float), b (float), a (float, optional — default 1.0)
 *                      Override a vector/color parameter on the MIC.
 *
 *   set_texture      → asset_path (string), param_name (string), texture_path (string)
 *                      Override a texture parameter on the MIC.
 *
 *   set_switch       → asset_path (string), param_name (string), value (bool)
 *                      Override a static-switch parameter on the MIC (rebuilds static permutation).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMaterialInstanceHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("material_instance"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_instance"), TEXT("set_scalar"), TEXT("set_vector"), TEXT("set_texture"), TEXT("set_switch"), TEXT("get_parameters"), TEXT("get_parent") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateInstance(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetScalar     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetVector     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetTexture    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetSwitch     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetParameters (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetParent     (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a UMaterialInstanceConstant from a content path.
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UMaterialInstanceConstant* LoadMIC(const FString& AssetPath, FBridgeResult& Result);
};
