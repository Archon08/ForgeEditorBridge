#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "NavMeshHandler.generated.h"

/**
 * NavMeshHandler — domain "navmesh"  (v0.11.0 / UE 5.7)
 *
 * Actions:
 *   set_agent_params  → label (string — actor label of ARecastNavMesh;
 *                               default "RecastNavMesh-Default"; pass "first" for any),
 *                       agent_radius (float), agent_height (float),
 *                       max_step_height (float), max_slope (float, degrees)
 *                       All param fields are optional — only supplied fields are written.
 *
 *   create_nav_volume → x, y, z (float — world center),
 *                       extent_x, extent_y, extent_z (float — half-extents),
 *                       label (string, optional actor label)
 *
 *   rebuild_nav       → (no required params — rebuilds nav in the current editor world)
 *
 *   query_path        → start {x,y,z}, end {x,y,z},
 *                       agent_radius (float, optional — override for this query),
 *                       Returns path_exists (bool), path_length (float), path_cost (float),
 *                       waypoints (array of {x,y,z}), and a text description.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UNavMeshHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("navmesh"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("set_agent_params"), TEXT("create_nav_volume"), TEXT("rebuild_nav"), TEXT("query_path"), TEXT("add_nav_link"), TEXT("set_jump_config"), TEXT("get_reachable_area") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_SetAgentParams    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateNavVolume   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RebuildNav        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_QueryPath         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddNavLink        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetJumpConfig     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetReachableArea  (TSharedPtr<FJsonObject> Params);
};
