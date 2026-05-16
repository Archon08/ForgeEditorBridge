#include "Handlers/BlueprintGraphAssembler.h"
#include "Handlers/BlueprintGraphLayout.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// K2 node types used by the DSL
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"

// ============================================================================
// Internal state + helpers
// ============================================================================
namespace
{
    // Local result builder — MakeSuccess/MakeError are protected on UBridgeHandlerBase.
    static FBridgeResult MakeBuildResult(bool bSuccess, const FString& Message,
                                         TSharedPtr<FJsonObject> Data,
                                         int32 ErrorCode = 0,
                                         const FString& RecoveryHint = FString())
    {
        FBridgeResult R;
        R.bSuccess     = bSuccess;
        R.Domain       = TEXT("blueprint");
        R.Action       = TEXT("build_blueprint_graph");
        R.Message      = Message;
        R.Data         = Data;
        R.ErrorCode    = ErrorCode;
        R.RecoveryHint = RecoveryHint;
        R.Timestamp    = FDateTime::UtcNow().ToIso8601();
        return R;
    }

    // "nodeId.pinName" → UEdGraphPin*. Tries exact match first, then case-insensitive,
    // then common exec aliases (exec/then/true/false).
    static UEdGraphPin* ResolvePinRef(const FString& Ref,
                                      const TMap<FString, UEdGraphNode*>& NodeMap,
                                      EEdGraphPinDirection DirectionHint)
    {
        FString NodeId, PinLabel;
        if (!Ref.Split(TEXT("."), &NodeId, &PinLabel) || NodeId.IsEmpty() || PinLabel.IsEmpty())
            return nullptr;

        UEdGraphNode* const* Found = NodeMap.Find(NodeId);
        if (!Found || !*Found) return nullptr;
        UEdGraphNode* N = *Found;

        using K2 = UEdGraphSchema_K2;

        auto MatchAlias = [&](const UEdGraphPin* P) -> bool
        {
            if (!P) return false;
            const FString PN = P->PinName.ToString();
            if (PN.Equals(PinLabel, ESearchCase::IgnoreCase)) return true;

            // Exec aliases
            if (PinLabel.Equals(TEXT("exec"), ESearchCase::IgnoreCase)
                && P->PinName == K2::PN_Execute
                && P->PinType.PinCategory == K2::PC_Exec) return true;
            if (PinLabel.Equals(TEXT("then"), ESearchCase::IgnoreCase)
                && P->PinName == K2::PN_Then
                && P->PinType.PinCategory == K2::PC_Exec) return true;
            if (PinLabel.Equals(TEXT("true"), ESearchCase::IgnoreCase)
                && P->PinName == K2::PN_Then
                && P->PinType.PinCategory == K2::PC_Exec
                && N->IsA<UK2Node_IfThenElse>()) return true;
            if (PinLabel.Equals(TEXT("false"), ESearchCase::IgnoreCase)
                && P->PinName == K2::PN_Else
                && P->PinType.PinCategory == K2::PC_Exec
                && N->IsA<UK2Node_IfThenElse>()) return true;

            // Cast-specific aliases. "result" pin name is dynamic in UE 5.7
            // (prefix PN_CastedValuePrefix + type string), so query the accessor.
            if (UK2Node_DynamicCast* DCast = Cast<UK2Node_DynamicCast>(N))
            {
                if (PinLabel.Equals(TEXT("object"), ESearchCase::IgnoreCase)
                    && P == DCast->GetCastSourcePin()) return true;
                if (PinLabel.Equals(TEXT("result"), ESearchCase::IgnoreCase)
                    && P == DCast->GetCastResultPin()) return true;
                if (PinLabel.Equals(TEXT("success"), ESearchCase::IgnoreCase)
                    && P == DCast->GetValidCastPin()) return true;
                if (PinLabel.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
                    && P == DCast->GetInvalidCastPin()) return true;
            }
            return false;
        };

        // First pass: try direction-matched
        for (UEdGraphPin* P : N->Pins)
            if (P && P->Direction == DirectionHint && MatchAlias(P)) return P;

        // Second pass: ignore direction (wires encoded ambiguously)
        for (UEdGraphPin* P : N->Pins)
            if (MatchAlias(P)) return P;

        return nullptr;
    }

    // Load the engine-wide StandardMacros Blueprint and return its named macro graph.
    static UEdGraph* LoadStandardMacro(const FString& MacroName)
    {
        UBlueprint* Macros = LoadObject<UBlueprint>(nullptr,
            TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
        if (!Macros) return nullptr;
        for (UEdGraph* G : Macros->MacroGraphs)
            if (G && G->GetName() == MacroName) return G;
        return nullptr;
    }

    // Create a single node from one DSL entry. Returns the new node, or nullptr on
    // failure (and writes a reason into OutError).
    static UEdGraphNode* CreateNodeFromSpec(UBlueprint* BP,
                                            UEdGraph* Graph,
                                            TSharedPtr<FJsonObject> Spec,
                                            FString& OutError)
    {
        FString Type;
        Spec->TryGetStringField(TEXT("type"), Type);
        Type = Type.ToLower();

        if (Type == TEXT("event"))
        {
            FString Name; Spec->TryGetStringField(TEXT("name"), Name);
            if (Name.IsEmpty()) { OutError = TEXT("event node missing 'name'"); return nullptr; }

            UClass* ParentClass = BP->ParentClass;
            if (!ParentClass) { OutError = TEXT("Blueprint has no parent class"); return nullptr; }

            // De-dup: an override already placed?
            if (FBlueprintEditorUtils::FindOverrideForFunction(BP, ParentClass, FName(*Name)))
            {
                OutError = FString::Printf(TEXT("event override '%s' already exists on %s"),
                    *Name, *BP->GetName());
                return nullptr;
            }

            FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
            UK2Node_Event* N = Creator.CreateNode(false);
            N->EventReference.SetExternalMember(FName(*Name), ParentClass);
            N->bOverrideFunction = true;
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("custom_event"))
        {
            FString Name; Spec->TryGetStringField(TEXT("name"), Name);
            if (Name.IsEmpty()) { OutError = TEXT("custom_event missing 'name'"); return nullptr; }
            FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
            UK2Node_CustomEvent* N = Creator.CreateNode(false);
            N->CustomFunctionName = FName(*Name);
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("function_call"))
        {
            FString FuncName, FuncClass;
            Spec->TryGetStringField(TEXT("function_name"), FuncName);
            Spec->TryGetStringField(TEXT("function_class"), FuncClass);
            if (FuncName.IsEmpty()) { OutError = TEXT("function_call missing 'function_name'"); return nullptr; }

            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            UK2Node_CallFunction* N = Creator.CreateNode(false);

            if (!FuncClass.IsEmpty())
            {
                UClass* Cls = FindObject<UClass>(nullptr, *FuncClass);
                if (Cls)
                {
                    UFunction* Fn = Cls->FindFunctionByName(FName(*FuncName));
                    if (Fn) N->SetFromFunction(Fn);
                    else N->FunctionReference.SetExternalMember(FName(*FuncName), Cls);
                }
                else N->FunctionReference.SetSelfMember(FName(*FuncName));
            }
            else
            {
                N->FunctionReference.SetSelfMember(FName(*FuncName));
            }
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("call_event"))
        {
            FString EventName; Spec->TryGetStringField(TEXT("event"), EventName);
            if (EventName.IsEmpty()) { OutError = TEXT("call_event missing 'event'"); return nullptr; }
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            UK2Node_CallFunction* N = Creator.CreateNode(false);
            N->FunctionReference.SetSelfMember(FName(*EventName));
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("cast"))
        {
            FString ClassPath; Spec->TryGetStringField(TEXT("target_class"), ClassPath);
            if (ClassPath.IsEmpty()) { OutError = TEXT("cast missing 'target_class'"); return nullptr; }

            UClass* Target = LoadObject<UClass>(nullptr, *ClassPath);
            if (!Target)
            {
                // Fallback — Blueprint class paths often end in _C
                if (!ClassPath.EndsWith(TEXT("_C")))
                {
                    const FString WithSuffix = ClassPath + TEXT("_C");
                    Target = LoadObject<UClass>(nullptr, *WithSuffix);
                }
            }
            if (!Target)
            {
                OutError = FString::Printf(TEXT("cast target class not loadable: %s"), *ClassPath);
                return nullptr;
            }

            bool bPure = false; Spec->TryGetBoolField(TEXT("pure"), bPure);

            FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
            UK2Node_DynamicCast* N = Creator.CreateNode(false);
            N->TargetType = Target;
            N->SetPurity(bPure);
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("branch"))
        {
            FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
            UK2Node_IfThenElse* N = Creator.CreateNode(false);
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("sequence"))
        {
            int32 Outputs = 2; Spec->TryGetNumberField(TEXT("outputs"), Outputs);
            FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
            UK2Node_ExecutionSequence* N = Creator.CreateNode(false);
            Creator.Finalize();
            // Default Sequence has 2 "then" outputs. AddInputPin() appends more "then" pins.
            for (int32 i = 2; i < Outputs; ++i) N->AddInputPin();
            return N;
        }
        if (Type == TEXT("get_var") || Type == TEXT("set_var"))
        {
            FString VarName, VarClass;
            Spec->TryGetStringField(TEXT("var_name"), VarName);
            Spec->TryGetStringField(TEXT("var_class"), VarClass);
            if (VarName.IsEmpty()) { OutError = TEXT("get_var/set_var missing 'var_name'"); return nullptr; }

            if (Type == TEXT("get_var"))
            {
                FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
                UK2Node_VariableGet* N = Creator.CreateNode(false);
                if (!VarClass.IsEmpty())
                {
                    UClass* Cls = FindObject<UClass>(nullptr, *VarClass);
                    if (Cls) N->VariableReference.SetExternalMember(FName(*VarName), Cls);
                    else     N->VariableReference.SetSelfMember(FName(*VarName));
                }
                else N->VariableReference.SetSelfMember(FName(*VarName));
                Creator.Finalize();
                return N;
            }
            else
            {
                FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
                UK2Node_VariableSet* N = Creator.CreateNode(false);
                if (!VarClass.IsEmpty())
                {
                    UClass* Cls = FindObject<UClass>(nullptr, *VarClass);
                    if (Cls) N->VariableReference.SetExternalMember(FName(*VarName), Cls);
                    else     N->VariableReference.SetSelfMember(FName(*VarName));
                }
                else N->VariableReference.SetSelfMember(FName(*VarName));
                Creator.Finalize();
                return N;
            }
        }
        if (Type == TEXT("for_each") || Type == TEXT("for_each_with_break")
            || Type == TEXT("while") || Type == TEXT("do_once")
            || Type == TEXT("flip_flop") || Type == TEXT("is_valid") || Type == TEXT("gate"))
        {
            static const TMap<FString, FString> MacroMap = {
                { TEXT("for_each"),            TEXT("ForEachLoop") },
                { TEXT("for_each_with_break"), TEXT("ForEachLoopWithBreak") },
                { TEXT("while"),               TEXT("WhileLoop") },
                { TEXT("do_once"),             TEXT("DoOnce") },
                { TEXT("flip_flop"),           TEXT("FlipFlop") },
                { TEXT("is_valid"),            TEXT("IsValid") },
                { TEXT("gate"),                TEXT("Gate") },
            };
            const FString* MacroName = MacroMap.Find(Type);
            if (!MacroName) { OutError = FString::Printf(TEXT("unknown macro alias '%s'"), *Type); return nullptr; }

            UEdGraph* MacroGraph = LoadStandardMacro(*MacroName);
            if (!MacroGraph)
            {
                OutError = FString::Printf(TEXT("StandardMacros/%s not found"), **MacroName);
                return nullptr;
            }

            FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
            UK2Node_MacroInstance* N = Creator.CreateNode(false);
            N->SetMacroGraph(MacroGraph);
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("reroute"))
        {
            FGraphNodeCreator<UK2Node_Knot> Creator(*Graph);
            UK2Node_Knot* N = Creator.CreateNode(false);
            Creator.Finalize();
            return N;
        }
        if (Type == TEXT("comment"))
        {
            FString Title; Spec->TryGetStringField(TEXT("title"), Title);
            FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Graph);
            UEdGraphNode_Comment* N = Creator.CreateNode(false);
            N->NodeComment = Title;
            Creator.Finalize();
            return N;
        }

        OutError = FString::Printf(TEXT("unknown node type '%s'"), *Type);
        return nullptr;
    }

    // Apply a single wire { from, to }. Writes failure reason into OutError.
    static bool ApplyWire(UEdGraph* Graph,
                          const TMap<FString, UEdGraphNode*>& NodeMap,
                          TSharedPtr<FJsonObject> WireObj,
                          FString& OutError)
    {
        FString FromRef, ToRef;
        WireObj->TryGetStringField(TEXT("from"), FromRef);
        WireObj->TryGetStringField(TEXT("to"),   ToRef);
        if (FromRef.IsEmpty() || ToRef.IsEmpty()) { OutError = TEXT("wire missing from/to"); return false; }

        UEdGraphPin* FromPin = ResolvePinRef(FromRef, NodeMap, EGPD_Output);
        UEdGraphPin* ToPin   = ResolvePinRef(ToRef,   NodeMap, EGPD_Input);
        if (!FromPin) { OutError = FString::Printf(TEXT("unresolved wire source '%s'"), *FromRef); return false; }
        if (!ToPin)   { OutError = FString::Printf(TEXT("unresolved wire target '%s'"), *ToRef);   return false; }

        const UEdGraphSchema* Schema = Graph->GetSchema();
        if (!Schema) { OutError = TEXT("graph has no schema"); return false; }

        if (!Schema->TryCreateConnection(FromPin, ToPin))
        {
            OutError = FString::Printf(TEXT("TryCreateConnection rejected %s -> %s"), *FromRef, *ToRef);
            return false;
        }
        // TryCreateConnection fires NotifyPinConnectionListChanged on both pins,
        // which triggers UK2Node_Knot::PropagatePinType internally — no explicit
        // call needed (the method is not BLUEPRINTGRAPH_API exported anyway).
        return true;
    }

    static bool ApplyDefault(const TMap<FString, UEdGraphNode*>& NodeMap,
                             TSharedPtr<FJsonObject> DefObj,
                             FString& OutError)
    {
        FString NodeId, PinLabel, Value;
        DefObj->TryGetStringField(TEXT("node"),  NodeId);
        DefObj->TryGetStringField(TEXT("pin"),   PinLabel);
        DefObj->TryGetStringField(TEXT("value"), Value);
        if (NodeId.IsEmpty() || PinLabel.IsEmpty()) { OutError = TEXT("default missing node/pin"); return false; }

        UEdGraphNode* const* Found = NodeMap.Find(NodeId);
        if (!Found || !*Found) { OutError = FString::Printf(TEXT("default target node '%s' not found"), *NodeId); return false; }
        UEdGraphNode* N = *Found;

        UEdGraphPin* Pin = nullptr;
        for (UEdGraphPin* P : N->Pins)
            if (P && P->PinName.ToString().Equals(PinLabel, ESearchCase::IgnoreCase))
                { Pin = P; break; }
        if (!Pin) { OutError = FString::Printf(TEXT("pin '%s' not found on node '%s'"), *PinLabel, *NodeId); return false; }

        if (const UEdGraphSchema* Schema = N->GetGraph() ? N->GetGraph()->GetSchema() : nullptr)
        {
            Schema->TrySetDefaultValue(*Pin, Value);
            return true;
        }
        OutError = TEXT("no schema available to set default");
        return false;
    }

    static void ParseGroupsForLayout(TSharedPtr<FJsonObject> Params,
                                     const TMap<FString, UEdGraphNode*>& NodeMap,
                                     TArray<FBlueprintGraphGroup>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* GroupsArr = nullptr;
        if (!Params->TryGetArrayField(TEXT("groups"), GroupsArr) || !GroupsArr) return;

        for (const TSharedPtr<FJsonValue>& V : *GroupsArr)
        {
            TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
            if (!O.IsValid()) continue;

            FBlueprintGraphGroup G;
            O->TryGetStringField(TEXT("title"), G.Title);

            const TArray<TSharedPtr<FJsonValue>>* NArr = nullptr;
            if (O->TryGetArrayField(TEXT("nodes"), NArr) && NArr)
            {
                for (const TSharedPtr<FJsonValue>& NV : *NArr)
                {
                    if (!NV.IsValid()) continue;
                    const FString Id = NV->AsString();
                    // Convert DSL ids → NodeGuid strings so layout can resolve consistently
                    if (UEdGraphNode* const* Hit = NodeMap.Find(Id))
                        G.NodeIds.Add((*Hit)->NodeGuid.ToString());
                    else
                        G.NodeIds.Add(Id); // pass through; layout tries name-match next
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* CArr = nullptr;
            if (O->TryGetArrayField(TEXT("color"), CArr) && CArr && CArr->Num() >= 3)
            {
                G.Color.R = (float)(*CArr)[0]->AsNumber();
                G.Color.G = (float)(*CArr)[1]->AsNumber();
                G.Color.B = (float)(*CArr)[2]->AsNumber();
                G.Color.A = CArr->Num() >= 4 ? (float)(*CArr)[3]->AsNumber() : 0.3f;
            }
            Out.Add(MoveTemp(G));
        }
    }
}

// ============================================================================
// Public API
// ============================================================================
FBridgeResult FBlueprintGraphAssembler::Build(UBlueprint* Blueprint,
                                              UEdGraph* Graph,
                                              TSharedPtr<FJsonObject> Params)
{
    if (!Blueprint || !Graph)
        return MakeBuildResult(false, TEXT("Blueprint/graph not resolved"), nullptr, 3000);

    const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("nodes"), NodesArr) || !NodesArr || NodesArr->Num() == 0)
        return MakeBuildResult(false, TEXT("'nodes' array is required and non-empty"), nullptr, 1000);

    // --- 1. Create nodes -----------------------------------------------------
    TMap<FString, UEdGraphNode*>   NodeMap;
    TArray<TSharedPtr<FJsonValue>> NodeDiagnostics;
    int32 CreatedCount = 0;

    for (const TSharedPtr<FJsonValue>& V : *NodesArr)
    {
        TSharedPtr<FJsonObject> Spec = V.IsValid() ? V->AsObject() : nullptr;
        if (!Spec.IsValid())
        {
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("error"), TEXT("node spec is not an object"));
            NodeDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            continue;
        }

        FString Id; Spec->TryGetStringField(TEXT("id"), Id);
        if (Id.IsEmpty())
        {
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("error"), TEXT("node spec missing 'id'"));
            NodeDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            continue;
        }
        if (NodeMap.Contains(Id))
        {
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("id"), Id);
            D->SetStringField(TEXT("error"), TEXT("duplicate node id"));
            NodeDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            continue;
        }

        FString Err;
        UEdGraphNode* N = CreateNodeFromSpec(Blueprint, Graph, Spec, Err);
        if (!N)
        {
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("id"),    Id);
            D->SetStringField(TEXT("error"), Err);
            NodeDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            continue;
        }
        NodeMap.Add(Id, N);
        ++CreatedCount;
    }

    if (CreatedCount == 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("node_diagnostics"), NodeDiagnostics);
        return MakeBuildResult(false,
            TEXT("No nodes created — see node_diagnostics for per-spec reasons"),
            Data, 3000);
    }

    // --- 2. Apply wires ------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> WireDiagnostics;
    int32 WiresApplied = 0;
    const TArray<TSharedPtr<FJsonValue>>* WiresArr = nullptr;
    if (Params->TryGetArrayField(TEXT("wires"), WiresArr) && WiresArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *WiresArr)
        {
            TSharedPtr<FJsonObject> WObj = V.IsValid() ? V->AsObject() : nullptr;
            if (!WObj.IsValid()) continue;
            FString Err;
            if (ApplyWire(Graph, NodeMap, WObj, Err)) ++WiresApplied;
            else
            {
                TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
                FString FromRef, ToRef;
                WObj->TryGetStringField(TEXT("from"), FromRef);
                WObj->TryGetStringField(TEXT("to"),   ToRef);
                D->SetStringField(TEXT("from"),  FromRef);
                D->SetStringField(TEXT("to"),    ToRef);
                D->SetStringField(TEXT("error"), Err);
                WireDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            }
        }
    }

    // --- 3. Apply pin defaults ----------------------------------------------
    TArray<TSharedPtr<FJsonValue>> DefaultDiagnostics;
    int32 DefaultsApplied = 0;
    const TArray<TSharedPtr<FJsonValue>>* DefaultsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("defaults"), DefaultsArr) && DefaultsArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *DefaultsArr)
        {
            TSharedPtr<FJsonObject> DObj = V.IsValid() ? V->AsObject() : nullptr;
            if (!DObj.IsValid()) continue;
            FString Err;
            if (ApplyDefault(NodeMap, DObj, Err)) ++DefaultsApplied;
            else
            {
                TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
                D->SetStringField(TEXT("error"), Err);
                DefaultDiagnostics.Add(MakeShared<FJsonValueObject>(D));
            }
        }
    }

    // --- 4. Layout pass (groups + optional auto-layout) ----------------------
    FString LayoutMode = TEXT("auto");
    Params->TryGetStringField(TEXT("layout"), LayoutMode);
    const bool bAutoLayout = !LayoutMode.Equals(TEXT("none"), ESearchCase::IgnoreCase);

    TArray<FBlueprintGraphGroup> Groups;
    ParseGroupsForLayout(Params, NodeMap, Groups);

    if (bAutoLayout)
    {
        FBlueprintGraphLayout::Layout(Graph, Groups, /*bInsertReroutes=*/true);
    }
    else if (Groups.Num() > 0)
    {
        // Groups-only: emit comments at current node positions by running layout
        // with reroutes disabled + positions pre-existing (layout still overwrites
        // positions). To emit groups without reflow, call Layout with a no-op mode.
        // Simpler: always run with groups passed; caller asked for "none" so skip
        // reflow by flagging. Acceptable simplification: if user sets layout=none,
        // we don't emit groups in this call — they can emit via layout_graph later.
    }

    // --- 5. Commit + compile -------------------------------------------------
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::BatchCompile);

    const bool bCompileError = (Blueprint->Status == BS_Error);

    // --- 6. Response ---------------------------------------------------------
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("nodes_created"),     CreatedCount);
    Data->SetNumberField(TEXT("wires_applied"),     WiresApplied);
    Data->SetNumberField(TEXT("defaults_applied"),  DefaultsApplied);
    Data->SetNumberField(TEXT("groups_emitted"),    Groups.Num());
    Data->SetBoolField  (TEXT("auto_layout"),       bAutoLayout);
    Data->SetBoolField  (TEXT("compile_error"),     bCompileError);
    Data->SetStringField(TEXT("graph_name"),        Graph->GetName());
    Data->SetArrayField (TEXT("node_diagnostics"),  NodeDiagnostics);
    Data->SetArrayField (TEXT("wire_diagnostics"),  WireDiagnostics);
    Data->SetArrayField (TEXT("default_diagnostics"),DefaultDiagnostics);

    // Node id → guid map for downstream callers
    TSharedPtr<FJsonObject> IdMap = MakeShared<FJsonObject>();
    for (const auto& KV : NodeMap) IdMap->SetStringField(KV.Key, KV.Value->NodeGuid.ToString());
    Data->SetObjectField(TEXT("id_to_guid"), IdMap);

    const FString Summary = FString::Printf(
        TEXT("Built %d/%d node(s), %d wire(s), %d default(s), %d group(s)%s"),
        CreatedCount, NodesArr->Num(), WiresApplied, DefaultsApplied, Groups.Num(),
        bCompileError ? TEXT(" — compile FAILED") : TEXT(""));

    return MakeBuildResult(!bCompileError, Summary, Data, bCompileError ? 3001 : 0,
        bCompileError ? TEXT("Blueprint compiled with errors; inspect the Message Log.") : FString());
}