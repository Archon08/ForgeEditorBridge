#include "Handlers/PCGGraphHandler.h"
#include "ForgeAISubsystem.h"

// ---- PCG runtime ------------------------------------------------------------
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"

// ---- PCG component (for runtime operations) ---------------------------------
#include "PCGComponent.h"

// ---- Engine (for FindObject in world) ---------------------------------------
#include "Engine/World.h"
#include "Editor.h"
#include "EngineUtils.h"

// ---- PCGEditor (factory) ----------------------------------------------------
// #include "Factories/PCGGraphFactory.h"  // UE 5.7: PCGGraphFactory removed

// ---- Asset creation ---------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UPCGGraphHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UPCGGraphHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"),     Type);
		O->SetBoolField  (TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"),     Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new UPCGGraph asset"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path, e.g. /Game/PCG/MyGraph"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_pcg_graph"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a node to a PCG graph. Returns node_index in data for use in connect_pcg_pins."));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); Pr->SetObjectField(TEXT("node_type"), P(TEXT("string"), true, TEXT("Registry key e.g. SurfaceSampler, Delaunay2D, AStar. Use system/describe for full list."))); Pr->SetObjectField(TEXT("x"), P(TEXT("int"), false, TEXT("Editor X position"))); Pr->SetObjectField(TEXT("y"), P(TEXT("int"), false, TEXT("Editor Y position"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_pcg_node"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Connect two PCG nodes by pin label"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); Pr->SetObjectField(TEXT("from_node"), P(TEXT("string"), true, TEXT("Integer index or 'input'/'output'"))); Pr->SetObjectField(TEXT("from_pin"),  P(TEXT("string"), true, TEXT("Output pin label on from_node"))); Pr->SetObjectField(TEXT("to_node"),   P(TEXT("string"), true, TEXT("Integer index or 'input'/'output'"))); Pr->SetObjectField(TEXT("to_pin"),    P(TEXT("string"), true, TEXT("Input pin label on to_node"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("connect_pcg_pins"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a property on a node's UPCGSettings via reflection"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"),    P(TEXT("string"), true, TEXT("PCG graph asset path"))); Pr->SetObjectField(TEXT("node_index"),   P(TEXT("int"),    true, TEXT("Node index from add_pcg_node or get_pcg_graph_topology"))); Pr->SetObjectField(TEXT("property_name"), P(TEXT("string"), true, TEXT("UPROPERTY name on the settings class"))); Pr->SetObjectField(TEXT("value"),         P(TEXT("string"), true, TEXT("Value as string; float/int/bool/name supported"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_pcg_node_settings"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return graph topology: all nodes (index, class, position) and their outbound edges"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_pcg_graph_topology"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Remove a node from the graph by index or name"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"),  P(TEXT("string"), true,  TEXT("PCG graph asset path"))); Pr->SetObjectField(TEXT("node_index"), P(TEXT("int"),    false, TEXT("Node index (preferred)"))); Pr->SetObjectField(TEXT("node_name"),  P(TEXT("string"), false, TEXT("Node name (fallback)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("remove_pcg_node"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Disconnect two PCG nodes"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); Pr->SetObjectField(TEXT("from_node"), P(TEXT("string"), true, TEXT("Integer index or 'input'/'output'"))); Pr->SetObjectField(TEXT("from_pin"),  P(TEXT("string"), true, TEXT("Output pin label"))); Pr->SetObjectField(TEXT("to_node"),   P(TEXT("string"), true, TEXT("Integer index or 'input'/'output'"))); Pr->SetObjectField(TEXT("to_pin"),    P(TEXT("string"), true, TEXT("Input pin label"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("disconnect_pcg_pins"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List graph-level user parameters (FInstancedPropertyBag)"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_pcg_parameters"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set seed on a PCGComponent in the world and regenerate"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label or path in the editor world"))); Pr->SetObjectField(TEXT("seed"), P(TEXT("int"), true, TEXT("New seed value"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_seed"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Cleanup a PCGComponent (remove generated instances)"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label or path in the editor world"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("cleanup"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Regenerate a PCGComponent"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label or path in the editor world"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("regenerate"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read telemetry from a PCG graph: node count, connection count, per-node property dump and pin labels. Enables AI read-back loop."));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("PCG graph asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("read_telemetry"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("pcg_graph"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_pcg_graph"))      return Action_CreatePCGGraph(Params);
	if (Action == TEXT("add_pcg_node"))          return Action_AddPCGNode(Params);
	if (Action == TEXT("connect_pcg_pins"))      return Action_ConnectPCGPins(Params);
	if (Action == TEXT("set_pcg_node_settings")) return Action_SetPCGNodeSettings(Params);
	if (Action == TEXT("get_pcg_graph_topology"))return Action_GetPCGGraphTopology(Params);
	if (Action == TEXT("remove_pcg_node"))      return Action_RemovePCGNode(Params);
	if (Action == TEXT("disconnect_pcg_pins"))  return Action_DisconnectPCGPins(Params);
	if (Action == TEXT("get_pcg_parameters"))   return Action_GetPCGParameters(Params);
	if (Action == TEXT("set_seed"))             return Action_SetSeed(Params);
	if (Action == TEXT("cleanup"))              return Action_Cleanup(Params);
	if (Action == TEXT("regenerate"))           return Action_Regenerate(Params);
	if (Action == TEXT("read_telemetry"))          return Action_ReadTelemetry(Params);
	if (Action == TEXT("add_attribute_override"))  return Action_AddAttributeOverride(Params);
	if (Action == TEXT("get_node_list"))           return Action_GetNodeList(Params);
	if (Action == TEXT("set_mesh_entry"))          return Action_SetMeshEntry(Params);
	if (Action == TEXT("execute_local"))           return Action_ExecuteLocal(Params);

	// Back-compat aliases — spec-canonical short names routed to our canonical implementations.
	if (Action == TEXT("create_graph"))       return Action_CreatePCGGraph(Params);
	if (Action == TEXT("add_node"))           return Action_AddPCGNode(Params);
	if (Action == TEXT("connect_nodes"))      return Action_ConnectPCGPins(Params);
	if (Action == TEXT("disconnect_nodes"))   return Action_DisconnectPCGPins(Params);
	if (Action == TEXT("remove_node"))        return Action_RemovePCGNode(Params);
	if (Action == TEXT("set_node_property"))  return Action_SetPCGNodeSettings(Params);
	if (Action == TEXT("set_node_settings"))  return Action_SetPCGNodeSettings(Params);
	if (Action == TEXT("get_topology"))       return Action_GetPCGGraphTopology(Params);
	if (Action == TEXT("list_nodes"))         return Action_GetNodeList(Params);
	if (Action == TEXT("get_parameters"))     return Action_GetPCGParameters(Params);
	if (Action == TEXT("execute"))            return Action_ExecuteLocal(Params);

	return MakeError(TEXT("pcg_graph"), Action, 1001,
		FString::Printf(TEXT("Unknown pcg_graph action '%s'"), *Action),
		TEXT("Valid: create_pcg_graph (alias create_graph), add_pcg_node, connect_pcg_pins (alias connect_nodes), set_pcg_node_settings (alias set_node_property), get_pcg_graph_topology, remove_pcg_node, disconnect_pcg_pins, get_pcg_parameters, set_seed, cleanup, regenerate, read_telemetry, add_attribute_override, get_node_list, set_mesh_entry, execute_local"));
}

// ---------------------------------------------------------------------------
// create_pcg_graph
// Params: asset_path (string) — e.g. "/Game/PCG/MyGraph"
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_CreatePCGGraph(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg_graph"), TEXT("create_pcg_graph"));

	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_pcg_graph: 'asset_path' is required");
		return Result;
	}

	FString AssetName  = FPackageName::GetLongPackageAssetName(AssetPath);
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	// UE 5.7: PCGGraphFactory removed — create asset without factory
	UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, UPCGGraph::StaticClass(), nullptr);

	if (!NewAsset)
	{
		Result.Message = FString::Printf(TEXT("create_pcg_graph: failed to create asset at '%s'"), *AssetPath);
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Created PCG graph: %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_pcg_node
// Params: asset_path, node_type (registry key), x (int, opt), y (int, opt)
// Returns: node index in ExtraData as JSON {"node_index": N}
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_AddPCGNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg_graph"), TEXT("add_pcg_node"));

	FString AssetPath, NodeType;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("node_type"),  NodeType)  || NodeType.IsEmpty())
	{
		Result.Message = TEXT("add_pcg_node: 'asset_path' and 'node_type' are required");
		return Result;
	}

	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Result);
	if (!Graph) return Result;

	TSubclassOf<UPCGSettings> SettingsClass = ResolveNodeType(NodeType);
	if (!SettingsClass)
	{
		Result.ErrorCode = 3000; // NOT_SUPPORTED — unknown node_type registry key
		Result.Message = FString::Printf(
			TEXT("add_pcg_node: unknown node_type '%s'. "
			     "Valid UE PCG types: SurfaceSampler, SplineSampler, SphereSampler, StaticMeshSpawner, SpawnActor, "
			     "DensityFilter, PointFilter, AttributeFilter, AttributeFiltering, SelfPruning, DistanceShape, "
			     "DifferencePoints, IntersectionPoints, UnionPoints, MergePoints, GatherPoints, CopyPoints, ConvexHull, "
			     "TransformPoints, FlipPoints, ProjectPoints, BoundsModifier, NormalToDensity, "
			     "AttributeNoise, CreateAttribute, CopyAttributes, DeleteAttributes, MetadataOperation, "
			     "AttributeToProperty, PropertyToAttribute, MatchAndSet, MatchAndSetAttributes, "
			     "GetActorData, GetLandscapeData, GetLandscapeHeightData, GetSplineData, GetPrimitiveData, "
			     "GetVolumeData, GetPolyLineData, GetActorProperty, SetActorProperty, "
			     "ExecuteBlueprint, AddTag, Reroute, GridProjection, MakeConcretePoints. "
			     "Valid PCGEx types (require plugin): Delaunay2D, Voronoi2D, Relax, Simplification, BridgeClusters, "
			     "FuseClusters, DensifyClusters, AStar, Navmesh, PathfindingEdges, "
			     "SampleNearestPoint, SampleNearestSpline, SampleNearestSurface, "
			     "PoleEffector, SpinEffector, FlowEffector, ExtrudeTensors, "
			     "PathToEdges, MakePathsFromEdges, ClustersToPoints, "
			     "SortPoints, SubdivideEdges, EdgeDirection, WriteNeighbors, ClusterFilter, EdgeFilter. "
			     "Use system/describe with domain='pcg_graph' for the full node registry."),
			*NodeType);
		return Result;
	}

	UPCGSettings* DefaultSettings = NewObject<UPCGSettings>(Graph, SettingsClass);
	UPCGNode* NewNode = Graph->AddNode(DefaultSettings);
	if (!NewNode)
	{
		Result.Message = FString::Printf(TEXT("add_pcg_node: Graph->AddNode returned null for type '%s'"), *NodeType);
		return Result;
	}

	// Apply optional editor position
	double XVal = 0.0, YVal = 0.0;
	Params->TryGetNumberField(TEXT("x"), XVal);
	Params->TryGetNumberField(TEXT("y"), YVal);
	NewNode->PositionX = (int32)XVal;
	NewNode->PositionY = (int32)YVal;

	Graph->MarkPackageDirty();

	// Return the 0-based index of the newly added node in Graph->GetNodes()
	const auto& Nodes = Graph->GetNodes();
	int32 NewIndex = Nodes.Num() - 1;

	// Build ExtraData JSON
	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetNumberField(TEXT("node_index"), (double)NewIndex);
	Extra->SetStringField(TEXT("node_type"),  NodeType);
	Extra->SetNumberField(TEXT("position_x"), (double)NewNode->PositionX);
	Extra->SetNumberField(TEXT("position_y"), (double)NewNode->PositionY);

	FString ExtraStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ExtraStr);
	FJsonSerializer::Serialize(Extra.ToSharedRef(), W);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.ExtraData    = ExtraStr;
	Result.Message      = FString::Printf(TEXT("Added node '%s' at index %d"), *NodeType, NewIndex);
	return Result;
}

// ---------------------------------------------------------------------------
// connect_pcg_pins
// Params: asset_path, from_node (int or "input"/"output"), from_pin,
//         to_node   (int or "input"/"output"), to_pin
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_ConnectPCGPins(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg_graph"), TEXT("connect_pcg_pins"));

	FString AssetPath, FromNodeRef, FromPin, ToNodeRef, ToPin;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)   || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("from_node"),   FromNodeRef) || FromNodeRef.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("from_pin"),    FromPin)     || FromPin.IsEmpty()     ||
	    !Params->TryGetStringField(TEXT("to_node"),     ToNodeRef)   || ToNodeRef.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("to_pin"),      ToPin)       || ToPin.IsEmpty())
	{
		Result.Message = TEXT("connect_pcg_pins: asset_path, from_node, from_pin, to_node, to_pin are all required");
		return Result;
	}

	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Result);
	if (!Graph) return Result;

	FString FromError, ToError;
	UPCGNode* FromNode = ResolveNode(Graph, FromNodeRef, FromError);
	UPCGNode* ToNode   = ResolveNode(Graph, ToNodeRef,   ToError);

	if (!FromNode)
	{
		Result.Message = FString::Printf(TEXT("connect_pcg_pins: from_node '%s' — %s"), *FromNodeRef, *FromError);
		return Result;
	}
	if (!ToNode)
	{
		Result.Message = FString::Printf(TEXT("connect_pcg_pins: to_node '%s' — %s"), *ToNodeRef, *ToError);
		return Result;
	}

	// AddEdge was renamed to AddLabeledEdge in UE 5.4; AddEdge silently no-ops in 5.7.
	// AddLabeledEdge returns bool (true on success) in UE 5.7.
	const bool bEdgeAdded = Graph->AddLabeledEdge(FromNode, FName(*FromPin), ToNode, FName(*ToPin));
	if (!bEdgeAdded)
	{
		Result.Message = FString::Printf(
			TEXT("connect_pcg_pins: AddLabeledEdge failed — pin labels '%s'->'%s' may not exist on the nodes"),
			*FromPin, *ToPin);
		return Result;
	}

	Graph->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Connected %s[%s] -> %s[%s]"),
		*FromNodeRef, *FromPin, *ToNodeRef, *ToPin);
	return Result;
}

// ---------------------------------------------------------------------------
// set_pcg_node_settings
// Params: asset_path, node_index (int), property_name (string), value (string)
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_SetPCGNodeSettings(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg_graph"), TEXT("set_pcg_node_settings"));

	FString AssetPath, PropName, Value;
	double  NodeIndexNum = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("node_index"),    NodeIndexNum)                      ||
	    !Params->TryGetStringField(TEXT("property_name"), PropName)  || PropName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("value"),         Value))
	{
		Result.Message = TEXT("set_pcg_node_settings: asset_path, node_index, property_name, value are all required");
		return Result;
	}

	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Result);
	if (!Graph) return Result;

	const auto& Nodes = Graph->GetNodes();
	const int32 NodeIndex = (int32)NodeIndexNum;
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		Result.Message = FString::Printf(
			TEXT("set_pcg_node_settings: node_index %d out of range (graph has %d user nodes)"),
			NodeIndex, Nodes.Num());
		return Result;
	}

	UPCGNode*     Node     = Nodes[NodeIndex];
	UPCGSettings* Settings = Node ? const_cast<UPCGSettings*>(Node->GetSettings()) : nullptr;
	if (!Settings)
	{
		Result.Message = FString::Printf(TEXT("set_pcg_node_settings: node %d has null settings"), NodeIndex);
		return Result;
	}

	FProperty* Prop = Settings->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		Result.Message = FString::Printf(TEXT("set_pcg_node_settings: property '%s' not found on %s"),
			*PropName, *Settings->GetClass()->GetName());
		return Result;
	}

	void* Container = Settings;

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop)) { FloatProp->SetPropertyValue_InContainer(Container, FCString::Atof(*Value)); }
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop)) { DoubleProp->SetPropertyValue_InContainer(Container, (double)FCString::Atof(*Value)); }
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop)) { IntProp->SetPropertyValue_InContainer(Container, FCString::Atoi(*Value)); }
	else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop)) { BoolProp->SetPropertyValue_InContainer(Container, Value.ToBool()); }
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop)) { StrProp->SetPropertyValue_InContainer(Container, Value); }
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop)) { NameProp->SetPropertyValue_InContainer(Container, FName(*Value)); }
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		// Value is the enum entry name string (e.g. "AllActors")
		int64 EnumVal = EnumProp->GetEnum()->GetValueByNameString(Value);
		if (EnumVal == INDEX_NONE)
		{
			Result.Message = FString::Printf(
				TEXT("set_pcg_node_settings: invalid enum value '%s' for property '%s'. "
				     "Use GetValueByNameString-compatible enum entry name."),
				*Value, *PropName);
			return Result;
		}
		EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(
			EnumProp->ContainerPtrToValuePtr<void>(Container), EnumVal);
	}
	else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		// Byte properties that back enums
		if (ByteProp->Enum)
		{
			int64 EnumVal = ByteProp->Enum->GetValueByNameString(Value);
			if (EnumVal == INDEX_NONE)
			{
				Result.Message = FString::Printf(
					TEXT("set_pcg_node_settings: invalid enum value '%s' for byte-enum property '%s'"),
					*Value, *PropName);
				return Result;
			}
			ByteProp->SetPropertyValue_InContainer(Container, (uint8)EnumVal);
		}
		else
		{
			ByteProp->SetPropertyValue_InContainer(Container, (uint8)FCString::Atoi(*Value));
		}
	}
	else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		// Value is a content path string (e.g. "/Game/MyMesh.MyMesh")
		UObject* LoadedObj = FSoftObjectPath(Value).TryLoad();
		if (!LoadedObj)
		{
			Result.Message = FString::Printf(
				TEXT("set_pcg_node_settings: object at path '%s' could not be loaded"), *Value);
			Result.ErrorCode = 2003; // ASSET_NOT_LOADED
			return Result;
		}
		if (!LoadedObj->IsA(ObjProp->PropertyClass))
		{
			Result.Message = FString::Printf(
				TEXT("set_pcg_node_settings: object '%s' is not a '%s'"),
				*Value, *ObjProp->PropertyClass->GetName());
			Result.ErrorCode = 2001;
			return Result;
		}
		ObjProp->SetObjectPropertyValue_InContainer(Container, LoadedObj);
	}
	else if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
	{
		// UE 5.7: FSoftObjectProperty::SetPropertyValue_InContainer expects FSoftObjectPtr,
		// not FSoftObjectPath. Build the FSoftObjectPath as a named local so the
		// FSoftObjectPtr construction is unambiguous (avoids most-vexing-parse).
		const FSoftObjectPath SoftPath(Value);
		const FSoftObjectPtr  SoftPtr = FSoftObjectPtr(SoftPath);
		SoftObjProp->SetPropertyValue_InContainer(Container, SoftPtr);
	}
	else
	{
		Result.Message = FString::Printf(TEXT("set_pcg_node_settings: unsupported property type '%s' for '%s'"),
			*Prop->GetClass()->GetName(), *PropName);
		return Result;
	}

	Settings->MarkPackageDirty();
	Graph->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Set node[%d].%s = '%s' on %s"),
		NodeIndex, *PropName, *Value, *Settings->GetClass()->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// get_pcg_graph_topology
// Params: asset_path
// Returns: JSON topology in ExtraData
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_GetPCGGraphTopology(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg_graph"), TEXT("get_pcg_graph_topology"));

	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_pcg_graph_topology: 'asset_path' is required");
		return Result;
	}

	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Result);
	if (!Graph) return Result;

	// Build topology JSON ---------------------------------------------------
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Graph-level input/output nodes
	{
		TSharedPtr<FJsonObject> InputObj  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
		InputObj ->SetStringField(TEXT("label"), TEXT("GraphInput"));
		OutputObj->SetStringField(TEXT("label"), TEXT("GraphOutput"));
		Root->SetObjectField(TEXT("input_node"),  InputObj);
		Root->SetObjectField(TEXT("output_node"), OutputObj);
	}

	// User-added nodes
	const auto& Nodes = Graph->GetNodes();
	TArray<TSharedPtr<FJsonValue>> NodeArray;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		UPCGNode* Node = Nodes[i];
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("index"),      (double)i);
		NodeObj->SetNumberField(TEXT("position_x"), (double)Node->PositionX);
		NodeObj->SetNumberField(TEXT("position_y"), (double)Node->PositionY);

		const UPCGSettings* Settings = Node->GetSettings();
		NodeObj->SetStringField(TEXT("settings_class"),
			Settings ? Settings->GetClass()->GetName() : TEXT("None"));

		// Enumerate outbound edges via output pins.
		// PCG edge naming is reversed from Blueprint convention:
		//   InputPin  = the SOURCE pin (the upstream node's output pin — data enters the edge here)
		//   OutputPin = the DESTINATION pin (the downstream node's input pin — data exits the edge here)
		// So to find the target node we follow Edge->OutputPin, not Edge->InputPin.
		TArray<TSharedPtr<FJsonValue>> EdgeArray;
		for (UPCGPin* Pin : Node->GetOutputPins())
		{
			if (!Pin) continue;
			for (UPCGEdge* Edge : Pin->Edges)
			{
				if (!Edge || !Edge->OutputPin) continue;
				UPCGNode* TargetNode = Edge->OutputPin->Node;
				if (!TargetNode) continue;

				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetStringField(TEXT("from_pin"), Pin->Properties.Label.ToString());

				// Resolve target to index ("input"/"output" for implicit nodes)
				FString ToRef;
				if (TargetNode == Graph->GetInputNode())       ToRef = TEXT("input");
				else if (TargetNode == Graph->GetOutputNode()) ToRef = TEXT("output");
				else
				{
					int32 Idx = Nodes.IndexOfByKey(TargetNode);
					ToRef = (Idx != INDEX_NONE) ? FString::FromInt(Idx) : TEXT("unknown");
				}
				EdgeObj->SetStringField(TEXT("to_node"), ToRef);
				EdgeObj->SetStringField(TEXT("to_pin"),  Edge->OutputPin->Properties.Label.ToString());
				EdgeArray.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
		}
		NodeObj->SetArrayField(TEXT("edges_out"), EdgeArray);
		NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Root->SetArrayField(TEXT("nodes"), NodeArray);
	Root->SetNumberField(TEXT("node_count"), (double)Nodes.Num());

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Data         = Root;
	Result.Message      = FString::Printf(TEXT("Graph topology: %d nodes"), Nodes.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// LoadPCGGraph — helper
// ---------------------------------------------------------------------------

UPCGGraph* UPCGGraphHandler::LoadPCGGraph(const FString& AssetPath, FBridgeResult& Result)
{
	FString LongPackageName = AssetPath;
	if (!LongPackageName.StartsWith(TEXT("/")))
		LongPackageName = TEXT("/Game/") + LongPackageName;

	// Try loading the package
	UObject* Loaded = StaticLoadObject(UPCGGraph::StaticClass(), nullptr, *LongPackageName);
	if (!Loaded)
	{
		// Also try with package.assetname suffix
		FString AssetName = FPackageName::GetLongPackageAssetName(LongPackageName);
		FString FullRef   = LongPackageName + TEXT(".") + AssetName;
		Loaded = StaticLoadObject(UPCGGraph::StaticClass(), nullptr, *FullRef);
	}

	UPCGGraph* Graph = Cast<UPCGGraph>(Loaded);
	if (!Graph)
	{
		Result.bSuccess = false;
		Result.ErrorCode = 2000;
		Result.Message  = FString::Printf(TEXT("LoadPCGGraph: could not load UPCGGraph at '%s'"), *AssetPath);
	}
	return Graph;
}

// ---------------------------------------------------------------------------
// ResolveNode — helper
// ---------------------------------------------------------------------------

UPCGNode* UPCGGraphHandler::ResolveNode(UPCGGraph* Graph, const FString& NodeRef, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return nullptr;
	}

	if (NodeRef.Equals(TEXT("input"), ESearchCase::IgnoreCase))
		return Graph->GetInputNode();

	if (NodeRef.Equals(TEXT("output"), ESearchCase::IgnoreCase))
		return Graph->GetOutputNode();

	if (!NodeRef.IsNumeric())
	{
		OutError = FString::Printf(TEXT("'%s' is not a valid node ref (use integer index, 'input', or 'output')"), *NodeRef);
		return nullptr;
	}

	const int32 Index = FCString::Atoi(*NodeRef);
	const auto& Nodes = Graph->GetNodes();
	if (!Nodes.IsValidIndex(Index))
	{
		OutError = FString::Printf(TEXT("index %d out of range (%d nodes)"), Index, Nodes.Num());
		return nullptr;
	}

	return Nodes[Index];
}

// ---------------------------------------------------------------------------
// ResolveNodeType — static helper
// Maps short keys to UPCGSettings subclass objects via FindFirstObject.
// No element headers needed here; classes are found at runtime.
// ---------------------------------------------------------------------------

TSubclassOf<UPCGSettings> UPCGGraphHandler::ResolveNodeType(const FString& NodeType)
{
	// Short key → fully-qualified UClass name (resolved via FindFirstObject at runtime).
	// Classes from optional plugins (e.g. PCGEx) resolve only if the plugin is loaded.
	// Adding an unknown key is safe: FindFirstObject returns nullptr and the call fails gracefully.
	static const TMap<FString, FString> Registry =
	{
		// ----------------------------------------------------------------
		// Standard UE PCG — Sampling
		// ----------------------------------------------------------------
		{ TEXT("SurfaceSampler"),         TEXT("PCGSurfaceSamplerSettings")          },
		{ TEXT("SplineSampler"),          TEXT("PCGSplineSamplerSettings")           },
		{ TEXT("SphereSampler"),          TEXT("PCGSphereSamplerSettings")           },
		{ TEXT("StaticMeshSpawner"),      TEXT("PCGStaticMeshSpawnerSettings")       },
		{ TEXT("SpawnActor"),             TEXT("PCGSpawnActorSettings")              },

		// ----------------------------------------------------------------
		// Standard UE PCG — Filtering & Pruning
		// ----------------------------------------------------------------
		{ TEXT("DensityFilter"),          TEXT("PCGDensityFilterSettings")           },
		{ TEXT("PointFilter"),            TEXT("PCGPointFilterSettings")             },
		{ TEXT("AttributeFilter"),        TEXT("PCGAttributeFilteringSettings")      },
		{ TEXT("AttributeFiltering"),     TEXT("PCGAttributeFilteringSettings")      },
		{ TEXT("SelfPruning"),            TEXT("PCGSelfPruningSettings")             },
		{ TEXT("DistanceShape"),          TEXT("PCGDistanceSettings")                },

		// ----------------------------------------------------------------
		// Standard UE PCG — Set Operations
		// ----------------------------------------------------------------
		{ TEXT("DifferencePoints"),       TEXT("PCGDifferenceSettings")              },
		{ TEXT("IntersectionPoints"),     TEXT("PCGIntersectionSettings")            },
		{ TEXT("UnionPoints"),            TEXT("PCGUnionSettings")                   },
		{ TEXT("MergePoints"),            TEXT("PCGMergeSettings")                   },
		{ TEXT("GatherPoints"),           TEXT("PCGGatherSettings")                  },
		{ TEXT("CopyPoints"),             TEXT("PCGCopyPointsSettings")              },
		{ TEXT("ConvexHull"),             TEXT("PCGConvexHullSettings")              },

		// ----------------------------------------------------------------
		// Standard UE PCG — Transform & Geometry
		// ----------------------------------------------------------------
		{ TEXT("TransformPoints"),        TEXT("PCGTransformPointsSettings")         },
		{ TEXT("FlipPoints"),             TEXT("PCGFlipPointsSettings")              },
		{ TEXT("ProjectPoints"),          TEXT("PCGProjectPointsSettings")           },
		{ TEXT("BoundsModifier"),         TEXT("PCGBoundsModifierSettings")          },
		{ TEXT("NormalToDensity"),        TEXT("PCGNormalToDensitySettings")         },
		// SubdivideEdges is PCGEx-only; no such node in standard UE PCG. Moved to PCGEx section below.

		// ----------------------------------------------------------------
		// Standard UE PCG — Attributes
		// ----------------------------------------------------------------
		{ TEXT("AttributeNoise"),         TEXT("PCGAttributeNoiseSettings")          },
		{ TEXT("CreateAttribute"),        TEXT("PCGCreateAttributeSettings")         },
		{ TEXT("CopyAttributes"),         TEXT("PCGCopyAttributesSettings")          },
		{ TEXT("DeleteAttributes"),       TEXT("PCGDeleteAttributesSettings")        },
		{ TEXT("MetadataOperation"),      TEXT("PCGMetadataOperationSettings")       },
		{ TEXT("AttributeToProperty"),    TEXT("PCGAttributeToPropertySettings")     },
		{ TEXT("PropertyToAttribute"),    TEXT("PCGPropertyToAttributeSettings")     },
		{ TEXT("MatchAndSet"),            TEXT("PCGMatchAndSetAttributesSettings")   },
		{ TEXT("MatchAndSetAttributes"),  TEXT("PCGMatchAndSetAttributesSettings")   },

		// ----------------------------------------------------------------
		// Standard UE PCG — Data Input
		// ----------------------------------------------------------------
		{ TEXT("GetActorData"),           TEXT("PCGGetActorDataSettings")            },
		{ TEXT("GetLandscapeData"),       TEXT("PCGGetLandscapeDataSettings")        },
		{ TEXT("GetLandscapeHeightData"), TEXT("PCGGetLandscapeHeightDataSettings")  },
		{ TEXT("GetSplineData"),          TEXT("PCGGetSplineDataSettings")           },
		{ TEXT("GetPrimitiveData"),       TEXT("PCGGetPrimitiveDataSettings")        },
		{ TEXT("GetVolumeData"),          TEXT("PCGGetVolumeDataSettings")           },
		{ TEXT("GetPolyLineData"),        TEXT("PCGGetPolyLineDataSettings")         },
		{ TEXT("GetActorProperty"),       TEXT("PCGGetActorPropertySettings")        },
		{ TEXT("SetActorProperty"),       TEXT("PCGSetActorPropertySettings")        },

		// ----------------------------------------------------------------
		// Standard UE PCG — Graph / Flow
		// ----------------------------------------------------------------
		{ TEXT("ExecuteBlueprint"),       TEXT("PCGExecuteBlueprintSettings")        },
		{ TEXT("AddTag"),                 TEXT("PCGAddTagSettings")                  },
		{ TEXT("Reroute"),                TEXT("PCGRerouteSettings")                 },
		{ TEXT("GridProjection"),         TEXT("PCGGridProjectionSettings")          },
		{ TEXT("MakeConcretePoints"),     TEXT("PCGMakeConcretePointsSettings")      },

		// ----------------------------------------------------------------
		// PCGEx — Clusters / Graph construction  (requires PCGEx plugin)
		// ----------------------------------------------------------------
		{ TEXT("Delaunay2D"),             TEXT("PCGExBuildDelaunayGraph2DSettings")  },
		{ TEXT("Voronoi2D"),              TEXT("PCGExBuildVoronoiGraph2DSettings")   },
		{ TEXT("Relax"),                  TEXT("PCGExRelaxClustersSettings")         },
		{ TEXT("Simplification"),         TEXT("PCGExSimplifyEdgesSettings")         },
		{ TEXT("BridgeClusters"),         TEXT("PCGExBridgeClustersSettings")        },
		{ TEXT("FuseClusters"),           TEXT("PCGExFuseClustersSettings")          },
		{ TEXT("DensifyClusters"),        TEXT("PCGExDensifyEdgesSettings")          },

		// ----------------------------------------------------------------
		// PCGEx — Pathfinding
		// ----------------------------------------------------------------
		{ TEXT("AStar"),                  TEXT("PCGExPathfindingAStarSettings")      },
		{ TEXT("Navmesh"),                TEXT("PCGExPathfindingNavmeshSettings")    },
		{ TEXT("PathfindingEdges"),       TEXT("PCGExPathfindingEdgesSettings")      },

		// ----------------------------------------------------------------
		// PCGEx — Sampling
		// ----------------------------------------------------------------
		{ TEXT("SampleNearestPoint"),     TEXT("PCGExSampleNearestPointSettings")    },
		{ TEXT("SampleNearestSpline"),    TEXT("PCGExSampleNearestSplineSettings")   },
		{ TEXT("SampleNearestSurface"),   TEXT("PCGExSampleNearestSurfaceSettings")  },

		// ----------------------------------------------------------------
		// PCGEx — Tensors
		// ----------------------------------------------------------------
		{ TEXT("PoleEffector"),           TEXT("PCGExTensorPoleEffectorSettings")    },
		{ TEXT("SpinEffector"),           TEXT("PCGExTensorSpinEffectorSettings")    },
		{ TEXT("FlowEffector"),           TEXT("PCGExTensorFlowEffectorSettings")    },
		{ TEXT("ExtrudeTensors"),         TEXT("PCGExExtrudeTensorsSettings")        },

		// ----------------------------------------------------------------
		// PCGEx — Path <-> Cluster
		// ----------------------------------------------------------------
		{ TEXT("PathToEdges"),            TEXT("PCGExPathToEdgesSettings")           },
		{ TEXT("MakePathsFromEdges"),     TEXT("PCGExMakePathsFromEdgesSettings")    },
		{ TEXT("ClustersToPoints"),       TEXT("PCGExClustersToPointsSettings")      },

		// ----------------------------------------------------------------
		// PCGEx — Sorting & Misc
		// ----------------------------------------------------------------
		{ TEXT("SortPoints"),             TEXT("PCGExSortPointsSettings")            },
		{ TEXT("SubdivideEdges"),         TEXT("PCGExSubdivideEdgesSettings")        },
		{ TEXT("EdgeDirection"),          TEXT("PCGExEdgeDirectionSettings")         },
		{ TEXT("WriteNeighbors"),         TEXT("PCGExWriteEdgesExtrasSettings")      },
		{ TEXT("ClusterFilter"),          TEXT("PCGExClusterFilterSettings")         },
		{ TEXT("EdgeFilter"),             TEXT("PCGExEdgeFilterSettings")            },
	};

	const FString* ClassName = Registry.Find(NodeType);
	if (!ClassName) return nullptr;

	UClass* Found = FindFirstObject<UClass>(**ClassName, EFindFirstObjectOptions::NativeFirst);
	if (Found && Found->IsChildOf(UPCGSettings::StaticClass()))
		return TSubclassOf<UPCGSettings>(Found);

	return nullptr;
}

// ===========================================================================
// Phase 3 expansions — new actions
// ===========================================================================

static const FString PCG_DOMAIN = TEXT("pcg_graph");

// ---------------------------------------------------------------------------
// remove_pcg_node
// Params: asset_path, node_name (string) or node_index (int)
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_RemovePCGNode(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("remove_pcg_node");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

	FBridgeResult TempResult = CreateResult(PCG_DOMAIN, Action);
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, TempResult);
	if (!Graph)
		return MakeError(PCG_DOMAIN, Action, 2000, TempResult.Message.IsEmpty()
			? FString::Printf(TEXT("Could not load PCG graph at '%s'"), *AssetPath)
			: TempResult.Message);

	// Resolve by node_name (string ref) or node_index (number)
	FString NodeRef;
	double NodeIndexNum = -1.0;
	Params->TryGetStringField(TEXT("node_name"), NodeRef);
	Params->TryGetNumberField(TEXT("node_index"), NodeIndexNum);

	if (NodeRef.IsEmpty() && NodeIndexNum < 0)
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'node_name' or 'node_index' is required"));

	if (NodeRef.IsEmpty())
		NodeRef = FString::FromInt((int32)NodeIndexNum);

	FString Error;
	UPCGNode* Node = ResolveNode(Graph, NodeRef, Error);
	if (!Node)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not resolve node '%s': %s"), *NodeRef, *Error));

	Graph->RemoveNode(Node);
	Graph->MarkPackageDirty();

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Removed node '%s' from graph '%s'"), *NodeRef, *AssetPath));
#else
	return MakeError(PCG_DOMAIN, TEXT("remove_pcg_node"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// disconnect_pcg_pins
// Params: asset_path, from_node, from_pin, to_node, to_pin
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_DisconnectPCGPins(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("disconnect_pcg_pins");

	FString AssetPath, FromNodeRef, FromPin, ToNodeRef, ToPin;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("from_node"),  FromNodeRef) || FromNodeRef.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("from_pin"),   FromPin)    || FromPin.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("to_node"),    ToNodeRef)  || ToNodeRef.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("to_pin"),     ToPin)      || ToPin.IsEmpty())
	{
		return MakeError(PCG_DOMAIN, Action, 1000,
			TEXT("asset_path, from_node, from_pin, to_node, to_pin are all required"));
	}

	FBridgeResult TempResult = CreateResult(PCG_DOMAIN, Action);
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, TempResult);
	if (!Graph)
		return MakeError(PCG_DOMAIN, Action, 2000, TempResult.Message.IsEmpty()
			? FString::Printf(TEXT("Could not load PCG graph at '%s'"), *AssetPath)
			: TempResult.Message);

	FString FromError, ToError;
	UPCGNode* FromNode = ResolveNode(Graph, FromNodeRef, FromError);
	UPCGNode* ToNode   = ResolveNode(Graph, ToNodeRef,   ToError);

	if (!FromNode)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("from_node '%s': %s"), *FromNodeRef, *FromError));
	if (!ToNode)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("to_node '%s': %s"), *ToNodeRef, *ToError));

	// Find and remove the edge between the specified pins
	bool bRemoved = Graph->RemoveEdge(FromNode, FName(*FromPin), ToNode, FName(*ToPin));
	if (!bRemoved)
	{
		return MakeError(PCG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("No edge found between %s[%s] -> %s[%s]"), *FromNodeRef, *FromPin, *ToNodeRef, *ToPin),
			TEXT("Check pin labels with get_pcg_graph_topology"));
	}

	Graph->MarkPackageDirty();

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Disconnected %s[%s] -> %s[%s]"), *FromNodeRef, *FromPin, *ToNodeRef, *ToPin));
#else
	return MakeError(PCG_DOMAIN, TEXT("disconnect_pcg_pins"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// get_pcg_parameters
// Params: asset_path
// Returns: JSON array of graph parameters (UE 5.7 FPropertyBag-based)
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_GetPCGParameters(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("get_pcg_parameters");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

	FBridgeResult TempResult = CreateResult(PCG_DOMAIN, Action);
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, TempResult);
	if (!Graph)
		return MakeError(PCG_DOMAIN, Action, 2000, TempResult.Message.IsEmpty()
			? FString::Printf(TEXT("Could not load PCG graph at '%s'"), *AssetPath)
			: TempResult.Message);

	// UE 5.7: PCG graph parameters are stored via FInstancedPropertyBag
	TArray<TSharedPtr<FJsonValue>> ParamArray;

	const FInstancedPropertyBag* ParamBag = Graph->GetUserParametersStruct();
	const UPropertyBag* BagStruct = ParamBag ? ParamBag->GetPropertyBagStruct() : nullptr;
	if (BagStruct && ParamBag)
	{
		const FConstStructView BagValue = ParamBag->GetValue();
		for (TFieldIterator<FProperty> PropIt(BagStruct); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop) continue;

			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Prop->GetName());
			ParamObj->SetStringField(TEXT("type"), Prop->GetCPPType());

			// Export default value
			FString DefaultVal;
			if (BagValue.GetMemory())
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(BagValue.GetMemory());
				Prop->ExportTextItem_Direct(DefaultVal, ValuePtr, nullptr, nullptr, PPF_None);
			}
			ParamObj->SetStringField(TEXT("default"), DefaultVal);

			ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("parameters"), ParamArray);
	Data->SetNumberField(TEXT("count"), (double)ParamArray.Num());

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Graph has %d parameters"), ParamArray.Num()), Data);
#else
	return MakeError(PCG_DOMAIN, TEXT("get_pcg_parameters"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// set_seed
// Params: actor_path, seed (int)
// Operates on UPCGComponent in the world, not on an asset.
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_SetSeed(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("set_seed");

	FString ActorPath;
	double SeedNum = 0.0;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'actor_path' is required"));
	if (!Params->TryGetNumberField(TEXT("seed"), SeedNum))
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'seed' (integer) is required"));

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(PCG_DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* Actor = FindObject<AActor>(World->GetCurrentLevel(), *ActorPath);
	if (!Actor)
	{
		// Try iterating actors by label/path
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetPathName() == ActorPath || It->GetActorLabel() == ActorPath)
			{
				Actor = *It;
				break;
			}
		}
	}
	if (!Actor)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("Actor '%s' not found in world"), *ActorPath));

	UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
	if (!PCGComp)
		return MakeError(PCG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorPath),
			TEXT("Ensure the actor has a PCG component attached"));

	PCGComp->Seed = (int32)SeedNum;
	PCGComp->Generate(true);
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("seed"), SeedNum);

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Set seed to %d and regenerated PCG on '%s'"), (int32)SeedNum, *ActorPath), Data);
#else
	return MakeError(PCG_DOMAIN, TEXT("set_seed"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// cleanup
// Params: actor_path
// Calls CleanupLocal on the UPCGComponent.
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_Cleanup(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("cleanup");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(PCG_DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == ActorPath || It->GetActorLabel() == ActorPath)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("Actor '%s' not found in world"), *ActorPath));

	UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
	if (!PCGComp)
		return MakeError(PCG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorPath),
			TEXT("Ensure the actor has a PCG component attached"));

	PCGComp->CleanupLocal(/*bRemoveComponents=*/true);

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Cleaned up PCG on '%s'"), *ActorPath));
#else
	return MakeError(PCG_DOMAIN, TEXT("cleanup"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// regenerate
// Params: actor_path
// Calls Generate(true) on the UPCGComponent.
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_Regenerate(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("regenerate");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(PCG_DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == ActorPath || It->GetActorLabel() == ActorPath)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
		return MakeError(PCG_DOMAIN, Action, 2000, FString::Printf(TEXT("Actor '%s' not found in world"), *ActorPath));

	UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
	if (!PCGComp)
		return MakeError(PCG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorPath),
			TEXT("Ensure the actor has a PCG component attached"));

	PCGComp->Generate(true);

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Regenerated PCG on '%s'"), *ActorPath));
#else
	return MakeError(PCG_DOMAIN, TEXT("regenerate"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// read_telemetry
// Params: asset_path
// Returns: node count, connection count, per-node settings property dump.
// Enables the AI read-back loop: Read → Plan → Execute → Verify.
// ---------------------------------------------------------------------------

FBridgeResult UPCGGraphHandler::Action_ReadTelemetry(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("read_telemetry");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(PCG_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

	FBridgeResult TempResult = CreateResult(PCG_DOMAIN, Action);
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, TempResult);
	if (!Graph) return TempResult;

	const auto& Nodes = Graph->GetNodes();

	// Count total connections across all output pins of all nodes
	int32 ConnectionCount = 0;
	for (UPCGNode* Node : Nodes)
	{
		if (!Node) continue;
		for (UPCGPin* Pin : Node->GetOutputPins())
		{
			if (Pin) ConnectionCount += Pin->Edges.Num();
		}
	}

	// Build per-node property telemetry
	TArray<TSharedPtr<FJsonValue>> NodeTelemetry;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		UPCGNode* Node = Nodes[i];
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("index"),      (double)i);
		NodeObj->SetNumberField(TEXT("position_x"), (double)Node->PositionX);
		NodeObj->SetNumberField(TEXT("position_y"), (double)Node->PositionY);

		const UPCGSettings* Settings = Node->GetSettings();
		const UClass* SettingsClass  = Settings ? Settings->GetClass() : nullptr;
		NodeObj->SetStringField(TEXT("settings_class"), SettingsClass ? SettingsClass->GetName() : TEXT("None"));

		// Serialize every UPROPERTY on the settings object to a string via reflection
		TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		if (Settings && SettingsClass)
		{
			for (TFieldIterator<FProperty> PropIt(SettingsClass); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop) continue;
				if (!(Prop->PropertyFlags & CPF_Edit)) continue; // only user-editable props

				FString ValueStr;
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Settings);
				Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
				PropsObj->SetStringField(Prop->GetName(), ValueStr);
			}
		}
		NodeObj->SetObjectField(TEXT("properties"), PropsObj);

		// Pin labels
		TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
		for (UPCGPin* Pin : Node->GetInputPins())
			if (Pin) InPins.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		for (UPCGPin* Pin : Node->GetOutputPins())
			if (Pin) OutPins.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		NodeObj->SetArrayField(TEXT("input_pins"),  InPins);
		NodeObj->SetArrayField(TEXT("output_pins"), OutPins);

		NodeTelemetry.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),       AssetPath);
	Data->SetNumberField(TEXT("node_count"),        (double)Nodes.Num());
	Data->SetNumberField(TEXT("connection_count"),  (double)ConnectionCount);
	Data->SetArrayField (TEXT("nodes"),             NodeTelemetry);

	return MakeSuccess(PCG_DOMAIN, Action,
		FString::Printf(TEXT("Telemetry: %d nodes, %d connections in '%s'"),
			Nodes.Num(), ConnectionCount, *AssetPath),
		Data);
}

// ===========================================================================
// Phase 1c additions — add_attribute_override, get_node_list, set_mesh_entry, execute_local
// ===========================================================================

// ---------------------------------------------------------------------------
// add_attribute_override
// Params: asset_path, node_index (int), attribute_name (string),
//         source_attribute (string), operation (string: Set|Add|Multiply, optional)
// Adds/modifies a PCG attribute override on a node's settings via reflection.
// ---------------------------------------------------------------------------
FBridgeResult UPCGGraphHandler::Action_AddAttributeOverride(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString AssetPath, AttributeName, SourceAttribute;
	if (!Params->TryGetStringField(TEXT("asset_path"),      AssetPath)      || AssetPath.IsEmpty()      ||
	    !Params->TryGetStringField(TEXT("attribute_name"),  AttributeName)  || AttributeName.IsEmpty())
		return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 1000,
			TEXT("'asset_path' and 'attribute_name' are required"));

	FString NodeRef = TEXT("0");
	double NodeIdx = 0;
	if (Params->TryGetNumberField(TEXT("node_index"), NodeIdx))
		NodeRef = FString::FromInt((int32)NodeIdx);

	Params->TryGetStringField(TEXT("source_attribute"), SourceAttribute);

	FString Operation = TEXT("Set");
	Params->TryGetStringField(TEXT("operation"), Operation);

	FBridgeResult Tmp = CreateResult(PCG_DOMAIN, TEXT("add_attribute_override"));
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Tmp);
	if (!Graph) return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 2000, Tmp.Message);

	FString Err;
	UPCGNode* Node = ResolveNode(Graph, NodeRef, Err);
	if (!Node)
		return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 2001,
			FString::Printf(TEXT("Node '%s' not found: %s"), *NodeRef, *Err));

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
		return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 2002, TEXT("Node has no settings"));

	// NOTE: UE 5.7 PCG does NOT expose a universal `AttributeOverrides` UPROPERTY on UPCGSettings.
	// The override system is driven by the engine's OverridableParams pin-list (private,
	// regenerated at post-load), and per-property overrides are wired via an auto-generated
	// "Override Source Pin" input rather than by writing into an array on the settings asset.
	// The former FindFProperty("AttributeOverrides") branch always returned nullptr and is removed.
	//
	// Primary path: treat AttributeName as a UPROPERTY name on the settings object and set it
	// directly via reflection. Also try common canonical aliases if the user-supplied name
	// doesn't resolve (e.g. "input_attribute" -> "InputSource").
	FProperty* TargetProp = Settings->GetClass()->FindPropertyByName(FName(*AttributeName));

	if (!TargetProp)
	{
		static const TArray<FString> Aliases = {
			TEXT("InputSource"),  TEXT("InputAttributeName"),  TEXT("SourceAttribute"),
			TEXT("OutputTarget"), TEXT("OutputAttributeName"), TEXT("TargetAttribute")
		};
		for (const FString& Alias : Aliases)
		{
			TargetProp = Settings->GetClass()->FindPropertyByName(FName(*Alias));
			if (TargetProp) break;
		}
	}

	if (!TargetProp)
	{
		return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 3003,
			FString::Printf(TEXT("No property '%s' (or common alias) on '%s'"),
				*AttributeName, *Settings->GetClass()->GetName()),
			TEXT("Attribute override via PCG pin parameters is not directly accessible — "
			     "use set_pcg_node_settings to modify individual node settings properties instead. "
			     "Use read_telemetry / get_node_list to inspect editable UPROPERTY names."));
	}

	const FString Value = SourceAttribute.IsEmpty() ? AttributeName : SourceAttribute;
	TargetProp->ImportText_InContainer(*Value, Settings, Settings, PPF_None);
	// UE 5.7: UPCGGraph::NotifyGraphChanged is private. Use Modify + PostEditChange
	// on the settings object (which routes the change back through the graph) plus
	// MarkPackageDirty to signal the editor that the asset has changed.
	Settings->Modify();
	// UE 5.7: UPCGSettings::DirtyCache() is protected; Settings->Modify() above plus
	// the graph-level Modify/MarkPackageDirty below are sufficient to mark the change.
	Graph->Modify();
	Graph->MarkPackageDirty();

	return MakeSuccess(PCG_DOMAIN, TEXT("add_attribute_override"),
		FString::Printf(TEXT("Set '%s' = '%s' on node %s in '%s'"),
			*TargetProp->GetName(), *Value, *NodeRef, *AssetPath));
#else
	return MakeError(PCG_DOMAIN, TEXT("add_attribute_override"), 3003, TEXT("Editor only"));
#endif
}

// ---------------------------------------------------------------------------
// get_node_list
// Params: asset_path (string)
// Returns all nodes in the PCG graph with index, type, and settings class.
// ---------------------------------------------------------------------------
FBridgeResult UPCGGraphHandler::Action_GetNodeList(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(PCG_DOMAIN, TEXT("get_node_list"), 1000, TEXT("'asset_path' is required"));

	FBridgeResult Tmp = CreateResult(PCG_DOMAIN, TEXT("get_node_list"));
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Tmp);
	if (!Graph) return MakeError(PCG_DOMAIN, TEXT("get_node_list"), 2000, Tmp.Message);

	// UPCGGraph::GetNodes() already returns const TArray<UPCGNode*>&; copy directly.
	TArray<UPCGNode*> Nodes(Graph->GetNodes());
	TArray<TSharedPtr<FJsonValue>> NodeArr;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		UPCGNode* Node = Nodes[i];
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("index"), i);
		NodeObj->SetStringField(TEXT("name"), Node->GetName());

		UPCGSettings* S = Node->GetSettings();
		NodeObj->SetStringField(TEXT("settings_class"), S ? S->GetClass()->GetName() : TEXT("None"));

		// Pin counts
		const TArray<UPCGPin*>& InPins  = Node->GetInputPins();
		const TArray<UPCGPin*>& OutPins = Node->GetOutputPins();
		NodeObj->SetNumberField(TEXT("input_pin_count"), InPins.Num());
		NodeObj->SetNumberField(TEXT("output_pin_count"), OutPins.Num());

		NodeArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	// Also report input/output graph nodes
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), NodeArr.Num());
	Data->SetArrayField(TEXT("nodes"), NodeArr);

	return MakeSuccess(PCG_DOMAIN, TEXT("get_node_list"),
		FString::Printf(TEXT("%d nodes in '%s'"), Nodes.Num(), *AssetPath), Data);
#else
	return MakeError(PCG_DOMAIN, TEXT("get_node_list"), 3003, TEXT("Editor only"));
#endif
}

// ---------------------------------------------------------------------------
// set_mesh_entry
// Params: asset_path (string), node_index (int),
//         mesh_path (string), weight (float, optional, default 1.0),
//         entry_index (int, optional — overwrite specific entry; -1 = append)
// Sets a mesh entry on a UPCGStaticMeshSpawnerSettings node.
// C++ path: UPCGStaticMeshSpawnerSettings::MeshEntries (or Descriptor.Entries in older API)
// ---------------------------------------------------------------------------
FBridgeResult UPCGGraphHandler::Action_SetMeshEntry(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString AssetPath, MeshPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("mesh_path"),  MeshPath)  || MeshPath.IsEmpty())
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 1000,
			TEXT("'asset_path' and 'mesh_path' are required"));

	double NodeIdx = 0;
	if (!Params->TryGetNumberField(TEXT("node_index"), NodeIdx))
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 1000, TEXT("'node_index' is required"));

	double Weight = 1.0;
	Params->TryGetNumberField(TEXT("weight"), Weight);

	FBridgeResult Tmp = CreateResult(PCG_DOMAIN, TEXT("set_mesh_entry"));
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Tmp);
	if (!Graph) return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 2000, Tmp.Message);

	FString NodeRef = FString::FromInt((int32)NodeIdx);
	FString Err;
	UPCGNode* Node = ResolveNode(Graph, NodeRef, Err);
	if (!Node)
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 2001,
			FString::Printf(TEXT("Node '%s' not found: %s"), *NodeRef, *Err));

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 2002, TEXT("Node has no settings"));

	// Verify this is a StaticMeshSpawner settings node
	const FString ClassName = Settings->GetClass()->GetName();
	if (!ClassName.Contains(TEXT("StaticMeshSpawner")) && !ClassName.Contains(TEXT("MeshSpawner")))
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 2003,
			FString::Printf(TEXT("Node %d is not a StaticMeshSpawner (class: %s)"), (int32)NodeIdx, *ClassName),
			TEXT("Use add_pcg_node with node_type=StaticMeshSpawner first"));

	// Load the mesh asset
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		const FString Suffix = MeshPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MeshPath);
		Mesh = LoadObject<UStaticMesh>(nullptr, *Suffix);
	}
	if (!Mesh)
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 3003,
			FString::Printf(TEXT("StaticMesh not found at '%s'"), *MeshPath));

	// UE 5.7 layout:
	//   UPCGStaticMeshSpawnerSettings::MeshSelectorType      (TSubclassOf<UPCGMeshSelectorBase>)
	//   UPCGStaticMeshSpawnerSettings::MeshSelectorParameters (instanced sub-object of that class)
	//   UPCGMeshSelectorWeighted::MeshEntries                (TArray<FPCGMeshSelectorWeightedEntry>)
	//   FPCGMeshSelectorWeightedEntry { FSoftISMComponentDescriptor Descriptor; int32 Weight; }
	//   FSoftISMComponentDescriptor::StaticMesh              (TSoftObjectPtr<UStaticMesh>)
	//
	// The old implementation wrote `((Mesh=...,Weight=...))` directly on the settings class.
	// That fails on 5.7 because MeshEntries does not exist as a top-level UPROPERTY on the
	// settings class — it lives inside the MeshSelectorParameters sub-object.
	//
	// Resolve MeshSelectorParameters, then reflect MeshEntries on it and ImportText the entry.
	FProperty* SelectorProp = Settings->GetClass()->FindPropertyByName(TEXT("MeshSelectorParameters"));
	UObject* Selector = nullptr;
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(SelectorProp))
	{
		Selector = ObjProp->GetObjectPropertyValue_InContainer(Settings);
	}

	if (!Selector)
	{
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 3003,
			FString::Printf(
				TEXT("set_mesh_entry: MeshSelectorParameters sub-object not resolvable on node %d (class %s). "
				     "UE 5.7 moved weighted entries to the selector sub-object; ensure MeshSelectorType is "
				     "UPCGMeshSelectorWeighted so the sub-object is created, or use set_pcg_node_settings "
				     "to drive the selector manually."),
				(int32)NodeIdx, *ClassName),
			TEXT("Open the node in the PCG editor once after creation to materialize the selector sub-object, "
			     "then re-run set_mesh_entry."));
	}

	FArrayProperty* EntriesProp = CastField<FArrayProperty>(
		Selector->GetClass()->FindPropertyByName(TEXT("MeshEntries")));
	if (!EntriesProp)
	{
		return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 3003,
			FString::Printf(
				TEXT("set_mesh_entry: MeshEntries array not found on selector '%s' (node %d). "
				     "Selector must be UPCGMeshSelectorWeighted or a subclass exposing MeshEntries. "
				     "UE 5.7 header verification may be required if the struct layout has moved."),
				*Selector->GetClass()->GetName(), (int32)NodeIdx),
			TEXT("set_mesh_entry: MeshSelectorParameters struct layout requires UE 5.7 header verification. "
			     "Use Python/editor to configure mesh entries if reflection fails."));
	}

	// Build a one-element array literal in ImportText form.
	// Each entry is FPCGMeshSelectorWeightedEntry — StaticMesh lives at Descriptor.StaticMesh.
	// Weight is int32 in UE 5.7 (not float). Clamp to >= 1.
	const int32 IntWeight = FMath::Max(1, (int32)FMath::RoundToInt(Weight));
	const FString EntryStr = FString::Printf(
		TEXT("((Descriptor=(StaticMesh=\"%s\"),Weight=%d))"),
		*FSoftObjectPath(Mesh).ToString(),
		IntWeight);

	EntriesProp->ImportText_InContainer(*EntryStr, Selector, Selector, PPF_None);

	Selector->Modify();
	Settings->Modify();
	// UE 5.7: UPCGGraph::NotifyGraphChanged is private and UPCGSettings::DirtyCache/
	// PostEditChangeProperty are protected. The Modify() calls above plus the graph-level
	// MarkPackageDirty below are sufficient to mark the change through the editor
	// transaction system.
	Graph->Modify();
	Graph->MarkPackageDirty();

	// TODO: respect an `entry_index` param (replace-at-index / append semantics).
	// Current behavior reuses ImportText on the whole array; splice logic would require
	// FScriptArrayHelper + parsing the existing entries.

	return MakeSuccess(PCG_DOMAIN, TEXT("set_mesh_entry"),
		FString::Printf(TEXT("Set mesh entry '%s' (weight=%d) on node %d in '%s'"),
			*FPackageName::GetLongPackageAssetName(MeshPath), IntWeight, (int32)NodeIdx, *AssetPath));
#else
	return MakeError(PCG_DOMAIN, TEXT("set_mesh_entry"), 3003, TEXT("Editor only"));
#endif
}

// ---------------------------------------------------------------------------
// execute_local  (editor-only per spec)
// Params: asset_path (string), actor_name (string, optional — actor with PCGComponent)
// Triggers a local PCG graph execution on all actors with matching PCGComponent,
// or on a named actor if specified.
// ---------------------------------------------------------------------------
FBridgeResult UPCGGraphHandler::Action_ExecuteLocal(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(PCG_DOMAIN, TEXT("execute_local"), 1000, TEXT("'asset_path' is required"));

	FBridgeResult Tmp = CreateResult(PCG_DOMAIN, TEXT("execute_local"));
	UPCGGraph* Graph = LoadPCGGraph(AssetPath, Tmp);
	if (!Graph) return MakeError(PCG_DOMAIN, TEXT("execute_local"), 2000, Tmp.Message);

	FString ActorName;
	Params->TryGetStringField(TEXT("actor_name"), ActorName);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(PCG_DOMAIN, TEXT("execute_local"), 2001, TEXT("No editor world available"));

	int32 ExecutedCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!ActorName.IsEmpty() && !Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
			continue;

		UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
		if (!PCGComp) continue;
		if (PCGComp->GetGraph() != Graph) continue;

		PCGComp->GenerateLocal(false);
		++ExecutedCount;
	}

	if (ExecutedCount == 0)
	{
		return MakeError(PCG_DOMAIN, TEXT("execute_local"), 2002,
			ActorName.IsEmpty()
				? FString::Printf(TEXT("No actors with PCGComponent using graph '%s' found"), *AssetPath)
				: FString::Printf(TEXT("Actor '%s' not found or has no PCGComponent for '%s'"), *ActorName, *AssetPath),
			TEXT("Ensure actors in the level have a PCGComponent assigned to this graph"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("executed_count"), ExecutedCount);

	return MakeSuccess(PCG_DOMAIN, TEXT("execute_local"),
		FString::Printf(TEXT("PCG graph '%s' executed locally on %d actor(s)"), *AssetPath, ExecutedCount),
		Data);
#else
	return MakeError(PCG_DOMAIN, TEXT("execute_local"), 3003, TEXT("Editor only"));
#endif
}
