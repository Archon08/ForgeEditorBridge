#include "Handlers/StaticMeshHandler.h"
#include "ForgeAISubsystem.h"

// ---- Static mesh types -----------------------------------------------------
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

// ---- Physics / Collision ---------------------------------------------------
#include "PhysicsEngine/BodySetup.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UStaticMeshHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("static_mesh"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("set_nanite"))          return Action_SetNanite(Params);
	if (Action == TEXT("set_lod_screen_size")) return Action_SetLODScreenSize(Params);
	if (Action == TEXT("set_material_slot"))   return Action_SetMaterialSlot(Params);
	if (Action == TEXT("get_info"))            return Action_GetInfo(Params);
	if (Action == TEXT("set_collision"))       return Action_SetCollision(Params);

	FBridgeResult R = CreateResult(TEXT("static_mesh"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown static_mesh action '%s'. Valid: set_nanite, set_lod_screen_size, set_material_slot, get_info, set_collision"),
		*Action);
	return R;
}

// ---------------------------------------------------------------------------
// set_nanite
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::Action_SetNanite(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("static_mesh"), TEXT("set_nanite"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("set_nanite: 'asset_path' is required");
		return Result;
	}

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	UStaticMesh* Mesh = LoadStaticMesh(AssetPath, Result);
	if (!Mesh) return Result;

	// UE 5.7: NaniteSettings direct access deprecated; use getter/setter.
	FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
	NaniteSettings.bEnabled = bEnabled;
	Mesh->SetNaniteSettings(NaniteSettings);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Nanite %s on '%s'"), bEnabled ? TEXT("enabled") : TEXT("disabled"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_lod_screen_size
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::Action_SetLODScreenSize(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("static_mesh"), TEXT("set_lod_screen_size"));

	FString AssetPath;
	double LODIndexD = 0.0, ScreenSize = 0.5;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetNumberField(TEXT("lod_index"),   LODIndexD)  ||
	    !Params->TryGetNumberField(TEXT("screen_size"), ScreenSize))
	{
		Result.Message = TEXT("set_lod_screen_size: 'asset_path', 'lod_index', and 'screen_size' are required");
		return Result;
	}

	const int32 LODIndex = FMath::Max(0, (int32)LODIndexD);

	UStaticMesh* Mesh = LoadStaticMesh(AssetPath, Result);
	if (!Mesh) return Result;

	if (LODIndex >= Mesh->GetNumSourceModels())
	{
		Result.Message = FString::Printf(
			TEXT("set_lod_screen_size: LOD index %d is out of range for mesh '%s' (has %d source models)"),
			LODIndex, *AssetPath, Mesh->GetNumSourceModels());
		return Result;
	}

	Mesh->GetSourceModel(LODIndex).ScreenSize.Default = (float)ScreenSize;
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("LOD %d screen size set to %.4f on '%s'"), LODIndex, (float)ScreenSize, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_material_slot
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::Action_SetMaterialSlot(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("static_mesh"), TEXT("set_material_slot"));

	FString AssetPath, MaterialPath;
	double SlotIndexD = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("slot_index"),    SlotIndexD))
	{
		Result.Message = TEXT("set_material_slot: 'asset_path', 'material_path', and 'slot_index' are required");
		return Result;
	}

	const int32 SlotIndex = FMath::Max(0, (int32)SlotIndexD);

	UStaticMesh* Mesh = LoadStaticMesh(AssetPath, Result);
	if (!Mesh) return Result;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		const FString Suffix = MaterialPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr, *Suffix);
	}
	if (!Material)
	{
		Result.Message = FString::Printf(
			TEXT("set_material_slot: no UMaterialInterface found at '%s'"), *MaterialPath);
		return Result;
	}

	if (SlotIndex >= Mesh->GetStaticMaterials().Num())
	{
		Result.Message = FString::Printf(
			TEXT("set_material_slot: slot index %d is out of range for mesh '%s' (has %d slots)"),
			SlotIndex, *AssetPath, Mesh->GetStaticMaterials().Num());
		return Result;
	}

	Mesh->SetMaterial(SlotIndex, Material);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Material slot %d set to '%s' on '%s'"), SlotIndex, *MaterialPath, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_info
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::Action_GetInfo(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("static_mesh"), TEXT("get_info"),
			1000, TEXT("get_info: 'asset_path' is required"),
			TEXT("Provide the content path of a UStaticMesh asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("static_mesh"), TEXT("get_info"));
	UStaticMesh* Mesh = LoadStaticMesh(AssetPath, TempResult);
	if (!Mesh)
	{
		return MakeError(TEXT("static_mesh"), TEXT("get_info"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UStaticMesh"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// LOD count
	Data->SetNumberField(TEXT("num_lods"), Mesh->GetNumLODs());

	// Bounds
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
	BoundsObj->SetStringField(TEXT("origin"), FString::Printf(TEXT("(%.2f, %.2f, %.2f)"),
		Bounds.Origin.X, Bounds.Origin.Y, Bounds.Origin.Z));
	BoundsObj->SetStringField(TEXT("box_extent"), FString::Printf(TEXT("(%.2f, %.2f, %.2f)"),
		Bounds.BoxExtent.X, Bounds.BoxExtent.Y, Bounds.BoxExtent.Z));
	BoundsObj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	Data->SetObjectField(TEXT("bounds"), BoundsObj);

	// Triangle count (LOD 0)
	if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
	{
		int32 TotalTris = 0;
		for (const FStaticMeshSection& Section : Mesh->GetRenderData()->LODResources[0].Sections)
		{
			TotalTris += Section.NumTriangles;
		}
		Data->SetNumberField(TEXT("triangle_count_lod0"), TotalTris);
	}

	// Nanite
	Data->SetBoolField(TEXT("nanite_enabled"), Mesh->GetNaniteSettings().bEnabled);

	// Material count
	Data->SetNumberField(TEXT("material_count"), Mesh->GetStaticMaterials().Num());

	return MakeSuccess(TEXT("static_mesh"), TEXT("get_info"),
		FString::Printf(TEXT("Static mesh info for '%s'"), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_collision
// ---------------------------------------------------------------------------

FBridgeResult UStaticMeshHandler::Action_SetCollision(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, CollisionType;
	if (!Params->TryGetStringField(TEXT("asset_path"),     AssetPath)     || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("collision_type"), CollisionType) || CollisionType.IsEmpty())
	{
		return MakeError(TEXT("static_mesh"), TEXT("set_collision"),
			1000, TEXT("set_collision: 'asset_path' and 'collision_type' are required"),
			TEXT("collision_type: NoCollision, BlockAll, BlockAllDynamic, OverlapAll, etc."));
	}

	FBridgeResult TempResult = CreateResult(TEXT("static_mesh"), TEXT("set_collision"));
	UStaticMesh* Mesh = LoadStaticMesh(AssetPath, TempResult);
	if (!Mesh)
	{
		return MakeError(TEXT("static_mesh"), TEXT("set_collision"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UStaticMesh"));
	}

	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
	{
		return MakeError(TEXT("static_mesh"), TEXT("set_collision"),
			3000, FString::Printf(TEXT("Mesh '%s' has no BodySetup"), *AssetPath),
			TEXT("Ensure the mesh has collision data or rebuild collision"));
	}

	// Map collision_type string to collision trace flag
	bool bComplexAsSimple = false;
	Params->TryGetBoolField(TEXT("complex_as_simple"), bComplexAsSimple);

	if (bComplexAsSimple)
	{
		BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	}
	else if (CollisionType == TEXT("NoCollision"))
	{
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->DefaultInstance.SetCollisionProfileName(FName(TEXT("NoCollision")));
	}
	else
	{
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAndComplex;
		BodySetup->DefaultInstance.SetCollisionProfileName(FName(*CollisionType));
	}

	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	return MakeSuccess(TEXT("static_mesh"), TEXT("set_collision"),
		FString::Printf(TEXT("Collision set to '%s' (complex_as_simple=%s) on '%s'"),
			*CollisionType, bComplexAsSimple ? TEXT("true") : TEXT("false"), *AssetPath));
}

// ---------------------------------------------------------------------------
// LoadStaticMesh helper
// ---------------------------------------------------------------------------

UStaticMesh* UStaticMeshHandler::LoadStaticMesh(const FString& AssetPath, FBridgeResult& Result)
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Mesh = LoadObject<UStaticMesh>(nullptr, *Suffix);
	}
	if (!Mesh)
	{
		Result.Message = FString::Printf(
			TEXT("LoadStaticMesh: no UStaticMesh found at '%s'"), *AssetPath);
	}
	return Mesh;
}

TSharedPtr<FJsonObject> UStaticMeshHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Enable or disable Nanite on a static mesh")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the static mesh"))); Ps->SetObjectField(TEXT("enabled"), P(TEXT("bool"), false, TEXT("Enable Nanite (default true)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_nanite"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set screen size threshold for an LOD")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the static mesh"))); Ps->SetObjectField(TEXT("lod_index"), P(TEXT("int"), true, TEXT("LOD index"))); Ps->SetObjectField(TEXT("screen_size"), P(TEXT("float"), true, TEXT("Screen size fraction (0-1)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_lod_screen_size"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a material on a static mesh slot")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the static mesh"))); Ps->SetObjectField(TEXT("material_path"), P(TEXT("string"), true, TEXT("Content path of the material"))); Ps->SetObjectField(TEXT("slot_index"), P(TEXT("int"), true, TEXT("Material slot index"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_material_slot"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get static mesh info (LODs, bounds, triangles, Nanite)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the static mesh"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_info"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set collision profile on a static mesh")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the static mesh"))); Ps->SetObjectField(TEXT("collision_type"), P(TEXT("string"), true, TEXT("Collision profile: NoCollision, BlockAll, etc."))); Ps->SetObjectField(TEXT("complex_as_simple"), P(TEXT("bool"), false, TEXT("Use complex mesh as simple collision"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_collision"), A); }

	return Root;
}
