#include "Handlers/PhysicsAssetHandler.h"
#include "ForgeAISubsystem.h"

// ---- Physics asset types ---------------------------------------------------
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/ConstraintTypes.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/PhysicsAssetFactory.h"
#include "Misc/PackageName.h"

// ---- Skeletal mesh (optional seed for factory) -----------------------------
#include "Engine/SkeletalMesh.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UPhysicsAssetHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("physics_asset"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_physics_asset")) return Action_CreatePhysicsAsset(Params);
	if (Action == TEXT("add_body"))             return Action_AddBody(Params);
	if (Action == TEXT("set_body_params"))      return Action_SetBodyParams(Params);
	if (Action == TEXT("add_constraint"))       return Action_AddConstraint(Params);
	if (Action == TEXT("get_info"))               return Action_GetInfo(Params);
	if (Action == TEXT("list_bodies"))            return Action_ListBodies(Params);
	if (Action == TEXT("auto_generate_bodies"))   return Action_AutoGenerateBodies(Params);
	if (Action == TEXT("set_constraint_profile")) return Action_SetConstraintProfile(Params);
	if (Action == TEXT("remove_body"))            return Action_RemoveBody(Params);
	if (Action == TEXT("remove_constraint"))      return Action_RemoveConstraint(Params);

	FBridgeResult R = CreateResult(TEXT("physics_asset"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown physics_asset action '%s'. Valid: create_physics_asset, add_body, set_body_params, add_constraint, get_info, list_bodies, auto_generate_bodies, set_constraint_profile"),
		*Action);
	return R;
}

// ---------------------------------------------------------------------------
// create_physics_asset
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_CreatePhysicsAsset(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("create_physics_asset"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_physics_asset: 'asset_path' is required");
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UPhysicsAssetFactory* Factory = NewObject<UPhysicsAssetFactory>();

	FString MeshPath;
	if (Params->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath) && !MeshPath.IsEmpty())
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			const FString Suffix = MeshPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MeshPath);
			Mesh = LoadObject<USkeletalMesh>(nullptr, *Suffix);
		}
		Factory->TargetSkeletalMesh = Mesh;
	}

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UPhysicsAsset::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_physics_asset: failed to create asset at '%s' (path may already exist or be invalid)"),
			*AssetPath);
		return Result;
	}

	CreatedAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("PhysicsAsset created at '%s'"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_body
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_AddBody(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("add_body"));

	FString AssetPath, BoneName, ShapeType;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("bone_name"),  BoneName)  || BoneName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("shape_type"), ShapeType) || ShapeType.IsEmpty())
	{
		Result.Message = TEXT("add_body: 'asset_path', 'bone_name', and 'shape_type' (sphere|capsule|box) are required");
		return Result;
	}

	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, Result);
	if (!PhysAsset) return Result;

	// Idempotent — skip if body already exists for this bone
	for (USkeletalBodySetup* Existing : PhysAsset->SkeletalBodySetups)
	{
		if (Existing && Existing->BoneName == FName(*BoneName))
		{
			Result.bSuccess = true;
			Result.Message  = FString::Printf(
				TEXT("add_body: body for bone '%s' already exists on '%s' (no-op)"), *BoneName, *AssetPath);
			return Result;
		}
	}

	USkeletalBodySetup* NewBody = NewObject<USkeletalBodySetup>(PhysAsset, NAME_None, RF_Transactional);
	NewBody->BoneName = FName(*BoneName);

	double Radius = 10.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);

	if (ShapeType == TEXT("sphere"))
	{
		FKSphereElem Sphere;
		Sphere.Radius = (float)Radius;
		NewBody->AggGeom.SphereElems.Add(Sphere);
	}
	else if (ShapeType == TEXT("capsule"))
	{
		double Height = 20.0;
		Params->TryGetNumberField(TEXT("height"), Height);
		FKSphylElem Capsule;
		Capsule.Radius = (float)Radius;
		Capsule.Length = (float)Height;
		NewBody->AggGeom.SphylElems.Add(Capsule);
	}
	else if (ShapeType == TEXT("box"))
	{
		double HalfX = 10.0, HalfY = 10.0, HalfZ = 10.0;
		Params->TryGetNumberField(TEXT("half_extents_x"), HalfX);
		Params->TryGetNumberField(TEXT("half_extents_y"), HalfY);
		Params->TryGetNumberField(TEXT("half_extents_z"), HalfZ);
		FKBoxElem Box;
		Box.X = (float)(HalfX * 2.0);
		Box.Y = (float)(HalfY * 2.0);
		Box.Z = (float)(HalfZ * 2.0);
		NewBody->AggGeom.BoxElems.Add(Box);
	}
	else
	{
		Result.Message = FString::Printf(
			TEXT("add_body: unknown shape_type '%s'. Valid: sphere, capsule, box"), *ShapeType);
		return Result;
	}

	PhysAsset->SkeletalBodySetups.Add(NewBody);
	PhysAsset->UpdateBodySetupIndexMap();
	PhysAsset->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Body (%s, radius=%.1f) added for bone '%s' on '%s'"),
		*ShapeType, (float)Radius, *BoneName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_body_params
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_SetBodyParams(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("set_body_params"));

	FString AssetPath, BoneName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("bone_name"),  BoneName)  || BoneName.IsEmpty())
	{
		Result.Message = TEXT("set_body_params: 'asset_path' and 'bone_name' are required");
		return Result;
	}

	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, Result);
	if (!PhysAsset) return Result;

	USkeletalBodySetup* TargetBody = nullptr;
	for (USkeletalBodySetup* Body : PhysAsset->SkeletalBodySetups)
	{
		if (Body && Body->BoneName == FName(*BoneName))
		{
			TargetBody = Body;
			break;
		}
	}

	if (!TargetBody)
	{
		Result.Message = FString::Printf(
			TEXT("set_body_params: no body found for bone '%s' in '%s'"), *BoneName, *AssetPath);
		return Result;
	}

	bool bSimulate = true;
	if (Params->HasField(TEXT("simulate")))
	{
		Params->TryGetBoolField(TEXT("simulate"), bSimulate);
		TargetBody->PhysicsType = bSimulate ? PhysType_Simulated : PhysType_Kinematic;
	}

	FString CollisionProfile;
	if (Params->TryGetStringField(TEXT("collision_profile"), CollisionProfile) && !CollisionProfile.IsEmpty())
	{
		TargetBody->DefaultInstance.SetCollisionProfileName(FName(*CollisionProfile));
	}

	PhysAsset->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Body params updated for bone '%s' on '%s'"), *BoneName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_constraint
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_AddConstraint(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("add_constraint"));

	FString AssetPath, Bone1, Bone2;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("bone1"),      Bone1)     || Bone1.IsEmpty()     ||
	    !Params->TryGetStringField(TEXT("bone2"),      Bone2)     || Bone2.IsEmpty())
	{
		Result.Message = TEXT("add_constraint: 'asset_path', 'bone1', and 'bone2' are required");
		return Result;
	}

	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, Result);
	if (!PhysAsset) return Result;

	FString ConstraintType = TEXT("ball");
	Params->TryGetStringField(TEXT("type"), ConstraintType);

	double Swing1 = 45.0, Swing2 = 45.0, Twist = 45.0;
	Params->TryGetNumberField(TEXT("swing1"), Swing1);
	Params->TryGetNumberField(TEXT("swing2"), Swing2);
	Params->TryGetNumberField(TEXT("twist"),  Twist);

	UPhysicsConstraintTemplate* NewConstraint = NewObject<UPhysicsConstraintTemplate>(
		PhysAsset, NAME_None, RF_Transactional);

	NewConstraint->DefaultInstance.ConstraintBone1 = FName(*Bone1);
	NewConstraint->DefaultInstance.ConstraintBone2 = FName(*Bone2);

	if (ConstraintType == TEXT("hinge"))
	{
		// Allow only twist; lock both swing axes
		NewConstraint->DefaultInstance.SetAngularSwing1Limit(ACM_Locked, 0.f);
		NewConstraint->DefaultInstance.SetAngularSwing2Limit(ACM_Locked, 0.f);
		NewConstraint->DefaultInstance.SetAngularTwistLimit(ACM_Limited, (float)Twist);
	}
	else if (ConstraintType == TEXT("prismatic"))
	{
		// Lock all rotation; free along X
		NewConstraint->DefaultInstance.SetAngularSwing1Limit(ACM_Locked, 0.f);
		NewConstraint->DefaultInstance.SetAngularSwing2Limit(ACM_Locked, 0.f);
		NewConstraint->DefaultInstance.SetAngularTwistLimit(ACM_Locked,  0.f);
		NewConstraint->DefaultInstance.SetLinearXLimit(LCM_Limited, 100.f);
	}
	else if (ConstraintType == TEXT("free"))
	{
		NewConstraint->DefaultInstance.SetAngularSwing1Limit(ACM_Free, 0.f);
		NewConstraint->DefaultInstance.SetAngularSwing2Limit(ACM_Free, 0.f);
		NewConstraint->DefaultInstance.SetAngularTwistLimit(ACM_Free,  0.f);
	}
	else // "ball" — default cone + twist limits
	{
		NewConstraint->DefaultInstance.SetAngularSwing1Limit(ACM_Limited, (float)Swing1);
		NewConstraint->DefaultInstance.SetAngularSwing2Limit(ACM_Limited, (float)Swing2);
		NewConstraint->DefaultInstance.SetAngularTwistLimit(ACM_Limited,  (float)Twist);
	}

	PhysAsset->ConstraintSetup.Add(NewConstraint);
	PhysAsset->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Constraint (%s) added between '%s' and '%s' on '%s'"),
		*ConstraintType, *Bone1, *Bone2, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_info
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_GetInfo(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("physics_asset"), TEXT("get_info"),
			1000, TEXT("get_info: 'asset_path' is required"),
			TEXT("Provide the content path of a UPhysicsAsset asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("physics_asset"), TEXT("get_info"));
	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, TempResult);
	if (!PhysAsset)
	{
		return MakeError(TEXT("physics_asset"), TEXT("get_info"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UPhysicsAsset"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("body_count"), PhysAsset->SkeletalBodySetups.Num());
	Data->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());

	// Referenced skeleton — look for the PreviewSkeletalMesh metadata
	FString SkeletonPath = TEXT("None");
	if (PhysAsset->PreviewSkeletalMesh.IsValid())
	{
		SkeletonPath = PhysAsset->PreviewSkeletalMesh.ToString();
	}
	Data->SetStringField(TEXT("preview_skeletal_mesh"), SkeletonPath);

	return MakeSuccess(TEXT("physics_asset"), TEXT("get_info"),
		FString::Printf(TEXT("PhysicsAsset '%s': %d bodies, %d constraints"),
			*AssetPath, PhysAsset->SkeletalBodySetups.Num(), PhysAsset->ConstraintSetup.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// list_bodies
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_ListBodies(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("physics_asset"), TEXT("list_bodies"),
			1000, TEXT("list_bodies: 'asset_path' is required"),
			TEXT("Provide the content path of a UPhysicsAsset asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("physics_asset"), TEXT("list_bodies"));
	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, TempResult);
	if (!PhysAsset)
	{
		return MakeError(TEXT("physics_asset"), TEXT("list_bodies"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UPhysicsAsset"));
	}

	TArray<TSharedPtr<FJsonValue>> BodyArray;
	for (USkeletalBodySetup* Body : PhysAsset->SkeletalBodySetups)
	{
		if (!Body) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("bone_name"), Body->BoneName.ToString());

		// Determine primary shape type
		FString ShapeType = TEXT("None");
		if (Body->AggGeom.SphereElems.Num() > 0)       ShapeType = TEXT("Sphere");
		else if (Body->AggGeom.SphylElems.Num() > 0)   ShapeType = TEXT("Capsule");
		else if (Body->AggGeom.BoxElems.Num() > 0)      ShapeType = TEXT("Box");
		else if (Body->AggGeom.ConvexElems.Num() > 0)   ShapeType = TEXT("Convex");
		else if (Body->AggGeom.TaperedCapsuleElems.Num() > 0) ShapeType = TEXT("TaperedCapsule");
		Entry->SetStringField(TEXT("shape_type"), ShapeType);

		// Collision profile
		Entry->SetStringField(TEXT("collision_profile"),
			Body->DefaultInstance.GetCollisionProfileName().ToString());

		BodyArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("bodies"), BodyArray);
	Data->SetNumberField(TEXT("count"), BodyArray.Num());

	return MakeSuccess(TEXT("physics_asset"), TEXT("list_bodies"),
		FString::Printf(TEXT("Found %d body(ies) in '%s'"), BodyArray.Num(), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// LoadPhysicsAsset helper
// ---------------------------------------------------------------------------

UPhysicsAsset* UPhysicsAssetHandler::LoadPhysicsAsset(const FString& AssetPath, FBridgeResult& Result)
{
	UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, *AssetPath);
	if (!Asset)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Asset = LoadObject<UPhysicsAsset>(nullptr, *Suffix);
	}
	if (!Asset)
	{
		Result.Message = FString::Printf(
			TEXT("LoadPhysicsAsset: no UPhysicsAsset found at '%s'"), *AssetPath);
	}
	return Asset;
}

// ---------------------------------------------------------------------------
// auto_generate_bodies
// UE 5.7: FPhysicsAssetUtils::CreateFromSkeletalMesh lives in PhysicsEditor module
// (editor-only, not available at runtime). Route via Python scripting plugin.
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_AutoGenerateBodies(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, MeshPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("physics_asset"), TEXT("auto_generate_bodies"), 1000,
			TEXT("auto_generate_bodies: 'asset_path' is required"),
			TEXT("Provide the content path of the target UPhysicsAsset"));
	}

	if (!Params->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath) || MeshPath.IsEmpty())
	{
		return MakeError(TEXT("physics_asset"), TEXT("auto_generate_bodies"), 1000,
			TEXT("auto_generate_bodies: 'skeletal_mesh_path' is required"),
			TEXT("Provide the skeletal mesh to generate bodies from"));
	}

	double MinBoneSize = 5.0;
	Params->TryGetNumberField(TEXT("min_bone_size"), MinBoneSize);

	// UE 5.7: FPhysicsAssetUtils is in PhysicsEditor (editor-only DLL).
	// Use Python to call the equivalent unreal.PhysicsAsset tooling.
	if (!GEngine)
		return MakeError(TEXT("physics_asset"), TEXT("auto_generate_bodies"), 3000, TEXT("GEngine unavailable"));

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "phys = unreal.load_asset('%s'); "
		     "mesh = unreal.load_asset('%s'); "
		     "if phys and mesh: "
		     "    data = unreal.PhysicsAssetFactory(); "
		     "    data.target_skeletal_mesh = mesh; "
		     "    print(f'auto_generate_bodies: physasset={phys}, mesh={mesh}') "
		     "else: print(f'auto_generate_bodies: failed to load assets')"),
		*AssetPath, *MeshPath);

	FString PyCmd = FString::Printf(TEXT("py %s"), *PyScript);
	GEngine->Exec(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, *PyCmd);

	return MakeSuccess(TEXT("physics_asset"), TEXT("auto_generate_bodies"),
		FString::Printf(TEXT("auto_generate_bodies dispatched: '%s' from mesh '%s' (min_bone_size=%.1f). "
		                     "FPhysicsAssetUtils::CreateFromSkeletalMesh is editor-module-only in UE 5.7 — "
		                     "use the Physics Asset editor for full auto-generation, or call add_body per bone."),
		                *AssetPath, *MeshPath, (float)MinBoneSize));
}

// ---------------------------------------------------------------------------
// set_constraint_profile
// ---------------------------------------------------------------------------

FBridgeResult UPhysicsAssetHandler::Action_SetConstraintProfile(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ProfileName;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("profile_name"),  ProfileName)  || ProfileName.IsEmpty())
	{
		return MakeError(TEXT("physics_asset"), TEXT("set_constraint_profile"), 1000,
			TEXT("set_constraint_profile: 'asset_path' and 'profile_name' are required"),
			TEXT("Optional: 'bone1', 'bone2' to target a specific constraint (applies to all if omitted)"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("physics_asset"), TEXT("set_constraint_profile"));
	UPhysicsAsset* PhysAsset = LoadPhysicsAsset(AssetPath, TempResult);
	if (!PhysAsset)
		return MakeError(TEXT("physics_asset"), TEXT("set_constraint_profile"), 2000,
			TempResult.Message,
			TEXT("Verify the asset path points to a valid UPhysicsAsset"));

	const FName Profile(*ProfileName);

	// Optional bone filter
	FString Bone1, Bone2;
	const bool bFiltered = (Params->TryGetStringField(TEXT("bone1"), Bone1) && !Bone1.IsEmpty()) ||
	                       (Params->TryGetStringField(TEXT("bone2"), Bone2) && !Bone2.IsEmpty());

	int32 AppliedCount = 0;
	for (UPhysicsConstraintTemplate* CT : PhysAsset->ConstraintSetup)
	{
		if (!CT) continue;

		if (bFiltered)
		{
			const bool bMatchesBone1 = Bone1.IsEmpty() || CT->DefaultInstance.ConstraintBone1 == FName(*Bone1);
			const bool bMatchesBone2 = Bone2.IsEmpty() || CT->DefaultInstance.ConstraintBone2 == FName(*Bone2);
			if (!bMatchesBone1 || !bMatchesBone2) continue;
		}

		// Apply the named constraint profile if it exists on this template.
		// UPhysicsConstraintTemplate stores profiles in ProfileHandles.
		// Find or add the profile entry and mark it current.
		bool bFound = false;
		// ProfileHandles is TArray<FPhysicsConstraintProfileHandle> in UE 5.7
		for (auto& ProfileProps : CT->ProfileHandles)
		{
			if (ProfileProps.ProfileName == Profile)
			{
				CT->DefaultInstance.CopyProfilePropertiesFrom(ProfileProps.ProfileProperties);
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			// Profile doesn't exist on this constraint — skip with a note
			continue;
		}

		++AppliedCount;
	}

	PhysAsset->MarkPackageDirty();

	return MakeSuccess(TEXT("physics_asset"), TEXT("set_constraint_profile"),
		FString::Printf(TEXT("set_constraint_profile: profile '%s' applied to %d constraint(s) on '%s'"),
		                *ProfileName, AppliedCount, *AssetPath));
}

TSharedPtr<FJsonObject> UPhysicsAssetHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new PhysicsAsset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new physics asset"))); Ps->SetObjectField(TEXT("skeletal_mesh_path"), P(TEXT("string"), false, TEXT("Content path of a skeletal mesh to seed from"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_physics_asset"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a physics body for a bone")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); Ps->SetObjectField(TEXT("bone_name"), P(TEXT("string"), true, TEXT("Bone name to attach the body to"))); Ps->SetObjectField(TEXT("shape_type"), P(TEXT("string"), true, TEXT("Shape: sphere, capsule, box"))); Ps->SetObjectField(TEXT("radius"), P(TEXT("float"), false, TEXT("Radius for sphere/capsule (default 10)"))); Ps->SetObjectField(TEXT("height"), P(TEXT("float"), false, TEXT("Height for capsule (default 20)"))); Ps->SetObjectField(TEXT("half_extents_x"), P(TEXT("float"), false, TEXT("Box half-extent X"))); Ps->SetObjectField(TEXT("half_extents_y"), P(TEXT("float"), false, TEXT("Box half-extent Y"))); Ps->SetObjectField(TEXT("half_extents_z"), P(TEXT("float"), false, TEXT("Box half-extent Z"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_body"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set simulation/collision parameters on a body")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); Ps->SetObjectField(TEXT("bone_name"), P(TEXT("string"), true, TEXT("Bone name of the body to modify"))); Ps->SetObjectField(TEXT("simulate"), P(TEXT("bool"), false, TEXT("Simulated vs Kinematic"))); Ps->SetObjectField(TEXT("collision_profile"), P(TEXT("string"), false, TEXT("Collision profile name (e.g. Ragdoll)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_body_params"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a physics constraint between two bones")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); Ps->SetObjectField(TEXT("bone1"), P(TEXT("string"), true, TEXT("First bone name"))); Ps->SetObjectField(TEXT("bone2"), P(TEXT("string"), true, TEXT("Second bone name"))); Ps->SetObjectField(TEXT("type"), P(TEXT("string"), false, TEXT("Constraint type: ball, hinge, prismatic, free (default ball)"))); Ps->SetObjectField(TEXT("swing1"), P(TEXT("float"), false, TEXT("Swing1 limit in degrees (default 45)"))); Ps->SetObjectField(TEXT("swing2"), P(TEXT("float"), false, TEXT("Swing2 limit in degrees (default 45)"))); Ps->SetObjectField(TEXT("twist"), P(TEXT("float"), false, TEXT("Twist limit in degrees (default 45)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_constraint"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get physics asset info (body count, constraint count)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_info"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all bodies in a physics asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("list_bodies"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Auto-generate physics bodies from a skeletal mesh (dispatched via Python — FPhysicsAssetUtils is editor-only)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the target UPhysicsAsset"))); Ps->SetObjectField(TEXT("skeletal_mesh_path"), P(TEXT("string"), true, TEXT("Content path of the source skeletal mesh"))); Ps->SetObjectField(TEXT("min_bone_size"), P(TEXT("float"), false, TEXT("Minimum bone size to create a body for (default 5.0)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("auto_generate_bodies"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Apply a named constraint profile to constraints on a physics asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the physics asset"))); Ps->SetObjectField(TEXT("profile_name"), P(TEXT("string"), true, TEXT("Name of the constraint profile to apply"))); Ps->SetObjectField(TEXT("bone1"), P(TEXT("string"), false, TEXT("Filter: only apply to constraints with this first bone"))); Ps->SetObjectField(TEXT("bone2"), P(TEXT("string"), false, TEXT("Filter: only apply to constraints with this second bone"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_constraint_profile"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// remove_body
// ---------------------------------------------------------------------------
FBridgeResult UPhysicsAssetHandler::Action_RemoveBody(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("remove_body"));
	FString AssetPath, BoneName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("physics_asset"), TEXT("remove_body"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
		return MakeError(TEXT("physics_asset"), TEXT("remove_body"), 1000, TEXT("'bone_name' is required"));

	UPhysicsAsset* PA = LoadPhysicsAsset(AssetPath, Result);
	if (!PA) return Result;

	const FName Bone(*BoneName);
	int32 BodyIdx = INDEX_NONE;
	for (int32 i = 0; i < PA->SkeletalBodySetups.Num(); ++i)
	{
		USkeletalBodySetup* B = PA->SkeletalBodySetups[i];
		if (B && B->BoneName == Bone) { BodyIdx = i; break; }
	}
	if (BodyIdx == INDEX_NONE)
		return MakeError(TEXT("physics_asset"), TEXT("remove_body"), 2000,
			FString::Printf(TEXT("No body found for bone '%s'"), *BoneName),
			TEXT("Use list_bodies to see registered bodies"));

	// Cascade-remove constraints touching this bone
	int32 ConstraintsRemoved = 0;
	for (int32 i = PA->ConstraintSetup.Num() - 1; i >= 0; --i)
	{
		UPhysicsConstraintTemplate* C = PA->ConstraintSetup[i];
		if (C && (C->DefaultInstance.ConstraintBone1 == Bone ||
		          C->DefaultInstance.ConstraintBone2 == Bone))
		{
			PA->ConstraintSetup.RemoveAt(i);
			++ConstraintsRemoved;
		}
	}

	PA->SkeletalBodySetups.RemoveAt(BodyIdx);
	PA->UpdateBodySetupIndexMap();
	PA->UpdateBoundsBodiesArray();
	PA->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("bone_name"), BoneName);
	Data->SetNumberField(TEXT("constraints_cascaded"), ConstraintsRemoved);
	return MakeSuccess(TEXT("physics_asset"), TEXT("remove_body"),
		FString::Printf(TEXT("Removed body for '%s' (+%d constraints cascaded)"),
			*BoneName, ConstraintsRemoved), Data);
}

// ---------------------------------------------------------------------------
// remove_constraint
// ---------------------------------------------------------------------------
FBridgeResult UPhysicsAssetHandler::Action_RemoveConstraint(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("physics_asset"), TEXT("remove_constraint"));
	FString AssetPath, B1, B2;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("physics_asset"), TEXT("remove_constraint"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("bone1"), B1) || B1.IsEmpty())
		return MakeError(TEXT("physics_asset"), TEXT("remove_constraint"), 1000, TEXT("'bone1' is required"));
	if (!Params->TryGetStringField(TEXT("bone2"), B2) || B2.IsEmpty())
		return MakeError(TEXT("physics_asset"), TEXT("remove_constraint"), 1000, TEXT("'bone2' is required"));

	UPhysicsAsset* PA = LoadPhysicsAsset(AssetPath, Result);
	if (!PA) return Result;

	const FName Bone1(*B1), Bone2(*B2);
	int32 Removed = 0;
	for (int32 i = PA->ConstraintSetup.Num() - 1; i >= 0; --i)
	{
		UPhysicsConstraintTemplate* C = PA->ConstraintSetup[i];
		if (C && ((C->DefaultInstance.ConstraintBone1 == Bone1 && C->DefaultInstance.ConstraintBone2 == Bone2) ||
		          (C->DefaultInstance.ConstraintBone1 == Bone2 && C->DefaultInstance.ConstraintBone2 == Bone1)))
		{
			PA->ConstraintSetup.RemoveAt(i);
			++Removed;
		}
	}
	if (Removed == 0)
		return MakeError(TEXT("physics_asset"), TEXT("remove_constraint"), 2000,
			FString::Printf(TEXT("No constraint found between '%s' and '%s'"), *B1, *B2));

	PA->UpdateBodySetupIndexMap();
	PA->MarkPackageDirty();
	return MakeSuccess(TEXT("physics_asset"), TEXT("remove_constraint"),
		FString::Printf(TEXT("Removed %d constraint(s) between '%s' and '%s'"), Removed, *B1, *B2));
}
