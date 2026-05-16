#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "PhysicsAssetHandler.generated.h"

/**
 * PhysicsAssetHandler — domain "physics_asset"  (v0.9.0 / UE 5.7)
 *
 * Authors ragdoll bodies and constraints on a UPhysicsAsset through the Bridge.
 *
 * Actions:
 *   create_physics_asset → asset_path (string),
 *                          skeletal_mesh_path (string, optional — seeds the asset with the mesh's skeleton)
 *
 *   add_body             → asset_path (string), bone_name (string),
 *                          shape_type ("sphere"|"capsule"|"box"),
 *                          radius (float, sphere & capsule),
 *                          height (float, capsule only — length of the cylinder section),
 *                          half_extents_x/y/z (float, box only — half-size per axis)
 *
 *   set_body_params      → asset_path (string), bone_name (string),
 *                          simulate (bool, optional — PhysType_Simulated vs PhysType_Kinematic),
 *                          collision_profile (string, optional — e.g. "Ragdoll", "BlockAll")
 *
 *   add_constraint       → asset_path (string), bone1 (string), bone2 (string),
 *                          type ("ball"|"hinge"|"prismatic"|"free", default "ball"),
 *                          swing1 (float, degrees), swing2 (float, degrees), twist (float, degrees)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPhysicsAssetHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("physics_asset"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_physics_asset"), TEXT("add_body"), TEXT("set_body_params"), TEXT("add_constraint"), TEXT("get_info"), TEXT("list_bodies"), TEXT("auto_generate_bodies"), TEXT("set_constraint_profile"), TEXT("remove_body"), TEXT("remove_constraint") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreatePhysicsAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddBody           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetBodyParams     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddConstraint     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetInfo              (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListBodies           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AutoGenerateBodies   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetConstraintProfile (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveBody           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveConstraint     (TSharedPtr<FJsonObject> Params);

	/** Load a UPhysicsAsset from a content path. Populates Result.Message on failure and returns nullptr. */
	class UPhysicsAsset* LoadPhysicsAsset(const FString& AssetPath, FBridgeResult& Result);
};
