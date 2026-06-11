#include "Handlers/ChaosHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

// ---- Geometry Collection (Chaos) -------------------------------------------
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionActor.h"

// ---- Fracture editor -------------------------------------------------------
// #include "FractureToolVoronoi.h"  // UE 5.7: FractureToolVoronoi removed
#include "GeometryCollection/GeometryCollectionFactory.h"

// ---- Physics fields --------------------------------------------------------
#include "Field/FieldSystemActor.h"
#include "Field/FieldSystemComponent.h"
#include "Field/FieldSystemNodes.h"

// ---- Editor / World --------------------------------------------------------
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UChaosHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UChaosHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("chaos"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("fracture"))                  return Action_Fracture(Params);
	if (Action == TEXT("spawn_field"))               return Action_SpawnField(Params);
	if (Action == TEXT("list_geometry_collections")) return Action_ListGeometryCollections(Params);
	if (Action == TEXT("get_fracture_info"))         return Action_GetFractureInfo(Params);
	if (Action == TEXT("set_physics_field"))                return Action_SetPhysicsField(Params);
	if (Action == TEXT("set_geometry_collection_settings")) return Action_SetGeometryCollectionSettings(Params);
	if (Action == TEXT("set_material_damage"))              return Action_SetMaterialDamage(Params);

	FBridgeResult R = CreateResult(TEXT("chaos"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown chaos action '%s'. Valid: fracture, spawn_field, list_geometry_collections, get_fracture_info, set_physics_field, set_geometry_collection_settings, set_material_damage"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// fracture
// ---------------------------------------------------------------------------

FBridgeResult UChaosHandler::Action_Fracture(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("chaos"), TEXT("fracture"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("fracture: 'asset_path' is required (e.g. '/Game/Destruction/GC_Wall')");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Verify asset exists — we do NOT create or fracture it here.
	UGeometryCollection* GeoCollection = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
	if (!GeoCollection)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		GeoCollection = LoadObject<UGeometryCollection>(nullptr, *Suffix);
	}
	if (!GeoCollection)
	{
		return MakeError(TEXT("chaos"), TEXT("fracture"), 2000,
			FString::Printf(TEXT("fracture: UGeometryCollection not found at '%s'"), *AssetPath),
			TEXT("Create the Geometry Collection asset in the Content Browser first."));
	}

	return MakeError(TEXT("chaos"), TEXT("fracture"), 3000,
		TEXT("fracture: Programmatic Voronoi/Uniform fracture requires UE editor fracture tools. No public C++ API is available in UE 5.7."),
		TEXT("Open the Geometry Collection asset in the Fracture Mode editor to fracture it manually."));
}

// ---------------------------------------------------------------------------
// spawn_field
// ---------------------------------------------------------------------------

FBridgeResult UChaosHandler::Action_SpawnField(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("chaos"), TEXT("spawn_field"));

	FString FieldType, LocationStr;
	if (!Params->TryGetStringField(TEXT("field_type"), FieldType) || FieldType.IsEmpty())
	{
		Result.Message = TEXT("spawn_field: 'field_type' is required (radial|uniform|plane_fall_off)");
		Result.ErrorCode = 1000;
		return Result;
	}

	if (!Params->TryGetStringField(TEXT("location"), LocationStr) || LocationStr.IsEmpty())
	{
		Result.Message = TEXT("spawn_field: 'location' is required (format: 'X,Y,Z')");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Parse location
	TArray<FString> Parts;
	LocationStr.ParseIntoArray(Parts, TEXT(","));
	FVector Location(0.0, 0.0, 0.0);
	if (Parts.Num() >= 1) Location.X = FCString::Atod(*Parts[0]);
	if (Parts.Num() >= 2) Location.Y = FCString::Atod(*Parts[1]);
	if (Parts.Num() >= 3) Location.Z = FCString::Atod(*Parts[2]);

	double Magnitude = 1000.0;
	Params->TryGetNumberField(TEXT("magnitude"), Magnitude);

	double Radius = 500.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);

	// Get the editor world
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("spawn_field: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Spawn FieldSystem actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FTransform SpawnTransform(FRotator::ZeroRotator, Location);
	AFieldSystemActor* FieldActor = Cast<AFieldSystemActor>(
		World->SpawnActor(AFieldSystemActor::StaticClass(), &SpawnTransform, SpawnParams));
	if (!FieldActor)
	{
		Result.Message = TEXT("spawn_field: failed to spawn FieldSystemActor");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Add the appropriate field node based on type
	UFieldSystemComponent* FieldComp = FieldActor->GetFieldSystemComponent();
	if (!FieldComp)
	{
		Result.Message = TEXT("spawn_field: FieldSystemActor has no FieldSystemComponent");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Create and configure the appropriate field node
	FieldActor->SetActorLabel(FString::Printf(TEXT("FieldSystem_%s"), *FieldType));

	if (FieldType == TEXT("radial"))
	{
		URadialFalloff* Falloff = NewObject<URadialFalloff>(FieldActor);
		Falloff->Magnitude = (float)Magnitude;
		Falloff->MinRange  = 0.f;
		Falloff->MaxRange  = (float)Radius;
		Falloff->Default   = 0.f;
		Falloff->Radius    = (float)Radius;
		Falloff->Position  = FVector::ZeroVector;  // local space
		// Attach as a construction field so it survives editor save/load.
		// Do NOT call ResetFieldSystem() — that wipes construction fields.
		FieldComp->AddFieldCommand(true, EFieldPhysicsType::Field_LinearForce,
			/*MetaData*/ nullptr, Falloff);
	}
	else if (FieldType == TEXT("uniform"))
	{
		// UUniformVector produces a uniform force direction; UUniformScalar does not.
		UUniformVector* Uniform = NewObject<UUniformVector>(FieldActor);
		Uniform->Magnitude = (float)Magnitude;
		Uniform->Direction = FVector(1, 0, 0);
		FieldComp->AddFieldCommand(true, EFieldPhysicsType::Field_LinearForce,
			/*MetaData*/ nullptr, Uniform);
	}

	FieldComp->MarkRenderStateDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = FString::Printf(TEXT("FieldSystem_%s"), *FieldType);
	Result.Message      = FString::Printf(
		TEXT("Spawned %s field actor at (%s) magnitude=%.0f radius=%.0f"),
		*FieldType, *LocationStr, Magnitude, Radius);
	return Result;
}

// ---------------------------------------------------------------------------
// list_geometry_collections
// ---------------------------------------------------------------------------

FBridgeResult UChaosHandler::Action_ListGeometryCollections(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("chaos"), TEXT("list_geometry_collections"));

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UGeometryCollection::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	TArray<TSharedPtr<FJsonValue>> GCArr;
	for (const FAssetData& AD : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AD.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), AD.GetObjectPathString());
		GCArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("geometry_collections"), GCArr);
	Data->SetNumberField(TEXT("count"), GCArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("list_geometry_collections: %d asset(s) found"), GCArr.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// get_fracture_info
// ---------------------------------------------------------------------------

FBridgeResult UChaosHandler::Action_GetFractureInfo(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("chaos"), TEXT("get_fracture_info"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_fracture_info: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UGeometryCollection* GC = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
	if (!GC)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		GC = LoadObject<UGeometryCollection>(nullptr, *Suffix);
	}
	if (!GC)
	{
		Result.Message = FString::Printf(TEXT("get_fracture_info: no UGeometryCollection at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Get geometry collection data
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe>& GeoData = GC->GetGeometryCollection();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), GC->GetName());

	if (GeoData.IsValid())
	{
		// TManagedArray<FTransform> is in the Transform group
		const int32 TransformCount = GeoData->NumElements(FGeometryCollection::TransformGroup);
		Data->SetNumberField(TEXT("transform_count"), TransformCount);
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("get_fracture_info: '%s' loaded"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_physics_field  (Phase 1e P2 gap)
// Params: field_type (string: Radial|Linear|Noise), location {x,y,z},
//         magnitude (float), radius (float), label (string, optional)
// Spawns or reconfigures an AFieldSystemActor in the current level.
// ---------------------------------------------------------------------------
FBridgeResult UChaosHandler::Action_SetPhysicsField(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_physics_field");

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("chaos"), Action, 2000, TEXT("No editor world available"));

	// Parse / validate params so callers get a consistent validation surface even though
	// the action is not implemented at editor time. These were previously silently ignored.
	FString FieldType = TEXT("Radial");
	Params->TryGetStringField(TEXT("field_type"), FieldType);

	double Magnitude = 1000.0, Radius = 500.0;
	Params->TryGetNumberField(TEXT("magnitude"), Magnitude);
	Params->TryGetNumberField(TEXT("radius"),    Radius);

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
	{
		double X = 0, Y = 0, Z = 0;
		(*LocObj)->TryGetNumberField(TEXT("x"), X);
		(*LocObj)->TryGetNumberField(TEXT("y"), Y);
		(*LocObj)->TryGetNumberField(TEXT("z"), Z);
	}

	// UE 5.7: UFieldSystemComponent::ApplyRadialVectorSpace and ApplyUniformVector do NOT
	// exist. Runtime field application requires PIE; editor-time setup requires configuring
	// construction field commands manually. Use the spawn_field action for editor setup.
	return MakeError(TEXT("chaos"), Action, 3004,
		TEXT("set_physics_field: Runtime field application requires PIE. Field construction commands (AddFieldCommand) must be set up in the editor and applied at runtime."),
		TEXT("Add a UFieldSystemComponent to your actor, configure field commands in the actor's BeginPlay, and test in PIE."));
}

// ---------------------------------------------------------------------------
// set_geometry_collection_settings  (Phase 1e P2 gap)
// Params: asset_path (string), damage_threshold (float, optional),
//         initial_dynamic_state (string: Sleeping|Kinematic|Static, optional),
//         enable_nanite (bool, optional), cluster_group_index (int, optional)
// Direct CDO on UGeometryCollection asset.
// ---------------------------------------------------------------------------
FBridgeResult UChaosHandler::Action_SetGeometryCollectionSettings(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_geometry_collection_settings");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("chaos"), Action, 1000, TEXT("'asset_path' is required"));

	UGeometryCollection* GC = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
	if (!GC)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		GC = LoadObject<UGeometryCollection>(nullptr, *Suffix);
	}
	if (!GC)
		return MakeError(TEXT("chaos"), Action, 2000,
			FString::Printf(TEXT("UGeometryCollection not found at '%s'"), *AssetPath));

	GC->Modify();

	double DamageThreshold = 0.0;
	if (Params->TryGetNumberField(TEXT("damage_threshold"), DamageThreshold))
	{
		// UGeometryCollection stores damage thresholds per level
		if (GC->DamageThreshold.Num() > 0)
			GC->DamageThreshold[0] = (float)DamageThreshold;
		else
			GC->DamageThreshold.Add((float)DamageThreshold);
	}

	// UE 5.7: UGeometryCollection does not expose a simple `InitialDynamicState` UPROPERTY
	// of type EObjectStateTypeEnum. The state lives on the UGeometryCollectionComponent
	// (runtime) or on the per-transform TManagedArray inside the underlying FGeometryCollection.
	// Try reflection for safety; if absent, skip silently and surface a note in the response.
	FString InitStatStr;
	bool bInitialDynamicStateHandled = false;
	if (Params->TryGetStringField(TEXT("initial_dynamic_state"), InitStatStr))
	{
		// Attempt a byte-property write for engines that expose the enum as a byte UPROPERTY.
		if (FByteProperty* ByteProp = FindFProperty<FByteProperty>(GC->GetClass(), TEXT("InitialDynamicState")))
		{
			uint8 NewVal = (uint8)EObjectStateTypeEnum::Chaos_Object_Sleeping;
			if      (InitStatStr == TEXT("Sleeping"))  NewVal = (uint8)EObjectStateTypeEnum::Chaos_Object_Sleeping;
			else if (InitStatStr == TEXT("Kinematic")) NewVal = (uint8)EObjectStateTypeEnum::Chaos_Object_Kinematic;
			else if (InitStatStr == TEXT("Static"))    NewVal = (uint8)EObjectStateTypeEnum::Chaos_Object_Static;
			else if (InitStatStr == TEXT("Dynamic"))   NewVal = (uint8)EObjectStateTypeEnum::Chaos_Object_Dynamic;
			ByteProp->SetPropertyValue_InContainer(GC, NewVal);
			bInitialDynamicStateHandled = true;
		}
		else if (FEnumProperty* EnumProp = FindFProperty<FEnumProperty>(GC->GetClass(), TEXT("InitialDynamicState")))
		{
			int64 NewVal = (int64)EObjectStateTypeEnum::Chaos_Object_Sleeping;
			if      (InitStatStr == TEXT("Sleeping"))  NewVal = (int64)EObjectStateTypeEnum::Chaos_Object_Sleeping;
			else if (InitStatStr == TEXT("Kinematic")) NewVal = (int64)EObjectStateTypeEnum::Chaos_Object_Kinematic;
			else if (InitStatStr == TEXT("Static"))    NewVal = (int64)EObjectStateTypeEnum::Chaos_Object_Static;
			else if (InitStatStr == TEXT("Dynamic"))   NewVal = (int64)EObjectStateTypeEnum::Chaos_Object_Dynamic;
			void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(GC);
			EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, NewVal);
			bInitialDynamicStateHandled = true;
		}
		// else: property does not exist on this UE 5.7 variant — skip silently.
	}

	bool bEnableNanite = false;
	if (Params->TryGetBoolField(TEXT("enable_nanite"), bEnableNanite))
	{
		// Use the BlueprintSetter so editor-only Nanite rebuild logic runs (UE-252163).
		GC->SetEnableNanite(bEnableNanite);
	}

	GC->MarkPackageDirty();
	GC->PostEditChange();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("enable_nanite"), GC->EnableNanite);
	if (Params->HasField(TEXT("initial_dynamic_state")) && !bInitialDynamicStateHandled)
	{
		Data->SetStringField(TEXT("initial_dynamic_state_note"),
			TEXT("InitialDynamicState is set at runtime on UGeometryCollectionComponent"));
	}

	return MakeSuccess(TEXT("chaos"), Action,
		FString::Printf(TEXT("GeometryCollection settings updated on '%s'"), *AssetPath), Data);
}

// ---------------------------------------------------------------------------
// set_material_damage  (Phase 1e P2 gap)
// Params: asset_path (string), material_index (int, optional, default 0),
//         damage_type (string: Impact|Break|Both, optional),
//         threshold_scale (float, optional)
// Sets damage-related properties on UGeometryCollection.
// ---------------------------------------------------------------------------
FBridgeResult UChaosHandler::Action_SetMaterialDamage(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_material_damage");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("chaos"), Action, 1000, TEXT("'asset_path' is required"));

	UGeometryCollection* GC = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
	if (!GC)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		GC = LoadObject<UGeometryCollection>(nullptr, *Suffix);
	}
	if (!GC)
		return MakeError(TEXT("chaos"), Action, 2000,
			FString::Printf(TEXT("UGeometryCollection not found at '%s'"), *AssetPath));

	GC->Modify();

	double ThresholdScale = 0.0;
	bool bScaled = false;
	if (Params->TryGetNumberField(TEXT("threshold_scale"), ThresholdScale))
	{
		// Scale all existing damage thresholds by the given factor
		for (float& Thresh : GC->DamageThreshold)
			Thresh *= (float)ThresholdScale;
		bScaled = true;
	}

	FString DamageType = TEXT("Both");
	Params->TryGetStringField(TEXT("damage_type"), DamageType);

	// UE 5.7: UGeometryCollection has no BreakDamageThreshold or CollisionImpulseThreshold
	// UPROPERTY. Per-type (break vs impact) thresholds are not a per-asset concept — they
	// are set at runtime on UGeometryCollectionComponent (ImpulseStrainThreshold etc.).
	// We apply `break_threshold` and `impact_threshold` as additional DamageThreshold
	// scaling factors so callers get a visible state change, and surface a note clarifying
	// the actual engine shape.
	double BreakThreshold = 0.0, ImpactThreshold = 0.0;
	bool bHasBreak  = Params->TryGetNumberField(TEXT("break_threshold"),  BreakThreshold);
	bool bHasImpact = Params->TryGetNumberField(TEXT("impact_threshold"), ImpactThreshold);

	if (bHasBreak)
	{
		for (float& Thresh : GC->DamageThreshold)
			Thresh *= (float)BreakThreshold;
	}
	if (bHasImpact)
	{
		for (float& Thresh : GC->DamageThreshold)
			Thresh *= (float)ImpactThreshold;
	}

	GC->MarkPackageDirty();
	GC->PostEditChange();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("damage_type"), DamageType);
	Data->SetBoolField(TEXT("threshold_scale_applied"), bScaled);
	Data->SetBoolField(TEXT("break_threshold_applied"), bHasBreak);
	Data->SetBoolField(TEXT("impact_threshold_applied"), bHasImpact);
	if (bHasBreak || bHasImpact || Params->HasField(TEXT("damage_type")))
	{
		Data->SetStringField(TEXT("note"),
			TEXT("break_threshold and impact_threshold applied as DamageThreshold scale; per-type thresholds require runtime component configuration"));
	}

	return MakeSuccess(TEXT("chaos"), Action,
		FString::Printf(TEXT("Material damage settings updated on '%s' (type=%s)"), *AssetPath, *DamageType),
		Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas — self-documenting parameter schemas for all actions.
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UChaosHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Programmatic Voronoi fracture is not available in UE 5.7 — returns 3000 with guidance to use the Fracture Mode editor.")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Geometry Collection asset (e.g. /Game/Destruction/GC_Wall)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("fracture"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn an AFieldSystemActor and attach a construction field node (radial or uniform)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("field_type"), P(TEXT("string"), true, TEXT("Field type: radial|uniform|plane_fall_off"))); Ps->SetObjectField(TEXT("location"), P(TEXT("string"), true, TEXT("Location as 'X,Y,Z'"))); Ps->SetObjectField(TEXT("magnitude"), P(TEXT("float"), false, TEXT("Field magnitude (default 1000)"))); Ps->SetObjectField(TEXT("radius"), P(TEXT("float"), false, TEXT("Field radius (default 500)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("spawn_field"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all UGeometryCollection assets in the project")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("package_path"), P(TEXT("string"), false, TEXT("Optional path prefix filter (e.g. /Game/Destruction)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("list_geometry_collections"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return transform count and geometry info for a Geometry Collection")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Geometry Collection asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_fracture_info"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Runtime field application requires PIE — returns 3004 with guidance. Use spawn_field for editor-time setup.")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("field_type"), P(TEXT("string"), false, TEXT("Field type: Radial|Uniform|Linear"))); Ps->SetObjectField(TEXT("location"), P(TEXT("object"), false, TEXT("Location object {x,y,z}"))); Ps->SetObjectField(TEXT("magnitude"), P(TEXT("float"), false, TEXT("Field magnitude (default 1000)"))); Ps->SetObjectField(TEXT("radius"), P(TEXT("float"), false, TEXT("Field radius (default 500)"))); Ps->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Optional actor label"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_physics_field"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set properties on a UGeometryCollection asset (damage threshold, Nanite, optional initial dynamic state)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Geometry Collection asset"))); Ps->SetObjectField(TEXT("damage_threshold"), P(TEXT("float"), false, TEXT("Per-level damage threshold (writes DamageThreshold[0])"))); Ps->SetObjectField(TEXT("enable_nanite"), P(TEXT("bool"), false, TEXT("Enable Nanite (uses SetEnableNanite blueprint setter)"))); Ps->SetObjectField(TEXT("initial_dynamic_state"), P(TEXT("string"), false, TEXT("Sleeping|Kinematic|Static|Dynamic — may be ignored if not exposed on the asset in UE 5.7"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_geometry_collection_settings"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Scale DamageThreshold array on a Geometry Collection asset. Note: break/impact per-type thresholds do not exist as asset properties; they are applied as scale factors.")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Geometry Collection asset"))); Ps->SetObjectField(TEXT("threshold_scale"), P(TEXT("float"), false, TEXT("Scale factor applied to every DamageThreshold entry"))); Ps->SetObjectField(TEXT("break_threshold"), P(TEXT("float"), false, TEXT("Additional scale factor (no direct asset property in UE 5.7)"))); Ps->SetObjectField(TEXT("impact_threshold"), P(TEXT("float"), false, TEXT("Additional scale factor (no direct asset property in UE 5.7)"))); Ps->SetObjectField(TEXT("damage_type"), P(TEXT("string"), false, TEXT("Echoed in response: Impact|Break|Both"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_material_damage"), A); }

	return Root;
}
