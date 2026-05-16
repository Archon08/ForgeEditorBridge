#include "Handlers/SkeletalMeshHandler.h"
#include "ForgeAISubsystem.h"

// ---- Skeletal mesh types ---------------------------------------------------
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ReferenceSkeleton.h"
#include "Animation/MorphTarget.h"
#include "Materials/MaterialInterface.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Misc/PackageName.h"

// ---- Import / reimport (editor-only) ---------------------------------------
#include "EditorFramework/AssetImportData.h"
#include "Misc/FeedbackContext.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"

#if WITH_EDITOR
#include "Editor/UnrealEdEngine.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
		return MakeError(TEXT("skeletal_mesh"), Action, 1000, TEXT("Params object is null"), TEXT("Provide a valid JSON params object."));

	if (Action == TEXT("set_material_slot"))    return Action_SetMaterialSlot(Params);
	if (Action == TEXT("get_bones"))            return Action_GetBones(Params);
	if (Action == TEXT("get_morph_targets"))    return Action_GetMorphTargets(Params);
	if (Action == TEXT("import_skeletal_mesh")) return Action_ImportSkeletalMesh(Params);
	if (Action == TEXT("reimport"))             return Action_Reimport(Params);
	if (Action == TEXT("get_info"))             return Action_GetInfo(Params);

	return MakeError(TEXT("skeletal_mesh"), Action, 1001,
		FString::Printf(TEXT("Unknown skeletal_mesh action '%s'. Valid: set_material_slot, get_bones, get_morph_targets, import_skeletal_mesh, reimport, get_info"), *Action),
		TEXT("Check action spelling against GetSupportedActions()."));
}

// ---------------------------------------------------------------------------
// set_material_slot
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_SetMaterialSlot(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("set_material_slot");

	FString AssetPath, MaterialPath;
	double SlotIndexD = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD) ||
	    !Params->TryGetStringField(TEXT("material"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("set_material_slot: 'asset_path', 'slot_index', and 'material' are required"),
			TEXT("Provide asset_path (string), slot_index (int), and material (string content path)."));
	}

	const int32 SlotIndex = FMath::Max(0, (int32)SlotIndexD);

	FBridgeResult TempResult;
	USkeletalMesh* Mesh = LoadSkeletalMesh(AssetPath, ActionName, TempResult);
	if (!Mesh) return TempResult;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		const FString Suffix = MaterialPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr, *Suffix);
	}
	if (!Material)
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 2000,
			FString::Printf(TEXT("No UMaterialInterface found at '%s'"), *MaterialPath),
			TEXT("Verify the material content path is correct and the asset exists."));
	}

	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	if (SlotIndex >= Materials.Num())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1001,
			FString::Printf(TEXT("Slot index %d out of range (mesh has %d slots)"), SlotIndex, Materials.Num()),
			TEXT("Use get_info to check material_count first."));
	}

	Mesh->GetMaterials()[SlotIndex].MaterialInterface = Material;
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Material slot %d set to '%s' on '%s'"), SlotIndex, *MaterialPath, *AssetPath));
}

// ---------------------------------------------------------------------------
// get_bones
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_GetBones(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("get_bones");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("get_bones: 'asset_path' is required"),
			TEXT("Provide the content path to a USkeletalMesh."));
	}

	FBridgeResult TempResult;
	USkeletalMesh* Mesh = LoadSkeletalMesh(AssetPath, ActionName, TempResult);
	if (!Mesh) return TempResult;

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	TArray<TSharedPtr<FJsonValue>> BonesArray;
	BonesArray.Reserve(NumBones);

	for (int32 i = 0; i < NumBones; ++i)
	{
		TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
		BoneObj->SetNumberField(TEXT("index"), i);
		BoneObj->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(i).ToString());
		BoneObj->SetNumberField(TEXT("parent_index"), RefSkeleton.GetParentIndex(i));

		const FTransform& BonePose = RefSkeleton.GetRefBonePose()[i];
		const FVector Pos = BonePose.GetTranslation();
		const FQuat Rot = BonePose.GetRotation();

		TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
		PosObj->SetNumberField(TEXT("x"), Pos.X);
		PosObj->SetNumberField(TEXT("y"), Pos.Y);
		PosObj->SetNumberField(TEXT("z"), Pos.Z);
		BoneObj->SetObjectField(TEXT("position"), PosObj);

		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("x"), Rot.X);
		RotObj->SetNumberField(TEXT("y"), Rot.Y);
		RotObj->SetNumberField(TEXT("z"), Rot.Z);
		RotObj->SetNumberField(TEXT("w"), Rot.W);
		BoneObj->SetObjectField(TEXT("rotation"), RotObj);

		BonesArray.Add(MakeShared<FJsonValueObject>(BoneObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("bones"), BonesArray);
	Data->SetNumberField(TEXT("bone_count"), NumBones);

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Retrieved %d bones from '%s'"), NumBones, *AssetPath), Data);
}

// ---------------------------------------------------------------------------
// get_morph_targets
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_GetMorphTargets(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("get_morph_targets");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("get_morph_targets: 'asset_path' is required"),
			TEXT("Provide the content path to a USkeletalMesh."));
	}

	FBridgeResult TempResult;
	USkeletalMesh* Mesh = LoadSkeletalMesh(AssetPath, ActionName, TempResult);
	if (!Mesh) return TempResult;

	const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();

	TArray<TSharedPtr<FJsonValue>> NamesArray;
	NamesArray.Reserve(MorphTargets.Num());

	for (const TObjectPtr<UMorphTarget>& MT : MorphTargets)
	{
		if (MT)
		{
			NamesArray.Add(MakeShared<FJsonValueString>(MT->GetName()));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("morph_targets"), NamesArray);
	Data->SetNumberField(TEXT("count"), NamesArray.Num());

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Retrieved %d morph targets from '%s'"), NamesArray.Num(), *AssetPath), Data);
}

// ---------------------------------------------------------------------------
// import_skeletal_mesh
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_ImportSkeletalMesh(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("import_skeletal_mesh");

#if WITH_EDITOR
	FString SourceFile, DestPath;
	if (!Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("import_skeletal_mesh: 'source_file' and 'dest_path' are required"),
			TEXT("Provide source_file (disk path to FBX/glTF) and dest_path (content path)."));
	}

	if (!FPaths::FileExists(SourceFile))
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 2000,
			FString::Printf(TEXT("Source file not found: '%s'"), *SourceFile),
			TEXT("Verify the file exists on disk."));
	}

	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourceFile);
	ImportData->DestinationPath = DestPath;
	ImportData->bReplaceExisting = true;

	FString SkeletonPath;
	if (Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) && !SkeletonPath.IsEmpty())
	{
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
		if (!Skeleton)
		{
			const FString Suffix = SkeletonPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SkeletonPath);
			Skeleton = LoadObject<USkeleton>(nullptr, *Suffix);
		}
		if (!Skeleton)
		{
			return MakeError(TEXT("skeletal_mesh"), ActionName, 2000,
				FString::Printf(TEXT("Skeleton not found: '%s'"), *SkeletonPath),
				TEXT("Verify the skeleton content path."));
		}
		// Factory settings would pick up skeleton from import data
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	TArray<UObject*> ImportedAssets = AssetTools.ImportAssetsAutomated(ImportData);

	if (ImportedAssets.Num() == 0)
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 3000,
			FString::Printf(TEXT("Import failed for '%s'"), *SourceFile),
			TEXT("Check the source file format and UE log for import errors."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ImportedPaths;
	for (UObject* Obj : ImportedAssets)
	{
		if (Obj)
		{
			ImportedPaths.Add(MakeShared<FJsonValueString>(Obj->GetPathName()));
		}
	}
	Data->SetArrayField(TEXT("imported_assets"), ImportedPaths);

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Imported %d asset(s) from '%s'"), ImportedAssets.Num(), *SourceFile), Data);
#else
	return MakeError(TEXT("skeletal_mesh"), ActionName, 3000,
		TEXT("import_skeletal_mesh requires WITH_EDITOR"),
		TEXT("This action is only available in editor builds."));
#endif
}

// ---------------------------------------------------------------------------
// reimport
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_Reimport(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("reimport");

#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("reimport: 'asset_path' is required"),
			TEXT("Provide the content path to a USkeletalMesh."));
	}

	FBridgeResult TempResult;
	USkeletalMesh* Mesh = LoadSkeletalMesh(AssetPath, ActionName, TempResult);
	if (!Mesh) return TempResult;

	bool bSuccess = FReimportManager::Instance()->Reimport(Mesh, /*bAskForNewFileIfMissing=*/false);
	if (!bSuccess)
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 3000,
			FString::Printf(TEXT("Reimport failed for '%s'"), *AssetPath),
			TEXT("Check that the original source file still exists and is accessible."));
	}

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Reimported '%s' successfully"), *AssetPath));
#else
	return MakeError(TEXT("skeletal_mesh"), ActionName, 3000,
		TEXT("reimport requires WITH_EDITOR"),
		TEXT("This action is only available in editor builds."));
#endif
}

// ---------------------------------------------------------------------------
// get_info
// ---------------------------------------------------------------------------

FBridgeResult USkeletalMeshHandler::Action_GetInfo(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("get_info");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("skeletal_mesh"), ActionName, 1000,
			TEXT("get_info: 'asset_path' is required"),
			TEXT("Provide the content path to a USkeletalMesh."));
	}

	FBridgeResult TempResult;
	USkeletalMesh* Mesh = LoadSkeletalMesh(AssetPath, ActionName, TempResult);
	if (!Mesh) return TempResult;

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("bone_count"), RefSkeleton.GetNum());
	Data->SetNumberField(TEXT("morph_target_count"), Mesh->GetMorphTargets().Num());
	Data->SetNumberField(TEXT("material_count"), Mesh->GetMaterials().Num());

	// LOD info
	const int32 LODCount = Mesh->GetLODNum();
	Data->SetNumberField(TEXT("lod_count"), LODCount);

	// Vertex counts per LOD
	TArray<TSharedPtr<FJsonValue>> VertexCounts;
	if (const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
	{
		for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
		{
			const int32 NumVerts = RenderData->LODRenderData[LODIdx].GetNumVertices();
			VertexCounts.Add(MakeShared<FJsonValueNumber>(NumVerts));
		}
	}
	Data->SetArrayField(TEXT("vertex_counts_per_lod"), VertexCounts);

	// Skeleton path
	if (USkeleton* Skel = Mesh->GetSkeleton())
	{
		Data->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("skeleton_path"), TEXT(""));
	}

	// Physics asset path
	if (UPhysicsAsset* PhysAsset = Mesh->GetPhysicsAsset())
	{
		Data->SetStringField(TEXT("physics_asset_path"), PhysAsset->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("physics_asset_path"), TEXT(""));
	}

	return MakeSuccess(TEXT("skeletal_mesh"), ActionName,
		FString::Printf(TEXT("Info for '%s': %d bones, %d LODs"), *AssetPath, RefSkeleton.GetNum(), LODCount), Data);
}

// ---------------------------------------------------------------------------
// LoadSkeletalMesh helper
// ---------------------------------------------------------------------------

USkeletalMesh* USkeletalMeshHandler::LoadSkeletalMesh(const FString& AssetPath, const FString& ActionName, FBridgeResult& OutResult)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Mesh = LoadObject<USkeletalMesh>(nullptr, *Suffix);
	}
	if (!Mesh)
	{
		OutResult = MakeError(TEXT("skeletal_mesh"), ActionName, 2000,
			FString::Printf(TEXT("No USkeletalMesh found at '%s'"), *AssetPath),
			TEXT("Verify the content path. Use the asset_handler list_assets action to search."));
	}
	return Mesh;
}

TSharedPtr<FJsonObject> USkeletalMeshHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a material on a skeletal mesh slot")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the skeletal mesh"))); Ps->SetObjectField(TEXT("slot_index"), P(TEXT("int"), true, TEXT("Material slot index"))); Ps->SetObjectField(TEXT("material"), P(TEXT("string"), true, TEXT("Content path of the material"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_material_slot"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get bone hierarchy from a skeletal mesh")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the skeletal mesh"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_bones"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get morph target names from a skeletal mesh")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the skeletal mesh"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_morph_targets"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Import a skeletal mesh from disk (FBX/glTF)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("source_file"), P(TEXT("string"), true, TEXT("Disk path to FBX/glTF file"))); Ps->SetObjectField(TEXT("dest_path"), P(TEXT("string"), true, TEXT("Content destination path"))); Ps->SetObjectField(TEXT("skeleton"), P(TEXT("string"), false, TEXT("Content path of existing USkeleton to use"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("import_skeletal_mesh"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Reimport a skeletal mesh from its original source")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the skeletal mesh"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("reimport"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get skeletal mesh info (bones, LODs, materials)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the skeletal mesh"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_info"), A); }

	return Root;
}
