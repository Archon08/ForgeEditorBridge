#include "Handlers/StructHandler.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/StructureEditorUtils.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("struct");

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create"))           return Action_Create(Params);
	if (Action == TEXT("add_field"))        return Action_AddField(Params);
	if (Action == TEXT("remove_field"))     return Action_RemoveField(Params);
	if (Action == TEXT("rename_field"))     return Action_RenameField(Params);
	if (Action == TEXT("set_field_default"))return Action_SetFieldDefault(Params);
	if (Action == TEXT("list_fields"))      return Action_ListFields(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown struct action '%s'"), *Action));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

UUserDefinedStruct* UStructHandler::LoadStruct(const FString& AssetPath, FBridgeResult& Result) const
{
	UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Struct)
	{
		Result = MakeError(DOMAIN, TEXT("struct"), 2001,
			FString::Printf(TEXT("UUserDefinedStruct not found: '%s'"), *AssetPath));
	}
	return Struct;
}

bool UStructHandler::ParseFieldType(const FString& TypeStr, FEdGraphPinType& OutPinType)
{
	OutPinType = FEdGraphPinType();

	if (TypeStr.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (TypeStr.Equals(TEXT("int"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("int32"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (TypeStr.Equals(TEXT("int64"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	// In UE 5.7 PC_Float was removed; float = PC_Real + PC_Double subcategory
	if (TypeStr.Equals(TEXT("float"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("double"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory    = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (TypeStr.Equals(TEXT("string"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FString"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (TypeStr.Equals(TEXT("name"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FName"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (TypeStr.Equals(TEXT("text"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FText"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	if (TypeStr.Equals(TEXT("vector"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FVector"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory            = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject   = TBaseStructure<FVector>::Get();
		return true;
	}
	if (TypeStr.Equals(TEXT("rotator"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FRotator"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory            = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject   = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (TypeStr.Equals(TEXT("transform"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FTransform"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory            = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject   = TBaseStructure<FTransform>::Get();
		return true;
	}
	if (TypeStr.Equals(TEXT("color"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("FLinearColor"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory            = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject   = TBaseStructure<FLinearColor>::Get();
		return true;
	}
	if (TypeStr.Equals(TEXT("object"), ESearchCase::IgnoreCase) ||
	    TypeStr.Equals(TEXT("UObject"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory          = UEdGraphSchema_K2::PC_Object;
		OutPinType.PinSubCategoryObject = UObject::StaticClass();
		return true;
	}

	return false;
}

FGuid UStructHandler::FindNewVarGuid(UUserDefinedStruct* Struct, const TSet<FGuid>& Before)
{
	for (const FStructVariableDescription& Desc : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (!Before.Contains(Desc.VarGuid))
			return Desc.VarGuid;
	}
	return FGuid();
}

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_Create(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("create"), 1000,
			TEXT("'asset_path' is required, e.g. '/Game/Data/S_MyStruct'"));

	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	if (AssetName.IsEmpty())
		return MakeError(DOMAIN, TEXT("create"), 1000,
			TEXT("'asset_path' must be a full package path"));

	// Create via IAssetTools + factory loaded by class name (avoids header dependency on UUserDefinedStructFactory)
	UClass* FactClass = FindObject<UClass>(nullptr, TEXT("/Script/StructureEditor.UserDefinedStructFactory"));
	if (!FactClass)
	{
		// Fallback: ensure the module is loaded then retry
		FModuleManager::LoadModuleChecked<IModuleInterface>("StructureEditor");
		FactClass = FindObject<UClass>(nullptr, TEXT("/Script/StructureEditor.UserDefinedStructFactory"));
	}

	UUserDefinedStruct* Struct = nullptr;

	if (FactClass)
	{
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactClass);
		FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Created = AT.CreateAsset(AssetName, PackagePath,
			UUserDefinedStruct::StaticClass(), Factory);
		Struct = Cast<UUserDefinedStruct>(Created);
	}

	if (!Struct)
	{
		// Raw fallback: create package + NewObject without factory
		UPackage* Package = CreatePackage(*AssetPath);
		Package->FullyLoad();
		Struct = NewObject<UUserDefinedStruct>(Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Struct)
			return MakeError(DOMAIN, TEXT("create"), 3000,
				FString::Printf(TEXT("Failed to create UUserDefinedStruct at '%s'"), *AssetPath));
		Struct->EditorData = NewObject<UUserDefinedStructEditorData>(Struct, NAME_None, RF_Transactional);
		Struct->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Struct);
	}

	int32 FieldsAdded = 0;

	// Process optional inline fields array: [{name, type}, ...]
	const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr)
	{
		for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArr)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!FieldVal->TryGetObject(FieldObj)) continue;

			FString FieldName, FieldType;
			(*FieldObj)->TryGetStringField(TEXT("name"), FieldName);
			(*FieldObj)->TryGetStringField(TEXT("type"), FieldType);
			if (FieldName.IsEmpty() || FieldType.IsEmpty()) continue;

			FEdGraphPinType PinType;
			if (!ParseFieldType(FieldType, PinType)) continue;

			TSet<FGuid> Before;
			for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
				Before.Add(D.VarGuid);

			if (FStructureEditorUtils::AddVariable(Struct, PinType))
			{
				FGuid NewGuid = FindNewVarGuid(Struct, Before);
				if (NewGuid.IsValid())
					FStructureEditorUtils::RenameVariable(Struct, NewGuid, FieldName);
				++FieldsAdded;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("fields_added"), FieldsAdded);

	return MakeSuccess(DOMAIN, TEXT("create"),
		FString::Printf(TEXT("Created struct '%s' (%d initial field(s))"), *AssetPath, FieldsAdded),
		Data);
}

// ---------------------------------------------------------------------------
// add_field
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_AddField(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_field"), 1000, TEXT("'asset_path' is required"));

	FString FieldName;
	if (!Params->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_field"), 1000, TEXT("'field_name' is required"));

	FString FieldType;
	if (!Params->TryGetStringField(TEXT("field_type"), FieldType) || FieldType.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_field"), 1000,
			TEXT("'field_type' is required: bool|int|int64|float|string|name|text|vector|rotator|transform|color|object"));

	FEdGraphPinType PinType;
	if (!ParseFieldType(FieldType, PinType))
		return MakeError(DOMAIN, TEXT("add_field"), 1000,
			FString::Printf(TEXT("Unrecognised field_type '%s'"), *FieldType));

	FBridgeResult ErrResult;
	UUserDefinedStruct* Struct = LoadStruct(AssetPath, ErrResult);
	if (!Struct) return ErrResult;

	// GUID-diff: capture before-set so we can find the new var to rename it
	TSet<FGuid> Before;
	for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
		Before.Add(D.VarGuid);

	if (!FStructureEditorUtils::AddVariable(Struct, PinType))
		return MakeError(DOMAIN, TEXT("add_field"), 3000,
			FString::Printf(TEXT("FStructureEditorUtils::AddVariable failed for '%s'"), *FieldName));

	FGuid NewGuid = FindNewVarGuid(Struct, Before);
	if (!NewGuid.IsValid())
		return MakeError(DOMAIN, TEXT("add_field"), 3000,
			TEXT("Could not determine GUID of newly added variable"));

	if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, FieldName))
		return MakeError(DOMAIN, TEXT("add_field"), 3000,
			FString::Printf(TEXT("RenameVariable failed for '%s'"), *FieldName));

	return MakeSuccess(DOMAIN, TEXT("add_field"),
		FString::Printf(TEXT("Added field '%s' (%s) to '%s'"), *FieldName, *FieldType, *AssetPath));
}

// ---------------------------------------------------------------------------
// remove_field
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_RemoveField(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("remove_field"), 1000, TEXT("'asset_path' is required"));

	FString FieldName;
	if (!Params->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		return MakeError(DOMAIN, TEXT("remove_field"), 1000, TEXT("'field_name' is required"));

	FBridgeResult ErrResult;
	UUserDefinedStruct* Struct = LoadStruct(AssetPath, ErrResult);
	if (!Struct) return ErrResult;

	// Find the GUID for the named field
	FGuid TargetGuid;
	for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (D.FriendlyName.Equals(FieldName, ESearchCase::IgnoreCase) ||
		    D.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
		{
			TargetGuid = D.VarGuid;
			break;
		}
	}

	if (!TargetGuid.IsValid())
		return MakeError(DOMAIN, TEXT("remove_field"), 2001,
			FString::Printf(TEXT("Field '%s' not found in struct '%s'"), *FieldName, *AssetPath));

	if (!FStructureEditorUtils::RemoveVariable(Struct, TargetGuid))
		return MakeError(DOMAIN, TEXT("remove_field"), 3000,
			FString::Printf(TEXT("RemoveVariable failed for '%s'"), *FieldName));

	return MakeSuccess(DOMAIN, TEXT("remove_field"),
		FString::Printf(TEXT("Removed field '%s' from '%s'"), *FieldName, *AssetPath));
}

// ---------------------------------------------------------------------------
// rename_field
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_RenameField(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("rename_field"), 1000, TEXT("'asset_path' is required"));

	FString OldName, NewName;
	if (!Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
		return MakeError(DOMAIN, TEXT("rename_field"), 1000, TEXT("'old_name' is required"));
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		return MakeError(DOMAIN, TEXT("rename_field"), 1000, TEXT("'new_name' is required"));

	FBridgeResult ErrResult;
	UUserDefinedStruct* Struct = LoadStruct(AssetPath, ErrResult);
	if (!Struct) return ErrResult;

	FGuid TargetGuid;
	for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (D.FriendlyName.Equals(OldName, ESearchCase::IgnoreCase) ||
		    D.VarName.ToString().Equals(OldName, ESearchCase::IgnoreCase))
		{
			TargetGuid = D.VarGuid;
			break;
		}
	}

	if (!TargetGuid.IsValid())
		return MakeError(DOMAIN, TEXT("rename_field"), 2001,
			FString::Printf(TEXT("Field '%s' not found in struct '%s'"), *OldName, *AssetPath));

	if (!FStructureEditorUtils::RenameVariable(Struct, TargetGuid, NewName))
		return MakeError(DOMAIN, TEXT("rename_field"), 3000,
			FString::Printf(TEXT("RenameVariable failed: '%s' → '%s'"), *OldName, *NewName));

	return MakeSuccess(DOMAIN, TEXT("rename_field"),
		FString::Printf(TEXT("Renamed field '%s' → '%s' in '%s'"), *OldName, *NewName, *AssetPath));
}

// ---------------------------------------------------------------------------
// set_field_default
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_SetFieldDefault(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_field_default"), 1000, TEXT("'asset_path' is required"));

	FString FieldName;
	if (!Params->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_field_default"), 1000, TEXT("'field_name' is required"));

	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	FBridgeResult ErrResult;
	UUserDefinedStruct* Struct = LoadStruct(AssetPath, ErrResult);
	if (!Struct) return ErrResult;

	FGuid TargetGuid;
	for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (D.FriendlyName.Equals(FieldName, ESearchCase::IgnoreCase) ||
		    D.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
		{
			TargetGuid = D.VarGuid;
			break;
		}
	}

	if (!TargetGuid.IsValid())
		return MakeError(DOMAIN, TEXT("set_field_default"), 2001,
			FString::Printf(TEXT("Field '%s' not found in struct '%s'"), *FieldName, *AssetPath));

	if (!FStructureEditorUtils::ChangeVariableDefaultValue(Struct, TargetGuid, DefaultValue))
		return MakeError(DOMAIN, TEXT("set_field_default"), 3000,
			FString::Printf(TEXT("ChangeVariableDefaultValue failed for '%s'"), *FieldName));

	return MakeSuccess(DOMAIN, TEXT("set_field_default"),
		FString::Printf(TEXT("Set default for field '%s' to '%s' in '%s'"),
			*FieldName, *DefaultValue, *AssetPath));
}

// ---------------------------------------------------------------------------
// list_fields
// ---------------------------------------------------------------------------

FBridgeResult UStructHandler::Action_ListFields(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("list_fields"), 1000, TEXT("'asset_path' is required"));

	FBridgeResult ErrResult;
	UUserDefinedStruct* Struct = LoadStruct(AssetPath, ErrResult);
	if (!Struct) return ErrResult;

	TArray<TSharedPtr<FJsonValue>> Fields;
	for (const FStructVariableDescription& D : FStructureEditorUtils::GetVarDesc(Struct))
	{
		TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("name"),          D.FriendlyName);
		F->SetStringField(TEXT("var_name"),       D.VarName.ToString());
		// UE 5.7: FStructVariableDescription stores type info as decomposed fields.
		// Category/SubCategory are FName; there is no PinType/VarType sub-struct.
		F->SetStringField(TEXT("type"),           D.Category.ToString());
		F->SetStringField(TEXT("sub_category"),   D.SubCategory.ToString());
		F->SetStringField(TEXT("default_value"),  D.DefaultValue);
		F->SetStringField(TEXT("guid"),           D.VarGuid.ToString());
		Fields.Add(MakeShared<FJsonValueObject>(F));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("fields"), Fields);
	Data->SetNumberField(TEXT("count"), Fields.Num());
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	return MakeSuccess(DOMAIN, TEXT("list_fields"),
		FString::Printf(TEXT("'%s' has %d field(s)"), *AssetPath, Fields.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UStructHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto MakeParam = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetBoolField(TEXT("required"), bReq);
		P->SetStringField(TEXT("desc"), Desc);
		return P;
	};

	static const FString TypeList = TEXT("bool|int|int64|float|string|name|text|vector|rotator|transform|color|object");

	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"),        true,  TEXT("Full package path e.g. /Game/Data/S_MyStruct")));
		Pm->SetObjectField(TEXT("fields"),     MakeParam(TEXT("array[{name,type}]"), false, TEXT("Optional initial fields")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Create a new UUserDefinedStruct Blueprintable data asset"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("create"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Package path to existing struct")));
		Pm->SetObjectField(TEXT("field_name"), MakeParam(TEXT("string"), true, TEXT("Name for the new field")));
		Pm->SetObjectField(TEXT("field_type"), MakeParam(TEXT("string"), true, TypeList));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a typed variable to the struct (uses GUID-diff to reliably rename)"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("add_field"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Package path to existing struct")));
		Pm->SetObjectField(TEXT("field_name"), MakeParam(TEXT("string"), true, TEXT("Name of field to remove")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Remove a variable from the struct"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("remove_field"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Package path to existing struct")));
		Pm->SetObjectField(TEXT("old_name"),   MakeParam(TEXT("string"), true, TEXT("Current field name")));
		Pm->SetObjectField(TEXT("new_name"),   MakeParam(TEXT("string"), true, TEXT("New field name — propagates to DataTables")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Rename a struct field; FStructureEditorUtils propagates the rename to dependent DataTables"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("rename_field"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"),   MakeParam(TEXT("string"), true,  TEXT("Package path to existing struct")));
		Pm->SetObjectField(TEXT("field_name"),   MakeParam(TEXT("string"), true,  TEXT("Name of the field to modify")));
		Pm->SetObjectField(TEXT("default_value"),MakeParam(TEXT("string"), false, TEXT("New default value as string")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the default value for a struct field"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_field_default"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Package path to existing struct")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Return all fields as [{name, var_name, type, sub_category, default_value, guid}]"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("list_fields"), A);
	}

	return Root;
}
