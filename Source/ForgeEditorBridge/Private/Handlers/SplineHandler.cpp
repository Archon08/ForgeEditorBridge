#include "Handlers/SplineHandler.h"
#include "ForgeAISubsystem.h"

// ---- Spline / Actor --------------------------------------------------------
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"               // TActorIterator

// ---- Editor ----------------------------------------------------------------
#include "Editor.h"                     // GEditor
#include "ScopedTransaction.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

static const FString SPLINE_DOMAIN = TEXT("spline");

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void USplineHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("create_spline_actor")) return Action_CreateSplineActor(Params);
	if (Action == TEXT("add_point"))           return Action_AddPoint(Params);
	if (Action == TEXT("set_point"))           return Action_SetPoint(Params);
	if (Action == TEXT("set_tangent"))         return Action_SetTangent(Params);
	if (Action == TEXT("get_points"))          return Action_GetPoints(Params);
	if (Action == TEXT("close_loop"))          return Action_CloseLoop(Params);
	if (Action == TEXT("set_point_type"))      return Action_SetPointType(Params);
	if (Action == TEXT("delete_point"))        return Action_DeletePoint(Params);
	if (Action == TEXT("clear_points"))        return Action_ClearPoints(Params);
	if (Action == TEXT("get_length"))              return Action_GetLength(Params);
	if (Action == TEXT("get_point_at_distance"))   return Action_GetPointAtDistance(Params);

	return MakeError(SPLINE_DOMAIN, Action, 1000,
		FString::Printf(TEXT("Unknown spline action '%s'. Valid: create_spline_actor, add_point, set_point, set_tangent, get_points, close_loop, set_point_type, delete_point, clear_points, get_length, get_point_at_distance"), *Action));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FVector USplineHandler::ParseVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& bFound)
{
	bFound = false;
	const TSharedPtr<FJsonObject>* SubObj = nullptr;
	if (Obj->TryGetObjectField(Key, SubObj) && SubObj && SubObj->IsValid())
	{
		bFound = true;
		double X = 0.0, Y = 0.0, Z = 0.0;
		(*SubObj)->TryGetNumberField(TEXT("x"), X);
		(*SubObj)->TryGetNumberField(TEXT("y"), Y);
		(*SubObj)->TryGetNumberField(TEXT("z"), Z);
		return FVector(X, Y, Z);
	}
	return FVector::ZeroVector;
}

AActor* USplineHandler::FindActorByLabel(const FString& ActorLabel, FString& OutError)
{
#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		OutError = TEXT("No editor world available");
		return nullptr;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == ActorLabel)
		{
			return Actor;
		}
	}

	OutError = FString::Printf(TEXT("No actor found with label '%s'"), *ActorLabel);
	return nullptr;
#else
	OutError = TEXT("Editor-only operation");
	return nullptr;
#endif
}

USplineComponent* USplineHandler::GetSplineComponent(AActor* Actor, FString& OutError)
{
	if (!Actor)
	{
		OutError = TEXT("Actor is null");
		return nullptr;
	}

	USplineComponent* Spline = Actor->FindComponentByClass<USplineComponent>();
	if (!Spline)
	{
		OutError = FString::Printf(TEXT("Actor '%s' has no USplineComponent"), *Actor->GetActorLabel());
	}
	return Spline;
}

USplineComponent* USplineHandler::GetSplineComponentByName(AActor* Actor, const FString& ComponentName, FString& OutError)
{
	if (!Actor)
	{
		OutError = TEXT("Actor is null");
		return nullptr;
	}

	// If no name given, fall back to the first spline component on the actor.
	if (ComponentName.IsEmpty())
	{
		return GetSplineComponent(Actor, OutError);
	}

	TArray<USplineComponent*> Splines;
	Actor->GetComponents<USplineComponent>(Splines);
	for (USplineComponent* S : Splines)
	{
		if (!S) continue;
		if (S->GetName() == ComponentName || S->GetFName() == FName(*ComponentName))
		{
			return S;
		}
	}

	OutError = FString::Printf(TEXT("Actor '%s' has no USplineComponent named '%s' (found %d spline component(s))"),
		*Actor->GetActorLabel(), *ComponentName, Splines.Num());
	return nullptr;
}

// ---------------------------------------------------------------------------
// create_spline_actor
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_CreateSplineActor(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_spline_actor");

#if WITH_EDITOR
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_name'"));
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		return MakeError(SPLINE_DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	// Parse location
	bool bHasLocation = false;
	FVector Location = ParseVector(Params, TEXT("location"), bHasLocation);

	// Parse points array
	TArray<FVector> Points;
	const TArray<TSharedPtr<FJsonValue>>* PointsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("points"), PointsArray) && PointsArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *PointsArray)
		{
			const TSharedPtr<FJsonObject>* PtObj = nullptr;
			if (Val->TryGetObject(PtObj) && PtObj && PtObj->IsValid())
			{
				double X = 0.0, Y = 0.0, Z = 0.0;
				(*PtObj)->TryGetNumberField(TEXT("x"), X);
				(*PtObj)->TryGetNumberField(TEXT("y"), Y);
				(*PtObj)->TryGetNumberField(TEXT("z"), Z);
				Points.Add(FVector(X, Y, Z));
			}
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Create Spline Actor '%s'"), *ActorName)));

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*ActorName);
	FRotator ZeroRot = FRotator::ZeroRotator;
	AActor* NewActor = World->SpawnActor(AActor::StaticClass(), &Location, &ZeroRot, SpawnParams);
	if (!NewActor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 3000, TEXT("Failed to spawn actor"));
	}

	NewActor->SetActorLabel(ActorName);

	USplineComponent* Spline = NewObject<USplineComponent>(NewActor, TEXT("SplineComponent"));
	Spline->SetupAttachment(NewActor->GetRootComponent());
	Spline->RegisterComponent();
	NewActor->AddInstanceComponent(Spline);

	Spline->ClearSplinePoints(false);
	for (const FVector& Pt : Points)
	{
		Spline->AddSplinePoint(Pt, ESplineCoordinateSpace::Local, false);
	}
	Spline->UpdateSpline();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), ActorName);
	Data->SetNumberField(TEXT("point_count"), Points.Num());

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Created spline actor '%s' with %d points"), *ActorName, Points.Num()),
		Data);
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// add_point
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_AddPoint(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("add_point");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	bool bHasPos = false;
	FVector Position = ParseVector(Params, TEXT("position"), bHasPos);
	if (!bHasPos)
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'position' {x,y,z}"));
	}

	double IndexD = -1.0;
	Params->TryGetNumberField(TEXT("index"), IndexD);
	int32 Index = static_cast<int32>(IndexD);

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Add Spline Point to '%s'"), *ActorPath)));

	if (Index >= 0 && Index <= Spline->GetNumberOfSplinePoints())
	{
		Spline->AddSplinePointAtIndex(Position, Index, ESplineCoordinateSpace::Local, true);
	}
	else
	{
		Spline->AddSplinePoint(Position, ESplineCoordinateSpace::Local, true);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total_points"), Spline->GetNumberOfSplinePoints());

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Added point at (%.1f, %.1f, %.1f) to '%s'"), Position.X, Position.Y, Position.Z, *ActorPath),
		Data);
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// set_point
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_SetPoint(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_point");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double IndexD = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), IndexD))
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'index'"));
	}
	int32 Index = static_cast<int32>(IndexD);

	bool bHasPos = false;
	FVector Position = ParseVector(Params, TEXT("position"), bHasPos);
	if (!bHasPos)
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'position' {x,y,z}"));
	}

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Index %d out of range [0, %d)"), Index, Spline->GetNumberOfSplinePoints()));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Set Spline Point %d on '%s'"), Index, *ActorPath)));

	Spline->SetLocationAtSplinePoint(Index, Position, ESplineCoordinateSpace::Local, true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Set point %d to (%.1f, %.1f, %.1f) on '%s'"), Index, Position.X, Position.Y, Position.Z, *ActorPath));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// set_tangent
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_SetTangent(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_tangent");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double IndexD = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), IndexD))
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'index'"));
	}
	int32 Index = static_cast<int32>(IndexD);

	bool bHasArrive = false, bHasLeave = false;
	FVector ArriveTangent = ParseVector(Params, TEXT("arrive_tangent"), bHasArrive);
	FVector LeaveTangent  = ParseVector(Params, TEXT("leave_tangent"), bHasLeave);
	if (!bHasArrive || !bHasLeave)
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			TEXT("Missing required params: 'arrive_tangent' and 'leave_tangent' {x,y,z}"));
	}

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Index %d out of range [0, %d)"), Index, Spline->GetNumberOfSplinePoints()));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Set Spline Tangent %d on '%s'"), Index, *ActorPath)));

	Spline->SetTangentsAtSplinePoint(Index, ArriveTangent, LeaveTangent, ESplineCoordinateSpace::Local, true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Set tangents at point %d on '%s'"), Index, *ActorPath));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// get_points
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_GetPoints(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_points");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	const int32 NumPoints = Spline->GetNumberOfSplinePoints();

	TArray<TSharedPtr<FJsonValue>> PointsArray;
	for (int32 i = 0; i < NumPoints; ++i)
	{
		TSharedPtr<FJsonObject> PtObj = MakeShared<FJsonObject>();

		FVector Pos = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::Local);
		TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
		PosObj->SetNumberField(TEXT("x"), Pos.X);
		PosObj->SetNumberField(TEXT("y"), Pos.Y);
		PosObj->SetNumberField(TEXT("z"), Pos.Z);
		PtObj->SetObjectField(TEXT("position"), PosObj);

		FVector Arrive = Spline->GetArriveTangentAtSplinePoint(i, ESplineCoordinateSpace::Local);
		TSharedPtr<FJsonObject> ArrObj = MakeShared<FJsonObject>();
		ArrObj->SetNumberField(TEXT("x"), Arrive.X);
		ArrObj->SetNumberField(TEXT("y"), Arrive.Y);
		ArrObj->SetNumberField(TEXT("z"), Arrive.Z);
		PtObj->SetObjectField(TEXT("arrive_tangent"), ArrObj);

		FVector Leave = Spline->GetLeaveTangentAtSplinePoint(i, ESplineCoordinateSpace::Local);
		TSharedPtr<FJsonObject> LvObj = MakeShared<FJsonObject>();
		LvObj->SetNumberField(TEXT("x"), Leave.X);
		LvObj->SetNumberField(TEXT("y"), Leave.Y);
		LvObj->SetNumberField(TEXT("z"), Leave.Z);
		PtObj->SetObjectField(TEXT("leave_tangent"), LvObj);

		ESplinePointType::Type PointType = Spline->GetSplinePointType(i);
		FString TypeStr;
		switch (PointType)
		{
		case ESplinePointType::Linear:       TypeStr = TEXT("Linear"); break;
		case ESplinePointType::Curve:        TypeStr = TEXT("Curve"); break;
		case ESplinePointType::Constant:     TypeStr = TEXT("Constant"); break;
		case ESplinePointType::CurveClamped: TypeStr = TEXT("CurveClamped"); break;
		default:                             TypeStr = TEXT("Unknown"); break;
		}
		PtObj->SetStringField(TEXT("type"), TypeStr);
		PtObj->SetNumberField(TEXT("index"), i);

		PointsArray.Add(MakeShared<FJsonValueObject>(PtObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("points"), PointsArray);
	Data->SetNumberField(TEXT("point_count"), NumPoints);
	Data->SetBoolField(TEXT("closed"), Spline->IsClosedLoop());
	Data->SetNumberField(TEXT("total_length"), Spline->GetSplineLength());

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Spline '%s': %d points, length=%.1f, closed=%s"),
			*ActorPath, NumPoints, Spline->GetSplineLength(),
			Spline->IsClosedLoop() ? TEXT("true") : TEXT("false")),
		Data);
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// close_loop
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_CloseLoop(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("close_loop");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	bool bClosed = true;
	Params->TryGetBoolField(TEXT("closed"), bClosed);

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: %s Spline Loop on '%s'"),
			bClosed ? TEXT("Close") : TEXT("Open"), *ActorPath)));

	Spline->SetClosedLoop(bClosed, true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Spline '%s' closed_loop set to %s"), *ActorPath,
			bClosed ? TEXT("true") : TEXT("false")));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// set_point_type
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_SetPointType(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_point_type");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double IndexD = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), IndexD))
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'index'"));
	}
	int32 Index = static_cast<int32>(IndexD);

	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			TEXT("Missing required param: 'type' (Linear/Curve/Constant/CurveClamped)"));
	}

	ESplinePointType::Type PointType;
	if (TypeStr.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))
	{
		PointType = ESplinePointType::Linear;
	}
	else if (TypeStr.Equals(TEXT("Curve"), ESearchCase::IgnoreCase))
	{
		PointType = ESplinePointType::Curve;
	}
	else if (TypeStr.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))
	{
		PointType = ESplinePointType::Constant;
	}
	else if (TypeStr.Equals(TEXT("CurveClamped"), ESearchCase::IgnoreCase))
	{
		PointType = ESplinePointType::CurveClamped;
	}
	else
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Invalid type '%s'. Valid: Linear, Curve, Constant, CurveClamped"), *TypeStr),
			TEXT("Use one of: Linear, Curve, Constant, CurveClamped"));
	}

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Index %d out of range [0, %d)"), Index, Spline->GetNumberOfSplinePoints()));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Set Spline Point Type %d on '%s'"), Index, *ActorPath)));

	Spline->SetSplinePointType(Index, PointType, true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Set point %d type to '%s' on '%s'"), Index, *TypeStr, *ActorPath));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// delete_point
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_DeletePoint(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("delete_point");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double IndexD = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), IndexD))
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'index'"));
	}
	const int32 Index = static_cast<int32>(IndexD);

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Index %d out of range [0, %d)"), Index, Spline->GetNumberOfSplinePoints()));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Delete Spline Point %d on '%s'"), Index, *ActorPath)));

	Spline->RemoveSplinePoint(Index, /*bUpdateSpline=*/true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Deleted spline point %d on '%s' (%d points remaining)"),
			Index, *ActorPath, Spline->GetNumberOfSplinePoints()));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// clear_points
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_ClearPoints(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("clear_points");

#if WITH_EDITOR
	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	FString Error;
	AActor* Actor = FindActorByLabel(ActorPath, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponent(Actor, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("ForgeEditorBridge: Clear Spline Points on '%s'"), *ActorPath)));

	Spline->ClearSplinePoints(/*bUpdateSpline=*/true);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Cleared all spline points on '%s'"), *ActorPath));
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// get_length
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_GetLength(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_length");

#if WITH_EDITOR
	FString ActorLabel;
	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_label'"));
	}

	FString ComponentName;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);

	FString Error;
	AActor* Actor = FindActorByLabel(ActorLabel, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponentByName(Actor, ComponentName, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	const float Length     = Spline->GetSplineLength();
	const int32 NumPoints  = Spline->GetNumberOfSplinePoints();
	const bool bClosedLoop = Spline->IsClosedLoop();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("length"), Length);
	Data->SetNumberField(TEXT("num_points"), NumPoints);
	Data->SetBoolField(TEXT("is_closed_loop"), bClosedLoop);

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Spline '%s': length=%.2f, points=%d, closed=%s"),
			*ActorLabel, Length, NumPoints, bClosedLoop ? TEXT("true") : TEXT("false")),
		Data);
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// get_point_at_distance
// ---------------------------------------------------------------------------

FBridgeResult USplineHandler::Action_GetPointAtDistance(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_point_at_distance");

#if WITH_EDITOR
	FString ActorLabel;
	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_label'"));
	}

	double DistanceD = 0.0;
	if (!Params->TryGetNumberField(TEXT("distance"), DistanceD))
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000, TEXT("Missing required param: 'distance' (float, arc-length from spline start)"));
	}
	const float Distance = static_cast<float>(DistanceD);
	if (Distance < 0.0f)
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Invalid 'distance' %.3f: must be >= 0"), Distance));
	}

	FString ComponentName;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);

	FString CoordSpaceStr = TEXT("world");
	Params->TryGetStringField(TEXT("coordinate_space"), CoordSpaceStr);
	ESplineCoordinateSpace::Type CoordSpace;
	if (CoordSpaceStr.Equals(TEXT("world"), ESearchCase::IgnoreCase))
	{
		CoordSpace = ESplineCoordinateSpace::World;
	}
	else if (CoordSpaceStr.Equals(TEXT("local"), ESearchCase::IgnoreCase))
	{
		CoordSpace = ESplineCoordinateSpace::Local;
	}
	else
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Invalid 'coordinate_space' '%s'. Valid: world, local"), *CoordSpaceStr));
	}

	bool bIncludeTangent  = true;
	bool bIncludeNormal   = false;
	bool bIncludeRotation = false;
	Params->TryGetBoolField(TEXT("include_tangent"),  bIncludeTangent);
	Params->TryGetBoolField(TEXT("include_normal"),   bIncludeNormal);
	Params->TryGetBoolField(TEXT("include_rotation"), bIncludeRotation);

	FString Error;
	AActor* Actor = FindActorByLabel(ActorLabel, Error);
	if (!Actor)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2000, Error, TEXT("Check actor label in the World Outliner"));
	}

	USplineComponent* Spline = GetSplineComponentByName(Actor, ComponentName, Error);
	if (!Spline)
	{
		return MakeError(SPLINE_DOMAIN, Action, 2001, Error);
	}

	const float TotalLength = Spline->GetSplineLength();
	if (Distance > TotalLength)
	{
		return MakeError(SPLINE_DOMAIN, Action, 1000,
			FString::Printf(TEXT("Invalid 'distance' %.3f: exceeds spline length %.3f"), Distance, TotalLength),
			TEXT("Pass a distance in [0, spline length]"));
	}

	auto VecToObj = [](const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("distance"), Distance);
	Data->SetStringField(TEXT("coordinate_space"),
		CoordSpace == ESplineCoordinateSpace::World ? TEXT("world") : TEXT("local"));

	const FVector Location = Spline->GetLocationAtDistanceAlongSpline(Distance, CoordSpace);
	Data->SetObjectField(TEXT("location"), VecToObj(Location));

	if (bIncludeTangent)
	{
		const FVector Tangent = Spline->GetTangentAtDistanceAlongSpline(Distance, CoordSpace);
		Data->SetObjectField(TEXT("tangent"), VecToObj(Tangent));
	}

	if (bIncludeNormal)
	{
		// Right vector is exposed as the "normal" consistent with USplineComponent API usage.
		const FVector RightVec = Spline->GetRightVectorAtDistanceAlongSpline(Distance, CoordSpace);
		Data->SetObjectField(TEXT("normal"), VecToObj(RightVec));
	}

	if (bIncludeRotation)
	{
		const FRotator Rot = Spline->GetRotationAtDistanceAlongSpline(Distance, CoordSpace);
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
		Data->SetObjectField(TEXT("rotation"), RotObj);
	}

	return MakeSuccess(SPLINE_DOMAIN, Action,
		FString::Printf(TEXT("Spline '%s' @ distance=%.2f -> (%.1f, %.1f, %.1f)"),
			*ActorLabel, Distance, Location.X, Location.Y, Location.Z),
		Data);
#else
	return MakeError(SPLINE_DOMAIN, Action, 3003, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> USplineHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a new actor with a USplineComponent and optional initial points"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_name"), P(TEXT("string"), true, TEXT("Label and internal name for the new actor"))); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), false, TEXT("World location"))); Pr->SetObjectField(TEXT("points"), P(TEXT("array<{x,y,z}>"), false, TEXT("Initial spline points in local space"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_spline_actor"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a point to an existing spline"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("position"), P(TEXT("object {x,y,z}"), true, TEXT("Point position in local space"))); Pr->SetObjectField(TEXT("index"), P(TEXT("int"), false, TEXT("Insert index (-1 or omit to append)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_point"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Move an existing spline point to a new position"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("index"), P(TEXT("int"), true, TEXT("Point index"))); Pr->SetObjectField(TEXT("position"), P(TEXT("object {x,y,z}"), true, TEXT("New position"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_point"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set arrive and leave tangents at a spline point"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("index"), P(TEXT("int"), true, TEXT("Point index"))); Pr->SetObjectField(TEXT("arrive_tangent"), P(TEXT("object {x,y,z}"), true, TEXT("Arrive tangent"))); Pr->SetObjectField(TEXT("leave_tangent"), P(TEXT("object {x,y,z}"), true, TEXT("Leave tangent"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_tangent"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return all spline points with positions, tangents, types, closed status, and total length"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_points"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set whether the spline forms a closed loop"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("closed"), P(TEXT("bool"), false, TEXT("Close the loop (default true)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("close_loop"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the interpolation type at a spline point"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("index"), P(TEXT("int"), true, TEXT("Point index"))); Pr->SetObjectField(TEXT("type"), P(TEXT("string"), true, TEXT("Linear, Curve, Constant, or CurveClamped"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_point_type"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return total arc length, control point count, and closed-loop state of a spline component"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Label of the spline actor"))); Pr->SetObjectField(TEXT("component_name"), P(TEXT("string"), false, TEXT("Named spline component (omit to use the first one on the actor)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_length"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Sample a spline at a given arc-length distance; returns location plus optional tangent/normal/rotation"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
      Pr->SetObjectField(TEXT("actor_label"),      P(TEXT("string"), true,  TEXT("Label of the spline actor")));
      Pr->SetObjectField(TEXT("component_name"),   P(TEXT("string"), false, TEXT("Named spline component (omit to use the first one on the actor)")));
      Pr->SetObjectField(TEXT("distance"),         P(TEXT("float"),  true,  TEXT("Arc-length distance from spline start (0 .. GetSplineLength)")));
      Pr->SetObjectField(TEXT("coordinate_space"), P(TEXT("string"), false, TEXT("'world' (default) or 'local'")));
      Pr->SetObjectField(TEXT("include_tangent"),  P(TEXT("bool"),   false, TEXT("Include tangent in response (default true)")));
      Pr->SetObjectField(TEXT("include_normal"),   P(TEXT("bool"),   false, TEXT("Include right-vector as normal (default false)")));
      Pr->SetObjectField(TEXT("include_rotation"), P(TEXT("bool"),   false, TEXT("Include rotation {pitch,yaw,roll} (default false)")));
      A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_point_at_distance"), A); }

    return Root;
}
