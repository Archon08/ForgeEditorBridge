#include "Handlers/SceneQueryHandler.h"

#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMeshActor.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("scene_query");

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace
{

static UWorld* GetEditorWorldLocal()
{
#if WITH_EDITOR
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Editor && Ctx.World())
				return Ctx.World();
		}
	}
#endif
	return nullptr;
}

static TSharedPtr<FJsonObject> MakeVecObj(const FVector& V)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("x"), V.X);
	O->SetNumberField(TEXT("y"), V.Y);
	O->SetNumberField(TEXT("z"), V.Z);
	return O;
}

static TSharedPtr<FJsonObject> MakeRotObj(const FRotator& R)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("pitch"), R.Pitch);
	O->SetNumberField(TEXT("yaw"),   R.Yaw);
	O->SetNumberField(TEXT("roll"),  R.Roll);
	return O;
}

static bool ReadVec3(TSharedPtr<FJsonObject> Params, const FString& Key, FVector& Out)
{
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Params->TryGetObjectField(Key, Sub)) return false;
	double X = 0, Y = 0, Z = 0;
	(*Sub)->TryGetNumberField(TEXT("x"), X);
	(*Sub)->TryGetNumberField(TEXT("y"), Y);
	(*Sub)->TryGetNumberField(TEXT("z"), Z);
	Out = FVector(X, Y, Z);
	return true;
}

/** Resolve a class name string to UClass* (AActor subclasses only). */
static UClass* ResolveActorClass(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;

	// Full asset path → Blueprint _C
	if (ClassName.StartsWith(TEXT("/")))
	{
		FString Path = ClassName;
		if (!Path.EndsWith(TEXT("_C"))) Path += TEXT("_C");
		if (UClass* C = LoadClass<AActor>(nullptr, *Path)) return C;
	}

	// Native scripts — common prefixes
	static const TCHAR* Prefixes[] = {
		TEXT("/Script/Engine."),
		TEXT("/Script/GameFramework."),
		TEXT("/Script/UMG."),
		TEXT("/Script/AIModule."),
		TEXT("/Script/Landscape."),
	};
	for (const TCHAR* P : Prefixes)
	{
		FString FP = FString(P) + ClassName;
		if (UClass* C = FindObject<UClass>(nullptr, *FP)) return C;
	}

	// Last resort: scan all loaded AActor subclasses
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((*It)->IsChildOf(AActor::StaticClass()) &&
			(*It)->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
			return *It;
	}
	return nullptr;
}

/** Find an actor in World by label (case-insensitive). */
static AActor* FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World || Label.IsEmpty()) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
			return *It;
	}
	return nullptr;
}

/** Parse channel name → ECollisionChannel. Defaults to Visibility. */
static ECollisionChannel ParseChannel(const FString& Name)
{
	if (Name.Equals(TEXT("Camera"),       ESearchCase::IgnoreCase)) return ECC_Camera;
	if (Name.Equals(TEXT("WorldStatic"),  ESearchCase::IgnoreCase)) return ECC_WorldStatic;
	if (Name.Equals(TEXT("WorldDynamic"), ESearchCase::IgnoreCase)) return ECC_WorldDynamic;
	if (Name.Equals(TEXT("Pawn"),         ESearchCase::IgnoreCase)) return ECC_Pawn;
	if (Name.Equals(TEXT("PhysicsBody"),  ESearchCase::IgnoreCase)) return ECC_PhysicsBody;
	return ECC_Visibility;
}

/** Recursively build a scene component node with children. */
static TSharedPtr<FJsonObject> BuildSceneCompNode(USceneComponent* Comp)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("name"),  Comp->GetName());
	Node->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
	Node->SetObjectField(TEXT("relative_location"), MakeVecObj(Comp->GetRelativeLocation()));
	Node->SetObjectField(TEXT("relative_rotation"), MakeRotObj(Comp->GetRelativeRotation()));
	Node->SetObjectField(TEXT("relative_scale"),    MakeVecObj(Comp->GetRelativeScale3D()));

	TArray<TSharedPtr<FJsonValue>> Children;
	for (USceneComponent* Child : Comp->GetAttachChildren())
	{
		if (Child)
			Children.Add(MakeShared<FJsonValueObject>(BuildSceneCompNode(Child)));
	}
	if (Children.Num() > 0)
		Node->SetArrayField(TEXT("children"), Children);

	return Node;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// GetEditorWorld
// ---------------------------------------------------------------------------

UWorld* USceneQueryHandler::GetEditorWorld()
{
	return GetEditorWorldLocal();
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("find_actors"))         return Action_FindActors(Params);
	if (Action == TEXT("get_actor_details"))   return Action_GetActorDetails(Params);
	if (Action == TEXT("get_component_tree"))  return Action_GetComponentTree(Params);
	if (Action == TEXT("get_scene_bounds"))    return Action_GetSceneBounds(Params);
	if (Action == TEXT("get_actors_in_radius"))return Action_GetActorsInRadius(Params);
	if (Action == TEXT("raycast"))             return Action_Raycast(Params);
	if (Action == TEXT("overlap_query"))       return Action_OverlapQuery(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown scene_query action '%s'. Valid: find_actors, get_actor_details, "
			"get_component_tree, get_scene_bounds, get_actors_in_radius, raycast, overlap_query"), *Action));
}

// ---------------------------------------------------------------------------
// find_actors
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_FindActors(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("find_actors"), 3000, TEXT("No editor world available"));

	FString ClassFilter, NameFilter, TagFilter;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
	Params->TryGetStringField(TEXT("name_filter"),  NameFilter);
	Params->TryGetStringField(TEXT("tag_filter"),   TagFilter);

	UClass* FilterClass = ResolveActorClass(ClassFilter);

	TArray<TSharedPtr<FJsonValue>> Results;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		if (FilterClass && !Actor->IsA(FilterClass)) continue;
		if (!NameFilter.IsEmpty() &&
			!Actor->GetActorLabel().Contains(NameFilter, ESearchCase::IgnoreCase)) continue;
		if (!TagFilter.IsEmpty() && !Actor->ActorHasTag(*TagFilter)) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"),    Actor->GetActorLabel());
		Entry->SetStringField(TEXT("class"),    Actor->GetClass()->GetName());
		Entry->SetObjectField(TEXT("location"), MakeVecObj(Actor->GetActorLocation()));
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("actors"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());

	return MakeSuccess(DOMAIN, TEXT("find_actors"),
		FString::Printf(TEXT("Found %d actors"), Results.Num()), Data);
}

// ---------------------------------------------------------------------------
// get_actor_details
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_GetActorDetails(TSharedPtr<FJsonObject> Params)
{
	FString ActorLabel;
	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_actor_details"), 1000, TEXT("'actor_label' is required"));

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("get_actor_details"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("get_actor_details"), 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorLabel));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	// Transform
	TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
	Transform->SetObjectField(TEXT("location"), MakeVecObj(Actor->GetActorLocation()));
	Transform->SetObjectField(TEXT("rotation"), MakeRotObj(Actor->GetActorRotation()));
	Transform->SetObjectField(TEXT("scale"),    MakeVecObj(Actor->GetActorScale3D()));
	Data->SetObjectField(TEXT("transform"), Transform);

	// Visibility + shadow
	Data->SetBoolField(TEXT("hidden"),       Actor->IsHidden());
	// bCastShadow moved to UPrimitiveComponent in UE 5.7 — true if any primitive component casts shadows
	{
		bool bCastShadow = false;
		TInlineComponentArray<UPrimitiveComponent*> Prims;
		Actor->GetComponents(Prims);
		for (UPrimitiveComponent* Prim : Prims)
		{
			if (Prim && Prim->CastShadow)
			{
				bCastShadow = true;
				break;
			}
		}
		Data->SetBoolField(TEXT("cast_shadow"), bCastShadow);
	}

	// Actor tags
	TArray<TSharedPtr<FJsonValue>> Tags;
	for (const FName& Tag : Actor->Tags)
		Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	Data->SetArrayField(TEXT("tags"), Tags);

	// Components (flat list)
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	TArray<TSharedPtr<FJsonValue>> CompArray;
	for (UActorComponent* Comp : Components)
	{
		if (!Comp) continue;
		TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("name"),  Comp->GetName());
		CObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
		CObj->SetBoolField(TEXT("active"),  Comp->IsActive());
		CompArray.Add(MakeShared<FJsonValueObject>(CObj));
	}
	Data->SetArrayField(TEXT("components"), CompArray);

	return MakeSuccess(DOMAIN, TEXT("get_actor_details"),
		FString::Printf(TEXT("Details for '%s'"), *ActorLabel), Data);
}

// ---------------------------------------------------------------------------
// get_component_tree
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_GetComponentTree(TSharedPtr<FJsonObject> Params)
{
	FString ActorLabel;
	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_component_tree"), 1000, TEXT("'actor_label' is required"));

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("get_component_tree"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("get_component_tree"), 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorLabel));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());

	// Scene component hierarchy (recursive from RootComponent)
	if (USceneComponent* Root = Actor->GetRootComponent())
	{
		Data->SetObjectField(TEXT("scene_hierarchy"), BuildSceneCompNode(Root));
	}
	else
	{
		Data->SetBoolField(TEXT("scene_hierarchy"), false);
	}

	// Non-scene components (flat list — no spatial attachment)
	TArray<UActorComponent*> AllComps;
	Actor->GetComponents(AllComps);
	TArray<TSharedPtr<FJsonValue>> NonScene;
	for (UActorComponent* Comp : AllComps)
	{
		if (!Comp || Comp->IsA<USceneComponent>()) continue;
		TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("name"),  Comp->GetName());
		CObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
		NonScene.Add(MakeShared<FJsonValueObject>(CObj));
	}
	Data->SetArrayField(TEXT("non_scene_components"), NonScene);

	return MakeSuccess(DOMAIN, TEXT("get_component_tree"),
		FString::Printf(TEXT("Component tree for '%s'"), *ActorLabel), Data);
}

// ---------------------------------------------------------------------------
// get_scene_bounds
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_GetSceneBounds(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("get_scene_bounds"), 3000, TEXT("No editor world available"));

	FBox WorldBox(ForceInitToZero);
	int32 ActorCount = 0;
	int32 StaticMeshCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		++ActorCount;

		FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
		if (ActorBox.IsValid)
			WorldBox += ActorBox;

		if (Actor->IsA<AStaticMeshActor>())
			++StaticMeshCount;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("actor_count"),       ActorCount);
	Data->SetNumberField(TEXT("static_mesh_count"), StaticMeshCount);

	if (WorldBox.IsValid)
	{
		Data->SetObjectField(TEXT("min"),    MakeVecObj(WorldBox.Min));
		Data->SetObjectField(TEXT("max"),    MakeVecObj(WorldBox.Max));
		Data->SetObjectField(TEXT("center"), MakeVecObj(WorldBox.GetCenter()));
		Data->SetObjectField(TEXT("extent"), MakeVecObj(WorldBox.GetExtent()));
		Data->SetBoolField(TEXT("valid"), true);
	}
	else
	{
		Data->SetBoolField(TEXT("valid"), false);
	}

	return MakeSuccess(DOMAIN, TEXT("get_scene_bounds"),
		FString::Printf(TEXT("Scene: %d actors, %d static meshes"), ActorCount, StaticMeshCount), Data);
}

// ---------------------------------------------------------------------------
// get_actors_in_radius
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_GetActorsInRadius(TSharedPtr<FJsonObject> Params)
{
	FVector Center = FVector::ZeroVector;
	double Radius = 500.0;

	ReadVec3(Params, TEXT("center"), Center);
	Params->TryGetNumberField(TEXT("radius"), Radius);

	if (Radius <= 0.0)
		return MakeError(DOMAIN, TEXT("get_actors_in_radius"), 1000, TEXT("'radius' must be > 0"));

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("get_actors_in_radius"), 3000, TEXT("No editor world available"));

	// Collect (distance, actor) pairs
	TArray<TPair<float, AActor*>> Found;
	const float RadiusSq = (float)(Radius * Radius);

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		float DistSq = FVector::DistSquared(Center, Actor->GetActorLocation());
		if (DistSq <= RadiusSq)
			Found.Add({ FMath::Sqrt(DistSq), Actor });
	}

	// Sort nearest first
	Found.Sort([](const TPair<float, AActor*>& A, const TPair<float, AActor*>& B)
	{
		return A.Key < B.Key;
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const auto& Pair : Found)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"),    Pair.Value->GetActorLabel());
		Entry->SetStringField(TEXT("class"),    Pair.Value->GetClass()->GetName());
		Entry->SetNumberField(TEXT("distance"), Pair.Key);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("actors"), Results);
	Data->SetNumberField(TEXT("count"),  Results.Num());

	return MakeSuccess(DOMAIN, TEXT("get_actors_in_radius"),
		FString::Printf(TEXT("Found %d actors within %.0f cm"), Results.Num(), Radius), Data);
}

// ---------------------------------------------------------------------------
// raycast
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_Raycast(TSharedPtr<FJsonObject> Params)
{
	FVector Start = FVector::ZeroVector;
	FVector End   = FVector::ForwardVector * 1000.0f;

	ReadVec3(Params, TEXT("start"), Start);
	ReadVec3(Params, TEXT("end"),   End);

	FString ChannelStr = TEXT("Visibility");
	Params->TryGetStringField(TEXT("channel"), ChannelStr);

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("raycast"), 3000, TEXT("No editor world available"));

	FHitResult Hit;
	FCollisionQueryParams QueryParams(TEXT("SceneQueryRaycast"), /*bTraceComplex=*/true);
	const bool bHit = World->LineTraceSingleByChannel(
		Hit, Start, End, ParseChannel(ChannelStr), QueryParams);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("hit"), bHit);

	if (bHit)
	{
		Data->SetStringField(TEXT("actor_label"),
			Hit.GetActor() ? Hit.GetActor()->GetActorLabel() : TEXT("(none)"));
		Data->SetStringField(TEXT("component"),
			Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("(none)"));
		Data->SetObjectField(TEXT("location"), MakeVecObj(Hit.ImpactPoint));
		Data->SetObjectField(TEXT("normal"),   MakeVecObj(Hit.ImpactNormal));
		Data->SetNumberField(TEXT("distance"), Hit.Distance);
	}

	return MakeSuccess(DOMAIN, TEXT("raycast"),
		bHit
			? FString::Printf(TEXT("Hit '%s' at distance %.1f"),
				Hit.GetActor() ? *Hit.GetActor()->GetActorLabel() : TEXT("(none)"), Hit.Distance)
			: TEXT("No hit"),
		Data);
}

// ---------------------------------------------------------------------------
// overlap_query
// ---------------------------------------------------------------------------

FBridgeResult USceneQueryHandler::Action_OverlapQuery(TSharedPtr<FJsonObject> Params)
{
	FVector Center = FVector::ZeroVector;
	FVector Extent = FVector(100.0f);

	ReadVec3(Params, TEXT("center"), Center);
	ReadVec3(Params, TEXT("extent"), Extent);

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("overlap_query"), 3000, TEXT("No editor world available"));

	// Bounding-box intersection — reliable in editor without physics tick
	FBox QueryBox(Center - Extent, Center + Extent);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
		if (ActorBox.IsValid && QueryBox.Intersect(ActorBox))
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Entry->SetStringField(TEXT("class"),       Actor->GetClass()->GetName());
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("actors"), Results);
	Data->SetNumberField(TEXT("count"),  Results.Num());

	return MakeSuccess(DOMAIN, TEXT("overlap_query"),
		FString::Printf(TEXT("Found %d overlapping actors"), Results.Num()), Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> USceneQueryHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto P = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"),     Type);
		O->SetBoolField(TEXT("required"),   bReq);
		O->SetStringField(TEXT("desc"),     Desc);
		return O;
	};

	// find_actors
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("class_filter"), P(TEXT("string"), false, TEXT("Class name to filter (e.g. StaticMeshActor, /Game/BP_Foo)")));
		Ps->SetObjectField(TEXT("name_filter"),  P(TEXT("string"), false, TEXT("Substring match on actor label (case-insensitive)")));
		Ps->SetObjectField(TEXT("tag_filter"),   P(TEXT("string"), false, TEXT("Actor tag to filter on")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("List actors in the current level with optional class/name/tag filters"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("find_actors"), A);
	}

	// get_actor_details
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label (exact, case-insensitive)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Full transform, component list, tags and visibility for one actor"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("get_actor_details"), A);
	}

	// get_component_tree
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Recursive scene-component hierarchy + flat non-scene component list for an actor"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("get_component_tree"), A);
	}

	// get_scene_bounds
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("World-space bounding box over all actors, plus actor and static mesh counts"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_scene_bounds"), A);
	}

	// get_actors_in_radius
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("center"), P(TEXT("object{x,y,z}"), true,  TEXT("World-space center point (cm)")));
		Ps->SetObjectField(TEXT("radius"), P(TEXT("float"),          true,  TEXT("Search radius in cm")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("All actors within radius of center, sorted nearest first"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("get_actors_in_radius"), A);
	}

	// raycast
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("start"),   P(TEXT("object{x,y,z}"), true,  TEXT("Ray origin (cm)")));
		Ps->SetObjectField(TEXT("end"),     P(TEXT("object{x,y,z}"), true,  TEXT("Ray end point (cm)")));
		Ps->SetObjectField(TEXT("channel"), P(TEXT("string"),         false, TEXT("Collision channel: Visibility (default), Camera, WorldStatic, WorldDynamic, Pawn, PhysicsBody")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Single line trace in the editor world. Returns hit actor, component, location, normal and distance"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("raycast"), A);
	}

	// overlap_query
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("center"), P(TEXT("object{x,y,z}"), true,  TEXT("Box center in world space (cm)")));
		Ps->SetObjectField(TEXT("extent"), P(TEXT("object{x,y,z}"), false, TEXT("Box half-extents (cm, default 100 each)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Actors whose bounding boxes intersect the query box (axis-aligned)"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("overlap_query"), A);
	}

	return Root;
}
