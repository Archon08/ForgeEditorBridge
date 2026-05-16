#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "PCGGraphHandler.generated.h"

/**
 * PCGGraphHandler — domain "pcg_graph"  (v0.5.0 / UE 5.7)
 *
 * Creates and authors PCG Graph assets programmatically through the Bridge.
 *
 * Actions:
 *   create_pcg_graph       → asset_path (string)
 *                            Creates a new UPCGGraph asset at the given content path.
 *
 *   add_pcg_node           → asset_path (string), node_type (registry key),
 *                            x (int, optional), y (int, optional)
 *                            Adds a node to the graph. Returns the new node index in ExtraData.
 *                            Registry keys: SurfaceSampler, StaticMeshSpawner, DensityFilter,
 *                            AttributeNoise, TransformPoints, CopyPoints, MergePoints
 *
 *   connect_pcg_pins       → asset_path (string),
 *                            from_node (int index or "input"/"output"),
 *                            from_pin (string label, e.g. "Out"),
 *                            to_node   (int index or "input"/"output"),
 *                            to_pin    (string label, e.g. "In")
 *                            Connects two node pins via Graph->AddEdge.
 *
 *   set_pcg_node_settings  → asset_path (string), node_index (int),
 *                            property_name (string), value (string)
 *                            Reflection-based property setter on UPCGSettings (e.g.
 *                            PointsPerSquaredMeter on UPCGSurfaceSamplerSettings).
 *
 *   get_pcg_graph_topology → asset_path (string)
 *                            Serializes graph nodes and edges to JSON in ExtraData.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPCGGraphHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("pcg_graph"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_pcg_graph"), TEXT("add_pcg_node"), TEXT("connect_pcg_pins"), TEXT("set_pcg_node_settings"), TEXT("get_pcg_graph_topology"), TEXT("remove_pcg_node"), TEXT("disconnect_pcg_pins"), TEXT("get_pcg_parameters"), TEXT("set_seed"), TEXT("cleanup"), TEXT("regenerate"), TEXT("read_telemetry"), TEXT("add_attribute_override"), TEXT("get_node_list"), TEXT("set_mesh_entry"), TEXT("execute_local") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreatePCGGraph     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddPCGNode         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ConnectPCGPins     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPCGNodeSettings (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPCGGraphTopology(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemovePCGNode          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DisconnectPCGPins      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPCGParameters       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetSeed                (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Cleanup                (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Regenerate             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadTelemetry          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddAttributeOverride   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetNodeList            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMeshEntry           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExecuteLocal           (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a UPCGGraph asset from a content path (e.g. "/Game/PCG/MyGraph").
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UPCGGraph* LoadPCGGraph(const FString& AssetPath, FBridgeResult& Result);

	/**
	 * Resolve a node reference to a UPCGNode*.
	 * NodeRef may be an integer index into Graph->GetNodes(), or the
	 * special strings "input" / "output" for the graph's implicit endpoints.
	 */
	class UPCGNode* ResolveNode(class UPCGGraph* Graph, const FString& NodeRef, FString& OutError);

	/**
	 * Map a short node-type key to the corresponding UPCGSettings subclass.
	 * Uses FindFirstObject so no element headers need to be included here.
	 */
	static TSubclassOf<class UPCGSettings> ResolveNodeType(const FString& NodeType);
};
