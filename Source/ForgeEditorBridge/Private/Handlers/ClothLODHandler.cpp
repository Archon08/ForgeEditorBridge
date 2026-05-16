#include "Handlers/ClothLODHandler.h"
#include "ForgeAISubsystem.h"

// ---- Skeletal mesh / LOD ---------------------------------------------------
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODImporterData.h"   // FSkeletalMeshLODInfo (transitive via SkeletalMesh.h)
#include "Misc/PackageName.h"

// ---- Clothing --------------------------------------------------------------
#include "ClothingAssetBase.h"                        // UClothingAssetBase

// ---- JSON / property -------------------------------------------------------
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"                       // FProperty, ImportText_Direct

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UClothLODHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UClothLODHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("cloth_lod"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("bind_cloth"))          return Action_BindCloth(Params);
	if (Action == TEXT("set_cloth_config"))    return Action_SetClothConfig(Params);
	if (Action == TEXT("set_lod_screen_size")) return Action_SetLODScreenSize(Params);

	FBridgeResult R = CreateResult(TEXT("cloth_lod"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown cloth_lod action '%s'. Valid: bind_cloth, set_cloth_config, set_lod_screen_size"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// bind_cloth
// ---------------------------------------------------------------------------

FBridgeResult UClothLODHandler::Action_BindCloth(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("cloth_lod"), TEXT("bind_cloth"));

	FString MeshPath, ClothAssetPath;
	if (!Params->TryGetStringField(TEXT("mesh_path"),        MeshPath)       || MeshPath.IsEmpty()       ||
	    !Params->TryGetStringField(TEXT("cloth_asset_path"), ClothAssetPath) || ClothAssetPath.IsEmpty())
	{
		Result.Message = TEXT("bind_cloth: 'mesh_path' and 'cloth_asset_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	double LODIndexD = 0.0, SectionIndexD = 0.0;
	Params->TryGetNumberField(TEXT("lod_index"),     LODIndexD);
	Params->TryGetNumberField(TEXT("section_index"), SectionIndexD);
	const int32 LODIndex     = FMath::Max(0, (int32)LODIndexD);
	const int32 SectionIndex = FMath::Max(0, (int32)SectionIndexD);

	// Load skeletal mesh
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		const FString Suffix = MeshPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MeshPath);
		Mesh = LoadObject<USkeletalMesh>(nullptr, *Suffix);
	}
	if (!Mesh)
	{
		Result.Message = FString::Printf(TEXT("bind_cloth: no USkeletalMesh found at '%s'"), *MeshPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify mesh_path points to a valid USkeletalMesh asset");
		return Result;
	}

	// Load clothing asset
	UClothingAssetBase* ClothAsset = LoadObject<UClothingAssetBase>(nullptr, *ClothAssetPath);
	if (!ClothAsset)
	{
		const FString Suffix = ClothAssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ClothAssetPath);
		ClothAsset = LoadObject<UClothingAssetBase>(nullptr, *Suffix);
	}
	if (!ClothAsset)
	{
		Result.Message = FString::Printf(
			TEXT("bind_cloth: no UClothingAssetBase found at '%s'"), *ClothAssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify cloth_asset_path points to a valid UClothingAssetBase asset");
		return Result;
	}

	// Bind to the mesh section
	ClothAsset->BindToSkeletalMesh(Mesh, LODIndex, SectionIndex, 0);
	Mesh->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = MeshPath;
	Result.Message      = FString::Printf(
		TEXT("Cloth asset '%s' bound to mesh '%s' (LOD %d, Section %d)"),
		*ClothAssetPath, *MeshPath, LODIndex, SectionIndex);
	return Result;
}

// ---------------------------------------------------------------------------
// set_cloth_config
// ---------------------------------------------------------------------------

FBridgeResult UClothLODHandler::Action_SetClothConfig(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("cloth_lod"), TEXT("set_cloth_config"));

	FString ClothAssetPath, PropertyName, Value;
	if (!Params->TryGetStringField(TEXT("cloth_asset_path"), ClothAssetPath) || ClothAssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("property"),         PropertyName)    || PropertyName.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("value"),            Value))
	{
		Result.Message = TEXT("set_cloth_config: 'cloth_asset_path', 'property', and 'value' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UClothingAssetBase* ClothAsset = LoadObject<UClothingAssetBase>(nullptr, *ClothAssetPath);
	if (!ClothAsset)
	{
		const FString Suffix = ClothAssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ClothAssetPath);
		ClothAsset = LoadObject<UClothingAssetBase>(nullptr, *Suffix);
	}
	if (!ClothAsset)
	{
		Result.Message = FString::Printf(
			TEXT("set_cloth_config: no UClothingAssetBase found at '%s'"), *ClothAssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify cloth_asset_path points to a valid UClothingAssetBase asset");
		return Result;
	}

	// Walk the cloth asset's inner objects (config sub-objects) and look for the property
	bool bFound = false;
	TArray<UObject*> InnerObjects;
	GetObjectsWithOuter(ClothAsset, InnerObjects, /*bIncludeNestedObjects=*/true);
	InnerObjects.Add(ClothAsset);   // also check the asset itself

	for (UObject* InnerObj : InnerObjects)
	{
		if (!InnerObj) continue;
		FProperty* Prop = InnerObj->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (Prop)
		{
			void* Container = Prop->ContainerPtrToValuePtr<void>(InnerObj);
			Prop->ImportText_Direct(*Value, Container, InnerObj, PPF_None);
			InnerObj->MarkPackageDirty();
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		Result.Message = FString::Printf(
			TEXT("set_cloth_config: property '%s' not found on cloth asset '%s' or its config objects. "
			     "Common Chaos properties: EdgeStiffnessWeft, BendingStiffnessWeft, DampingCoefficient, GravityScale"),
			*PropertyName, *ClothAssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = ClothAssetPath;
	Result.Message      = FString::Printf(
		TEXT("Cloth config property '%s' set to '%s' on '%s'"), *PropertyName, *Value, *ClothAssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_lod_screen_size
// ---------------------------------------------------------------------------

FBridgeResult UClothLODHandler::Action_SetLODScreenSize(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("cloth_lod"), TEXT("set_lod_screen_size"));

	FString MeshPath;
	double LODIndexD = 0.0, ScreenSize = 0.5;
	if (!Params->TryGetStringField(TEXT("mesh_path"),   MeshPath)   || MeshPath.IsEmpty()   ||
	    !Params->TryGetNumberField(TEXT("lod_index"),   LODIndexD)  ||
	    !Params->TryGetNumberField(TEXT("screen_size"), ScreenSize))
	{
		Result.Message = TEXT("set_lod_screen_size: 'mesh_path', 'lod_index', and 'screen_size' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	const int32 LODIndex = FMath::Max(0, (int32)LODIndexD);

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		const FString Suffix = MeshPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MeshPath);
		Mesh = LoadObject<USkeletalMesh>(nullptr, *Suffix);
	}
	if (!Mesh)
	{
		Result.Message = FString::Printf(TEXT("set_lod_screen_size: no USkeletalMesh found at '%s'"), *MeshPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify mesh_path points to a valid USkeletalMesh asset");
		return Result;
	}

	if (!Mesh->IsValidLODIndex(LODIndex))
	{
		Result.Message = FString::Printf(
			TEXT("set_lod_screen_size: LOD index %d is out of range for mesh '%s' (has %d LODs)"),
			LODIndex, *MeshPath, Mesh->GetLODNum());
		Result.ErrorCode = 1001;
		return Result;
	}

	FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIndex);
	if (!LODInfo)
	{
		Result.Message = FString::Printf(
			TEXT("set_lod_screen_size: GetLODInfo returned null for LOD %d on '%s'"),
			LODIndex, *MeshPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	LODInfo->ScreenSize.Default = (float)ScreenSize;
	Mesh->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = MeshPath;
	Result.Message      = FString::Printf(
		TEXT("LOD %d screen size set to %.4f on '%s'"), LODIndex, (float)ScreenSize, *MeshPath);
	return Result;
}
