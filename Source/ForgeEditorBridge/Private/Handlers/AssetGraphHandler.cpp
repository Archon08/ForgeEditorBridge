#include "Handlers/AssetGraphHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset Registry --------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

// ---- File system -----------------------------------------------------------
#include "UObject/Linker.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

static const FString AG_DOMAIN = TEXT("asset_graph");

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UAssetGraphHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UAssetGraphHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(AG_DOMAIN, Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("get_references"))      return Action_GetReferences(Params);
	if (Action == TEXT("get_dependencies"))     return Action_GetDependencies(Params);
	if (Action == TEXT("validate_references"))  return Action_ValidateReferences(Params);
	if (Action == TEXT("get_package_size"))     return Action_GetPackageSize(Params);

	return MakeError(AG_DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown asset_graph action '%s'. Valid: get_references, get_dependencies, validate_references, get_package_size"), *Action));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void CollectDependenciesRecursive(IAssetRegistry& Registry, const FName& PackageName,
	TSet<FName>& Visited, TArray<FName>& OutPaths)
{
	TArray<FName> Deps;
	Registry.GetDependencies(PackageName, Deps);

	for (const FName& Dep : Deps)
	{
		if (!Visited.Contains(Dep))
		{
			Visited.Add(Dep);
			OutPaths.Add(Dep);
			CollectDependenciesRecursive(Registry, Dep, Visited, OutPaths);
		}
	}
}

FString UAssetGraphHandler::HumanReadableSize(int64 Bytes)
{
	if (Bytes < 1024)
	{
		return FString::Printf(TEXT("%lld B"), Bytes);
	}
	if (Bytes < 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
	}
	if (Bytes < 1024 * 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0));
	}
	return FString::Printf(TEXT("%.2f GB"), Bytes / (1024.0 * 1024.0 * 1024.0));
}

// ---------------------------------------------------------------------------
// get_references
// ---------------------------------------------------------------------------

FBridgeResult UAssetGraphHandler::Action_GetReferences(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_references");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(AG_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FName PackageName = FName(*FPackageName::ObjectPathToPackageName(AssetPath));

	TArray<FName> Referencers;
	Registry.GetReferencers(PackageName, Referencers);

	TArray<TSharedPtr<FJsonValue>> RefArray;
	for (const FName& Ref : Referencers)
	{
		RefArray.Add(MakeShared<FJsonValueString>(Ref.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("referencers"), RefArray);
	Data->SetNumberField(TEXT("count"), Referencers.Num());

	return MakeSuccess(AG_DOMAIN, Action,
		FString::Printf(TEXT("'%s' has %d referencer(s)"), *AssetPath, Referencers.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_dependencies
// ---------------------------------------------------------------------------

FBridgeResult UAssetGraphHandler::Action_GetDependencies(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_dependencies");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(AG_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	bool bRecursive = false;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FName PackageName = FName(*FPackageName::ObjectPathToPackageName(AssetPath));

	TArray<FName> Dependencies;
	if (bRecursive)
	{
		TSet<FName> Visited;
		Visited.Add(PackageName);
		CollectDependenciesRecursive(Registry, PackageName, Visited, Dependencies);
	}
	else
	{
		Registry.GetDependencies(PackageName, Dependencies);
	}

	TArray<TSharedPtr<FJsonValue>> DepArray;
	for (const FName& Dep : Dependencies)
	{
		DepArray.Add(MakeShared<FJsonValueString>(Dep.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetArrayField(TEXT("dependencies"), DepArray);
	Data->SetNumberField(TEXT("count"), Dependencies.Num());

	return MakeSuccess(AG_DOMAIN, Action,
		FString::Printf(TEXT("'%s' has %d %sdependenc%s"), *AssetPath, Dependencies.Num(),
			bRecursive ? TEXT("recursive ") : TEXT(""),
			Dependencies.Num() == 1 ? TEXT("y") : TEXT("ies")),
		Data);
}

// ---------------------------------------------------------------------------
// validate_references
// ---------------------------------------------------------------------------

FBridgeResult UAssetGraphHandler::Action_ValidateReferences(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("validate_references");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(AG_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FName PackageName = FName(*FPackageName::ObjectPathToPackageName(AssetPath));

	TArray<FName> Dependencies;
	Registry.GetDependencies(PackageName, Dependencies);

	TArray<TSharedPtr<FJsonValue>> MissingArray;
	for (const FName& Dep : Dependencies)
	{
		// Check if the package exists in the registry
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Dep, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

		if (Assets.Num() == 0)
		{
			// Also check if it's a script or engine package that might not be in registry
			FString DepStr = Dep.ToString();
			if (!DepStr.StartsWith(TEXT("/Script/")) && !DepStr.StartsWith(TEXT("/Engine/")))
			{
				MissingArray.Add(MakeShared<FJsonValueString>(DepStr));
			}
		}
	}

	bool bValid = (MissingArray.Num() == 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("valid"), bValid);
	Data->SetArrayField(TEXT("missing_references"), MissingArray);
	Data->SetNumberField(TEXT("broken_count"), MissingArray.Num());
	Data->SetNumberField(TEXT("total_dependencies"), Dependencies.Num());

	return MakeSuccess(AG_DOMAIN, Action,
		FString::Printf(TEXT("'%s': %s (%d/%d dependencies valid)"),
			*AssetPath,
			bValid ? TEXT("all references valid") : TEXT("BROKEN references found"),
			Dependencies.Num() - MissingArray.Num(), Dependencies.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_package_size
// ---------------------------------------------------------------------------

FBridgeResult UAssetGraphHandler::Action_GetPackageSize(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_package_size");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(AG_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	// Convert content path to package name, then to disk path
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	FString DiskPath;

	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, DiskPath))
	{
		return MakeError(AG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Could not resolve package path for '%s'"), *AssetPath),
			TEXT("Ensure the asset_path is a valid content path like /Game/..."));
	}

	// Try common asset extensions
	static const TCHAR* Extensions[] = { TEXT(".uasset"), TEXT(".umap") };
	int64 FileSize = -1;
	FString FoundPath;

	for (const TCHAR* Ext : Extensions)
	{
		FString FullPath = DiskPath + Ext;
		FileSize = IFileManager::Get().FileSize(*FullPath);
		if (FileSize >= 0)
		{
			FoundPath = FullPath;
			break;
		}
	}

	if (FileSize < 0)
	{
		return MakeError(AG_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Package file not found on disk for '%s'"), *AssetPath),
			TEXT("The asset may not be saved to disk yet"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("disk_path"), FoundPath);
	Data->SetNumberField(TEXT("disk_size_bytes"), static_cast<double>(FileSize));
	Data->SetStringField(TEXT("disk_size_human"), HumanReadableSize(FileSize));

	return MakeSuccess(AG_DOMAIN, Action,
		FString::Printf(TEXT("'%s': %s (%lld bytes)"), *AssetPath, *HumanReadableSize(FileSize), FileSize),
		Data);
}

TSharedPtr<FJsonObject> UAssetGraphHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get all assets that reference this asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_references"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get all dependencies of an asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); Ps->SetObjectField(TEXT("recursive"), P(TEXT("bool"), false, TEXT("Recurse into dependencies (default false)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_dependencies"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Check if all dependencies of an asset exist")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("validate_references"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get the on-disk size of a package")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_package_size"), A); }

	return Root;
}
