#include "Capture/ForgeUMGCapture.h"
#include "IO/ForgeContextWriter.h"

// --- UMG Editor ---
#include "WidgetBlueprint.h"

// --- UMG Animations ---
#include "Animation/WidgetAnimation.h"

// --- UMG Runtime widget types ---
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/SpinBox.h"

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    // In UE 5.7, FDelegateEditorBinding::PropertyName stores the delegate field
    // name — NOT the user-facing property name. Mapping:
    //   TextBlock.Text         -> "TextDelegate"
    //   Widget.bIsEnabled      -> "bIsEnabledDelegate"
    //   ProgressBar.Percent    -> "PercentDelegate"
    //   Image.Brush            -> "BrushDelegate"
    // Build the lookup set once per widget blueprint, then query by
    // "WidgetName.DelegatePropertyName".
    TSet<FString> BuildBoundPropertySet(const TArray<FDelegateEditorBinding>& Bindings)
    {
        TSet<FString> Out;
        Out.Reserve(Bindings.Num());
        for (const FDelegateEditorBinding& B : Bindings)
        {
            Out.Add(B.ObjectName + TEXT(".") + B.PropertyName.ToString());
        }
        return Out;
    }

    FString SlateVisibilityToString(ESlateVisibility V)
    {
        switch (V)
        {
            case ESlateVisibility::Visible:             return TEXT("Visible");
            case ESlateVisibility::Collapsed:           return TEXT("Collapsed");
            case ESlateVisibility::Hidden:              return TEXT("Hidden");
            case ESlateVisibility::HitTestInvisible:    return TEXT("HitTestInvisible");
            case ESlateVisibility::SelfHitTestInvisible:return TEXT("SelfHitTestInvisible");
            default:                                    return TEXT("Unknown");
        }
    }

    // Walk the parent chain to compute nesting depth (0 = no parent = root)
    int32 GetWidgetDepth(UWidget* W)
    {
        int32 Depth = 0;
        UPanelWidget* Parent = W->GetParent();
        while (Parent && Depth < 32)
        {
            ++Depth;
            Parent = Parent->GetParent();
        }
        return Depth;
    }

    // Interactive touch targets — all widget types the user can interact with
    bool IsInteractiveWidget(UWidget* W)
    {
        if (W->IsA<UButton>()
         || W->IsA<UCheckBox>()
         || W->IsA<USlider>()
         || W->IsA<UEditableText>()
         || W->IsA<UEditableTextBox>()
         || W->IsA<UComboBoxString>()
         || W->IsA<USpinBox>())
        {
            return true;
        }
        // MultiLineEditableText / MultiLineEditableTextBox — check by class name
        // (avoids an extra include for an uncommon type)
        const FString ClassName = W->GetClass()->GetName();
        return ClassName.Contains(TEXT("MultiLineEditable"));
    }

    // List containers that require an explicit empty-state widget
    bool IsListContainer(UWidget* W)
    {
        const FString ClassName = W->GetClass()->GetName();
        return ClassName.Contains(TEXT("ListView"))
            || ClassName.Contains(TEXT("TileView"))
            || ClassName.Contains(TEXT("TreeView"));
    }

    // Heuristic: does this widget name suggest it is an empty-state indicator?
    bool IsEmptyStateWidget(const FString& WidgetName)
    {
        const FString Lower = WidgetName.ToLower();
        return Lower.Contains(TEXT("empty"))
            || Lower.Contains(TEXT("nodata"))
            || Lower.Contains(TEXT("no_data"))
            || Lower.Contains(TEXT("placeholder"))
            || Lower.Contains(TEXT("noitem"))
            || Lower.Contains(TEXT("no_item"))
            || Lower.Contains(TEXT("fallback"));
    }

    TSharedPtr<FJsonValue> Vec2ToJson(const FVector2D& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        return MakeShared<FJsonValueArray>(Arr);
    }
} // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeUMGCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("ui")));
}

// ---------------------------------------------------------------------------
// ExportWidget
// ---------------------------------------------------------------------------

bool UForgeUMGCapture::ExportWidget(const FString& AssetPath)
{
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(
        StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *AssetPath));
    if (!WBP)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeUMGCapture: Failed to load WidgetBlueprint '%s'"), *AssetPath);
        return false;
    }

    if (!WBP->WidgetTree)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeUMGCapture: WidgetBlueprint '%s' has no WidgetTree"), *AssetPath);
        return false;
    }

    // -----------------------------------------------------------------------
    // Property binding lookup (delegate property names — see note at top)
    // "WidgetName.DelegatePropertyName" -> present in set means it's bound
    // -----------------------------------------------------------------------
    const TSet<FString> BoundProps = BuildBoundPropertySet(WBP->Bindings);

    // Helper: is a specific delegate bound for a named widget?
    auto IsBound = [&](const FString& WidgetName, const TCHAR* DelegatePropName) -> bool
    {
        return BoundProps.Contains(WidgetName + TEXT(".") + DelegatePropName);
    };

    // -----------------------------------------------------------------------
    // Animations
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> AnimArr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (!Anim) continue;
        TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
        // Use DisplayLabel if set (editor friendly name), fall back to object name
        const FString AnimName = Anim->GetDisplayLabel().IsEmpty()
            ? Anim->GetFName().ToString()
            : Anim->GetDisplayLabel();
        A->SetStringField(TEXT("name"),           AnimName);
        A->SetNumberField(TEXT("start_time"),      Anim->GetStartTime());
        A->SetNumberField(TEXT("end_time"),        Anim->GetEndTime());
        A->SetNumberField(TEXT("binding_count"),   Anim->GetBindings().Num());
        AnimArr.Add(MakeShared<FJsonValueObject>(A));
    }

    // -----------------------------------------------------------------------
    // Collect all widgets
    // -----------------------------------------------------------------------
    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);
    const int32 TotalWidgets = AllWidgets.Num();

    // Pre-scan for list container / empty-state checks
    bool bHasListContainer    = false;
    bool bHasEmptyStateWidget = false;
    for (UWidget* W : AllWidgets)
    {
        if (IsListContainer(W))              bHasListContainer    = true;
        if (IsEmptyStateWidget(W->GetName())) bHasEmptyStateWidget = true;
    }

    // -----------------------------------------------------------------------
    // Per-widget entries
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> WidgetsArr;

    for (UWidget* Widget : AllWidgets)
    {
        if (!Widget) continue;
        const FString WidgetName = Widget->GetName();

        TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
        W->SetStringField(TEXT("name"),       WidgetName);
        W->SetStringField(TEXT("class_type"), Widget->GetClass()->GetName());
        W->SetBoolField  (TEXT("is_variable"),Widget->bIsVariable);
        W->SetStringField(TEXT("visibility"), SlateVisibilityToString(Widget->GetVisibility()));

        UPanelWidget* ParentPanel = Widget->GetParent();
        W->SetStringField(TEXT("parent"), ParentPanel ? ParentPanel->GetName() : TEXT(""));

        // Canvas slot — position, size, anchors, z-order
        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Widget->Slot))
        {
            TSharedRef<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            const FAnchors A = CS->GetAnchors();
            SlotObj->SetField(TEXT("anchor_min"), Vec2ToJson(A.Minimum));
            SlotObj->SetField(TEXT("anchor_max"), Vec2ToJson(A.Maximum));
            SlotObj->SetField(TEXT("position"),   Vec2ToJson(CS->GetPosition()));
            SlotObj->SetField(TEXT("size"),        Vec2ToJson(CS->GetSize()));
            SlotObj->SetNumberField(TEXT("z_order"), CS->GetZOrder());
            SlotObj->SetField(TEXT("alignment"),   Vec2ToJson(CS->GetAlignment()));
            W->SetObjectField(TEXT("canvas_slot"), SlotObj);
        }

        // TextBlock — text value, binding state (delegate: "TextDelegate")
        if (UTextBlock* TB = Cast<UTextBlock>(Widget))
        {
            FString TextStr = TB->GetText().ToString();
            if (TextStr.Len() > 64) { TextStr = TextStr.Left(61) + TEXT("..."); }
            W->SetStringField(TEXT("text_value"),    TextStr);
            W->SetBoolField  (TEXT("text_is_empty"), TB->GetText().IsEmpty());
            W->SetBoolField  (TEXT("has_text_binding"), IsBound(WidgetName, TEXT("TextDelegate")));
        }

        // Image — brush resource path and binding (delegate: "BrushDelegate")
        if (UImage* Img = Cast<UImage>(Widget))
        {
            const FSlateBrush& Brush = Img->GetBrush();
            FString ResourcePath;
            FString ResourceType = TEXT("none");
            if (UObject* Res = Brush.GetResourceObject())
            {
                ResourcePath = Res->GetPathName();
                ResourceType = Res->GetClass()->GetName(); // Texture2D, Material, etc.
            }
            W->SetStringField(TEXT("brush_resource_path"), ResourcePath);
            W->SetStringField(TEXT("brush_resource_type"), ResourceType);
            W->SetBoolField  (TEXT("has_brush_binding"), IsBound(WidgetName, TEXT("BrushDelegate")));
        }

        // ProgressBar — percent value and binding (delegate: "PercentDelegate")
        if (UProgressBar* PB = Cast<UProgressBar>(Widget))
        {
            W->SetNumberField(TEXT("percent"),             PB->GetPercent());
            W->SetBoolField  (TEXT("has_percent_binding"), IsBound(WidgetName, TEXT("PercentDelegate")));
        }

        // Button — enabled-state binding (delegate: "bIsEnabledDelegate")
        if (Widget->IsA<UButton>())
        {
            W->SetBoolField(TEXT("has_enabled_binding"), IsBound(WidgetName, TEXT("bIsEnabledDelegate")));
        }

        WidgetsArr.Add(MakeShared<FJsonValueObject>(W));
    }

    // -----------------------------------------------------------------------
    // Audit pass
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> Issues;

    // HIGH_WIDGET_COUNT — Slate per-widget invalidation overhead
    const int32 WidgetCountThreshold = 100;
    if (TotalWidgets > WidgetCountThreshold)
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("HIGH_WIDGET_COUNT"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("widget_name"),TEXT(""));
        Issue->SetStringField(TEXT("detail"),
            FString::Printf(
                TEXT("Widget blueprint has %d widgets (threshold: %d). Slate invalidation "
                     "overhead scales with widget count. Consider UListView for repeated "
                     "items, or splitting into sub-widgets with Collapsed visibility."),
                TotalWidgets, WidgetCountThreshold));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // SCROLL_NO_EMPTY_STATE — list/tile/tree view without empty-state indicator
    if (bHasListContainer && !bHasEmptyStateWidget)
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("SCROLL_NO_EMPTY_STATE"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("widget_name"),TEXT(""));
        Issue->SetStringField(TEXT("detail"),
            TEXT("Blueprint contains a ListView, TileView, or TreeView but no widget "
                 "named to suggest an empty state (e.g. 'Empty', 'NoData', 'Placeholder'). "
                 "Add a sibling widget to display when the list has zero items."));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // Per-widget rules
    for (UWidget* Widget : AllWidgets)
    {
        if (!Widget) continue;
        const FString WidgetName = Widget->GetName();

        // --- TextBlock rules ---
        if (UTextBlock* TB = Cast<UTextBlock>(Widget))
        {
            const bool bHasTextBinding = IsBound(WidgetName, TEXT("TextDelegate"));

            // HARDCODED_TEXT — literal text present with no binding
            if (!TB->GetText().IsEmpty() && !bHasTextBinding)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("HARDCODED_TEXT"));
                Issue->SetStringField(TEXT("severity"),   TEXT("info"));
                Issue->SetStringField(TEXT("widget_name"),WidgetName);
                FString Preview = TB->GetText().ToString();
                if (Preview.Len() > 32) Preview = Preview.Left(29) + TEXT("...");
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("TextBlock '%s' has literal text \"%s\" with no TextDelegate binding. "
                             "Hardcoded strings cannot be localized. Use NSLOCTEXT/LOCTABLE "
                             "or add a binding function to update text at runtime."),
                        *WidgetName, *Preview));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }

            // MISSING_BINDING — variable TextBlock with no text binding
            if (Widget->bIsVariable && !bHasTextBinding)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("MISSING_BINDING"));
                Issue->SetStringField(TEXT("severity"),   TEXT("info"));
                Issue->SetStringField(TEXT("widget_name"),WidgetName);
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("TextBlock '%s' is exposed as a variable but has no TextDelegate "
                             "binding. If text changes at runtime, add a binding function. "
                             "If it never changes, remove the variable flag."),
                        *WidgetName));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }

        // --- ProgressBar rules ---
        if (UProgressBar* PB = Cast<UProgressBar>(Widget))
        {
            if (!IsBound(WidgetName, TEXT("PercentDelegate")))
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("PROGRESS_NO_BINDING"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("widget_name"),WidgetName);
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("ProgressBar '%s' has no PercentDelegate binding. A static "
                             "progress bar provides no runtime feedback. Add a binding "
                             "function or drive Percent from code."),
                        *WidgetName));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }

        // --- Button rules ---
        if (Widget->IsA<UButton>())
        {
            // MISSING_DISABLED_STATE — button that can never appear disabled
            if (!IsBound(WidgetName, TEXT("bIsEnabledDelegate")))
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("MISSING_DISABLED_STATE"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("widget_name"),WidgetName);
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Button '%s' has no bIsEnabledDelegate binding. Without a disabled "
                             "state, the button gives no feedback when the action is unavailable — "
                             "a UX regression on mobile. Add a binding or call SetIsEnabled from code."),
                        *WidgetName));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }

        // --- TOUCH_TARGET_SMALL — interactive widgets under 44x44px ---
        if (IsInteractiveWidget(Widget))
        {
            if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Widget->Slot))
            {
                const FVector2D Size = CS->GetSize();
                const float MinTouch = 44.f;
                if (Size.X < MinTouch || Size.Y < MinTouch)
                {
                    TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                    Issue->SetStringField(TEXT("issue_type"), TEXT("TOUCH_TARGET_SMALL"));
                    Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                    Issue->SetStringField(TEXT("widget_name"),WidgetName);
                    Issue->SetStringField(TEXT("detail"),
                        FString::Printf(
                            TEXT("Interactive widget '%s' (%s) has canvas size %.0fx%.0f px, "
                                 "below the 44x44 minimum touch target. "
                                 "Increase size or add invisible padding around the widget."),
                            *WidgetName, *Widget->GetClass()->GetName(),
                            Size.X, Size.Y));
                    Issues.Add(MakeShared<FJsonValueObject>(Issue));
                }
            }
        }

        // --- WRONG_ANCHOR — large offset but corner anchor ---
        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Widget->Slot))
        {
            const FAnchors  Anchors = CS->GetAnchors();
            const FVector2D Pos     = CS->GetPosition();
            // IsNearlyEqual was removed from TVector2 in UE5.5 — use Equals() instead
            const bool bCornerAnchor =
                Anchors.Minimum.Equals(FVector2D::ZeroVector) &&
                Anchors.Maximum.Equals(FVector2D::ZeroVector);
            const float OffsetThreshold = 400.f;
            if (bCornerAnchor &&
                (FMath::Abs(Pos.X) > OffsetThreshold || FMath::Abs(Pos.Y) > OffsetThreshold))
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("WRONG_ANCHOR"));
                Issue->SetStringField(TEXT("severity"),   TEXT("info"));
                Issue->SetStringField(TEXT("widget_name"),WidgetName);
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Widget '%s' is at position (%.0f, %.0f) but anchored to the "
                             "top-left corner. At this offset the widget may be off-screen "
                             "on smaller viewports. Anchor to the nearest edge or center."),
                        *WidgetName, Pos.X, Pos.Y));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }

        // --- DEEP_HIERARCHY — Slate adds a layout pass per level ---
        const int32 DepthThreshold = 5;
        const int32 Depth = GetWidgetDepth(Widget);
        if (Depth > DepthThreshold)
        {
            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("issue_type"), TEXT("DEEP_HIERARCHY"));
            Issue->SetStringField(TEXT("severity"),   TEXT("info"));
            Issue->SetStringField(TEXT("widget_name"),WidgetName);
            Issue->SetStringField(TEXT("detail"),
                FString::Printf(
                    TEXT("Widget '%s' is nested %d levels deep (threshold: %d). "
                         "Deep hierarchies add Slate layout and paint passes at each level. "
                         "Flatten the hierarchy or extract deep branches into sub-widget blueprints."),
                    *WidgetName, Depth, DepthThreshold));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }
    }

    // -----------------------------------------------------------------------
    // Assemble root
    // -----------------------------------------------------------------------
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),    FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("asset_path"),   AssetPath);
    Root->SetNumberField(TEXT("widget_count"), TotalWidgets);
    Root->SetNumberField(TEXT("anim_count"),   AnimArr.Num());
    Root->SetArrayField (TEXT("animations"),   AnimArr);
    Root->SetArrayField (TEXT("widgets"),      WidgetsArr);

    TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
    AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
    AuditObj->SetArrayField (TEXT("issues"),       Issues);
    Root->SetObjectField(TEXT("audit"), AuditObj);

    const FString WidgetBaseName = FPaths::GetBaseFilename(AssetPath);
    const bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("ui"), WidgetBaseName, Root);

    if (bOK)
    {
        UE_LOG(LogTemp, Log,
            TEXT("ForgeUMGCapture: Exported '%s' — %d widgets, %d anims, %d issues"),
            *AssetPath, TotalWidgets, AnimArr.Num(), Issues.Num());
    }
    return bOK;
}

// ---------------------------------------------------------------------------
// ExportWidgetsByPattern
// ---------------------------------------------------------------------------

int32 UForgeUMGCapture::ExportWidgetsByPattern(const FString& NamePattern)
{
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(TEXT("/Game"));
    Filter.bRecursivePaths   = true;
    Filter.bRecursiveClasses = false;

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    int32 Exported = 0;
    for (const FAssetData& Asset : Assets)
    {
        const FString AssetName = Asset.AssetName.ToString();
        if (!NamePattern.IsEmpty() &&
            !AssetName.Contains(NamePattern, ESearchCase::IgnoreCase))
        {
            continue;
        }
        if (ExportWidget(Asset.GetObjectPathString()))
        {
            ++Exported;
        }
    }

    UpdateIndexFile(Exported);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeUMGCapture: Exported %d widget blueprints (pattern='%s')"),
        Exported, *NamePattern);
    return Exported;
}

// ---------------------------------------------------------------------------
// AuditWidget
// ---------------------------------------------------------------------------

int32 UForgeUMGCapture::AuditWidget(const FString& AssetPath)
{
    // ExportWidget writes the full output including audit. Return 0 on success,
    // -1 on failure. Caller reads the audit section from the written JSON.
    return ExportWidget(AssetPath) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeUMGCapture::UpdateIndexFile(int32 WidgetCount)
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
    {
        if (Found->IsValid() && (*Found)->Type == EJson::Object)
        {
            Captures = (*Found)->AsObject();
        }
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> UMGSection = MakeShared<FJsonObject>();
    UMGSection->SetStringField(TEXT("directory"),    TEXT("ui/"));
    UMGSection->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    if (WidgetCount >= 0)
    {
        UMGSection->SetNumberField(TEXT("widget_count"), WidgetCount);
    }
    Captures->SetObjectField(TEXT("umg"), UMGSection);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
