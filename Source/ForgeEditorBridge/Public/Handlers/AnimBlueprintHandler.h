#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "AnimBlueprintHandler.generated.h"

/**
 * AnimBlueprintHandler — domain "animbp"  (v0.6.0 / UE 5.7)
 *
 * Creates and authors AnimBlueprint assets programmatically through the Bridge.
 *
 * Actions:
 *   create_animbp          → asset_path (string), skeleton_path (string, optional)
 *                            Creates a new UAnimBlueprint asset via UAnimBlueprintFactory.
 *
 *   add_state_machine      → asset_path (string), node_name (string),
 *                            x (int, optional), y (int, optional)
 *                            Adds a UAnimGraphNode_StateMachine to the root AnimGraph.
 *                            PostPlacedNewNode() creates the inner UAnimationStateMachineGraph.
 *
 *   add_state              → asset_path (string), state_machine_name (string),
 *                            state_name (string), x (int, optional), y (int, optional)
 *                            Adds a UAnimStateNode inside the named state machine's graph.
 *
 *   set_state_animation    → asset_path (string), state_machine_name (string),
 *                            state_name (string), sequence_path (string)
 *                            Places a UAnimGraphNode_SequencePlayer in the state's bound graph
 *                            and wires it to the UAnimGraphNode_StateResult node.
 *
 *   add_transition         → asset_path (string), state_machine_name (string),
 *                            from_state (string), to_state (string)
 *                            Creates a UAnimStateTransitionNode connecting two state nodes.
 *
 *   set_entry_state        → asset_path (string), state_machine_name (string),
 *                            state_name (string)
 *                            Wires the UAnimStateEntryNode's output to the named state's input.
 *                            Must be called after add_state for a valid AnimBP compile.
 *
 *   compile_animbp         → asset_path (string)
 *                            Triggers FKismetEditorUtilities::CompileBlueprint.
 *
 *   get_graph_topology     → asset_path (string)
 *                            Returns state machine names, state counts, and transition counts.
 *
 *   list_states            → asset_path (string), state_machine_name (string)
 *                            Returns all state names in the specified state machine.
 *
 *   get_transitions        → asset_path (string), state_machine_name (string)
 *                            Returns all transitions: from_state, to_state, priority.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAnimBlueprintHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("animbp"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_animbp"), TEXT("add_state_machine"), TEXT("add_state"), TEXT("set_state_animation"), TEXT("add_transition"), TEXT("set_entry_state"), TEXT("compile_animbp"), TEXT("get_graph_topology"), TEXT("list_states"), TEXT("get_transitions"), TEXT("remove_state"), TEXT("remove_transition"), TEXT("add_anim_node"), TEXT("set_transition_rule_variable"), TEXT("set_transition_rule_function"), TEXT("set_transition_blend_settings"), TEXT("add_state_alias"), TEXT("add_conduit"), TEXT("set_sequence_player_settings") }; }
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateAnimBP         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddStateMachine      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddState             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetStateAnimation    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddTransition        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetEntryState        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CompileAnimBP        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetGraphTopology     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListStates           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetTransitions       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveState          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveTransition     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddAnimNode          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetTransitionRuleVariable(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetTransitionRuleFunction(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetTransitionBlendSettings(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddStateAlias             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddConduit                (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetSequencePlayerSettings (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a UAnimBlueprint from a content path.
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UAnimBlueprint* LoadAnimBP(const FString& AssetPath, FBridgeResult& Result);

	/**
	 * Find the root UAnimationGraph on an AnimBlueprint (the graph named "AnimGraph").
	 * Returns nullptr and sets OutError on failure.
	 */
	class UEdGraph* FindAnimGraph(class UAnimBlueprint* AnimBP, FString& OutError);

	/**
	 * Find a UAnimGraphNode_StateMachine inside the AnimGraph by its inner graph name.
	 * Returns nullptr and sets OutError on failure.
	 */
	class UAnimGraphNode_StateMachine* FindStateMachineNode(
		class UAnimBlueprint* AnimBP,
		const FString& NodeName,
		FString& OutError);

	/**
	 * Find a UAnimStateNode inside a state machine graph by state name (= bound graph name).
	 * Returns nullptr and sets OutError on failure.
	 */
	class UAnimStateNode* FindStateNode(
		class UEdGraph* SMGraph,
		const FString& StateName,
		FString& OutError);
};
