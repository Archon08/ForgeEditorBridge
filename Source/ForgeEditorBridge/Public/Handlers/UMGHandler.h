#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "UMGHandler.generated.h"

class UWidgetBlueprint;
class UWidget;

/**
 * UMGHandler — domain "umg"
 *
 * Full UMG widget read/write.  Ported from winyunq/UnrealMotionGraphicsMCP (MIT).
 *
 * Read actions:
 *   get_widget_tree          → full JSON hierarchy of target WBP
 *   query_widget_properties  → widget_name, properties[] → key/value map
 *   get_widget_schema        → widget_type → all settable properties
 *   get_layout_data          → bounding boxes of all widgets
 *
 * Write actions:
 *   create_widget            → widget_type, new_widget_name, parent_name?
 *   set_widget_properties    → widget_name, properties{} (camelCase or PascalCase)
 *   reparent_widget          → widget_name, new_parent_name
 *   set_active_widget        → widget_name (updates attention scope)
 *   save_asset               → saves current target WBP
 *
 * WBP resolution order: params["asset_path"] → AttentionManager cache
 */
UCLASS()
class FORGEEDITORBRIDGE_API UUMGHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("umg"); }
    virtual TArray<FString> GetSupportedActions() const override { return { TEXT("get_widget_tree"), TEXT("query_widget_properties"), TEXT("get_widget_schema"), TEXT("get_layout_data"), TEXT("create_widget"), TEXT("set_widget_properties"), TEXT("reparent_widget"), TEXT("set_active_widget"), TEXT("save_asset"), TEXT("read_umg_capture"), TEXT("audit_umg_widget"), TEXT("create_widget_blueprint"), TEXT("add_widget"), TEXT("set_binding"), TEXT("set_anchors"), TEXT("remove_widget"), TEXT("set_widget_property"), TEXT("list_widgets"), TEXT("create_editor_utility_widget"), TEXT("add_widget_animation"), TEXT("list_widget_animations"), TEXT("remove_widget_animation"), TEXT("bind_widget_event"), TEXT("add_named_slot"), TEXT("add_widget_anim_binding"), TEXT("list_widget_anim_bindings"), TEXT("add_widget_anim_track"), TEXT("remove_widget_anim_track"), TEXT("list_widget_anim_tracks"), TEXT("add_widget_anim_keyframe_float"), TEXT("unbind_widget_event"), TEXT("list_widget_event_bindings") }; }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    // ---- Read ---------------------------------------------------------------
    FBridgeResult Action_GetWidgetTree        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_QueryWidgetProperties(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetWidgetSchema      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetLayoutData        (TSharedPtr<FJsonObject> Params);

    // ---- Capture read -------------------------------------------------------
    FBridgeResult Action_ReadUMGCapture       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AuditUMGWidget       (TSharedPtr<FJsonObject> Params);

    // ---- Write --------------------------------------------------------------
    FBridgeResult Action_CreateWidget         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetWidgetProperties  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ReparentWidget       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetActiveWidget      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SaveAsset            (TSharedPtr<FJsonObject> Params);

    // ---- Phase 1e additions ------------------------------------------------
    FBridgeResult Action_CreateWidgetBlueprint(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddWidget            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetBinding           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetAnchors           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveWidget         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetWidgetProperty    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListWidgets          (TSharedPtr<FJsonObject> Params);

    // ---- Wave 8: Editor Utility Widget + animations + bindings -------------
    FBridgeResult Action_CreateEditorUtilityWidget (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddWidgetAnimation        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListWidgetAnimations      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveWidgetAnimation     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_BindWidgetEvent           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddNamedSlot              (TSharedPtr<FJsonObject> Params);

    // ---- Wave 9: MovieScene track/keyframe authoring + event binding -------
    FBridgeResult Action_AddWidgetAnimBinding      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListWidgetAnimBindings    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddWidgetAnimTrack        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveWidgetAnimTrack     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListWidgetAnimTracks      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddWidgetAnimKeyframeFloat(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_UnbindWidgetEvent         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListWidgetEventBindings   (TSharedPtr<FJsonObject> Params);

    // ---- Helpers ------------------------------------------------------------
    UWidgetBlueprint* ResolveWBP(TSharedPtr<FJsonObject> Params, FString& OutError) const;
    TSharedPtr<FJsonObject> NormalizePascalCase(const TSharedPtr<FJsonObject>& In) const;
};
