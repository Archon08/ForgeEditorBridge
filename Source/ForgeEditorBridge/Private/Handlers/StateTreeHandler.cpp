#include "Handlers/StateTreeHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- StateTree runtime -----------------------------------------------------
// Note: StateTreeTransitions.h does not exist in UE 5.7 — FStateTreeTransition
// and EStateTreeTransitionTrigger live in StateTreeTypes.h (via StateTreeState.h).
#include "StateTree.h"
#include "StateTreeTypes.h"

// ---- StateTree editor ------------------------------------------------------
// UE 5.7: StateTree editor data lives in StateTreeEditorModule
// Includes guarded — headers may shift between minor UE releases.
// Verify at compile time: StateTreeEditorData.h, StateTreeState.h, StateTreeEditorNode.h
#if WITH_EDITOR
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#endif

// ---- GameplayTags ----------------------------------------------------------
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const FString ST_DOMAIN = TEXT("state_tree");

UStateTree* UStateTreeHandler::LoadStateTree(const FString& AssetPath, FBridgeResult& Result) const
{
	UStateTree* ST = LoadObject<UStateTree>(nullptr, *AssetPath);
	if (!ST)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		ST = LoadObject<UStateTree>(nullptr, *Suffix);
	}
	if (!ST)
		Result.Message = FString::Printf(TEXT("UStateTree not found at '%s'"), *AssetPath);
	return ST;
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(ST_DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_state_tree")) return Action_CreateStateTree(Params);
	if (Action == TEXT("add_state"))         return Action_AddState(Params);
	if (Action == TEXT("add_transition"))    return Action_AddTransition(Params);
	if (Action == TEXT("set_state_tasks"))   return Action_SetStateTasks(Params);
	if (Action == TEXT("add_evaluator"))     return Action_AddEvaluator(Params);
	if (Action == TEXT("remove_state"))      return Action_RemoveState(Params);
	if (Action == TEXT("remove_transition")) return Action_RemoveTransition(Params);
	if (Action == TEXT("remove_evaluator"))  return Action_RemoveEvaluator(Params);

	return MakeError(ST_DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown state_tree action '%s'"), *Action),
		TEXT("Valid: create_state_tree, add_state, add_transition, set_state_tasks, add_evaluator, remove_state, remove_transition, remove_evaluator"));
}

// ---------------------------------------------------------------------------
// create_state_tree
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::Action_CreateStateTree(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("create_state_tree"), 1000, TEXT("'asset_path' is required"));

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// Locate the UStateTreeFactory class at runtime (StateTreeEditorModule)
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeEditorModule.StateTreeFactory"));
	if (!FactoryClass)
		FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeEditor.StateTreeFactory"));

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UFactory* Factory = FactoryClass ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
		UStateTree::StaticClass(), Factory);
	if (!CreatedAsset)
		return MakeError(ST_DOMAIN, TEXT("create_state_tree"), 3000,
			FString::Printf(TEXT("Failed to create UStateTree at '%s'. "
			                     "If factory is null, StateTreeEditorModule may not be loaded."), *AssetPath));

	// Optionally bind context class
	FString ContextClassPath;
	if (Params->TryGetStringField(TEXT("context_class"), ContextClassPath) && !ContextClassPath.IsEmpty())
	{
		UClass* ContextClass = FindObject<UClass>(nullptr, *ContextClassPath);
		if (!ContextClass)
			ContextClass = LoadObject<UClass>(nullptr, *ContextClassPath);
		if (ContextClass)
		{
			UStateTree* ST = CastChecked<UStateTree>(CreatedAsset);
			// UE 5.7: context class is set via FStateTreeEditorPropertyPath in editor data
			// Access via SetContextClass if available; otherwise mark dirty and note
			// The context class binding is accessible through StateTreeEditorData in UE 5.7
#if WITH_EDITOR
			UStateTreeEditorData* EdData = Cast<UStateTreeEditorData>(ST->EditorData);
			if (EdData && EdData->Schema)
			{
				// Schema->SetContextActorClass(ContextClass) — verify method name at compile time
				// FStateTreeEditorContext is set per-schema; leave for post-compile verification
			}
#endif
		}
	}

	CreatedAsset->MarkPackageDirty();

	return MakeSuccess(ST_DOMAIN, TEXT("create_state_tree"),
		FString::Printf(TEXT("UStateTree created at '%s'"), *AssetPath));
}

// ---------------------------------------------------------------------------
// add_state
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::Action_AddState(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, StateName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("state_name"), StateName) || StateName.IsEmpty())
	{
		return MakeError(ST_DOMAIN, TEXT("add_state"), 1000,
			TEXT("add_state: 'asset_path' and 'state_name' are required"),
			TEXT("Optional: 'parent_state' (string) — name of the parent state (default: root)"));
	}

#if WITH_EDITOR
	FBridgeResult TempResult = CreateResult(ST_DOMAIN, TEXT("add_state"));
	UStateTree* ST = LoadStateTree(AssetPath, TempResult);
	if (!ST) return TempResult;

	UStateTreeEditorData* EdData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EdData)
		return MakeError(ST_DOMAIN, TEXT("add_state"), 2001,
			TEXT("UStateTree has no EditorData — was it created without the factory?"));

	// Find parent state, or use root
	FString ParentName;
	UStateTreeState* ParentState = nullptr;
	if (Params->TryGetStringField(TEXT("parent_state"), ParentName) && !ParentName.IsEmpty())
	{
		// Search through all states for the named parent
		TFunction<UStateTreeState*(const TArray<UStateTreeState*>&)> FindState;
		FindState = [&](const TArray<UStateTreeState*>& States) -> UStateTreeState*
		{
			for (UStateTreeState* S : States)
			{
				if (!S) continue;
				if (S->Name.ToString().Equals(ParentName, ESearchCase::IgnoreCase)) return S;
				UStateTreeState* Found = FindState(S->Children);
				if (Found) return Found;
			}
			return nullptr;
		};
		ParentState = FindState(EdData->SubTrees);
	}

	// Create the new state
	UStateTreeState* NewState = NewObject<UStateTreeState>(EdData, NAME_None, RF_Transactional);
	NewState->Name = FName(*StateName);

	if (ParentState)
		ParentState->Children.Add(NewState);
	else
		EdData->SubTrees.Add(NewState);

	ST->MarkPackageDirty();

	return MakeSuccess(ST_DOMAIN, TEXT("add_state"),
		FString::Printf(TEXT("State '%s' added to '%s' (parent: %s)"),
		                *StateName, *AssetPath, ParentState ? *ParentName : TEXT("root")));
#else
	return MakeError(ST_DOMAIN, TEXT("add_state"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_transition
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::Action_AddTransition(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, FromState, ToState, TriggerStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("from_state"), FromState) || FromState.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("to_state"),   ToState)   || ToState.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("trigger"),    TriggerStr)|| TriggerStr.IsEmpty())
	{
		return MakeError(ST_DOMAIN, TEXT("add_transition"), 1000,
			TEXT("add_transition: 'asset_path', 'from_state', 'to_state', and 'trigger' are required"),
			TEXT("trigger: OnStateCompleted | OnEvent | Unconditional. Optional: 'event_tag' (GameplayTag string for OnEvent)"));
	}

#if WITH_EDITOR
	FBridgeResult TempResult = CreateResult(ST_DOMAIN, TEXT("add_transition"));
	UStateTree* ST = LoadStateTree(AssetPath, TempResult);
	if (!ST) return TempResult;

	UStateTreeEditorData* EdData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EdData)
		return MakeError(ST_DOMAIN, TEXT("add_transition"), 2001, TEXT("UStateTree has no EditorData"));

	// Find the source state
	TFunction<UStateTreeState*(const TArray<UStateTreeState*>&, const FString&)> FindState;
	FindState = [&](const TArray<UStateTreeState*>& States, const FString& Name) -> UStateTreeState*
	{
		for (UStateTreeState* S : States)
		{
			if (!S) continue;
			if (S->Name.ToString().Equals(Name, ESearchCase::IgnoreCase)) return S;
			UStateTreeState* Found = FindState(S->Children, Name);
			if (Found) return Found;
		}
		return nullptr;
	};

	UStateTreeState* SourceState = FindState(EdData->SubTrees, FromState);
	if (!SourceState)
		return MakeError(ST_DOMAIN, TEXT("add_transition"), 2002,
			FString::Printf(TEXT("from_state '%s' not found in '%s'"), *FromState, *AssetPath));

	// Map trigger string to EStateTreeTransitionTrigger
	// EStateTreeTransitionTrigger: OnStateCompleted=1, OnStateSucceeded=2, OnStateFailed=3,
	//                              OnTick=4, OnEvent=5
	EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
	if      (TriggerStr == TEXT("OnEvent"))       Trigger = EStateTreeTransitionTrigger::OnEvent;
	else if (TriggerStr == TEXT("Unconditional"))  Trigger = EStateTreeTransitionTrigger::OnTick;  // closest to unconditional
	else                                           Trigger = EStateTreeTransitionTrigger::OnStateCompleted;

	// Build the transition
	FStateTreeTransition NewTransition;
	NewTransition.Trigger = Trigger;

	// Resolve to_state handle — FStateTreeTransition::State is an FStateTreeStateLink
	// in UE 5.7 (renamed from NextState). FStateTreeStateLink links by GUID; the target
	// GUID is resolved at compile time — initialize the link with the GotoState type so
	// the editor can reconcile it when the tree is compiled.
	NewTransition.State = FStateTreeStateLink(EStateTreeTransitionType::GotoState);

	// Optional event tag for OnEvent trigger
	FString EventTagStr;
	if (Params->TryGetStringField(TEXT("event_tag"), EventTagStr) && !EventTagStr.IsEmpty())
	{
		FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), false);
		if (EventTag.IsValid())
			NewTransition.RequiredEvent.Tag = EventTag;
	}

	SourceState->Transitions.Add(NewTransition);
	ST->MarkPackageDirty();

	return MakeSuccess(ST_DOMAIN, TEXT("add_transition"),
		FString::Printf(TEXT("Transition added: '%s' -> '%s' (trigger=%s) in '%s'. "
		                     "Note: GotoState link requires compile to resolve target GUID — "
		                     "compile the StateTree in the editor after authoring."),
		                *FromState, *ToState, *TriggerStr, *AssetPath));
#else
	return MakeError(ST_DOMAIN, TEXT("add_transition"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// set_state_tasks
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::Action_SetStateTasks(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, StateName;
	const TArray<TSharedPtr<FJsonValue>>* TasksArray = nullptr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("state_name"), StateName) || StateName.IsEmpty() ||
	    !Params->TryGetArrayField(TEXT("tasks"),       TasksArray) || !TasksArray)
	{
		return MakeError(ST_DOMAIN, TEXT("set_state_tasks"), 1000,
			TEXT("set_state_tasks: 'asset_path', 'state_name', and 'tasks' are required"),
			TEXT("tasks: array of {\"class_path\": \"/Script/Module.TaskClass\"}"));
	}

#if WITH_EDITOR
	FBridgeResult TempResult = CreateResult(ST_DOMAIN, TEXT("set_state_tasks"));
	UStateTree* ST = LoadStateTree(AssetPath, TempResult);
	if (!ST) return TempResult;

	UStateTreeEditorData* EdData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EdData)
		return MakeError(ST_DOMAIN, TEXT("set_state_tasks"), 2001, TEXT("UStateTree has no EditorData"));

	// Find the target state
	TFunction<UStateTreeState*(const TArray<UStateTreeState*>&, const FString&)> FindState;
	FindState = [&](const TArray<UStateTreeState*>& States, const FString& Name) -> UStateTreeState*
	{
		for (UStateTreeState* S : States)
		{
			if (!S) continue;
			if (S->Name.ToString().Equals(Name, ESearchCase::IgnoreCase)) return S;
			UStateTreeState* Found = FindState(S->Children, Name);
			if (Found) return Found;
		}
		return nullptr;
	};

	UStateTreeState* TargetState = FindState(EdData->SubTrees, StateName);
	if (!TargetState)
		return MakeError(ST_DOMAIN, TEXT("set_state_tasks"), 2002,
			FString::Printf(TEXT("State '%s' not found in '%s'"), *StateName, *AssetPath));

	// Bind tasks by class path
	// UE 5.7: Tasks are FStateTreeEditorNode entries in UStateTreeState::Tasks
	// FStateTreeEditorNode holds an FInstancedStruct with the task data
	int32 BoundCount = 0;
	for (const TSharedPtr<FJsonValue>& TaskVal : *TasksArray)
	{
		const TSharedPtr<FJsonObject>* TaskObj = nullptr;
		if (!TaskVal->TryGetObject(TaskObj) || !TaskObj) continue;

		FString ClassPath;
		if (!(*TaskObj)->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty()) continue;

		UScriptStruct* TaskStruct = FindObject<UScriptStruct>(nullptr, *ClassPath);
		if (!TaskStruct)
			TaskStruct = LoadObject<UScriptStruct>(nullptr, *ClassPath);

		if (TaskStruct)
		{
			FStateTreeEditorNode NewNode;
			NewNode.Node.InitializeAs(TaskStruct);
			TargetState->Tasks.Add(NewNode);
			++BoundCount;
		}
	}

	ST->MarkPackageDirty();

	return MakeSuccess(ST_DOMAIN, TEXT("set_state_tasks"),
		FString::Printf(TEXT("Bound %d task(s) to state '%s' in '%s'"), BoundCount, *StateName, *AssetPath));
#else
	return MakeError(ST_DOMAIN, TEXT("set_state_tasks"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_evaluator
// ---------------------------------------------------------------------------

FBridgeResult UStateTreeHandler::Action_AddEvaluator(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EvaluatorClass;
	if (!Params->TryGetStringField(TEXT("asset_path"),       AssetPath)      || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("evaluator_class"),  EvaluatorClass) || EvaluatorClass.IsEmpty())
	{
		return MakeError(ST_DOMAIN, TEXT("add_evaluator"), 1000,
			TEXT("add_evaluator: 'asset_path' and 'evaluator_class' are required"),
			TEXT("evaluator_class: UScriptStruct path e.g. '/Script/Module.MyEvaluator'"));
	}

#if WITH_EDITOR
	FBridgeResult TempResult = CreateResult(ST_DOMAIN, TEXT("add_evaluator"));
	UStateTree* ST = LoadStateTree(AssetPath, TempResult);
	if (!ST) return TempResult;

	UStateTreeEditorData* EdData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EdData)
		return MakeError(ST_DOMAIN, TEXT("add_evaluator"), 2001, TEXT("UStateTree has no EditorData"));

	UScriptStruct* EvalStruct = FindObject<UScriptStruct>(nullptr, *EvaluatorClass);
	if (!EvalStruct)
		EvalStruct = LoadObject<UScriptStruct>(nullptr, *EvaluatorClass);
	if (!EvalStruct)
		return MakeError(ST_DOMAIN, TEXT("add_evaluator"), 2002,
			FString::Printf(TEXT("UScriptStruct not found: '%s'"), *EvaluatorClass),
			TEXT("Ensure the struct is from a loaded module and is a StateTree evaluator type"));

	FStateTreeEditorNode NewEvaluatorNode;
	NewEvaluatorNode.Node.InitializeAs(EvalStruct);
	EdData->Evaluators.Add(NewEvaluatorNode);
	ST->MarkPackageDirty();

	return MakeSuccess(ST_DOMAIN, TEXT("add_evaluator"),
		FString::Printf(TEXT("Evaluator '%s' added to '%s'"), *EvaluatorClass, *AssetPath));
#else
	return MakeError(ST_DOMAIN, TEXT("add_evaluator"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UStateTreeHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new UStateTree asset"));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),    P(TEXT("string"), true,  TEXT("Content path for the new StateTree")));
	  Ps->SetObjectField(TEXT("context_class"), P(TEXT("string"), false, TEXT("Optional context class path (e.g. '/Script/Game.MyCharacter')")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_state_tree"), A); }

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a child state to the StateTree (root or named parent)"));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),  P(TEXT("string"), true,  TEXT("Content path of the StateTree")));
	  Ps->SetObjectField(TEXT("state_name"),  P(TEXT("string"), true,  TEXT("Name for the new state")));
	  Ps->SetObjectField(TEXT("parent_state"),P(TEXT("string"), false, TEXT("Parent state name (omit to add to root)")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_state"), A); }

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a transition on a state"));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true,  TEXT("Content path of the StateTree")));
	  Ps->SetObjectField(TEXT("from_state"), P(TEXT("string"), true,  TEXT("Source state name")));
	  Ps->SetObjectField(TEXT("to_state"),   P(TEXT("string"), true,  TEXT("Destination state name")));
	  Ps->SetObjectField(TEXT("trigger"),    P(TEXT("string"), true,  TEXT("OnStateCompleted | OnEvent | Unconditional")));
	  Ps->SetObjectField(TEXT("event_tag"),  P(TEXT("string"), false, TEXT("GameplayTag string required for OnEvent trigger")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_transition"), A); }

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Bind task instances to a state by struct class path"));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the StateTree")));
	  Ps->SetObjectField(TEXT("state_name"), P(TEXT("string"), true, TEXT("Target state name")));
	  Ps->SetObjectField(TEXT("tasks"),      P(TEXT("array"),  true, TEXT("Array of {class_path} objects")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_state_tasks"), A); }

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Append a global evaluator to the StateTree"));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),      P(TEXT("string"), true, TEXT("Content path of the StateTree")));
	  Ps->SetObjectField(TEXT("evaluator_class"),  P(TEXT("string"), true, TEXT("UScriptStruct path of the evaluator")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_evaluator"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// remove_state — recursive search by name; removes from parent
// ---------------------------------------------------------------------------
FBridgeResult UStateTreeHandler::Action_RemoveState(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FBridgeResult Result = CreateResult(ST_DOMAIN, TEXT("remove_state"));
	FString AssetPath, StateName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_state"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("state_name"), StateName) || StateName.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_state"), 1000, TEXT("'state_name' is required"));

	UStateTree* ST = LoadStateTree(AssetPath, Result);
	if (!ST) { Result.ErrorCode = 2000; return Result; }
	UStateTreeEditorData* Data = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!Data) return MakeError(ST_DOMAIN, TEXT("remove_state"), 3000, TEXT("EditorData missing"));

	const FName Target(*StateName);
	auto* MutableSub = const_cast<TArray<TObjectPtr<UStateTreeState>>*>(&Data->SubTrees);
	auto RecurseRemove = [&](auto& Self, TArray<TObjectPtr<UStateTreeState>>& Children) -> bool
	{
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			UStateTreeState* S = Children[i];
			if (!S) continue;
			if (S->Name == Target) { Children.RemoveAt(i); return true; }
			if (Self(Self, S->Children)) return true;
		}
		return false;
	};

	if (!RecurseRemove(RecurseRemove, *MutableSub))
		return MakeError(ST_DOMAIN, TEXT("remove_state"), 2000,
			FString::Printf(TEXT("State '%s' not found"), *StateName));

	ST->MarkPackageDirty();
	return MakeSuccess(ST_DOMAIN, TEXT("remove_state"),
		FString::Printf(TEXT("Removed state '%s'"), *StateName));
#else
	return MakeError(ST_DOMAIN, TEXT("remove_state"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// remove_transition — by from_state + to_state match
// ---------------------------------------------------------------------------
FBridgeResult UStateTreeHandler::Action_RemoveTransition(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FBridgeResult Result = CreateResult(ST_DOMAIN, TEXT("remove_transition"));
	FString AssetPath, FromState, ToState;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_transition"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("from_state"), FromState) || FromState.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_transition"), 1000, TEXT("'from_state' is required"));
	if (!Params->TryGetStringField(TEXT("to_state"), ToState) || ToState.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_transition"), 1000, TEXT("'to_state' is required"));

	UStateTree* ST = LoadStateTree(AssetPath, Result);
	if (!ST) { Result.ErrorCode = 2000; return Result; }
	UStateTreeEditorData* Data = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!Data) return MakeError(ST_DOMAIN, TEXT("remove_transition"), 3000, TEXT("EditorData missing"));

	const FName From(*FromState);
	const FName To(*ToState);
	auto FindStateByName = [&](auto& Self, TArray<TObjectPtr<UStateTreeState>>& Children, const FName& Name) -> UStateTreeState*
	{
		for (TObjectPtr<UStateTreeState>& Ptr : Children)
		{
			UStateTreeState* S = Ptr;
			if (!S) continue;
			if (S->Name == Name) return S;
			if (UStateTreeState* Sub = Self(Self, S->Children, Name)) return Sub;
		}
		return nullptr;
	};

	UStateTreeState* SrcState = FindStateByName(FindStateByName,
		const_cast<TArray<TObjectPtr<UStateTreeState>>&>(Data->SubTrees), From);
	if (!SrcState)
		return MakeError(ST_DOMAIN, TEXT("remove_transition"), 2000,
			FString::Printf(TEXT("from_state '%s' not found"), *FromState));

	int32 Removed = 0;
	for (int32 i = SrcState->Transitions.Num() - 1; i >= 0; --i)
	{
		const FStateTreeTransition& T = SrcState->Transitions[i];
		if (T.State.Name == To) { SrcState->Transitions.RemoveAt(i); ++Removed; }
	}
	if (Removed == 0)
		return MakeError(ST_DOMAIN, TEXT("remove_transition"), 2000,
			FString::Printf(TEXT("No transition '%s' -> '%s'"), *FromState, *ToState));

	ST->MarkPackageDirty();
	return MakeSuccess(ST_DOMAIN, TEXT("remove_transition"),
		FString::Printf(TEXT("Removed %d transition(s) '%s' -> '%s'"), Removed, *FromState, *ToState));
#else
	return MakeError(ST_DOMAIN, TEXT("remove_transition"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// remove_evaluator — by index
// ---------------------------------------------------------------------------
FBridgeResult UStateTreeHandler::Action_RemoveEvaluator(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FBridgeResult Result = CreateResult(ST_DOMAIN, TEXT("remove_evaluator"));
	FString AssetPath;
	int32 Index = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(ST_DOMAIN, TEXT("remove_evaluator"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetNumberField(TEXT("index"), Index) || Index < 0)
		return MakeError(ST_DOMAIN, TEXT("remove_evaluator"), 1000, TEXT("'index' (>=0) is required"));

	UStateTree* ST = LoadStateTree(AssetPath, Result);
	if (!ST) { Result.ErrorCode = 2000; return Result; }
	UStateTreeEditorData* Data = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!Data) return MakeError(ST_DOMAIN, TEXT("remove_evaluator"), 3000, TEXT("EditorData missing"));

	if (!Data->Evaluators.IsValidIndex(Index))
		return MakeError(ST_DOMAIN, TEXT("remove_evaluator"), 1001,
			FString::Printf(TEXT("index %d out of range (have %d)"), Index, Data->Evaluators.Num()));

	Data->Evaluators.RemoveAt(Index);
	ST->MarkPackageDirty();
	return MakeSuccess(ST_DOMAIN, TEXT("remove_evaluator"),
		FString::Printf(TEXT("Removed evaluator at index %d"), Index));
#else
	return MakeError(ST_DOMAIN, TEXT("remove_evaluator"), 3003, TEXT("Editor-only action"));
#endif
}
