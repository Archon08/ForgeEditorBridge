#include "Handlers/AssetHandler.h"
#include "ForgeAISettings.h"
#include "ForgeAISubsystem.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/Material.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/SavePackage.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "EditorValidatorSubsystem.h"
#include "AutomatedAssetImportData.h"
#include "AssetImportTask.h"
#include "HAL/FileManager.h"
#include "Editor.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("asset");

FBridgeResult UAssetHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), Action);

	if (!Params.IsValid())
	{
		Result.Message = TEXT("Params object is null");
		Result.ErrorCode = 1000;
		return Result;
	}

	if (Action == TEXT("create_data_asset")) return CreateDataAsset(Params);
	if (Action == TEXT("create_material"))   return CreateMaterial(Params);
	if (Action == TEXT("duplicate_asset"))   return DuplicateAsset(Params);
	if (Action == TEXT("set_metadata"))      return Action_SetMetadata(Params);
	if (Action == TEXT("get_metadata"))      return Action_GetMetadata(Params);
	if (Action == TEXT("rename_asset"))      return Action_RenameAsset(Params);
	if (Action == TEXT("move_asset"))        return Action_MoveAsset(Params);
	if (Action == TEXT("search_assets"))     return Action_SearchAssets(Params);
	if (Action == TEXT("import_asset"))      return Action_ImportAsset(Params);
	if (Action == TEXT("import_folder"))     return Action_ImportFolder(Params);
	if (Action == TEXT("save_asset"))        return Action_SaveAsset(Params);
	if (Action == TEXT("validate_asset"))    return Action_ValidateAsset(Params);
	if (Action == TEXT("get_asset_info"))    return Action_GetAssetInfo(Params);
	if (Action == TEXT("delete_asset"))      return Action_DeleteAsset(Params);
	if (Action == TEXT("fix_up_referencers")) return Action_FixUpReferencers(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown asset action '%s'"), *Action),
		TEXT("system/capabilities"));
}

// ---------------------------------------------------------------------------
// delete_asset
// ---------------------------------------------------------------------------
FBridgeResult UAssetHandler::Action_DeleteAsset(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(DOMAIN, TEXT("delete_asset"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("delete_asset requires 'asset_path'");
		Result.ErrorCode = 1000;
		return Result;
	}
	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	UObject* Obj = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Obj)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Obj = LoadObject<UObject>(nullptr, *Suffix);
	}
	if (!Obj)
	{
		Result.Message = FString::Printf(TEXT("delete_asset: asset not found '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	TArray<UObject*> ToDelete;
	ToDelete.Add(Obj);

	const int32 NumDeleted = bForce
		? ObjectTools::ForceDeleteObjects(ToDelete, /*ShowConfirmation=*/false)
		: ObjectTools::DeleteObjects(ToDelete, /*bShowConfirmation=*/false);

	if (NumDeleted <= 0)
	{
		Result.Message = FString::Printf(
			TEXT("delete_asset: failed to delete '%s' (referencers may exist — retry with force=true)"),
			*AssetPath);
		Result.ErrorCode = 3000;
		Result.RecoveryHint = TEXT("Run asset/fix_up_referencers first, or pass force=true");
		return Result;
	}

	Result.bSuccess = true;
	Result.AffectedPath = AssetPath;
	Result.Message = FString::Printf(TEXT("Deleted asset '%s' (force=%s)"),
		*AssetPath, bForce ? TEXT("true") : TEXT("false"));
	return Result;
}

// ---------------------------------------------------------------------------
// fix_up_referencers — rewrite stale references after move/rename
// ---------------------------------------------------------------------------
FBridgeResult UAssetHandler::Action_FixUpReferencers(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(DOMAIN, TEXT("fix_up_referencers"));

	const TArray<TSharedPtr<FJsonValue>>* PackagesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("redirector_paths"), PackagesArr) || !PackagesArr)
	{
		Result.Message = TEXT("fix_up_referencers requires 'redirector_paths' (string array of redirector package names)");
		Result.ErrorCode = 1000;
		return Result;
	}

	TArray<UObjectRedirector*> Redirectors;
	for (const TSharedPtr<FJsonValue>& V : *PackagesArr)
	{
		const FString Path = V->AsString();
		if (UObject* Obj = LoadObject<UObject>(nullptr, *Path))
		{
			if (UObjectRedirector* R = Cast<UObjectRedirector>(Obj))
			{
				Redirectors.Add(R);
			}
		}
	}
	if (Redirectors.Num() == 0)
	{
		Result.Message = TEXT("fix_up_referencers: no UObjectRedirector(s) resolved from input");
		Result.ErrorCode = 2000;
		return Result;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetToolsModule.Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt=*/false);

	Result.bSuccess = true;
	Result.Message = FString::Printf(TEXT("Fixed up %d redirector(s)"), Redirectors.Num());
	return Result;
}

FBridgeResult UAssetHandler::CreateDataAsset(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), TEXT("create_data_asset"));

	FString AssetName, PackagePath, ClassName;
	if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) ||
		!Params->TryGetStringField(TEXT("package_path"), PackagePath))
	{
		Result.Message = TEXT("create_data_asset requires: asset_name, package_path. Optional: class_name");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("class_name"), ClassName);

	// Resolve class — default to UDataAsset if not specified or not found
	UClass* AssetClass = nullptr;
	if (!ClassName.IsEmpty())
	{
		AssetClass = FindObject<UClass>(nullptr, *ClassName);
		if (!AssetClass)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == ClassName && It->IsChildOf(UObject::StaticClass()))
				{
					AssetClass = *It;
					break;
				}
			}
		}
	}
	if (!AssetClass)
	{
		AssetClass = UObject::StaticClass(); // Fallback — caller should provide a valid class
		if (!ClassName.IsEmpty())
		{
			Result.Message = FString::Printf(TEXT("Class not found '%s', falling back to UObject"), *ClassName);
		}
	}

	// Build the package name and create the asset via direct package construction
	// This approach works for any UObject-derived class without needing a specific factory
	FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		Result.Message = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		Result.ErrorCode = 3000;
		return Result;
	}
	Package->FullyLoad();

	UObject* NewObj = NewObject<UObject>(Package, AssetClass, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!NewObj)
	{
		Result.Message = FString::Printf(TEXT("Failed to create object of class '%s'"), *AssetClass->GetName());
		Result.ErrorCode = 3000;
		return Result;
	}

	FAssetRegistryModule::AssetCreated(NewObj);
	Package->MarkPackageDirty();

	// Save to disk
	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename,
		FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, NewObj, *PackageFilename, SaveArgs);
	}

	Result.bSuccess = true;
	Result.AffectedPath = PackageName;
	Result.Message = FString::Printf(TEXT("Data asset created: %s (class: %s)"),
		*PackageName, *AssetClass->GetName());
	return Result;
}

FBridgeResult UAssetHandler::CreateMaterial(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), TEXT("create_material"));

	FString MaterialName, PackagePath;
	if (!Params->TryGetStringField(TEXT("material_name"), MaterialName) ||
		!Params->TryGetStringField(TEXT("package_path"), PackagePath))
	{
		Result.Message = TEXT("create_material requires: material_name, package_path");
		Result.ErrorCode = 1000;
		return Result;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UObject* NewMaterial = AssetToolsModule.Get().CreateAsset(MaterialName, PackagePath,
		UMaterial::StaticClass(), Factory);

	if (NewMaterial)
	{
		Result.bSuccess = true;
		Result.AffectedPath = PackagePath / MaterialName;
		Result.Message = FString::Printf(TEXT("Material created: %s"), *Result.AffectedPath);
	}
	else
	{
		Result.Message = FString::Printf(TEXT("Failed to create material '%s' in '%s'"),
			*MaterialName, *PackagePath);
		Result.ErrorCode = 3000;
	}
	return Result;
}

FBridgeResult UAssetHandler::DuplicateAsset(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), TEXT("duplicate_asset"));

	FString SourcePath, DestPath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) ||
		!Params->TryGetStringField(TEXT("dest_path"), DestPath))
	{
		Result.Message = TEXT("duplicate_asset requires: source_path, dest_path");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Normalise source to object path (e.g. /Game/Folder/Asset -> /Game/Folder/Asset.Asset)
	FString SourceObjectPath = SourcePath;
	if (!SourceObjectPath.Contains(TEXT(".")))
	{
		SourceObjectPath = SourcePath + TEXT(".") + FPackageName::GetShortName(SourcePath);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData SourceData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
	if (!SourceData.IsValid())
	{
		Result.Message = FString::Printf(TEXT("Source asset not found: %s"), *SourceObjectPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	FString DestName   = FPackageName::GetShortName(DestPath);
	FString DestFolder = FPackageName::GetLongPackagePath(DestPath);

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* Duplicated = AssetToolsModule.Get().DuplicateAsset(DestName, DestFolder, SourceData.GetAsset());

	if (Duplicated)
	{
		Result.bSuccess = true;
		Result.AffectedPath = DestFolder / DestName;
		Result.Message = FString::Printf(TEXT("Asset duplicated: %s -> %s"), *SourcePath, *Result.AffectedPath);
	}
	else
	{
		Result.Message = FString::Printf(TEXT("Failed to duplicate '%s' to '%s/%s'"),
			*SourcePath, *DestFolder, *DestName);
		Result.ErrorCode = 3000;
	}
	return Result;
}

// ---------------------------------------------------------------------------
// set_metadata
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_SetMetadata(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), TEXT("set_metadata"));

	FString AssetPath, Key, Value;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("key"),        Key)       || Key.IsEmpty()       ||
	    !Params->TryGetStringField(TEXT("value"),      Value))
	{
		Result.Message = TEXT("set_metadata: 'asset_path', 'key', and 'value' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	TSharedPtr<FJsonObject> Store = LoadMetadataStore();

	// Get or create the per-asset object
	TSharedPtr<FJsonObject> AssetObj;
	if (Store->HasTypedField<EJson::Object>(AssetPath))
	{
		AssetObj = Store->GetObjectField(AssetPath);
	}
	else
	{
		AssetObj = MakeShared<FJsonObject>();
	}
	AssetObj->SetStringField(Key, Value);
	Store->SetObjectField(AssetPath, AssetObj);

	FString SaveError;
	if (!SaveMetadataStore(Store, SaveError))
	{
		Result.Message = FString::Printf(TEXT("set_metadata: failed to write store: %s"), *SaveError);
		Result.ErrorCode = 3000;
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Metadata set: '%s'.'%s' = '%s'"), *AssetPath, *Key, *Value);
	return Result;
}

// ---------------------------------------------------------------------------
// get_metadata
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_GetMetadata(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("asset"), TEXT("get_metadata"));

	FString AssetPath, Key;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_metadata: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("key"), Key);

	TSharedPtr<FJsonObject> Store = LoadMetadataStore();

	if (!Store->HasTypedField<EJson::Object>(AssetPath))
	{
		Result.bSuccess  = true;
		Result.ExtraData = TEXT("{}");
		Result.Message   = FString::Printf(TEXT("get_metadata: no metadata stored for '%s'"), *AssetPath);
		return Result;
	}

	TSharedPtr<FJsonObject> AssetObj = Store->GetObjectField(AssetPath);

	TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
	if (!Key.IsEmpty())
	{
		// Single key requested
		FString Val;
		if (AssetObj->TryGetStringField(Key, Val))
		{
			ResponseObj->SetStringField(Key, Val);
		}
		// Return empty object if key not found (not an error)
	}
	else
	{
		// All keys
		ResponseObj = AssetObj;
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("Metadata retrieved for '%s'%s"),
		*AssetPath, Key.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (key='%s')"), *Key));
	return Result;
}

// ---------------------------------------------------------------------------
// Metadata store helpers
// ---------------------------------------------------------------------------

FString UAssetHandler::MetadataStorePath() const
{
	return FPaths::ConvertRelativePathToFull(
		GetMutableDefault<UForgeAISettings>()->GetAbsoluteContextDirectory() / TEXT("asset-metadata.json"));
}

TSharedPtr<FJsonObject> UAssetHandler::LoadMetadataStore() const
{
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *MetadataStorePath()))
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			return Parsed;
		}
	}
	return MakeShared<FJsonObject>();
}

bool UAssetHandler::SaveMetadataStore(TSharedPtr<FJsonObject> Store, FString& OutError) const
{
	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Store.ToSharedRef(), Writer);

	const FString FilePath = MetadataStorePath();
	// Ensure directory exists
	const FString Dir = FPaths::GetPath(FilePath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir) && !PF.CreateDirectoryTree(*Dir))
	{
		OutError = FString::Printf(TEXT("cannot create directory '%s'"), *Dir);
		return false;
	}

	if (!FFileHelper::SaveStringToFile(OutStr, *FilePath))
	{
		OutError = FString::Printf(TEXT("FFileHelper::SaveStringToFile failed for '%s'"), *FilePath);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// rename_asset
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_RenameAsset(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("rename_asset");

	FString OldPath, NewName;
	if (!Params->TryGetStringField(TEXT("old_path"), OldPath) || OldPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'old_path'"));
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'new_name'"));

	// Normalise to object path
	FString OldObjectPath = OldPath;
	if (!OldObjectPath.Contains(TEXT(".")))
	{
		OldObjectPath = OldPath + TEXT(".") + FPackageName::GetShortName(OldPath);
	}

	// Verify the asset exists
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = ARModule.Get().GetAssetByObjectPath(FSoftObjectPath(OldObjectPath));
	if (!AssetData.IsValid())
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Asset not found: %s"), *OldPath));

	// Build the rename data
	FString OldFolder = FPackageName::GetLongPackagePath(OldPath);
	FString NewPath = OldFolder / NewName;

	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(FSoftObjectPath(OldObjectPath), FSoftObjectPath(NewPath + TEXT(".") + NewName));

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	bool bSuccess = AssetToolsModule.Get().RenameAssets(RenameData);

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("old_path"), OldPath);
		Data->SetStringField(TEXT("new_path"), NewPath);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Asset renamed: %s -> %s"), *OldPath, *NewPath), Data);
	}
	return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to rename '%s' to '%s'"), *OldPath, *NewName),
		TEXT("Check that no asset exists at the destination and the source is not read-only"));
}

// ---------------------------------------------------------------------------
// move_asset
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_MoveAsset(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("move_asset");

	FString SourcePath, DestPath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'source_path'"));
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'dest_path'"));

	// Normalise to object paths
	FString SourceObjectPath = SourcePath;
	if (!SourceObjectPath.Contains(TEXT(".")))
	{
		SourceObjectPath = SourcePath + TEXT(".") + FPackageName::GetShortName(SourcePath);
	}

	FString DestName = FPackageName::GetShortName(DestPath);
	FString DestObjectPath = DestPath;
	if (!DestObjectPath.Contains(TEXT(".")))
	{
		DestObjectPath = DestPath + TEXT(".") + DestName;
	}

	// Verify the source asset exists
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = ARModule.Get().GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
	if (!AssetData.IsValid())
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Source asset not found: %s"), *SourcePath));

	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(FSoftObjectPath(SourceObjectPath), FSoftObjectPath(DestObjectPath));

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	bool bSuccess = AssetToolsModule.Get().RenameAssets(RenameData);

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("source_path"), SourcePath);
		Data->SetStringField(TEXT("dest_path"), DestPath);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Asset moved: %s -> %s"), *SourcePath, *DestPath), Data);
	}
	return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to move '%s' to '%s'"), *SourcePath, *DestPath),
		TEXT("Check that no asset exists at the destination and the source is not read-only"));
}

// ---------------------------------------------------------------------------
// search_assets
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_SearchAssets(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("search_assets");

	FString ClassName, SearchPath, NameFilter;
	bool bRecursive = true;
	Params->TryGetStringField(TEXT("class"), ClassName);
	Params->TryGetStringField(TEXT("path"), SearchPath);
	Params->TryGetStringField(TEXT("name_filter"), NameFilter);
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = ARModule.Get();

	FARFilter Filter;

	// UE 5.7 BREAKING: Use FTopLevelAssetPath instead of FName for class filtering
	if (!ClassName.IsEmpty())
	{
		// Try to find the class to get its proper path
		UClass* FilterClass = FindObject<UClass>(nullptr, *ClassName);
		if (!FilterClass)
		{
			// Try short name search
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == ClassName)
				{
					FilterClass = *It;
					break;
				}
			}
		}
		if (FilterClass)
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(FilterClass->GetPathName()));
		}
		else
		{
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Class not found: %s"), *ClassName),
				TEXT("Provide a valid UClass name (e.g. 'StaticMesh', 'Material', 'Blueprint')"));
		}
	}

	if (!SearchPath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*SearchPath));
		Filter.bRecursivePaths = bRecursive;
	}

	TArray<FAssetData> Results;
	AssetRegistry.GetAssets(Filter, Results);

	// Apply name filter if provided
	if (!NameFilter.IsEmpty())
	{
		Results.RemoveAll([&NameFilter](const FAssetData& Asset)
		{
			return !Asset.AssetName.ToString().Contains(NameFilter);
		});
	}

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Results)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
		// UE 5.7: Use AssetClassPath instead of deprecated AssetClass
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetArrayField(TEXT("assets"), Items);

	return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Found %d assets"), Results.Num()), Data);
}

// ---------------------------------------------------------------------------
// import_asset
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_ImportAsset(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("import_asset");

	FString SourceFile, DestPath;
	bool bReplace = false;
	if (!Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'source_file'"));
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'dest_path'"));
	Params->TryGetBoolField(TEXT("replace"), bReplace);

	// Verify source file exists on disk
	if (!FPaths::FileExists(SourceFile))
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Source file not found: %s"), *SourceFile),
			TEXT("Provide a valid absolute filesystem path"));

	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourceFile);
	ImportData->DestinationPath = DestPath;
	ImportData->bReplaceExisting = bReplace;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (ImportedObjects.Num() > 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Paths;
		for (UObject* Obj : ImportedObjects)
		{
			Paths.Add(MakeShared<FJsonValueString>(Obj->GetPathName()));
		}
		Data->SetArrayField(TEXT("imported"), Paths);
		Data->SetNumberField(TEXT("count"), ImportedObjects.Num());
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Imported %d asset(s) from '%s'"), ImportedObjects.Num(), *SourceFile), Data);
	}
	return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Import failed for '%s'"), *SourceFile),
		TEXT("Check that the file format is supported and the destination path is valid"));
}

// ---------------------------------------------------------------------------
// import_folder — batch-import a folder of files into a target content path
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_ImportFolder(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("import_folder");

#if WITH_EDITOR
	FString SourceFolder, TargetPackagePath;
	if (!Params->TryGetStringField(TEXT("source_folder"), SourceFolder) || SourceFolder.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'source_folder'"));
	if (!Params->TryGetStringField(TEXT("target_package_path"), TargetPackagePath) || TargetPackagePath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'target_package_path'"));

	// Validate target package path — must be a mounted content path.
	if (!FPackageName::IsValidLongPackageName(TargetPackagePath / TEXT("_probe"), /*bIncludeReadOnlyRoots=*/true))
	{
		return MakeError(DOMAIN, Action, 1000,
			FString::Printf(TEXT("Invalid target_package_path '%s': must be a mounted content path (e.g. '/Game/Environment/Textures')"),
				*TargetPackagePath),
			TEXT("Use a path starting with /Game/ or a registered mount point"));
	}

	// Normalize and validate source folder on disk.
	IFileManager& FM = IFileManager::Get();
	const FString NormalizedSource = FPaths::ConvertRelativePathToFull(SourceFolder);
	if (!FM.DirectoryExists(*NormalizedSource))
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("Source folder not found: %s"), *NormalizedSource),
			TEXT("Provide an absolute filesystem path to an existing directory"));
	}

	// Extensions
	TArray<FString> Extensions;
	const TArray<TSharedPtr<FJsonValue>>* ExtArray = nullptr;
	if (Params->TryGetArrayField(TEXT("file_extensions"), ExtArray) && ExtArray)
	{
		for (const TSharedPtr<FJsonValue>& V : *ExtArray)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S))
			{
				S.TrimStartAndEndInline();
				if (!S.IsEmpty())
				{
					// Strip a leading dot if the caller passed ".png"; keep "*" sentinel as-is.
					if (S.StartsWith(TEXT(".")))
					{
						S.RemoveAt(0, 1);
					}
					Extensions.Add(S);
				}
			}
		}
	}
	if (Extensions.Num() == 0)
	{
		Extensions.Add(TEXT("*"));
	}

	bool bRecursive = false;
	bool bReplaceExisting = false;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	// Enumerate files.
	TArray<FString> FoundFiles;
	const bool bAllExtensions = Extensions.Contains(TEXT("*"));

	if (bAllExtensions)
	{
		if (bRecursive)
		{
			FM.FindFilesRecursive(FoundFiles, *NormalizedSource, TEXT("*"), /*Files=*/true, /*Directories=*/false);
		}
		else
		{
			FM.FindFiles(FoundFiles, *(NormalizedSource / TEXT("*")), /*Files=*/true, /*Directories=*/false);
			// FindFiles returns basenames only; prefix the directory for consistency.
			for (FString& F : FoundFiles)
			{
				F = NormalizedSource / F;
			}
		}
	}
	else
	{
		for (const FString& Ext : Extensions)
		{
			const FString Pattern = FString::Printf(TEXT("*.%s"), *Ext);
			TArray<FString> Batch;
			if (bRecursive)
			{
				FM.FindFilesRecursive(Batch, *NormalizedSource, *Pattern, /*Files=*/true, /*Directories=*/false);
				FoundFiles.Append(Batch);
			}
			else
			{
				FM.FindFiles(Batch, *(NormalizedSource / Pattern), /*Files=*/true, /*Directories=*/false);
				for (const FString& F : Batch)
				{
					FoundFiles.Add(NormalizedSource / F);
				}
			}
		}
	}

	// De-duplicate.
	TSet<FString> SeenSet;
	FoundFiles.RemoveAll([&SeenSet](const FString& F)
	{
		bool bAlready = false;
		SeenSet.Add(F, &bAlready);
		return bAlready;
	});

	// Build import tasks.
	TArray<UAssetImportTask*> Tasks;
	Tasks.Reserve(FoundFiles.Num());
	for (const FString& SourceFile : FoundFiles)
	{
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename         = SourceFile;
		Task->DestinationPath  = TargetPackagePath;
		Task->bAutomated       = true;
		Task->bSave            = true;
		Task->bReplaceExisting = bReplaceExisting;
		Tasks.Add(Task);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ImportedArr;
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	int32 ImportedCount = 0;
	int32 SkippedCount  = 0;
	int32 FailedCount   = 0;

	if (Tasks.Num() == 0)
	{
		Data->SetNumberField(TEXT("imported_count"), 0);
		Data->SetNumberField(TEXT("skipped_count"), 0);
		Data->SetNumberField(TEXT("failed_count"), 0);
		Data->SetArrayField(TEXT("imported_assets"), ImportedArr);
		Data->SetArrayField(TEXT("errors"), ErrorsArr);
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("No files matched in '%s' with extensions=%s recursive=%s"),
				*NormalizedSource, *FString::Join(Extensions, TEXT(",")),
				bRecursive ? TEXT("true") : TEXT("false")),
			Data);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks(Tasks);

	for (UAssetImportTask* Task : Tasks)
	{
		if (!Task) continue;

		if (Task->ImportedObjectPaths.Num() > 0)
		{
			++ImportedCount;
			for (const FString& P : Task->ImportedObjectPaths)
			{
				ImportedArr.Add(MakeShared<FJsonValueString>(P));
			}
			continue;
		}

		// Zero imported paths. UAssetImportTask does not cleanly separate
		// "silently skipped because target exists and bReplaceExisting=false"
		// from "factory error". Use the task's Filename and flags to infer:
		// if bReplaceExisting is false and a package with the same short name
		// already exists under DestinationPath, treat as skipped; otherwise failed.
		const FString BaseName = FPaths::GetBaseFilename(Task->Filename);
		const FString CandidatePackage = Task->DestinationPath / BaseName;
		const bool bTargetExists = FPackageName::DoesPackageExist(CandidatePackage);

		if (!Task->bReplaceExisting && bTargetExists)
		{
			++SkippedCount;
		}
		else
		{
			++FailedCount;
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("source_file"), Task->Filename);
			ErrObj->SetStringField(TEXT("reason"),
				TEXT("No asset imported. Check Output Log for factory errors (unsupported format, invalid target path, etc.)"));
			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	Data->SetNumberField(TEXT("imported_count"), ImportedCount);
	Data->SetNumberField(TEXT("skipped_count"),  SkippedCount);
	Data->SetNumberField(TEXT("failed_count"),   FailedCount);
	Data->SetArrayField(TEXT("imported_assets"), ImportedArr);
	Data->SetArrayField(TEXT("errors"),          ErrorsArr);

	const FString Summary = FString::Printf(
		TEXT("import_folder '%s' -> '%s': imported=%d skipped=%d failed=%d (matched %d files, ext=%s, recursive=%s)"),
		*NormalizedSource, *TargetPackagePath,
		ImportedCount, SkippedCount, FailedCount,
		FoundFiles.Num(), *FString::Join(Extensions, TEXT(",")),
		bRecursive ? TEXT("true") : TEXT("false"));

	if (ImportedCount == 0 && FailedCount > 0)
	{
		return MakeError(DOMAIN, Action, 3000, Summary,
			TEXT("All import tasks failed — verify file formats are supported and the target content path exists"));
	}

	return MakeSuccess(DOMAIN, Action, Summary, Data);
#else
	return MakeError(DOMAIN, Action, 3003, TEXT("import_folder is editor-only (requires AssetTools)"));
#endif
}

// ---------------------------------------------------------------------------
// save_asset
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_SaveAsset(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("save_asset");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	// Normalise to object path
	FString ObjectPath = AssetPath;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	}

	// Find the asset
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = ARModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid())
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

	UObject* Asset = AssetData.GetAsset();
	if (!Asset)
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));

	UPackage* Package = Asset->GetOutermost();
	FString PackageFilename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), PackageFilename,
		FPackageName::GetAssetPackageExtension()))
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Cannot resolve filename for package: %s"), *Package->GetName()));
	}

	// UE 5.7 BREAKING: Use FSavePackageArgs (old overloads removed)
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);

	if (bSaved)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), AssetPath);
		Data->SetStringField(TEXT("filename"), PackageFilename);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Asset saved: %s"), *AssetPath), Data);
	}
	return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath));
}

// ---------------------------------------------------------------------------
// validate_asset
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_ValidateAsset(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("validate_asset");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	// Normalise to object path
	FString ObjectPath = AssetPath;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	}

	// Find the asset
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = ARModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid())
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

	// Get the editor validator subsystem
	UEditorValidatorSubsystem* ValidatorSubsystem = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	if (!ValidatorSubsystem)
		return MakeError(DOMAIN, Action, 3000, TEXT("UEditorValidatorSubsystem not available"));

	// UE 5.7: Use ValidateAssetsWithSettings (ValidateAssets deprecated)
	FValidateAssetsSettings Settings;
	FValidateAssetsResults Results;

	TArray<FAssetData> AssetsToValidate;
	AssetsToValidate.Add(AssetData);
	Settings.bLoadAssetsForValidation = true;

	ValidatorSubsystem->ValidateAssetsWithSettings(AssetsToValidate, Settings, Results);

	int32 NumWarnings = Results.NumWarnings;
	int32 NumErrors = Results.NumInvalid;
	int32 NumValid = Results.NumValid;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), AssetPath);
	Data->SetNumberField(TEXT("valid"), NumValid);
	Data->SetNumberField(TEXT("warnings"), NumWarnings);
	Data->SetNumberField(TEXT("errors"), NumErrors);

	FString Status = (NumErrors > 0) ? TEXT("invalid") : (NumWarnings > 0) ? TEXT("warnings") : TEXT("valid");
	Data->SetStringField(TEXT("status"), Status);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Validation: %s — valid=%d warnings=%d errors=%d"), *Status, NumValid, NumWarnings, NumErrors), Data);
}

// ---------------------------------------------------------------------------
// get_asset_info
// ---------------------------------------------------------------------------

FBridgeResult UAssetHandler::Action_GetAssetInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_asset_info");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	// Normalise to object path
	FString ObjectPath = AssetPath;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	}

	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = ARModule.Get();

	// UE 5.7: GetAssetByObjectPath takes FSoftObjectPath
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid())
		return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	// UE 5.7: Use AssetClassPath instead of deprecated AssetClass
	Data->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
	Data->SetStringField(TEXT("path"), AssetData.GetSoftObjectPath().ToString());
	Data->SetStringField(TEXT("package"), AssetData.PackageName.ToString());

	// Disk size
	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(
		AssetData.PackageName.ToString(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		const int64 FileSize = IFileManager::Get().FileSize(*PackageFilename);
		Data->SetNumberField(TEXT("disk_size_bytes"), static_cast<double>(FileSize));
	}

	// Is dirty?
	UPackage* Package = FindPackage(nullptr, *AssetData.PackageName.ToString());
	Data->SetBoolField(TEXT("is_dirty"), Package ? Package->IsDirty() : false);

	// Dependencies
	TArray<FAssetIdentifier> Dependencies;
	AssetRegistry.GetDependencies(FAssetIdentifier(AssetData.PackageName), Dependencies);
	TArray<TSharedPtr<FJsonValue>> DepArray;
	for (const FAssetIdentifier& Dep : Dependencies)
	{
		DepArray.Add(MakeShared<FJsonValueString>(Dep.PackageName.ToString()));
	}
	Data->SetArrayField(TEXT("dependencies"), DepArray);

	// Referencers
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(AssetData.PackageName), Referencers);
	TArray<TSharedPtr<FJsonValue>> RefArray;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		RefArray.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
	}
	Data->SetArrayField(TEXT("referencers"), RefArray);

	return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Asset info: %s"), *AssetPath), Data);
}

TSharedPtr<FJsonObject> UAssetHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a data asset of a given class")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_name"), P(TEXT("string"), true, TEXT("Asset name"))); Ps->SetObjectField(TEXT("package_path"), P(TEXT("string"), true, TEXT("Content folder path"))); Ps->SetObjectField(TEXT("class_name"), P(TEXT("string"), false, TEXT("UClass name (default UObject)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_data_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new Material asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("material_name"), P(TEXT("string"), true, TEXT("Material name"))); Ps->SetObjectField(TEXT("package_path"), P(TEXT("string"), true, TEXT("Content folder path"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_material"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Duplicate an existing asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("source_path"), P(TEXT("string"), true, TEXT("Content path of source asset"))); Ps->SetObjectField(TEXT("dest_path"), P(TEXT("string"), true, TEXT("Content path for the duplicate"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("duplicate_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a metadata key-value pair on an asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("Metadata key"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Metadata value"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_metadata"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get metadata for an asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), false, TEXT("Specific key to get (omit for all)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_metadata"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Rename an asset in place")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("old_path"), P(TEXT("string"), true, TEXT("Current content path"))); Ps->SetObjectField(TEXT("new_name"), P(TEXT("string"), true, TEXT("New asset name"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("rename_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Move an asset to a new location")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("source_path"), P(TEXT("string"), true, TEXT("Current content path"))); Ps->SetObjectField(TEXT("dest_path"), P(TEXT("string"), true, TEXT("Destination content path"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("move_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Search assets by class, path, and name filter")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("class"), P(TEXT("string"), false, TEXT("UClass name to filter by"))); Ps->SetObjectField(TEXT("path"), P(TEXT("string"), false, TEXT("Content path to search in"))); Ps->SetObjectField(TEXT("name_filter"), P(TEXT("string"), false, TEXT("Substring to match in asset name"))); Ps->SetObjectField(TEXT("recursive"), P(TEXT("bool"), false, TEXT("Search subfolders (default true)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("search_assets"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Import an asset from a file on disk")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("source_file"), P(TEXT("string"), true, TEXT("Absolute disk path"))); Ps->SetObjectField(TEXT("dest_path"), P(TEXT("string"), true, TEXT("Content destination folder"))); Ps->SetObjectField(TEXT("replace"), P(TEXT("bool"), false, TEXT("Replace existing (default false)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("import_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Batch-import every matching file in a folder into a target content path via UAssetImportTask")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("source_folder"),        P(TEXT("string"),        true,  TEXT("Absolute filesystem path to the source folder")));
	  Ps->SetObjectField(TEXT("target_package_path"),  P(TEXT("string"),        true,  TEXT("UE content path, e.g. '/Game/Environment/Textures'")));
	  Ps->SetObjectField(TEXT("file_extensions"),      P(TEXT("array<string>"), false, TEXT("Extensions like ['png','tga','jpg']; '*' or omit means all supported")));
	  Ps->SetObjectField(TEXT("recursive"),            P(TEXT("bool"),          false, TEXT("Recurse into subfolders (default false)")));
	  Ps->SetObjectField(TEXT("replace_existing"),     P(TEXT("bool"),          false, TEXT("Overwrite existing assets at the target (default false)")));
	  A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("import_folder"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Save an asset to disk")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("save_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Validate an asset using the editor validator subsystem")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("validate_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get detailed asset info (class, size, dependencies, referencers)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_asset_info"), A); }

	return Root;
}
