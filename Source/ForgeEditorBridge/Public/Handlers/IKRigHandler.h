#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "IKRigHandler.generated.h"

/**
 * IKRigHandler — domain "ikrig"  (UE 5.7)
 *
 * Authoring for UIKRigDefinition. Sibling to the existing IKRetargeterHandler
 * (which only consumes pre-existing rigs). Solver/goal/effector edits go via
 * the asset's controller — but to avoid the full IK Rig editor surface,
 * this handler covers the lifecycle and pre-built solver/goal arrays.
 *
 * Actions:
 *   create_ik_rig         → asset_path, skeletal_mesh_path?
 *   set_skeletal_mesh     → asset_path, skeletal_mesh_path
 *   add_chain             → asset_path, chain_name, start_bone, end_bone, goal_name?
 *   list_chains           → asset_path
 *   add_goal              → asset_path, goal_name, bone_name
 *   list_goals            → asset_path
 *   get_info              → asset_path → returns chain count, goal count, mesh path
 */
UCLASS()
class FORGEEDITORBRIDGE_API UIKRigHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("ikrig"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_ik_rig"), TEXT("set_skeletal_mesh"),
            TEXT("add_chain"), TEXT("list_chains"),
            TEXT("add_goal"), TEXT("list_goals"),
            TEXT("get_info")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateIKRig       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetSkeletalMesh   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddChain          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListChains        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddGoal           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListGoals         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetInfo           (TSharedPtr<FJsonObject> Params);
};
