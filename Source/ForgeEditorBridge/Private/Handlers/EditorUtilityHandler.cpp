#include "Handlers/EditorUtilityHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityObject.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("editor_utility");

FBridgeResult UEditorUtilityHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- run_euw ----
	if (Action == TEXT("run_euw"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

#if WITH_EDITOR
		UEditorUtilitySubsystem* EUS = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
		if (!EUS) return MakeError(DOMAIN, Action, 3000, TEXT("UEditorUtilitySubsystem not available"));

		UEditorUtilityWidgetBlueprint* WBP = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP)
		{
			// Try adding _C suffix
			WBP = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr, *(AssetPath + TEXT("_C")));
		}
		if (!WBP) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("EUW not found: %s"), *AssetPath));

		FName TabID;
		EUS->SpawnAndRegisterTabAndGetID(WBP, TabID);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("tab_id"), TabID.ToString());
		Data->SetBoolField(TEXT("spawned"), true);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("EUW spawned: %s"), *TabID.ToString()), Data);
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
	}

	// ---- run_eub ----
	if (Action == TEXT("run_eub"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

#if WITH_EDITOR
		UEditorUtilitySubsystem* EUS = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
		if (!EUS) return MakeError(DOMAIN, Action, 3000, TEXT("UEditorUtilitySubsystem not available"));

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!BP || !BP->GeneratedClass)
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Blueprint not found or not compiled: %s"), *AssetPath));

		UEditorUtilityObject* CDO = Cast<UEditorUtilityObject>(BP->GeneratedClass->GetDefaultObject());
		if (!CDO) return MakeError(DOMAIN, Action, 2001, FString::Printf(TEXT("Not an EditorUtilityObject: %s"), *AssetPath));

		EUS->TryRun(CDO->GetClass()->GetDefaultObject<UEditorUtilityObject>());

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("EUB executed: %s"), *AssetPath));
		R.AffectedPath = AssetPath;
		return R;
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
	}

	// ---- call_function ----
	if (Action == TEXT("call_function"))
	{
		FString AssetPath, FuncName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
		if (!Params->TryGetStringField(TEXT("function_name"), FuncName))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'function_name'"));

		UObject* Obj = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Obj) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Object not found: %s"), *AssetPath));

		// If it's a Blueprint, get the CDO
		if (UBlueprint* BP = Cast<UBlueprint>(Obj))
		{
			if (BP->GeneratedClass)
				Obj = BP->GeneratedClass->GetDefaultObject();
		}

		UFunction* Func = Obj->GetClass()->FindFunctionByName(FName(*FuncName));
		if (!Func)
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Function '%s' not found on %s"), *FuncName, *Obj->GetClass()->GetName()));

		// Allocate + init parameter buffer
		uint8* ParamBuf = (uint8*)FMemory_Alloca(Func->ParmsSize);
		FMemory::Memzero(ParamBuf, Func->ParmsSize);
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			It->InitializeValue_InContainer(ParamBuf);

		// Marshal from JSON args
		const TSharedPtr<FJsonObject>* ArgsObj;
		if (Params->TryGetObjectField(TEXT("args"), ArgsObj))
		{
			for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm); ++It)
			{
				FString PName = It->GetNameCPP();
				double NumVal; bool BoolVal; FString StrVal;

				if (FFloatProperty* FP = CastField<FFloatProperty>(*It))
				{
					if ((*ArgsObj)->TryGetNumberField(PName, NumVal)) FP->SetPropertyValue_InContainer(ParamBuf, (float)NumVal);
				}
				else if (FDoubleProperty* DP = CastField<FDoubleProperty>(*It))
				{
					if ((*ArgsObj)->TryGetNumberField(PName, NumVal)) DP->SetPropertyValue_InContainer(ParamBuf, NumVal);
				}
				else if (FIntProperty* IP = CastField<FIntProperty>(*It))
				{
					if ((*ArgsObj)->TryGetNumberField(PName, NumVal)) IP->SetPropertyValue_InContainer(ParamBuf, (int32)NumVal);
				}
				else if (FBoolProperty* BP = CastField<FBoolProperty>(*It))
				{
					if ((*ArgsObj)->TryGetBoolField(PName, BoolVal)) BP->SetPropertyValue_InContainer(ParamBuf, BoolVal);
				}
				else if (FStrProperty* SP = CastField<FStrProperty>(*It))
				{
					if ((*ArgsObj)->TryGetStringField(PName, StrVal)) SP->SetPropertyValue_InContainer(ParamBuf, StrVal);
				}
			}
		}

		Obj->ProcessEvent(Func, ParamBuf);

		// Cleanup
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			It->DestroyValue_InContainer(ParamBuf);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Called %s::%s"), *Obj->GetClass()->GetName(), *FuncName));
	}

	// ---- list_utilities ----
	if (Action == TEXT("list_utilities"))
	{
		FString Prefix = TEXT("/Game/");
		if (Params.IsValid()) Params->TryGetStringField(TEXT("prefix"), Prefix);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<TSharedPtr<FJsonValue>> Items;

		// EUW search
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(UEditorUtilityWidgetBlueprint::StaticClass()->GetClassPathName());
			Filter.PackagePaths.Add(FName(*Prefix));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			AR.GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("path"), A.GetObjectPathString());
				O->SetStringField(TEXT("name"), A.AssetName.ToString());
				O->SetStringField(TEXT("type"), TEXT("EditorUtilityWidget"));
				Items.Add(MakeShared<FJsonValueObject>(O));
			}
		}

		// EUB search
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Blutility"), TEXT("EditorUtilityBlueprint")));
			Filter.PackagePaths.Add(FName(*Prefix));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			AR.GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("path"), A.GetObjectPathString());
				O->SetStringField(TEXT("name"), A.AssetName.ToString());
				O->SetStringField(TEXT("type"), TEXT("EditorUtilityBlueprint"));
				Items.Add(MakeShared<FJsonValueObject>(O));
			}
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("utilities"), Items);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Found %d editor utilities"), Items.Num()), Data);
	}

	return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("system/capabilities"));
}
