#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "MetasoundBuilderBase.h"
#include "MetaSoundBuilderHandler.generated.h"

class UMetaSoundBuilderBase;

/**
 * Per-builder session state. Tracks the live builder + the user-friendly
 * id->handle maps so multi-call authoring chains resolve correctly.
 */
USTRUCT()
struct FMetaSoundBuilderSession
{
    GENERATED_BODY()

    UPROPERTY() TObjectPtr<UMetaSoundBuilderBase> Builder;
    UPROPERTY() TMap<FString, FMetaSoundNodeHandle> Nodes;

    /** Graph inputs are exposed as node-OUTPUT handles to internal consumers. */
    UPROPERTY() TMap<FString, FMetaSoundBuilderNodeOutputHandle> GraphInputs;

    /** Graph outputs are exposed as node-INPUT handles to internal producers. */
    UPROPERTY() TMap<FString, FMetaSoundBuilderNodeInputHandle> GraphOutputs;

    UPROPERTY() bool bIsSource = false;
};

/**
 * MetaSoundBuilderHandler — domain "msbuilder"  (UE 5.7)
 *
 * Symmetric CRUD over MetaSound graphs via UMetaSoundBuilderSubsystem.
 * Distinct from AudioHandler's add-only MetaSound verbs — that domain remains
 * for legacy callers; new authoring should use this builder API.
 *
 * Workflow (patch):
 *   1) create_patch_builder builder_id="my_patch"
 *   2) add_graph_input  builder_id="my_patch" name="Gain"   data_type="Float"
 *   3) add_graph_output builder_id="my_patch" name="Result" data_type="Float"
 *   4) connect builder_id="my_patch" from_pin_name="Gain" to_pin_name="Result"
 *      (when from_node_id / to_node_id are empty, pin names resolve against
 *       the graph IO maps)
 *   5) save_to_asset builder_id="my_patch" asset_path="/Game/.../MyPatch"
 *
 * For source builders, CreateSourceBuilder auto-wires:
 *   _ON_PLAY (graph input — node-output handle) → trigger
 *   _ON_FINISHED (graph output — node-input handle) → trigger
 *   _AUDIO_OUT_0, _AUDIO_OUT_1 (graph outputs) → audio
 *
 * Actions:
 *   create_patch_builder, create_source_builder, destroy_builder, list_builders
 *   add_graph_input, add_graph_output, list_graph_inputs, list_graph_outputs
 *   add_node, remove_node, list_nodes
 *   connect, disconnect_input
 *   set_node_input_default, set_graph_input_default
 *   save_to_asset
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMetaSoundBuilderHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("msbuilder"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_patch_builder"), TEXT("create_source_builder"),
            TEXT("destroy_builder"), TEXT("list_builders"),
            TEXT("add_graph_input"), TEXT("add_graph_output"),
            TEXT("list_graph_inputs"), TEXT("list_graph_outputs"),
            TEXT("add_node"), TEXT("remove_node"), TEXT("list_nodes"),
            TEXT("connect"), TEXT("disconnect_input"),
            TEXT("set_node_input_default"), TEXT("set_graph_input_default"),
            TEXT("save_to_asset")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    UPROPERTY()
    TMap<FString, FMetaSoundBuilderSession> Sessions;

    FMetaSoundBuilderSession* GetSession(const FString& BuilderId, const FString& Action, FBridgeResult& OutErr);

    FBridgeResult Action_CreatePatchBuilder       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateSourceBuilder      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DestroyBuilder           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListBuilders             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddGraphInput            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddGraphOutput           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListGraphInputs          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListGraphOutputs         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddNode                  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveNode               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListNodes                (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Connect                  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DisconnectInput          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetNodeInputDefault      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetGraphInputDefault     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SaveToAsset              (TSharedPtr<FJsonObject> Params);
};
