#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ContextHandler.generated.h"

/**
 * ContextHandler — domain "context"
 *
 * Thin wrapper over BridgeAttentionManager for stateful session context.
 * No UE API calls — pure state management.
 *
 * Supported actions:
 *   get_target_umg_asset          → returns current target asset path
 *   set_target_umg_asset          → asset_path → loads WBP into cache
 *   get_last_edited_umg_asset     → from recent asset history
 *   get_recently_edited_umg_assets → max_count=5 → array of paths
 *   set_target_graph              → graph_name
 *   set_cursor_node               → node_id
 *   get_cursor_node               → returns cursor node id
 *   set_active_widget             → widget_name
 *   get_active_widget             → returns active widget name
 *   set_animation_scope           → animation_name
 *   get_animation_scope           → returns animation scope name
 */
UCLASS()
class FORGEEDITORBRIDGE_API UContextHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("context"); }
    virtual TArray<FString> GetSupportedActions() const override { return { TEXT("set_target_umg_asset"), TEXT("get_target_umg_asset"), TEXT("get_last_edited_umg_asset"), TEXT("get_recently_edited_umg_assets"), TEXT("set_target_graph"), TEXT("set_cursor_node"), TEXT("get_cursor_node"), TEXT("set_active_widget"), TEXT("get_active_widget"), TEXT("set_animation_scope"), TEXT("get_animation_scope") }; }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
