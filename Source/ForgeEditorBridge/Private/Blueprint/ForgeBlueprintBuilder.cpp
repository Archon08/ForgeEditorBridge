#include "Blueprint/ForgeBlueprintBuilder.h"

// --- UMG / Widget ---
#include "WidgetBlueprint.h"              // UWidgetBlueprint
#include "WidgetBlueprintFactory.h"       // UWidgetBlueprintFactory (UMGEditor)
#include "Blueprint/WidgetTree.h"         // UWidgetTree
#include "Components/CanvasPanel.h"       // UCanvasPanel
#include "Components/CanvasPanelSlot.h"   // UCanvasPanelSlot
#include "Components/PanelWidget.h"       // UPanelWidget (AddChild)
#include "Components/HorizontalBox.h"     // UHorizontalBox
#include "Components/HorizontalBoxSlot.h" // UHorizontalBoxSlot
#include "Components/VerticalBox.h"       // UVerticalBox
#include "Components/VerticalBoxSlot.h"   // UVerticalBoxSlot
#include "Components/Overlay.h"           // UOverlay
#include "Components/OverlaySlot.h"       // UOverlaySlot
#include "Components/ProgressBar.h"       // UProgressBar
#include "Components/TextBlock.h"         // UTextBlock

// --- Blueprint graph ---
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"             // UEdGraphSchema_K2, TryCreateConnection
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_SwitchEnum.h"            // UK2Node_SwitchEnum
#include "K2Node_IfThenElse.h"            // UK2Node_IfThenElse

// --- Editor utilities ---
#include "Kismet2/KismetEditorUtilities.h"     // FKismetEditorUtilities
#include "Kismet2/BlueprintEditorUtils.h"      // FBlueprintEditorUtils
#include "KismetCompilerModule.h"
#include "AssetRegistry/AssetRegistryModule.h"

// --- Asset creation helpers ---
#include "AssetToolsModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "UObject/SavePackage.h"          // FSavePackageArgs, FSavePackageResultStruct

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
    UPackage* GetOrCreatePackage(const FString& PackagePath)
    {
        FString PackageName = PackagePath;
        if (PackageName.EndsWith(TEXT("/")))
        {
            PackageName = PackageName.LeftChop(1);
        }
        UPackage* Package = CreatePackage(*PackageName);
        Package->FullyLoad();
        return Package;
    }

    void NotifyAssetCreated(UObject* Asset)
    {
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
    }
}

// ---------------------------------------------------------------------------
// Asset Loading
// ---------------------------------------------------------------------------

UWidgetBlueprint* UForgeBlueprintBuilder::LoadWidgetBlueprintByPath(const FString& AssetPath)
{
    UWidgetBlueprint* BP = Cast<UWidgetBlueprint>(
        StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *AssetPath));

    if (!BP)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::LoadWidgetBlueprintByPath: Failed to load '%s'"),
               *AssetPath);
    }
    return BP;
}

UBlueprint* UForgeBlueprintBuilder::LoadBlueprintByPath(const FString& AssetPath)
{
    UBlueprint* BP = Cast<UBlueprint>(
        StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));

    if (!BP)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::LoadBlueprintByPath: Failed to load '%s'"),
               *AssetPath);
    }
    return BP;
}

// ---------------------------------------------------------------------------
// Widget Blueprint API — Tree Construction
// ---------------------------------------------------------------------------

UWidgetBlueprint* UForgeBlueprintBuilder::CreateWidgetBlueprint(
    const FString& PackagePath,
    const FForgeWidgetDescriptor& RootDescriptor)
{
    NodeRegistry.Empty();

    FString AssetName;
    FString PackageName = PackagePath;
    if (PackageName.EndsWith(TEXT("/"))) { PackageName = PackageName.LeftChop(1); }
    int32 SlashIdx = INDEX_NONE;
    // When the path has no '/', the whole string is the asset name; an unchecked
    // FindLastChar would leave SlashIdx uninitialized and corrupt RightChop.
    AssetName = PackageName.FindLastChar(TEXT('/'), SlashIdx)
        ? PackageName.RightChop(SlashIdx + 1)
        : PackageName;

    UPackage* Package = GetOrCreatePackage(PackagePath);
    if (!Package)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder: Failed to create package '%s'"), *PackagePath);
        return nullptr;
    }

    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->BlueprintType = BPTYPE_Normal;

    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(
        Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(),
                                  Package, *AssetName,
                                  RF_Public | RF_Standalone | RF_Transactional,
                                  nullptr, GWarn));

    if (!WidgetBP)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder: WidgetBlueprintFactory returned null for '%s'"),
               *PackagePath);
        return nullptr;
    }

    NotifyAssetCreated(WidgetBP);

    if (!RootDescriptor.WidgetType.IsEmpty())
    {
        AddWidget(WidgetBP, RootDescriptor);
    }

    return WidgetBP;
}

bool UForgeBlueprintBuilder::AddWidget(UWidgetBlueprint* BP,
                                        const FForgeWidgetDescriptor& Descriptor)
{
    if (!BP || !BP->WidgetTree)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::AddWidget: null BP or WidgetTree"));
        return false;
    }

    // Resolve widget class
    UClass* WidgetClass = FindFirstObject<UClass>(*Descriptor.WidgetType,
                                                   EFindFirstObjectOptions::None);
    if (!WidgetClass)
    {
        WidgetClass = FindFirstObject<UClass>(*(TEXT("U") + Descriptor.WidgetType),
                                              EFindFirstObjectOptions::None);
    }
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::AddWidget: Unknown widget type '%s'"),
               *Descriptor.WidgetType);
        return false;
    }

    // Construct the widget inside the WidgetTree
    FName WidgetFName = Descriptor.WidgetName.IsEmpty()
                        ? NAME_None
                        : FName(*Descriptor.WidgetName);

    UWidget* NewWidget = BP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetFName);
    if (!NewWidget)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::AddWidget: ConstructWidget failed for '%s'"),
               *Descriptor.WidgetType);
        return false;
    }

    // ---- Widget-specific property shortcuts --------------------------------

    NewWidget->bIsVariable = Descriptor.bIsVariable;

    if (UTextBlock* TB = Cast<UTextBlock>(NewWidget))
    {
        if (!Descriptor.Text.IsEmpty())
        {
            TB->SetText(FText::FromString(Descriptor.Text));
        }
    }
    else if (UProgressBar* PB = Cast<UProgressBar>(NewWidget))
    {
        PB->SetPercent(Descriptor.Percent);

        // Only override color if caller changed it from the default (White)
        if (Descriptor.FillColor != FLinearColor::White)
        {
            PB->SetFillColorAndOpacity(Descriptor.FillColor);
        }
    }

    // ---- Attach to parent --------------------------------------------------

    if (!Descriptor.ParentWidgetName.IsEmpty())
    {
        UWidget* ParentWidget = FindWidgetByName(BP, Descriptor.ParentWidgetName);
        UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
        if (!Panel)
        {
            UE_LOG(LogTemp, Warning,
                   TEXT("ForgeBlueprintBuilder::AddWidget: Parent '%s' not found or not a panel"),
                   *Descriptor.ParentWidgetName);
            // Widget was constructed but not attached — still return true so caller
            // can see what happened.
            BP->MarkPackageDirty();
            return true;
        }

        UPanelSlot* Slot = Panel->AddChild(NewWidget);

        // ---- Slot-type-specific configuration ------------------------------

        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
        {
            FAnchorData AnchorData;
            AnchorData.Anchors.Minimum = Descriptor.AnchorMin;
            AnchorData.Anchors.Maximum = Descriptor.AnchorMax;
            CanvasSlot->SetLayout(AnchorData);
        }
        else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
        {
            if (Descriptor.bSlotFill)
            {
                HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            }
            else
            {
                HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
            }
            HSlot->SetPadding(Descriptor.SlotPadding);
            HSlot->SetVerticalAlignment(ParseVAlign(Descriptor.SlotVAlign));
        }
        else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
        {
            if (Descriptor.bSlotFill)
            {
                VSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            }
            else
            {
                VSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
            }
            VSlot->SetPadding(Descriptor.SlotPadding);
            VSlot->SetHorizontalAlignment(ParseHAlign(Descriptor.SlotHAlign));
        }
        else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
        {
            OSlot->SetPadding(Descriptor.SlotPadding);
            OSlot->SetHorizontalAlignment(ParseHAlign(Descriptor.SlotHAlign));
            OSlot->SetVerticalAlignment(ParseVAlign(Descriptor.SlotVAlign));
        }
        else if (Slot)
        {
            // Unknown slot type — at least try to set padding via reflection
            FProperty* PaddingProp = Slot->GetClass()->FindPropertyByName(TEXT("Padding"));
            if (PaddingProp)
            {
                void* Data = PaddingProp->ContainerPtrToValuePtr<void>(Slot);
                FMargin TempPadding = Descriptor.SlotPadding;
                FMemory::Memcpy(Data, &TempPadding, sizeof(FMargin));
            }
        }
    }
    else if (BP->WidgetTree->RootWidget == nullptr)
    {
        // No parent + no root yet → make this the root
        BP->WidgetTree->RootWidget = NewWidget;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    BP->MarkPackageDirty();
    return true;
}

bool UForgeBlueprintBuilder::ClearWidgetChildren(UWidgetBlueprint* BP,
                                                   const FString& ParentWidgetName)
{
    if (!BP || !BP->WidgetTree)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::ClearWidgetChildren: null BP or WidgetTree"));
        return false;
    }

    UWidget* ParentWidget = FindWidgetByName(BP, ParentWidgetName);
    UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
    if (!Panel)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::ClearWidgetChildren: '%s' not found or not a panel"),
               *ParentWidgetName);
        return false;
    }

    // Collect children first (cannot mutate while iterating via index)
    TArray<UWidget*> Children;
    for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
    {
        Children.Add(Panel->GetChildAt(i));
    }

    if (Children.Num() == 0)
    {
        return false;
    }

    for (UWidget* Child : Children)
    {
        if (!Child) continue;

        Panel->RemoveChild(Child);

        // Move out of the BP package so the widget is excluded from the next save.
        // GC will collect it during the next collection cycle.
        Child->Rename(nullptr, GetTransientPackage(),
                      REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    BP->MarkPackageDirty();
    return true;
}

bool UForgeBlueprintBuilder::SetWidgetProperty(UWidgetBlueprint* BP,
                                                 const FString& WidgetName,
                                                 const FString& PropertyName,
                                                 const FString& PropertyValue)
{
    if (!BP || !BP->WidgetTree)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::SetWidgetProperty: null BP or WidgetTree"));
        return false;
    }

    UWidget* Widget = FindWidgetByName(BP, WidgetName);
    if (!Widget)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::SetWidgetProperty: Widget '%s' not found"),
               *WidgetName);
        return false;
    }

    // bIsVariable is an editor-only bool on UWidget — handle before generic reflection
    if (PropertyName.Equals(TEXT("bIsVariable"), ESearchCase::IgnoreCase))
    {
        Widget->bIsVariable = PropertyValue.ToBool();
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
        BP->MarkPackageDirty();
        return true;
    }

    // Generic reflection-based property import
    FProperty* Prop = Widget->GetClass()->FindPropertyByName(*PropertyName);
    if (!Prop)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::SetWidgetProperty: Property '%s' not found on '%s' (%s)"),
               *PropertyName, *WidgetName, *Widget->GetClass()->GetName());
        return false;
    }

    void* Data = Prop->ContainerPtrToValuePtr<void>(Widget);
    Prop->ImportText_Direct(*PropertyValue, Data, Widget, PPF_None);

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    BP->MarkPackageDirty();
    return true;
}

// ---------------------------------------------------------------------------
// Actor Blueprint API
// ---------------------------------------------------------------------------

UBlueprint* UForgeBlueprintBuilder::CreateActorBlueprint(const FString& PackagePath,
                                                           TSubclassOf<AActor> ParentClass)
{
    NodeRegistry.Empty();

    if (!ParentClass)
    {
        ParentClass = AActor::StaticClass();
    }

    FString AssetName;
    FString PackageName = PackagePath;
    if (PackageName.EndsWith(TEXT("/"))) { PackageName = PackageName.LeftChop(1); }
    int32 SlashIdx = INDEX_NONE;
    // When the path has no '/', the whole string is the asset name; an unchecked
    // FindLastChar would leave SlashIdx uninitialized and corrupt RightChop.
    AssetName = PackageName.FindLastChar(TEXT('/'), SlashIdx)
        ? PackageName.RightChop(SlashIdx + 1)
        : PackageName;

    UPackage* Package = GetOrCreatePackage(PackagePath);
    if (!Package)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder: Failed to create package '%s'"), *PackagePath);
        return nullptr;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        ParentClass, Package, *AssetName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        NAME_None);

    if (!BP)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder: CreateBlueprint returned null for '%s'"),
               *PackagePath);
        return nullptr;
    }

    NotifyAssetCreated(BP);
    return BP;
}

bool UForgeBlueprintBuilder::AddGraphNode(UBlueprint* BP,
                                           const FString& GraphName,
                                           const FForgeNodeDescriptor& Descriptor)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::AddGraphNode: null BP"));
        return false;
    }

    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (!Graph)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::AddGraphNode: Graph '%s' not found in '%s'"),
               *GraphName, *BP->GetName());
        return false;
    }

    // Resolve node class
    const FString FullNodeClassName = TEXT("U") + Descriptor.NodeType;
    UClass* NodeClass = FindFirstObject<UClass>(*FullNodeClassName,
                                                 EFindFirstObjectOptions::None);
    if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
    {
        NodeClass = FindFirstObject<UClass>(*Descriptor.NodeType,
                                            EFindFirstObjectOptions::None);
    }
    if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::AddGraphNode: Unknown node type '%s'"),
               *Descriptor.NodeType);
        return false;
    }

    UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
    if (!NewNode)
    {
        return false;
    }

    NewNode->NodePosX = static_cast<int32>(Descriptor.Position.X);
    NewNode->NodePosY = static_cast<int32>(Descriptor.Position.Y);

    Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    NewNode->CreateNewGuid();
    NewNode->PostPlacedNewNode();
    NewNode->AllocateDefaultPins();

    // ---- Node-type-specific setup ------------------------------------------

    if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode))
    {
        if (!Descriptor.FunctionName.IsEmpty())
        {
            FString ClassName, FuncName;
            if (Descriptor.FunctionName.Split(TEXT("::"), &ClassName, &FuncName))
            {
                UClass* OwnerClass = FindFirstObject<UClass>(*ClassName,
                                                              EFindFirstObjectOptions::None);
                if (OwnerClass)
                {
                    UFunction* Func = OwnerClass->FindFunctionByName(*FuncName);
                    if (Func)
                    {
                        CallNode->SetFromFunction(Func);
                        CallNode->ReconstructNode();
                    }
                }
            }
        }
    }
    else if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(NewNode))
    {
        if (!Descriptor.MemberName.IsEmpty())
        {
            VarGet->VariableReference.SetSelfMember(*Descriptor.MemberName);
            VarGet->ReconstructNode();
        }
    }
    else if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(NewNode))
    {
        if (!Descriptor.MemberName.IsEmpty())
        {
            VarSet->VariableReference.SetSelfMember(*Descriptor.MemberName);
            VarSet->ReconstructNode();
        }
    }
    else if (UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(NewNode))
    {
        if (!Descriptor.EnumName.IsEmpty())
        {
            UEnum* EnumType = FindFirstObject<UEnum>(*Descriptor.EnumName,
                                                      EFindFirstObjectOptions::None);
            if (EnumType)
            {
                SwitchNode->SetEnum(EnumType);
                // SetEnum calls ReconstructNode internally — pins are now per enum value
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                       TEXT("ForgeBlueprintBuilder::AddGraphNode: Enum '%s' not found for SwitchEnum node '%s'"),
                       *Descriptor.EnumName, *Descriptor.NodeId);
            }
        }
    }
    else if (UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(NewNode))
    {
        // Branch has standard pins (Condition/Then/Else) — ReconstructNode refreshes them
        BranchNode->ReconstructNode();
    }

    // Register by NodeId for WireNodes and SetPinDefaultValue
    if (!Descriptor.NodeId.IsEmpty())
    {
        NodeRegistry.Add(Descriptor.NodeId, TWeakObjectPtr<UEdGraphNode>(NewNode));
    }

    BP->MarkPackageDirty();
    return true;
}

bool UForgeBlueprintBuilder::WireNodes(UBlueprint* BP,
                                        const FString& GraphName,
                                        const FForgeLinkDescriptor& Link)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::WireNodes: null BP"));
        return false;
    }

    TWeakObjectPtr<UEdGraphNode>* FromWeak = NodeRegistry.Find(Link.FromNodeId);
    TWeakObjectPtr<UEdGraphNode>* ToWeak   = NodeRegistry.Find(Link.ToNodeId);

    if (!FromWeak || !FromWeak->IsValid())
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: FromNodeId '%s' not found or GC'd"),
               *Link.FromNodeId);
        return false;
    }
    if (!ToWeak || !ToWeak->IsValid())
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: ToNodeId '%s' not found or GC'd"),
               *Link.ToNodeId);
        return false;
    }

    UEdGraphNode* FromNode = FromWeak->Get();
    UEdGraphNode* ToNode   = ToWeak->Get();

    UEdGraphPin* FromPin = FromNode->FindPin(*Link.FromPinName, EGPD_Output);
    UEdGraphPin* ToPin   = ToNode->FindPin(*Link.ToPinName,   EGPD_Input);

    if (!FromPin)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: Output pin '%s' not found on '%s'"),
               *Link.FromPinName, *Link.FromNodeId);
        return false;
    }
    if (!ToPin)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: Input pin '%s' not found on '%s'"),
               *Link.ToPinName, *Link.ToNodeId);
        return false;
    }

    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (!Graph)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: Graph '%s' not found"), *GraphName);
        return false;
    }

    const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    if (!Schema)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: Graph '%s' has no K2 schema"),
               *GraphName);
        return false;
    }

    const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::WireNodes: Cannot connect '%s'.%s → '%s'.%s: %s"),
               *Link.FromNodeId, *Link.FromPinName,
               *Link.ToNodeId,   *Link.ToPinName,
               *Response.Message.ToString());
        return false;
    }

    Schema->TryCreateConnection(FromPin, ToPin);

    BP->MarkPackageDirty();
    return true;
}

bool UForgeBlueprintBuilder::SetPinDefaultValue(UBlueprint* BP,
                                                  const FString& GraphName,
                                                  const FString& NodeId,
                                                  const FString& PinName,
                                                  const FString& Value)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::SetPinDefaultValue: null BP"));
        return false;
    }

    TWeakObjectPtr<UEdGraphNode>* Weak = NodeRegistry.Find(NodeId);
    if (!Weak || !Weak->IsValid())
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::SetPinDefaultValue: NodeId '%s' not found or GC'd"),
               *NodeId);
        return false;
    }

    UEdGraphNode* Node = Weak->Get();
    UEdGraphPin* Pin = Node->FindPin(*PinName, EGPD_Input);
    if (!Pin)
    {
        // Try without direction filter as fallback
        Pin = Node->FindPin(*PinName);
    }
    if (!Pin)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::SetPinDefaultValue: Pin '%s' not found on node '%s'"),
               *PinName, *NodeId);
        return false;
    }

    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (Graph)
    {
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema)
        {
            Schema->TrySetDefaultValue(*Pin, Value);
        }
        else
        {
            // Fallback: set directly on the pin
            Pin->DefaultValue = Value;
        }
    }
    else
    {
        Pin->DefaultValue = Value;
    }

    BP->MarkPackageDirty();
    return true;
}

void UForgeBlueprintBuilder::CompileBlueprint(UBlueprint* BP)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::CompileBlueprint: null BP"));
        return;
    }

    FKismetEditorUtilities::CompileBlueprint(BP,
        EBlueprintCompileOptions::SkipGarbageCollection);

    BP->MarkPackageDirty();
    // NodeRegistry intentionally NOT cleared — caller must call ClearNodeRegistry() explicitly.
}

bool UForgeBlueprintBuilder::SaveBlueprint(UBlueprint* BP)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::SaveBlueprint: null BP"));
        return false;
    }

    UPackage* Package = BP->GetOutermost();
    if (!Package)
    {
        return false;
    }

    FString PackageFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(
            Package->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::SaveBlueprint: Cannot resolve filename for '%s'"),
               *Package->GetName());
        return false;
    }

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    const FSavePackageResultStruct Result = UPackage::Save(Package, BP, *PackageFilename, SaveArgs);
    return Result.IsSuccessful();
}

bool UForgeBlueprintBuilder::ImplementEventOverride(UBlueprint* BP,
                                                      const FString& GraphName,
                                                      const FString& ParentFunctionName,
                                                      const FString& NodeId)
{
    if (!BP)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("ForgeBlueprintBuilder::ImplementEventOverride: null BP"));
        return false;
    }

    UEdGraph* Graph = FindGraph(BP, GraphName);
    if (!Graph)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::ImplementEventOverride: Graph '%s' not found in '%s'"),
               *GraphName, *BP->GetName());
        return false;
    }

    UClass* ParentClass = BP->ParentClass;
    if (!ParentClass)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::ImplementEventOverride: BP '%s' has no parent class"),
               *BP->GetName());
        return false;
    }

    UFunction* ParentFunc = ParentClass->FindFunctionByName(*ParentFunctionName);
    if (!ParentFunc)
    {
        UE_LOG(LogTemp, Error,
               TEXT("ForgeBlueprintBuilder::ImplementEventOverride: function '%s' not found on '%s'"),
               *ParentFunctionName, *ParentClass->GetName());
        return false;
    }

    UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph, UK2Node_Event::StaticClass());
    EventNode->bOverrideFunction = true;
    EventNode->EventReference.SetFromField<UFunction>(ParentFunc, /*bIsSelfContext=*/false);

    Graph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    EventNode->CreateNewGuid();
    EventNode->PostPlacedNewNode();
    EventNode->AllocateDefaultPins();

    if (!NodeId.IsEmpty())
    {
        NodeRegistry.Add(NodeId, TWeakObjectPtr<UEdGraphNode>(EventNode));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    BP->MarkPackageDirty();
    return true;
}

void UForgeBlueprintBuilder::ClearNodeRegistry()
{
    NodeRegistry.Empty();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

UEdGraph* UForgeBlueprintBuilder::FindGraph(UBlueprint* BP, const FString& GraphName) const
{
    if (!BP) return nullptr;

    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (Graph && Graph->GetName() == GraphName)
        {
            return Graph;
        }
    }
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == GraphName)
        {
            return Graph;
        }
    }
    return nullptr;
}

UWidget* UForgeBlueprintBuilder::FindWidgetByName(UWidgetBlueprint* BP,
                                                    const FString& WidgetName) const
{
    if (!BP || !BP->WidgetTree) return nullptr;

    UWidget* Found = nullptr;
    BP->WidgetTree->ForEachWidget([&](UWidget* Widget)
    {
        if (Widget && Widget->GetName() == WidgetName)
        {
            Found = Widget;
        }
    });
    return Found;
}

EHorizontalAlignment UForgeBlueprintBuilder::ParseHAlign(const FString& S)
{
    if (S.Equals(TEXT("Left"),   ESearchCase::IgnoreCase)) return HAlign_Left;
    if (S.Equals(TEXT("Right"),  ESearchCase::IgnoreCase)) return HAlign_Right;
    if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
    return HAlign_Fill;
}

EVerticalAlignment UForgeBlueprintBuilder::ParseVAlign(const FString& S)
{
    if (S.Equals(TEXT("Top"),    ESearchCase::IgnoreCase)) return VAlign_Top;
    if (S.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
    if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
    return VAlign_Fill;
}
