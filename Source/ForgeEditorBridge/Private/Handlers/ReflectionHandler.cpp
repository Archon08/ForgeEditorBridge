#include "Handlers/ReflectionHandler.h"
#include "ForgeAISubsystem.h"
#include "Capture/ForgeSymbolCapture.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- Reflection ------------------------------------------------------------
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/PropertyPortFlags.h"

// ---- Asset loading ---------------------------------------------------------
#include "Misc/PackageName.h"

// ---- Blutility / Editor Utility Widget -------------------------------------
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilitySubsystem.h"

// ---- Editor ----------------------------------------------------------------
#include "Editor.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UReflectionHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("reflection"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("get_prop"))        return Action_GetProp(Params);
	if (Action == TEXT("set_prop"))        return Action_SetProp(Params);
	if (Action == TEXT("call_utility_fn")) return Action_CallUtilityFn(Params);
	if (Action == TEXT("run_euw"))         return Action_RunEUW(Params);
	if (Action == TEXT("list_functions")) return Action_ListFunctions(Params);

	if (Action == TEXT("read_symbol_capture"))
	{
		if (Subsystem && Subsystem->SymbolCapture)
			Subsystem->SymbolCapture->ExportSymbolIndex();
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("symbols/index.json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(TEXT("reflection"), Action, TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}

	FBridgeResult R = CreateResult(TEXT("reflection"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown reflection action '%s'. Valid: get_prop, set_prop, call_utility_fn, run_euw, list_functions, read_symbol_capture"),
		*Action);
	return R;
}

// ---------------------------------------------------------------------------
// get_prop
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::Action_GetProp(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("reflection"), TEXT("get_prop"));

	FString AssetPath, PropertyName;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		Result.Message = TEXT("get_prop: 'asset_path' and 'property_name' are required");
		return Result;
	}

	UObject* Obj = LoadObjectByPath(AssetPath, Result);
	if (!Obj) return Result;

	// Find the property by name using UE reflection
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		Result.Message = FString::Printf(
			TEXT("get_prop: property '%s' not found on %s (%s)"),
			*PropertyName, *Obj->GetName(), *Obj->GetClass()->GetName());
		return Result;
	}

	// Export the property value to a string
	FString ValueStr;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Obj, PPF_None);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.ExtraData    = ValueStr;
	Result.Message      = FString::Printf(
		TEXT("%s.%s = %s"), *Obj->GetName(), *PropertyName, *ValueStr);
	return Result;
}

// ---------------------------------------------------------------------------
// set_prop
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::Action_SetProp(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("reflection"), TEXT("set_prop"));

	FString AssetPath, PropertyName, Value;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("value"),         Value))
	{
		Result.Message = TEXT("set_prop: 'asset_path', 'property_name', and 'value' are required");
		return Result;
	}

	UObject* Obj = LoadObjectByPath(AssetPath, Result);
	if (!Obj) return Result;

	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		Result.Message = FString::Printf(
			TEXT("set_prop: property '%s' not found on %s (%s)"),
			*PropertyName, *Obj->GetName(), *Obj->GetClass()->GetName());
		return Result;
	}

	// Import the value from string using UE reflection
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Obj, PPF_None);

	if (!ImportResult)
	{
		Result.Message = FString::Printf(
			TEXT("set_prop: failed to import value '%s' into property '%s' — type mismatch?"),
			*Value, *PropertyName);
		return Result;
	}

	Obj->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Set %s.%s = %s"), *Obj->GetName(), *PropertyName, *Value);
	return Result;
}

// ---------------------------------------------------------------------------
// call_utility_fn
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::Action_CallUtilityFn(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("reflection"), TEXT("call_utility_fn"));

	FString AssetPath, FunctionName;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		Result.Message = TEXT("call_utility_fn: 'asset_path' and 'function_name' are required");
		return Result;
	}

	UObject* Obj = LoadObjectByPath(AssetPath, Result);
	if (!Obj) return Result;

	// Find the function on the object's class
	UFunction* Func = Obj->FindFunction(FName(*FunctionName));
	if (!Func)
	{
		Result.Message = FString::Printf(
			TEXT("call_utility_fn: function '%s' not found on %s (%s)"),
			*FunctionName, *Obj->GetName(), *Obj->GetClass()->GetName());
		return Result;
	}

	// Verify the function is safe to call (BlueprintCallable or CallInEditor)
	bool bCallable = Func->HasAnyFunctionFlags(FUNC_BlueprintCallable) ||
	                 Func->GetBoolMetaData(TEXT("CallInEditor"));
	if (!bCallable)
	{
		Result.Message = FString::Printf(
			TEXT("call_utility_fn: function '%s' is not BlueprintCallable or CallInEditor"),
			*FunctionName);
		return Result;
	}

	// Allocate parameter buffer and initialize defaults
	uint8* ParamBuffer = nullptr;
	if (Func->ParmsSize > 0)
	{
		ParamBuffer = (uint8*)FMemory::Malloc(Func->ParmsSize);
		FMemory::Memzero(ParamBuffer, Func->ParmsSize);

		// Try to populate parameters from the "args" JSON object
		TSharedPtr<FJsonObject> Args;
		if (Params->HasField(TEXT("args")))
		{
			Args = Params->GetObjectField(TEXT("args"));
		}

		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			FProperty* Param = *It;
			if (Param->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

			// Initialize with defaults
			Param->InitializeValue_InContainer(ParamBuffer);

			// Override from args if provided
			if (Args.IsValid())
			{
				FString ParamName = Param->GetName();
				FString ParamValue;
				if (Args->TryGetStringField(ParamName, ParamValue))
				{
					void* ParamPtr = Param->ContainerPtrToValuePtr<void>(ParamBuffer);
					Param->ImportText_Direct(*ParamValue, ParamPtr, Obj, PPF_None);
				}
			}
		}
	}

	// Invoke the function
	Obj->ProcessEvent(Func, ParamBuffer);

	// Extract return value if present
	FString ReturnValue;
	for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			const void* RetPtr = It->ContainerPtrToValuePtr<void>(ParamBuffer);
			It->ExportTextItem_Direct(ReturnValue, RetPtr, nullptr, Obj, PPF_None);
			break;
		}
	}

	if (ParamBuffer)
	{
		// Destroy param values
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(ParamBuffer);
		}
		FMemory::Free(ParamBuffer);
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.ExtraData    = ReturnValue;
	Result.Message      = FString::Printf(
		TEXT("Called %s.%s()%s"),
		*Obj->GetName(), *FunctionName,
		ReturnValue.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" → %s"), *ReturnValue));
	return Result;
}

// ---------------------------------------------------------------------------
// run_euw
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::Action_RunEUW(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("reflection"), TEXT("run_euw"));

	FString WidgetPath;
	if (!Params->TryGetStringField(TEXT("widget_path"), WidgetPath) || WidgetPath.IsEmpty())
	{
		Result.Message = TEXT("run_euw: 'widget_path' is required "
		                      "(e.g. '/Game/EditorUtilities/EUW_MyWidget')");
		return Result;
	}

	// Load the Editor Utility Widget Blueprint
	UEditorUtilityWidgetBlueprint* WidgetBP = LoadObject<UEditorUtilityWidgetBlueprint>(
		nullptr, *WidgetPath);
	if (!WidgetBP)
	{
		const FString Suffix = WidgetPath + TEXT(".") +
			FPackageName::GetLongPackageAssetName(WidgetPath);
		WidgetBP = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr, *Suffix);
	}
	if (!WidgetBP)
	{
		Result.Message = FString::Printf(
			TEXT("run_euw: EditorUtilityWidgetBlueprint not found at '%s'"), *WidgetPath);
		return Result;
	}

	// Launch the widget via the Editor Utility Subsystem
	UEditorUtilitySubsystem* EUSubsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
	if (!EUSubsystem)
	{
		Result.Message = TEXT("run_euw: UEditorUtilitySubsystem not available");
		return Result;
	}

	EUSubsystem->SpawnAndRegisterTab(WidgetBP);

	Result.bSuccess     = true;
	Result.AffectedPath = WidgetPath;
	Result.Message      = FString::Printf(TEXT("Editor Utility Widget launched: %s"), *WidgetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// list_functions
// ---------------------------------------------------------------------------

FBridgeResult UReflectionHandler::Action_ListFunctions(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("reflection"), TEXT("list_functions"),
			1000, TEXT("list_functions: 'asset_path' is required"),
			TEXT("Provide the content path of a UObject asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("reflection"), TEXT("list_functions"));
	UObject* Obj = LoadObjectByPath(AssetPath, TempResult);
	if (!Obj)
	{
		return MakeError(TEXT("reflection"), TEXT("list_functions"),
			2000, TempResult.Message,
			TEXT("Verify the asset path is correct and the asset exists"));
	}

	TArray<TSharedPtr<FJsonValue>> FunctionArray;
	for (TFieldIterator<UFunction> It(Obj->GetClass()); It; ++It)
	{
		UFunction* Func = *It;
		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Func->GetName());

		// Collect flag strings
		TArray<TSharedPtr<FJsonValue>> Flags;
		if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintCallable")));
		if (Func->GetBoolMetaData(TEXT("CallInEditor")))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("CallInEditor")));
		if (Func->HasAnyFunctionFlags(FUNC_Const))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("Const")));
		if (Func->HasAnyFunctionFlags(FUNC_BlueprintPure))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintPure")));
		if (Func->HasAnyFunctionFlags(FUNC_Event))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("Event")));
		if (Func->HasAnyFunctionFlags(FUNC_Net))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("Net")));
		if (Func->HasAnyFunctionFlags(FUNC_Static))
			Flags.Add(MakeShared<FJsonValueString>(TEXT("Static")));
		FuncObj->SetArrayField(TEXT("flags"), Flags);

		// Count parameters (excluding return param)
		int32 ParamCount = 0;
		for (TFieldIterator<FProperty> ParamIt(Func); ParamIt && (ParamIt->PropertyFlags & CPF_Parm); ++ParamIt)
		{
			if (!ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
				ParamCount++;
		}
		FuncObj->SetNumberField(TEXT("param_count"), ParamCount);

		FunctionArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("functions"), FunctionArray);
	Data->SetNumberField(TEXT("count"), FunctionArray.Num());

	return MakeSuccess(TEXT("reflection"), TEXT("list_functions"),
		FString::Printf(TEXT("Found %d function(s) on %s (%s)"),
			FunctionArray.Num(), *Obj->GetName(), *Obj->GetClass()->GetName()),
		Data);
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

UObject* UReflectionHandler::LoadObjectByPath(const FString& AssetPath, FBridgeResult& Result)
{
	UObject* Obj = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Obj)
	{
		const FString Suffix = AssetPath + TEXT(".") +
			FPackageName::GetLongPackageAssetName(AssetPath);
		Obj = LoadObject<UObject>(nullptr, *Suffix);
	}
	if (!Obj)
	{
		Result.Message = FString::Printf(
			TEXT("LoadObjectByPath: no UObject found at '%s'"), *AssetPath);
	}
	return Obj;
}
