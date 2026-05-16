#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ChaosHandler.generated.h"

/**
 * ChaosHandler — domain "chaos"  (v0.12.0 / Wave K)
 *
 * Real-time destruction authoring using Chaos physics (Geometry Collections).
 *
 * Actions:
 *   fracture     → asset_path (string), source_mesh (string, optional),
 *                  num_pieces (int, default 10), seed (int, optional)
 *                  Creates or opens a UGeometryCollection and applies Voronoi fracture.
 *
 *   spawn_field  → field_type (string: "radial"|"uniform"|"plane_fall_off"),
 *                  location (string, "X,Y,Z"), magnitude (float, default 1000),
 *                  radius (float, default 500)
 *                  Spawns a physics field actor in the current level.
 *
 *   list_geometry_collections → package_path (string, optional — filter path prefix, e.g. "/Game/Destruction")
 *                               Lists all UGeometryCollection assets in the project.
 *
 *   get_fracture_info → asset_path (string) — returns transform count, geometry cluster count, and
 *                       initial dynamic state of the specified UGeometryCollection.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UChaosHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("chaos"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("fracture"), TEXT("spawn_field"), TEXT("list_geometry_collections"), TEXT("get_fracture_info"), TEXT("set_physics_field"), TEXT("set_geometry_collection_settings"), TEXT("set_material_damage") }; }
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;

private:
	FBridgeResult Action_Fracture                        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SpawnField                      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListGeometryCollections         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetFractureInfo                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPhysicsField                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetGeometryCollectionSettings   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMaterialDamage               (TSharedPtr<FJsonObject> Params);
};
