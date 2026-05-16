#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "SkeletalMeshHandler.generated.h"

/**
 * SkeletalMeshHandler — domain "skeletal_mesh"  (Phase 4 / UE 5.7)
 *
 * Actions:
 *   set_material_slot     → asset_path (string), slot_index (int), material (string)
 *   get_bones             → asset_path (string) → JSON array of bone data
 *   get_morph_targets     → asset_path (string) → JSON array of morph target names
 *   import_skeletal_mesh  → source_file (string), dest_path (string), skeleton (string, optional)
 *   reimport              → asset_path (string)
 *   get_info              → asset_path (string) → bone_count, morph_target_count, etc.
 */
UCLASS()
class FORGEEDITORBRIDGE_API USkeletalMeshHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("skeletal_mesh"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("set_material_slot"), TEXT("get_bones"), TEXT("get_morph_targets"), TEXT("import_skeletal_mesh"), TEXT("reimport"), TEXT("get_info") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_SetMaterialSlot    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetBones           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMorphTargets    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportSkeletalMesh (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Reimport           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetInfo            (TSharedPtr<FJsonObject> Params);

	/** Load a USkeletalMesh from a content path. Returns nullptr on failure. */
	class USkeletalMesh* LoadSkeletalMesh(const FString& AssetPath, const FString& ActionName, FBridgeResult& OutResult);
};
