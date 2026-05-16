#pragma once

#include "CoreMinimal.h"
#include "ForgeBlueprintTypes.generated.h"

/**
 * Describes a single widget to place in a UMG WidgetBlueprint.
 * Used by UForgeBlueprintBuilder::CreateWidgetBlueprint and AddWidget.
 *
 * Slot configuration:
 *   Canvas parent  → AnchorMin / AnchorMax are used.
 *   HBox parent    → bSlotFill / SlotFillValue / SlotPadding / SlotVAlign are used.
 *   VBox parent    → bSlotFill / SlotFillValue / SlotPadding / SlotHAlign are used.
 *   Overlay parent → SlotHAlign / SlotVAlign / SlotPadding are used.
 *   Other parents  → SlotPadding only.
 *
 * Widget property shortcuts (applied after ConstructWidget):
 *   Text           → TextBlock.SetText()
 *   Percent        → ProgressBar Percent property
 *   FillColor      → ProgressBar FillColorAndOpacity  (format: see FLinearColor)
 *   bIsVariable    → widget->bIsVariable  (makes it addressable in BP graph)
 *
 * Alignment strings: "Fill" | "Left" | "Right" | "Center" (H), "Fill" | "Top" | "Bottom" | "Center" (V)
 */
USTRUCT(BlueprintType)
struct FORGEEDITORBRIDGE_API FForgeWidgetDescriptor
{
    GENERATED_BODY()

    // ------------------------------------------------------------------
    // Identity
    // ------------------------------------------------------------------

    /** UClass name of the widget, e.g. "TextBlock", "Button", "HorizontalBox", "ProgressBar" */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString WidgetType;

    /** Name for the widget inside the WidgetTree. Leave blank to auto-name. */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString WidgetName;

    /**
     * Name of the parent panel widget to attach to.
     * Empty string = set as WidgetTree root (only when no root exists).
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString ParentWidgetName;

    /** If true, the widget is exposed as a Blueprint variable (accessible in the graph). */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    bool bIsVariable = false;

    // ------------------------------------------------------------------
    // Widget-specific property shortcuts
    // ------------------------------------------------------------------

    /** TextBlock: initial text content. */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString Text;

    /** ProgressBar: fill fraction (0.0-1.0). */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    float Percent = 1.0f;

    /**
     * ProgressBar / Image: fill / tint color (FillColorAndOpacity).
     * Default white = no tint (bar renders its style color unchanged).
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FLinearColor FillColor = FLinearColor::White;

    // ------------------------------------------------------------------
    // Slot configuration — shared across slot types
    // ------------------------------------------------------------------

    /**
     * Padding applied to the slot regardless of parent type.
     * (Left, Top, Right, Bottom) in Slate units.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FMargin SlotPadding;

    /**
     * HBox / VBox: true → ESlateSizeRule::Fill (stretches), false → ESlateSizeRule::Auto.
     * Ignored for Canvas and Overlay parents.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    bool bSlotFill = false;

    /**
     * HBox / VBox: fill weight when bSlotFill=true. Default 1.0.
     * Ignored when bSlotFill=false.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    float SlotFillValue = 1.0f;

    /**
     * Horizontal alignment string for the slot.
     * Valid values: "Fill" (default), "Left", "Right", "Center"
     * Applied to: Overlay slots (always), VBox slots, Canvas slots.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString SlotHAlign = TEXT("Fill");

    /**
     * Vertical alignment string for the slot.
     * Valid values: "Fill" (default), "Top", "Bottom", "Center"
     * Applied to: Overlay slots (always), HBox slots, Canvas slots.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString SlotVAlign = TEXT("Fill");

    // ------------------------------------------------------------------
    // Canvas-specific slot configuration
    // ------------------------------------------------------------------

    /** Normalised anchor min (Canvas panels). X/Y in [0,1]. */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FVector2D AnchorMin = FVector2D(0.f, 0.f);

    /** Normalised anchor max (Canvas panels). X/Y in [0,1]. */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FVector2D AnchorMax = FVector2D(1.f, 1.f);

    FForgeWidgetDescriptor()
        : AnchorMin(0.f, 0.f)
        , AnchorMax(1.f, 1.f)
    {}
};


/**
 * Describes a graph node to add to a Blueprint event graph.
 * Used by UForgeBlueprintBuilder::AddGraphNode.
 *
 * Supported NodeType values and their extra fields:
 *   "K2Node_CallFunction"  → FunctionName ("ClassName::FuncName")
 *   "K2Node_VariableGet"   → MemberName
 *   "K2Node_VariableSet"   → MemberName
 *   "K2Node_SwitchEnum"    → EnumName (e.g. "EHAMPool")
 *   "K2Node_IfThenElse"    → (no extra fields)
 *   "K2Node_Event"         → FunctionName (event name, e.g. "ReceiveBeginPlay")
 */
USTRUCT(BlueprintType)
struct FORGEEDITORBRIDGE_API FForgeNodeDescriptor
{
    GENERATED_BODY()

    /**
     * Node class name without the U prefix, e.g.:
     *   "K2Node_CallFunction"   → UK2Node_CallFunction
     *   "K2Node_VariableGet"    → UK2Node_VariableGet
     *   "K2Node_SwitchEnum"     → UK2Node_SwitchEnum
     *   "K2Node_IfThenElse"     → UK2Node_IfThenElse
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString NodeType;

    /** 2-D position in the graph canvas (pixels). */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FVector2D Position = FVector2D(0.f, 0.f);

    /**
     * Fully-qualified function name for K2Node_CallFunction / K2Node_Event.
     * Format: "ClassName::FunctionName", e.g. "KismetSystemLibrary::PrintString"
     * For events: "ReceiveBeginPlay", "UpdatePoolUI", etc.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString FunctionName;

    /**
     * Member variable name for K2Node_VariableGet / K2Node_VariableSet.
     * Leave empty when not applicable.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString MemberName;

    /**
     * Enum type name for K2Node_SwitchEnum.
     * Example: "EHAMPool", "ECombatState"
     * The enum must be discoverable via FindFirstObject<UEnum>.
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString EnumName;

    /**
     * Logical identifier used when referencing this node from FForgeLinkDescriptor
     * or SetPinDefaultValue. Must be unique within the session (not just the graph).
     */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString NodeId;

    FForgeNodeDescriptor()
        : Position(0.f, 0.f)
    {}
};


/**
 * Describes a pin-to-pin connection between two nodes.
 * Used by UForgeBlueprintBuilder::WireNodes.
 */
USTRUCT(BlueprintType)
struct FORGEEDITORBRIDGE_API FForgeLinkDescriptor
{
    GENERATED_BODY()

    /** NodeId of the source node (must have been added via AddGraphNode). */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString FromNodeId;

    /** Pin name on the source node, e.g. "then", "ReturnValue". */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString FromPinName;

    /** NodeId of the target node. */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString ToNodeId;

    /** Pin name on the target node, e.g. "execute", "NewValue". */
    UPROPERTY(BlueprintReadWrite, Category = "Forge|Blueprint")
    FString ToPinName;
};
