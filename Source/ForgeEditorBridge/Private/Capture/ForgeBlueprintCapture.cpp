#include "Capture/ForgeBlueprintCapture.h"
#include "IO/ForgeContextWriter.h"

// Blueprint / graph types
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_Event.h"
#include "K2Node_DynamicCast.h"
#include "GameFramework/Actor.h"

// Asset Registry
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// JSON + IO
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ============================================================
// Initialize
// ============================================================

void UForgeBlueprintCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("blueprints")));
    UE_LOG(LogTemp, Log, TEXT("ForgeBlueprint: Initialized"));
}

// ============================================================
// ExportBlueprint
// ============================================================

bool UForgeBlueprintCapture::ExportBlueprint(const FString& AssetPath)
{
    UBlueprint* BP = Cast<UBlueprint>(
        StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));

    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeBlueprint: Could not load Blueprint at '%s'"), *AssetPath);
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    if (!SerializeBlueprintToJSON(BP, AssetPath, Root))
        return false;

    const FString Filename = BP->GetName() + TEXT(".json");
    const bool bOK = FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("blueprints"), Filename, Root);

    if (bOK)
    {
        UE_LOG(LogTemp, Log, TEXT("ForgeBlueprint: Exported '%s'"), *BP->GetName());
    }
    return bOK;
}

// ============================================================
// ExportAllBlueprints / ExportBlueprintsByPrefix
// ============================================================

int32 UForgeBlueprintCapture::ExportAllBlueprints()
{
    return ExportBlueprintsByPrefix(TEXT("/Game"));
}

int32 UForgeBlueprintCapture::ExportBlueprintsByPrefix(const FString& Prefix)
{
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARModule.Get();
    AR.SearchAllAssets(true);

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(FName(*Prefix));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    if (Assets.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeBlueprint: No Blueprint assets found under '%s'"), *Prefix);
        return 0;
    }

    int32 Exported = 0;
    for (const FAssetData& Asset : Assets)
    {
        if (ExportBlueprint(Asset.GetObjectPathString()))
            Exported++;
    }

    if (Exported > 0)
        UpdateIndexFile(Exported);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeBlueprint: ExportByPrefix complete — %d / %d exported"),
        Exported, Assets.Num());
    return Exported;
}

// ============================================================
// SerializeBlueprintToJSON
// ============================================================

bool UForgeBlueprintCapture::SerializeBlueprintToJSON(UBlueprint* BP,
    const FString& AssetPath, TSharedRef<FJsonObject> OutRoot)
{
    if (!BP)
        return false;

    OutRoot->SetStringField(TEXT("timestamp"),    FForgeContextWriter::NowISO8601());
    OutRoot->SetStringField(TEXT("asset_path"),   AssetPath);
    OutRoot->SetStringField(TEXT("name"),         BP->GetName());
    OutRoot->SetStringField(TEXT("parent_class"),
        BP->ParentClass ? BP->ParentClass->GetName() : TEXT("null"));

    SerializeVariables(BP, OutRoot);
    SerializeComponents(BP, OutRoot);
    SerializeGraphs(BP, OutRoot);

    TArray<TSharedPtr<FJsonValue>> AuditArr = RunAudit(BP);
    OutRoot->SetArrayField(TEXT("audit_results"), AuditArr);
    OutRoot->SetNumberField(TEXT("audit_issue_count"), AuditArr.Num());

    return true;
}

// ============================================================
// SerializeVariables
// ============================================================

void UForgeBlueprintCapture::SerializeVariables(UBlueprint* BP,
    TSharedRef<FJsonObject> OutRoot)
{
    TArray<TSharedPtr<FJsonValue>> VarsArr;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),         Var.VarName.ToString());
        Obj->SetStringField(TEXT("pin_category"), Var.VarType.PinCategory.ToString());
        if (!Var.DefaultValue.IsEmpty())
            Obj->SetStringField(TEXT("default_value"), Var.DefaultValue);

        const bool bReplicated = (Var.PropertyFlags & (uint64)CPF_Net) != 0;
        Obj->SetBoolField(TEXT("replicated"), bReplicated);
        if (bReplicated && Var.RepNotifyFunc != NAME_None)
            Obj->SetStringField(TEXT("rep_notify"), Var.RepNotifyFunc.ToString());

        VarsArr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    OutRoot->SetArrayField(TEXT("variables"), VarsArr);
}

// ============================================================
// SerializeComponents
// ============================================================

void UForgeBlueprintCapture::SerializeComponents(UBlueprint* BP,
    TSharedRef<FJsonObject> OutRoot)
{
    TArray<TSharedPtr<FJsonValue>> CompsArr;
    if (BP->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (!SCSNode) continue;
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("variable_name"),
                SCSNode->GetVariableName().ToString());
            Obj->SetStringField(TEXT("component_class"),
                SCSNode->ComponentClass ? SCSNode->ComponentClass->GetName() : TEXT("null"));
            Obj->SetStringField(TEXT("parent"),
                SCSNode->ParentComponentOrVariableName.ToString());
            CompsArr.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    OutRoot->SetArrayField(TEXT("components"), CompsArr);
}

// ============================================================
// SerializeGraphs
// ============================================================

void UForgeBlueprintCapture::SerializeGraphs(UBlueprint* BP,
    TSharedRef<FJsonObject> OutRoot)
{
    auto SerializeGraph = [](UEdGraph* Graph) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
        GObj->SetStringField(TEXT("name"),       Graph->GetName());
        GObj->SetStringField(TEXT("graph_type"), Graph->GetClass()->GetName());
        GObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

        TArray<TSharedPtr<FJsonValue>> NodesArr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
            NObj->SetStringField(TEXT("id"),        Node->NodeGuid.ToString());
            NObj->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
            NObj->SetStringField(TEXT("title"),
                Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            NObj->SetNumberField(TEXT("x"), Node->NodePosX);
            NObj->SetNumberField(TEXT("y"), Node->NodePosY);

            // Member reference for function call / variable nodes
            if (const UK2Node_CallFunction* FnNode = Cast<UK2Node_CallFunction>(Node))
            {
                NObj->SetStringField(TEXT("member_name"),
                    FnNode->FunctionReference.GetMemberName().ToString());
                const UClass* ParentClass = FnNode->FunctionReference.GetMemberParentClass();
                NObj->SetStringField(TEXT("member_parent"),
                    ParentClass ? ParentClass->GetName() : TEXT("self"));
            }
            else if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
            {
                NObj->SetStringField(TEXT("member_name"),
                    VarNode->VariableReference.GetMemberName().ToString());
            }

            // Pins
            TArray<TSharedPtr<FJsonValue>> PinsArr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
                PObj->SetStringField(TEXT("name"),         Pin->PinName.ToString());
                PObj->SetStringField(TEXT("direction"),
                    Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
                PObj->SetStringField(TEXT("pin_category"),
                    Pin->PinType.PinCategory.ToString());
                if (!Pin->DefaultValue.IsEmpty())
                    PObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);

                TArray<TSharedPtr<FJsonValue>> LinkedArr;
                for (UEdGraphPin* Other : Pin->LinkedTo)
                {
                    if (Other && Other->GetOwningNode())
                    {
                        LinkedArr.Add(MakeShared<FJsonValueString>(
                            Other->GetOwningNode()->NodeGuid.ToString()
                            + TEXT(":") + Other->PinName.ToString()));
                    }
                }
                PObj->SetArrayField(TEXT("linked_to"), LinkedArr);
                PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
            }
            NObj->SetArrayField(TEXT("pins"), PinsArr);
            NodesArr.Add(MakeShared<FJsonValueObject>(NObj));
        }
        GObj->SetArrayField(TEXT("nodes"), NodesArr);
        return GObj;
    };

    auto ToGraphArray = [&](const TArray<UEdGraph*>& Graphs)
        -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (UEdGraph* G : Graphs)
            if (G) Arr.Add(MakeShared<FJsonValueObject>(SerializeGraph(G)));
        return Arr;
    };

    OutRoot->SetArrayField(TEXT("event_graphs"),    ToGraphArray(BP->UbergraphPages));
    OutRoot->SetArrayField(TEXT("function_graphs"), ToGraphArray(BP->FunctionGraphs));
    OutRoot->SetArrayField(TEXT("macro_graphs"),    ToGraphArray(BP->MacroGraphs));
}

// ============================================================
// RunAudit — 8 rules
// ============================================================

TArray<TSharedPtr<FJsonValue>> UForgeBlueprintCapture::RunAudit(UBlueprint* BP)
{
    TArray<TSharedPtr<FJsonValue>> Results;

    auto Issue = [&Results](const TCHAR* Rule, const TCHAR* Severity,
                            const FString& Graph, const FString& NodeId,
                            const FString& Detail)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("rule"),     Rule);
        Obj->SetStringField(TEXT("severity"), Severity);
        Obj->SetStringField(TEXT("graph"),    Graph);
        Obj->SetStringField(TEXT("node_id"),  NodeId);
        Obj->SetStringField(TEXT("detail"),   Detail);
        Results.Add(MakeShared<FJsonValueObject>(Obj));
    };

    // Build known-variable set for MISSING_VARIABLE checks
    TSet<FName> KnownVarNames;
    for (const FBPVariableDescription& Var : BP->NewVariables)
        KnownVarNames.Add(Var.VarName);

    // -------------------------------------------------------
    // Rule 6: SHOULD_BE_CPP — function graphs with >50 nodes
    // -------------------------------------------------------
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (!Graph) continue;
        if (Graph->Nodes.Num() > 50)
        {
            Issue(TEXT("SHOULD_BE_CPP"), TEXT("warning"),
                Graph->GetName(), TEXT(""),
                FString::Printf(
                    TEXT("Function '%s' has %d nodes — consider moving logic to C++"),
                    *Graph->GetName(), Graph->Nodes.Num()));
        }
    }

    // -------------------------------------------------------
    // Rule 8: REPLICATED_NO_ONREP
    // -------------------------------------------------------
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        if ((Var.PropertyFlags & (uint64)CPF_Net) != 0 && Var.RepNotifyFunc == NAME_None)
        {
            Issue(TEXT("REPLICATED_NO_ONREP"), TEXT("warning"),
                TEXT(""), TEXT(""),
                FString::Printf(
                    TEXT("Variable '%s' is replicated but has no RepNotify function"),
                    *Var.VarName.ToString()));
        }
    }

    // -------------------------------------------------------
    // Rule 9: TICK_ENABLED_NO_LOGIC — Actor with tick on but empty ReceiveTick
    // -------------------------------------------------------
    if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
    {
        if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
        {
            if (CDO->PrimaryActorTick.bCanEverTick)
            {
                bool bHasTickLogic = false;
                for (UEdGraph* G : BP->UbergraphPages)
                {
                    if (!G || bHasTickLogic) break;
                    for (UEdGraphNode* N : G->Nodes)
                    {
                        if (UK2Node_Event* Evt = Cast<UK2Node_Event>(N))
                        {
                            if (Evt->GetFunctionName() == FName("ReceiveTick"))
                            {
                                for (UEdGraphPin* Pin : Evt->Pins)
                                {
                                    if (Pin && Pin->Direction == EGPD_Output &&
                                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                                        Pin->LinkedTo.Num() > 0)
                                    {
                                        bHasTickLogic = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (bHasTickLogic) break;
                    }
                }
                if (!bHasTickLogic)
                {
                    Issue(TEXT("TICK_ENABLED_NO_LOGIC"), TEXT("warning"),
                        TEXT(""), TEXT(""),
                        TEXT("PrimaryActorTick.bCanEverTick is true but no ReceiveTick event has connected logic. Disable tick to save performance."));
                }
            }
        }
    }

    // -------------------------------------------------------
    // Rule 10: MISSING_SUPER_CALL — function override with no parent call
    // -------------------------------------------------------
    if (BP->ParentClass)
    {
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            if (!Graph) continue;
            const FName FuncName(*Graph->GetName());
            UFunction* ParentFunc = BP->ParentClass->FindFunctionByName(
                FuncName, EIncludeSuperFlag::IncludeSuper);
            if (!ParentFunc) continue;
            // Skip static and pure functions — super calls are not required
            if (ParentFunc->HasAnyFunctionFlags(FUNC_Static | FUNC_BlueprintPure)) continue;

            bool bHasParentCall = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetClass()->GetName().Contains(TEXT("CallParentFunction")))
                {
                    bHasParentCall = true;
                    break;
                }
            }
            if (!bHasParentCall)
            {
                Issue(TEXT("MISSING_SUPER_CALL"), TEXT("info"),
                    Graph->GetName(), TEXT(""),
                    FString::Printf(
                        TEXT("Function '%s' overrides parent class '%s' without calling the parent implementation."),
                        *FuncName.ToString(), *BP->ParentClass->GetName()));
            }
        }
    }

    // -------------------------------------------------------
    // Per-graph rules: 1, 2, 3, 4, 5, 7, 11, 12
    // -------------------------------------------------------
    TArray<UEdGraph*> AllGraphs;
    AllGraphs.Append(BP->UbergraphPages);
    AllGraphs.Append(BP->FunctionGraphs);

    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph) continue;
        const FString GraphName = Graph->GetName();

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            const FString NodeId    = Node->NodeGuid.ToString();
            const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
            const UK2Node* K2Node = Cast<UK2Node>(Node);
            const bool    bIsPure   = K2Node ? K2Node->IsNodePure() : false;

            // --- Rule 3: MISSING_FUNCTION ---
            if (UK2Node_CallFunction* FnNode = Cast<UK2Node_CallFunction>(Node))
            {
                if (FnNode->GetTargetFunction() == nullptr)
                {
                    Issue(TEXT("MISSING_FUNCTION"), TEXT("error"),
                        GraphName, NodeId,
                        FString::Printf(TEXT("Function call '%s' could not be resolved"),
                            *FnNode->FunctionReference.GetMemberName().ToString()));
                }
            }
            // --- Rule 4: MISSING_VARIABLE ---
            else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
            {
                if (VarNode->VariableReference.IsSelfContext())
                {
                    const FName VarName = VarNode->VariableReference.GetMemberName();
                    const bool bFoundLocal = KnownVarNames.Contains(VarName);
                    const bool bFoundInClass = BP->SkeletonGeneratedClass &&
                        BP->SkeletonGeneratedClass->FindPropertyByName(VarName) != nullptr;
                    if (!bFoundLocal && !bFoundInClass)
                    {
                        Issue(TEXT("MISSING_VARIABLE"), TEXT("error"),
                            GraphName, NodeId,
                            FString::Printf(
                                TEXT("Variable '%s' not found on this Blueprint or its class"),
                                *VarName.ToString()));
                    }
                }
            }
            // --- Rule 5: CAST_ALWAYS_FAILS ---
            else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
            {
                if (CastNode->TargetType == nullptr)
                {
                    Issue(TEXT("CAST_ALWAYS_FAILS"), TEXT("error"),
                        GraphName, NodeId,
                        TEXT("Cast node has null TargetType — target class is missing"));
                }
            }
            // --- Rule 7: RPC_NO_AUTH_CHECK ---
            else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                UFunction* Func = nullptr;
                if (BP->SkeletonGeneratedClass)
                {
                    Func = BP->SkeletonGeneratedClass->FindFunctionByName(
                        EventNode->GetFunctionName());
                }
                if (Func && Func->HasAnyFunctionFlags(FUNC_NetServer))
                {
                    bool bHasAuthBranch = false;
                    for (UEdGraphPin* Pin : EventNode->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Output) continue;
                        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                        for (UEdGraphPin* Linked : Pin->LinkedTo)
                        {
                            if (!Linked || !Linked->GetOwningNode()) continue;
                            const FString ConnType =
                                Linked->GetOwningNode()->GetClass()->GetName();
                            if (ConnType.Contains(TEXT("Branch")) ||
                                ConnType.Contains(TEXT("SwitchEnum")))
                            {
                                bHasAuthBranch = true;
                                break;
                            }
                        }
                        if (bHasAuthBranch) break;
                    }
                    if (!bHasAuthBranch)
                    {
                        Issue(TEXT("RPC_NO_AUTH_CHECK"), TEXT("warning"),
                            GraphName, NodeId,
                            FString::Printf(
                                TEXT("Server RPC '%s' has no immediate authority/branch check"),
                                *EventNode->GetFunctionName().ToString()));
                    }
                }
            }

            // --- Rule 1: DANGLING_OUTPUT_PIN ---
            // Only flag on non-pure nodes with >1 exec output (Branch, Switch, Sequence)
            // to avoid flagging natural terminal nodes
            if (!bIsPure)
            {
                int32 ExecOutCount  = 0;
                int32 ExecOutLinked = 0;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    if (Pin->Direction == EGPD_Output &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        ExecOutCount++;
                        if (Pin->LinkedTo.Num() > 0) ExecOutLinked++;
                    }
                }
                if (ExecOutCount > 1)
                {
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin) continue;
                        if (Pin->Direction == EGPD_Output &&
                            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                            Pin->LinkedTo.Num() == 0)
                        {
                            Issue(TEXT("DANGLING_OUTPUT_PIN"), TEXT("warning"),
                                GraphName, NodeId,
                                FString::Printf(
                                    TEXT("Node '%s' has unconnected exec output '%s'"),
                                    *NodeTitle, *Pin->PinName.ToString()));
                        }
                    }
                }
            }

            // --- Rule 2: UNREACHABLE_NODE ---
            // Non-pure, non-event node where all exec inputs are unlinked
            if (!bIsPure && !Cast<UK2Node_Event>(Node))
            {
                bool bHasExecIn    = false;
                bool bExecInLinked = false;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    if (Pin->Direction == EGPD_Input &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        bHasExecIn = true;
                        if (Pin->LinkedTo.Num() > 0) bExecInLinked = true;
                    }
                }
                if (bHasExecIn && !bExecInLinked)
                {
                    Issue(TEXT("UNREACHABLE_NODE"), TEXT("warning"),
                        GraphName, NodeId,
                        FString::Printf(
                            TEXT("Node '%s' has no linked exec input — may be unreachable"),
                            *NodeTitle));
                }
            }

            // --- Rule 11: DEPRECATED_NODE ---
            // Note: UEdGraphNode::GetDeprecationMessage() removed in UE 5.7; use static message.
            if (Node->IsDeprecated())
            {
                Issue(TEXT("DEPRECATED_NODE"), TEXT("warning"),
                    GraphName, NodeId,
                    FString::Printf(TEXT("Node '%s' is deprecated. Replace with the recommended alternative."),
                        *NodeTitle));
            }
        }

        // --- Rule 12: EXCESSIVE_CAST_CHAIN ---
        // Walk the exec-success chain from each DynamicCast; flag chains > 3.
        {
            TSet<UEdGraphNode*> Counted;
            for (UEdGraphNode* StartNode : Graph->Nodes)
            {
                if (!StartNode || Counted.Contains(StartNode)) continue;
                UK2Node_DynamicCast* StartCast = Cast<UK2Node_DynamicCast>(StartNode);
                if (!StartCast) continue;

                int32 ChainLen = 1;
                UK2Node_DynamicCast* Cur = StartCast;
                while (Cur)
                {
                    UK2Node_DynamicCast* Next = nullptr;
                    for (UEdGraphPin* Pin : Cur->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Output) continue;
                        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                        if (Pin->PinName == FName("CastFailed")) continue;
                        if (Pin->LinkedTo.Num() == 0) continue;
                        UEdGraphNode* NextNode = Pin->LinkedTo[0]->GetOwningNode();
                        Next = Cast<UK2Node_DynamicCast>(NextNode);
                        break;
                    }
                    if (!Next) break;
                    Counted.Add(Next);
                    ChainLen++;
                    Cur = Next;
                }

                if (ChainLen > 3)
                {
                    Issue(TEXT("EXCESSIVE_CAST_CHAIN"), TEXT("warning"),
                        GraphName, StartCast->NodeGuid.ToString(),
                        FString::Printf(
                            TEXT("%d consecutive casts starting at '%s'. Consider polymorphism or a dispatch table."),
                            ChainLen,
                            *StartCast->GetNodeTitle(ENodeTitleType::FullTitle).ToString()));
                }
            }
        }
    }

    return Results;
}

// ============================================================
// UpdateIndexFile — READ-MERGE-WRITE preserving all sections
// ============================================================

void UForgeBlueprintCapture::UpdateIndexFile(int32 BlueprintCount)
{
    const FString IndexPath = OutputDir / TEXT("index.json");
    const FString Timestamp = FForgeContextWriter::NowISO8601();

    TSharedPtr<FJsonObject> Root;
    FString Existing;
    if (FFileHelper::LoadFileToString(Existing, *IndexPath))
    {
        TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Existing);
        FJsonSerializer::Deserialize(R, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
        Captures = *ExistingCaptures;
    else
        Captures = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> BPSection = MakeShared<FJsonObject>();
    BPSection->SetStringField(TEXT("output_dir"),      TEXT("blueprints/"));
    BPSection->SetNumberField(TEXT("blueprint_count"), BlueprintCount);
    BPSection->SetStringField(TEXT("last_updated"),    Timestamp);
    Captures->SetObjectField(TEXT("blueprints"), BPSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), TEXT("0.2.6"));

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
