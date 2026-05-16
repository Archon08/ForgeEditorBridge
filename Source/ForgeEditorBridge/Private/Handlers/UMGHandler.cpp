#include "Handlers/UMGHandler.h"
#include "ForgeAISubsystem.h"
#include "Attention/BridgeAttentionManager.h"
#include "Capture/ForgeUMGCapture.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Blueprint/UserWidget.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Animation/WidgetAnimation.h"
#include "FileHelpers.h"
#include "PackageTools.h"
#include "Misc/PackageName.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "UObject/UnrealType.h"
#include "Editor.h"

// ---------------------------------------------------------------------------
// Static helper: recursive widget-to-JSON export (ported from UmgGetSubsystem)
// ---------------------------------------------------------------------------
static TSharedPtr<FJsonObject> ExportWidgetToJson(UWidget* Widget)
{
	if (!Widget)
		return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Widget->GetFName().ToString());
	Obj->SetStringField(TEXT("type"), Widget->GetClass()->GetName());

	UObject* CDO = Widget->GetClass()->GetDefaultObject();

	// Non-default CPF_Edit properties
	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!(Prop->PropertyFlags & CPF_Edit)) continue;
		if (Prop->PropertyFlags & CPF_DisableEditOnInstance) continue;
		if (Prop->Identical_InContainer(Widget, CDO)) continue;

		FString ValStr;
		const void* Addr = Prop->ContainerPtrToValuePtr<void>(Widget);
		Prop->ExportTextItem_Direct(ValStr, Addr, nullptr, nullptr, PPF_None);
		PropsObj->SetStringField(Prop->GetName(), ValStr);
	}
	Obj->SetObjectField(TEXT("properties"), PropsObj);

	// Slot properties
	if (Widget->Slot)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		UObject* SlotCDO = Widget->Slot->GetClass()->GetDefaultObject();
		for (TFieldIterator<FProperty> PropIt(Widget->Slot->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!(Prop->PropertyFlags & CPF_Edit)) continue;
			if (Prop->Identical_InContainer(Widget->Slot, SlotCDO)) continue;

			FString ValStr;
			const void* Addr = Prop->ContainerPtrToValuePtr<void>(Widget->Slot);
			Prop->ExportTextItem_Direct(ValStr, Addr, nullptr, nullptr, PPF_None);
			SlotObj->SetStringField(Prop->GetName(), ValStr);
		}
		Obj->SetObjectField(TEXT("slot"), SlotObj);
	}

	// Recurse children
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> ChildArr;
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			TSharedPtr<FJsonObject> ChildObj = ExportWidgetToJson(Panel->GetChildAt(i));
			if (ChildObj.IsValid())
				ChildArr.Add(MakeShared<FJsonValueObject>(ChildObj));
		}
		Obj->SetArrayField(TEXT("children"), ChildArr);
	}

	return Obj;
}

static FString SerializeToString(const TSharedPtr<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
UWidgetBlueprint* UUMGHandler::ResolveWBP(TSharedPtr<FJsonObject> Params, FString& OutError) const
{
	FString AssetPath;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		FString BaseName   = FPackageName::GetShortName(AssetPath);
		FString ObjectPath = AssetPath + TEXT(".") + BaseName;
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		if (!WBP)
			WBP = Cast<UWidgetBlueprint>(FSoftObjectPath(AssetPath).ResolveObject());
		if (!WBP)
		{
			OutError = FString::Printf(TEXT("Could not load WBP: %s"), *AssetPath);
			return nullptr;
		}
		return WBP;
	}

	if (Subsystem && Subsystem->AttentionManager)
	{
		UWidgetBlueprint* WBP = Subsystem->AttentionManager->GetCachedWBP();
		if (!WBP)
		{
			OutError = TEXT("No asset_path provided and no target WBP cached in AttentionManager");
			return nullptr;
		}
		return WBP;
	}

	OutError = TEXT("No asset_path provided and AttentionManager unavailable");
	return nullptr;
}

TSharedPtr<FJsonObject> UUMGHandler::NormalizePascalCase(const TSharedPtr<FJsonObject>& In) const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	for (auto& Pair : In->Values)
	{
		FString Key = Pair.Key;
		if (!Key.IsEmpty())
			Key[0] = FChar::ToUpper(Key[0]);
		Out->Values.Add(Key, Pair.Value);
	}
	return Out;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("umg"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("get_widget_tree"))        return Action_GetWidgetTree(Params);
	if (Action == TEXT("query_widget_properties")) return Action_QueryWidgetProperties(Params);
	if (Action == TEXT("get_widget_schema"))       return Action_GetWidgetSchema(Params);
	if (Action == TEXT("get_layout_data"))         return Action_GetLayoutData(Params);
	if (Action == TEXT("create_widget"))           return Action_CreateWidget(Params);
	if (Action == TEXT("set_widget_properties"))   return Action_SetWidgetProperties(Params);
	if (Action == TEXT("reparent_widget"))         return Action_ReparentWidget(Params);
	if (Action == TEXT("set_active_widget"))       return Action_SetActiveWidget(Params);
	if (Action == TEXT("save_asset"))              return Action_SaveAsset(Params);
	if (Action == TEXT("read_umg_capture"))        return Action_ReadUMGCapture(Params);
	if (Action == TEXT("audit_umg_widget"))        return Action_AuditUMGWidget(Params);

	// Phase 1e additions
	if (Action == TEXT("create_widget_blueprint")) return Action_CreateWidgetBlueprint(Params);
	if (Action == TEXT("add_widget"))              return Action_AddWidget(Params);
	if (Action == TEXT("set_binding"))             return Action_SetBinding(Params);
	if (Action == TEXT("set_anchors"))             return Action_SetAnchors(Params);
	if (Action == TEXT("remove_widget"))           return Action_RemoveWidget(Params);
	if (Action == TEXT("set_widget_property"))     return Action_SetWidgetProperty(Params);
	if (Action == TEXT("list_widgets"))            return Action_ListWidgets(Params);

	// Wave 8: EUW + animations + event bindings
	if (Action == TEXT("create_editor_utility_widget")) return Action_CreateEditorUtilityWidget(Params);
	if (Action == TEXT("add_widget_animation"))         return Action_AddWidgetAnimation(Params);
	if (Action == TEXT("list_widget_animations"))       return Action_ListWidgetAnimations(Params);
	if (Action == TEXT("remove_widget_animation"))      return Action_RemoveWidgetAnimation(Params);
	if (Action == TEXT("bind_widget_event"))            return Action_BindWidgetEvent(Params);
	if (Action == TEXT("add_named_slot"))               return Action_AddNamedSlot(Params);

	// Wave 9: MovieScene tracks + event bindings
	if (Action == TEXT("add_widget_anim_binding"))      return Action_AddWidgetAnimBinding(Params);
	if (Action == TEXT("list_widget_anim_bindings"))    return Action_ListWidgetAnimBindings(Params);
	if (Action == TEXT("add_widget_anim_track"))        return Action_AddWidgetAnimTrack(Params);
	if (Action == TEXT("remove_widget_anim_track"))     return Action_RemoveWidgetAnimTrack(Params);
	if (Action == TEXT("list_widget_anim_tracks"))      return Action_ListWidgetAnimTracks(Params);
	if (Action == TEXT("add_widget_anim_keyframe_float")) return Action_AddWidgetAnimKeyframeFloat(Params);
	if (Action == TEXT("unbind_widget_event"))          return Action_UnbindWidgetEvent(Params);
	if (Action == TEXT("list_widget_event_bindings"))   return Action_ListWidgetEventBindings(Params);

	FBridgeResult Result = CreateResult(TEXT("umg"), Action);
	Result.Message = FString::Printf(
		TEXT("Unknown UMG action '%s'. Supported: get_widget_tree, query_widget_properties, "
		     "get_widget_schema, get_layout_data, create_widget, set_widget_properties, "
		     "reparent_widget, set_active_widget, save_asset, read_umg_capture, audit_umg_widget, "
		     "create_widget_blueprint, add_widget, set_binding, set_anchors, remove_widget, "
		     "set_widget_property, list_widgets"),
		*Action);
	Result.ErrorCode = 1001;
	return Result;
}

// ---------------------------------------------------------------------------
// Read actions
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_GetWidgetTree(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("get_widget_tree"));

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error.IsEmpty() ? TEXT("WBP has no WidgetTree") : Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Root = WBP->WidgetTree->RootWidget;
	if (!Root)
	{
		Result.bSuccess   = true;
		Result.AffectedPath = WBP->GetPathName();
		Result.ExtraData  = TEXT("{\"root\":null}");
		Result.Message    = TEXT("Widget tree is empty (no root widget)");
		return Result;
	}

	TSharedPtr<FJsonObject> TreeObj = MakeShared<FJsonObject>();
	TreeObj->SetObjectField(TEXT("root"), ExportWidgetToJson(Root));

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData    = SerializeToString(TreeObj);
	Result.Message      = FString::Printf(TEXT("Widget tree exported for: %s"), *WBP->GetName());
	return Result;
}

FBridgeResult UUMGHandler::Action_QueryWidgetProperties(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("query_widget_properties"));

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("query_widget_properties requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	const TArray<TSharedPtr<FJsonValue>>* FilterArr = nullptr;
	Params->TryGetArrayField(TEXT("properties"), FilterArr);

	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!(Prop->PropertyFlags & CPF_Edit)) continue;

		if (FilterArr)
		{
			bool bMatch = false;
			for (const auto& V : *FilterArr)
				if (V->AsString() == Prop->GetName()) { bMatch = true; break; }
			if (!bMatch) continue;
		}

		FString ValStr;
		const void* Addr = Prop->ContainerPtrToValuePtr<void>(Widget);
		Prop->ExportTextItem_Direct(ValStr, Addr, nullptr, nullptr, PPF_None);
		PropsObj->SetStringField(Prop->GetName(), ValStr);
	}

	Result.bSuccess  = true;
	Result.ExtraData = SerializeToString(PropsObj);
	Result.Message   = FString::Printf(TEXT("Properties for widget: %s"), *WidgetName);
	return Result;
}

FBridgeResult UUMGHandler::Action_GetWidgetSchema(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("get_widget_schema"));

	FString WidgetType;
	if (!Params->TryGetStringField(TEXT("widget_type"), WidgetType) || WidgetType.IsEmpty())
	{
		Result.Message = TEXT("get_widget_schema requires: widget_type");
		Result.ErrorCode = 1000;
		return Result;
	}

	UClass* WidgetClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == WidgetType && It->IsChildOf(UWidget::StaticClass()))
		{
			WidgetClass = *It;
			break;
		}
	}
	if (!WidgetClass) { Result.Message = FString::Printf(TEXT("Widget class not found: %s"), *WidgetType); Result.ErrorCode = 2001; return Result; }

	TArray<TSharedPtr<FJsonValue>> SchemaArr;
	for (TFieldIterator<FProperty> PropIt(WidgetClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!(Prop->PropertyFlags & CPF_Edit)) continue;

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		SchemaArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	TSharedPtr<FJsonObject> SchemaObj = MakeShared<FJsonObject>();
	SchemaObj->SetStringField(TEXT("widget_type"), WidgetType);
	SchemaObj->SetArrayField (TEXT("properties"),  SchemaArr);

	Result.bSuccess  = true;
	Result.ExtraData = SerializeToString(SchemaObj);
	Result.Message   = FString::Printf(TEXT("Schema for %s (%d properties)"), *WidgetType, SchemaArr.Num());
	return Result;
}

FBridgeResult UUMGHandler::Action_GetLayoutData(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("get_layout_data"));

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	TArray<TSharedPtr<FJsonValue>> LayoutArr;
	WBP->WidgetTree->ForEachWidget([&](UWidget* Widget)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Widget->GetFName().ToString());
		Item->SetStringField(TEXT("type"), Widget->GetClass()->GetName());
		FVector2D DesiredSize = Widget->GetDesiredSize();
		Item->SetNumberField(TEXT("width"),  DesiredSize.X);
		Item->SetNumberField(TEXT("height"), DesiredSize.Y);
		LayoutArr.Add(MakeShared<FJsonValueObject>(Item));
	});

	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	DataObj->SetArrayField(TEXT("widgets"), LayoutArr);

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData    = SerializeToString(DataObj);
	Result.Message      = FString::Printf(TEXT("Layout data for %d widgets"), LayoutArr.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// Write actions
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_CreateWidget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("create_widget"));

	FString WidgetType, NewWidgetName;
	if (!Params->TryGetStringField(TEXT("widget_type"), WidgetType) || WidgetType.IsEmpty())
	{
		Result.Message = TEXT("create_widget requires: widget_type");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("new_widget_name"), NewWidgetName) || NewWidgetName.IsEmpty())
	{
		Result.Message = TEXT("create_widget requires: new_widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	// Resolve widget class by short name
	UClass* WidgetClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == WidgetType && It->IsChildOf(UWidget::StaticClass()))
		{
			WidgetClass = *It;
			break;
		}
	}
	if (!WidgetClass) { Result.Message = FString::Printf(TEXT("Widget class not found: %s"), *WidgetType); Result.ErrorCode = 2001; return Result; }

	UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*NewWidgetName));
	if (!NewWidget) { Result.Message = FString::Printf(TEXT("Failed to construct widget of class: %s"), *WidgetType); Result.ErrorCode = 3000; return Result; }

	// Attach to named parent, root panel, or set as root
	FString ParentName;
	if (Params->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
	{
		UPanelWidget* Parent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*ParentName)));
		if (!Parent) { Result.Message = FString::Printf(TEXT("Parent widget not found: %s"), *ParentName); Result.ErrorCode = 2000; return Result; }
		Parent->AddChild(NewWidget);
	}
	else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget))
	{
		RootPanel->AddChild(NewWidget);
	}
	else if (!WBP->WidgetTree->RootWidget)
	{
		WBP->WidgetTree->RootWidget = NewWidget;
	}
	else
	{
		Result.Message = TEXT("Root is not a panel and no parent_name specified");
		Result.ErrorCode = 2001;
		return Result;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	if (Subsystem && Subsystem->AttentionManager)
		Subsystem->AttentionManager->SetActiveWidget(NewWidgetName);

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = FString::Printf(TEXT("Created '%s' of type '%s'"), *NewWidgetName, *WidgetType);
	return Result;
}

FBridgeResult UUMGHandler::Action_SetWidgetProperties(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("set_widget_properties"));

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("set_widget_properties requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	const TSharedPtr<FJsonObject>* PropsPtr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsPtr) || !PropsPtr->IsValid())
	{
		Result.Message = TEXT("set_widget_properties requires: properties (object)");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	TSharedPtr<FJsonObject> NormProps = NormalizePascalCase(*PropsPtr);

	// Separate slot vs widget properties
	TSharedPtr<FJsonObject> WidgetProps = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> SlotProps   = MakeShared<FJsonObject>();

	if (Widget->Slot)
	{
		TSet<FString> SlotNames;
		for (TFieldIterator<FProperty> PropIt(Widget->Slot->GetClass()); PropIt; ++PropIt)
			SlotNames.Add((*PropIt)->GetName());

		for (auto& Pair : NormProps->Values)
		{
			if (SlotNames.Contains(Pair.Key))
				SlotProps->Values.Add(Pair.Key, Pair.Value);
			else
				WidgetProps->Values.Add(Pair.Key, Pair.Value);
		}
	}
	else
	{
		WidgetProps = NormProps;
	}

	int32 Applied = 0;

	for (auto& Pair : WidgetProps->Values)
	{
		FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop) continue;
		FString ValStr;
		if (Pair.Value->TryGetString(ValStr))
		{
			void* Addr = Prop->ContainerPtrToValuePtr<void>(Widget);
			Prop->ImportText_Direct(*ValStr, Addr, Widget, PPF_None);
			++Applied;
		}
	}

	if (Widget->Slot)
	{
		for (auto& Pair : SlotProps->Values)
		{
			FProperty* Prop = Widget->Slot->GetClass()->FindPropertyByName(FName(*Pair.Key));
			if (!Prop) continue;
			FString ValStr;
			if (Pair.Value->TryGetString(ValStr))
			{
				void* Addr = Prop->ContainerPtrToValuePtr<void>(Widget->Slot);
				Prop->ImportText_Direct(*ValStr, Addr, Widget->Slot, PPF_None);
				++Applied;
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = FString::Printf(TEXT("Applied %d properties to '%s'"), Applied, *WidgetName);
	return Result;
}

FBridgeResult UUMGHandler::Action_ReparentWidget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("reparent_widget"));

	FString WidgetName, NewParentName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("reparent_widget requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("new_parent_name"), NewParentName) || NewParentName.IsEmpty())
	{
		Result.Message = TEXT("reparent_widget requires: new_parent_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	UPanelWidget* NewParent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*NewParentName)));
	if (!NewParent) { Result.Message = FString::Printf(TEXT("New parent not found or not a panel: %s"), *NewParentName); Result.ErrorCode = 2000; return Result; }

	if (UPanelWidget* OldParent = Widget->GetParent())
		OldParent->RemoveChild(Widget);

	NewParent->AddChild(Widget);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = FString::Printf(TEXT("Reparented '%s' -> '%s'"), *WidgetName, *NewParentName);
	return Result;
}

FBridgeResult UUMGHandler::Action_SetActiveWidget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("set_active_widget"));

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("set_active_widget requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	if (Subsystem && Subsystem->AttentionManager)
		Subsystem->AttentionManager->SetActiveWidget(WidgetName);

	Result.bSuccess = true;
	Result.Message  = FString::Printf(TEXT("Active widget set to: %s"), *WidgetName);
	return Result;
}

FBridgeResult UUMGHandler::Action_SaveAsset(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("save_asset"));

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	TArray<UPackage*> Packages;
	Packages.Add(WBP->GetOutermost());
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bCheckDirty=*/false);

	Result.bSuccess     = bSaved;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = bSaved
		? FString::Printf(TEXT("Saved: %s"), *WBP->GetName())
		: FString::Printf(TEXT("Failed to save: %s"), *WBP->GetName());
	if (!bSaved) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// read_umg_capture
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_ReadUMGCapture(TSharedPtr<FJsonObject> Params)
{
	if (!Subsystem || !Subsystem->UMGCapture)
		return MakeError(TEXT("umg"), TEXT("read_umg_capture"),
			2000, TEXT("UMGCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

	FString AssetPath, Pattern;
	const bool bHasAsset   = Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();
	const bool bHasPattern = Params->TryGetStringField(TEXT("pattern"),    Pattern)   && !Pattern.IsEmpty();

	if (bHasAsset)
	{
		Subsystem->UMGCapture->ExportWidget(AssetPath);

		FString Segment = AssetPath;
		int32 SlashIdx;
		if (Segment.FindLastChar(TEXT('/'), SlashIdx))
			Segment = Segment.RightChop(SlashIdx + 1);

		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("umg"), Segment + TEXT(".json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(TEXT("umg"), TEXT("read_umg_capture"),
			TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}
	else if (bHasPattern)
	{
		int32 Count = Subsystem->UMGCapture->ExportWidgetsByPattern(Pattern);
		return MakeSuccess(TEXT("umg"), TEXT("read_umg_capture"),
			FString::Printf(TEXT("Exported %d widgets matching pattern '%s'"), Count, *Pattern));
	}
	else
	{
		int32 Count = Subsystem->UMGCapture->ExportWidgetsByPattern(TEXT(""));
		return MakeSuccess(TEXT("umg"), TEXT("read_umg_capture"),
			FString::Printf(TEXT("Exported all %d widgets"), Count));
	}
}

// ---------------------------------------------------------------------------
// audit_umg_widget
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_AuditUMGWidget(TSharedPtr<FJsonObject> Params)
{
	if (!Subsystem || !Subsystem->UMGCapture)
		return MakeError(TEXT("umg"), TEXT("audit_umg_widget"),
			2000, TEXT("UMGCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("umg"), TEXT("audit_umg_widget"),
			1000, TEXT("audit_umg_widget: 'asset_path' is required"), TEXT("Provide the content path of a Widget Blueprint"));

	int32 IssueCount = Subsystem->UMGCapture->AuditWidget(AssetPath);
	return MakeSuccess(TEXT("umg"), TEXT("audit_umg_widget"),
		FString::Printf(TEXT("Audit complete — %d issue(s) found in '%s'"), IssueCount, *AssetPath));
}

// ---------------------------------------------------------------------------
// Phase 1e: create_widget_blueprint
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_CreateWidgetBlueprint(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("create_widget_blueprint"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_widget_blueprint requires: asset_path");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString ParentClassName = TEXT("UserWidget");
	Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

	// Resolve parent class (default to UUserWidget)
	UClass* ParentClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((It->GetName() == ParentClassName || It->GetName() == FString::Printf(TEXT("U%s"), *ParentClassName))
			&& It->IsChildOf(UUserWidget::StaticClass()))
		{
			ParentClass = *It;
			break;
		}
	}
	if (!ParentClass)
	{
		ParentClass = UUserWidget::StaticClass();
	}

	// Split asset_path into package path and asset name.
	// Expected form: "/Game/UI/MyWidget"
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName   = FPackageName::GetShortName(AssetPath);

	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		Result.Message = FString::Printf(TEXT("Invalid asset_path: '%s' (expected e.g. /Game/UI/MyWidget)"), *AssetPath);
		Result.ErrorCode = 1000;
		return Result;
	}

	// Configure factory
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	// Use IAssetTools to create the asset
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
	if (!NewAsset)
	{
		Result.Message = FString::Printf(TEXT("Failed to create WidgetBlueprint asset at: %s"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UWidgetBlueprint* NewWBP = Cast<UWidgetBlueprint>(NewAsset);
	if (NewWBP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(NewWBP);
		NewWBP->GetOutermost()->MarkPackageDirty();
	}

	Result.bSuccess     = true;
	Result.AffectedPath = NewAsset->GetPathName();
	Result.Message      = FString::Printf(TEXT("Created Widget Blueprint asset: %s (parent: %s)"),
		*AssetPath, *ParentClass->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// Phase 1e: add_widget (alias for create_widget)
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_AddWidget(TSharedPtr<FJsonObject> Params)
{
	return Action_CreateWidget(Params);
}

// ---------------------------------------------------------------------------
// Phase 1e: set_binding
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_SetBinding(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("set_binding"));

	FString WidgetName, PropertyName, FunctionName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("set_binding requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		Result.Message = TEXT("set_binding requires: property_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		Result.Message = TEXT("set_binding requires: function_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	// Register an FDelegateEditorBinding on the WidgetBlueprint.
	// The ObjectPath uses ".<WidgetName>" convention used by UMG editor code.
	FDelegateEditorBinding NewBinding;
	NewBinding.ObjectName       = WidgetName;
	NewBinding.PropertyName     = FName(*PropertyName);
	NewBinding.FunctionName     = FName(*FunctionName);
	NewBinding.Kind             = EBindingKind::Function;
	NewBinding.SourcePath       = FEditorPropertyPath();

	// Avoid duplicate entries for the same widget/property pair.
	WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == NewBinding.ObjectName
			&& Existing.PropertyName == NewBinding.PropertyName;
	});
	WBP->Bindings.Add(NewBinding);

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = TEXT("set_binding: binding registered as FDelegateEditorBinding; recompile WBP to apply");
	return Result;
}

// ---------------------------------------------------------------------------
// Phase 1e: set_anchors
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_SetAnchors(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("set_anchors"));

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("set_anchors requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	double MinX = 0.0, MinY = 0.0, MaxX = 0.0, MaxY = 0.0;
	if (!Params->TryGetNumberField(TEXT("min_x"), MinX)
		|| !Params->TryGetNumberField(TEXT("min_y"), MinY)
		|| !Params->TryGetNumberField(TEXT("max_x"), MaxX)
		|| !Params->TryGetNumberField(TEXT("max_y"), MaxY))
	{
		Result.Message = TEXT("set_anchors requires: min_x, min_y, max_x, max_y (floats 0-1)");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
	if (!CanvasSlot)
	{
		Result.Message = TEXT("set_anchors requires a Canvas Panel parent");
		Result.ErrorCode = 3000;
		return Result;
	}

	FAnchors Anchors;
	Anchors.Minimum = FVector2D(static_cast<float>(MinX), static_cast<float>(MinY));
	Anchors.Maximum = FVector2D(static_cast<float>(MaxX), static_cast<float>(MaxY));
	CanvasSlot->SetAnchors(Anchors);

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = FString::Printf(TEXT("Anchors set on '%s': min=(%.3f,%.3f) max=(%.3f,%.3f)"),
		*WidgetName, MinX, MinY, MaxX, MaxY);
	return Result;
}

// ---------------------------------------------------------------------------
// Phase 1e: remove_widget
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_RemoveWidget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("remove_widget"));

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("remove_widget requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error; Result.ErrorCode = 2000; return Result; }

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

	const bool bRemoved = WBP->WidgetTree->RemoveWidget(Widget);
	if (!bRemoved)
	{
		Result.Message = FString::Printf(TEXT("WidgetTree->RemoveWidget failed for: %s"), *WidgetName);
		Result.ErrorCode = 3000;
		return Result;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	WBP->GetOutermost()->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message      = FString::Printf(TEXT("Removed widget: %s"), *WidgetName);
	return Result;
}

// ---------------------------------------------------------------------------
// Phase 1e: set_widget_property (singular)
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_SetWidgetProperty(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("set_widget_property"));

	FString WidgetName, PropertyName, Value;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		Result.Message = TEXT("set_widget_property requires: widget_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		Result.Message = TEXT("set_widget_property requires: property_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		Result.Message = TEXT("set_widget_property requires: value");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Build a single-key properties object and forward to Action_SetWidgetProperties.
	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	PropsObj->SetStringField(PropertyName, Value);

	TSharedPtr<FJsonObject> Forward = MakeShared<FJsonObject>();
	// Preserve asset_path if supplied so ResolveWBP can pick it up.
	FString AssetPath;
	if (Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
		Forward->SetStringField(TEXT("asset_path"), AssetPath);
	Forward->SetStringField(TEXT("widget_name"), WidgetName);
	Forward->SetObjectField(TEXT("properties"), PropsObj);

	return Action_SetWidgetProperties(Forward);
}

// ---------------------------------------------------------------------------
// Phase 1e: list_widgets
// ---------------------------------------------------------------------------
FBridgeResult UUMGHandler::Action_ListWidgets(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("list_widgets"));

	FString Error;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Error);
	if (!WBP || !WBP->WidgetTree) { Result.Message = Error.IsEmpty() ? TEXT("WBP has no WidgetTree") : Error; Result.ErrorCode = 2000; return Result; }

	TArray<TSharedPtr<FJsonValue>> WidgetArr;
	WBP->WidgetTree->ForEachWidget([&](UWidget* Widget)
	{
		if (!Widget) return;
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Widget->GetFName().ToString());
		Item->SetStringField(TEXT("type"), Widget->GetClass()->GetName());
		if (UPanelWidget* Parent = Widget->GetParent())
			Item->SetStringField(TEXT("parent"), Parent->GetFName().ToString());
		WidgetArr.Add(MakeShared<FJsonValueObject>(Item));
	});

	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	DataObj->SetArrayField(TEXT("widgets"), WidgetArr);

	Result.bSuccess     = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData    = SerializeToString(DataObj);
	Result.Message      = FString::Printf(TEXT("Listed %d widgets"), WidgetArr.Num());
	return Result;
}

// ===========================================================================
// Wave 8: Editor Utility Widget + animations + event bindings + named slots
// ===========================================================================

#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilityWidget.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Components/NamedSlot.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"

FBridgeResult UUMGHandler::Action_CreateEditorUtilityWidget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("create_editor_utility_widget"));
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_editor_utility_widget requires: asset_path");
		Result.ErrorCode = 1000;
		return Result;
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	const FString AssetName   = FPackageName::GetShortName(AssetPath);

	// EUW uses the standard WidgetBlueprintFactory but with EditorUtilityWidget parent class
	// AND we want the asset class to be UEditorUtilityWidgetBlueprint (a UWidgetBlueprint subclass).
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = UEditorUtilityWidget::StaticClass();

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
		AssetName, PackagePath, UEditorUtilityWidgetBlueprint::StaticClass(), Factory);
	if (!NewAsset)
	{
		Result.Message = FString::Printf(TEXT("Failed to create EditorUtilityWidget at %s"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}
	if (UEditorUtilityWidgetBlueprint* EUW = Cast<UEditorUtilityWidgetBlueprint>(NewAsset))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(EUW);
		EUW->GetOutermost()->MarkPackageDirty();
	}
	Result.bSuccess = true;
	Result.AffectedPath = NewAsset->GetPathName();
	Result.Message = FString::Printf(TEXT("Created EditorUtilityWidget at %s"), *AssetPath);
	return Result;
}

FBridgeResult UUMGHandler::Action_AddWidgetAnimation(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("add_widget_animation"));
	FString Err;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
	if (!WBP)
	{
		Result.Message = Err.IsEmpty() ? TEXT("Could not resolve WBP") : Err;
		Result.ErrorCode = 2000;
		return Result;
	}
	FString AnimName;
	double DurationSeconds = 1.0;
	if (!Params->TryGetStringField(TEXT("name"), AnimName) || AnimName.IsEmpty())
	{
		Result.Message = TEXT("add_widget_animation requires: name");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds);

	UWidgetAnimation* Anim = NewObject<UWidgetAnimation>(WBP, FName(*AnimName), RF_Transactional);
	UMovieScene* MS = NewObject<UMovieScene>(Anim, NAME_None, RF_Transactional);
	Anim->MovieScene = MS;
	const FFrameRate TickResolution = MS->GetTickResolution();
	const FFrameNumber Start = TickResolution.AsFrameNumber(0.0);
	const FFrameNumber End   = TickResolution.AsFrameNumber(DurationSeconds);
	MS->SetPlaybackRange(TRange<FFrameNumber>(Start, End));

	WBP->Animations.Add(Anim);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("animation_name"), AnimName);
	Data->SetNumberField(TEXT("duration_seconds"), DurationSeconds);

	Result.bSuccess = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData = SerializeToString(Data);
	Result.Message = FString::Printf(TEXT("Added widget animation '%s' (%.2fs)"), *AnimName, DurationSeconds);
	return Result;
}

FBridgeResult UUMGHandler::Action_ListWidgetAnimations(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("list_widget_animations"));
	FString Err;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
	if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (UWidgetAnimation* A : WBP->Animations)
	{
		if (!A) continue;
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), A->GetName());
		if (A->MovieScene)
		{
			const TRange<FFrameNumber> Range = A->MovieScene->GetPlaybackRange();
			O->SetNumberField(TEXT("frame_start"), Range.GetLowerBoundValue().Value);
			O->SetNumberField(TEXT("frame_end"),   Range.GetUpperBoundValue().Value);
			O->SetNumberField(TEXT("track_count"), A->MovieScene->GetTracks().Num());
		}
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("animations"), Arr);
	Data->SetNumberField(TEXT("count"), Arr.Num());
	Result.bSuccess = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData = SerializeToString(Data);
	Result.Message = FString::Printf(TEXT("%d animation(s)"), Arr.Num());
	return Result;
}

FBridgeResult UUMGHandler::Action_RemoveWidgetAnimation(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("remove_widget_animation"));
	FString Err, AnimName;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
	if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
	if (!Params->TryGetStringField(TEXT("name"), AnimName) || AnimName.IsEmpty())
	{
		Result.Message = TEXT("remove_widget_animation requires: name");
		Result.ErrorCode = 1000;
		return Result;
	}
	const FName Target(*AnimName);
	int32 Removed = 0;
	for (int32 i = WBP->Animations.Num() - 1; i >= 0; --i)
	{
		if (WBP->Animations[i] && WBP->Animations[i]->GetFName() == Target)
		{
			WBP->Animations.RemoveAt(i);
			++Removed;
		}
	}
	if (Removed == 0)
	{
		Result.Message = FString::Printf(TEXT("Animation '%s' not found"), *AnimName);
		Result.ErrorCode = 2000;
		return Result;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	Result.bSuccess = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message = FString::Printf(TEXT("Removed animation '%s'"), *AnimName);
	return Result;
}

FBridgeResult UUMGHandler::Action_BindWidgetEvent(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("bind_widget_event"));
	FString Err, WidgetName, EventName, FunctionName;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
	if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("event_name"),  EventName)  || EventName.IsEmpty())
	{
		Result.Message = TEXT("bind_widget_event requires: widget_name, event_name (function_name optional — auto-derived if omitted)");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("function_name"), FunctionName);

	// 1) Resolve the widget by name in the widget tree
	UWidget* Widget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget)
	{
		Result.Message = FString::Printf(TEXT("Widget '%s' not found in WBP tree"), *WidgetName);
		Result.ErrorCode = 2000;
		return Result;
	}

	// 2) Find the multicast delegate property on the widget class
	FMulticastDelegateProperty* DelegateProp = nullptr;
	for (TFieldIterator<FMulticastDelegateProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		if (PropIt->GetName() == EventName) { DelegateProp = *PropIt; break; }
	}
	if (!DelegateProp)
	{
		Result.Message = FString::Printf(TEXT("No multicast delegate '%s' on '%s'"),
			*EventName, *Widget->GetClass()->GetName());
		Result.ErrorCode = 2001;
		return Result;
	}

	// 3) Resolve event graph (UbergraphPages[0])
	UEdGraph* EventGraph = WBP->UbergraphPages.Num() > 0 ? WBP->UbergraphPages[0] : nullptr;
	if (!EventGraph)
	{
		Result.Message = TEXT("WBP has no UbergraphPages");
		Result.ErrorCode = 3000;
		return Result;
	}

	// 4) Idempotence: if a UK2Node_ComponentBoundEvent for this widget+event already exists, return success
	for (UEdGraphNode* N : EventGraph->Nodes)
	{
		UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(N);
		if (!Bound) continue;
		if (Bound->ComponentPropertyName == FName(*WidgetName) &&
		    Bound->DelegatePropertyName == FName(*EventName))
		{
			TSharedPtr<FJsonObject> ExistingData = MakeShared<FJsonObject>();
			ExistingData->SetStringField(TEXT("node_guid"), Bound->NodeGuid.ToString());
			ExistingData->SetStringField(TEXT("custom_function_name"), Bound->CustomFunctionName.ToString());
			ExistingData->SetBoolField(TEXT("idempotent"), true);
			Result.bSuccess = true;
			Result.AffectedPath = WBP->GetPathName();
			Result.ExtraData = SerializeToString(ExistingData);
			Result.Message = FString::Printf(TEXT("Already bound: '%s'.%s -> %s"),
				*WidgetName, *EventName, *Bound->CustomFunctionName.ToString());
			return Result;
		}
	}

	// 5) Spawn the bound-event node and initialize via the documented helper
	UK2Node_ComponentBoundEvent* BoundEvent = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
	EventGraph->AddNode(BoundEvent, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	BoundEvent->NodePosX = 0;
	BoundEvent->NodePosY = 0;
	BoundEvent->CreateNewGuid();

	// InitializeComponentBoundEventParams in 5.7 takes (FObjectProperty const*, FMulticastDelegateProperty const*).
	// The FObjectProperty is the named-widget variable on the WBP's generated class
	// (UMG auto-generates one UObjectProperty per named widget).
	FObjectProperty* ComponentProperty = nullptr;
	if (UClass* GenClass = WBP->GeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(GenClass, FName(*WidgetName));
	}
	if (!ComponentProperty)
	{
		Result.Message = FString::Printf(
			TEXT("Could not find FObjectProperty '%s' on WBP generated class — recompile the WBP first so the widget variable exists"),
			*WidgetName);
		Result.ErrorCode = 3002;
		return Result;
	}
	BoundEvent->InitializeComponentBoundEventParams(ComponentProperty, DelegateProp);

	// Optional override of CustomFunctionName (auto-derived as "BndEvt__<Widget>_<Event>_K2Node_..." otherwise)
	if (!FunctionName.IsEmpty())
	{
		BoundEvent->CustomFunctionName = FName(*FunctionName);
	}

	BoundEvent->PostPlacedNewNode();
	BoundEvent->AllocateDefaultPins();
	BoundEvent->ReconstructNode();

	// 6) Recompile so the runtime binding is generated by the WBP compiler
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_guid"), BoundEvent->NodeGuid.ToString());
	Data->SetStringField(TEXT("custom_function_name"), BoundEvent->CustomFunctionName.ToString());
	Data->SetStringField(TEXT("widget_name"), WidgetName);
	Data->SetStringField(TEXT("event_name"), EventName);
	Result.bSuccess = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.ExtraData = SerializeToString(Data);
	Result.Message = FString::Printf(TEXT("Bound '%s'.%s -> %s"),
		*WidgetName, *EventName, *BoundEvent->CustomFunctionName.ToString());
	return Result;
}

FBridgeResult UUMGHandler::Action_AddNamedSlot(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("add_named_slot"));
	FString Err, SlotName, ParentName;
	UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
	if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
	if (!Params->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
	{
		Result.Message = TEXT("add_named_slot requires: slot_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		Result.Message = TEXT("WBP has no WidgetTree");
		Result.ErrorCode = 3000;
		return Result;
	}

	UNamedSlot* NewSlot = Tree->ConstructWidget<UNamedSlot>(UNamedSlot::StaticClass(), FName(*SlotName));
	if (!NewSlot)
	{
		Result.Message = TEXT("Failed to construct UNamedSlot");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Parent under named widget if specified, else as root child
	UWidget* ParentWidget = nullptr;
	if (!ParentName.IsEmpty())
	{
		ParentWidget = Tree->FindWidget(FName(*ParentName));
	}
	if (UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget))
	{
		ParentPanel->AddChild(NewSlot);
	}
	else if (!Tree->RootWidget)
	{
		Tree->RootWidget = NewSlot;
	}
	else if (UPanelWidget* RootAsPanel = Cast<UPanelWidget>(Tree->RootWidget))
	{
		RootAsPanel->AddChild(NewSlot);
	}
	else
	{
		Result.Message = TEXT("Could not find a parent panel to host the named slot");
		Result.ErrorCode = 3000;
		return Result;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	Result.bSuccess = true;
	Result.AffectedPath = WBP->GetPathName();
	Result.Message = FString::Printf(TEXT("Added NamedSlot '%s'"), *SlotName);
	return Result;
}

// ===========================================================================
// Wave 9: UMG MovieScene track/keyframe authoring + event binding
// ===========================================================================

#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimationBinding.h"
// (Already included transitively in some configs but pinning explicitly here)
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"

namespace
{
    UWidgetAnimation* FindWidgetAnimation(UWidgetBlueprint* WBP, const FString& AnimName)
    {
        if (!WBP) return nullptr;
        const FName Target(*AnimName);
        for (UWidgetAnimation* A : WBP->Animations)
        {
            if (A && A->GetFName() == Target) return A;
        }
        return nullptr;
    }

    TSubclassOf<UMovieSceneTrack> ResolveWidgetTrackClass(const FString& Kind)
    {
        const FString K = Kind.ToLower();
        if (K == TEXT("float"))      return UMovieSceneFloatTrack::StaticClass();
        if (K == TEXT("visibility")) return UMovieSceneVisibilityTrack::StaticClass();
        if (K == TEXT("color"))      return UMovieSceneColorTrack::StaticClass();
        if (K == TEXT("transform"))  return UMovieScene2DTransformTrack::StaticClass();
        if (K == TEXT("margin"))     return UMovieSceneMarginTrack::StaticClass();
        if (K == TEXT("material"))   return UMovieSceneWidgetMaterialTrack::StaticClass();
        if (K == TEXT("audio"))      return UMovieSceneAudioTrack::StaticClass();
        if (K == TEXT("event"))      return UMovieSceneEventTrack::StaticClass();
        return nullptr;
    }
}

FBridgeResult UUMGHandler::Action_AddWidgetAnimBinding(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("add_widget_anim_binding"));
    FString Err, AnimName, WidgetName;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    { Result.Message = TEXT("'widget_name' required"); Result.ErrorCode = 1000; return Result; }

    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim || !Anim->MovieScene)
    { Result.Message = FString::Printf(TEXT("Animation '%s' not found"), *AnimName); Result.ErrorCode = 2000; return Result; }
    UWidget* W = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
    if (!W)
    { Result.Message = FString::Printf(TEXT("Widget '%s' not found in tree"), *WidgetName); Result.ErrorCode = 2000; return Result; }

    const FGuid PossessableGuid = Anim->MovieScene->AddPossessable(WidgetName, W->GetClass());

    FWidgetAnimationBinding Binding;
    Binding.WidgetName = FName(*WidgetName);
    Binding.AnimationGuid = PossessableGuid;
    Binding.bIsRootWidget = (W == WBP->WidgetTree->RootWidget);
    Anim->AnimationBindings.Add(Binding);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("binding_guid"), PossessableGuid.ToString());
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.ExtraData = SerializeToString(Data);
    Result.Message = FString::Printf(TEXT("Bound widget '%s' to animation '%s' (guid=%s)"),
        *WidgetName, *AnimName, *PossessableGuid.ToString());
    return Result;
}

FBridgeResult UUMGHandler::Action_ListWidgetAnimBindings(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("list_widget_anim_bindings"));
    FString Err, AnimName;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim) { Result.Message = TEXT("Animation not found"); Result.ErrorCode = 2000; return Result; }

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FWidgetAnimationBinding& B : Anim->AnimationBindings)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("widget_name"), B.WidgetName.ToString());
        O->SetStringField(TEXT("slot_widget_name"), B.SlotWidgetName.ToString());
        O->SetStringField(TEXT("guid"), B.AnimationGuid.ToString());
        O->SetBoolField(TEXT("is_root_widget"), B.bIsRootWidget);
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("bindings"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.ExtraData = SerializeToString(Data);
    Result.Message = FString::Printf(TEXT("%d binding(s) on animation '%s'"), Arr.Num(), *AnimName);
    return Result;
}

FBridgeResult UUMGHandler::Action_AddWidgetAnimTrack(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("add_widget_anim_track"));
    FString Err, AnimName, BindingGuidStr, TrackKind, PropertyName;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr) || BindingGuidStr.IsEmpty())
    { Result.Message = TEXT("'binding_guid' required (from add_widget_anim_binding)"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("track_kind"), TrackKind) || TrackKind.IsEmpty())
    { Result.Message = TEXT("'track_kind' required (float|visibility|color|transform|margin|material|audio|event)"); Result.ErrorCode = 1000; return Result; }
    Params->TryGetStringField(TEXT("property_name"), PropertyName);

    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim || !Anim->MovieScene) { Result.Message = TEXT("Animation not found"); Result.ErrorCode = 2000; return Result; }

    FGuid Guid;
    if (!FGuid::Parse(BindingGuidStr, Guid)) { Result.Message = TEXT("Invalid binding_guid"); Result.ErrorCode = 1001; return Result; }

    TSubclassOf<UMovieSceneTrack> Cls = ResolveWidgetTrackClass(TrackKind);
    if (!Cls) { Result.Message = FString::Printf(TEXT("Unknown track_kind: %s"), *TrackKind); Result.ErrorCode = 1001; return Result; }

    UMovieSceneTrack* Track = Anim->MovieScene->AddTrack(Cls, Guid);
    if (!Track) { Result.Message = TEXT("AddTrack returned null"); Result.ErrorCode = 3000; return Result; }
    if (!PropertyName.IsEmpty())
    {
        if (UMovieScenePropertyTrack* PropTrack = Cast<UMovieScenePropertyTrack>(Track))
        {
            PropTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
        }
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Data->SetStringField(TEXT("track_kind"), TrackKind);
    Data->SetStringField(TEXT("binding_guid"), BindingGuidStr);
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.ExtraData = SerializeToString(Data);
    Result.Message = FString::Printf(TEXT("Added %s track to binding %s"), *TrackKind, *BindingGuidStr);
    return Result;
}

FBridgeResult UUMGHandler::Action_RemoveWidgetAnimTrack(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("remove_widget_anim_track"));
    FString Err, AnimName, BindingGuidStr, TrackClassName;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr) || BindingGuidStr.IsEmpty())
    { Result.Message = TEXT("'binding_guid' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("track_class"), TrackClassName) || TrackClassName.IsEmpty())
    { Result.Message = TEXT("'track_class' required (e.g. MovieSceneFloatTrack)"); Result.ErrorCode = 1000; return Result; }
    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim || !Anim->MovieScene) { Result.Message = TEXT("Animation not found"); Result.ErrorCode = 2000; return Result; }
    FGuid Guid;
    if (!FGuid::Parse(BindingGuidStr, Guid)) { Result.Message = TEXT("Invalid binding_guid"); Result.ErrorCode = 1001; return Result; }

    int32 Removed = 0;
    TArray<UMovieSceneTrack*> AllTracks = Anim->MovieScene->FindTracks(UMovieSceneTrack::StaticClass(), Guid, NAME_None);
    for (UMovieSceneTrack* T : AllTracks)
    {
        if (T && T->GetClass()->GetName() == TrackClassName)
        {
            Anim->MovieScene->RemoveTrack(*T);
            ++Removed;
        }
    }
    if (Removed == 0) { Result.Message = FString::Printf(TEXT("No tracks of class '%s' on binding"), *TrackClassName); Result.ErrorCode = 2000; return Result; }
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.Message = FString::Printf(TEXT("Removed %d track(s)"), Removed);
    return Result;
}

FBridgeResult UUMGHandler::Action_ListWidgetAnimTracks(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("list_widget_anim_tracks"));
    FString Err, AnimName, BindingGuidStr;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim || !Anim->MovieScene) { Result.Message = TEXT("Animation not found"); Result.ErrorCode = 2000; return Result; }

    TArray<TSharedPtr<FJsonValue>> Arr;
    if (Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr) && !BindingGuidStr.IsEmpty())
    {
        FGuid Guid;
        if (!FGuid::Parse(BindingGuidStr, Guid)) { Result.Message = TEXT("Invalid binding_guid"); Result.ErrorCode = 1001; return Result; }
        TArray<UMovieSceneTrack*> Tracks = Anim->MovieScene->FindTracks(UMovieSceneTrack::StaticClass(), Guid, NAME_None);
        for (UMovieSceneTrack* T : Tracks)
        {
            if (!T) continue;
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("track_class"), T->GetClass()->GetName());
            O->SetStringField(TEXT("track_name"), T->GetTrackName().ToString());
            O->SetNumberField(TEXT("section_count"), T->GetAllSections().Num());
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    else
    {
        for (UMovieSceneTrack* T : Anim->MovieScene->GetTracks())
        {
            if (!T) continue;
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("track_class"), T->GetClass()->GetName());
            O->SetStringField(TEXT("track_name"), T->GetTrackName().ToString());
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("tracks"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.ExtraData = SerializeToString(Data);
    Result.Message = FString::Printf(TEXT("%d track(s)"), Arr.Num());
    return Result;
}

FBridgeResult UUMGHandler::Action_AddWidgetAnimKeyframeFloat(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("add_widget_anim_keyframe_float"));
    FString Err, AnimName, BindingGuidStr, TrackClassName;
    double TimeSeconds = 0.0, Value = 0.0;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
    { Result.Message = TEXT("'animation_name' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr) || BindingGuidStr.IsEmpty())
    { Result.Message = TEXT("'binding_guid' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
    { Result.Message = TEXT("'time_seconds' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetNumberField(TEXT("value"), Value))
    { Result.Message = TEXT("'value' required"); Result.ErrorCode = 1000; return Result; }
    Params->TryGetStringField(TEXT("track_class"), TrackClassName);

    UWidgetAnimation* Anim = FindWidgetAnimation(WBP, AnimName);
    if (!Anim || !Anim->MovieScene) { Result.Message = TEXT("Animation not found"); Result.ErrorCode = 2000; return Result; }
    FGuid Guid;
    if (!FGuid::Parse(BindingGuidStr, Guid)) { Result.Message = TEXT("Invalid binding_guid"); Result.ErrorCode = 1001; return Result; }

    TArray<UMovieSceneTrack*> Tracks = Anim->MovieScene->FindTracks(UMovieSceneTrack::StaticClass(), Guid, NAME_None);
    UMovieSceneFloatTrack* FloatTrack = nullptr;
    for (UMovieSceneTrack* T : Tracks)
    {
        if (!T) continue;
        if (TrackClassName.IsEmpty() || T->GetClass()->GetName() == TrackClassName)
        {
            FloatTrack = Cast<UMovieSceneFloatTrack>(T);
            if (FloatTrack) break;
        }
    }
    if (!FloatTrack) { Result.Message = TEXT("No UMovieSceneFloatTrack on this binding (use add_widget_anim_track first)"); Result.ErrorCode = 2000; return Result; }

    UMovieSceneSection* Section = nullptr;
    if (FloatTrack->GetAllSections().Num() == 0)
    {
        Section = FloatTrack->CreateNewSection();
        FloatTrack->AddSection(*Section);
    }
    else
    {
        Section = FloatTrack->GetAllSections()[0];
    }
    if (!Section) { Result.Message = TEXT("Could not create/find float section"); Result.ErrorCode = 3000; return Result; }

    const FFrameRate Resolution = Anim->MovieScene->GetTickResolution();
    const FFrameNumber Frame = Resolution.AsFrameNumber(TimeSeconds);

    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    FMovieSceneFloatChannel* Channel = Proxy.GetChannel<FMovieSceneFloatChannel>(0);
    if (!Channel) { Result.Message = TEXT("Float channel 0 not found on section"); Result.ErrorCode = 3000; return Result; }
    Channel->AddCubicKey(Frame, (float)Value);

    // Expand the section bounds to include the new key, if needed
    Section->ExpandToFrame(Frame);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.Message = FString::Printf(TEXT("Added float keyframe (t=%.4fs, v=%.4f)"), TimeSeconds, Value);
    return Result;
}

// ----------------------------------------------------------------------------
// Wave 9: UMG event delegate binding (replaces Wave 8 3003 stub with real impl)
// ----------------------------------------------------------------------------
namespace
{
    UEdGraph* GetWBPEventGraph(UWidgetBlueprint* WBP)
    {
        if (!WBP) return nullptr;
        for (UEdGraph* G : WBP->UbergraphPages) if (G) return G;
        return nullptr;
    }
}

// Override the previous bind_widget_event impl with a real graph-spawn version.
// (We reuse the already-declared Action_BindWidgetEvent slot — its earlier 3003
// body lives at the original cpp position; this is a Wave 9 second-iteration
// fork. To avoid dual-definition link errors, we route the dispatcher to a new
// distinct symbol — Action_UnbindWidgetEvent / Action_ListWidgetEventBindings —
// and let the original Action_BindWidgetEvent continue to issue 3003. The new
// list/unbind verbs cover query and removal, which is the most-asked-for half
// of the lifecycle. Real edit-time spawning of UK2Node_ComponentBoundEvent is
// recorded as Wave 9 risk (see checkpoint).
FBridgeResult UUMGHandler::Action_UnbindWidgetEvent(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("unbind_widget_event"));
    FString Err, WidgetName, EventName;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    { Result.Message = TEXT("'widget_name' required"); Result.ErrorCode = 1000; return Result; }
    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
    { Result.Message = TEXT("'event_name' required"); Result.ErrorCode = 1000; return Result; }

    UEdGraph* EventGraph = GetWBPEventGraph(WBP);
    if (!EventGraph) { Result.Message = TEXT("WBP has no UbergraphPages"); Result.ErrorCode = 3000; return Result; }

    int32 Removed = 0;
    TArray<UEdGraphNode*> Nodes = EventGraph->Nodes;
    for (UEdGraphNode* N : Nodes)
    {
        UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(N);
        if (!Bound) continue;
        if (Bound->ComponentPropertyName == FName(*WidgetName) &&
            Bound->DelegatePropertyName == FName(*EventName))
        {
            EventGraph->RemoveNode(Bound);
            ++Removed;
        }
    }
    if (Removed == 0) { Result.Message = TEXT("No matching bound-event node found"); Result.ErrorCode = 2000; return Result; }
    FKismetEditorUtilities::CompileBlueprint(WBP);
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.Message = FString::Printf(TEXT("Unbound %d event handler(s) for '%s.%s'"), Removed, *WidgetName, *EventName);
    return Result;
}

FBridgeResult UUMGHandler::Action_ListWidgetEventBindings(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("umg"), TEXT("list_widget_event_bindings"));
    FString Err;
    UWidgetBlueprint* WBP = ResolveWBP(Params, Err);
    if (!WBP) { Result.Message = Err; Result.ErrorCode = 2000; return Result; }

    UEdGraph* EventGraph = GetWBPEventGraph(WBP);
    TArray<TSharedPtr<FJsonValue>> Arr;
    if (EventGraph)
    {
        for (UEdGraphNode* N : EventGraph->Nodes)
        {
            UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(N);
            if (!Bound) continue;
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("widget_name"), Bound->ComponentPropertyName.ToString());
            O->SetStringField(TEXT("event_name"), Bound->DelegatePropertyName.ToString());
            O->SetStringField(TEXT("custom_function_name"), Bound->CustomFunctionName.ToString());
            O->SetStringField(TEXT("node_guid"), Bound->NodeGuid.ToString());
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("bindings"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Result.bSuccess = true;
    Result.AffectedPath = WBP->GetPathName();
    Result.ExtraData = SerializeToString(Data);
    Result.Message = FString::Printf(TEXT("%d component-bound event(s)"), Arr.Num());
    return Result;
}
