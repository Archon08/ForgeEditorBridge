#include "Handlers/MetaSoundBuilderHandler.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundDocumentInterface.h"
#include "Engine/Engine.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("msbuilder");

namespace
{
    UMetaSoundBuilderSubsystem* GetSub()
    {
        return GEngine ? GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>() : nullptr;
    }

    /** Build a literal from a JSON value + type name. */
    bool BuildLiteralFromJson(const TSharedPtr<FJsonValue>& Value, const FString& TypeName,
                              FMetasoundFrontendLiteral& OutLit, FString& OutErr)
    {
        const FString T = TypeName;
        if (!Value.IsValid())
        {
            OutLit = FMetasoundFrontendLiteral{};
            return true;
        }
        if (T == TEXT("Float"))   { OutLit.Set((float)Value->AsNumber()); return true; }
        if (T == TEXT("Int32"))   { OutLit.Set((int32)Value->AsNumber()); return true; }
        if (T == TEXT("Bool"))    { OutLit.Set(Value->AsBool());          return true; }
        if (T == TEXT("String"))  { OutLit.Set(Value->AsString());        return true; }
        // Audio / Trigger / Time — no defaults; empty literal is fine
        OutLit = FMetasoundFrontendLiteral{};
        return true;
    }

    bool ParseClassName(const TSharedPtr<FJsonObject>& Params, FMetasoundFrontendClassName& OutCN, FString& OutErr)
    {
        FString Namespace, Name, Variant;
        if (!Params->TryGetStringField(TEXT("namespace"), Namespace))
        {
            OutErr = TEXT("'namespace' is required (e.g. 'UE.Math')");
            return false;
        }
        if (!Params->TryGetStringField(TEXT("name"), Name))
        {
            OutErr = TEXT("'name' is required (e.g. 'Add')");
            return false;
        }
        Params->TryGetStringField(TEXT("variant"), Variant);
        if (Variant.IsEmpty())
        {
            OutCN = FMetasoundFrontendClassName(FName(*Namespace), FName(*Name));
        }
        else
        {
            OutCN = FMetasoundFrontendClassName(FName(*Namespace), FName(*Name), FName(*Variant));
        }
        return true;
    }
}

FMetaSoundBuilderSession* UMetaSoundBuilderHandler::GetSession(const FString& BuilderId, const FString& Action, FBridgeResult& OutErr)
{
    if (BuilderId.IsEmpty())
    {
        OutErr = MakeError(DOMAIN, Action, 1000, TEXT("'builder_id' is required"));
        return nullptr;
    }
    FMetaSoundBuilderSession* S = Sessions.Find(BuilderId);
    if (!S || !S->Builder)
    {
        OutErr = MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Builder '%s' not found — call create_patch_builder/create_source_builder first"), *BuilderId));
        return nullptr;
    }
    return S;
}

FBridgeResult UMetaSoundBuilderHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_patch_builder"))      return Action_CreatePatchBuilder(Params);
    if (Action == TEXT("create_source_builder"))     return Action_CreateSourceBuilder(Params);
    if (Action == TEXT("destroy_builder"))           return Action_DestroyBuilder(Params);
    if (Action == TEXT("list_builders"))             return Action_ListBuilders(Params);
    if (Action == TEXT("add_graph_input"))           return Action_AddGraphInput(Params);
    if (Action == TEXT("add_graph_output"))          return Action_AddGraphOutput(Params);
    if (Action == TEXT("list_graph_inputs"))         return Action_ListGraphInputs(Params);
    if (Action == TEXT("list_graph_outputs"))        return Action_ListGraphOutputs(Params);
    if (Action == TEXT("add_node"))                  return Action_AddNode(Params);
    if (Action == TEXT("remove_node"))               return Action_RemoveNode(Params);
    if (Action == TEXT("list_nodes"))                return Action_ListNodes(Params);
    if (Action == TEXT("connect"))                   return Action_Connect(Params);
    if (Action == TEXT("disconnect_input"))          return Action_DisconnectInput(Params);
    if (Action == TEXT("set_node_input_default"))    return Action_SetNodeInputDefault(Params);
    if (Action == TEXT("set_graph_input_default"))   return Action_SetGraphInputDefault(Params);
    if (Action == TEXT("save_to_asset"))             return Action_SaveToAsset(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_patch_builder, create_source_builder, destroy_builder, list_builders, add_graph_input, add_graph_output, list_graph_inputs, list_graph_outputs, add_node, remove_node, list_nodes, connect, disconnect_input, set_node_input_default, set_graph_input_default, save_to_asset"));
}

FBridgeResult UMetaSoundBuilderHandler::Action_CreatePatchBuilder(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId;
    if (!Params->TryGetStringField(TEXT("builder_id"), BuilderId) || BuilderId.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_patch_builder"), 1000, TEXT("'builder_id' is required"));
    if (Sessions.Contains(BuilderId))
        return MakeError(DOMAIN, TEXT("create_patch_builder"), 2002,
            FString::Printf(TEXT("Builder '%s' already exists"), *BuilderId));

    UMetaSoundBuilderSubsystem* Sub = GetSub();
    if (!Sub) return MakeError(DOMAIN, TEXT("create_patch_builder"), 3000, TEXT("UMetaSoundBuilderSubsystem unavailable"));

    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
    UMetaSoundPatchBuilder* Patch = Sub->CreatePatchBuilder(FName(*BuilderId), Result);
    if (Result != EMetaSoundBuilderResult::Succeeded || !Patch)
        return MakeError(DOMAIN, TEXT("create_patch_builder"), 3000, TEXT("CreatePatchBuilder failed"));

    FMetaSoundBuilderSession Session;
    Session.Builder = Patch;
    Session.bIsSource = false;
    Sessions.Add(BuilderId, Session);

    return MakeSuccess(DOMAIN, TEXT("create_patch_builder"),
        FString::Printf(TEXT("Created patch builder '%s'"), *BuilderId));
}

FBridgeResult UMetaSoundBuilderHandler::Action_CreateSourceBuilder(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, Format;
    bool bIsOneShot = true;
    if (!Params->TryGetStringField(TEXT("builder_id"), BuilderId) || BuilderId.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_source_builder"), 1000, TEXT("'builder_id' is required"));
    Params->TryGetStringField(TEXT("format"), Format);
    Params->TryGetBoolField(TEXT("is_one_shot"), bIsOneShot);
    if (Sessions.Contains(BuilderId))
        return MakeError(DOMAIN, TEXT("create_source_builder"), 2002,
            FString::Printf(TEXT("Builder '%s' already exists"), *BuilderId));

    UMetaSoundBuilderSubsystem* Sub = GetSub();
    if (!Sub) return MakeError(DOMAIN, TEXT("create_source_builder"), 3000, TEXT("UMetaSoundBuilderSubsystem unavailable"));

    EMetaSoundOutputAudioFormat Fmt = EMetaSoundOutputAudioFormat::Mono;
    if (Format.ToLower() == TEXT("stereo")) Fmt = EMetaSoundOutputAudioFormat::Stereo;

    FMetaSoundBuilderNodeOutputHandle OnPlay;
    FMetaSoundBuilderNodeInputHandle  OnFinished;
    TArray<FMetaSoundBuilderNodeInputHandle> AudioOuts;
    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;

    UMetaSoundSourceBuilder* Source = Sub->CreateSourceBuilder(
        FName(*BuilderId), OnPlay, OnFinished, AudioOuts, Result, Fmt, bIsOneShot);
    if (Result != EMetaSoundBuilderResult::Succeeded || !Source)
        return MakeError(DOMAIN, TEXT("create_source_builder"), 3000, TEXT("CreateSourceBuilder failed"));

    FMetaSoundBuilderSession Session;
    Session.Builder = Source;
    Session.bIsSource = true;
    Session.GraphInputs.Add(TEXT("_ON_PLAY"), OnPlay);
    Session.GraphOutputs.Add(TEXT("_ON_FINISHED"), OnFinished);
    for (int32 i = 0; i < AudioOuts.Num(); ++i)
    {
        Session.GraphOutputs.Add(FString::Printf(TEXT("_AUDIO_OUT_%d"), i), AudioOuts[i]);
    }
    Sessions.Add(BuilderId, Session);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("format"), Format.IsEmpty() ? TEXT("mono") : Format);
    Data->SetNumberField(TEXT("audio_out_count"), AudioOuts.Num());
    return MakeSuccess(DOMAIN, TEXT("create_source_builder"),
        FString::Printf(TEXT("Created source builder '%s' (audio_outs=%d)"), *BuilderId, AudioOuts.Num()),
        Data);
}

FBridgeResult UMetaSoundBuilderHandler::Action_DestroyBuilder(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (BuilderId.IsEmpty()) return MakeError(DOMAIN, TEXT("destroy_builder"), 1000, TEXT("'builder_id' is required"));
    UMetaSoundBuilderSubsystem* Sub = GetSub();
    if (Sub) Sub->UnregisterBuilder(FName(*BuilderId));
    if (Sessions.Remove(BuilderId) == 0)
        return MakeError(DOMAIN, TEXT("destroy_builder"), 2000, TEXT("Builder not found in handler sessions"));
    return MakeSuccess(DOMAIN, TEXT("destroy_builder"),
        FString::Printf(TEXT("Destroyed builder '%s'"), *BuilderId));
}

FBridgeResult UMetaSoundBuilderHandler::Action_ListBuilders(TSharedPtr<FJsonObject> Params)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, FMetaSoundBuilderSession>& Pair : Sessions)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("builder_id"), Pair.Key);
        Entry->SetBoolField(TEXT("is_source"), Pair.Value.bIsSource);
        Entry->SetNumberField(TEXT("node_count"), Pair.Value.Nodes.Num());
        Entry->SetNumberField(TEXT("graph_inputs"), Pair.Value.GraphInputs.Num());
        Entry->SetNumberField(TEXT("graph_outputs"), Pair.Value.GraphOutputs.Num());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("builders"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_builders"),
        FString::Printf(TEXT("%d builder(s)"), Arr.Num()), Data);
}

FBridgeResult UMetaSoundBuilderHandler::Action_AddGraphInput(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, Name, DataType;
    if (!Params->TryGetStringField(TEXT("builder_id"), BuilderId)) BuilderId = FString();
    if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_graph_input"), 1000, TEXT("'name' is required"));
    if (!Params->TryGetStringField(TEXT("data_type"), DataType) || DataType.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_graph_input"), 1000,
            TEXT("'data_type' is required (Float|Int32|Bool|String|Audio|Trigger|Time)"));
    bool bConstructorInput = false;
    Params->TryGetBoolField(TEXT("constructor"), bConstructorInput);

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("add_graph_input"), Err);
    if (!S) return Err;

    FMetasoundFrontendLiteral Default;
    FString LErr;
    BuildLiteralFromJson(Params->TryGetField(TEXT("default_value")), DataType, Default, LErr);

    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
    FMetaSoundBuilderNodeOutputHandle Handle = S->Builder->AddGraphInputNode(
        FName(*Name), FName(*DataType), Default, Result, bConstructorInput);
    if (Result != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("add_graph_input"), 3000,
            FString::Printf(TEXT("AddGraphInputNode failed for '%s'"), *Name));

    S->GraphInputs.Add(Name, Handle);
    return MakeSuccess(DOMAIN, TEXT("add_graph_input"),
        FString::Printf(TEXT("Added graph input '%s' (%s)"), *Name, *DataType));
}

FBridgeResult UMetaSoundBuilderHandler::Action_AddGraphOutput(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, Name, DataType;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_graph_output"), 1000, TEXT("'name' is required"));
    if (!Params->TryGetStringField(TEXT("data_type"), DataType) || DataType.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_graph_output"), 1000, TEXT("'data_type' is required"));
    bool bConstructorOutput = false;
    Params->TryGetBoolField(TEXT("constructor"), bConstructorOutput);

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("add_graph_output"), Err);
    if (!S) return Err;

    FMetasoundFrontendLiteral Default;
    FString LErr;
    BuildLiteralFromJson(Params->TryGetField(TEXT("default_value")), DataType, Default, LErr);

    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
    FMetaSoundBuilderNodeInputHandle Handle = S->Builder->AddGraphOutputNode(
        FName(*Name), FName(*DataType), Default, Result, bConstructorOutput);
    if (Result != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("add_graph_output"), 3000,
            FString::Printf(TEXT("AddGraphOutputNode failed for '%s'"), *Name));

    S->GraphOutputs.Add(Name, Handle);
    return MakeSuccess(DOMAIN, TEXT("add_graph_output"),
        FString::Printf(TEXT("Added graph output '%s' (%s)"), *Name, *DataType));
}

FBridgeResult UMetaSoundBuilderHandler::Action_ListGraphInputs(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("list_graph_inputs"), Err);
    if (!S) return Err;
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, FMetaSoundBuilderNodeOutputHandle>& Pair : S->GraphInputs)
    {
        Arr.Add(MakeShared<FJsonValueString>(Pair.Key));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("graph_inputs"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_graph_inputs"),
        FString::Printf(TEXT("%d input(s)"), Arr.Num()), Data);
}

FBridgeResult UMetaSoundBuilderHandler::Action_ListGraphOutputs(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("list_graph_outputs"), Err);
    if (!S) return Err;
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, FMetaSoundBuilderNodeInputHandle>& Pair : S->GraphOutputs)
    {
        Arr.Add(MakeShared<FJsonValueString>(Pair.Key));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("graph_outputs"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_graph_outputs"),
        FString::Printf(TEXT("%d output(s)"), Arr.Num()), Data);
}

FBridgeResult UMetaSoundBuilderHandler::Action_AddNode(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, NodeId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_node"), 1000, TEXT("'node_id' is required"));

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("add_node"), Err);
    if (!S) return Err;
    if (S->Nodes.Contains(NodeId))
        return MakeError(DOMAIN, TEXT("add_node"), 2002,
            FString::Printf(TEXT("Node id '%s' already in use"), *NodeId));

    FMetasoundFrontendClassName CN;
    FString CNErr;
    if (!ParseClassName(Params, CN, CNErr))
        return MakeError(DOMAIN, TEXT("add_node"), 1000, CNErr);

    int32 MajorVersion = 1;
    Params->TryGetNumberField(TEXT("major_version"), MajorVersion);

    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
    FMetaSoundNodeHandle Handle = S->Builder->AddNodeByClassName(CN, Result, MajorVersion);
    if (Result != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("add_node"), 3000,
            FString::Printf(TEXT("AddNodeByClassName failed for %s.%s"), *CN.Namespace.ToString(), *CN.Name.ToString()));

    S->Nodes.Add(NodeId, Handle);
    return MakeSuccess(DOMAIN, TEXT("add_node"),
        FString::Printf(TEXT("Added node '%s' (%s.%s)"), *NodeId, *CN.Namespace.ToString(), *CN.Name.ToString()));
}

FBridgeResult UMetaSoundBuilderHandler::Action_RemoveNode(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, NodeId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_node"), 1000, TEXT("'node_id' is required"));

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("remove_node"), Err);
    if (!S) return Err;
    FMetaSoundNodeHandle* Handle = S->Nodes.Find(NodeId);
    if (!Handle) return MakeError(DOMAIN, TEXT("remove_node"), 2000,
        FString::Printf(TEXT("Node '%s' not found"), *NodeId));

    EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
    S->Builder->RemoveNode(*Handle, Result);
    if (Result != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("remove_node"), 3000, TEXT("RemoveNode failed"));

    S->Nodes.Remove(NodeId);
    return MakeSuccess(DOMAIN, TEXT("remove_node"),
        FString::Printf(TEXT("Removed node '%s'"), *NodeId));
}

FBridgeResult UMetaSoundBuilderHandler::Action_ListNodes(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("list_nodes"), Err);
    if (!S) return Err;
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, FMetaSoundNodeHandle>& Pair : S->Nodes)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("node_id"), Pair.Key);
        Entry->SetStringField(TEXT("guid"), Pair.Value.NodeID.ToString());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("nodes"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_nodes"),
        FString::Printf(TEXT("%d node(s)"), Arr.Num()), Data);
}

FBridgeResult UMetaSoundBuilderHandler::Action_Connect(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, FromNodeId, FromPin, ToNodeId, ToPin;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    Params->TryGetStringField(TEXT("from_node_id"), FromNodeId);
    Params->TryGetStringField(TEXT("from_pin_name"), FromPin);
    Params->TryGetStringField(TEXT("to_node_id"), ToNodeId);
    Params->TryGetStringField(TEXT("to_pin_name"), ToPin);
    if (FromPin.IsEmpty() || ToPin.IsEmpty())
        return MakeError(DOMAIN, TEXT("connect"), 1000,
            TEXT("'from_pin_name' and 'to_pin_name' are required"));

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("connect"), Err);
    if (!S) return Err;

    // Resolve "from" — if no node id, treat as graph input
    FMetaSoundBuilderNodeOutputHandle OutHandle;
    if (FromNodeId.IsEmpty())
    {
        FMetaSoundBuilderNodeOutputHandle* Found = S->GraphInputs.Find(FromPin);
        if (!Found) return MakeError(DOMAIN, TEXT("connect"), 2000,
            FString::Printf(TEXT("Graph input '%s' not found"), *FromPin));
        OutHandle = *Found;
    }
    else
    {
        FMetaSoundNodeHandle* Node = S->Nodes.Find(FromNodeId);
        if (!Node) return MakeError(DOMAIN, TEXT("connect"), 2000,
            FString::Printf(TEXT("from_node '%s' not found"), *FromNodeId));
        EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
        OutHandle = S->Builder->FindNodeOutputByName(*Node, FName(*FromPin), R);
        if (R != EMetaSoundBuilderResult::Succeeded)
            return MakeError(DOMAIN, TEXT("connect"), 2000,
                FString::Printf(TEXT("Output pin '%s' not found on node '%s'"), *FromPin, *FromNodeId));
    }

    // Resolve "to" — if no node id, treat as graph output
    FMetaSoundBuilderNodeInputHandle InHandle;
    if (ToNodeId.IsEmpty())
    {
        FMetaSoundBuilderNodeInputHandle* Found = S->GraphOutputs.Find(ToPin);
        if (!Found) return MakeError(DOMAIN, TEXT("connect"), 2000,
            FString::Printf(TEXT("Graph output '%s' not found"), *ToPin));
        InHandle = *Found;
    }
    else
    {
        FMetaSoundNodeHandle* Node = S->Nodes.Find(ToNodeId);
        if (!Node) return MakeError(DOMAIN, TEXT("connect"), 2000,
            FString::Printf(TEXT("to_node '%s' not found"), *ToNodeId));
        EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
        InHandle = S->Builder->FindNodeInputByName(*Node, FName(*ToPin), R);
        if (R != EMetaSoundBuilderResult::Succeeded)
            return MakeError(DOMAIN, TEXT("connect"), 2000,
                FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *ToPin, *ToNodeId));
    }

    EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
    S->Builder->ConnectNodes(OutHandle, InHandle, R);
    if (R != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("connect"), 3000,
            FString::Printf(TEXT("ConnectNodes failed for %s.%s -> %s.%s"),
                *FromNodeId, *FromPin, *ToNodeId, *ToPin));

    return MakeSuccess(DOMAIN, TEXT("connect"),
        FString::Printf(TEXT("Connected %s.%s -> %s.%s"),
            FromNodeId.IsEmpty() ? TEXT("(graph)") : *FromNodeId, *FromPin,
            ToNodeId.IsEmpty()   ? TEXT("(graph)") : *ToNodeId,   *ToPin));
}

FBridgeResult UMetaSoundBuilderHandler::Action_DisconnectInput(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, ToNodeId, ToPin;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    Params->TryGetStringField(TEXT("to_node_id"), ToNodeId);
    if (!Params->TryGetStringField(TEXT("to_pin_name"), ToPin) || ToPin.IsEmpty())
        return MakeError(DOMAIN, TEXT("disconnect_input"), 1000, TEXT("'to_pin_name' is required"));

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("disconnect_input"), Err);
    if (!S) return Err;

    FMetaSoundBuilderNodeInputHandle InHandle;
    if (ToNodeId.IsEmpty())
    {
        FMetaSoundBuilderNodeInputHandle* Found = S->GraphOutputs.Find(ToPin);
        if (!Found) return MakeError(DOMAIN, TEXT("disconnect_input"), 2000,
            FString::Printf(TEXT("Graph output '%s' not found"), *ToPin));
        InHandle = *Found;
    }
    else
    {
        FMetaSoundNodeHandle* Node = S->Nodes.Find(ToNodeId);
        if (!Node) return MakeError(DOMAIN, TEXT("disconnect_input"), 2000,
            FString::Printf(TEXT("to_node '%s' not found"), *ToNodeId));
        EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
        InHandle = S->Builder->FindNodeInputByName(*Node, FName(*ToPin), R);
        if (R != EMetaSoundBuilderResult::Succeeded)
            return MakeError(DOMAIN, TEXT("disconnect_input"), 2000, TEXT("Input pin not found"));
    }

    EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
    S->Builder->DisconnectNodeInput(InHandle, R);
    if (R != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("disconnect_input"), 3000, TEXT("DisconnectNodeInput failed"));
    return MakeSuccess(DOMAIN, TEXT("disconnect_input"),
        FString::Printf(TEXT("Disconnected input %s.%s"),
            ToNodeId.IsEmpty() ? TEXT("(graph)") : *ToNodeId, *ToPin));
}

FBridgeResult UMetaSoundBuilderHandler::Action_SetNodeInputDefault(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, NodeId, InputName, TypeName;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_node_input_default"), 1000, TEXT("'node_id' is required"));
    if (!Params->TryGetStringField(TEXT("input_name"), InputName) || InputName.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_node_input_default"), 1000, TEXT("'input_name' is required"));
    if (!Params->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_node_input_default"), 1000,
            TEXT("'type' is required (Float|Int32|Bool|String)"));

    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("set_node_input_default"), Err);
    if (!S) return Err;
    FMetaSoundNodeHandle* Node = S->Nodes.Find(NodeId);
    if (!Node) return MakeError(DOMAIN, TEXT("set_node_input_default"), 2000, TEXT("Node not found"));

    EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
    FMetaSoundBuilderNodeInputHandle InH = S->Builder->FindNodeInputByName(*Node, FName(*InputName), R);
    if (R != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("set_node_input_default"), 2000,
            FString::Printf(TEXT("Input pin '%s' not found"), *InputName));

    FMetasoundFrontendLiteral Lit;
    FString LErr;
    BuildLiteralFromJson(Params->TryGetField(TEXT("value")), TypeName, Lit, LErr);

    R = EMetaSoundBuilderResult::Failed;
    S->Builder->SetNodeInputDefault(InH, Lit, R);
    if (R != EMetaSoundBuilderResult::Succeeded)
        return MakeError(DOMAIN, TEXT("set_node_input_default"), 3000, TEXT("SetNodeInputDefault failed"));
    return MakeSuccess(DOMAIN, TEXT("set_node_input_default"),
        FString::Printf(TEXT("Set %s.%s default"), *NodeId, *InputName));
}

FBridgeResult UMetaSoundBuilderHandler::Action_SetGraphInputDefault(TSharedPtr<FJsonObject> Params)
{
    // Graph inputs are exposed as node-OUTPUTs to consumers, so the "default"
    // is set by remaking the graph input with a new default. The builder API
    // does not expose mutable defaults on graph IO post-creation; for now we
    // delegate to set_node_input_default if the caller has the consumer side.
    return MakeError(DOMAIN, TEXT("set_graph_input_default"), 3003,
        TEXT("Graph-IO defaults are not mutable post-creation in the 5.7 builder API"),
        TEXT("Recreate the graph input via add_graph_input with the new default; or use set_node_input_default on the consumer node"));
}

FBridgeResult UMetaSoundBuilderHandler::Action_SaveToAsset(TSharedPtr<FJsonObject> Params)
{
    FString BuilderId, AssetPath;
    Params->TryGetStringField(TEXT("builder_id"), BuilderId);
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("save_to_asset"), 1000, TEXT("'asset_path' is required"));
    FBridgeResult Err;
    FMetaSoundBuilderSession* S = GetSession(BuilderId, TEXT("save_to_asset"), Err);
    if (!S) return Err;

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);

    // Build a transient MetaSound, then re-home it into the desired package and save.
    TScriptInterface<IMetaSoundDocumentInterface> DocIface =
        S->Builder->BuildNewMetaSound(FName(*AssetName));
    UObject* Built = DocIface.GetObject();
    if (!Built) return MakeError(DOMAIN, TEXT("save_to_asset"), 3000, TEXT("BuildNewMetaSound returned null"));

    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return MakeError(DOMAIN, TEXT("save_to_asset"), 3000,
        FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
    Package->FullyLoad();

    Built->Rename(*AssetName, Package, REN_DontCreateRedirectors);
    Built->SetFlags(RF_Public | RF_Standalone);
    FAssetRegistryModule::AssetCreated(Built);
    Package->MarkPackageDirty();

    const FString FileName = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_None;
    const bool bSaved = UPackage::SavePackage(Package, Built, *FileName, SaveArgs);
    if (!bSaved) return MakeError(DOMAIN, TEXT("save_to_asset"), 3000,
        FString::Printf(TEXT("SavePackage failed for %s"), *FileName));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("class"), Built->GetClass()->GetName());
    return MakeSuccess(DOMAIN, TEXT("save_to_asset"),
        FString::Printf(TEXT("Saved '%s' as %s"), *AssetPath, *Built->GetClass()->GetName()), Data);
}
