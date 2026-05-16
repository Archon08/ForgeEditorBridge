#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "StaticMeshHandler.generated.h"

/**
 * StaticMeshHandler — domain "static_mesh"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   set_nanite          → asset_path (string), enabled (bool)
 *
 *   set_lod_screen_size → asset_path (string), lod_index (int),
 *                         screen_size (float 0–1, fraction of viewport at which LOD becomes active)
 *
 *   set_material_slot   → asset_path (string), slot_index (int), material_path (string)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UStaticMeshHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("static_mesh"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("set_nanite"), TEXT("set_lod_screen_size"), TEXT("set_material_slot"), TEXT("get_info"), TEXT("set_collision") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_SetNanite       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLODScreenSize(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMaterialSlot (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetInfo         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetCollision    (TSharedPtr<FJsonObject> Params);

	/** Load a UStaticMesh from a content path. Populates Result.Message on failure and returns nullptr. */
	class UStaticMesh* LoadStaticMesh(const FString& AssetPath, FBridgeResult& Result);
};
