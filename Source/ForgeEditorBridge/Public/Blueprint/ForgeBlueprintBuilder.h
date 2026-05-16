#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Blueprint/ForgeBlueprintTypes.h"
#include "ForgeBlueprintBuilder.generated.h"

class UBlueprint;
class UWidgetBlueprint;
class UEdGraph;
class UEdGraphNode;
class UWidget;

/**
 * v0.8b+ — Blueprint & Widget Tree Construction API
 *
 * Provides a UFUNCTION-exposed C++ surface for programmatically creating and
 * modifying UBlueprint and UWidgetBlueprint assets from Python (via UE reflection).
 *
 * Python usage pattern:
 *   sub     = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   builder = sub.blueprint_builder
 *
 *   # Load an existing widget blueprint
 *   bp = builder.load_widget_blueprint_by_path("/Game/UI/WBP_HAMBars5")
 *
 *   # Clear the root container's children
 *   builder.clear_widget_children(bp, "RootVBox")
 *
 *   # Add a ProgressBar as a fill-sized child of HealthOverlay
 *   d = unreal.ForgeWidgetDescriptor()
 *   d.widget_type    = "ProgressBar"
 *   d.widget_name    = "HealthBar"
 *   d.parent_widget_name = "HealthOverlay"
 *   d.b_is_variable  = True
 *   d.percent        = 1.0
 *   d.fill_color     = unreal.LinearColor(0.18, 0.8, 0.44, 1.0)
 *   d.slot_h_align   = "Fill"
 *   d.slot_v_align   = "Fill"
 *   builder.add_widget(bp, d)
 *
 *   # Set a pin default on a graph node
 *   builder.set_pin_default_value(bp, "EventGraph", "MyNode", "Tolerance", "0.01")
 *
 *   builder.compile_blueprint(bp)
 *   builder.save_blueprint(bp)
 *
 * NodeRegistry lifetime:
 *   Nodes registered by AddGraphNode persist in memory until ClearNodeRegistry()
 *   is called, or until the editor session ends. Compile does NOT clear the registry,
 *   so you can wire nodes across multiple Python calls.
 *
 * All mutating methods call MarkPackageDirty() + MarkBlueprintAsModified() on success.
 */
UCLASS(BlueprintType)
class FORGEEDITORBRIDGE_API UForgeBlueprintBuilder : public UObject
{
    GENERATED_BODY()

public:

    // =========================================================================
    // Asset Loading
    // =========================================================================

    /**
     * Load an existing UWidgetBlueprint by game asset path.
     * Example: "/Game/UI/WBP_HAMBars5"
     * Returns nullptr on failure. Does NOT create a new asset.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& AssetPath);

    /**
     * Load an existing UBlueprint (actor or object) by game asset path.
     * Example: "/Game/Blueprints/BP_MyActor"
     * Returns nullptr on failure. Also works for UWidgetBlueprint (they inherit UBlueprint).
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    UBlueprint* LoadBlueprintByPath(const FString& AssetPath);


    // =========================================================================
    // Widget Blueprint API — Tree Construction
    // =========================================================================

    /**
     * Create a new UMG WidgetBlueprint at PackagePath.
     * RootDescriptor describes the root widget placed inside the WidgetTree.
     * Returns nullptr on failure.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    UWidgetBlueprint* CreateWidgetBlueprint(const FString& PackagePath,
                                            const FForgeWidgetDescriptor& RootDescriptor);

    /**
     * Add a child widget to an existing WidgetBlueprint.
     *
     * Slot handling by parent type:
     *   UCanvasPanel   → applies AnchorMin/AnchorMax from Descriptor
     *   UHorizontalBox → applies bSlotFill, SlotFillValue, SlotPadding, SlotVAlign
     *   UVerticalBox   → applies bSlotFill, SlotFillValue, SlotPadding, SlotHAlign
     *   UOverlay       → applies SlotHAlign, SlotVAlign, SlotPadding
     *   Other panels   → applies SlotPadding only
     *
     * Widget property shortcuts applied after construction:
     *   TextBlock    → Text field sets initial text
     *   ProgressBar  → Percent and FillColor are applied
     *   Any widget   → bIsVariable marks it as a Blueprint variable
     *
     * Returns true on success.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool AddWidget(UWidgetBlueprint* BP, const FForgeWidgetDescriptor& Descriptor);

    /**
     * Remove all children from the named panel widget inside BP's WidgetTree.
     * The widgets are detached from the panel and moved to the transient package
     * so they are excluded from the next save.
     * Returns true if the parent was found and had children to remove.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool ClearWidgetChildren(UWidgetBlueprint* BP, const FString& ParentWidgetName);

    /**
     * Set a named property on a widget inside BP using reflection.
     *
     * PropertyName is the exact UPROPERTY name on the widget class, e.g.:
     *   "Percent"              → float on UProgressBar
     *   "FillColorAndOpacity"  → FLinearColor on UProgressBar
     *                            PropertyValue format: "(R=0.18,G=0.8,B=0.44,A=1.0)"
     *   "ColorAndOpacity"      → FSlateColor on UTextBlock
     *   "bIsVariable"          → bool (use "true" / "false")
     *   "Visibility"           → ESlateVisibility (use "Visible", "Hidden", "Collapsed")
     *
     * Returns true if the widget was found and the property was successfully imported.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool SetWidgetProperty(UWidgetBlueprint* BP, const FString& WidgetName,
                           const FString& PropertyName, const FString& PropertyValue);


    // =========================================================================
    // Actor Blueprint API — Graph Construction
    // =========================================================================

    /**
     * Create a new Actor Blueprint at PackagePath derived from ParentClass.
     * Returns nullptr on failure.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    UBlueprint* CreateActorBlueprint(const FString& PackagePath,
                                     TSubclassOf<AActor> ParentClass);

    /**
     * Add a node to the named graph of BP.
     * GraphName is typically "EventGraph" or a function name.
     *
     * Descriptor.NodeId must be unique across all AddGraphNode calls in this session;
     * it is used as the lookup key for WireNodes and SetPinDefaultValue.
     *
     * NodeRegistry persists until ClearNodeRegistry() is called — you can safely
     * call AddGraphNode in one Python script and WireNodes in another.
     *
     * Returns true on success.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool AddGraphNode(UBlueprint* BP, const FString& GraphName,
                      const FForgeNodeDescriptor& Descriptor);

    /**
     * Connect two previously-added nodes.
     * Link.FromNodeId / ToNodeId must have been added via AddGraphNode.
     * Returns true on success.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool WireNodes(UBlueprint* BP, const FString& GraphName,
                   const FForgeLinkDescriptor& Link);

    /**
     * Set the default value of an input pin on a previously-added node.
     * NodeId must match the NodeId used in AddGraphNode.
     * PinName is the exact pin label, e.g. "Tolerance", "bSuppressOutputLog".
     * Value is a string formatted for UE's ImportText, e.g. "0.01", "true", "Hello".
     * Returns true if the pin was found and the value was applied.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool SetPinDefaultValue(UBlueprint* BP, const FString& GraphName,
                            const FString& NodeId, const FString& PinName,
                            const FString& Value);

    /**
     * Compile BP and refresh all open editors.
     * Does NOT clear the NodeRegistry — call ClearNodeRegistry() explicitly if desired.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    void CompileBlueprint(UBlueprint* BP);

    /**
     * Save BP's package to disk.
     * Returns true on success.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool SaveBlueprint(UBlueprint* BP);

    /**
     * Add a properly-configured BlueprintImplementableEvent (or BlueprintNativeEvent) override
     * node to the named graph and register it under NodeId for subsequent WireNodes calls.
     *
     * ParentFunctionName is the exact UFUNCTION name on the parent class, e.g. "UpdatePoolUI".
     * The created node will have output exec pin "then" plus one output pin per function parameter
     * named after the parameter (e.g. "Pool", "Current", "EffectiveMax", "Base").
     *
     * Returns true on success.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    bool ImplementEventOverride(UBlueprint* BP, const FString& GraphName,
                                const FString& ParentFunctionName, const FString& NodeId);

    /**
     * Clear the in-memory NodeId → EdGraphNode registry.
     * Call this if you are starting a new graph-building session on a different BP
     * and want to avoid NodeId collisions with previous work.
     * Not required between calls on the same BP.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge|Blueprint")
    void ClearNodeRegistry();


private:

    /**
     * NodeId → weak EdGraphNode pointer.
     * Keyed by NodeId alone — callers must ensure NodeIds are unique across all
     * AddGraphNode calls within a session (or call ClearNodeRegistry between BPs).
     * Weak pointers prevent dangling references if a node is garbage-collected.
     */
    TMap<FString, TWeakObjectPtr<UEdGraphNode>> NodeRegistry;

    // Helper: find or return nullptr an existing graph by name in BP.
    UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName) const;

    // Helper: resolve a widget by name from a WidgetTree (returns UWidget*).
    UWidget* FindWidgetByName(UWidgetBlueprint* BP, const FString& WidgetName) const;

    // Helper: parse "Fill"/"Left"/"Right"/"Center" → EHorizontalAlignment.
    static EHorizontalAlignment ParseHAlign(const FString& S);

    // Helper: parse "Fill"/"Top"/"Bottom"/"Center" → EVerticalAlignment.
    static EVerticalAlignment ParseVAlign(const FString& S);
};
