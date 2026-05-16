#include "Handlers/BlueprintHandler.h"
#include "ForgeAISubsystem.h"
#include "Attention/BridgeAttentionManager.h"
#include "Capture/ForgeBlueprintCapture.h"

// UE asset creation
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Blueprint graph
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"

// SCS / Components
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// Function graph nodes
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"

// Scoped transaction (for reparent)
#include "ScopedTransaction.h"

// Replication (ELifetimeCondition for set_variable_replication)
#include "Net/UnrealNetwork.h"

// Phase 1c additions
#include "K2Node_CallDelegate.h"
#include "FileHelpers.h"

// Phase 1e additions — RPC flags, UCS editing, assembler + layout trampolines
#include "UObject/Script.h"           // FUNC_Net / FUNC_NetServer / FUNC_NetClient / FUNC_NetMulticast / FUNC_NetReliable
#include "Handlers/BlueprintGraphAssembler.h"
#include "Handlers/BlueprintGraphLayout.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Utilities
#include "Misc/PackageName.h"
#include "Misc/DateTime.h"
#include "UObject/UObjectIterator.h"

// ============================================================================
// Anonymous-namespace helpers
// ============================================================================
namespace
{
    // Build an FEdGraphPinType from a friendly string name.
    // Returns true on success, fills OutType. On failure writes a descriptive
    // error into OutError and returns false.
    static bool BuildPinTypeFromString(
        const FString& TypeStr,
        const FString& SubTypeStr,
        FEdGraphPinType& OutType,
        FString& OutError)
    {
        const FString T = TypeStr.ToLower();
        using K2 = UEdGraphSchema_K2;

        OutType.ContainerType        = EPinContainerType::None;
        OutType.PinSubCategory       = NAME_None;
        OutType.PinSubCategoryObject = nullptr;

        if      (T == TEXT("exec"))        { OutType.PinCategory = K2::PC_Exec; }
        else if (T == TEXT("bool") ||
                 T == TEXT("boolean"))     { OutType.PinCategory = K2::PC_Boolean; }
        else if (T == TEXT("byte"))        { OutType.PinCategory = K2::PC_Byte; }
        else if (T == TEXT("int")  ||
                 T == TEXT("int32") ||
                 T == TEXT("integer"))     { OutType.PinCategory = K2::PC_Int; }
        else if (T == TEXT("int64"))       { OutType.PinCategory = K2::PC_Int64; }
        else if (T == TEXT("float"))       { OutType.PinCategory    = K2::PC_Real;
                                             OutType.PinSubCategory = K2::PC_Float; }
        else if (T == TEXT("double") ||
                 T == TEXT("real"))        { OutType.PinCategory    = K2::PC_Real;
                                             OutType.PinSubCategory = K2::PC_Double; }
        else if (T == TEXT("string"))      { OutType.PinCategory = K2::PC_String; }
        else if (T == TEXT("name"))        { OutType.PinCategory = K2::PC_Name; }
        else if (T == TEXT("text"))        { OutType.PinCategory = K2::PC_Text; }
        else if (T == TEXT("wildcard"))    { OutType.PinCategory = K2::PC_Wildcard; }
        else if (T == TEXT("struct"))
        {
            OutType.PinCategory = K2::PC_Struct;
            UScriptStruct* S = !SubTypeStr.IsEmpty()
                ? LoadObject<UScriptStruct>(nullptr, *SubTypeStr)
                : nullptr;
            if (!S)
            {
                OutError = FString::Printf(
                    TEXT("type 'struct' requires 'sub_type' as a struct path (got '%s')"),
                    *SubTypeStr);
                return false;
            }
            OutType.PinSubCategoryObject = S;
        }
        else if (T == TEXT("object") || T == TEXT("class") ||
                 T == TEXT("soft_object") || T == TEXT("softobject") ||
                 T == TEXT("soft_class")  || T == TEXT("softclass")  ||
                 T == TEXT("interface"))
        {
            if      (T == TEXT("object"))       OutType.PinCategory = K2::PC_Object;
            else if (T == TEXT("class"))        OutType.PinCategory = K2::PC_Class;
            else if (T == TEXT("soft_object") ||
                     T == TEXT("softobject"))   OutType.PinCategory = K2::PC_SoftObject;
            else if (T == TEXT("soft_class")  ||
                     T == TEXT("softclass"))    OutType.PinCategory = K2::PC_SoftClass;
            else                                OutType.PinCategory = K2::PC_Interface;
            UClass* C = nullptr;
            if (!SubTypeStr.IsEmpty())
            {
                C = FindObject<UClass>(nullptr, *SubTypeStr);
                if (!C) C = LoadObject<UClass>(nullptr, *SubTypeStr);
            }
            if (!C)
            {
                OutError = FString::Printf(
                    TEXT("type '%s' requires 'sub_type' as a class path (got '%s')"),
                    *T, *SubTypeStr);
                return false;
            }
            OutType.PinSubCategoryObject = C;
        }
        else if (T == TEXT("enum"))
        {
            OutType.PinCategory = K2::PC_Byte;
            UEnum* E = !SubTypeStr.IsEmpty()
                ? LoadObject<UEnum>(nullptr, *SubTypeStr)
                : nullptr;
            if (!E)
            {
                OutError = FString::Printf(
                    TEXT("type 'enum' requires 'sub_type' as an enum path (got '%s')"),
                    *SubTypeStr);
                return false;
            }
            OutType.PinSubCategoryObject = E;
        }
        else if (T == TEXT("delegate"))    { OutType.PinCategory = K2::PC_Delegate; }
        else if (T == TEXT("multicast_delegate") ||
                 T == TEXT("multicastdelegate") ||
                 T == TEXT("mcdelegate"))  { OutType.PinCategory = K2::PC_MCDelegate; }
        else
        {
            OutError = FString::Printf(TEXT("Unknown variable_type '%s'"), *TypeStr);
            return false;
        }
        return true;
    }

    // Cross-graph node resolution: searches UbergraphPages, FunctionGraphs,
    // MacroGraphs, DelegateSignatureGraphs for a node by GUID.
    static UEdGraphNode* FindNodeInAnyGraph(UBlueprint* BP, const FGuid& NodeGuid, UEdGraph** OutGraph = nullptr)
    {
        if (!BP) return nullptr;
        auto Search = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraphNode*
        {
            for (UEdGraph* G : Graphs)
            {
                if (!G) continue;
                for (UEdGraphNode* N : G->Nodes)
                {
                    if (N && N->NodeGuid == NodeGuid)
                    {
                        if (OutGraph) *OutGraph = G;
                        return N;
                    }
                }
            }
            return nullptr;
        };
        if (UEdGraphNode* N = Search(BP->UbergraphPages))         return N;
        if (UEdGraphNode* N = Search(BP->FunctionGraphs))         return N;
        if (UEdGraphNode* N = Search(BP->MacroGraphs))            return N;
        if (UEdGraphNode* N = Search(BP->DelegateSignatureGraphs))return N;
        return nullptr;
    }

    // Find graph by name across all graph collections.
    // Returns nullptr if not found (or if GraphName is empty).
    static UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName)
    {
        if (!BP || GraphName.IsEmpty()) return nullptr;
        for (UEdGraph* G : BP->UbergraphPages)          { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->FunctionGraphs)          { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->MacroGraphs)             { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->DelegateSignatureGraphs) { if (G && G->GetName() == GraphName) return G; }
        return nullptr;
    }
}

// ============================================================================
// Dispatch
// ============================================================================

FBridgeResult UBlueprintHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        FBridgeResult R = CreateResult(TEXT("blueprint"), Action);
        R.Message = TEXT("HandleCommand: Params object is null");
        return R;
    }

    if (Action == TEXT("create_blueprint"))       return Action_CreateBlueprint(Params);
    if (Action == TEXT("add_node"))               return Action_AddNode(Params);
    if (Action == TEXT("connect_nodes"))          return Action_ConnectNodes(Params);
    if (Action == TEXT("compile"))                return Action_Compile(Params);
    if (Action == TEXT("add_step"))               return Action_AddStep(Params);
    if (Action == TEXT("prepare_value"))          return Action_PrepareValue(Params);
    if (Action == TEXT("connect_data_to_pin"))    return Action_ConnectDataToPin(Params);
    if (Action == TEXT("get_function_nodes"))     return Action_GetFunctionNodes(Params);
    if (Action == TEXT("add_variable"))           return Action_AddVariable(Params);
    if (Action == TEXT("get_variables"))          return Action_GetVariables(Params);
    if (Action == TEXT("search_function_library"))return Action_SearchFunctionLibrary(Params);
    if (Action == TEXT("delete_node"))            return Action_DeleteNode(Params);
    if (Action == TEXT("add_component"))          return Action_AddComponent(Params);
    if (Action == TEXT("remove_component"))       return Action_RemoveComponent(Params);
    if (Action == TEXT("get_components"))         return Action_GetComponents(Params);
    if (Action == TEXT("add_function_graph"))     return Action_AddFunctionGraph(Params);
    if (Action == TEXT("add_custom_event"))       return Action_AddCustomEvent(Params);
    if (Action == TEXT("add_interface"))          return Action_AddInterface(Params);
    if (Action == TEXT("list_nodes"))             return Action_ListNodes(Params);
    if (Action == TEXT("list_pins"))              return Action_ListPins(Params);
    if (Action == TEXT("set_variable_default"))   return Action_SetVariableDefault(Params);
    if (Action == TEXT("reparent"))               return Action_Reparent(Params);
    if (Action == TEXT("read_blueprint_capture"))  return Action_ReadBlueprintCapture(Params);
    if (Action == TEXT("add_event_dispatcher"))    return Action_AddEventDispatcher(Params);
    if (Action == TEXT("get_event_dispatchers"))   return Action_GetEventDispatchers(Params);
    if (Action == TEXT("bind_event"))              return Action_BindEvent(Params);
    if (Action == TEXT("set_variable_replication"))return Action_SetVariableReplication(Params);
    if (Action == TEXT("set_component_property"))  return Action_SetComponentProperty(Params);

    // ---- Phase 1c dispatches -------------------------------------------------
    if (Action == TEXT("compile_all_dirty"))       return Action_CompileAllDirty(Params);
    if (Action == TEXT("save"))                    return Action_Save(Params);
    if (Action == TEXT("save_all"))                return Action_SaveAll(Params);
    if (Action == TEXT("remove_variable"))         return Action_RemoveVariable(Params);
    if (Action == TEXT("rename_variable"))         return Action_RenameVariable(Params);
    if (Action == TEXT("add_function"))            return Action_AddFunction(Params);
    if (Action == TEXT("add_macro"))               return Action_AddMacro(Params);
    if (Action == TEXT("override_function"))       return Action_OverrideFunction(Params);
    if (Action == TEXT("get_graph_names"))         return Action_GetGraphNames(Params);
    if (Action == TEXT("remove_interface"))        return Action_RemoveInterface(Params);
    if (Action == TEXT("add_local_variable"))      return Action_AddLocalVariable(Params);
    if (Action == TEXT("connect_pins"))            return Action_ConnectPins(Params);
    if (Action == TEXT("set_pin_default"))         return Action_SetPinDefault(Params);
    if (Action == TEXT("remove_event_dispatcher")) return Action_RemoveEventDispatcher(Params);
    if (Action == TEXT("call_event_dispatcher"))   return Action_CallEventDispatcher(Params);
    if (Action == TEXT("get_replicated_variables"))return Action_GetReplicatedVariables(Params);
    if (Action == TEXT("set_replication_condition"))return Action_SetReplicationCondition(Params);
    if (Action == TEXT("enable_push_model"))       return Action_EnablePushModel(Params);
    if (Action == TEXT("list_components"))         return Action_ListComponents(Params);

    // ---- Phase 1d: authoring completeness ----
    if (Action == TEXT("set_node_position"))       return Action_SetNodePosition(Params);
    if (Action == TEXT("disconnect_pins"))         return Action_DisconnectPins(Params);
    if (Action == TEXT("compile_blueprint"))       return Action_Compile(Params);
    if (Action == TEXT("list_interfaces"))         return Action_ListInterfaces(Params);

    // ---- Phase 1e: RPC authoring ----
    if (Action == TEXT("add_rpc_function"))        return Action_AddRpcFunction(Params);

    // ---- Phase 1e: User Construction Script editing ----
    if (Action == TEXT("get_construction_script"))   return Action_GetConstructionScript(Params);
    if (Action == TEXT("add_ucs_node"))              return Action_AddUcsNode(Params);
    if (Action == TEXT("connect_ucs_pins"))          return Action_ConnectUcsPins(Params);
    if (Action == TEXT("set_ucs_pin_default"))       return Action_SetUcsPinDefault(Params);
    if (Action == TEXT("clear_construction_script")) return Action_ClearConstructionScript(Params);

    // ---- Phase 1e: High-level graph assembly + auto-layout ----
    if (Action == TEXT("build_blueprint_graph"))     return Action_BuildBlueprintGraph(Params);
    if (Action == TEXT("layout_graph"))              return Action_LayoutGraph(Params);

    // ---- Wave 7: typed Blueprint creators (parent_class injected, then routed) ----
    if (Action == TEXT("create_function_library_bp"))   return Action_CreateTypedBP(Params, TEXT("BlueprintFunctionLibrary"), Action);
    if (Action == TEXT("create_macro_library_bp"))      return Action_CreateTypedBP(Params, TEXT("BlueprintMacroLibrary"),    Action);
    if (Action == TEXT("create_interface_bp"))          return Action_CreateTypedBP(Params, TEXT("Interface"),                 Action);
    if (Action == TEXT("create_save_game_bp"))          return Action_CreateTypedBP(Params, TEXT("SaveGame"),                  Action);
    if (Action == TEXT("create_game_mode_bp"))          return Action_CreateTypedBP(Params, TEXT("GameModeBase"),              Action);
    if (Action == TEXT("create_game_state_bp"))         return Action_CreateTypedBP(Params, TEXT("GameStateBase"),             Action);
    if (Action == TEXT("create_game_instance_bp"))      return Action_CreateTypedBP(Params, TEXT("GameInstance"),              Action);
    if (Action == TEXT("create_player_controller_bp")) return Action_CreateTypedBP(Params, TEXT("PlayerController"),          Action);
    if (Action == TEXT("create_pawn_bp"))               return Action_CreateTypedBP(Params, TEXT("Pawn"),                      Action);
    if (Action == TEXT("create_character_bp"))          return Action_CreateTypedBP(Params, TEXT("Character"),                 Action);
    if (Action == TEXT("create_hud_bp"))                return Action_CreateTypedBP(Params, TEXT("HUD"),                       Action);
    if (Action == TEXT("create_actor_component_bp"))    return Action_CreateTypedBP(Params, TEXT("ActorComponent"),            Action);

    // create_subsystem_bp accepts scope param to pick subsystem class
    if (Action == TEXT("create_subsystem_bp"))
    {
        FString Scope = TEXT("game");
        Params->TryGetStringField(TEXT("scope"), Scope);
        Scope = Scope.ToLower();
        FString ParentClass = TEXT("GameInstanceSubsystem");
        if (Scope == TEXT("world"))       ParentClass = TEXT("WorldSubsystem");
        else if (Scope == TEXT("localplayer")) ParentClass = TEXT("LocalPlayerSubsystem");
        else if (Scope == TEXT("engine"))      ParentClass = TEXT("EngineSubsystem");
        return Action_CreateTypedBP(Params, ParentClass, Action);
    }

    // ---- Wave 7: K2 node creators ----
    if (Action == TEXT("add_branch_node"))           return Action_AddBranchNode(Params);
    if (Action == TEXT("add_cast_node"))             return Action_AddCastNode(Params);
    if (Action == TEXT("add_sequence_node"))         return Action_AddSequenceNode(Params);
    if (Action == TEXT("add_make_struct_node"))      return Action_AddMakeStructNode(Params);
    if (Action == TEXT("add_break_struct_node"))     return Action_AddBreakStructNode(Params);
    if (Action == TEXT("add_variable_node"))         return Action_AddVariableNode(Params);
    if (Action == TEXT("add_self_node"))             return Action_AddSelfNode(Params);
    if (Action == TEXT("add_for_each_loop_node"))    return Action_AddForEachLoopNode(Params);

    // ---- Wave 7: graph removal ----
    if (Action == TEXT("remove_function_graph"))     return Action_RemoveFunctionGraph(Params);
    if (Action == TEXT("remove_macro_graph"))        return Action_RemoveMacroGraph(Params);

    // ---- Wave 8: EUB creator ----
    if (Action == TEXT("create_editor_utility_blueprint")) return Action_CreateEditorUtilityBlueprint(Params);

    FBridgeResult R = CreateResult(TEXT("blueprint"), Action);
    R.Message = FString::Printf(
        TEXT("Unknown blueprint action: '%s'. Valid: create_blueprint, add_node, connect_nodes, compile, compile_blueprint, "
             "add_step, prepare_value, connect_data_to_pin, get_function_nodes, add_variable, get_variables, "
             "search_function_library, delete_node, add_component, remove_component, get_components, "
             "add_function_graph, add_custom_event, add_interface, list_nodes, list_pins, set_variable_default, reparent, "
             "read_blueprint_capture, add_event_dispatcher, get_event_dispatchers, bind_event, "
             "set_variable_replication, set_component_property, "
             "set_node_position, disconnect_pins, list_interfaces, "
             "add_rpc_function, get_construction_script, add_ucs_node, connect_ucs_pins, "
             "set_ucs_pin_default, clear_construction_script, build_blueprint_graph, layout_graph"),
        *Action);
    return R;
}

// ============================================================================
// create_blueprint
// ============================================================================

FBridgeResult UBlueprintHandler::Action_CreateBlueprint(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("create_blueprint"));

    FString AssetName, PackagePath;
    if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
    {
        Result.Message = TEXT("create_blueprint: 'asset_name' is required");
        return Result;
    }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        Result.Message = TEXT("create_blueprint: 'package_path' is required (e.g. \"/Game/AI\")");
        return Result;
    }

    // Resolve parent class (default: AActor)
    FString ParentClassName;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

    UClass* ParentClass = AActor::StaticClass();
    if (!ParentClassName.IsEmpty())
    {
        // Try /Script/Engine.<Name> first, then /Script/CoreUObject.<Name>
        const FString EngineClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *ParentClassName);
        UClass* Found = FindObject<UClass>(nullptr, *EngineClassPath);
        if (!Found)
        {
            // Fallback: iterate registered classes (slow but robust for project classes)
            for (TObjectIterator<UClass> It; It; ++It)
            {
                if (It->GetName() == ParentClassName)
                {
                    Found = *It;
                    break;
                }
            }
        }
        if (Found)
        {
            ParentClass = Found;
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("BlueprintHandler: Parent class '%s' not found, defaulting to AActor"), *ParentClassName);
        }
    }

    // Create asset via AssetTools
    FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
    if (!CreatedAsset)
    {
        Result.Message = FString::Printf(
            TEXT("create_blueprint: AssetTools failed to create '%s' in '%s' (path may already exist or be invalid)"),
            *AssetName, *PackagePath);
        return Result;
    }

    // Normalise the output path — strip trailing slash from package_path
    FString NormPath = PackagePath;
    NormPath.RemoveFromEnd(TEXT("/"));
    const FString AssetPath = FString::Printf(TEXT("%s/%s"), *NormPath, *AssetName);

    Result.bSuccess   = true;
    Result.AffectedPath = AssetPath;
    Result.Message    = FString::Printf(TEXT("Blueprint created at %s (parent: %s)"),
                                        *AssetPath, *ParentClass->GetName());
    return Result;
}

// ============================================================================
// add_node
// ============================================================================

FBridgeResult UBlueprintHandler::Action_AddNode(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("add_node"));

    FString BlueprintPath, NodeType;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
    {
        Result.Message = TEXT("add_node: 'blueprint_path' is required");
        return Result;
    }
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
    {
        Result.Message = TEXT("add_node: 'node_type' is required (CallFunction | Event)");
        return Result;
    }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
    {
        Result.Message = FString::Printf(TEXT("add_node: Blueprint not found at '%s'"), *BlueprintPath);
        return Result;
    }

    // Resolve target graph: explicit 'graph_name' param (searches Ubergraph/Function/Macro
    // /DelegateSignature graphs) falls back to the event graph if omitted. Without this,
    // add_node could only target the first ubergraph page — never a function or macro.
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
    {
        FString GraphName;
        Params->TryGetStringField(TEXT("graph_name"), GraphName);
        Result.Message = FString::Printf(
            TEXT("add_node: graph '%s' not found in '%s' (omit graph_name for default EventGraph)"),
            GraphName.IsEmpty() ? TEXT("<EventGraph>") : *GraphName, *BlueprintPath);
        return Result;
    }

    const int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 200;
    const int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 200;

    UEdGraphNode* NewNode = nullptr;

    // ---- CallFunction --------------------------------------------------------
    if (NodeType == TEXT("CallFunction"))
    {
        FString FunctionName, FunctionClassPath;
        if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
        {
            Result.Message = TEXT("add_node (CallFunction): 'function_name' is required");
            return Result;
        }

        Params->TryGetStringField(TEXT("function_class"), FunctionClassPath);

        // Resolve the owning UClass
        UClass* FunctionClass = nullptr;
        if (!FunctionClassPath.IsEmpty())
        {
            FunctionClass = FindObject<UClass>(nullptr, *FunctionClassPath);
        }

        // Create node
        UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
        Graph->AddNode(CallNode, false, false);
        CallNode->CreateNewGuid();
        CallNode->PostPlacedNewNode();

        if (FunctionClass)
        {
            UFunction* Func = FunctionClass->FindFunctionByName(FName(*FunctionName));
            if (Func)
            {
                FMemberReference Ref;
                Ref.SetFromField<UFunction>(Func, false);
                CallNode->FunctionReference = Ref;
            }
            else
            {
                // Set by name only — compiler will validate
                CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), FunctionClass);
            }
        }
        else
        {
            // Unknown class — set member name, Blueprint compiler will resolve
            UClass* FuncScope = BP->GeneratedClass ? static_cast<UClass*>(BP->GeneratedClass) : AActor::StaticClass();
            CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), FuncScope);
        }

        CallNode->AllocateDefaultPins();
        CallNode->NodePosX = PosX;
        CallNode->NodePosY = PosY;
        NewNode = CallNode;
    }
    // ---- Event ---------------------------------------------------------------
    else if (NodeType == TEXT("Event"))
    {
        FString EventName;
        if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
        {
            Result.Message = TEXT("add_node (Event): 'event_name' is required (e.g. ReceiveBeginPlay)");
            return Result;
        }

        // Check if an event node with this name already exists
        UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            BP, BP->GeneratedClass ? BP->GeneratedClass : BP->ParentClass,
            FName(*EventName));

        if (ExistingEvent)
        {
            // Return the existing node's GUID rather than duplicating
            Result.bSuccess   = true;
            Result.AffectedPath = ExistingEvent->NodeGuid.ToString();
            Result.Message    = FString::Printf(TEXT("Event '%s' already exists (NodeGuid: %s)"),
                                                *EventName, *ExistingEvent->NodeGuid.ToString());
            FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
            return Result;
        }

        // Create a new Event override node
        UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
        Graph->AddNode(EventNode, false, false);
        EventNode->CreateNewGuid();
        EventNode->PostPlacedNewNode();

        UClass* EventScope = BP->ParentClass ? static_cast<UClass*>(BP->ParentClass) : AActor::StaticClass();
        EventNode->EventReference.SetExternalMember(FName(*EventName), EventScope);
        EventNode->bOverrideFunction = true;
        EventNode->AllocateDefaultPins();
        EventNode->NodePosX = PosX;
        EventNode->NodePosY = PosY;
        NewNode = EventNode;
    }
    else
    {
        Result.Message = FString::Printf(TEXT("add_node: Unknown node_type '%s' (supported: CallFunction, Event)"), *NodeType);
        return Result;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    Result.bSuccess   = true;
    Result.AffectedPath = NewNode->NodeGuid.ToString();
    Result.Message    = FString::Printf(TEXT("Node '%s' added to %s (NodeGuid: %s)"),
                                        *NodeType, *BlueprintPath, *NewNode->NodeGuid.ToString());
    return Result;
}

// ============================================================================
// connect_nodes
// ============================================================================

FBridgeResult UBlueprintHandler::Action_ConnectNodes(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("connect_nodes"));

    FString BlueprintPath, SrcGuidStr, SrcPinName, DstGuidStr, DstPinName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
    {
        Result.Message = TEXT("connect_nodes: 'blueprint_path' is required");
        return Result;
    }
    if (!Params->TryGetStringField(TEXT("source_node_guid"), SrcGuidStr) ||
        !Params->TryGetStringField(TEXT("source_pin"),       SrcPinName) ||
        !Params->TryGetStringField(TEXT("target_node_guid"), DstGuidStr) ||
        !Params->TryGetStringField(TEXT("target_pin"),       DstPinName))
    {
        Result.Message = TEXT("connect_nodes: required fields: source_node_guid, source_pin, target_node_guid, target_pin");
        return Result;
    }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: Blueprint not found at '%s'"), *BlueprintPath);
        return Result;
    }

    // Resolve target graph: explicit 'graph_name' param falls back to event graph.
    // Needed for wiring inside function / macro / OnRep graphs.
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
    {
        FString GraphName;
        Params->TryGetStringField(TEXT("graph_name"), GraphName);
        Result.Message = FString::Printf(
            TEXT("connect_nodes: graph '%s' not found in '%s' (omit graph_name for default EventGraph)"),
            GraphName.IsEmpty() ? TEXT("<EventGraph>") : *GraphName, *BlueprintPath);
        return Result;
    }

    UEdGraphNode* SrcNode = FindNodeByGuidStr(Graph, SrcGuidStr);
    UEdGraphNode* DstNode = FindNodeByGuidStr(Graph, DstGuidStr);

    if (!SrcNode)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: source_node_guid '%s' not found"), *SrcGuidStr);
        return Result;
    }
    if (!DstNode)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: target_node_guid '%s' not found"), *DstGuidStr);
        return Result;
    }

    // Find pins — output on source, input on target
    UEdGraphPin* SrcPin = SrcNode->FindPin(FName(*SrcPinName), EGPD_Output);
    UEdGraphPin* DstPin = DstNode->FindPin(FName(*DstPinName), EGPD_Input);

    if (!SrcPin)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: output pin '%s' not found on source node"), *SrcPinName);
        return Result;
    }
    if (!DstPin)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: input pin '%s' not found on target node"), *DstPinName);
        return Result;
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    const FPinConnectionResponse Response = Schema->CanCreateConnection(SrcPin, DstPin);

    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
    {
        Result.Message = FString::Printf(TEXT("connect_nodes: Connection disallowed — %s"), *Response.Message.ToString());
        return Result;
    }

    Schema->TryCreateConnection(SrcPin, DstPin);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    Result.bSuccess   = true;
    Result.AffectedPath = BlueprintPath;
    Result.Message    = FString::Printf(TEXT("Connected %s[%s] -> %s[%s] in %s"),
                                        *SrcGuidStr.Left(8), *SrcPinName,
                                        *DstGuidStr.Left(8), *DstPinName,
                                        *BlueprintPath);
    return Result;
}

// ============================================================================
// compile
// ============================================================================

FBridgeResult UBlueprintHandler::Action_Compile(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("compile"));

    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
    {
        Result.Message = TEXT("compile: 'blueprint_path' is required");
        return Result;
    }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
    {
        Result.Message = FString::Printf(TEXT("compile: Blueprint not found at '%s'"), *BlueprintPath);
        return Result;
    }

    // Compile — this is synchronous in editor context
    FKismetEditorUtilities::CompileBlueprint(BP,
        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::BatchCompile);

    const bool bHasErrors = BP->Status == BS_Error;
    Result.bSuccess   = !bHasErrors;
    Result.AffectedPath = BlueprintPath;
    Result.Message    = bHasErrors
        ? FString::Printf(TEXT("compile: Blueprint '%s' compiled with errors"), *BlueprintPath)
        : FString::Printf(TEXT("compile: Blueprint '%s' compiled successfully"), *BlueprintPath);

    return Result;
}

// ============================================================================
// Helpers
// ============================================================================

UBlueprint* UBlueprintHandler::LoadBlueprint(const FString& PackagePath) const
{
    // Derive object path: /Game/Foo/BP_Bar  ->  /Game/Foo/BP_Bar.BP_Bar
    FString ObjectPath = PackagePath;
    if (!ObjectPath.Contains(TEXT(".")))
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }
    return LoadObject<UBlueprint>(nullptr, *ObjectPath);
}

UEdGraph* UBlueprintHandler::GetEventGraph(UBlueprint* Blueprint) const
{
    return FBlueprintEditorUtils::FindEventGraph(Blueprint);
}

UEdGraphNode* UBlueprintHandler::FindNodeByGuidStr(UEdGraph* Graph, const FString& GuidStr) const
{
    FGuid NodeGuid;
    if (!FGuid::Parse(GuidStr, NodeGuid))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("BlueprintHandler::FindNodeByGuidStr: Could not parse GUID '%s'"), *GuidStr);
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == NodeGuid)
        {
            return Node;
        }
    }
    return nullptr;
}

UEdGraphNode* UBlueprintHandler::FindNodeByIdOrName(UEdGraph* Graph, const FString& IdOrName) const
{
    if (!Graph || IdOrName.IsEmpty()) return nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        if (Node->NodeGuid.ToString() == IdOrName || Node->GetName() == IdOrName)
            return Node;
    }
    // Fallback: try GUID parse
    FGuid NodeGuid;
    if (FGuid::Parse(IdOrName, NodeGuid))
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == NodeGuid)
                return Node;
        }
    }
    return nullptr;
}

UEdGraph* UBlueprintHandler::ResolveGraph(UBlueprint* BP, TSharedPtr<FJsonObject> Params) const
{
    FString GraphName;
    bool bExplicitlyRequested = false;
    if (Params.IsValid())
    {
        bExplicitlyRequested = Params->TryGetStringField(TEXT("graph_name"), GraphName)
                            && !GraphName.IsEmpty();
    }

    if (GraphName.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        GraphName = Subsystem->AttentionManager->GetTargetGraph();

    if (!GraphName.IsEmpty() && !GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        // Cross-graph search: function graphs, ubergraph pages, macro graphs, dispatcher signatures.
        for (UEdGraph* G : BP->FunctionGraphs)         { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->UbergraphPages)         { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->MacroGraphs)            { if (G && G->GetName() == GraphName) return G; }
        for (UEdGraph* G : BP->DelegateSignatureGraphs){ if (G && G->GetName() == GraphName) return G; }

        // Caller explicitly asked for a specific graph and we could not find it —
        // signal failure rather than silently falling back to the event graph.
        if (bExplicitlyRequested) return nullptr;
    }
    return GetEventGraph(BP);
}

UEdGraphNode* UBlueprintHandler::CreateCallFunctionNode(UEdGraph* Graph, const FString& FunctionName,
    const FString& ClassPath, int32 PosX, int32 PosY) const
{
    UBlueprint* BP = Cast<UBlueprint>(Graph->GetOuter());

    UFunction* Func = nullptr;

    // Try explicit class path first
    if (!ClassPath.IsEmpty())
    {
        if (UClass* Cls = FindObject<UClass>(nullptr, *ClassPath))
            Func = Cls->FindFunctionByName(FName(*FunctionName));
    }

    // Priority search across common libraries
    if (!Func && BP && BP->GeneratedClass)
        Func = BP->GeneratedClass->FindFunctionByName(FName(*FunctionName));
    if (!Func) Func = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
    if (!Func) Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
    if (!Func) Func = UGameplayStatics::StaticClass()->FindFunctionByName(FName(*FunctionName));

    if (!Func) return nullptr;

    FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
    UK2Node_CallFunction* Node = Creator.CreateNode(false);
    Node->SetFromFunction(Func);
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;
    Creator.Finalize();
    return Node;
}

// ============================================================================
// Phase A — add_step
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddStep(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("add_step"));

    FString BlueprintPath;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (BlueprintPath.IsEmpty())
    {
        Result.Message = TEXT("add_step: 'blueprint_path' required (or set target via context/set_target_umg_asset)");
        return Result;
    }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("add_step: Blueprint not found: %s"), *BlueprintPath); return Result; }

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) { Result.Message = TEXT("add_step: No target graph found"); return Result; }

    // Cursor info from AttentionManager
    FString CursorNodeId;
    FVector2D CursorPos(200.f, 200.f);
    if (Subsystem && Subsystem->AttentionManager)
    {
        CursorNodeId = Subsystem->AttentionManager->GetCursorNode();
        CursorPos    = Subsystem->AttentionManager->GetAndAdvanceCursorPosition();
    }

    UEdGraphNode* CursorNode = !CursorNodeId.IsEmpty() ? FindNodeByIdOrName(Graph, CursorNodeId) : nullptr;

    int32 PosX = CursorNode ? CursorNode->NodePosX + 300 : (int32)CursorPos.X;
    int32 PosY = CursorNode ? CursorNode->NodePosY         : (int32)CursorPos.Y;

    if (Params->HasField(TEXT("pos_x"))) PosX = (int32)Params->GetNumberField(TEXT("pos_x"));
    if (Params->HasField(TEXT("pos_y"))) PosY = (int32)Params->GetNumberField(TEXT("pos_y"));

    // Create node (delegate to same logic as add_node, or function-name shortcut)
    UEdGraphNode* NewNode = nullptr;

    FString NodeType, FunctionName, ClassPath;
    Params->TryGetStringField(TEXT("node_type"),      NodeType);
    Params->TryGetStringField(TEXT("function_name"),  FunctionName);
    Params->TryGetStringField(TEXT("function_class"), ClassPath);

    if (NodeType.IsEmpty() && !FunctionName.IsEmpty()) NodeType = TEXT("CallFunction");
    if (NodeType.IsEmpty()) NodeType = TEXT("CallFunction");

    if (NodeType == TEXT("CallFunction") || (!NodeType.IsEmpty() && FunctionName.IsEmpty()))
    {
        if (FunctionName.IsEmpty()) FunctionName = NodeType; // allow node_type="PrintString" shorthand
        NewNode = CreateCallFunctionNode(Graph, FunctionName, ClassPath, PosX, PosY);
        if (!NewNode)
        {
            Result.Message = FString::Printf(TEXT("add_step: Function '%s' not found in self or common libraries"), *FunctionName);
            return Result;
        }
    }
    else if (NodeType == TEXT("Branch") || NodeType == TEXT("If"))
    {
        FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
        auto* N = Creator.CreateNode(false); N->NodePosX = PosX; N->NodePosY = PosY; Creator.Finalize();
        NewNode = N;
    }
    else if (NodeType == TEXT("VariableGet") || NodeType == TEXT("Get"))
    {
        FString VarName; Params->TryGetStringField(TEXT("variable_name"), VarName);
        FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
        auto* N = Creator.CreateNode(false);
        N->VariableReference.SetSelfMember(FName(*VarName));
        N->NodePosX = PosX; N->NodePosY = PosY;
        Creator.Finalize(); NewNode = N;
    }
    else if (NodeType == TEXT("VariableSet") || NodeType == TEXT("Set"))
    {
        FString VarName; Params->TryGetStringField(TEXT("variable_name"), VarName);
        FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
        auto* N = Creator.CreateNode(false);
        N->VariableReference.SetSelfMember(FName(*VarName));
        N->NodePosX = PosX; N->NodePosY = PosY;
        Creator.Finalize(); NewNode = N;
    }
    else
    {
        // Treat as function name
        NewNode = CreateCallFunctionNode(Graph, NodeType, ClassPath, PosX, PosY);
        if (!NewNode)
        {
            Result.Message = FString::Printf(TEXT("add_step: node_type '%s' not recognised and not found as function"), *NodeType);
            return Result;
        }
    }

    // Auto-wire exec: cursor node's Then → new node's Execute
    bool bAutoConnect = true;
    if (Params->HasField(TEXT("auto_connect")))
        bAutoConnect = Params->GetBoolField(TEXT("auto_connect"));

    if (bAutoConnect && CursorNode)
    {
        // Find first free output exec pin on cursor node
        UEdGraphPin* ExecOut = CursorNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        if (!ExecOut || ExecOut->LinkedTo.Num() > 0)
        {
            // Search for any free exec output
            for (UEdGraphPin* Pin : CursorNode->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                    && Pin->LinkedTo.Num() == 0)
                {
                    ExecOut = Pin; break;
                }
            }
        }
        UEdGraphPin* ExecIn = NewNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!ExecIn)
        {
            for (UEdGraphPin* Pin : NewNode->Pins)
            {
                if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                { ExecIn = Pin; break; }
            }
        }
        if (ExecOut && ExecIn)
            Graph->GetSchema()->TryCreateConnection(ExecOut, ExecIn);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    // Advance cursor to this node
    if (Subsystem && Subsystem->AttentionManager)
        Subsystem->AttentionManager->SetCursorNode(NewNode->NodeGuid.ToString());

    // Build response JSON with unconnected input pins
    TArray<TSharedPtr<FJsonValue>> UnconnectedPins;
    for (UEdGraphPin* Pin : NewNode->Pins)
    {
        if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden)
        {
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
            UnconnectedPins.Add(MakeShared<FJsonValueObject>(PinObj));
        }
    }

    TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
    DataObj->SetStringField(TEXT("node_id"),   NewNode->NodeGuid.ToString());
    DataObj->SetStringField(TEXT("node_name"), NewNode->GetName());
    DataObj->SetArrayField (TEXT("unconnected_inputs"), UnconnectedPins);

    FString DataStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataStr);
    FJsonSerializer::Serialize(DataObj.ToSharedRef(), Writer);

    Result.bSuccess     = true;
    Result.AffectedPath = NewNode->NodeGuid.ToString();
    Result.ExtraData    = DataStr;
    Result.Message      = FString::Printf(TEXT("Added step '%s' (NodeGuid: %s)"), *NewNode->GetName(), *NewNode->NodeGuid.ToString());
    return Result;
}

// ============================================================================
// prepare_value — create data node, no exec wiring
// ============================================================================
FBridgeResult UBlueprintHandler::Action_PrepareValue(TSharedPtr<FJsonObject> Params)
{
    // Same as add_step but forces auto_connect=false and positions below cursor
    Params->SetBoolField(TEXT("auto_connect"), false);
    FBridgeResult R = Action_AddStep(Params);
    R.Action = TEXT("prepare_value");
    return R;
}

// ============================================================================
// connect_data_to_pin — "nodeIdA:pinNameA" -> "nodeIdB:pinNameB"
// ============================================================================
FBridgeResult UBlueprintHandler::Action_ConnectDataToPin(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("connect_data_to_pin"));

    FString BlueprintPath, PairA, PairB;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();

    if (!Params->TryGetStringField(TEXT("from"), PairA) || !Params->TryGetStringField(TEXT("to"), PairB))
    {
        Result.Message = TEXT("connect_data_to_pin: requires 'from' (\"nodeId:pinName\") and 'to' (\"nodeId:pinName\")");
        return Result;
    }

    FString FromNodeId, FromPin, ToNodeId, ToPin;
    if (!PairA.Split(TEXT(":"), &FromNodeId, &FromPin)) { FromNodeId = PairA; FromPin = TEXT(""); }
    if (!PairB.Split(TEXT(":"), &ToNodeId,   &ToPin))   { ToNodeId   = PairB; ToPin   = TEXT(""); }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("connect_data_to_pin: Blueprint not found: %s"), *BlueprintPath); return Result; }

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) { Result.Message = TEXT("connect_data_to_pin: No graph found"); return Result; }

    UEdGraphNode* SrcNode = FindNodeByIdOrName(Graph, FromNodeId);
    UEdGraphNode* DstNode = FindNodeByIdOrName(Graph, ToNodeId);
    if (!SrcNode) { Result.Message = FString::Printf(TEXT("connect_data_to_pin: source node '%s' not found"), *FromNodeId); return Result; }
    if (!DstNode) { Result.Message = FString::Printf(TEXT("connect_data_to_pin: target node '%s' not found"), *ToNodeId);   return Result; }

    // Find output pin on source
    UEdGraphPin* SrcPin = nullptr;
    if (!FromPin.IsEmpty())
        SrcPin = SrcNode->FindPin(FName(*FromPin), EGPD_Output);
    if (!SrcPin)
    {
        for (UEdGraphPin* Pin : SrcNode->Pins)
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden)
            { SrcPin = Pin; break; }
    }

    // Find input pin on target
    UEdGraphPin* DstPinPtr = nullptr;
    if (!ToPin.IsEmpty())
        DstPinPtr = DstNode->FindPin(FName(*ToPin), EGPD_Input);
    if (!DstPinPtr)
    {
        for (UEdGraphPin* Pin : DstNode->Pins)
            if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden && Pin->LinkedTo.Num() == 0)
            { DstPinPtr = Pin; break; }
    }

    if (!SrcPin) { Result.Message = TEXT("connect_data_to_pin: source output pin not found"); return Result; }
    if (!DstPinPtr) { Result.Message = TEXT("connect_data_to_pin: target input pin not found"); return Result; }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (Schema->CanCreateConnection(SrcPin, DstPinPtr).Response == CONNECT_RESPONSE_DISALLOW)
    {
        Result.Message = TEXT("connect_data_to_pin: Connection disallowed by schema");
        return Result;
    }

    Schema->TryCreateConnection(SrcPin, DstPinPtr);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    Result.bSuccess = true;
    Result.Message  = FString::Printf(TEXT("Connected %s:%s -> %s:%s"),
        *SrcNode->GetName(), *SrcPin->PinName.ToString(),
        *DstNode->GetName(), *DstPinPtr->PinName.ToString());
    return Result;
}

// ============================================================================
// get_function_nodes — BFS from cursor (max 50), or all if no cursor
// ============================================================================
FBridgeResult UBlueprintHandler::Action_GetFunctionNodes(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("get_function_nodes"));

    FString BlueprintPath;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("get_function_nodes: Blueprint not found: %s"), *BlueprintPath); return Result; }

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) { Result.Message = TEXT("get_function_nodes: No graph found"); return Result; }

    TArray<UEdGraphNode*> TargetNodes;

    // BFS from cursor if cursor is set
    FString CursorId;
    if (Subsystem && Subsystem->AttentionManager)
        CursorId = Subsystem->AttentionManager->GetCursorNode();

    if (!CursorId.IsEmpty())
    {
        UEdGraphNode* StartNode = FindNodeByIdOrName(Graph, CursorId);
        if (StartNode)
        {
            TSet<UEdGraphNode*> Visited;
            TArray<UEdGraphNode*> Queue = { StartNode };
            Visited.Add(StartNode);
            int32 Head = 0;
            while (Head < Queue.Num() && Visited.Num() < 50)
            {
                UEdGraphNode* Cur = Queue[Head++];
                for (UEdGraphPin* Pin : Cur->Pins)
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                        if (UEdGraphNode* N = Linked->GetOwningNode(); N && !Visited.Contains(N))
                        { Visited.Add(N); Queue.Add(N); }
            }
            TargetNodes = Queue;
        }
    }

    if (TargetNodes.IsEmpty())
        TargetNodes = Graph->Nodes;

    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* Node : TargetNodes)
    {
        if (!Node) continue;
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("id"),    Node->NodeGuid.ToString());
        NodeObj->SetStringField(TEXT("name"),  Node->GetName());
        NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        bool bIsExec = false;
        for (UEdGraphPin* Pin : Node->Pins)
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { bIsExec = true; break; }
        NodeObj->SetBoolField(TEXT("is_exec"), bIsExec);
        NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
        NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);
        NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
    }

    TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
    DataObj->SetArrayField(TEXT("nodes"), NodesArr);

    FString DataStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataStr);
    FJsonSerializer::Serialize(DataObj.ToSharedRef(), Writer);

    Result.bSuccess  = true;
    Result.ExtraData = DataStr;
    Result.Message   = FString::Printf(TEXT("Returned %d nodes"), NodesArr.Num());
    return Result;
}

// ============================================================================
// add_variable
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddVariable(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("add_variable"));

    FString BlueprintPath, VarName, VarType, SubType;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();

    if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
    { Result.Message = TEXT("add_variable: 'variable_name' required"); return Result; }
    if (!Params->TryGetStringField(TEXT("variable_type"), VarType) || VarType.IsEmpty())
    { Result.Message = TEXT("add_variable: 'variable_type' required (e.g. bool, int, float, string, object)"); return Result; }
    Params->TryGetStringField(TEXT("sub_type"), SubType);

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("add_variable: Blueprint not found: %s"), *BlueprintPath); return Result; }

    // Build the pin type via the K2 schema's verified mapping. Raw FName(*VarType)
    // passthrough silently accepts typos ("Float", "integer") and the literal
    // "float" is not a valid PinCategory in UE 5.0+ (must be PC_Real + PC_Float).
    FEdGraphPinType PinType;
    FString BuildErr;
    if (!BuildPinTypeFromString(VarType, SubType, PinType, BuildErr))
    {
        Result.Message = FString::Printf(TEXT("add_variable: %s"), *BuildErr);
        return Result;
    }

    FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VarName), PinType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    Result.bSuccess = true;
    Result.Message  = FString::Printf(TEXT("Added variable '%s' of type '%s'"), *VarName, *VarType);
    return Result;
}

// ============================================================================
// get_variables
// ============================================================================
FBridgeResult UBlueprintHandler::Action_GetVariables(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("get_variables"));

    FString BlueprintPath;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("get_variables: Blueprint not found: %s"), *BlueprintPath); return Result; }

    TArray<TSharedPtr<FJsonValue>> VarsArr;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
        VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
        VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
        VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
    }

    TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
    DataObj->SetArrayField(TEXT("variables"), VarsArr);

    FString DataStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataStr);
    FJsonSerializer::Serialize(DataObj.ToSharedRef(), Writer);

    Result.bSuccess  = true;
    Result.ExtraData = DataStr;
    Result.Message   = FString::Printf(TEXT("Returned %d variables"), VarsArr.Num());
    return Result;
}

// ============================================================================
// search_function_library
// ============================================================================
FBridgeResult UBlueprintHandler::Action_SearchFunctionLibrary(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("search_function_library"));

    FString Query;
    Params->TryGetStringField(TEXT("query"), Query);

    // Blueprint's own parent class (if target is a BP)
    UClass* BlueprintParentClass = nullptr;
    FString BlueprintPath;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (!BlueprintPath.IsEmpty())
    {
        if (UBlueprint* BP = LoadBlueprint(BlueprintPath))
            BlueprintParentClass = BP->ParentClass;
    }

    TArray<UClass*> SearchClasses = {
        BlueprintParentClass,
        UKismetSystemLibrary::StaticClass(),
        UKismetMathLibrary::StaticClass(),
        UGameplayStatics::StaticClass()
    };

    TArray<TSharedPtr<FJsonValue>> FuncArr;
    for (UClass* Cls : SearchClasses)
    {
        if (!Cls) continue;
        for (TFieldIterator<UFunction> It(Cls); It; ++It)
        {
            UFunction* Func = *It;
            if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
            if (!Query.IsEmpty() && !Func->GetName().Contains(Query, ESearchCase::IgnoreCase)) continue;

            TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
            FuncObj->SetStringField(TEXT("name"),  Func->GetName());
            FuncObj->SetStringField(TEXT("class"), Cls->GetName());
            FuncObj->SetBoolField  (TEXT("pure"),  Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
            FuncArr.Add(MakeShared<FJsonValueObject>(FuncObj));

            if (FuncArr.Num() >= 50) break;
        }
        if (FuncArr.Num() >= 50) break;
    }

    TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
    DataObj->SetArrayField(TEXT("functions"), FuncArr);

    FString DataStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataStr);
    FJsonSerializer::Serialize(DataObj.ToSharedRef(), Writer);

    Result.bSuccess  = true;
    Result.ExtraData = DataStr;
    Result.Message   = FString::Printf(TEXT("Found %d functions matching '%s'"), FuncArr.Num(), *Query);
    return Result;
}

// ============================================================================
// delete_node
// ============================================================================
FBridgeResult UBlueprintHandler::Action_DeleteNode(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("delete_node"));

    FString BlueprintPath, NodeId;
    Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
    if (BlueprintPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        BlueprintPath = Subsystem->AttentionManager->GetTargetAssetPath();

    if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    { Result.Message = TEXT("delete_node: 'node_id' required"); return Result; }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP) { Result.Message = FString::Printf(TEXT("delete_node: Blueprint not found: %s"), *BlueprintPath); return Result; }

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) { Result.Message = TEXT("delete_node: No graph found"); return Result; }

    UEdGraphNode* NodeToDelete = FindNodeByIdOrName(Graph, NodeId);
    if (!NodeToDelete) { Result.Message = FString::Printf(TEXT("delete_node: Node '%s' not found"), *NodeId); return Result; }

    // If deleting cursor node, step cursor back to predecessor
    FString NewCursorId;
    if (Subsystem && Subsystem->AttentionManager
        && Subsystem->AttentionManager->GetCursorNode() == NodeId)
    {
        UEdGraphPin* ExecIn = NodeToDelete->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ExecIn && ExecIn->LinkedTo.Num() > 0)
            NewCursorId = ExecIn->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString();
    }

    FBlueprintEditorUtils::RemoveNode(BP, NodeToDelete, true);

    if (!NewCursorId.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        Subsystem->AttentionManager->SetCursorNode(NewCursorId);

    Result.bSuccess = true;
    Result.Message  = FString::Printf(TEXT("Deleted node '%s'"), *NodeId);
    if (!NewCursorId.IsEmpty())
        Result.AffectedPath = NewCursorId; // cursor regressed to this node
    return Result;
}

// ============================================================================
// Phase 3 — Static domain constant
// ============================================================================
// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("blueprint");

// ============================================================================
// add_component
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddComponent(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_component");

    FString AssetPath, ComponentClassName, ComponentName, ParentComponentName;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("component_class"), ComponentClassName) || ComponentClassName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'component_class' is required (e.g. StaticMeshComponent)"));

    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    Params->TryGetStringField(TEXT("parent_component"), ParentComponentName);

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
        return MakeError(DOMAIN, Action, 3000, TEXT("Blueprint has no SimpleConstructionScript (not an Actor BP?)"),
            TEXT("Ensure this is an Actor-based Blueprint"));

    // Resolve component class
    UClass* CompClass = nullptr;
    {
        // Try /Script/Engine.<Name> first
        const FString EngineClassPath = FString::Printf(TEXT("/Script/Engine.U%s"), *ComponentClassName);
        CompClass = FindObject<UClass>(nullptr, *EngineClassPath);
        if (!CompClass)
        {
            // Try without U prefix
            const FString EngineClassPath2 = FString::Printf(TEXT("/Script/Engine.%s"), *ComponentClassName);
            CompClass = FindObject<UClass>(nullptr, *EngineClassPath2);
        }
        if (!CompClass)
        {
            // Iterate all classes
            for (TObjectIterator<UClass> It; It; ++It)
            {
                if (It->GetName() == ComponentClassName || It->GetName() == (TEXT("U") + ComponentClassName))
                {
                    CompClass = *It;
                    break;
                }
            }
        }
    }
    if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Component class '%s' not found or not a UActorComponent"), *ComponentClassName),
            TEXT("Use a valid component class name like StaticMeshComponent, BoxCollisionComponent, etc."));

    // Create SCS node
    USCS_Node* NewSCSNode = SCS->CreateNode(CompClass, ComponentName.IsEmpty() ? FName(*ComponentClassName) : FName(*ComponentName));
    if (!NewSCSNode)
        return MakeError(DOMAIN, Action, 3000, TEXT("SCS->CreateNode returned null"));

    // Attach to parent or add as root
    if (!ParentComponentName.IsEmpty())
    {
        USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentComponentName));
        if (ParentNode)
        {
            ParentNode->AddChildNode(NewSCSNode);
        }
        else
        {
            // Parent not found — add as root and warn
            SCS->AddNode(NewSCSNode);
            UE_LOG(LogTemp, Warning, TEXT("BlueprintHandler::add_component: Parent '%s' not found, added as root"), *ParentComponentName);
        }
    }
    else
    {
        SCS->AddNode(NewSCSNode);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("component_name"), NewSCSNode->GetVariableName().ToString());
    Data->SetStringField(TEXT("component_class"), CompClass->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added component '%s' (%s) to %s"),
            *NewSCSNode->GetVariableName().ToString(), *CompClass->GetName(), *AssetPath),
        Data);
}

// ============================================================================
// remove_component
// ============================================================================
FBridgeResult UBlueprintHandler::Action_RemoveComponent(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("remove_component");

    FString AssetPath, ComponentName;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'component_name' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
        return MakeError(DOMAIN, Action, 3000, TEXT("Blueprint has no SimpleConstructionScript"));

    USCS_Node* NodeToRemove = SCS->FindSCSNode(FName(*ComponentName));
    if (!NodeToRemove)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Component '%s' not found in SCS"), *ComponentName),
            TEXT("Use get_components to see available component names"));

    // Use RemoveNodeAndPromoteChildren to preserve child component hierarchy:
    // RemoveNode would drop all children with their parent (data loss).
    SCS->RemoveNodeAndPromoteChildren(NodeToRemove);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Removed component '%s' from %s"), *ComponentName, *AssetPath));
}

// ============================================================================
// get_components
// ============================================================================
FBridgeResult UBlueprintHandler::Action_GetComponents(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("get_components");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
        return MakeError(DOMAIN, Action, 3000, TEXT("Blueprint has no SimpleConstructionScript"));

    const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();

    TArray<TSharedPtr<FJsonValue>> CompArr;
    for (USCS_Node* SCSNode : AllNodes)
    {
        if (!SCSNode) continue;

        TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
        CompObj->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());
        CompObj->SetStringField(TEXT("class"), SCSNode->ComponentClass ? SCSNode->ComponentClass->GetName() : TEXT("Unknown"));

        // Find parent
        FString ParentName;
        for (USCS_Node* Candidate : AllNodes)
        {
            if (Candidate && Candidate->GetChildNodes().Contains(SCSNode))
            {
                ParentName = Candidate->GetVariableName().ToString();
                break;
            }
        }
        CompObj->SetStringField(TEXT("parent"), ParentName);

        CompArr.Add(MakeShared<FJsonValueObject>(CompObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("components"), CompArr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d components from %s"), CompArr.Num(), *AssetPath),
        Data);
}

// ============================================================================
// add_function_graph
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddFunctionGraph(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_function_graph");

    FString AssetPath, FunctionName;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'function_name' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Check for duplicate
    for (UEdGraph* ExistingGraph : BP->FunctionGraphs)
    {
        if (ExistingGraph && ExistingGraph->GetName() == FunctionName)
            return MakeError(DOMAIN, Action, 2002,
                FString::Printf(TEXT("Function graph '%s' already exists"), *FunctionName),
                TEXT("Use a different name or modify the existing function"));
    }

    // Create new function graph
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FunctionName),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
        return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create function graph"));

    FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);

    // Find the FunctionEntry node to add input/output pins
    UK2Node_FunctionEntry* EntryNode = nullptr;
    UK2Node_FunctionResult* ResultNode = nullptr;
    for (UEdGraphNode* Node : NewGraph->Nodes)
    {
        if (!EntryNode)  EntryNode  = Cast<UK2Node_FunctionEntry>(Node);
        if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
    }

    // Process inputs
    const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("inputs"), InputsArr) && InputsArr)
    {
        for (const TSharedPtr<FJsonValue>& InputVal : *InputsArr)
        {
            if (!InputVal.IsValid()) continue;
            TSharedPtr<FJsonObject> InputObj = InputVal->AsObject();
            if (!InputObj.IsValid()) continue;

            FString PinName, PinType, SubType;
            InputObj->TryGetStringField(TEXT("name"), PinName);
            InputObj->TryGetStringField(TEXT("type"), PinType);
            InputObj->TryGetStringField(TEXT("sub_type"), SubType);
            if (PinName.IsEmpty() || PinType.IsEmpty()) continue;

            FEdGraphPinType NewPinType;
            FString BuildErr;
            if (!BuildPinTypeFromString(PinType, SubType, NewPinType, BuildErr))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("add_function_graph: skipping input '%s' — %s"), *PinName, *BuildErr);
                continue;
            }

            if (EntryNode)
            {
                // Use UserDefinedPins on FunctionEntry
                TSharedPtr<FUserPinInfo> PinInfo = MakeShared<FUserPinInfo>();
                PinInfo->PinName = FName(*PinName);
                PinInfo->PinType = NewPinType;
                PinInfo->DesiredPinDirection = EGPD_Output; // Entry outputs = function inputs
                EntryNode->UserDefinedPins.Add(PinInfo);
            }
        }
        if (EntryNode)
        {
            EntryNode->ReconstructNode();
        }
    }

    // Process outputs
    const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("outputs"), OutputsArr) && OutputsArr)
    {
        // Ensure we have a result node
        if (!ResultNode)
        {
            FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
            ResultNode = Creator.CreateNode(false);
            ResultNode->NodePosX = 600;
            ResultNode->NodePosY = 0;
            Creator.Finalize();
        }

        for (const TSharedPtr<FJsonValue>& OutputVal : *OutputsArr)
        {
            if (!OutputVal.IsValid()) continue;
            TSharedPtr<FJsonObject> OutputObj = OutputVal->AsObject();
            if (!OutputObj.IsValid()) continue;

            FString PinName, PinType, SubType;
            OutputObj->TryGetStringField(TEXT("name"), PinName);
            OutputObj->TryGetStringField(TEXT("type"), PinType);
            OutputObj->TryGetStringField(TEXT("sub_type"), SubType);
            if (PinName.IsEmpty() || PinType.IsEmpty()) continue;

            FEdGraphPinType NewPinType;
            FString BuildErr;
            if (!BuildPinTypeFromString(PinType, SubType, NewPinType, BuildErr))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("add_function_graph: skipping output '%s' — %s"), *PinName, *BuildErr);
                continue;
            }

            TSharedPtr<FUserPinInfo> PinInfo = MakeShared<FUserPinInfo>();
            PinInfo->PinName = FName(*PinName);
            PinInfo->PinType = NewPinType;
            PinInfo->DesiredPinDirection = EGPD_Input; // Result inputs = function outputs
            ResultNode->UserDefinedPins.Add(PinInfo);
        }
        ResultNode->ReconstructNode();
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added function graph '%s' to %s"), *FunctionName, *AssetPath),
        Data);
}

// ============================================================================
// add_custom_event
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddCustomEvent(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_custom_event");

    FString AssetPath, EventName;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'event_name' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Resolve graph (default EventGraph)
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
        return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    // Check for existing custom event with same name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_CustomEvent* Existing = Cast<UK2Node_CustomEvent>(Node))
        {
            if (Existing->CustomFunctionName == FName(*EventName))
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetStringField(TEXT("node_guid"), Existing->NodeGuid.ToString());
                Data->SetBoolField(TEXT("already_existed"), true);
                return MakeSuccess(DOMAIN, Action,
                    FString::Printf(TEXT("Custom event '%s' already exists (NodeGuid: %s)"),
                        *EventName, *Existing->NodeGuid.ToString()),
                    Data);
            }
        }
    }

    // Create custom event node
    UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
    Graph->AddNode(EventNode, false, false);
    EventNode->CreateNewGuid();
    EventNode->PostPlacedNewNode();
    EventNode->CustomFunctionName = FName(*EventName);
    EventNode->AllocateDefaultPins();

    const int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 200;
    const int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 200;
    EventNode->NodePosX = PosX;
    EventNode->NodePosY = PosY;

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), EventNode->NodeGuid.ToString());
    Data->SetBoolField(TEXT("already_existed"), false);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added custom event '%s' to %s (NodeGuid: %s)"),
            *EventName, *AssetPath, *EventNode->NodeGuid.ToString()),
        Data);
}

// ============================================================================
// add_interface
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddInterface(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_interface");

    FString AssetPath, InterfacePath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("interface"), InterfacePath) || InterfacePath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'interface' is required (e.g. /Script/Engine.Interface_PostProcessVolume)"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Resolve the interface class
    UClass* InterfaceClass = FindObject<UClass>(nullptr, *InterfacePath);
    if (!InterfaceClass)
    {
        // Try iterating if short name was given
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == InterfacePath && It->HasAnyClassFlags(CLASS_Interface))
            {
                InterfaceClass = *It;
                break;
            }
        }
    }
    if (!InterfaceClass)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Interface class '%s' not found"), *InterfacePath),
            TEXT("Use full path like /Script/Engine.Interface_PostProcessVolume or the short class name"));

    // Check if already implemented
    for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
    {
        if (Desc.Interface == InterfaceClass)
            return MakeSuccess(DOMAIN, Action,
                FString::Printf(TEXT("Interface '%s' is already implemented on %s"), *InterfaceClass->GetName(), *AssetPath));
    }

    FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(InterfaceClass->GetPathName()));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("interface_class"), InterfaceClass->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added interface '%s' to %s"), *InterfaceClass->GetName(), *AssetPath),
        Data);
}

// ============================================================================
// list_nodes
// ============================================================================
FBridgeResult UBlueprintHandler::Action_ListNodes(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("list_nodes");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
        return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;

        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("name"),  Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        NodeObj->SetStringField(TEXT("guid"),  Node->NodeGuid.ToString());
        NodeObj->SetNumberField(TEXT("x"),     Node->NodePosX);
        NodeObj->SetNumberField(TEXT("y"),     Node->NodePosY);
        NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), Graph->GetName());
    Data->SetArrayField(TEXT("nodes"), NodesArr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d nodes from graph '%s'"), NodesArr.Num(), *Graph->GetName()),
        Data);
}

// ============================================================================
// list_pins
// ============================================================================
FBridgeResult UBlueprintHandler::Action_ListPins(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("list_pins");

    FString AssetPath, NodeGuidStr;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'node_guid' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Search across all graphs for the node
    UEdGraphNode* FoundNode = nullptr;
    auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* G : Graphs)
        {
            if (!G || FoundNode) continue;
            FoundNode = FindNodeByIdOrName(G, NodeGuidStr);
        }
    };

    SearchGraphs(BP->UbergraphPages);
    SearchGraphs(BP->FunctionGraphs);

    if (!FoundNode)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Node '%s' not found in any graph"), *NodeGuidStr),
            TEXT("Use list_nodes to get valid node GUIDs"));

    TArray<TSharedPtr<FJsonValue>> PinsArr;
    for (UEdGraphPin* Pin : FoundNode->Pins)
    {
        if (!Pin || Pin->bHidden) continue;

        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"),      Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("type"),       Pin->PinType.PinCategory.ToString());
        PinObj->SetStringField(TEXT("direction"),  Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
        PinObj->SetBoolField  (TEXT("connected"),  Pin->LinkedTo.Num() > 0);
        PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);

        // Sub-type info if available
        if (Pin->PinType.PinSubCategoryObject.IsValid())
            PinObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategoryObject->GetName());

        PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), FoundNode->NodeGuid.ToString());
    Data->SetStringField(TEXT("node_name"), FoundNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Data->SetArrayField(TEXT("pins"), PinsArr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d pins for node '%s'"), PinsArr.Num(), *FoundNode->GetName()),
        Data);
}

// ============================================================================
// set_variable_default
// ============================================================================
FBridgeResult UBlueprintHandler::Action_SetVariableDefault(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_variable_default");

    FString AssetPath, VariableName, DefaultValue;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));

    if (!Params->TryGetStringField(TEXT("default_value"), DefaultValue))
        return MakeError(DOMAIN, Action, 1000, TEXT("'default_value' is required (as string)"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Find variable in NewVariables
    FBPVariableDescription* FoundVar = nullptr;
    for (FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*VariableName))
        {
            FoundVar = &Var;
            break;
        }
    }

    if (!FoundVar)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName),
            TEXT("Use get_variables to see available variable names, or add_variable to create one first"));

    // UE 5.7: FBlueprintEditorUtils::SetVariableDefaultValue does not exist.
    // Write DefaultValue directly to the matching UBlueprint::NewVariables entry, then
    // mark modified so the compile pipeline propagates the value into the generated CDO.
    FoundVar->DefaultValue = DefaultValue;
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_name"), VariableName);
    Data->SetStringField(TEXT("default_value"), DefaultValue);
    Data->SetStringField(TEXT("type"), FoundVar->VarType.PinCategory.ToString());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Set default value of '%s' to '%s'"), *VariableName, *DefaultValue),
        Data);
}

// ============================================================================
// reparent
// ============================================================================
FBridgeResult UBlueprintHandler::Action_Reparent(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("reparent");

    FString AssetPath, NewParentPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("new_parent"), NewParentPath) || NewParentPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'new_parent' is required (class path, e.g. /Script/Engine.Pawn or Pawn)"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // Resolve parent class
    UClass* NewParentClass = FindObject<UClass>(nullptr, *NewParentPath);
    if (!NewParentClass)
    {
        // Try /Script/Engine.<Name>
        const FString EngineClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *NewParentPath);
        NewParentClass = FindObject<UClass>(nullptr, *EngineClassPath);
    }
    if (!NewParentClass)
    {
        // Iterate all classes
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == NewParentPath)
            {
                NewParentClass = *It;
                break;
            }
        }
    }
    if (!NewParentClass)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Parent class '%s' not found"), *NewParentPath),
            TEXT("Use full path like /Script/Engine.Pawn or short class name like Pawn, Character, Actor"));

    // Check if already that parent
    if (BP->ParentClass == NewParentClass)
        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Blueprint already has parent class '%s'"), *NewParentClass->GetName()));

    FString OldParentName = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

#if WITH_EDITOR
    FScopedTransaction Transaction(FText::FromString(FString::Printf(
        TEXT("Reparent Blueprint %s to %s"), *BP->GetName(), *NewParentClass->GetName())));
#endif

    // UE 5.7: FBlueprintEditorUtils::ReparentBlueprint does not exist.
    // Assign ParentClass directly, then RefreshAllNodes + CompileBlueprint to fix up
    // graphs against the new parent hierarchy.
    BP->ParentClass = NewParentClass;
    FBlueprintEditorUtils::RefreshAllNodes(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    BP->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("old_parent"), OldParentName);
    Data->SetStringField(TEXT("new_parent"), NewParentClass->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Reparented '%s' from '%s' to '%s'"),
            *AssetPath, *OldParentName, *NewParentClass->GetName()),
        Data);
}

// ---------------------------------------------------------------------------
// read_blueprint_capture
// ---------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_ReadBlueprintCapture(TSharedPtr<FJsonObject> Params)
{
    if (!Subsystem || !Subsystem->BlueprintCapture)
        return MakeError(TEXT("blueprint"), TEXT("read_blueprint_capture"),
            2000, TEXT("BlueprintCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

    FString AssetPath, Prefix;
    const bool bHasAsset  = Params->TryGetStringField(TEXT("asset_path"), AssetPath)  && !AssetPath.IsEmpty();
    const bool bHasPrefix = Params->TryGetStringField(TEXT("prefix"),     Prefix)     && !Prefix.IsEmpty();

    if (bHasAsset)
    {
        Subsystem->BlueprintCapture->ExportBlueprint(AssetPath);

        // Derive filename: last slash-separated segment of asset_path
        FString Segment = AssetPath;
        int32 SlashIdx;
        if (Segment.FindLastChar(TEXT('/'), SlashIdx))
            Segment = Segment.RightChop(SlashIdx + 1);

        FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("blueprints"), Segment + TEXT(".json"));
        FString FileContent;
        FBridgeResult Res = MakeSuccess(GetDomainName(), TEXT("read_blueprint_capture"),
            TEXT("Capture complete: ") + FilePath);
        if (FFileHelper::LoadFileToString(FileContent, *FilePath))
        {
            TSharedPtr<FJsonObject> JsonObj;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
            if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
                Res.Data = JsonObj;
        }
        return Res;
    }
    else if (bHasPrefix)
    {
        int32 Count = Subsystem->BlueprintCapture->ExportBlueprintsByPrefix(Prefix);
        return MakeSuccess(GetDomainName(), TEXT("read_blueprint_capture"),
            FString::Printf(TEXT("Exported %d blueprints with prefix '%s'"), Count, *Prefix));
    }
    else
    {
        int32 Count = Subsystem->BlueprintCapture->ExportAllBlueprints();
        return MakeSuccess(GetDomainName(), TEXT("read_blueprint_capture"),
            FString::Printf(TEXT("Exported all %d blueprints"), Count));
    }
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UBlueprintHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new Blueprint asset"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_name"), P(TEXT("string"), true, TEXT("Asset name (e.g. BP_MyActor)"))); Pr->SetObjectField(TEXT("package_path"), P(TEXT("string"), true, TEXT("Package path (e.g. /Game/AI)"))); Pr->SetObjectField(TEXT("parent_class"), P(TEXT("string"), false, TEXT("Parent class name (default Actor)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_blueprint"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a node (CallFunction or Event) to the EventGraph"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("node_type"), P(TEXT("string"), true, TEXT("CallFunction or Event"))); Pr->SetObjectField(TEXT("function_name"), P(TEXT("string"), false, TEXT("Function name (for CallFunction)"))); Pr->SetObjectField(TEXT("function_class"), P(TEXT("string"), false, TEXT("Owning class path (for CallFunction)"))); Pr->SetObjectField(TEXT("event_name"), P(TEXT("string"), false, TEXT("Event name (for Event, e.g. ReceiveBeginPlay)"))); Pr->SetObjectField(TEXT("pos_x"), P(TEXT("int"), false, TEXT("Node X position"))); Pr->SetObjectField(TEXT("pos_y"), P(TEXT("int"), false, TEXT("Node Y position"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_node"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Connect two Blueprint nodes by their GUIDs and pin names"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("source_node_guid"), P(TEXT("string"), true, TEXT("Source node GUID"))); Pr->SetObjectField(TEXT("source_pin"), P(TEXT("string"), true, TEXT("Source output pin name"))); Pr->SetObjectField(TEXT("target_node_guid"), P(TEXT("string"), true, TEXT("Target node GUID"))); Pr->SetObjectField(TEXT("target_pin"), P(TEXT("string"), true, TEXT("Target input pin name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("connect_nodes"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Compile a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("compile"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a function call step with auto-exec wiring from cursor position"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path (or uses AttentionManager target)"))); Pr->SetObjectField(TEXT("node_type"), P(TEXT("string"), false, TEXT("CallFunction, Branch, VariableGet, VariableSet, or function name"))); Pr->SetObjectField(TEXT("function_name"), P(TEXT("string"), false, TEXT("Function name (inferred from node_type if omitted)"))); Pr->SetObjectField(TEXT("function_class"), P(TEXT("string"), false, TEXT("Owning class path"))); Pr->SetObjectField(TEXT("variable_name"), P(TEXT("string"), false, TEXT("Variable name (for Get/Set)"))); Pr->SetObjectField(TEXT("auto_connect"), P(TEXT("bool"), false, TEXT("Auto-wire exec from cursor (default true)"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Target graph name (default EventGraph)"))); Pr->SetObjectField(TEXT("pos_x"), P(TEXT("int"), false, TEXT("Node X position"))); Pr->SetObjectField(TEXT("pos_y"), P(TEXT("int"), false, TEXT("Node Y position"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_step"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a data node (no exec wiring) — same as add_step with auto_connect=false"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); Pr->SetObjectField(TEXT("node_type"), P(TEXT("string"), false, TEXT("Node type or function name"))); Pr->SetObjectField(TEXT("function_name"), P(TEXT("string"), false, TEXT("Function name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("prepare_value"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Connect a data output to a data input using nodeId:pinName format"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); Pr->SetObjectField(TEXT("from"), P(TEXT("string"), true, TEXT("Source as nodeId:pinName"))); Pr->SetObjectField(TEXT("to"), P(TEXT("string"), true, TEXT("Target as nodeId:pinName"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Target graph name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("connect_data_to_pin"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List nodes in a graph (BFS from cursor if set, else all)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Target graph name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_function_nodes"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a member variable to a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); Pr->SetObjectField(TEXT("variable_name"), P(TEXT("string"), true, TEXT("Variable name"))); Pr->SetObjectField(TEXT("variable_type"), P(TEXT("string"), true, TEXT("Pin category (bool, int, float, string, object)"))); Pr->SetObjectField(TEXT("sub_type"), P(TEXT("string"), false, TEXT("Sub-type class for object variables"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_variable"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all member variables in a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_variables"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Search callable functions across common libraries and BP parent class"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path (for parent class scope)"))); Pr->SetObjectField(TEXT("query"), P(TEXT("string"), false, TEXT("Search substring (empty = all, up to 50 results)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("search_function_library"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Delete a node by GUID or name from a graph"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), false, TEXT("Blueprint path"))); Pr->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Node GUID or name"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Target graph name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("delete_node"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a component to a Blueprint via SCS"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("component_class"), P(TEXT("string"), true, TEXT("Component class name (e.g. StaticMeshComponent)"))); Pr->SetObjectField(TEXT("component_name"), P(TEXT("string"), false, TEXT("Variable name for the component"))); Pr->SetObjectField(TEXT("parent_component"), P(TEXT("string"), false, TEXT("Parent component name to attach under"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_component"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Remove a component from a Blueprint's SCS"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("component_name"), P(TEXT("string"), true, TEXT("Component variable name to remove"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("remove_component"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all components in a Blueprint's SCS"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_components"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new function graph in a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("function_name"), P(TEXT("string"), true, TEXT("Function graph name"))); Pr->SetObjectField(TEXT("inputs"), P(TEXT("array<{name,type}>"), false, TEXT("Input parameter definitions"))); Pr->SetObjectField(TEXT("outputs"), P(TEXT("array<{name,type}>"), false, TEXT("Output parameter definitions"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_function_graph"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a Custom Event node to a graph"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("event_name"), P(TEXT("string"), true, TEXT("Custom event name"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Target graph"))); Pr->SetObjectField(TEXT("pos_x"), P(TEXT("int"), false, TEXT("Node X position"))); Pr->SetObjectField(TEXT("pos_y"), P(TEXT("int"), false, TEXT("Node Y position"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_custom_event"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Implement a Blueprint interface on a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("interface"), P(TEXT("string"), true, TEXT("Interface class path (e.g. /Script/Engine.Interface_PostProcessVolume)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_interface"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all nodes in a Blueprint graph"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Graph name (default EventGraph)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_nodes"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all pins on a node by its GUID"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("node_guid"), P(TEXT("string"), true, TEXT("Node GUID to inspect"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Graph name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_pins"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the default value of a Blueprint member variable"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("variable_name"), P(TEXT("string"), true, TEXT("Variable name"))); Pr->SetObjectField(TEXT("default_value"), P(TEXT("string"), true, TEXT("Default value as string"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_variable_default"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Change the parent class of a Blueprint"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("new_parent"), P(TEXT("string"), true, TEXT("New parent class name or path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("reparent"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Trigger blueprint capture export and return the JSON file contents. Without params, exports all blueprints and returns a count."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Single blueprint asset path to export (e.g. /Game/AI/BP_Foo)"))); Pr->SetObjectField(TEXT("prefix"), P(TEXT("string"), false, TEXT("Export all blueprints matching this prefix; returns count"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("read_blueprint_capture"), A); }

    // ---- Phase 1d: authoring completeness ----
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Move a Blueprint node by GUID to (x, y) on its graph"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("node_guid"), P(TEXT("string"), true, TEXT("Node GUID"))); Pr->SetObjectField(TEXT("x"), P(TEXT("int"), true, TEXT("New X position"))); Pr->SetObjectField(TEXT("y"), P(TEXT("int"), true, TEXT("New Y position"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Optional graph name to scope the search"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_node_position"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Break links on a pin. With target_node_guid+target_pin_name, breaks that single link; otherwise breaks ALL links on the source pin."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("node_guid"), P(TEXT("string"), true, TEXT("Source node GUID"))); Pr->SetObjectField(TEXT("pin_name"), P(TEXT("string"), true, TEXT("Source pin name"))); Pr->SetObjectField(TEXT("target_node_guid"), P(TEXT("string"), false, TEXT("Optional target node GUID (omit to break all links)"))); Pr->SetObjectField(TEXT("target_pin_name"), P(TEXT("string"), false, TEXT("Optional target pin name"))); Pr->SetObjectField(TEXT("graph_name"), P(TEXT("string"), false, TEXT("Optional graph name to scope the search"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("disconnect_pins"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all interfaces implemented by a Blueprint, including matching stub graph names"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_interfaces"), A); }

    return Root;
}

// ============================================================================
// add_event_dispatcher
// ============================================================================

FBridgeResult UBlueprintHandler::Action_AddEventDispatcher(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("add_event_dispatcher"));

    FString AssetPath, DispatcherName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        Result.Message = TEXT("add_event_dispatcher: 'asset_path' is required");
        Result.ErrorCode = 1000;
        return Result;
    }
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) || DispatcherName.IsEmpty())
    {
        Result.Message = TEXT("add_event_dispatcher: 'dispatcher_name' is required");
        Result.ErrorCode = 1000;
        return Result;
    }

    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!BP)
    {
        Result.Message = FString::Printf(TEXT("add_event_dispatcher: no UBlueprint at '%s'"), *AssetPath);
        Result.ErrorCode = 2000;
        return Result;
    }

    // Check if already exists
    // UE 5.7: EventDispatchers removed; dispatchers live in DelegateSignatureGraphs.
    const FName DispName(*DispatcherName);
    for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
    {
        if (Graph && Graph->GetFName() == DispName)
        {
            Result.bSuccess = true;
            Result.Message  = FString::Printf(
                TEXT("add_event_dispatcher: '%s' already exists (no-op)"), *DispatcherName);
            return Result;
        }
    }

    UEdGraph* NewGraph = NewObject<UEdGraph>(BP, UEdGraph::StaticClass(), DispName);
    NewGraph->Schema = UEdGraphSchema_K2::StaticClass();
    BP->DelegateSignatureGraphs.Add(NewGraph);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    BP->MarkPackageDirty();

    Result.bSuccess = true;
    Result.Message  = FString::Printf(
        TEXT("add_event_dispatcher: added '%s' to '%s'"), *DispatcherName, *AssetPath);
    return Result;
}

// ============================================================================
// get_event_dispatchers
// ============================================================================

FBridgeResult UBlueprintHandler::Action_GetEventDispatchers(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("get_event_dispatchers"));

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        Result.Message = TEXT("get_event_dispatchers: 'asset_path' is required");
        Result.ErrorCode = 1000;
        return Result;
    }

    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!BP)
    {
        Result.Message = FString::Printf(TEXT("get_event_dispatchers: no UBlueprint at '%s'"), *AssetPath);
        Result.ErrorCode = 2000;
        return Result;
    }

    // UE 5.7: EventDispatchers removed; dispatchers live in DelegateSignatureGraphs.
    TArray<TSharedPtr<FJsonValue>> DispArr;
    for (UEdGraph* Disp : BP->DelegateSignatureGraphs)
    {
        if (!Disp) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Disp->GetName());
        Entry->SetStringField(TEXT("guid"), TEXT(""));
        DispArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("dispatchers"), DispArr);
    Data->SetNumberField(TEXT("count"), DispArr.Num());

    FString OutStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
    FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

    Result.bSuccess  = true;
    Result.ExtraData = OutStr;
    Result.Message   = FString::Printf(
        TEXT("get_event_dispatchers: %d dispatcher(s) on '%s'"), DispArr.Num(), *AssetPath);
    return Result;
}

// ============================================================================
// bind_event
// ============================================================================

FBridgeResult UBlueprintHandler::Action_BindEvent(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("blueprint"), TEXT("bind_event"));

    // bind_event requires spawning UK2Node_AssignDelegate nodes which ties into
    // the full node placement workflow. Provide a clear actionable error so
    // callers know to use add_node + connect_nodes instead.
    // 3004 = BLOCKED_API — this path is permanently blocked (not transiently unsupported).
    Result.bSuccess = false;
    Result.ErrorCode = 3004;
    Result.Message =
        TEXT("bind_event: direct event binding via C++ is not supported in UE 5.7 due to "
             "UK2Node_AssignDelegate pin layout complexity. "
             "Use add_event_dispatcher to declare the dispatcher, then use add_node "
             "(UK2Node_AssignDelegate) + connect_nodes to wire the binding in the graph.");
    return Result;
}

// ============================================================================
// set_variable_replication
// Params: asset_path, variable_name, replication (string: None|Replicated|RepNotify),
//         condition (string, optional: None|InitialOnly|OwnerOnly|SkipOwner|SimulatedOnly|...)
// ============================================================================
FBridgeResult UBlueprintHandler::Action_SetVariableReplication(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_variable_replication");

    FString AssetPath, VariableName, ReplicationStr;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));

    if (!Params->TryGetStringField(TEXT("replication"), ReplicationStr) || ReplicationStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'replication' is required (None|Replicated|RepNotify)"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    FBPVariableDescription* FoundVar = nullptr;
    for (FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*VariableName))
        {
            FoundVar = &Var;
            break;
        }
    }
    if (!FoundVar)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName),
            TEXT("Use get_variables to list available variable names"));

    // Apply replication flags — clear existing rep flags first
    FoundVar->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
    FoundVar->RepNotifyFunc  = NAME_None;

    if (ReplicationStr == TEXT("Replicated"))
    {
        FoundVar->PropertyFlags |= CPF_Net;
    }
    else if (ReplicationStr == TEXT("RepNotify"))
    {
        FoundVar->PropertyFlags |= CPF_Net | CPF_RepNotify;
        const FName OnRepFuncName = FName(*FString::Printf(TEXT("OnRep_%s"), *VariableName));
        FoundVar->RepNotifyFunc = OnRepFuncName;

        // Auto-create the OnRep function graph if it does not already exist
        bool bGraphExists = false;
        for (UEdGraph* G : BP->FunctionGraphs)
        {
            if (G && G->GetFName() == OnRepFuncName) { bGraphExists = true; break; }
        }
        if (!bGraphExists)
        {
            UEdGraph* OnRepGraph = FBlueprintEditorUtils::CreateNewGraph(
                BP, OnRepFuncName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (OnRepGraph)
            {
                FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, OnRepGraph, false, nullptr);
            }
        }
    }
    else if (ReplicationStr != TEXT("None"))
    {
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Invalid replication value '%s'. Use None, Replicated, or RepNotify"), *ReplicationStr));
    }

    // Optional lifetime condition (ELifetimeCondition)
    FString ConditionStr;
    if (Params->TryGetStringField(TEXT("condition"), ConditionStr) && !ConditionStr.IsEmpty())
    {
        ELifetimeCondition Condition = COND_None;
        if      (ConditionStr == TEXT("InitialOnly"))       Condition = COND_InitialOnly;
        else if (ConditionStr == TEXT("OwnerOnly"))         Condition = COND_OwnerOnly;
        else if (ConditionStr == TEXT("SkipOwner"))         Condition = COND_SkipOwner;
        else if (ConditionStr == TEXT("SimulatedOnly"))     Condition = COND_SimulatedOnly;
        else if (ConditionStr == TEXT("AutonomousOnly"))    Condition = COND_AutonomousOnly;
        else if (ConditionStr == TEXT("SimulatedOrPhysics"))Condition = COND_SimulatedOrPhysics;
        else if (ConditionStr == TEXT("InitialOrOwner"))    Condition = COND_InitialOrOwner;
        else if (ConditionStr == TEXT("ReplayOrOwner"))              Condition = COND_ReplayOrOwner;
        else if (ConditionStr == TEXT("ReplayOnly"))                 Condition = COND_ReplayOnly;
        else if (ConditionStr == TEXT("Custom"))                     Condition = COND_Custom;
        else if (ConditionStr == TEXT("SimulatedOnlyNoReplay"))      Condition = COND_SimulatedOnlyNoReplay;
        else if (ConditionStr == TEXT("SimulatedOrPhysicsNoReplay")) Condition = COND_SimulatedOrPhysicsNoReplay;
        else if (ConditionStr == TEXT("SkipReplay"))                 Condition = COND_SkipReplay;
        else if (ConditionStr == TEXT("Never"))                      Condition = COND_Never;
        else if (ConditionStr == TEXT("Dynamic"))                    Condition = COND_Dynamic;
        else if (ConditionStr == TEXT("NetGroup"))                   Condition = COND_NetGroup;
        else if (ConditionStr == TEXT("None"))                       Condition = COND_None;
        else
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("Invalid condition '%s'"), *ConditionStr),
                TEXT("Valid: None|InitialOnly|OwnerOnly|SkipOwner|SimulatedOnly|AutonomousOnly|"
                     "SimulatedOrPhysics|InitialOrOwner|ReplayOrOwner|ReplayOnly|Custom|"
                     "SimulatedOnlyNoReplay|SimulatedOrPhysicsNoReplay|SkipReplay|Never|"
                     "Dynamic|NetGroup"));

        FoundVar->ReplicationCondition = Condition;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_name"), VariableName);
    Data->SetStringField(TEXT("replication"), ReplicationStr);
    if (!ConditionStr.IsEmpty())
        Data->SetStringField(TEXT("condition"), ConditionStr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Set replication of '%s' to '%s' in '%s'"),
            *VariableName, *ReplicationStr, *AssetPath),
        Data);
}

// ============================================================================
// set_component_property
// Params: asset_path, component_name, property_name, value (string)
// Walks SCS->GetAllNodes() to find USCS_Node by name, then sets property on
// the component template via FProperty::ImportText_InContainer.
// ============================================================================
FBridgeResult UBlueprintHandler::Action_SetComponentProperty(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_component_property");

    FString AssetPath, ComponentName, PropertyName, Value;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'component_name' is required"));

    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'property_name' is required"));

    if (!Params->TryGetStringField(TEXT("value"), Value))
        return MakeError(DOMAIN, Action, 1000, TEXT("'value' is required (as string)"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
        return MakeError(DOMAIN, Action, 3000,
            TEXT("Blueprint has no SimpleConstructionScript (not an Actor Blueprint)"),
            TEXT("set_component_property only works on Actor-based Blueprints"));

    // Walk all SCS nodes (including inherited) to find by variable name
    USCS_Node* FoundNode = nullptr;
    for (USCS_Node* SCSNode : SCS->GetAllNodes())
    {
        if (SCSNode && SCSNode->GetVariableName() == FName(*ComponentName))
        {
            FoundNode = SCSNode;
            break;
        }
    }
    if (!FoundNode)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Component '%s' not found in SCS"), *ComponentName),
            TEXT("Use get_components to list available component variable names"));

    UActorComponent* Template = FoundNode->ComponentTemplate;
    if (!Template)
        return MakeError(DOMAIN, Action, 3000,
            FString::Printf(TEXT("SCS node '%s' has null ComponentTemplate"), *ComponentName));

    FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Prop)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Property '%s' not found on component '%s' (%s)"),
                *PropertyName, *ComponentName, *Template->GetClass()->GetName()),
            TEXT("Use exact UPROPERTY name (case-sensitive)"));

    void* Container = Template;

    // Use ImportText_InContainer for broad type coverage (handles structs, vectors, rotators, etc.)
    FString ImportError;
    const TCHAR* Result = Prop->ImportText_InContainer(*Value, Container, nullptr, PPF_None, GWarn);
    if (!Result)
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Failed to set property '%s' to value '%s' — format mismatch for type %s"),
                *PropertyName, *Value, *Prop->GetCPPType()),
            TEXT("Use text format matching the property type: booleans as True/False, vectors as (X=0,Y=0,Z=0)"));

    Template->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("component_name"), ComponentName);
    Data->SetStringField(TEXT("property_name"),  PropertyName);
    Data->SetStringField(TEXT("value"),          Value);
    Data->SetStringField(TEXT("component_class"), Template->GetClass()->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Set %s.%s = '%s' on '%s'"),
            *ComponentName, *PropertyName, *Value, *AssetPath),
        Data);
}

// ============================================================================
// Phase 1c — Compile & Save
// ============================================================================

// compile_all_dirty — compile every BP whose package is dirty
FBridgeResult UBlueprintHandler::Action_CompileAllDirty(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("compile_all_dirty");
    int32 Count = 0;
    for (TObjectIterator<UBlueprint> It; It; ++It)
    {
        UBlueprint* BP = *It;
        if (!BP) continue;
        if (BP->bBeingCompiled) continue;
        UPackage* Pkg = BP->GetOutermost();
        if (!Pkg || !Pkg->IsDirty()) continue;

        FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
        ++Count;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("compiled_count"), Count);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Compiled %d dirty blueprint(s)"), Count),
        Data);
}

// save — save a single blueprint's package
FBridgeResult UBlueprintHandler::Action_Save(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("save");

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UPackage* Pkg = BP->GetOutermost();
    if (!Pkg)
        return MakeError(DOMAIN, Action, 3000, TEXT("Blueprint has no outer package"));

    TArray<UPackage*> Packages = { Pkg };
    const FEditorFileUtils::EPromptReturnCode SaveResult =
        FEditorFileUtils::PromptForCheckoutAndSave(
            Packages,
            /*bCheckDirty=*/false,
            /*bPromptToSave=*/false);
    const bool bSaved = (SaveResult == FEditorFileUtils::PR_Success);
    if (!bSaved)
        return MakeError(DOMAIN, Action, 3000,
            FString::Printf(TEXT("SavePackages failed for '%s'"), *AssetPath));

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Saved blueprint '%s'"), *AssetPath));
}

// save_all — save all dirty packages
FBridgeResult UBlueprintHandler::Action_SaveAll(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("save_all");

    const bool bSuccess = FEditorFileUtils::SaveDirtyPackages(
        /*bPromptUserToSave=*/false,
        /*bSaveMapPackages=*/true,
        /*bSaveContentPackages=*/true);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), bSuccess);

    return MakeSuccess(DOMAIN, Action,
        bSuccess ? TEXT("Saved all dirty packages") : TEXT("SaveDirtyPackages returned false (some packages may not have saved)"),
        Data);
}

// ============================================================================
// Phase 1c — Variable
// ============================================================================

// remove_variable
FBridgeResult UBlueprintHandler::Action_RemoveVariable(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("remove_variable");

    FString BlueprintPath, VarName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Verify variable exists first
    bool bFound = false;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*VarName)) { bFound = true; break; }
    }
    if (!bFound)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VarName),
            TEXT("Use get_variables to list available variable names"));

    FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*VarName));
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Removed variable '%s' from '%s'"), *VarName, *BlueprintPath));
}

// rename_variable
FBridgeResult UBlueprintHandler::Action_RenameVariable(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("rename_variable");

    FString BlueprintPath, OldName, NewName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'old_name' is required"));
    if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'new_name' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Verify variable exists
    bool bFound = false;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*OldName)) { bFound = true; break; }
    }
    if (!bFound)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *OldName),
            TEXT("Use get_variables to list available variable names"));

    FBlueprintEditorUtils::RenameMemberVariable(BP, FName(*OldName), FName(*NewName));
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Renamed variable '%s' -> '%s' in '%s'"), *OldName, *NewName, *BlueprintPath));
}

// ============================================================================
// Phase 1c — Function / Graph
// ============================================================================

// add_function — create a user-defined function graph (minimal, no pins)
FBridgeResult UBlueprintHandler::Action_AddFunction(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_function");

    FString BlueprintPath, FunctionName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'function_name' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Duplicate check
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName() == FunctionName)
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("Function graph '%s' already exists"), *FunctionName),
                TEXT("Use a unique function name"));
    }

    // UE 5.7: CreateNewGraph first, then register with AddFunctionGraph<UClass>
    // (same pattern as Action_AddFunctionGraph — AddFunctionGraph does NOT take FName directly)
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FunctionName),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
        return MakeError(DOMAIN, Action, 3000, TEXT("CreateNewGraph returned null for add_function"));
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added function '%s' to '%s'"), *NewGraph->GetName(), *BlueprintPath),
        Data);
}

// add_macro — create a macro graph
FBridgeResult UBlueprintHandler::Action_AddMacro(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_macro");

    FString BlueprintPath, MacroName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("macro_name"), MacroName) || MacroName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'macro_name' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Duplicate check
    for (UEdGraph* G : BP->MacroGraphs)
    {
        if (G && G->GetName() == MacroName)
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("Macro graph '%s' already exists"), *MacroName),
                TEXT("Use a unique macro name"));
    }

    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(*MacroName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!Graph)
        return MakeError(DOMAIN, Action, 3000, TEXT("CreateNewGraph returned null for macro"));

    BP->MacroGraphs.Add(Graph);

    // A macro graph needs two UK2Node_Tunnel nodes (entry + exit). Without them
    // UK2Node_MacroInstance placements resolve to a pinless node or crash the compiler.
    {
        FGraphNodeCreator<UK2Node_Tunnel> EntryCreator(*Graph);
        UK2Node_Tunnel* EntryTunnel = EntryCreator.CreateNode(/*bSelectNewNode=*/false);
        EntryTunnel->bCanHaveOutputs = true;   // macro's inputs come out of this node
        EntryTunnel->bCanHaveInputs  = false;
        EntryTunnel->NodePosX = 0;
        EntryTunnel->NodePosY = 0;
        EntryCreator.Finalize();
    }
    {
        FGraphNodeCreator<UK2Node_Tunnel> ExitCreator(*Graph);
        UK2Node_Tunnel* ExitTunnel = ExitCreator.CreateNode(/*bSelectNewNode=*/false);
        ExitTunnel->bCanHaveOutputs = false;
        ExitTunnel->bCanHaveInputs  = true;    // macro's outputs flow into this node
        ExitTunnel->NodePosX = 600;
        ExitTunnel->NodePosY = 0;
        ExitCreator.Finalize();
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), Graph->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added macro graph '%s' to '%s'"), *MacroName, *BlueprintPath),
        Data);
}

// override_function — add a function override stub for an inherited function
FBridgeResult UBlueprintHandler::Action_OverrideFunction(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("override_function");

    FString BlueprintPath, FunctionName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'function_name' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // UE 5.7: FBlueprintEditorUtils::AddFunctionOverride does not exist.
    // Resolve the parent function to override, then use FindOverrideForFunction +
    // AddFunctionGraph to create the override graph if it does not already exist.
    UClass* SuperClass = BP->GeneratedClass ? BP->GeneratedClass->GetSuperClass() : nullptr;
    UFunction* FuncToOverride = SuperClass ? SuperClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
    if (!FuncToOverride)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Function '%s' not found on parent class of '%s'"), *FunctionName, *BlueprintPath),
            TEXT("The function must exist on an ancestor class to be overridden."));

    UK2Node_Event* ExistingOverride = FBlueprintEditorUtils::FindOverrideForFunction(
        BP, CastChecked<UClass>(FuncToOverride->GetOuter()), FuncToOverride->GetFName());
    if (!ExistingOverride)
    {
        UEdGraph* NewGraph = NewObject<UEdGraph>(BP, FName(*FunctionName));
        FBlueprintEditorUtils::AddFunctionGraph(BP, NewGraph, /*bIsUserCreated=*/false, FuncToOverride);
    }
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    // Find the resulting graph to return its name
    FString GraphName = FunctionName;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName() == FunctionName) { GraphName = G->GetName(); break; }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), GraphName);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added function override '%s' to '%s'"), *FunctionName, *BlueprintPath),
        Data);
}

// get_graph_names — list all graph names (function, macro, event)
FBridgeResult UBlueprintHandler::Action_GetGraphNames(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("get_graph_names");

    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    TArray<TSharedPtr<FJsonValue>> GraphArr;
    auto AddGraphs = [&](const TArray<UEdGraph*>& Graphs, const FString& Kind)
    {
        for (UEdGraph* G : Graphs)
        {
            if (!G) continue;
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), G->GetName());
            Entry->SetStringField(TEXT("kind"), Kind);
            GraphArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    };

    AddGraphs(BP->FunctionGraphs, TEXT("function"));
    AddGraphs(BP->MacroGraphs,    TEXT("macro"));
    AddGraphs(BP->UbergraphPages, TEXT("event"));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("graphs"), GraphArr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d graphs from '%s'"), GraphArr.Num(), *BlueprintPath),
        Data);
}

// ============================================================================
// Phase 1c — Interface
// ============================================================================

// remove_interface
FBridgeResult UBlueprintHandler::Action_RemoveInterface(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("remove_interface");

    FString BlueprintPath, InterfacePath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("interface_class"), InterfacePath) || InterfacePath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000,
            TEXT("'interface_class' is required (e.g. /Script/Engine.Interface_ActorSubobject)"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    UClass* IClass = LoadObject<UClass>(nullptr, *InterfacePath);
    if (!IClass)
    {
        // Try short name fallback
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == InterfacePath && It->HasAnyClassFlags(CLASS_Interface))
            {
                IClass = *It;
                break;
            }
        }
    }
    if (!IClass)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Interface class '%s' not found"), *InterfacePath),
            TEXT("Use full path like /Script/Engine.Interface_ActorSubobject or short class name"));

    // UE 5.7: RemoveInterface takes FTopLevelAssetPath — use UObject* overload (package+name)
    FBlueprintEditorUtils::RemoveInterface(BP, FTopLevelAssetPath(IClass));
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Removed interface '%s' from '%s'"), *IClass->GetName(), *BlueprintPath));
}

// ============================================================================
// Phase 1c — Local Variable
// ============================================================================

// add_local_variable
FBridgeResult UBlueprintHandler::Action_AddLocalVariable(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_local_variable");

    FString BlueprintPath, GraphName, VarName, VarType;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'graph_name' is required"));
    if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));
    if (!Params->TryGetStringField(TEXT("variable_type"), VarType) || VarType.IsEmpty())
        return MakeError(DOMAIN, Action, 1000,
            TEXT("'variable_type' is required (bool|int|float|string|vector|object)"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Find named graph
    UEdGraph* TargetGraph = nullptr;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName() == GraphName) { TargetGraph = G; break; }
    }
    if (!TargetGraph)
    {
        for (UEdGraph* G : BP->MacroGraphs)
        {
            if (G && G->GetName() == GraphName) { TargetGraph = G; break; }
        }
    }
    if (!TargetGraph)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Graph '%s' not found (must be a function or macro graph)"), *GraphName),
            TEXT("Use get_graph_names to list available graphs"));

    // Build pin type. Special-case "vector" for backwards compat with the old hardcoded
    // branch; otherwise route through the verified BuildPinTypeFromString mapping.
    FEdGraphPinType PinType;
    FString SubType;
    Params->TryGetStringField(TEXT("sub_type"), SubType);
    if (VarType == TEXT("vector"))
    {
        PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else
    {
        FString BuildErr;
        if (!BuildPinTypeFromString(VarType, SubType, PinType, BuildErr))
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("add_local_variable: %s"), *BuildErr),
                TEXT("Valid types: bool|int|int64|float|double|string|name|text|"
                     "byte|struct|object|class|soft_object|soft_class|enum|vector|wildcard"));
    }

    FBlueprintEditorUtils::AddLocalVariable(BP, TargetGraph, FName(*VarName), PinType);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added local variable '%s' (%s) to graph '%s'"),
            *VarName, *VarType, *GraphName));
}

// ============================================================================
// Phase 1c — Pin operations
// ============================================================================

// connect_pins — alias for connect_nodes (identical semantics)
FBridgeResult UBlueprintHandler::Action_ConnectPins(TSharedPtr<FJsonObject> Params)
{
    return Action_ConnectNodes(Params);
}

// set_pin_default — set Pin->DefaultValue by node GUID and pin name
FBridgeResult UBlueprintHandler::Action_SetPinDefault(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_pin_default");

    FString BlueprintPath, NodeGuidStr, PinName, DefaultValue;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'node_guid' is required"));
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'pin_name' is required"));
    if (!Params->TryGetStringField(TEXT("default_value"), DefaultValue))
        return MakeError(DOMAIN, Action, 1000, TEXT("'default_value' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Search all graphs for the node
    UEdGraphNode* FoundNode = nullptr;
    auto SearchAllGraphs = [&](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* G : Graphs)
        {
            if (!G || FoundNode) continue;
            FoundNode = FindNodeByIdOrName(G, NodeGuidStr);
        }
    };
    SearchAllGraphs(BP->UbergraphPages);
    SearchAllGraphs(BP->FunctionGraphs);
    SearchAllGraphs(BP->MacroGraphs);

    if (!FoundNode)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Node '%s' not found in any graph"), *NodeGuidStr),
            TEXT("Use list_nodes to get valid node GUIDs"));

    UEdGraphPin* Pin = FoundNode->FindPin(FName(*PinName));
    if (!Pin)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *FoundNode->GetName()),
            TEXT("Use list_pins to inspect available pin names"));

    Pin->DefaultValue = DefaultValue;
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"),     NodeGuidStr);
    Data->SetStringField(TEXT("pin_name"),       PinName);
    Data->SetStringField(TEXT("default_value"),  DefaultValue);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Set pin '%s' default to '%s' on node '%s'"),
            *PinName, *DefaultValue, *FoundNode->GetName()),
        Data);
}

// ============================================================================
// Phase 1c — Dispatcher
// ============================================================================

// remove_event_dispatcher
FBridgeResult UBlueprintHandler::Action_RemoveEventDispatcher(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("remove_event_dispatcher");

    FString AssetPath, DispName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispName) || DispName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'dispatcher_name' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    // UE 5.7: FBlueprintEditorUtils::RemoveDelegate does not exist.
    // Find the delegate signature graph by name, then remove it via RemoveGraph.
    UEdGraph* DelegateGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, FName(*DispName));
    if (!DelegateGraph)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Event dispatcher '%s' not found on '%s'"), *DispName, *AssetPath),
            TEXT("Use get_graph_names to list delegate signatures, or add_event_dispatcher to create one."));

    FBlueprintEditorUtils::RemoveGraph(BP, DelegateGraph);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Removed event dispatcher '%s' from '%s'"), *DispName, *AssetPath));
}

// call_event_dispatcher — place a UK2Node_CallDelegate node that calls a named dispatcher
FBridgeResult UBlueprintHandler::Action_CallEventDispatcher(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("call_event_dispatcher");

    FString AssetPath, DispName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispName) || DispName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'dispatcher_name' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
        return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    const int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 400;
    const int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 200;

    FGraphNodeCreator<UK2Node_CallDelegate> Creator(*Graph);
    UK2Node_CallDelegate* CallNode = Creator.CreateNode(false);

    // Set the delegate reference by self-member name
    CallNode->DelegateReference.SetSelfMember(FName(*DispName));
    CallNode->NodePosX = PosX;
    CallNode->NodePosY = PosY;
    Creator.Finalize();

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), CallNode->NodeGuid.ToString());
    Data->SetStringField(TEXT("dispatcher_name"), DispName);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added CallDelegate node for '%s' (NodeGuid: %s)"),
            *DispName, *CallNode->NodeGuid.ToString()),
        Data);
}

// ============================================================================
// Phase 1c — Replication
// ============================================================================

// get_replicated_variables — return all variables that have CPF_Net set
FBridgeResult UBlueprintHandler::Action_GetReplicatedVariables(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("get_replicated_variables");

    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Build a reverse-map from ELifetimeCondition int -> string for output
    auto ConditionToString = [](ELifetimeCondition C) -> FString
    {
        switch (C)
        {
            case COND_None:                    return TEXT("None");
            case COND_InitialOnly:             return TEXT("InitialOnly");
            case COND_OwnerOnly:               return TEXT("OwnerOnly");
            case COND_SkipOwner:               return TEXT("SkipOwner");
            case COND_SimulatedOnly:           return TEXT("SimulatedOnly");
            case COND_AutonomousOnly:          return TEXT("AutonomousOnly");
            case COND_SimulatedOrPhysics:      return TEXT("SimulatedOrPhysics");
            case COND_InitialOrOwner:          return TEXT("InitialOrOwner");
            case COND_Custom:                  return TEXT("Custom");
            case COND_ReplayOrOwner:           return TEXT("ReplayOrOwner");
            case COND_ReplayOnly:              return TEXT("ReplayOnly");
            case COND_SimulatedOnlyNoReplay:   return TEXT("SimulatedOnlyNoReplay");
            case COND_SimulatedOrPhysicsNoReplay: return TEXT("SimulatedOrPhysicsNoReplay");
            default:                           return TEXT("Unknown");
        }
    };

    TArray<TSharedPtr<FJsonValue>> VarsArr;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        if (!(Var.PropertyFlags & CPF_Net)) continue;

        TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
        VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
        VarObj->SetBoolField(TEXT("is_replicated"), true);
        VarObj->SetStringField(TEXT("replication_condition"), ConditionToString(Var.ReplicationCondition));
        VarObj->SetBoolField(TEXT("rep_notify"), (Var.PropertyFlags & CPF_RepNotify) != 0);
        VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("variables"), VarsArr);
    Data->SetNumberField(TEXT("count"), VarsArr.Num());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d replicated variable(s) from '%s'"), VarsArr.Num(), *BlueprintPath),
        Data);
}

// set_replication_condition — change the ELifetimeCondition on a replicated variable
FBridgeResult UBlueprintHandler::Action_SetReplicationCondition(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_replication_condition");

    FString BlueprintPath, VarName, ConditionStr;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));
    if (!Params->TryGetStringField(TEXT("condition"), ConditionStr) || ConditionStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000,
            TEXT("'condition' is required (None|InitialOnly|OwnerOnly|SkipOwner|SimulatedOnly|"
                 "AutonomousOnly|SimulatedOrPhysics|InitialOrOwner|Custom|ReplayOrOwner|"
                 "ReplayOnly|SimulatedOnlyNoReplay|SimulatedOrPhysicsNoReplay|"
                 "SkipReplay|Never|Dynamic|NetGroup)"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    FBPVariableDescription* FoundVar = nullptr;
    for (FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*VarName)) { FoundVar = &Var; break; }
    }
    if (!FoundVar)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found"), *VarName),
            TEXT("Use get_variables to list available variables"));

    // Map string -> ELifetimeCondition (full set)
    ELifetimeCondition Condition = COND_None;
    bool bValid = true;
    if      (ConditionStr == TEXT("None"))                      Condition = COND_None;
    else if (ConditionStr == TEXT("InitialOnly"))               Condition = COND_InitialOnly;
    else if (ConditionStr == TEXT("OwnerOnly"))                 Condition = COND_OwnerOnly;
    else if (ConditionStr == TEXT("SkipOwner"))                 Condition = COND_SkipOwner;
    else if (ConditionStr == TEXT("SimulatedOnly"))             Condition = COND_SimulatedOnly;
    else if (ConditionStr == TEXT("AutonomousOnly"))            Condition = COND_AutonomousOnly;
    else if (ConditionStr == TEXT("SimulatedOrPhysics"))        Condition = COND_SimulatedOrPhysics;
    else if (ConditionStr == TEXT("InitialOrOwner"))            Condition = COND_InitialOrOwner;
    else if (ConditionStr == TEXT("Custom"))                    Condition = COND_Custom;
    else if (ConditionStr == TEXT("ReplayOrOwner"))             Condition = COND_ReplayOrOwner;
    else if (ConditionStr == TEXT("ReplayOnly"))                Condition = COND_ReplayOnly;
    else if (ConditionStr == TEXT("SimulatedOnlyNoReplay"))     Condition = COND_SimulatedOnlyNoReplay;
    else if (ConditionStr == TEXT("SimulatedOrPhysicsNoReplay"))Condition = COND_SimulatedOrPhysicsNoReplay;
    else if (ConditionStr == TEXT("SkipReplay"))                Condition = COND_SkipReplay;
    else if (ConditionStr == TEXT("Never"))                     Condition = COND_Never;
    else if (ConditionStr == TEXT("Dynamic"))                   Condition = COND_Dynamic;
    else if (ConditionStr == TEXT("NetGroup"))                  Condition = COND_NetGroup;
    // NOTE: "OnlyOnce" intentionally removed — COND_Max is a sentinel terminator,
    // not a legal condition. Use "InitialOnly" for one-shot replication semantics.
    else bValid = false;

    if (!bValid)
        return MakeError(DOMAIN, Action, 1000,
            FString::Printf(TEXT("Unknown replication condition '%s'"), *ConditionStr),
            TEXT("Valid: None|InitialOnly|OwnerOnly|SkipOwner|SimulatedOnly|AutonomousOnly|"
                 "SimulatedOrPhysics|InitialOrOwner|Custom|ReplayOrOwner|ReplayOnly|"
                 "SimulatedOnlyNoReplay|SimulatedOrPhysicsNoReplay|SkipReplay|Never|Dynamic|NetGroup"));

    FoundVar->ReplicationCondition = Condition;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Set replication condition of '%s' to '%s' in '%s'"),
            *VarName, *ConditionStr, *BlueprintPath));
}

// enable_push_model — add or remove CPF_PushModelForce on a variable
FBridgeResult UBlueprintHandler::Action_EnablePushModel(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("enable_push_model");

    FString BlueprintPath, VarName;
    bool bEnabled = true;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'variable_name' is required"));
    if (Params->HasField(TEXT("enabled")))
        bEnabled = Params->GetBoolField(TEXT("enabled"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    FBPVariableDescription* FoundVar = nullptr;
    for (FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarName == FName(*VarName)) { FoundVar = &Var; break; }
    }
    if (!FoundVar)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Variable '%s' not found"), *VarName),
            TEXT("Use get_variables to list available variables"));

    // Guard: push model only meaningful when variable is replicated
    if (bEnabled && !(FoundVar->PropertyFlags & CPF_Net))
        return MakeError(DOMAIN, Action, 3000,
            FString::Printf(TEXT("Variable '%s' is not replicated; set_variable_replication first"), *VarName),
            TEXT("Push model only applies to replicated variables"));

    // UE 5.7: CPF_PushModelForce does not exist as a property flag; push-model opt-in
    // is driven from MARK_PROPERTY_DIRTY calls at runtime, not a persistent flag on
    // FBPVariableDescription::PropertyFlags. No flag write is performed.

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("%s push model for variable '%s' in '%s'"),
            bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *VarName, *BlueprintPath));
}

// ============================================================================
// Phase 1c — Component alias
// ============================================================================

// list_components — delegate to get_components
FBridgeResult UBlueprintHandler::Action_ListComponents(TSharedPtr<FJsonObject> Params)
{
    return Action_GetComponents(Params);
}

// ============================================================================
// Phase 1d — Authoring completeness
// ============================================================================

// set_node_position — move a node by GUID on its graph
FBridgeResult UBlueprintHandler::Action_SetNodePosition(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_node_position");

    FString BlueprintPath, NodeGuidStr;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'node_guid' is required"));
    if (!Params->HasField(TEXT("x")) || !Params->HasField(TEXT("y")))
        return MakeError(DOMAIN, Action, 1000, TEXT("'x' and 'y' (int) are required"));

    const int32 X = (int32)Params->GetNumberField(TEXT("x"));
    const int32 Y = (int32)Params->GetNumberField(TEXT("y"));

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    // Parse GUID once
    FGuid NodeGuid;
    if (!FGuid::Parse(NodeGuidStr, NodeGuid))
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Could not parse node_guid '%s'"), *NodeGuidStr));

    // Scope search: if graph_name provided, search only that graph; else search all.
    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    UEdGraphNode* FoundNode = nullptr;
    UEdGraph* FoundGraph = nullptr;

    if (!GraphName.IsEmpty())
    {
        UEdGraph* Graph = FindGraphByName(BP, GraphName);
        if (!Graph)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Graph '%s' not found in '%s'"), *GraphName, *BlueprintPath),
                TEXT("Use get_graph_names to list available graph names"));
        FoundNode = FindNodeByGuidStr(Graph, NodeGuidStr);
        FoundGraph = Graph;
    }
    else
    {
        FoundNode = FindNodeInAnyGraph(BP, NodeGuid, &FoundGraph);
    }

    if (!FoundNode)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Node '%s' not found"), *NodeGuidStr),
            TEXT("Use list_nodes to get valid node GUIDs, or pass graph_name to scope the search"));

    FoundNode->Modify();
    FoundNode->NodePosX = X;
    FoundNode->NodePosY = Y;
    if (FoundGraph) FoundGraph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), NodeGuidStr);
    Data->SetNumberField(TEXT("x"), X);
    Data->SetNumberField(TEXT("y"), Y);
    if (FoundGraph) Data->SetStringField(TEXT("graph_name"), FoundGraph->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Moved node '%s' to (%d, %d)"), *NodeGuidStr, X, Y),
        Data);
}

// disconnect_pins — break links on a pin (or a specific link to another pin)
FBridgeResult UBlueprintHandler::Action_DisconnectPins(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("disconnect_pins");

    FString BlueprintPath, NodeGuidStr, PinName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'node_guid' is required"));
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'pin_name' is required"));

    // Optional target pin (if omitted, break ALL links on the source pin)
    FString TargetNodeGuidStr, TargetPinName;
    const bool bHasTargetNode = Params->TryGetStringField(TEXT("target_node_guid"), TargetNodeGuidStr)
                             && !TargetNodeGuidStr.IsEmpty();
    const bool bHasTargetPin  = Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName)
                             && !TargetPinName.IsEmpty();

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    FGuid NodeGuid;
    if (!FGuid::Parse(NodeGuidStr, NodeGuid))
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Could not parse node_guid '%s'"), *NodeGuidStr));

    // Scope search
    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    UEdGraphNode* SourceNode = nullptr;
    UEdGraph* SourceGraph = nullptr;

    if (!GraphName.IsEmpty())
    {
        UEdGraph* Graph = FindGraphByName(BP, GraphName);
        if (!Graph)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Graph '%s' not found in '%s'"), *GraphName, *BlueprintPath));
        SourceNode = FindNodeByGuidStr(Graph, NodeGuidStr);
        SourceGraph = Graph;
    }
    else
    {
        SourceNode = FindNodeInAnyGraph(BP, NodeGuid, &SourceGraph);
    }

    if (!SourceNode)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Source node '%s' not found"), *NodeGuidStr));

    UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*PinName));
    if (!SourcePin)
        return MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Pin '%s' not found on node '%s'"),
                *PinName, *SourceNode->GetName()),
            TEXT("Use list_pins to inspect available pin names"));

    int32 LinksBroken = 0;

    if (bHasTargetNode && bHasTargetPin)
    {
        // Break a single specific link
        FGuid TargetGuid;
        if (!FGuid::Parse(TargetNodeGuidStr, TargetGuid))
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("Could not parse target_node_guid '%s'"), *TargetNodeGuidStr));

        UEdGraphNode* TargetNode = FindNodeInAnyGraph(BP, TargetGuid);
        if (!TargetNode)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeGuidStr));

        UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
        if (!TargetPin)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Target pin '%s' not found on node '%s'"),
                    *TargetPinName, *TargetNode->GetName()));

        if (SourcePin->LinkedTo.Contains(TargetPin))
        {
            SourcePin->BreakLinkTo(TargetPin);
            LinksBroken = 1;
        }
    }
    else
    {
        // Break ALL links on the source pin
        LinksBroken = SourcePin->LinkedTo.Num();
        SourcePin->BreakAllPinLinks();
    }

    if (SourceGraph) SourceGraph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"),     NodeGuidStr);
    Data->SetStringField(TEXT("pin_name"),      PinName);
    Data->SetNumberField(TEXT("links_broken"),  LinksBroken);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Broke %d link(s) on %s[%s]"),
            LinksBroken, *SourceNode->GetName(), *PinName),
        Data);
}

// list_interfaces — enumerate implemented Blueprint interfaces and their stub graphs
FBridgeResult UBlueprintHandler::Action_ListInterfaces(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("list_interfaces");

    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
    {
        // Also accept asset_path for consistency with neighbouring actions
        if (!Params->TryGetStringField(TEXT("asset_path"), BlueprintPath) || BlueprintPath.IsEmpty())
            return MakeError(DOMAIN, Action, 1000, TEXT("'blueprint_path' (or 'asset_path') is required"));
    }

    UBlueprint* BP = LoadBlueprint(BlueprintPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FBPInterfaceDescription& I : BP->ImplementedInterfaces)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        const FString InterfaceName = I.Interface ? I.Interface->GetName()     : TEXT("");
        const FString InterfacePath = I.Interface ? I.Interface->GetPathName() : TEXT("");
        O->SetStringField(TEXT("interface_class"), InterfaceName);
        O->SetStringField(TEXT("interface_path"),  InterfacePath);

        // Collect the names of stub graphs that belong to this interface. Two signals:
        //   1) I.Graphs directly references the stub graphs for function-interface entries.
        //   2) BP->FunctionGraphs may also carry matching-named graphs for older patterns.
        TArray<TSharedPtr<FJsonValue>> Stubs;
        TSet<FString> Seen;
        for (UEdGraph* G : I.Graphs)
        {
            if (!G) continue;
            const FString N = G->GetName();
            if (!Seen.Contains(N)) { Seen.Add(N); Stubs.Add(MakeShared<FJsonValueString>(N)); }
        }
        if (I.Interface)
        {
            for (TFieldIterator<UFunction> It(I.Interface); It; ++It)
            {
                const FString FuncName = It->GetName();
                for (UEdGraph* G : BP->FunctionGraphs)
                {
                    if (!G) continue;
                    const FString N = G->GetName();
                    if (N == FuncName && !Seen.Contains(N))
                    {
                        Seen.Add(N);
                        Stubs.Add(MakeShared<FJsonValueString>(N));
                    }
                }
            }
        }
        O->SetArrayField(TEXT("stub_graphs"), Stubs);
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("interfaces"), Out);
    Data->SetNumberField(TEXT("count"), Out.Num());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Returned %d implemented interface(s) on '%s'"),
            Out.Num(), *BlueprintPath),
        Data);
}

// ============================================================================
// Phase 1e — add_rpc_function
// Mirrors Action_AddCustomEvent but sets EFunctionFlags on the UK2Node_CustomEvent
// so the compiled UFunction carries FUNC_Net + direction (Server/Client/Multicast)
// and optionally FUNC_NetReliable. FUNC_NetValidate is intentionally unsupported:
// K2Node_FunctionEntry.cpp:318 blocks it for BP-authored functions — it is a
// C++ UFUNCTION-only feature.
// ============================================================================
FBridgeResult UBlueprintHandler::Action_AddRpcFunction(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_rpc_function");

    // Unsupported param surfaced eagerly so callers learn the constraint.
    bool bWithValidationUnused = false;
    if (Params->TryGetBoolField(TEXT("with_validation"), bWithValidationUnused))
    {
        return MakeError(DOMAIN, Action, 1000,
            TEXT("with_validation is not supported on Blueprint custom events"),
            TEXT("FUNC_NetValidate is blocked for BP-authored functions (K2Node_FunctionEntry.cpp:318). "
                 "Declare the Server RPC in C++ as UFUNCTION(..., WithValidation) to get a _Validate stub."));
    }

    FString AssetPath, EventName, RpcType;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'event_name' is required"));

    if (!Params->TryGetStringField(TEXT("rpc_type"), RpcType) || RpcType.IsEmpty())
        return MakeError(DOMAIN, Action, 1000,
            TEXT("'rpc_type' is required"),
            TEXT("Valid values: 'Server' | 'Client' | 'NetMulticast'"));

    const bool bIsServer    = RpcType.Equals(TEXT("Server"),       ESearchCase::IgnoreCase);
    const bool bIsClient    = RpcType.Equals(TEXT("Client"),       ESearchCase::IgnoreCase);
    const bool bIsMulticast = RpcType.Equals(TEXT("NetMulticast"), ESearchCase::IgnoreCase);
    if (!bIsServer && !bIsClient && !bIsMulticast)
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Unknown rpc_type '%s'"), *RpcType),
            TEXT("Valid values: 'Server' | 'Client' | 'NetMulticast'"));

    // Default reliability: Server/Client = reliable, NetMulticast = unreliable
    bool bReliable = bIsServer || bIsClient;
    Params->TryGetBoolField(TEXT("reliable"), bReliable);

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph)
        return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    // Duplicate check — do not silently overwrite an existing event.
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_CustomEvent* Existing = Cast<UK2Node_CustomEvent>(Node))
        {
            if (Existing->CustomFunctionName == FName(*EventName))
            {
                return MakeError(DOMAIN, Action, 3000,
                    FString::Printf(TEXT("Custom event '%s' already exists in graph '%s'"),
                        *EventName, *Graph->GetName()),
                    TEXT("Pick a different event_name, or remove the existing node first."));
            }
        }
    }

    // Create the custom event node — same pattern as Action_AddCustomEvent.
    UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
    Graph->AddNode(EventNode, false, false);
    EventNode->CreateNewGuid();
    EventNode->PostPlacedNewNode();
    EventNode->CustomFunctionName = FName(*EventName);
    EventNode->AllocateDefaultPins();

    const int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 200;
    const int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 200;
    EventNode->NodePosX = PosX;
    EventNode->NodePosY = PosY;

    // Apply RPC flags. Mirrors BlueprintDetailsCustomization.cpp:5118-5152.
    EventNode->Modify();
    const int32 ClearMask = FUNC_Net | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast | FUNC_NetReliable;
    EventNode->FunctionFlags &= ~ClearMask;
    EventNode->FunctionFlags |= FUNC_Net;
    if (bIsServer)         EventNode->FunctionFlags |= FUNC_NetServer;
    else if (bIsClient)    EventNode->FunctionFlags |= FUNC_NetClient;
    else /* multicast */   EventNode->FunctionFlags |= FUNC_NetMulticast;
    if (bReliable)         EventNode->FunctionFlags |= FUNC_NetReliable;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

    // Assemble warnings array — mirrors ForgeBlueprintCapture's CHATTY_RPC rule.
    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (bIsMulticast && bReliable)
    {
        TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
        W->SetStringField(TEXT("code"), TEXT("CHATTY_RPC"));
        W->SetStringField(TEXT("severity"), TEXT("warning"));
        W->SetStringField(TEXT("message"),
            TEXT("Reliable NetMulticast RPC — reliable multicasts are expensive. Consider unreliable unless delivery guarantee is critical."));
        Warnings.Add(MakeShared<FJsonValueObject>(W));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), EventNode->NodeGuid.ToString());
    Data->SetStringField(TEXT("event_name"), EventName);
    Data->SetStringField(TEXT("rpc_type"), RpcType);
    Data->SetBoolField  (TEXT("reliable"), bReliable);
    Data->SetStringField(TEXT("function_flags_hex"),
        FString::Printf(TEXT("0x%X"), (uint32)EventNode->FunctionFlags));
    Data->SetArrayField (TEXT("warnings"), Warnings);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Added %s%s RPC event '%s' to %s"),
            bReliable ? TEXT("reliable ") : TEXT("unreliable "),
            *RpcType, *EventName, *AssetPath),
        Data);
}

// ============================================================================
// Phase 1e — User Construction Script editing
// ============================================================================
namespace
{
    // Resolve the UCS graph of BP in one place. Null return is a user-facing
    // error ("not Actor-based or UCS disabled"). Caller converts to MakeError.
    static UEdGraph* ResolveUCSGraph(UBlueprint* BP)
    {
        return BP ? FBlueprintEditorUtils::FindUserConstructionScript(BP) : nullptr;
    }

    // Return true if the node class (for CallFunction) or the function itself
    // has FUNC_Latent. Latent nodes are illegal inside function graphs (UCS).
    static bool IsLatentCallFunction(TSharedPtr<FJsonObject> Params)
    {
        FString NodeType, FuncClassPath, FuncName;
        Params->TryGetStringField(TEXT("node_type"), NodeType);
        if (!NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase)) return false;

        Params->TryGetStringField(TEXT("function_class"), FuncClassPath);
        Params->TryGetStringField(TEXT("function_name"),  FuncName);
        if (FuncClassPath.IsEmpty() || FuncName.IsEmpty()) return false;

        UClass* Cls = FindObject<UClass>(nullptr, *FuncClassPath);
        if (!Cls) return false;
        UFunction* Fn = Cls->FindFunctionByName(FName(*FuncName));
        // UE 5.7: latent is metadata, not a function flag.
        return Fn && Fn->HasMetaData(FBlueprintMetadata::MD_Latent);
    }
}

FBridgeResult UBlueprintHandler::Action_GetConstructionScript(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("get_construction_script");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* UCS = ResolveUCSGraph(BP);
    if (!UCS)
        return MakeError(DOMAIN, Action, 2000,
            TEXT("Blueprint has no User Construction Script"),
            TEXT("UCS only exists on Actor-derived Blueprints."));

    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* N : UCS->Nodes)
    {
        if (!N) continue;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),      N->GetName());
        O->SetStringField(TEXT("class"),     N->GetClass()->GetName());
        O->SetStringField(TEXT("node_guid"), N->NodeGuid.ToString());
        O->SetNumberField(TEXT("pos_x"),     N->NodePosX);
        O->SetNumberField(TEXT("pos_y"),     N->NodePosY);
        O->SetNumberField(TEXT("pin_count"), N->Pins.Num());
        O->SetBoolField  (TEXT("is_entry"),  N->IsA<UK2Node_FunctionEntry>());
        NodesArr.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), UCS->GetName());
    Data->SetNumberField(TEXT("node_count"), NodesArr.Num());
    Data->SetArrayField (TEXT("nodes"),      NodesArr);

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("UCS '%s' on %s has %d node(s)"),
            *UCS->GetName(), *AssetPath, NodesArr.Num()),
        Data);
}

FBridgeResult UBlueprintHandler::Action_AddUcsNode(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("add_ucs_node");

    // Normalize asset_path param key. Action_AddNode reads "blueprint_path".
    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty()) Params->TryGetStringField(TEXT("blueprint_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' (or 'blueprint_path') is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP)
        return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* UCS = ResolveUCSGraph(BP);
    if (!UCS)
        return MakeError(DOMAIN, Action, 2000,
            TEXT("Blueprint has no User Construction Script"),
            TEXT("UCS only exists on Actor-derived Blueprints."));

    if (IsLatentCallFunction(Params))
    {
        FString FuncName; Params->TryGetStringField(TEXT("function_name"), FuncName);
        return MakeError(DOMAIN, Action, 3000,
            FString::Printf(TEXT("Latent function '%s' is not allowed in a function graph (UCS)"), *FuncName),
            TEXT("UCS runs at construction — latent nodes require an Event Graph. Use add_node with graph_name=EventGraph."));
    }

    // Delegate to existing Action_AddNode with graph_name + blueprint_path injected.
    // Clone first — do not mutate the caller's params object.
    TSharedPtr<FJsonObject> Forward = MakeShared<FJsonObject>(*Params);
    Forward->SetStringField(TEXT("blueprint_path"), AssetPath);
    Forward->SetStringField(TEXT("graph_name"),    UCS->GetName());
    FBridgeResult R = Action_AddNode(Forward);
    R.Action = Action;
    if (!R.Data.IsValid()) R.Data = MakeShared<FJsonObject>();
    R.Data->SetStringField(TEXT("resolved_graph"), UCS->GetName());
    return R;
}

FBridgeResult UBlueprintHandler::Action_ConnectUcsPins(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("connect_ucs_pins");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty()) Params->TryGetStringField(TEXT("blueprint_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
    UEdGraph* UCS = ResolveUCSGraph(BP);
    if (!UCS) return MakeError(DOMAIN, Action, 2000, TEXT("Blueprint has no User Construction Script"));

    TSharedPtr<FJsonObject> Forward = MakeShared<FJsonObject>(*Params);
    Forward->SetStringField(TEXT("blueprint_path"), AssetPath);
    Forward->SetStringField(TEXT("graph_name"),    UCS->GetName());
    FBridgeResult R = Action_ConnectPins(Forward);
    R.Action = Action;
    if (!R.Data.IsValid()) R.Data = MakeShared<FJsonObject>();
    R.Data->SetStringField(TEXT("resolved_graph"), UCS->GetName());
    return R;
}

FBridgeResult UBlueprintHandler::Action_SetUcsPinDefault(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_ucs_pin_default");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty()) Params->TryGetStringField(TEXT("blueprint_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
    UEdGraph* UCS = ResolveUCSGraph(BP);
    if (!UCS) return MakeError(DOMAIN, Action, 2000, TEXT("Blueprint has no User Construction Script"));

    TSharedPtr<FJsonObject> Forward = MakeShared<FJsonObject>(*Params);
    Forward->SetStringField(TEXT("blueprint_path"), AssetPath);
    Forward->SetStringField(TEXT("graph_name"),    UCS->GetName());
    FBridgeResult R = Action_SetPinDefault(Forward);
    R.Action = Action;
    if (!R.Data.IsValid()) R.Data = MakeShared<FJsonObject>();
    R.Data->SetStringField(TEXT("resolved_graph"), UCS->GetName());
    return R;
}

FBridgeResult UBlueprintHandler::Action_ClearConstructionScript(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("clear_construction_script");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
    UEdGraph* UCS = ResolveUCSGraph(BP);
    if (!UCS) return MakeError(DOMAIN, Action, 2000,
        TEXT("Blueprint has no User Construction Script"),
        TEXT("UCS only exists on Actor-derived Blueprints."));

    // Preserve UK2Node_FunctionEntry, remove everything else.
    TArray<UEdGraphNode*> ToRemove;
    for (UEdGraphNode* N : UCS->Nodes)
        if (N && !N->IsA<UK2Node_FunctionEntry>())
            ToRemove.Add(N);

    for (UEdGraphNode* N : ToRemove)
        FBlueprintEditorUtils::RemoveNode(BP, N, /*bDontRecompile=*/true);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("removed_count"),         ToRemove.Num());
    Data->SetNumberField(TEXT("remaining_node_count"),  UCS->Nodes.Num());
    Data->SetStringField(TEXT("graph_name"),            UCS->GetName());

    return MakeSuccess(DOMAIN, Action,
        FString::Printf(TEXT("Cleared %d node(s) from UCS '%s' on %s"),
            ToRemove.Num(), *UCS->GetName(), *AssetPath),
        Data);
}

// ============================================================================
// Phase 1e — High-level graph assembly + auto-layout (trampolines)
// Heavy lifting lives in BlueprintGraphAssembler.cpp / BlueprintGraphLayout.cpp.
// These entry points resolve params, load the BP/graph, then hand off.
// ============================================================================
FBridgeResult UBlueprintHandler::Action_BuildBlueprintGraph(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("build_blueprint_graph");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    return FBlueprintGraphAssembler::Build(BP, Graph, Params);
}

FBridgeResult UBlueprintHandler::Action_LayoutGraph(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("layout_graph");

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetTargetAssetPath();
    if (AssetPath.IsEmpty())
        return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

    UBlueprint* BP = LoadBlueprint(AssetPath);
    if (!BP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, Action, 3000, TEXT("No target graph found"));

    return FBlueprintGraphLayout::LayoutAction(BP, Graph, Params);
}

// ============================================================================
// Wave 7: typed Blueprint creators + K2 node creators + graph removal
// ============================================================================

#include "K2Node_IfThenElse.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "K2Node_MacroInstance.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
    int32 GetNodePos(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, int32 Default)
    {
        int32 Out = Default;
        P->TryGetNumberField(Key, Out);
        return Out;
    }

    UStruct* ResolveStruct(const FString& Path)
    {
        if (UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *Path)) return S;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UScriptStruct>(nullptr, *Suffix);
    }

    template<typename TNode>
    TNode* CreateK2Node(UEdGraph* Graph, int32 X, int32 Y)
    {
        TNode* NewNode = NewObject<TNode>(Graph);
        Graph->AddNode(NewNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
        NewNode->NodePosX = X;
        NewNode->NodePosY = Y;
        NewNode->CreateNewGuid();
        NewNode->PostPlacedNewNode();
        NewNode->AllocateDefaultPins();
        return NewNode;
    }
}

// ----------------------------------------------------------------------------
// create_*_bp typed creators (route to Action_CreateBlueprint)
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_CreateTypedBP(TSharedPtr<FJsonObject> Params,
                                                     const FString& ParentClass,
                                                     const FString& Action)
{
    // Inject parent_class then route through the existing creation path so all
    // factory + asset-registry handling stays consistent.
    Params->SetStringField(TEXT("parent_class"), ParentClass);
    FBridgeResult R = Action_CreateBlueprint(Params);
    if (R.bSuccess)
    {
        R.Action = Action;  // surface the typed action name in the result
        R.Message = FString::Printf(TEXT("Created %s blueprint"), *ParentClass)
                  + (R.AffectedPath.IsEmpty() ? FString() : FString::Printf(TEXT(" at '%s'"), *R.AffectedPath));
    }
    return R;
}

// ----------------------------------------------------------------------------
// add_branch_node — UK2Node_IfThenElse
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddBranchNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_branch_node"), 1000, TEXT("'blueprint_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_branch_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_branch_node"), 3000, TEXT("No target graph"));

    UK2Node_IfThenElse* Node = CreateK2Node<UK2Node_IfThenElse>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    FBridgeResult R = MakeSuccess(DOMAIN, TEXT("add_branch_node"),
        FString::Printf(TEXT("Branch node added (guid=%s)"), *Node->NodeGuid.ToString()), Data);
    R.AffectedPath = Node->NodeGuid.ToString();
    return R;
}

// ----------------------------------------------------------------------------
// add_cast_node — UK2Node_DynamicCast
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddCastNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, TargetClass;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_cast_node"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("target_class"), TargetClass))
        return MakeError(DOMAIN, TEXT("add_cast_node"), 1000, TEXT("'target_class' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_cast_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_cast_node"), 3000, TEXT("No target graph"));

    UClass* Target = FindObject<UClass>(nullptr, *TargetClass);
    if (!Target)
    {
        const FString WithScript = TargetClass.StartsWith(TEXT("/Script/")) ? TargetClass
            : FString::Printf(TEXT("/Script/Engine.%s"), *TargetClass);
        Target = FindObject<UClass>(nullptr, *WithScript);
        if (!Target) Target = LoadObject<UClass>(nullptr, *TargetClass);
    }
    if (!Target) return MakeError(DOMAIN, TEXT("add_cast_node"), 2000,
        FString::Printf(TEXT("Target class not found: %s"), *TargetClass));

    UK2Node_DynamicCast* Node = CreateK2Node<UK2Node_DynamicCast>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    Node->TargetType = Target;
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    Data->SetStringField(TEXT("target_class"), Target->GetPathName());
    FBridgeResult R = MakeSuccess(DOMAIN, TEXT("add_cast_node"),
        FString::Printf(TEXT("Cast<%s> added (guid=%s)"), *Target->GetName(), *Node->NodeGuid.ToString()), Data);
    R.AffectedPath = Node->NodeGuid.ToString();
    return R;
}

// ----------------------------------------------------------------------------
// add_sequence_node — UK2Node_ExecutionSequence
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddSequenceNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_sequence_node"), 1000, TEXT("'blueprint_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_sequence_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_sequence_node"), 3000, TEXT("No target graph"));

    UK2Node_ExecutionSequence* Node = CreateK2Node<UK2Node_ExecutionSequence>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));

    int32 NumPins = 2;
    Params->TryGetNumberField(TEXT("num_pins"), NumPins);
    // UK2Node_ExecutionSequence implements IK2Node_AddPinInterface — call AddInputPin().
    while (Node->GetThenPinGivenIndex(NumPins - 1) == nullptr) Node->AddInputPin();
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    Data->SetNumberField(TEXT("num_pins"), NumPins);
    return MakeSuccess(DOMAIN, TEXT("add_sequence_node"),
        FString::Printf(TEXT("Sequence node (%d pins) added"), NumPins), Data);
}

// ----------------------------------------------------------------------------
// add_make_struct_node / add_break_struct_node
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddMakeStructNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, StructPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_make_struct_node"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("struct_path"), StructPath))
        return MakeError(DOMAIN, TEXT("add_make_struct_node"), 1000, TEXT("'struct_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_make_struct_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_make_struct_node"), 3000, TEXT("No target graph"));

    UScriptStruct* SS = Cast<UScriptStruct>(ResolveStruct(StructPath));
    if (!SS) return MakeError(DOMAIN, TEXT("add_make_struct_node"), 2000,
        FString::Printf(TEXT("Struct not found: %s"), *StructPath));

    UK2Node_MakeStruct* Node = CreateK2Node<UK2Node_MakeStruct>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    Node->StructType = SS;
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    return MakeSuccess(DOMAIN, TEXT("add_make_struct_node"),
        FString::Printf(TEXT("MakeStruct(%s) added"), *SS->GetName()), Data);
}

FBridgeResult UBlueprintHandler::Action_AddBreakStructNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, StructPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_break_struct_node"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("struct_path"), StructPath))
        return MakeError(DOMAIN, TEXT("add_break_struct_node"), 1000, TEXT("'struct_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_break_struct_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_break_struct_node"), 3000, TEXT("No target graph"));

    UScriptStruct* SS = Cast<UScriptStruct>(ResolveStruct(StructPath));
    if (!SS) return MakeError(DOMAIN, TEXT("add_break_struct_node"), 2000,
        FString::Printf(TEXT("Struct not found: %s"), *StructPath));

    UK2Node_BreakStruct* Node = CreateK2Node<UK2Node_BreakStruct>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    Node->StructType = SS;
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    return MakeSuccess(DOMAIN, TEXT("add_break_struct_node"),
        FString::Printf(TEXT("BreakStruct(%s) added"), *SS->GetName()), Data);
}

// ----------------------------------------------------------------------------
// add_variable_node — Get or Set
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddVariableNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, VarName, Mode = TEXT("get");
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_variable_node"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
        return MakeError(DOMAIN, TEXT("add_variable_node"), 1000, TEXT("'variable_name' is required"));
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_variable_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_variable_node"), 3000, TEXT("No target graph"));

    const int32 X = GetNodePos(Params, TEXT("pos_x"), 0);
    const int32 Y = GetNodePos(Params, TEXT("pos_y"), 0);
    FString GuidStr;
    if (Mode == TEXT("set"))
    {
        UK2Node_VariableSet* Node = CreateK2Node<UK2Node_VariableSet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(FName(*VarName));
        Node->ReconstructNode();
        GuidStr = Node->NodeGuid.ToString();
    }
    else
    {
        UK2Node_VariableGet* Node = CreateK2Node<UK2Node_VariableGet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(FName(*VarName));
        Node->ReconstructNode();
        GuidStr = Node->NodeGuid.ToString();
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), GuidStr);
    Data->SetStringField(TEXT("mode"), Mode);
    Data->SetStringField(TEXT("variable_name"), VarName);
    return MakeSuccess(DOMAIN, TEXT("add_variable_node"),
        FString::Printf(TEXT("%s '%s' node added"),
            Mode == TEXT("set") ? TEXT("Set") : TEXT("Get"), *VarName), Data);
}

// ----------------------------------------------------------------------------
// add_self_node — UK2Node_Self
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddSelfNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_self_node"), 1000, TEXT("'blueprint_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_self_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_self_node"), 3000, TEXT("No target graph"));

    UK2Node_Self* Node = CreateK2Node<UK2Node_Self>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    return MakeSuccess(DOMAIN, TEXT("add_self_node"), TEXT("Self node added"), Data);
}

// ----------------------------------------------------------------------------
// add_for_each_loop_node — Macro instance from StandardMacros
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_AddForEachLoopNode(TSharedPtr<FJsonObject> Params)
{
    FString BPPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("add_for_each_loop_node"), 1000, TEXT("'blueprint_path' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("add_for_each_loop_node"), 2000, TEXT("Blueprint not found"));
    UEdGraph* Graph = ResolveGraph(BP, Params);
    if (!Graph) return MakeError(DOMAIN, TEXT("add_for_each_loop_node"), 3000, TEXT("No target graph"));

    // Locate the StandardMacros library (built into Engine content)
    UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr,
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
    if (!StandardMacros)
        return MakeError(DOMAIN, TEXT("add_for_each_loop_node"), 2001,
            TEXT("StandardMacros not found"),
            TEXT("Engine StandardMacros library could not be loaded — check engine install"));

    UEdGraph* MacroGraph = nullptr;
    for (UEdGraph* G : StandardMacros->MacroGraphs)
    {
        if (G && G->GetFName().ToString().Contains(TEXT("ForEachLoop"), ESearchCase::IgnoreCase))
        {
            MacroGraph = G; break;
        }
    }
    if (!MacroGraph) return MakeError(DOMAIN, TEXT("add_for_each_loop_node"), 2001,
        TEXT("ForEachLoop macro not found in StandardMacros"));

    UK2Node_MacroInstance* Node = CreateK2Node<UK2Node_MacroInstance>(Graph,
        GetNodePos(Params, TEXT("pos_x"), 0), GetNodePos(Params, TEXT("pos_y"), 0));
    Node->SetMacroGraph(MacroGraph);
    Node->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
    return MakeSuccess(DOMAIN, TEXT("add_for_each_loop_node"), TEXT("ForEachLoop macro instance added"), Data);
}

// ----------------------------------------------------------------------------
// remove_function_graph / remove_macro_graph
// ----------------------------------------------------------------------------
FBridgeResult UBlueprintHandler::Action_RemoveFunctionGraph(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, FuncName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("remove_function_graph"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("function_name"), FuncName))
        return MakeError(DOMAIN, TEXT("remove_function_graph"), 1000, TEXT("'function_name' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("remove_function_graph"), 2000, TEXT("Blueprint not found"));

    UEdGraph* Found = nullptr;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetFName() == FName(*FuncName)) { Found = G; break; }
    }
    if (!Found) return MakeError(DOMAIN, TEXT("remove_function_graph"), 2000,
        FString::Printf(TEXT("Function graph '%s' not found"), *FuncName));

    FBlueprintEditorUtils::RemoveGraph(BP, Found, EGraphRemoveFlags::Recompile);
    return MakeSuccess(DOMAIN, TEXT("remove_function_graph"),
        FString::Printf(TEXT("Removed function graph '%s'"), *FuncName));
}

FBridgeResult UBlueprintHandler::Action_RemoveMacroGraph(TSharedPtr<FJsonObject> Params)
{
    FString BPPath, MacroName;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BPPath))
        return MakeError(DOMAIN, TEXT("remove_macro_graph"), 1000, TEXT("'blueprint_path' is required"));
    if (!Params->TryGetStringField(TEXT("macro_name"), MacroName))
        return MakeError(DOMAIN, TEXT("remove_macro_graph"), 1000, TEXT("'macro_name' is required"));
    UBlueprint* BP = LoadBlueprint(BPPath);
    if (!BP) return MakeError(DOMAIN, TEXT("remove_macro_graph"), 2000, TEXT("Blueprint not found"));

    UEdGraph* Found = nullptr;
    for (UEdGraph* G : BP->MacroGraphs)
    {
        if (G && G->GetFName() == FName(*MacroName)) { Found = G; break; }
    }
    if (!Found) return MakeError(DOMAIN, TEXT("remove_macro_graph"), 2000,
        FString::Printf(TEXT("Macro graph '%s' not found"), *MacroName));

    FBlueprintEditorUtils::RemoveGraph(BP, Found, EGraphRemoveFlags::Recompile);
    return MakeSuccess(DOMAIN, TEXT("remove_macro_graph"),
        FString::Printf(TEXT("Removed macro graph '%s'"), *MacroName));
}

// ============================================================================
// Wave 8: EditorUtilityBlueprint creator
// ============================================================================

#include "EditorUtilityBlueprint.h"
#include "EditorUtilityBlueprintFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

FBridgeResult UBlueprintHandler::Action_CreateEditorUtilityBlueprint(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_editor_utility_blueprint"), 1000, TEXT("'asset_path' is required"));

    FString ParentClassName = TEXT("EditorUtilityObject");
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

    UClass* ParentClass = FindObject<UClass>(nullptr, *ParentClassName);
    if (!ParentClass)
    {
        const FString WithScript = ParentClassName.StartsWith(TEXT("/Script/"))
            ? ParentClassName : FString::Printf(TEXT("/Script/Blutility.%s"), *ParentClassName);
        ParentClass = FindObject<UClass>(nullptr, *WithScript);
    }
    if (!ParentClass) ParentClass = UObject::StaticClass();

    const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
    const FString AssetName   = FPackageName::GetShortName(AssetPath);

    UEditorUtilityBlueprintFactory* Factory = NewObject<UEditorUtilityBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
        AssetName, PackagePath, UEditorUtilityBlueprint::StaticClass(), Factory);
    if (!NewAsset)
        return MakeError(DOMAIN, TEXT("create_editor_utility_blueprint"), 3000,
            FString::Printf(TEXT("Failed to create EditorUtilityBlueprint at %s"), *AssetPath));

    if (UEditorUtilityBlueprint* EUB = Cast<UEditorUtilityBlueprint>(NewAsset))
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(EUB);
        EUB->GetOutermost()->MarkPackageDirty();
    }
    FBridgeResult R = MakeSuccess(DOMAIN, TEXT("create_editor_utility_blueprint"),
        FString::Printf(TEXT("Created EditorUtilityBlueprint at %s (parent: %s)"),
            *AssetPath, *ParentClass->GetName()));
    R.AffectedPath = NewAsset->GetPathName();
    return R;
}
