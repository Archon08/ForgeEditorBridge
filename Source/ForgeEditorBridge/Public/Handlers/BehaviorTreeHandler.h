#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "BehaviorTreeHandler.generated.h"

class UBehaviorTree;
class UBTCompositeNode;
class UBTNode;

/**
 * BehaviorTreeHandler — domain "behavior_tree"  (v0.11.0 / UE 5.7)
 *
 * Actions:
 *   create_bt      → asset_path (string, e.g. "/Game/AI/BT_NPC")
 *
 *   add_node       → bt_path (string),
 *                    node_type ("selector"|"sequence"|"wait"|"move_to"),
 *                    parent_index (int — index into BTGraph->Nodes; -1 = auto-find root),
 *                    pos_x (float), pos_y (float)
 *                    Returns new node index in message.
 *
 *   add_decorator  → bt_path (string),
 *                    node_index (int — index in BTGraph->Nodes),
 *                    decorator_type ("blackboard"|"loop"|"force_success"),
 *                    bb_key (string — key name, used when decorator_type="blackboard")
 *
 *   create_bb      → asset_path (string, e.g. "/Game/AI/BB_NPC")
 *
 *   add_key        → bb_path (string), key_name (string),
 *                    key_type ("bool"|"float"|"int"|"string"|"vector"|"object")
 *
 *   add_service    → asset_path (string), parent_node_index (int),
 *                    service_class (string — full class path, e.g. "BTService_DefaultFocus")
 *                    Attaches a UBTService to a composite node.
 *
 *   set_node_property → asset_path (string), node_index (int — DFS index),
 *                       property (string), value (string)
 *                       Sets a property on a BT node via FProperty reflection.
 *
 *   link_blackboard   → asset_path (string), blackboard_path (string)
 *                       Associates a UBlackboardData asset with a BehaviorTree.
 *
 *   get_tree_topology → asset_path (string)
 *                       Returns the full BT structure as nested JSON: nodes, decorators, services.
 *
 *   add_task_node     → asset_path (string, BehaviorTree path),
 *                       task_class (string, e.g. "BTTask_BlueprintBase" or full path),
 *                       node_label (string, optional display name)
 *                       Adds a BTTask leaf node to the tree's root composite.
 *
 *   set_blackboard_key_default → asset_path (string, BlackboardData path),
 *                                key_name (string),
 *                                default_value (string)
 *                                Sets a simple default on a blackboard key (bool/float/int/string/name supported).
 *
 *   set_service_interval → asset_path (string, BehaviorTree path),
 *                          service_name (string, display or class name),
 *                          interval (float),
 *                          random_deviation (float, optional, default 0)
 *                          Sets Interval and RandomDeviation on a matching UBTService in the tree.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UBehaviorTreeHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("behavior_tree"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_bt"), TEXT("add_node"), TEXT("add_decorator"), TEXT("create_bb"), TEXT("create_blackboard"), TEXT("add_key"), TEXT("add_blackboard_key"), TEXT("add_service"), TEXT("set_node_property"), TEXT("link_blackboard"), TEXT("get_tree_topology"), TEXT("add_task_node"), TEXT("set_blackboard_key_default"), TEXT("set_service_interval"), TEXT("remove_node"), TEXT("remove_decorator"), TEXT("remove_service"), TEXT("remove_blackboard_key") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateBT    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddNode     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddDecorator(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateBB        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddKey          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddService      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetNodeProperty (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_LinkBlackboard  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetTreeTopology      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddTaskNode          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetBlackboardKeyDefault(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetServiceInterval   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveNode           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveDecorator      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveService        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveBlackboardKey  (TSharedPtr<FJsonObject> Params);

	/** Helper: Load a UBehaviorTree asset, trying both path and path.assetname forms. */
	UBehaviorTree* LoadBTAsset(const FString& AssetPath, FBridgeResult& Result);

	/** Helper: DFS-traverse the BT tree and collect all nodes in order; returns index-accessible array. */
	void CollectNodesDFS(UBTCompositeNode* Node, TArray<UBTNode*>& OutNodes);

	/** Helper: Build topology JSON for a composite node (recursive). */
	TSharedPtr<FJsonObject> BuildNodeJson(UBTCompositeNode* CompNode);
};
