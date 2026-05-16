#include "Handlers/AnimBlueprintHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- AnimBlueprint ---------------------------------------------------------
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Factories/AnimBlueprintFactory.h"

// ---- Anim graph node types -------------------------------------------------
// These headers live in the AnimGraph module (AnimationBlueprintEditor must be
// in PrivateDependencyModuleNames for access to the factory).
#include "AnimGraphNode_StateMachine.h"     // UAnimGraphNode_StateMachine
#include "AnimStateNode.h"                  // UAnimStateNode
#include "AnimStateTransitionNode.h"        // UAnimStateTransitionNode
#include "AnimStateEntryNode.h"             // UAnimStateEntryNode
#include "AnimGraphNode_SequencePlayer.h"   // UAnimGraphNode_SequencePlayer
#include "AnimGraphNode_StateResult.h"      // UAnimGraphNode_StateResult

// ---- Anim graph types ------------------------------------------------------
#include "AnimationGraph.h"                 // UAnimationGraph
#include "AnimationStateMachineGraph.h"     // UAnimationStateMachineGraph
#include "AnimationStateGraph.h"            // UAnimationStateGraph

// ---- Animation asset -------------------------------------------------------
#include "Animation/AnimSequence.h"

// ---- Blueprint compilation -------------------------------------------------
#include "Kismet2/KismetEditorUtilities.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UAnimBlueprintHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("animbp"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_animbp"))       return Action_CreateAnimBP(Params);
	if (Action == TEXT("add_state_machine"))   return Action_AddStateMachine(Params);
	if (Action == TEXT("add_state"))           return Action_AddState(Params);
	if (Action == TEXT("set_state_animation")) return Action_SetStateAnimation(Params);
	if (Action == TEXT("add_transition"))      return Action_AddTransition(Params);
	if (Action == TEXT("set_entry_state"))     return Action_SetEntryState(Params);
	if (Action == TEXT("compile_animbp"))      return Action_CompileAnimBP(Params);
	if (Action == TEXT("get_graph_topology"))  return Action_GetGraphTopology(Params);
	if (Action == TEXT("list_states"))         return Action_ListStates(Params);
	if (Action == TEXT("get_transitions"))     return Action_GetTransitions(Params);
	if (Action == TEXT("remove_state"))        return Action_RemoveState(Params);
	if (Action == TEXT("remove_transition"))   return Action_RemoveTransition(Params);
	if (Action == TEXT("add_anim_node"))       return Action_AddAnimNode(Params);
	if (Action == TEXT("set_transition_rule_variable")) return Action_SetTransitionRuleVariable(Params);
	if (Action == TEXT("set_transition_rule_function")) return Action_SetTransitionRuleFunction(Params);
	if (Action == TEXT("set_transition_blend_settings")) return Action_SetTransitionBlendSettings(Params);
	if (Action == TEXT("add_state_alias"))     return Action_AddStateAlias(Params);
	if (Action == TEXT("add_conduit"))         return Action_AddConduit(Params);
	if (Action == TEXT("set_sequence_player_settings")) return Action_SetSequencePlayerSettings(Params);

	FBridgeResult R = CreateResult(TEXT("animbp"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown animbp action '%s'. Valid: create_animbp, add_state_machine, add_state, "
		     "set_state_animation, add_transition, set_entry_state, compile_animbp, "
		     "get_graph_topology, list_states, get_transitions"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_animbp
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_CreateAnimBP(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("create_animbp"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_animbp: 'asset_path' is required (e.g. '/Game/Animations/ABP_MyChar')");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString SkeletonPath;
	Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->ParentClass   = UAnimInstance::StaticClass();

	if (!SkeletonPath.IsEmpty())
	{
		USkeleton* Skel = LoadObject<USkeleton>(nullptr, *SkeletonPath);
		if (!Skel)
		{
			Result.Message = FString::Printf(
				TEXT("create_animbp: skeleton not found at '%s' (proceeding without skeleton)"),
				*SkeletonPath);
			// Non-fatal — skeleton is optional; AnimBP can be assigned one later
		}
		Factory->TargetSkeleton = Skel;
	}

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UAnimBlueprint::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_animbp: failed to create asset at '%s' (path may already exist or be invalid)"),
			*AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UAnimBlueprint* NewAnimBP = CastChecked<UAnimBlueprint>(CreatedAsset);
	FKismetEditorUtilities::CompileBlueprint(NewAnimBP);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("AnimBlueprint created at %s (skeleton=%s)"),
		*AssetPath, SkeletonPath.IsEmpty() ? TEXT("none") : *SkeletonPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_state_machine
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_AddStateMachine(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_state_machine"));

	FString AssetPath, NodeName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("add_state_machine: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
	{
		Result.Message = TEXT("add_state_machine: 'node_name' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	double XVal = 0.0, YVal = 0.0;
	Params->TryGetNumberField(TEXT("x"), XVal);
	Params->TryGetNumberField(TEXT("y"), YVal);

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FString GraphError;
	UEdGraph* AnimGraph = FindAnimGraph(AnimBP, GraphError);
	if (!AnimGraph)
	{
		Result.Message = FString::Printf(
			TEXT("add_state_machine: %s"), *GraphError);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Create the state machine node. PostPlacedNewNode() creates EditorStateMachineGraph.
	UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
	SMNode->CreateNewGuid();
	SMNode->PostPlacedNewNode();
	SMNode->AllocateDefaultPins();
	SMNode->NodePosX = (int32)XVal;
	SMNode->NodePosY = (int32)YVal;
	AnimGraph->Nodes.Add(SMNode);

	// Name the inner state machine graph so it can be found by name later.
	// EditorStateMachineGraph is public UPROPERTY on UAnimGraphNode_StateMachine.
	if (SMNode->EditorStateMachineGraph)
	{
		SMNode->EditorStateMachineGraph->Rename(
			*NodeName, nullptr, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
	}

	AnimBP->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("State machine '%s' added to AnimGraph of '%s'"), *NodeName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_state
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_AddState(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_state"));

	FString AssetPath, SMName, StateName;
	if (!Params->TryGetStringField(TEXT("asset_path"),          AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("state_machine_name"),  SMName)    || SMName.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("state_name"),          StateName) || StateName.IsEmpty())
	{
		Result.Message = TEXT("add_state: 'asset_path', 'state_machine_name', and 'state_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	double XVal = 0.0, YVal = 0.0;
	Params->TryGetNumberField(TEXT("x"), XVal);
	Params->TryGetNumberField(TEXT("y"), YVal);

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FString SMError;
	UAnimGraphNode_StateMachine* SMNode = FindStateMachineNode(AnimBP, SMName, SMError);
	if (!SMNode)
	{
		Result.Message = FString::Printf(TEXT("add_state: %s"), *SMError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		Result.Message = FString::Printf(
			TEXT("add_state: state machine '%s' has no inner graph"), *SMName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Create the state node. PostPlacedNewNode() creates the bound UAnimationStateGraph.
	UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
	StateNode->CreateNewGuid();
	StateNode->PostPlacedNewNode();
	StateNode->AllocateDefaultPins();
	StateNode->NodePosX = (int32)XVal;
	StateNode->NodePosY = (int32)YVal;
	SMGraph->Nodes.Add(StateNode);

	// Rename the bound graph so the state name shows correctly in the editor.
	UEdGraph* BoundGraph = StateNode->GetBoundGraph();
	if (BoundGraph)
	{
		BoundGraph->Rename(
			*StateName, nullptr, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
	}

	AnimBP->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("State '%s' added to state machine '%s' in '%s'"), *StateName, *SMName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_state_animation
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_SetStateAnimation(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_state_animation"));

	FString AssetPath, SMName, StateName, SequencePath;
	if (!Params->TryGetStringField(TEXT("asset_path"),         AssetPath)    || AssetPath.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName)        || SMName.IsEmpty()       ||
	    !Params->TryGetStringField(TEXT("state_name"),         StateName)     || StateName.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("sequence_path"),      SequencePath)  || SequencePath.IsEmpty())
	{
		Result.Message = TEXT("set_state_animation: 'asset_path', 'state_machine_name', "
		                      "'state_name', and 'sequence_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FString SMError;
	UAnimGraphNode_StateMachine* SMNode = FindStateMachineNode(AnimBP, SMName, SMError);
	if (!SMNode)
	{
		Result.Message = FString::Printf(TEXT("set_state_animation: %s"), *SMError);
		Result.ErrorCode = 2000;
		return Result;
	}

	FString StateError;
	UAnimStateNode* StateNode = FindStateNode(SMNode->EditorStateMachineGraph, StateName, StateError);
	if (!StateNode)
	{
		Result.Message = FString::Printf(TEXT("set_state_animation: %s"), *StateError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UEdGraph* BoundGraph = StateNode->GetBoundGraph();
	UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(BoundGraph);
	if (!StateGraph)
	{
		Result.Message = FString::Printf(
			TEXT("set_state_animation: bound graph of state '%s' is not a UAnimationStateGraph"),
			*StateName);
		Result.ErrorCode = 2001;
		return Result;
	}

	// Load the animation sequence (optional — node can exist without sequence set)
	UAnimSequence* AnimSeq = nullptr;
	if (!SequencePath.IsEmpty())
	{
		AnimSeq = LoadObject<UAnimSequence>(nullptr, *SequencePath);
		if (!AnimSeq)
		{
			Result.Message = FString::Printf(
				TEXT("set_state_animation: UAnimSequence not found at '%s'"), *SequencePath);
			Result.ErrorCode = 2000;
			Result.RecoveryHint = TEXT("Verify the sequence_path points to a valid UAnimSequence asset");
			return Result;
		}
	}

	// Locate the pre-existing UAnimGraphNode_StateResult node in the state graph
	UAnimGraphNode_StateResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : StateGraph->Nodes)
	{
		ResultNode = Cast<UAnimGraphNode_StateResult>(Node);
		if (ResultNode) break;
	}

	// Create the sequence player node
	UAnimGraphNode_SequencePlayer* SeqPlayer =
		NewObject<UAnimGraphNode_SequencePlayer>(StateGraph);
	SeqPlayer->SetAnimationAsset(AnimSeq);
	SeqPlayer->CreateNewGuid();
	SeqPlayer->PostPlacedNewNode();
	SeqPlayer->AllocateDefaultPins();
	SeqPlayer->NodePosX = ResultNode ? (ResultNode->NodePosX - 200) : 0;
	SeqPlayer->NodePosY = ResultNode ? ResultNode->NodePosY          : 0;
	StateGraph->Nodes.Add(SeqPlayer);

	// Wire sequence player's pose output to the result node's pose input.
	// Pin directions: SeqPlayer exposes EGPD_Output pose, ResultNode expects EGPD_Input pose.
	// We use the first (and only) output/input pose link pins on each node.
	if (ResultNode)
	{
		UEdGraphPin* OutPin = nullptr;
		for (UEdGraphPin* Pin : SeqPlayer->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output) { OutPin = Pin; break; }
		}

		UEdGraphPin* InPin = nullptr;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)  { InPin  = Pin; break; }
		}

		if (OutPin && InPin)
		{
			OutPin->MakeLinkTo(InPin);
		}
	}

	AnimBP->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("SequencePlayer set to '%s' for state '%s' in state machine '%s' ('%s')"),
		*SequencePath, *StateName, *SMName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_transition
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_AddTransition(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_transition"));

	FString AssetPath, SMName, FromStateName, ToStateName;
	if (!Params->TryGetStringField(TEXT("asset_path"),         AssetPath)     || AssetPath.IsEmpty()     ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName)         || SMName.IsEmpty()        ||
	    !Params->TryGetStringField(TEXT("from_state"),         FromStateName)  || FromStateName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("to_state"),           ToStateName)    || ToStateName.IsEmpty())
	{
		Result.Message = TEXT("add_transition: 'asset_path', 'state_machine_name', "
		                      "'from_state', and 'to_state' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FString SMError;
	UAnimGraphNode_StateMachine* SMNode = FindStateMachineNode(AnimBP, SMName, SMError);
	if (!SMNode)
	{
		Result.Message = FString::Printf(TEXT("add_transition: %s"), *SMError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		Result.Message = FString::Printf(
			TEXT("add_transition: state machine '%s' has no inner graph"), *SMName);
		Result.ErrorCode = 3000;
		return Result;
	}

	FString FromError, ToError;
	UAnimStateNode* FromState = FindStateNode(SMGraph, FromStateName, FromError);
	if (!FromState)
	{
		Result.Message = FString::Printf(TEXT("add_transition (from_state): %s"), *FromError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UAnimStateNode* ToState = FindStateNode(SMGraph, ToStateName, ToError);
	if (!ToState)
	{
		Result.Message = FString::Printf(TEXT("add_transition (to_state): %s"), *ToError);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Create the transition node. PostPlacedNewNode() creates its bound rule graph.
	UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
	TransNode->CreateNewGuid();
	TransNode->PostPlacedNewNode();
	TransNode->AllocateDefaultPins();

	// Position transition midway between the two states
	TransNode->NodePosX = (FromState->NodePosX + ToState->NodePosX) / 2;
	TransNode->NodePosY = (FromState->NodePosY + ToState->NodePosY) / 2;
	SMGraph->Nodes.Add(TransNode);

	// Wire: FromState's output pin → TransNode's input pin → ToState's input pin.
	// UAnimStateNode pins: [0]=In (EGPD_Input), [1]=Out (EGPD_Output)
	// UAnimStateTransitionNode pins: [0]=In (EGPD_Input), [1]=Out (EGPD_Output)
	// NOTE: Verify exact pin indices at compile time if assertions fire.
	UEdGraphPin* FromOutPin  = FromState->GetOutputPin();
	UEdGraphPin* TransInPin  = TransNode->GetInputPin();
	UEdGraphPin* TransOutPin = TransNode->GetOutputPin();
	UEdGraphPin* ToInPin     = ToState->GetInputPin();

	if (FromOutPin && TransInPin)   FromOutPin->MakeLinkTo(TransInPin);
	if (TransOutPin && ToInPin)     TransOutPin->MakeLinkTo(ToInPin);

	AnimBP->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Transition added: '%s' → '%s' in state machine '%s' ('%s')"),
		*FromStateName, *ToStateName, *SMName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_entry_state — wire the UAnimStateEntryNode to the named initial state
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_SetEntryState(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_entry_state"));

	FString AssetPath, SMName, StateName;
	if (!Params->TryGetStringField(TEXT("asset_path"),         AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName)     || SMName.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("state_name"),         StateName)  || StateName.IsEmpty())
	{
		Result.Message = TEXT("set_entry_state: 'asset_path', 'state_machine_name', and 'state_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FString SMError;
	UAnimGraphNode_StateMachine* SMNode = FindStateMachineNode(AnimBP, SMName, SMError);
	if (!SMNode)
	{
		Result.Message = FString::Printf(TEXT("set_entry_state: %s"), *SMError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		Result.Message = FString::Printf(
			TEXT("set_entry_state: state machine '%s' has no inner graph"), *SMName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Locate the auto-created UAnimStateEntryNode (exactly one per state machine graph)
	UAnimStateEntryNode* EntryNode = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		EntryNode = Cast<UAnimStateEntryNode>(Node);
		if (EntryNode) break;
	}
	if (!EntryNode)
	{
		Result.Message = FString::Printf(
			TEXT("set_entry_state: no UAnimStateEntryNode found in state machine '%s'"), *SMName);
		Result.ErrorCode = 3000;
		return Result;
	}

	FString StateError;
	UAnimStateNode* TargetState = FindStateNode(SMGraph, StateName, StateError);
	if (!TargetState)
	{
		Result.Message = FString::Printf(TEXT("set_entry_state: %s"), *StateError);
		Result.ErrorCode = 2000;
		return Result;
	}

	UEdGraphPin* EntryOutPin = EntryNode->GetOutputPin();
	if (!EntryOutPin)
	{
		Result.Message = TEXT("set_entry_state: UAnimStateEntryNode has no output pin");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Break any existing wiring from the entry node, then re-wire to the target state
	EntryOutPin->BreakAllPinLinks();

	UEdGraphPin* TargetInPin = TargetState->GetInputPin();
	if (!TargetInPin)
	{
		Result.Message = FString::Printf(
			TEXT("set_entry_state: state '%s' has no input pin"), *StateName);
		Result.ErrorCode = 3000;
		return Result;
	}

	EntryOutPin->MakeLinkTo(TargetInPin);
	AnimBP->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Entry state set to '%s' in state machine '%s' ('%s')"),
		*StateName, *SMName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// compile_animbp
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_CompileAnimBP(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("compile_animbp"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("compile_animbp: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("AnimBlueprint compiled: %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

UAnimBlueprint* UAnimBlueprintHandler::LoadAnimBP(const FString& AssetPath, FBridgeResult& Result)
{
	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBP)
	{
		// Try path with asset name suffix (e.g. "/Game/Animations/ABP_Char.ABP_Char")
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
	}
	if (!AnimBP)
	{
		Result.Message = FString::Printf(
			TEXT("LoadAnimBP: no UAnimBlueprint found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the asset_path points to a valid UAnimBlueprint asset");
	}
	return AnimBP;
}

UEdGraph* UAnimBlueprintHandler::FindAnimGraph(UAnimBlueprint* AnimBP, FString& OutError)
{
	if (!AnimBP) { OutError = TEXT("AnimBP is null"); return nullptr; }

	// The root anim graph is a UAnimationGraph; it lives in FunctionGraphs.
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (Graph && Graph->IsA<UAnimationGraph>()) return Graph;
	}

	OutError = FString::Printf(
		TEXT("UAnimationGraph not found in FunctionGraphs of '%s'"), *AnimBP->GetName());
	return nullptr;
}

UAnimGraphNode_StateMachine* UAnimBlueprintHandler::FindStateMachineNode(
	UAnimBlueprint* AnimBP,
	const FString& NodeName,
	FString& OutError)
{
	FString GraphError;
	UEdGraph* AnimGraph = FindAnimGraph(AnimBP, GraphError);
	if (!AnimGraph)
	{
		OutError = GraphError;
		return nullptr;
	}

	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
		if (SMNode && SMNode->EditorStateMachineGraph &&
		    SMNode->EditorStateMachineGraph->GetName() == NodeName)
		{
			return SMNode;
		}
	}

	OutError = FString::Printf(
		TEXT("State machine node named '%s' not found in AnimGraph of '%s'"),
		*NodeName, *AnimBP->GetName());
	return nullptr;
}

UAnimStateNode* UAnimBlueprintHandler::FindStateNode(
	UEdGraph* SMGraph,
	const FString& StateName,
	FString& OutError)
{
	if (!SMGraph) { OutError = TEXT("State machine graph is null"); return nullptr; }

	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
		if (StateNode)
		{
			UEdGraph* BoundGraph = StateNode->GetBoundGraph();
			if (BoundGraph && BoundGraph->GetName() == StateName)
			{
				return StateNode;
			}
		}
	}

	OutError = FString::Printf(
		TEXT("State node named '%s' not found in state machine graph '%s'"),
		*StateName, *SMGraph->GetName());
	return nullptr;
}

// ---------------------------------------------------------------------------
// get_graph_topology
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_GetGraphTopology(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("get_graph_topology"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_graph_topology: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBP)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
	}
	if (!AnimBP)
	{
		Result.Message = FString::Printf(TEXT("get_graph_topology: no UAnimBlueprint at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (!Graph) continue;
		TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
		G->SetStringField(TEXT("name"),  Graph->GetName());
		G->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		G->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArr.Add(MakeShared<FJsonValueObject>(G));
	}
	// Also include the anim graph
	for (UEdGraph* Graph : AnimBP->UbergraphPages)
	{
		if (!Graph) continue;
		TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
		G->SetStringField(TEXT("name"),  Graph->GetName());
		G->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		G->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArr.Add(MakeShared<FJsonValueObject>(G));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("graphs"), GraphsArr);
	Data->SetNumberField(TEXT("graph_count"), GraphsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("get_graph_topology: %d graphs in '%s'"), GraphsArr.Num(), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// list_states
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_ListStates(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("list_states"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("list_states: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBP)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
	}
	if (!AnimBP)
	{
		Result.Message = FString::Printf(TEXT("list_states: no UAnimBlueprint at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> StatesArr;
	// Iterate all graphs looking for state machine graphs
	TArray<UEdGraph*> AllGraphs;
	AnimBP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
			if (!StateNode) continue;
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("name"), StateNode->GetStateName());
			S->SetStringField(TEXT("graph"), Graph->GetName());
			StatesArr.Add(MakeShared<FJsonValueObject>(S));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("states"), StatesArr);
	Data->SetNumberField(TEXT("count"), StatesArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("list_states: %d state(s) in '%s'"), StatesArr.Num(), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_transitions
// ---------------------------------------------------------------------------

FBridgeResult UAnimBlueprintHandler::Action_GetTransitions(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("get_transitions"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_transitions: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBP)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
	}
	if (!AnimBP)
	{
		Result.Message = FString::Printf(TEXT("get_transitions: no UAnimBlueprint at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> TransArr;
	TArray<UEdGraph*> AllGraphs;
	AnimBP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node);
			if (!TransNode) continue;
			TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("name"), TransNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			T->SetStringField(TEXT("graph"), Graph->GetName());
			T->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);
			T->SetNumberField(TEXT("priority_order"), TransNode->PriorityOrder);
			TransArr.Add(MakeShared<FJsonValueObject>(T));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("transitions"), TransArr);
	Data->SetNumberField(TEXT("count"), TransArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("get_transitions: %d transition(s) in '%s'"), TransArr.Num(), *AssetPath);
	return Result;
}

// ===========================================================================
// Wave 7: AnimBP deepening (remove_state, remove_transition, add_anim_node, set_transition_rule_variable)
// ===========================================================================

#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimStateTransitionNode.h"
#include "K2Node_VariableGet.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

FBridgeResult UAnimBlueprintHandler::Action_RemoveState(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("remove_state"));
	FString AssetPath, SMName, StateName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
	    !Params->TryGetStringField(TEXT("state_name"), StateName))
		return MakeError(TEXT("animbp"), TEXT("remove_state"), 1000,
			TEXT("'asset_path', 'state_machine_name', and 'state_name' are required"));

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;
	FString Err;
	UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
	if (!SM) return MakeError(TEXT("animbp"), TEXT("remove_state"), 2000, Err);
	UEdGraph* SMGraph = SM->EditorStateMachineGraph;
	UAnimStateNode* StateNode = FindStateNode(SMGraph, StateName, Err);
	if (!StateNode) return MakeError(TEXT("animbp"), TEXT("remove_state"), 2000, Err);

	int32 RemovedTransitions = 0;
	TArray<UEdGraphNode*> Nodes = SMGraph->Nodes;
	for (UEdGraphNode* N : Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
		if (T && (T->GetPreviousState() == StateNode || T->GetNextState() == StateNode))
		{
			SMGraph->RemoveNode(T);
			++RemovedTransitions;
		}
	}
	SMGraph->RemoveNode(StateNode);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	return MakeSuccess(TEXT("animbp"), TEXT("remove_state"),
		FString::Printf(TEXT("Removed state %s (+%d transitions cascaded)"), *StateName, RemovedTransitions));
}

FBridgeResult UAnimBlueprintHandler::Action_RemoveTransition(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("remove_transition"));
	FString AssetPath, SMName, FromState, ToState;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
	    !Params->TryGetStringField(TEXT("from_state"), FromState) ||
	    !Params->TryGetStringField(TEXT("to_state"), ToState))
		return MakeError(TEXT("animbp"), TEXT("remove_transition"), 1000,
			TEXT("Required: asset_path, state_machine_name, from_state, to_state"));

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;
	FString Err;
	UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
	if (!SM) return MakeError(TEXT("animbp"), TEXT("remove_transition"), 2000, Err);
	UEdGraph* SMGraph = SM->EditorStateMachineGraph;
	UAnimStateNode* FromNode = FindStateNode(SMGraph, FromState, Err);
	if (!FromNode) return MakeError(TEXT("animbp"), TEXT("remove_transition"), 2000, Err);
	UAnimStateNode* ToNode = FindStateNode(SMGraph, ToState, Err);
	if (!ToNode) return MakeError(TEXT("animbp"), TEXT("remove_transition"), 2000, Err);

	int32 Removed = 0;
	TArray<UEdGraphNode*> Nodes = SMGraph->Nodes;
	for (UEdGraphNode* N : Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
		if (T && T->GetPreviousState() == FromNode && T->GetNextState() == ToNode)
		{
			SMGraph->RemoveNode(T);
			++Removed;
		}
	}
	if (Removed == 0)
		return MakeError(TEXT("animbp"), TEXT("remove_transition"), 2000,
			FString::Printf(TEXT("No transition %s -> %s"), *FromState, *ToState));
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	return MakeSuccess(TEXT("animbp"), TEXT("remove_transition"),
		FString::Printf(TEXT("Removed %d transition(s)"), Removed));
}

FBridgeResult UAnimBlueprintHandler::Action_AddAnimNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_anim_node"));
	FString AssetPath, NodeType, GraphName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeError(TEXT("animbp"), TEXT("add_anim_node"), 1000, TEXT("asset_path required"));
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeError(TEXT("animbp"), TEXT("add_anim_node"), 1000,
			TEXT("node_type required (sequence_player|blendspace_player|blend_list_by_bool|use_cached_pose|save_cached_pose)"));
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;

	UEdGraph* TargetGraph = nullptr;
	if (!GraphName.IsEmpty())
	{
		const FName GN(*GraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs) if (G && G->GetFName() == GN) { TargetGraph = G; break; }
		if (!TargetGraph) for (UEdGraph* G : AnimBP->MacroGraphs) if (G && G->GetFName() == GN) { TargetGraph = G; break; }
	}
	if (!TargetGraph)
	{
		FString Err;
		TargetGraph = FindAnimGraph(AnimBP, Err);
		if (!TargetGraph) return MakeError(TEXT("animbp"), TEXT("add_anim_node"), 2000, Err);
	}

	int32 X = 0, Y = 0;
	Params->TryGetNumberField(TEXT("pos_x"), X);
	Params->TryGetNumberField(TEXT("pos_y"), Y);

	auto Place = [&](UEdGraphNode* N) -> UEdGraphNode*
	{
		TargetGraph->AddNode(N, true, false);
		N->NodePosX = X; N->NodePosY = Y;
		N->CreateNewGuid(); N->PostPlacedNewNode(); N->AllocateDefaultPins();
		return N;
	};

	UEdGraphNode* NewNode = nullptr;
	const FString T = NodeType.ToLower();
	if      (T == TEXT("sequence_player"))    NewNode = Place(NewObject<UAnimGraphNode_SequencePlayer>(TargetGraph));
	else if (T == TEXT("blendspace_player"))  NewNode = Place(NewObject<UAnimGraphNode_BlendSpacePlayer>(TargetGraph));
	else if (T == TEXT("blend_list_by_bool")) NewNode = Place(NewObject<UAnimGraphNode_BlendListByBool>(TargetGraph));
	else if (T == TEXT("use_cached_pose"))    NewNode = Place(NewObject<UAnimGraphNode_UseCachedPose>(TargetGraph));
	else if (T == TEXT("save_cached_pose"))   NewNode = Place(NewObject<UAnimGraphNode_SaveCachedPose>(TargetGraph));
	else
		return MakeError(TEXT("animbp"), TEXT("add_anim_node"), 1001,
			FString::Printf(TEXT("Unknown node_type: %s"), *NodeType));

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString());
	Data->SetStringField(TEXT("node_type"), NodeType);
	return MakeSuccess(TEXT("animbp"), TEXT("add_anim_node"),
		FString::Printf(TEXT("Added %s node"), *NodeType), Data);
}

FBridgeResult UAnimBlueprintHandler::Action_SetTransitionRuleVariable(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_transition_rule_variable"));
	FString AssetPath, SMName, FromState, ToState, BoolVarName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
	    !Params->TryGetStringField(TEXT("from_state"), FromState) ||
	    !Params->TryGetStringField(TEXT("to_state"), ToState) ||
	    !Params->TryGetStringField(TEXT("bool_variable_name"), BoolVarName))
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 1000,
			TEXT("Required: asset_path, state_machine_name, from_state, to_state, bool_variable_name"));

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;
	FString Err;
	UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
	if (!SM) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 2000, Err);
	UEdGraph* SMGraph = SM->EditorStateMachineGraph;
	UAnimStateNode* FromNode = FindStateNode(SMGraph, FromState, Err);
	if (!FromNode) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 2000, Err);
	UAnimStateNode* ToNode = FindStateNode(SMGraph, ToState, Err);
	if (!ToNode) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 2000, Err);

	UAnimStateTransitionNode* Transition = nullptr;
	for (UEdGraphNode* N : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
		if (T && T->GetPreviousState() == FromNode && T->GetNextState() == ToNode)
		{
			Transition = T; break;
		}
	}
	if (!Transition) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 2000,
		FString::Printf(TEXT("Transition %s -> %s not found"), *FromState, *ToState));

	bool bFound = false;
	for (const FBPVariableDescription& V : AnimBP->NewVariables)
	{
		if (V.VarName == FName(*BoolVarName))
		{
			if (V.VarType.PinCategory != UEdGraphSchema_K2::PC_Boolean)
				return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 1001,
					FString::Printf(TEXT("Variable %s is not a bool"), *BoolVarName));
			bFound = true; break;
		}
	}
	if (!bFound)
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 2000,
			FString::Printf(TEXT("Bool variable %s not found"), *BoolVarName));

	UEdGraph* RuleGraph = Transition->BoundGraph;
	if (!RuleGraph)
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_variable"), 3000, TEXT("Transition has no bound rule graph"));

	auto* GetNode = NewObject<UK2Node_VariableGet>(RuleGraph);
	RuleGraph->AddNode(GetNode, true, false);
	GetNode->VariableReference.SetSelfMember(FName(*BoolVarName));
	GetNode->NodePosX = -200; GetNode->NodePosY = 0;
	GetNode->CreateNewGuid(); GetNode->PostPlacedNewNode(); GetNode->AllocateDefaultPins();
	GetNode->ReconstructNode();

	UEdGraphNode* ResultNode = nullptr;
	for (UEdGraphNode* N : RuleGraph->Nodes)
	{
		if (N != GetNode && N->GetName().Contains(TEXT("Result"), ESearchCase::IgnoreCase))
		{
			ResultNode = N; break;
		}
	}
	if (ResultNode)
	{
		UEdGraphPin* OutPin = nullptr;
		for (UEdGraphPin* P : GetNode->Pins) if (P->Direction == EGPD_Output) { OutPin = P; break; }
		UEdGraphPin* InPin = nullptr;
		for (UEdGraphPin* P : ResultNode->Pins) if (P->Direction == EGPD_Input) { InPin = P; break; }
		if (OutPin && InPin) OutPin->MakeLinkTo(InPin);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	return MakeSuccess(TEXT("animbp"), TEXT("set_transition_rule_variable"),
		FString::Printf(TEXT("Wired %s to %s -> %s"), *BoolVarName, *FromState, *ToState));
}

// ----------------------------------------------------------------------------
// set_transition_rule_function — wires a member function (returning bool) to the
// transition rule's result pin. The function must already exist on the AnimBP.
// ----------------------------------------------------------------------------
#include "K2Node_CallFunction.h"

FBridgeResult UAnimBlueprintHandler::Action_SetTransitionRuleFunction(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_transition_rule_function"));
	FString AssetPath, SMName, FromState, ToState, FunctionName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
	    !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
	    !Params->TryGetStringField(TEXT("from_state"), FromState) ||
	    !Params->TryGetStringField(TEXT("to_state"), ToState) ||
	    !Params->TryGetStringField(TEXT("function_name"), FunctionName))
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 1000,
			TEXT("Required: asset_path, state_machine_name, from_state, to_state, function_name"));

	UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
	if (!AnimBP) return Result;
	FString Err;
	UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
	if (!SM) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 2000, Err);
	UEdGraph* SMGraph = SM->EditorStateMachineGraph;
	UAnimStateNode* FromNode = FindStateNode(SMGraph, FromState, Err);
	if (!FromNode) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 2000, Err);
	UAnimStateNode* ToNode = FindStateNode(SMGraph, ToState, Err);
	if (!ToNode) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 2000, Err);

	UAnimStateTransitionNode* Transition = nullptr;
	for (UEdGraphNode* N : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
		if (T && T->GetPreviousState() == FromNode && T->GetNextState() == ToNode)
		{
			Transition = T; break;
		}
	}
	if (!Transition) return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 2000,
		FString::Printf(TEXT("Transition %s -> %s not found"), *FromState, *ToState));

	UClass* GenClass = AnimBP->GeneratedClass;
	UFunction* Func = GenClass ? GenClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
	if (!Func)
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 2000,
			FString::Printf(TEXT("Function %s not found on AnimBP class"), *FunctionName));

	// Validate the function returns a bool
	FProperty* ReturnProp = Func->GetReturnProperty();
	if (!ReturnProp || !ReturnProp->IsA<FBoolProperty>())
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 1001,
			FString::Printf(TEXT("Function %s must return bool"), *FunctionName));

	UEdGraph* RuleGraph = Transition->BoundGraph;
	if (!RuleGraph)
		return MakeError(TEXT("animbp"), TEXT("set_transition_rule_function"), 3000,
			TEXT("Transition has no bound rule graph"));

	auto* CallNode = NewObject<UK2Node_CallFunction>(RuleGraph);
	RuleGraph->AddNode(CallNode, true, false);
	CallNode->SetFromFunction(Func);
	CallNode->NodePosX = -200; CallNode->NodePosY = 0;
	CallNode->CreateNewGuid(); CallNode->PostPlacedNewNode(); CallNode->AllocateDefaultPins();
	CallNode->ReconstructNode();

	UEdGraphNode* ResultNode = nullptr;
	for (UEdGraphNode* N : RuleGraph->Nodes)
	{
		if (N != CallNode && N->GetName().Contains(TEXT("Result"), ESearchCase::IgnoreCase))
		{
			ResultNode = N; break;
		}
	}
	if (ResultNode)
	{
		// Find the bool return pin on the call node and the bool input on the result
		UEdGraphPin* ReturnPin = nullptr;
		for (UEdGraphPin* P : CallNode->Pins)
		{
			if (P->Direction == EGPD_Output &&
			    P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				ReturnPin = P; break;
			}
		}
		if (!ReturnPin)
		{
			for (UEdGraphPin* P : CallNode->Pins)
				if (P->Direction == EGPD_Output && P->PinName == FName(TEXT("ReturnValue"))) { ReturnPin = P; break; }
		}
		UEdGraphPin* ResultIn = nullptr;
		for (UEdGraphPin* P : ResultNode->Pins) if (P->Direction == EGPD_Input) { ResultIn = P; break; }
		if (ReturnPin && ResultIn) ReturnPin->MakeLinkTo(ResultIn);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	return MakeSuccess(TEXT("animbp"), TEXT("set_transition_rule_function"),
		FString::Printf(TEXT("Wired function %s to transition %s -> %s"),
			*FunctionName, *FromState, *ToState));
}

// ===========================================================================
// Wave 10: AnimBP extras (transition blend settings, state alias, conduit, seq player settings)
// ===========================================================================

#include "Animation/AnimNode_SequencePlayer.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateNodeBase.h"

namespace
{
    EAlphaBlendOption ParseBlendMode(const FString& Mode)
    {
        const FString M = Mode.ToLower();
        if (M == TEXT("cubic"))                return EAlphaBlendOption::Cubic;
        if (M == TEXT("hermitecubic"))         return EAlphaBlendOption::HermiteCubic;
        if (M == TEXT("sinusoidal"))           return EAlphaBlendOption::Sinusoidal;
        if (M == TEXT("quadraticinout"))       return EAlphaBlendOption::QuadraticInOut;
        if (M == TEXT("cubicinout"))           return EAlphaBlendOption::CubicInOut;
        if (M == TEXT("quarticinout"))         return EAlphaBlendOption::QuarticInOut;
        if (M == TEXT("quinticinout"))         return EAlphaBlendOption::QuinticInOut;
        if (M == TEXT("circularin"))           return EAlphaBlendOption::CircularIn;
        if (M == TEXT("circularout"))          return EAlphaBlendOption::CircularOut;
        if (M == TEXT("circularinout"))        return EAlphaBlendOption::CircularInOut;
        if (M == TEXT("expin"))                return EAlphaBlendOption::ExpIn;
        if (M == TEXT("expout"))               return EAlphaBlendOption::ExpOut;
        if (M == TEXT("expinout"))             return EAlphaBlendOption::ExpInOut;
        return EAlphaBlendOption::Linear;
    }
}

FBridgeResult UAnimBlueprintHandler::Action_SetTransitionBlendSettings(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_transition_blend_settings"));
    FString AssetPath, SMName, FromState, ToState, BlendMode;
    double CrossfadeDuration = -1.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
        !Params->TryGetStringField(TEXT("from_state"), FromState) ||
        !Params->TryGetStringField(TEXT("to_state"), ToState))
        return MakeError(TEXT("animbp"), TEXT("set_transition_blend_settings"), 1000,
            TEXT("Required: asset_path, state_machine_name, from_state, to_state"));
    Params->TryGetNumberField(TEXT("crossfade_duration"), CrossfadeDuration);
    Params->TryGetStringField(TEXT("blend_mode"), BlendMode);

    UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
    if (!AnimBP) return Result;
    FString Err;
    UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
    if (!SM) return MakeError(TEXT("animbp"), TEXT("set_transition_blend_settings"), 2000, Err);
    UEdGraph* SMGraph = SM->EditorStateMachineGraph;
    UAnimStateNode* FromNode = FindStateNode(SMGraph, FromState, Err);
    if (!FromNode) return MakeError(TEXT("animbp"), TEXT("set_transition_blend_settings"), 2000, Err);
    UAnimStateNode* ToNode = FindStateNode(SMGraph, ToState, Err);
    if (!ToNode) return MakeError(TEXT("animbp"), TEXT("set_transition_blend_settings"), 2000, Err);

    UAnimStateTransitionNode* Transition = nullptr;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
        if (T && T->GetPreviousState() == FromNode && T->GetNextState() == ToNode)
        { Transition = T; break; }
    }
    if (!Transition) return MakeError(TEXT("animbp"), TEXT("set_transition_blend_settings"), 2000,
        FString::Printf(TEXT("Transition %s -> %s not found"), *FromState, *ToState));

    if (CrossfadeDuration >= 0.0) Transition->CrossfadeDuration = (float)CrossfadeDuration;
    if (!BlendMode.IsEmpty())     Transition->BlendMode         = ParseBlendMode(BlendMode);

    FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
    return MakeSuccess(TEXT("animbp"), TEXT("set_transition_blend_settings"),
        FString::Printf(TEXT("Updated transition '%s'->'%s' (duration=%.3f mode=%s)"),
            *FromState, *ToState, Transition->CrossfadeDuration, *BlendMode));
}

FBridgeResult UAnimBlueprintHandler::Action_AddStateAlias(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_state_alias"));
    FString AssetPath, SMName, AliasName;
    int32 X = 0, Y = 0;
    bool bGlobalAlias = false;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
        !Params->TryGetStringField(TEXT("alias_name"), AliasName))
        return MakeError(TEXT("animbp"), TEXT("add_state_alias"), 1000,
            TEXT("Required: asset_path, state_machine_name, alias_name"));
    Params->TryGetNumberField(TEXT("pos_x"), X);
    Params->TryGetNumberField(TEXT("pos_y"), Y);
    Params->TryGetBoolField(TEXT("global_alias"), bGlobalAlias);
    const TArray<TSharedPtr<FJsonValue>>* AliasedNames = nullptr;
    Params->TryGetArrayField(TEXT("aliased_states"), AliasedNames);

    UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
    if (!AnimBP) return Result;
    FString Err;
    UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
    if (!SM) return MakeError(TEXT("animbp"), TEXT("add_state_alias"), 2000, Err);
    UEdGraph* SMGraph = SM->EditorStateMachineGraph;

    UAnimStateAliasNode* Alias = NewObject<UAnimStateAliasNode>(SMGraph);
    SMGraph->AddNode(Alias, /*bUserAction=*/true, /*bSelectNewNode=*/false);
    Alias->NodePosX = X; Alias->NodePosY = Y;
    Alias->CreateNewGuid();
    Alias->StateAliasName = AliasName;          // FString field, not FName
    Alias->bGlobalAlias   = bGlobalAlias;
    Alias->PostPlacedNewNode();
    Alias->AllocateDefaultPins();

    int32 AliasedCount = 0;
    if (!bGlobalAlias && AliasedNames)
    {
        TSet<TWeakObjectPtr<UAnimStateNodeBase>>& AliasedSet = Alias->GetAliasedStates();
        for (const TSharedPtr<FJsonValue>& V : *AliasedNames)
        {
            const FString StateName = V->AsString();
            if (UAnimStateNode* StateNode = FindStateNode(SMGraph, StateName, Err))
            {
                AliasedSet.Add(StateNode);
                ++AliasedCount;
            }
        }
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("alias_name"), AliasName);
    Data->SetStringField(TEXT("node_guid"), Alias->NodeGuid.ToString());
    Data->SetBoolField(TEXT("global_alias"), bGlobalAlias);
    Data->SetNumberField(TEXT("aliased_count"), AliasedCount);
    const FString Mode = bGlobalAlias ? TEXT("global") : TEXT("explicit");
    return MakeSuccess(TEXT("animbp"), TEXT("add_state_alias"),
        FString::Printf(TEXT("Added state alias '%s' (%s, %d aliased)"),
            *AliasName, *Mode, AliasedCount),
        Data);
}

FBridgeResult UAnimBlueprintHandler::Action_AddConduit(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("add_conduit"));
    FString AssetPath, SMName, ConduitName;
    int32 X = 0, Y = 0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("state_machine_name"), SMName) ||
        !Params->TryGetStringField(TEXT("conduit_name"), ConduitName))
        return MakeError(TEXT("animbp"), TEXT("add_conduit"), 1000,
            TEXT("Required: asset_path, state_machine_name, conduit_name"));
    Params->TryGetNumberField(TEXT("pos_x"), X);
    Params->TryGetNumberField(TEXT("pos_y"), Y);

    UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
    if (!AnimBP) return Result;
    FString Err;
    UAnimGraphNode_StateMachine* SM = FindStateMachineNode(AnimBP, SMName, Err);
    if (!SM) return MakeError(TEXT("animbp"), TEXT("add_conduit"), 2000, Err);
    UEdGraph* SMGraph = SM->EditorStateMachineGraph;

    UAnimStateConduitNode* Conduit = NewObject<UAnimStateConduitNode>(SMGraph);
    SMGraph->AddNode(Conduit, /*bUserAction=*/true, /*bSelectNewNode=*/false);
    Conduit->NodePosX = X; Conduit->NodePosY = Y;
    Conduit->CreateNewGuid();
    Conduit->PostPlacedNewNode();  // creates inner BoundGraph (the conduit's transition rule)
    Conduit->AllocateDefaultPins();
    if (Conduit->BoundGraph)
    {
        Conduit->BoundGraph->Rename(*ConduitName, nullptr, REN_DontCreateRedirectors | REN_DoNotDirty);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("conduit_name"), ConduitName);
    Data->SetStringField(TEXT("node_guid"), Conduit->NodeGuid.ToString());
    return MakeSuccess(TEXT("animbp"), TEXT("add_conduit"),
        FString::Printf(TEXT("Added conduit '%s'"), *ConduitName), Data);
}

FBridgeResult UAnimBlueprintHandler::Action_SetSequencePlayerSettings(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("animbp"), TEXT("set_sequence_player_settings"));
    FString AssetPath, NodeGuidStr, GraphName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
        return MakeError(TEXT("animbp"), TEXT("set_sequence_player_settings"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
        return MakeError(TEXT("animbp"), TEXT("set_sequence_player_settings"), 1000, TEXT("'node_guid' is required"));
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = LoadAnimBP(AssetPath, Result);
    if (!AnimBP) return Result;

    FGuid TargetGuid;
    if (!FGuid::Parse(NodeGuidStr, TargetGuid))
        return MakeError(TEXT("animbp"), TEXT("set_sequence_player_settings"), 1001, TEXT("Invalid node_guid"));

    // Search candidate graphs
    UAnimGraphNode_SequencePlayer* SeqNode = nullptr;
    auto SearchInGraph = [&](UEdGraph* G)
    {
        if (!G || SeqNode) return;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (UAnimGraphNode_SequencePlayer* S = Cast<UAnimGraphNode_SequencePlayer>(N))
            {
                if (S->NodeGuid == TargetGuid) { SeqNode = S; break; }
            }
        }
    };
    if (!GraphName.IsEmpty())
    {
        const FName GN(*GraphName);
        for (UEdGraph* G : AnimBP->FunctionGraphs) if (G && G->GetFName() == GN) { SearchInGraph(G); break; }
        if (!SeqNode) for (UEdGraph* G : AnimBP->MacroGraphs) if (G && G->GetFName() == GN) { SearchInGraph(G); break; }
    }
    if (!SeqNode)
    {
        FString Err;
        if (UEdGraph* AG = FindAnimGraph(AnimBP, Err)) SearchInGraph(AG);
    }
    if (!SeqNode)
        return MakeError(TEXT("animbp"), TEXT("set_sequence_player_settings"), 2000,
            FString::Printf(TEXT("UAnimGraphNode_SequencePlayer with guid '%s' not found"), *NodeGuidStr));

    double PlayRate = -1.0;
    bool bLoopAnimation = false;
    bool bSetLoop = Params->TryGetBoolField(TEXT("loop"), bLoopAnimation);
    bool bSetRate = Params->TryGetNumberField(TEXT("play_rate"), PlayRate);
    FString SequencePath;
    Params->TryGetStringField(TEXT("sequence_path"), SequencePath);

    FAnimNode_SequencePlayer& Inner = SeqNode->Node;
    if (bSetRate) Inner.SetPlayRate((float)PlayRate);
    if (bSetLoop) Inner.SetLoopAnimation(bLoopAnimation);
    if (!SequencePath.IsEmpty())
    {
        UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *SequencePath);
        if (!Seq)
        {
            const FString Suffix = SequencePath + TEXT(".") + FPackageName::GetLongPackageAssetName(SequencePath);
            Seq = LoadObject<UAnimSequence>(nullptr, *Suffix);
        }
        if (Seq) Inner.SetSequence(Seq);
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), NodeGuidStr);
    Data->SetBoolField(TEXT("set_play_rate"), bSetRate);
    Data->SetBoolField(TEXT("set_loop"), bSetLoop);
    Data->SetBoolField(TEXT("set_sequence"), !SequencePath.IsEmpty());
    return MakeSuccess(TEXT("animbp"), TEXT("set_sequence_player_settings"),
        FString::Printf(TEXT("Updated sequence player (rate=%s loop=%s seq=%s)"),
            bSetRate ? TEXT("y") : TEXT("n"),
            bSetLoop ? TEXT("y") : TEXT("n"),
            SequencePath.IsEmpty() ? TEXT("n") : TEXT("y")),
        Data);
}
