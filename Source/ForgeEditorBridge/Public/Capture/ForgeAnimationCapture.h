#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeAnimationCapture.generated.h"

/**
 * v1.9 — Animation & Bone Capture
 *
 * Exports skeletal mesh structure, bone hierarchies, sockets, skin weight profiles,
 * Motion Matching Pose Search databases, Chooser tables, IK Rig bindings, and
 * GPU Deformer Graph chains for all skeletal actors in the editor level.
 *
 * Output: {ProjectRoot}/Forge/ue-context/animation/skeletal_data.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.animation_capture.export_animation_data()
 *
 * Exported JSON fields:
 *   generated, level_name,
 *   skeletal_meshes[]{
 *     actor_name, component_name, mesh_asset, skeleton_asset,
 *     bone_count, bones_truncated,
 *     bones[]{name, parent_index, local_transform{location,rotation,scale}},
 *     socket_count, sockets[]{name, parent_bone, relative_transform},
 *     skin_weight_profiles[],
 *     ik_rig{found, asset, solvers[]},
 *     deformer{found, asset, class_name, kernel_names[]}
 *   },
 *   pose_search_databases[]{asset, schema, is_indexed, pose_count},
 *   choosers[]{asset, result_type, row_count},
 *   audit{total_issues, issues[]{issue_type, severity, detail}}
 *
 * Token efficiency: bone list capped at 100 per mesh (bones_truncated flag set if exceeded).
 * Raw pose matrices are never exported — structural metadata only.
 *
 * Audit rules (3):
 *   MISSING_IK_RIG       — Error: skeletal actor has no IKRigComponent (breaks foot IK / retargeting)
 *   STALE_POSE_INDEX     — Warning: PoseSearchDatabase not indexed (Motion Matching will brute-force)
 *   DEEP_BONE_HIERARCHY  — Warning: mesh has 200+ bones (elevated skinning cost)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeAnimationCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Capture skeletal mesh / animation state from the editor level.
	 * Writes animation/skeletal_data.json.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportAnimationData();

private:
	FString OutputDir;

	// READ-MERGE-WRITE index.json to add/update the "animation" section
	void UpdateIndexFile();
};
