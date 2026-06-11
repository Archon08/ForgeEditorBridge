#include "Handlers/NavMeshHandler.h"
#include "ForgeAISubsystem.h"
#include "BridgeSessionStore.h"

// ---- NavMesh / Navigation types --------------------------------------------
#include "NavMesh/RecastNavMesh.h"          // ARecastNavMesh
#include "NavigationSystem.h"               // UNavigationSystemV1, FNavigationSystem
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"                 // UNavigationPath
#include "NavFilters/NavigationQueryFilter.h" // UNavigationQueryFilter
#include "AI/Navigation/NavigationTypes.h"  // FNavAgentProperties, FPathFindingQuery, FPathFindingResult
#include "AI/Navigation/NavLinkDefinition.h" // FNavigationLink, ENavLinkDirection
#include "Navigation/NavLinkProxy.h"        // ANavLinkProxy (AIModule)

// ---- Brush builder ---------------------------------------------------------
#include "Builders/CubeBuilder.h"           // UCubeBuilder (UnrealEd)

// ---- World / editor --------------------------------------------------------
#include "EngineUtils.h"                    // TActorIterator
#include "Editor.h"                         // GEditor

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UNavMeshHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UNavMeshHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("navmesh"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("set_agent_params"))  return Action_SetAgentParams(Params);
	if (Action == TEXT("create_nav_volume")) return Action_CreateNavVolume(Params);
	if (Action == TEXT("rebuild_nav"))       return Action_RebuildNav(Params);
	if (Action == TEXT("query_path"))        return Action_QueryPath(Params);
	if (Action == TEXT("add_nav_link"))       return Action_AddNavLink(Params);
	if (Action == TEXT("set_jump_config"))    return Action_SetJumpConfig(Params);
	if (Action == TEXT("get_reachable_area")) return Action_GetReachableArea(Params);

	return MakeError(TEXT("navmesh"), Action, 1001,
		FString::Printf(
			TEXT("Unknown navmesh action '%s'. Valid: set_agent_params, create_nav_volume, rebuild_nav, query_path, add_nav_link, set_jump_config, get_reachable_area"),
			*Action));
}

// ---------------------------------------------------------------------------
// set_agent_params
// ---------------------------------------------------------------------------

FBridgeResult UNavMeshHandler::Action_SetAgentParams(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("navmesh"), TEXT("set_agent_params"));

	FString Label = TEXT("RecastNavMesh-Default");
	double AgentRadius = -1.0, AgentHeight = -1.0, MaxStepHeight = -1.0, MaxSlope = -1.0;

	Params->TryGetStringField(TEXT("label"),           Label);
	Params->TryGetNumberField(TEXT("agent_radius"),    AgentRadius);
	Params->TryGetNumberField(TEXT("agent_height"),    AgentHeight);
	Params->TryGetNumberField(TEXT("max_step_height"), MaxStepHeight);
	Params->TryGetNumberField(TEXT("max_slope"),       MaxSlope);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("set_agent_params: no editor world available");
		return Result;
	}

	ARecastNavMesh* NavMesh = nullptr;
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		if (Label == TEXT("first") || (*It)->GetActorLabel() == Label)
		{
			NavMesh = *It;
			break;
		}
	}
	if (!NavMesh)
	{
		Result.Message = FString::Printf(
			TEXT("set_agent_params: no ARecastNavMesh found with label '%s'. Pass \"first\" to target any."),
			*Label);
		return Result;
	}

	// Set properties directly — UE 5.7: AgentMaxStepHeight deprecated, use NavMeshResolutionParams
	int32 Applied = 0;
	if (AgentRadius >= 0.0)    { NavMesh->AgentRadius  = (float)AgentRadius;  ++Applied; }
	if (AgentHeight >= 0.0)    { NavMesh->AgentHeight   = (float)AgentHeight;  ++Applied; }
	if (MaxSlope >= 0.0)       { NavMesh->AgentMaxSlope = (float)MaxSlope;     ++Applied; }
	if (MaxStepHeight >= 0.0)
	{
		// UE 5.7: Use SetAgentMaxStepHeight per-resolution instead of deprecated property
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::Default, (float)MaxStepHeight);
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::Low, (float)MaxStepHeight);
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::High, (float)MaxStepHeight);
		++Applied;
	}

	NavMesh->PostEditChange();
	NavMesh->MarkPackageDirty();

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("set_agent_params: applied %d params to NavMesh '%s' (radius=%.1f, height=%.1f, slope=%.1f)"),
		Applied, *Label,
		NavMesh->AgentRadius, NavMesh->AgentHeight, NavMesh->AgentMaxSlope);
	return Result;
}

// ---------------------------------------------------------------------------
// create_nav_volume
// ---------------------------------------------------------------------------

FBridgeResult UNavMeshHandler::Action_CreateNavVolume(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("navmesh"), TEXT("create_nav_volume"));

	FString Label;
	double X = 0.0, Y = 0.0, Z = 0.0;
	double ExtentX = 1000.0, ExtentY = 1000.0, ExtentZ = 500.0;

	Params->TryGetStringField(TEXT("label"),    Label);
	Params->TryGetNumberField(TEXT("x"),        X);
	Params->TryGetNumberField(TEXT("y"),        Y);
	Params->TryGetNumberField(TEXT("z"),        Z);
	Params->TryGetNumberField(TEXT("extent_x"), ExtentX);
	Params->TryGetNumberField(TEXT("extent_y"), ExtentY);
	Params->TryGetNumberField(TEXT("extent_z"), ExtentZ);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("create_nav_volume: no editor world available");
		return Result;
	}

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));

	ANavMeshBoundsVolume* Vol = World->SpawnActor<ANavMeshBoundsVolume>(
		ANavMeshBoundsVolume::StaticClass(), SpawnTransform);
	if (!Vol)
	{
		Result.Message = FString::Printf(
			TEXT("create_nav_volume: failed to spawn ANavMeshBoundsVolume at (%.0f, %.0f, %.0f)"),
			(float)X, (float)Y, (float)Z);
		return Result;
	}

	// Resize the volume brush using UCubeBuilder
	UCubeBuilder* Builder = NewObject<UCubeBuilder>(GetTransientPackage(), UCubeBuilder::StaticClass());
	Builder->X = (float)ExtentX * 2.0f;
	Builder->Y = (float)ExtentY * 2.0f;
	Builder->Z = (float)ExtentZ * 2.0f;
	Builder->Build(World, Vol);
	Vol->PostEditChange();

	if (!Label.IsEmpty())
	{
		Vol->SetActorLabel(Label);
	}

	// Notify the navigation system of the new bounds volume so affected tiles are marked dirty.
	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		NavSys->OnNavigationBoundsUpdated(Vol);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Vol->GetActorLabel());
	Data->SetNumberField(TEXT("x"), X);
	Data->SetNumberField(TEXT("y"), Y);
	Data->SetNumberField(TEXT("z"), Z);
	Data->SetNumberField(TEXT("extent_x"), ExtentX);
	Data->SetNumberField(TEXT("extent_y"), ExtentY);
	Data->SetNumberField(TEXT("extent_z"), ExtentZ);
	Data->SetStringField(TEXT("next_step"),
		TEXT("Call navmesh/rebuild_nav to rebuild the NavMesh bounds with the new volume."));

	return MakeSuccess(TEXT("navmesh"), TEXT("create_nav_volume"),
		FString::Printf(
			TEXT("NavMeshBoundsVolume '%s' created at (%.0f, %.0f, %.0f) extents=(%.0f, %.0f, %.0f). Call navmesh/rebuild_nav to regenerate tiles."),
			*Vol->GetActorLabel(), (float)X, (float)Y, (float)Z,
			(float)ExtentX, (float)ExtentY, (float)ExtentZ),
		Data);
}

// ---------------------------------------------------------------------------
// rebuild_nav
// ---------------------------------------------------------------------------

FBridgeResult UNavMeshHandler::Action_RebuildNav(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("rebuild_nav");

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("navmesh"), Action, 2000, TEXT("rebuild_nav: no editor world available"));

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
		return MakeError(TEXT("navmesh"), Action, 2001,
			TEXT("rebuild_nav: no UNavigationSystemV1 in current world. Ensure Navigation System is enabled in project settings."));

	// Register async job so callers can poll for completion.
	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("navmesh/rebuild_nav"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("NavMesh rebuild started"));

	// Async — tile generators run on worker threads over multiple frames.
	NavSys->Build();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"), JobId);
	Data->SetStringField(TEXT("note"),
		TEXT("NavMesh rebuild is async. Check navmesh rebuild completion via editor Build menu or listen to UNavigationSystemV1::OnNavigationGenerationFinishedDelegate."));

	return MakeSuccess(TEXT("navmesh"), Action, TEXT("NavMesh rebuild started"), Data);
}

// ---------------------------------------------------------------------------
// query_path
// ---------------------------------------------------------------------------

FBridgeResult UNavMeshHandler::Action_QueryPath(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("query_path");
	FBridgeResult Result = CreateResult(TEXT("navmesh"), Action);

	// Parse start
	const TSharedPtr<FJsonObject>* StartObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("start"), StartObj) || !StartObj || !(*StartObj).IsValid())
	{
		Result.Message = TEXT("query_path: 'start' {x,y,z} is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	double SX = 0.0, SY = 0.0, SZ = 0.0;
	(*StartObj)->TryGetNumberField(TEXT("x"), SX);
	(*StartObj)->TryGetNumberField(TEXT("y"), SY);
	(*StartObj)->TryGetNumberField(TEXT("z"), SZ);

	// Parse end
	const TSharedPtr<FJsonObject>* EndObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("end"), EndObj) || !EndObj || !(*EndObj).IsValid())
	{
		Result.Message = TEXT("query_path: 'end' {x,y,z} is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	double EX = 0.0, EY = 0.0, EZ = 0.0;
	(*EndObj)->TryGetNumberField(TEXT("x"), EX);
	(*EndObj)->TryGetNumberField(TEXT("y"), EY);
	(*EndObj)->TryGetNumberField(TEXT("z"), EZ);

	double AgentRadius = -1.0;
	Params->TryGetNumberField(TEXT("agent_radius"), AgentRadius);

	// PIE guard — NavMesh runtime state is unavailable in editor world.
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World || !World->IsPlayInEditor())
		return MakeError(TEXT("navmesh"), Action, 3004,
			TEXT("query_path requires PIE — NavMesh runtime state is unavailable in editor world."),
			TEXT("Start Play In Editor (PIE) and then call this action."));

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		Result.Message = TEXT("query_path: no UNavigationSystemV1 in current world. Ensure Navigation System is enabled.");
		return Result;
	}

	// Null-check nav data before dereferencing — GetDefaultNavDataInstance returns nullptr
	// when no navmesh has been built (empty level, no bounds volume, post-PIE cleanup, etc.).
	ANavigationData* NavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
	if (!NavData)
		return MakeError(TEXT("navmesh"), Action, 2001,
			TEXT("No NavMesh data found. Build the NavMesh first using navmesh/rebuild_nav."));

	FNavAgentProperties AgentProps;
	if (AgentRadius >= 0.0)
	{
		AgentProps.AgentRadius = (float)AgentRadius;
	}

	FVector StartLoc((float)SX, (float)SY, (float)SZ);
	FVector EndLoc((float)EX, (float)EY, (float)EZ);

	FPathFindingQuery Query(
		/*InOwner*/        nullptr,
		/*InNavData*/      *NavData,
		/*StartLocation*/  StartLoc,
		/*EndLocation*/    EndLoc,
		/*InQueryFilter*/  UNavigationQueryFilter::GetQueryFilter<UNavigationQueryFilter>(*NavData));

	FPathFindingResult PathResult = NavSys->FindPathSync(AgentProps, Query);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	const bool bSuccess = PathResult.IsSuccessful() && PathResult.Path.IsValid();
	Data->SetBoolField(TEXT("path_exists"), bSuccess);

	if (bSuccess)
	{
		const TArray<FNavPathPoint>& Points = PathResult.Path->GetPathPoints();
		Data->SetNumberField(TEXT("path_length"), (double)PathResult.Path->GetLength());
		Data->SetNumberField(TEXT("waypoint_count"), Points.Num());

		TArray<TSharedPtr<FJsonValue>> Waypoints;
		for (const FNavPathPoint& Pt : Points)
		{
			TSharedPtr<FJsonObject> WP = MakeShared<FJsonObject>();
			WP->SetNumberField(TEXT("x"), Pt.Location.X);
			WP->SetNumberField(TEXT("y"), Pt.Location.Y);
			WP->SetNumberField(TEXT("z"), Pt.Location.Z);
			Waypoints.Add(MakeShared<FJsonValueObject>(WP));
		}
		Data->SetArrayField(TEXT("waypoints"), Waypoints);

		Result.bSuccess = true;
		Result.Message = FString::Printf(
			TEXT("Path found: length=%.1f, waypoints=%d"),
			PathResult.Path->GetLength(), Points.Num());
	}
	else
	{
		Data->SetNumberField(TEXT("path_length"), 0.0);
		Data->SetNumberField(TEXT("waypoint_count"), 0);
		Data->SetArrayField(TEXT("waypoints"), TArray<TSharedPtr<FJsonValue>>());

		Result.bSuccess = true; // query succeeded even if no path
		Result.Message = TEXT("No navigable path found between start and end");
	}

	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result.ExtraData);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	return Result;
}

// ---------------------------------------------------------------------------
// add_nav_link  (Phase 1d P2 gap)
// Spawns a NavLinkProxy actor and configures its Smart/Simple link endpoints.
// Params: start {x,y,z}, end {x,y,z}, label (string, optional actor label),
//         direction (BothWays|LeftToRight|RightToLeft, optional, default BothWays)
// ---------------------------------------------------------------------------
FBridgeResult UNavMeshHandler::Action_AddNavLink(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("add_nav_link");

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("navmesh"), Action, 2000, TEXT("No editor world available"));

	// Parse start
	const TSharedPtr<FJsonObject>* StartObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("start"), StartObj) || !StartObj)
		return MakeError(TEXT("navmesh"), Action, 1000, TEXT("'start' {x,y,z} is required"));
	double SX = 0, SY = 0, SZ = 0;
	(*StartObj)->TryGetNumberField(TEXT("x"), SX);
	(*StartObj)->TryGetNumberField(TEXT("y"), SY);
	(*StartObj)->TryGetNumberField(TEXT("z"), SZ);

	// Parse end
	const TSharedPtr<FJsonObject>* EndObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("end"), EndObj) || !EndObj)
		return MakeError(TEXT("navmesh"), Action, 1000, TEXT("'end' {x,y,z} is required"));
	double EX = 0, EY = 0, EZ = 0;
	(*EndObj)->TryGetNumberField(TEXT("x"), EX);
	(*EndObj)->TryGetNumberField(TEXT("y"), EY);
	(*EndObj)->TryGetNumberField(TEXT("z"), EZ);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	FString Direction = TEXT("BothWays");
	Params->TryGetStringField(TEXT("direction"), Direction);

	// Spawn ANavLinkProxy directly — ANavLinkProxy is in AIModule and can be referenced concretely.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector MidPoint(((float)SX + (float)EX) * 0.5f, ((float)SY + (float)EY) * 0.5f, ((float)SZ + (float)EZ) * 0.5f);

	ANavLinkProxy* LinkProxy = World->SpawnActor<ANavLinkProxy>(
		ANavLinkProxy::StaticClass(), MidPoint, FRotator::ZeroRotator, SpawnParams);
	if (!LinkProxy)
		return MakeError(TEXT("navmesh"), Action, 3000, TEXT("Failed to spawn ANavLinkProxy actor"));

	if (!Label.IsEmpty())
		LinkProxy->SetActorLabel(Label);

	// Write the endpoint struct directly — ANavLinkProxy::PointLinks is BlueprintReadOnly,
	// so reflection-based property writes won't work here. Direct C++ member access is the correct path.
	if (LinkProxy->PointLinks.Num() == 0)
	{
		LinkProxy->PointLinks.Add(FNavigationLink());
	}

	{
		FNavigationLink& Link = LinkProxy->PointLinks[0];
		// Endpoints are actor-relative (MidPoint is the actor origin).
		const FVector ActorLoc = LinkProxy->GetActorLocation();
		const FVector StartLocation((float)SX, (float)SY, (float)SZ);
		const FVector EndLocation((float)EX, (float)EY, (float)EZ);
		Link.Left  = StartLocation - ActorLoc;
		Link.Right = EndLocation   - ActorLoc;

		if (Direction.Equals(TEXT("LeftToRight"), ESearchCase::IgnoreCase))
			Link.Direction = ENavLinkDirection::LeftToRight;
		else if (Direction.Equals(TEXT("RightToLeft"), ESearchCase::IgnoreCase))
			Link.Direction = ENavLinkDirection::RightToLeft;
		else
			Link.Direction = ENavLinkDirection::BothWays;
	}

	LinkProxy->MarkComponentsRenderStateDirty();
	LinkProxy->PostEditChange();
	LinkProxy->MarkPackageDirty();

	// Hint the navigation system to re-register this actor's nav links.
	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		NavSys->UpdateActorAndComponentsInNavOctree(*LinkProxy);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), LinkProxy->GetActorLabel());
	Data->SetStringField(TEXT("direction"),   Direction);
	Data->SetNumberField(TEXT("start_x"), SX); Data->SetNumberField(TEXT("start_y"), SY); Data->SetNumberField(TEXT("start_z"), SZ);
	Data->SetNumberField(TEXT("end_x"),   EX); Data->SetNumberField(TEXT("end_y"),   EY); Data->SetNumberField(TEXT("end_z"),   EZ);

	return MakeSuccess(TEXT("navmesh"), Action,
		FString::Printf(TEXT("NavLinkProxy '%s' spawned at (%.0f,%.0f,%.0f) with endpoints written"),
			*LinkProxy->GetActorLabel(), MidPoint.X, MidPoint.Y, MidPoint.Z),
		Data);
}

// ---------------------------------------------------------------------------
// set_jump_config  (Phase 1d P2 gap)
// Params: label (string — RecastNavMesh actor label, or "first"),
//         jump_over_low_height (float), jump_down_height_max (float),
//         jump_vertical_scale (float)
// UE 5.7: jump settings on ARecastNavMesh via NavMeshGenerationProperties or direct.
// ---------------------------------------------------------------------------
FBridgeResult UNavMeshHandler::Action_SetJumpConfig(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_jump_config");

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("navmesh"), Action, 2000, TEXT("No editor world available"));

	FString Label = TEXT("first");
	Params->TryGetStringField(TEXT("label"), Label);

	// Find the RecastNavMesh actor
	ARecastNavMesh* NavMesh = nullptr;
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		if (Label == TEXT("first") || (*It)->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			NavMesh = *It;
			break;
		}
	}
	if (!NavMesh)
		return MakeError(TEXT("navmesh"), Action, 2001,
			FString::Printf(TEXT("RecastNavMesh actor '%s' not found in current level"), *Label));

	// Reject jump_vertical_scale up-front — no Recast binding exists for it in UE 5.7.
	if (Params->HasField(TEXT("jump_vertical_scale")))
		return MakeError(TEXT("navmesh"), Action, 3003,
			TEXT("jump_vertical_scale has no direct Recast navmesh binding in UE 5.7. "
			     "Use NavLink Area classes or FNavigationLink.MaxFallDownLength."));

	// jump_over_low_height maps to per-agent jump height which does NOT exist on ARecastNavMesh
	// in UE 5.7 (not on FNavAgentProperties either). Return 3003 with a recovery hint.
	if (Params->HasField(TEXT("jump_over_low_height")))
		return MakeError(TEXT("navmesh"), Action, 3003,
			TEXT("jump_height configuration requires FNavAgentProperties at runtime — "
			     "not settable on ARecastNavMesh asset directly"),
			TEXT("Configure jump height via UCharacterMovementComponent::JumpZVelocity in your character class"));

	NavMesh->Modify();

	double JumpDownMax = 0.0;
	bool bAnySet = false;

	if (Params->TryGetNumberField(TEXT("jump_down_height_max"), JumpDownMax))
	{
		// UE 5.7: direct AgentMaxStepHeight is deprecated — use per-resolution setter.
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::Default, (float)JumpDownMax);
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::Low,     (float)JumpDownMax);
		NavMesh->SetAgentMaxStepHeight(ENavigationDataResolution::High,    (float)JumpDownMax);
		bAnySet = true;
	}

	if (!bAnySet)
		return MakeError(TEXT("navmesh"), Action, 1000,
			TEXT("jump_down_height_max is required (jump_over_low_height / jump_vertical_scale are not supported on ARecastNavMesh in UE 5.7)"));

	NavMesh->PostEditChange();
	NavMesh->MarkPackageDirty();

	// Rebuild via the navigation system (ARecastNavMesh::RequestNavMeshUpdate does not exist in UE 5.7).
	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		NavSys->Build();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("navmesh"), NavMesh->GetActorLabel());
	Data->SetNumberField(TEXT("agent_max_step_height_default"),
		NavMesh->GetAgentMaxStepHeight(ENavigationDataResolution::Default));
	Data->SetNumberField(TEXT("agent_max_step_height_low"),
		NavMesh->GetAgentMaxStepHeight(ENavigationDataResolution::Low));
	Data->SetNumberField(TEXT("agent_max_step_height_high"),
		NavMesh->GetAgentMaxStepHeight(ENavigationDataResolution::High));

	return MakeSuccess(TEXT("navmesh"), Action,
		FString::Printf(TEXT("Jump config updated on '%s' (per-resolution AgentMaxStepHeight = %.1f)"),
			*NavMesh->GetActorLabel(), (float)JumpDownMax), Data);
}

// ---------------------------------------------------------------------------
// get_reachable_area  (Phase 1d P2 gap)
// Params: origin {x,y,z}, max_distance (float, optional, default 2000),
//         agent_radius (float, optional)
// Returns approx reachable surface area by sampling a grid of nav queries.
// ---------------------------------------------------------------------------
FBridgeResult UNavMeshHandler::Action_GetReachableArea(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_reachable_area");

	const TSharedPtr<FJsonObject>* OriginObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("origin"), OriginObj) || !OriginObj)
		return MakeError(TEXT("navmesh"), Action, 1000, TEXT("'origin' {x,y,z} is required"));

	double OX = 0, OY = 0, OZ = 0;
	(*OriginObj)->TryGetNumberField(TEXT("x"), OX);
	(*OriginObj)->TryGetNumberField(TEXT("y"), OY);
	(*OriginObj)->TryGetNumberField(TEXT("z"), OZ);

	double MaxDist = 2000.0;
	Params->TryGetNumberField(TEXT("max_distance"), MaxDist);

	double AgentRadius = -1.0;
	Params->TryGetNumberField(TEXT("agent_radius"), AgentRadius);

	// PIE guard — NavMesh runtime queries are unsafe against the editor world.
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World || !World->IsPlayInEditor())
		return MakeError(TEXT("navmesh"), Action, 3004,
			TEXT("get_reachable_area requires PIE — NavMesh runtime state is unavailable in editor world."),
			TEXT("Start Play In Editor (PIE) and then call this action."));

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
		return MakeError(TEXT("navmesh"), Action, 2001, TEXT("No UNavigationSystemV1 in current world"));

	const FVector Origin((float)OX, (float)OY, (float)OZ);
	FNavAgentProperties AgentProps;
	if (AgentRadius >= 0.0) AgentProps.AgentRadius = (float)AgentRadius;

	// Sample a grid of points and count those with valid paths from origin
	const int32 Steps = 10;
	const float Step = (float)MaxDist / Steps;
	int32 ReachableCount = 0;
	int32 TotalSampled = 0;

	const ANavigationData* NavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
	if (!NavData)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("reachable_cells"), 0);
		Data->SetNumberField(TEXT("estimated_area_cm2"), 0.0);
		return MakeSuccess(TEXT("navmesh"), Action, TEXT("No nav data — build nav mesh first"), Data);
	}

	for (int32 xi = -Steps; xi <= Steps; ++xi)
	{
		for (int32 yi = -Steps; yi <= Steps; ++yi)
		{
			FVector TestPt = Origin + FVector(xi * Step, yi * Step, 0.0f);
			FPathFindingQuery Query(nullptr, *NavData, Origin, TestPt,
				UNavigationQueryFilter::GetQueryFilter<UNavigationQueryFilter>(*NavData));
			FPathFindingResult PR = NavSys->FindPathSync(AgentProps, Query);
			if (PR.IsSuccessful()) ++ReachableCount;
			++TotalSampled;
		}
	}

	const float CellArea = Step * Step;
	const float EstArea = ReachableCount * CellArea;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("reachable_cells"), ReachableCount);
	Data->SetNumberField(TEXT("total_sampled"), TotalSampled);
	Data->SetNumberField(TEXT("cell_size_cm"), Step);
	Data->SetNumberField(TEXT("estimated_area_cm2"), EstArea);
	Data->SetNumberField(TEXT("estimated_area_m2"), EstArea / 10000.0f);

	return MakeSuccess(TEXT("navmesh"), Action,
		FString::Printf(TEXT("Reachable: %d/%d cells (~%.0f m²) from origin"),
			ReachableCount, TotalSampled, EstArea / 10000.0f),
		Data);
}
