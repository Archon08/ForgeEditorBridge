#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "SkeletonHandler.generated.h"

/**
 * SkeletonHandler — domain "skeleton"  (v0.9.0 / UE 5.7)
 *
 * Manages sockets and virtual bones on a USkeleton through the Bridge.
 *
 * Actions:
 *   add_socket       → asset_path (string), socket_name (string), bone_name (string),
 *                      x/y/z (float, optional — relative location, default 0),
 *                      pitch/yaw/roll (float, optional — relative rotation degrees, default 0),
 *                      scale (float, optional — uniform scale, default 1.0)
 *
 *   move_socket      → asset_path (string), socket_name (string),
 *                      x/y/z (float, optional — new relative location),
 *                      pitch/yaw/roll (float, optional — new relative rotation degrees),
 *                      scale (float, optional — new uniform scale)
 *                      (omit any group to leave it unchanged)
 *
 *   add_virtual_bone → asset_path (string), source_bone (string), target_bone (string)
 *                      Creates a virtual bone from source_bone to target_bone.
 *                      Returns the generated virtual bone name in Message.
 *
 *   get_sockets      → asset_path (string)
 *                      Returns all sockets: name, bone, relative location/rotation/scale.
 *
 *   list_virtual_bones → asset_path (string)
 *                        Returns all virtual bones: name, source_bone, target_bone.
 *
 *   get_skeleton_info → asset_path (string)
 *                       Returns bone count, virtual bone count, socket count, ref pose bone list.
 */
UCLASS()
class FORGEEDITORBRIDGE_API USkeletonHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("skeleton"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("add_socket"), TEXT("move_socket"), TEXT("add_virtual_bone"), TEXT("get_sockets"), TEXT("list_virtual_bones"), TEXT("get_skeleton_info"), TEXT("remove_socket"), TEXT("remove_virtual_bone") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_AddSocket        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_MoveSocket       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddVirtualBone   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetSockets       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListVirtualBones (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetSkeletonInfo  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveSocket     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveVirtualBone(TSharedPtr<FJsonObject> Params);

	/** Load a USkeleton from a content path. Populates Result.Message on failure and returns nullptr. */
	class USkeleton* LoadSkeleton(const FString& AssetPath, FBridgeResult& Result);
};
