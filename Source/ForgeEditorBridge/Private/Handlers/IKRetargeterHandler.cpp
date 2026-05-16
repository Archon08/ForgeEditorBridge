#include "Handlers/IKRetargeterHandler.h"
#include "ForgeAISubsystem.h"

// ---- IK Retargeter asset class ---------------------------------------------
#include "Retargeter/IKRetargeter.h"

// ---- IK Retargeter controller (IKRigEditor module) -------------------------
#include "RetargetEditor/IKRetargeterController.h"

// ---- IK Retarget batch operation -------------------------------------------
#include "RetargetEditor/IKRetargetBatchOperation.h"

// ---- IK Rig definition -----------------------------------------------------
#include "Rig/IKRigDefinition.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// ---- Animation assets ------------------------------------------------------
#include "Animation/AnimSequence.h"

// ---- Editor save -----------------------------------------------------------
#include "FileHelpers.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Load a UIKRigDefinition with package-suffix fallback. */
static UIKRigDefinition* LoadIKRigDefinition(const FString& Path)
{
	UIKRigDefinition* Rig = LoadObject<UIKRigDefinition>(nullptr, *Path);
	if (!Rig)
	{
		const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		Rig = LoadObject<UIKRigDefinition>(nullptr, *Suffix);
	}
	return Rig;
}

/** Load a UIKRetargeter with package-suffix fallback. */
static UIKRetargeter* LoadRetargeter(const FString& Path)
{
	UIKRetargeter* Asset = LoadObject<UIKRetargeter>(nullptr, *Path);
	if (!Asset)
	{
		const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		Asset = LoadObject<UIKRetargeter>(nullptr, *Suffix);
	}
	return Asset;
}

/** Save a dirty package containing the given object. Best-effort. */
static void SaveAsset(UObject* Asset)
{
#if WITH_EDITOR
	if (!Asset) return;
	TArray<UPackage*> Packages;
	Packages.Add(Asset->GetOutermost());
	FEditorFileUtils::PromptForCheckoutAndSave(Packages, /*bCheckDirty=*/true, /*bPromptToSave=*/false);
#endif
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UIKRetargeterHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand - dispatch
// ---------------------------------------------------------------------------

FBridgeResult UIKRetargeterHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("ik_retargeter"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_ik_retargeter")) return Action_CreateIKRetargeter(Params);
	if (Action == TEXT("add_chain_mapping"))    return Action_AddChainMapping(Params);
	if (Action == TEXT("retarget"))             return Action_Retarget(Params);
	if (Action == TEXT("batch_retarget"))       return Action_BatchRetarget(Params);

	FBridgeResult R = CreateResult(TEXT("ik_retargeter"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown ik_retargeter action '%s'. Valid: create_ik_retargeter, add_chain_mapping, retarget, batch_retarget"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_ik_retargeter
// ---------------------------------------------------------------------------

FBridgeResult UIKRetargeterHandler::Action_CreateIKRetargeter(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("ik_retargeter"), TEXT("create_ik_retargeter"));

	FString AssetPath, SourceRigPath, TargetRigPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),         AssetPath)    || AssetPath.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("source_ik_rig_path"), SourceRigPath) || SourceRigPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("target_ik_rig_path"), TargetRigPath) || TargetRigPath.IsEmpty())
	{
		Result.Message = TEXT("create_ik_retargeter: 'asset_path', 'source_ik_rig_path', and 'target_ik_rig_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UIKRigDefinition* SourceRig = LoadIKRigDefinition(SourceRigPath);
	if (!SourceRig)
	{
		Result.Message = FString::Printf(
			TEXT("create_ik_retargeter: no UIKRigDefinition found at '%s'"), *SourceRigPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UIKRigDefinition* TargetRig = LoadIKRigDefinition(TargetRigPath);
	if (!TargetRig)
	{
		Result.Message = FString::Printf(
			TEXT("create_ik_retargeter: no UIKRigDefinition found at '%s'"), *TargetRigPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* CreatedAsset = AssetTools.CreateAsset(AssetName, PackagePath, UIKRetargeter::StaticClass(), nullptr);

	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_ik_retargeter: IAssetTools::CreateAsset failed at '%s'"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(CreatedAsset);
	if (!Retargeter)
	{
		Result.Message = TEXT("create_ik_retargeter: created asset is not a UIKRetargeter");
		Result.ErrorCode = 2001;
		return Result;
	}

	// Use the controller to assign both rigs
	UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Retargeter);
	if (!Ctrl)
	{
		Result.Message = FString::Printf(
			TEXT("create_ik_retargeter: UIKRetargeterController::GetController returned null for asset at '%s'"),
			*AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	Ctrl->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
	Ctrl->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);

	Retargeter->MarkPackageDirty();
	SaveAsset(Retargeter);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("UIKRetargeter created at '%s' (source='%s', target='%s')"),
		*AssetPath, *SourceRigPath, *TargetRigPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_chain_mapping
// ---------------------------------------------------------------------------

FBridgeResult UIKRetargeterHandler::Action_AddChainMapping(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("ik_retargeter"), TEXT("add_chain_mapping"));

	FString AssetPath, SourceChain, TargetChain;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("source_chain"), SourceChain) || SourceChain.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("target_chain"), TargetChain) || TargetChain.IsEmpty())
	{
		Result.Message = TEXT("add_chain_mapping: 'asset_path', 'source_chain', and 'target_chain' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UIKRetargeter* Retargeter = LoadRetargeter(AssetPath);
	if (!Retargeter)
	{
		Result.Message = FString::Printf(
			TEXT("add_chain_mapping: no UIKRetargeter found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Retargeter);
	if (!Ctrl)
	{
		Result.Message = FString::Printf(
			TEXT("add_chain_mapping: UIKRetargeterController::GetController returned null for '%s'"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	// SetSourceChain maps a source retarget chain onto a target retarget chain.
	// Returns true if the target chain was found and the mapping was applied.
	const bool bMapped = Ctrl->SetSourceChain(FName(*SourceChain), FName(*TargetChain));
	if (!bMapped)
	{
		Result.Message = FString::Printf(
			TEXT("add_chain_mapping: SetSourceChain failed - target chain '%s' not found on retargeter at '%s'. Verify chain names match the target IK rig."),
			*TargetChain, *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	Retargeter->MarkPackageDirty();
	SaveAsset(Retargeter);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("add_chain_mapping: mapped '%s' -> '%s' on '%s'"),
		*SourceChain, *TargetChain, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// retarget
// ---------------------------------------------------------------------------

FBridgeResult UIKRetargeterHandler::Action_Retarget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("ik_retargeter"), TEXT("retarget"));

	FString SourceRigPath, TargetRigPath;
	if (!Params->TryGetStringField(TEXT("source_ik_rig_path"), SourceRigPath) || SourceRigPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("target_ik_rig_path"), TargetRigPath) || TargetRigPath.IsEmpty())
	{
		Result.Message = TEXT("retarget: 'source_ik_rig_path' and 'target_ik_rig_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	// If retargeter_path is given load it; otherwise create one at output_path
	FString RetargeterPath;
	UIKRetargeter* Retargeter = nullptr;

	if (Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath) && !RetargeterPath.IsEmpty())
	{
		Retargeter = LoadRetargeter(RetargeterPath);
		if (!Retargeter)
		{
			Result.Message = FString::Printf(
				TEXT("retarget: no UIKRetargeter found at '%s'"), *RetargeterPath);
			Result.ErrorCode = 2000;
			return Result;
		}
	}
	else
	{
		FString OutputPath;
		if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
		{
			Result.Message = TEXT("retarget: either 'retargeter_path' (existing) or 'output_path' (new asset destination) is required");
			Result.ErrorCode = 1000;
			return Result;
		}
		RetargeterPath = OutputPath;

		const FString AssetName   = FPackageName::GetLongPackageAssetName(OutputPath);
		const FString PackagePath = FPackageName::GetLongPackagePath(OutputPath);

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* CreatedAsset = AssetTools.CreateAsset(AssetName, PackagePath, UIKRetargeter::StaticClass(), nullptr);
		if (!CreatedAsset)
		{
			Result.Message = FString::Printf(
				TEXT("retarget: failed to create UIKRetargeter at '%s'"), *OutputPath);
			Result.ErrorCode = 3000;
			return Result;
		}
		Retargeter = Cast<UIKRetargeter>(CreatedAsset);
		if (!Retargeter)
		{
			Result.Message = TEXT("retarget: created asset is not a UIKRetargeter");
			Result.ErrorCode = 2001;
			return Result;
		}
	}

	// Load rigs
	UIKRigDefinition* SourceRig = LoadIKRigDefinition(SourceRigPath);
	if (!SourceRig)
	{
		Result.Message = FString::Printf(
			TEXT("retarget: no UIKRigDefinition found at '%s'"), *SourceRigPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UIKRigDefinition* TargetRig = LoadIKRigDefinition(TargetRigPath);
	if (!TargetRig)
	{
		Result.Message = FString::Printf(
			TEXT("retarget: no UIKRigDefinition found at '%s'"), *TargetRigPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Retargeter);
	if (!Ctrl)
	{
		Result.Message = FString::Printf(
			TEXT("retarget: UIKRetargeterController::GetController returned null for '%s'"), *RetargeterPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Assign rigs
	Ctrl->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
	Ctrl->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);

	// Auto-map chains if requested (default: true)
	bool bAutoMap = true;
	Params->TryGetBoolField(TEXT("auto_map"), bAutoMap);

	if (bAutoMap)
	{
		// bForceRemap=false so existing manual mappings are preserved
		Ctrl->AutoMapChains(EAutoMapChainType::Fuzzy, /*bForceRemap=*/false);
	}

	Retargeter->MarkPackageDirty();
	SaveAsset(Retargeter);

	Result.bSuccess     = true;
	Result.AffectedPath = RetargeterPath;
	Result.Message      = FString::Printf(
		TEXT("retarget: retargeter configured at '%s' (source='%s', target='%s', auto_map=%s)"),
		*RetargeterPath, *SourceRigPath, *TargetRigPath, bAutoMap ? TEXT("true") : TEXT("false"));
	return Result;
}

// ---------------------------------------------------------------------------
// batch_retarget
// ---------------------------------------------------------------------------

FBridgeResult UIKRetargeterHandler::Action_BatchRetarget(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("ik_retargeter"), TEXT("batch_retarget"));

	FString RetargeterPath, OutputDirectory;
	if (!Params->TryGetStringField(TEXT("retargeter_path"),  RetargeterPath)  || RetargeterPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("output_directory"), OutputDirectory)  || OutputDirectory.IsEmpty())
	{
		Result.Message = TEXT("batch_retarget: 'retargeter_path' and 'output_directory' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* AnimPathsJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("animation_paths"), AnimPathsJson) || !AnimPathsJson || AnimPathsJson->Num() == 0)
	{
		Result.Message = TEXT("batch_retarget: 'animation_paths' (non-empty array) is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
	if (!Retargeter)
	{
		Result.Message = FString::Printf(
			TEXT("batch_retarget: no UIKRetargeter found at '%s'"), *RetargeterPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Get source and target preview meshes from the retargeter via the controller.
	// UIKRetargetBatchOperation::DuplicateAndRetarget requires these to determine skeleton proportions.
	UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Retargeter);
	if (!Ctrl)
	{
		Result.Message = FString::Printf(
			TEXT("batch_retarget: UIKRetargeterController::GetController returned null for '%s'"), *RetargeterPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	USkeletalMesh* SourceMesh = Ctrl->GetPreviewMesh(ERetargetSourceOrTarget::Source);
	USkeletalMesh* TargetMesh = Ctrl->GetPreviewMesh(ERetargetSourceOrTarget::Target);

	if (!SourceMesh || !TargetMesh)
	{
		Result.Message = FString::Printf(
			TEXT("batch_retarget: retargeter at '%s' is missing a preview mesh - assign source and target preview meshes before running batch retarget (source=%s, target=%s)"),
			*RetargeterPath,
			SourceMesh ? TEXT("ok") : TEXT("MISSING"),
			TargetMesh ? TEXT("ok") : TEXT("MISSING"));
		Result.ErrorCode = 1000;
		return Result;
	}

	// Resolve animation assets and build FAssetData list
	TArray<FAssetData> AssetsToRetarget;
	TArray<FString> NotFoundPaths;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	for (const TSharedPtr<FJsonValue>& Val : *AnimPathsJson)
	{
		const FString AnimPath = Val.IsValid() ? Val->AsString() : FString();
		if (AnimPath.IsEmpty()) continue;

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AnimPath));
		if (!AssetData.IsValid())
		{
			// Try with suffix
			const FString Suffix = AnimPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AnimPath);
			AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Suffix));
		}

		if (AssetData.IsValid())
		{
			AssetsToRetarget.Add(AssetData);
		}
		else
		{
			NotFoundPaths.Add(AnimPath);
		}
	}

	if (AssetsToRetarget.Num() == 0)
	{
		Result.Message = FString::Printf(
			TEXT("batch_retarget: no valid animation assets found from provided paths (%d paths not found)"),
			NotFoundPaths.Num());
		Result.ErrorCode = 2000;
		return Result;
	}

	// Normalize output directory (strip trailing slash)
	FString OutDir = OutputDirectory;
	OutDir.RemoveFromEnd(TEXT("/"));

	// Run the batch duplicate-and-retarget operation.
	// The Suffix parameter is left blank; instead the output_directory places assets in the desired folder.
	// Use ReplaceFrom="" ReplaceTo="" Prefix="" Suffix="" so names are preserved as-is.
	const TArray<FAssetData> CreatedAssets = UIKRetargetBatchOperation::DuplicateAndRetarget(
		AssetsToRetarget,
		SourceMesh,
		TargetMesh,
		Retargeter,
		/*Search=*/TEXT(""),
		/*Replace=*/TEXT(""),
		/*Prefix=*/TEXT(""),
		/*Suffix=*/TEXT("_Retargeted"),
		/*bIncludeReferencedAssets=*/true,
		/*bOverwriteExistingFiles=*/false);

	const int32 SuccessCount = CreatedAssets.Num();

	FString Msg = FString::Printf(
		TEXT("batch_retarget: %d/%d animations retargeted (retargeter='%s')"),
		SuccessCount, AssetsToRetarget.Num(), *RetargeterPath);

	if (NotFoundPaths.Num() > 0)
	{
		Msg += FString::Printf(TEXT(" (%d source paths not found in asset registry)"), NotFoundPaths.Num());
	}
	if (SuccessCount < AssetsToRetarget.Num())
	{
		Msg += FString::Printf(TEXT(" (%d retarget operations failed)"), AssetsToRetarget.Num() - SuccessCount);
	}

	Result.bSuccess     = (SuccessCount > 0);
	Result.AffectedPath = OutputDirectory;
	Result.Message      = Msg;
	return Result;
}
