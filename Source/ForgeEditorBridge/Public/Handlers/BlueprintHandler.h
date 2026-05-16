#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "BlueprintHandler.generated.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * BlueprintHandler — Phase 3 domain handler for "blueprint"
 *
 * Routes from DispatchCommand when Domain == "blueprint".
 *
 * Supported actions:
 *
 *   create_blueprint
 *     Required: "asset_name" (FString), "package_path" (FString, e.g. "/Game/AI")
 *     Optional: "parent_class" (FString, e.g. "Actor" — resolves /Script/Engine.<name>)
 *     Result:   AffectedPath = created asset package path
 *
 *   add_node
 *     Required: "blueprint_path" (FString), "node_type" ("CallFunction" | "Event")
 *     CallFunction: "function_name" (FString), "function_class" (FString, e.g. "/Script/Engine.KismetSystemLibrary")
 *     Event:        "event_name" (FString, e.g. "ReceiveBeginPlay")
 *     Optional: "pos_x" (int), "pos_y" (int)
 *     Result:   AffectedPath = NodeGuid string (use in connect_nodes)
 *
 *   connect_nodes
 *     Required: "blueprint_path", "source_node_guid", "source_pin",
 *               "target_node_guid", "target_pin"
 *     Result:   Message describes the connection made
 *
 *   compile
 *     Required: "blueprint_path"
 *     Result:   AffectedPath = compiled blueprint path; Message = compiler result
 *
 * Safety: All actions that could trigger "delete" are routed through QuarantineHandler
 * by DispatchCommand before reaching here — BlueprintHandler never destroys assets.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UBlueprintHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("blueprint"); }
    virtual TArray<FString> GetSupportedActions() const override { return {
        TEXT("create_blueprint"), TEXT("add_node"), TEXT("connect_nodes"), TEXT("compile"),
        TEXT("add_step"), TEXT("prepare_value"), TEXT("connect_data_to_pin"), TEXT("get_function_nodes"),
        TEXT("add_variable"), TEXT("get_variables"), TEXT("search_function_library"), TEXT("delete_node"),
        TEXT("add_component"), TEXT("remove_component"), TEXT("get_components"),
        TEXT("add_function_graph"), TEXT("add_custom_event"), TEXT("add_interface"),
        TEXT("list_nodes"), TEXT("list_pins"), TEXT("set_variable_default"), TEXT("reparent"),
        TEXT("read_blueprint_capture"), TEXT("add_event_dispatcher"), TEXT("get_event_dispatchers"),
        TEXT("bind_event"), TEXT("set_variable_replication"), TEXT("set_component_property"),
        // ---- Phase 1c additions ----
        TEXT("compile_all_dirty"), TEXT("save"), TEXT("save_all"),
        TEXT("remove_variable"), TEXT("rename_variable"),
        TEXT("add_function"), TEXT("add_macro"), TEXT("override_function"), TEXT("get_graph_names"),
        TEXT("remove_interface"),
        TEXT("add_local_variable"),
        TEXT("connect_pins"), TEXT("set_pin_default"),
        TEXT("remove_event_dispatcher"), TEXT("call_event_dispatcher"),
        TEXT("get_replicated_variables"), TEXT("set_replication_condition"), TEXT("enable_push_model"),
        TEXT("list_components"),
        // ---- Phase 1d: authoring completeness ----
        TEXT("set_node_position"), TEXT("disconnect_pins"),
        TEXT("compile_blueprint"), TEXT("list_interfaces"),
        // ---- Phase 1e: RPC + UCS + high-level graph assembly + layout ----
        TEXT("add_rpc_function"),
        TEXT("get_construction_script"), TEXT("add_ucs_node"),
        TEXT("connect_ucs_pins"), TEXT("set_ucs_pin_default"),
        TEXT("clear_construction_script"),
        TEXT("build_blueprint_graph"), TEXT("layout_graph"),
        // ---- Wave 7: typed Blueprint creators ----
        TEXT("create_function_library_bp"), TEXT("create_macro_library_bp"),
        TEXT("create_interface_bp"), TEXT("create_save_game_bp"),
        TEXT("create_game_mode_bp"), TEXT("create_game_state_bp"),
        TEXT("create_game_instance_bp"), TEXT("create_player_controller_bp"),
        TEXT("create_pawn_bp"), TEXT("create_character_bp"),
        TEXT("create_hud_bp"), TEXT("create_actor_component_bp"),
        TEXT("create_subsystem_bp"),
        // ---- Wave 7: K2 node creators (beyond CallFunction/Event) ----
        TEXT("add_branch_node"), TEXT("add_cast_node"),
        TEXT("add_sequence_node"), TEXT("add_make_struct_node"),
        TEXT("add_break_struct_node"), TEXT("add_variable_node"),
        TEXT("add_self_node"), TEXT("add_for_each_loop_node"),
        // ---- Wave 7: graph removal (CRUD-symmetry) ----
        TEXT("remove_function_graph"), TEXT("remove_macro_graph"),
        // ---- Wave 8: editor utility BP ----
        TEXT("create_editor_utility_blueprint"),
    }; }
    virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateBlueprint(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddNode        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ConnectNodes   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Compile        (TSharedPtr<FJsonObject> Params);

    // ---- Phase A extensions ---------------------------------------------------
    FBridgeResult Action_AddStep             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_PrepareValue        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ConnectDataToPin    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetFunctionNodes    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddVariable         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetVariables        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SearchFunctionLibrary(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DeleteNode          (TSharedPtr<FJsonObject> Params);

    // ---- Phase 3 extensions ---------------------------------------------------
    FBridgeResult Action_AddComponent        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveComponent     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetComponents       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddFunctionGraph    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddCustomEvent      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddInterface        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListNodes           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListPins            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetVariableDefault  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Reparent            (TSharedPtr<FJsonObject> Params);

    // ---- Capture read ---------------------------------------------------------
    FBridgeResult Action_ReadBlueprintCapture (TSharedPtr<FJsonObject> Params);

    // ---- Event dispatcher extensions ------------------------------------------
    FBridgeResult Action_AddEventDispatcher    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetEventDispatchers   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_BindEvent             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetVariableReplication(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetComponentProperty  (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Compile & Save --------------------------------------------
    FBridgeResult Action_CompileAllDirty       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Save                  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SaveAll               (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Variable --------------------------------------------------
    FBridgeResult Action_RemoveVariable        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RenameVariable        (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Function / Graph ------------------------------------------
    FBridgeResult Action_AddFunction           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddMacro              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_OverrideFunction      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetGraphNames         (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Interface -------------------------------------------------
    FBridgeResult Action_RemoveInterface       (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Local Variable --------------------------------------------
    FBridgeResult Action_AddLocalVariable      (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Pin operations --------------------------------------------
    FBridgeResult Action_ConnectPins           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetPinDefault         (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Dispatcher ------------------------------------------------
    FBridgeResult Action_RemoveEventDispatcher (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CallEventDispatcher   (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Replication -----------------------------------------------
    FBridgeResult Action_GetReplicatedVariables(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetReplicationCondition(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_EnablePushModel       (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1c: Component alias -------------------------------------------
    FBridgeResult Action_ListComponents        (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1d: Authoring completeness ------------------------------------
    FBridgeResult Action_SetNodePosition       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DisconnectPins        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListInterfaces        (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1e: RPC authoring ---------------------------------------------
    FBridgeResult Action_AddRpcFunction        (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1e: User Construction Script editing --------------------------
    FBridgeResult Action_GetConstructionScript   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddUcsNode              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ConnectUcsPins          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetUcsPinDefault        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearConstructionScript (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1e: High-level graph assembly + auto-layout -------------------
    FBridgeResult Action_BuildBlueprintGraph     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_LayoutGraph             (TSharedPtr<FJsonObject> Params);

    // ---- Wave 7: typed Blueprint creators (route to Action_CreateBlueprint) -
    FBridgeResult Action_CreateTypedBP           (TSharedPtr<FJsonObject> Params, const FString& ParentClass, const FString& Action);

    // ---- Wave 7: K2 node creators -------------------------------------------
    FBridgeResult Action_AddBranchNode           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddCastNode             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddSequenceNode         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddMakeStructNode       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddBreakStructNode      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddVariableNode         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddSelfNode             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddForEachLoopNode      (TSharedPtr<FJsonObject> Params);

    // ---- Wave 7: graph removal ----------------------------------------------
    FBridgeResult Action_RemoveFunctionGraph     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveMacroGraph        (TSharedPtr<FJsonObject> Params);

    // ---- Wave 8: EUB creator (different asset class than typed creators) ---
    FBridgeResult Action_CreateEditorUtilityBlueprint(TSharedPtr<FJsonObject> Params);

    // ---- Shared helpers -------------------------------------------------------

    /**
     * Load a UBlueprint from a package path like "/Game/AI/BP_RemoteActor".
     * Derives the object path automatically (PackagePath.AssetName).
     * Returns nullptr if the asset is not found or is not a Blueprint.
     */
    UBlueprint* LoadBlueprint(const FString& PackagePath) const;

    /** Returns the ubergraph (EventGraph) for the given Blueprint, or nullptr. */
    UEdGraph* GetEventGraph(UBlueprint* Blueprint) const;

    /**
     * Find a node in Graph by its NodeGuid.
     * GuidStr must be parseable by FGuid::Parse.
     * Returns nullptr if not found or the GUID is malformed.
     */
    UEdGraphNode* FindNodeByGuidStr(UEdGraph* Graph, const FString& GuidStr) const;

    /** Find a node by GUID string OR node class name (like UmgMCP's FindNodeByIdOrName) */
    UEdGraphNode* FindNodeByIdOrName(UEdGraph* Graph, const FString& IdOrName) const;

    /** Returns the graph named graph_name from params, or the AttentionManager target graph, or EventGraph. */
    UEdGraph* ResolveGraph(UBlueprint* BP, TSharedPtr<FJsonObject> Params) const;

    /** Creates a UK2Node_CallFunction in Graph, resolving function by name across common libraries. */
    UEdGraphNode* CreateCallFunctionNode(UEdGraph* Graph, const FString& FunctionName,
                                         const FString& ClassPath, int32 PosX, int32 PosY) const;
};
