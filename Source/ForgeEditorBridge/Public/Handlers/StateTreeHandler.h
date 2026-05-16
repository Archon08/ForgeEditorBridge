#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "StateTreeHandler.generated.h"

/**
 * StateTreeHandler — domain "state_tree"  (v0.2.6 / UE 5.7)
 *
 * Creates and authors UStateTree assets (Production Ready in UE 5.7).
 * Used for both AI and non-AI logic via the State Tree system.
 *
 * Actions:
 *   create_state_tree → asset_path (string), context_class (string, optional)
 *                       Creates a new UStateTree asset via UStateTreeFactory.
 *
 *   add_state         → asset_path (string), state_name (string), parent_state (string, optional)
 *                       Adds a child state to the root (or named parent state).
 *
 *   add_transition    → asset_path (string), from_state (string),
 *                       trigger ("OnStateCompleted"|"OnEvent"|"Unconditional"),
 *                       to_state (string), event_tag (string, optional for OnEvent)
 *                       Adds a transition on a named state.
 *
 *   set_state_tasks   → asset_path (string), state_name (string),
 *                       tasks (array of {class_path: string})
 *                       Binds task instances to a state by class path.
 *
 *   add_evaluator     → asset_path (string), evaluator_class (string)
 *                       Appends an evaluator to the StateTree's global evaluator list.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UStateTreeHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("state_tree"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return { TEXT("create_state_tree"), TEXT("add_state"), TEXT("add_transition"),
		         TEXT("set_state_tasks"), TEXT("add_evaluator"),
		         TEXT("remove_state"), TEXT("remove_transition"), TEXT("remove_evaluator") };
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateStateTree (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddState        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddTransition   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetStateTasks   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddEvaluator    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveState     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveTransition(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveEvaluator (TSharedPtr<FJsonObject> Params);

	/** Load a UStateTree from a content path. Populates Result.Message on failure. */
	class UStateTree* LoadStateTree(const FString& AssetPath, FBridgeResult& Result) const;
};
