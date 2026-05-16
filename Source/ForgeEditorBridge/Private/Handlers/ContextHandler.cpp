#include "Handlers/ContextHandler.h"
#include "ForgeAISubsystem.h"
#include "Attention/BridgeAttentionManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

FBridgeResult UContextHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("context"), Action);

    UBridgeAttentionManager* Attn = Subsystem ? Subsystem->AttentionManager : nullptr;
    if (!Attn)
    {
        Result.Message = TEXT("AttentionManager not initialized");
        Result.ErrorCode = 3003;
        return Result;
    }

    if (!Params.IsValid())
    {
        Result.Message = TEXT("Params is null");
        Result.ErrorCode = 1000;
        return Result;
    }

    // ---- set_target_umg_asset -----------------------------------------------
    if (Action == TEXT("set_target_umg_asset"))
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("asset_path"), Path) || Path.IsEmpty())
        {
            Result.Message = TEXT("set_target_umg_asset requires: asset_path");
            Result.ErrorCode = 1000;
            return Result;
        }
        Result.bSuccess = Attn->SetTargetAssetPath(Path);
        Result.AffectedPath = Path;
        Result.Message = Result.bSuccess
            ? FString::Printf(TEXT("Target UMG asset set to: %s"), *Path)
            : FString::Printf(TEXT("Failed to load or create asset at: %s"), *Path);
        if (!Result.bSuccess) Result.ErrorCode = 2000;
        return Result;
    }

    // ---- get_target_umg_asset -----------------------------------------------
    if (Action == TEXT("get_target_umg_asset"))
    {
        FString Path = Attn->GetTargetAssetPath();
        Result.bSuccess = !Path.IsEmpty();
        Result.AffectedPath = Path;
        Result.Message = Path.IsEmpty() ? TEXT("No target asset set") : Path;
        return Result;
    }

    // ---- get_last_edited_umg_asset ------------------------------------------
    if (Action == TEXT("get_last_edited_umg_asset"))
    {
        FString Path = Attn->GetLastEditedAsset();
        Result.bSuccess = !Path.IsEmpty();
        Result.Message = Path.IsEmpty() ? TEXT("No history yet") : Path;
        return Result;
    }

    // ---- get_recently_edited_umg_assets -------------------------------------
    if (Action == TEXT("get_recently_edited_umg_assets"))
    {
        int32 MaxCount = 5;
        if (Params->HasField(TEXT("max_count")))
            MaxCount = (int32)Params->GetNumberField(TEXT("max_count"));

        TArray<FString> Recent = Attn->GetRecentAssets(MaxCount);
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& P : Recent)
            Arr.Add(MakeShared<FJsonValueString>(P));

        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("assets"), Arr);

        FString DataStr;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataStr);
        FJsonSerializer::Serialize(DataObj.ToSharedRef(), Writer);

        Result.bSuccess = true;
        Result.ExtraData = DataStr;
        Result.Message = FString::Printf(TEXT("Returned %d recent assets"), Recent.Num());
        return Result;
    }

    // ---- set_target_graph ---------------------------------------------------
    if (Action == TEXT("set_target_graph"))
    {
        FString GraphName;
        if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
        {
            Result.Message = TEXT("set_target_graph requires: graph_name");
            Result.ErrorCode = 1000;
            return Result;
        }
        Attn->SetTargetGraph(GraphName);
        Result.bSuccess = true;
        Result.Message = FString::Printf(TEXT("Graph context set to: %s"), *GraphName);
        return Result;
    }

    // ---- set_cursor_node ----------------------------------------------------
    if (Action == TEXT("set_cursor_node"))
    {
        FString NodeId;
        Params->TryGetStringField(TEXT("node_id"), NodeId);
        Attn->SetCursorNode(NodeId);
        Result.bSuccess = true;
        Result.Message = FString::Printf(TEXT("Cursor node set to: %s"), *NodeId);
        return Result;
    }

    // ---- get_cursor_node ----------------------------------------------------
    if (Action == TEXT("get_cursor_node"))
    {
        FString NodeId = Attn->GetCursorNode();
        Result.bSuccess = true;
        Result.Message = NodeId.IsEmpty() ? TEXT("(none)") : NodeId;
        return Result;
    }

    // ---- set_active_widget --------------------------------------------------
    if (Action == TEXT("set_active_widget"))
    {
        FString WidgetName;
        if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
        {
            Result.Message = TEXT("set_active_widget requires: widget_name");
            Result.ErrorCode = 1000;
            return Result;
        }
        Attn->SetActiveWidget(WidgetName);
        Result.bSuccess = true;
        Result.Message = FString::Printf(TEXT("Active widget set to: %s"), *WidgetName);
        return Result;
    }

    // ---- get_active_widget --------------------------------------------------
    if (Action == TEXT("get_active_widget"))
    {
        Result.bSuccess = true;
        Result.Message = Attn->GetActiveWidget();
        return Result;
    }

    // ---- set_animation_scope ------------------------------------------------
    if (Action == TEXT("set_animation_scope"))
    {
        FString AnimName;
        if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
        {
            Result.Message = TEXT("set_animation_scope requires: animation_name");
            Result.ErrorCode = 1000;
            return Result;
        }
        Attn->SetAnimationScope(AnimName);
        Result.bSuccess = true;
        Result.Message = FString::Printf(TEXT("Animation scope set to: %s"), *AnimName);
        return Result;
    }

    // ---- get_animation_scope ------------------------------------------------
    if (Action == TEXT("get_animation_scope"))
    {
        Result.bSuccess = true;
        Result.Message = Attn->GetAnimationScope();
        return Result;
    }

    // ---- Unknown ------------------------------------------------------------
    Result.Message = FString::Printf(
        TEXT("Unknown context action '%s'. Valid: get/set_target_umg_asset, "
             "get_last/recently_edited_umg_assets, set/get_target_graph, "
             "set/get_cursor_node, set/get_active_widget, set/get_animation_scope"),
        *Action);
    Result.ErrorCode = 1001;
    return Result;
}
