#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeUMGCapture.generated.h"

/**
 * v1.2 — UMG Widget Capture + Audit
 *
 * Exports UWidgetBlueprint asset configurations as structured JSON, including
 * the full widget tree hierarchy, slot data, property bindings, and a set of
 * UX audit rules optimized for mobile/touch contexts.
 *
 * Output directory: {ProjectRoot}/Forge/ue-context/ui/
 *
 * Per-widget files:   ui/{WidgetName}.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   uc = sub.umg_capture
 *   uc.export_widget("/Game/UI/WBP_HUD")
 *   uc.export_widgets_by_pattern("WBP_")
 *
 * Per-widget JSON fields:
 *   asset_path, widget_count, widgets[]{
 *     name, class_type, is_variable, visibility, parent,
 *     canvas_slot{anchor_min, anchor_max, position, size, z_order, alignment},
 *     text_value, text_is_empty, has_text_binding  (TextBlock only),
 *     has_enabled_binding                           (Button only)
 *   },
 *   audit{total_issues, issues[]{issue_type, severity, detail, widget_name}}
 *
 * Audit rules (8):
 *   HARDCODED_TEXT        — TextBlock has non-empty literal text and no Text binding
 *   MISSING_BINDING       — TextBlock is_variable but has no Text binding
 *   TOUCH_TARGET_SMALL    — interactive widget in CanvasPanel slot < 44x44 px
 *   MISSING_DISABLED_STATE— Button without an IsEnabled property binding
 *   WRONG_ANCHOR          — widget offset > 400px from origin but anchored to (0,0)/(0,0)
 *   SCROLL_NO_EMPTY_STATE — ListView/TileView/TreeView with no apparent empty-state widget
 *   HIGH_WIDGET_COUNT     — widget blueprint has more than 100 widgets (Slate overhead)
 *   DEEP_HIERARCHY        — any widget is nested more than 5 levels deep
 *
 * Note: event bindings (OnClicked, OnHovered, etc.) are graph-level and are not
 * captured here. Only property bindings from UWidgetBlueprint::Bindings are reflected.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeUMGCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Export a single UWidgetBlueprint by asset path (e.g. "/Game/UI/WBP_HUD").
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportWidget(const FString& AssetPath);

    /**
     * Export all UWidgetBlueprint assets whose name contains NamePattern (case-insensitive).
     * Pass an empty string to export all widget blueprints under /Game/.
     * Returns the number of widgets successfully exported.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportWidgetsByPattern(const FString& NamePattern);

    /**
     * Run the audit pass on a single widget blueprint and return the issue count.
     * Writes the same output file as ExportWidget — this is a convenience alias
     * for focusing attention on the audit section.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 AuditWidget(const FString& AssetPath);

private:
    FString OutputDir;

    void UpdateIndexFile(int32 WidgetCount);
};
