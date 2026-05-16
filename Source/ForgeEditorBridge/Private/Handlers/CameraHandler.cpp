#include "Handlers/CameraHandler.h"
#include "ForgeAISubsystem.h"

// ---- Camera actors ---------------------------------------------------------
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "CameraRig_Rail.h"
#include "CameraRig_Crane.h"
#include "Components/SplineComponent.h"

// ---- World / editor --------------------------------------------------------
#include "EngineUtils.h"            // TActorIterator

#if WITH_EDITOR
#include "Editor.h"                 // GEditor
#include "ScopedTransaction.h"
#include "Camera/CameraShakeBase.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("camera");

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("create_cinecam"))    return Action_CreateCineCam(Params);
	if (Action == TEXT("set_filmback"))      return Action_SetFilmback(Params);
	if (Action == TEXT("set_focus"))         return Action_SetFocus(Params);
	if (Action == TEXT("set_look_at"))       return Action_SetLookAt(Params);
	if (Action == TEXT("create_camera_rig")) return Action_CreateCameraRig(Params);
	if (Action == TEXT("get_camera_info"))     return Action_GetCameraInfo(Params);
	if (Action == TEXT("list_cameras"))        return Action_ListCameras(Params);
	if (Action == TEXT("create_camera_shake")) return Action_CreateCameraShake(Params);
	if (Action == TEXT("preview_shake"))       return Action_PreviewShake(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown camera action '%s'"), *Action),
		TEXT("camera capabilities"));
}

// ---------------------------------------------------------------------------
// Helper: find actor by label
// ---------------------------------------------------------------------------

AActor* UCameraHandler::FindActorByLabel(UWorld* World, const FString& Label) const
{
	if (!World || Label.IsEmpty()) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			return *It;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// create_cinecam
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_CreateCineCam(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_cinecam");

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	// Parse location (required)
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("location"), LocationObj) || !LocationObj || !(*LocationObj).IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'location' {x,y,z}"));
	}

	double X = 0.0, Y = 0.0, Z = 0.0;
	(*LocationObj)->TryGetNumberField(TEXT("x"), X);
	(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
	(*LocationObj)->TryGetNumberField(TEXT("z"), Z);

	// Parse rotation (optional)
	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj).IsValid())
	{
		(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
		(*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
	}

	double FocalLength = 35.0;
	Params->TryGetNumberField(TEXT("focal_length"), FocalLength);

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create CineCameraActor")));

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));
	SpawnTransform.SetRotation(FQuat(FRotator((float)Pitch, (float)Yaw, (float)Roll)));

	ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(
		ACineCameraActor::StaticClass(), SpawnTransform);
	if (!Camera)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to spawn ACineCameraActor at (%.0f, %.0f, %.0f)"),
				(float)X, (float)Y, (float)Z));
	}

	if (UCineCameraComponent* CineComp = Camera->GetCineCameraComponent())
	{
		CineComp->CurrentFocalLength = (float)FocalLength;
	}

	Camera->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Camera->GetActorLabel());
	Data->SetNumberField(TEXT("focal_length"), FocalLength);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("CineCameraActor '%s' created at (%.0f, %.0f, %.0f) focal=%.1fmm"),
			*Camera->GetActorLabel(), (float)X, (float)Y, (float)Z, (float)FocalLength),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_filmback
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_SetFilmback(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_filmback");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	FString Preset;
	if (!Params->TryGetStringField(TEXT("preset"), Preset) || Preset.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'preset'"));
	}

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	AActor* Actor = FindActorByLabel(World, ActorPath);
	if (!Actor)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorPath));
	}

	ACineCameraActor* CineCam = Cast<ACineCameraActor>(Actor);
	if (!CineCam)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an ACineCameraActor"), *ActorPath));
	}

	UCineCameraComponent* CineComp = CineCam->GetCineCameraComponent();
	if (!CineComp)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("CineCameraComponent not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Filmback")));

	if (Preset == TEXT("Custom"))
	{
		double SensorWidth = 24.89, SensorHeight = 18.67;
		Params->TryGetNumberField(TEXT("sensor_width"), SensorWidth);
		Params->TryGetNumberField(TEXT("sensor_height"), SensorHeight);

		FCameraFilmbackSettings Filmback;
		Filmback.SensorWidth = (float)SensorWidth;
		Filmback.SensorHeight = (float)SensorHeight;
		CineComp->Filmback = Filmback;

		CineCam->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("preset"), TEXT("Custom"));
		Data->SetNumberField(TEXT("sensor_width"), SensorWidth);
		Data->SetNumberField(TEXT("sensor_height"), SensorHeight);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Filmback on '%s' set to Custom (%.2f x %.2f mm)"),
				*ActorPath, (float)SensorWidth, (float)SensorHeight),
			Data);
	}
	else
	{
		// Use preset name lookup
		CineComp->SetFilmbackPresetByName(Preset);

		CineCam->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("preset"), Preset);
		Data->SetNumberField(TEXT("sensor_width"), CineComp->Filmback.SensorWidth);
		Data->SetNumberField(TEXT("sensor_height"), CineComp->Filmback.SensorHeight);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Filmback on '%s' set to '%s' (%.2f x %.2f mm)"),
				*ActorPath, *Preset,
				CineComp->Filmback.SensorWidth, CineComp->Filmback.SensorHeight),
			Data);
	}
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_focus
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_SetFocus(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_focus");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double Distance = 0.0;
	if (!Params->TryGetNumberField(TEXT("distance"), Distance))
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'distance'"));
	}

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	AActor* Actor = FindActorByLabel(World, ActorPath);
	if (!Actor)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorPath));
	}

	ACineCameraActor* CineCam = Cast<ACineCameraActor>(Actor);
	if (!CineCam)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an ACineCameraActor"), *ActorPath));
	}

	UCineCameraComponent* CineComp = CineCam->GetCineCameraComponent();
	if (!CineComp)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("CineCameraComponent not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Focus")));

	// Method (optional, default Manual)
	FString MethodStr = TEXT("Manual");
	Params->TryGetStringField(TEXT("method"), MethodStr);

	if (MethodStr == TEXT("Tracking"))
	{
		CineComp->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
	}
	else
	{
		CineComp->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	}

	CineComp->FocusSettings.ManualFocusDistance = (float)Distance;

	// Aperture (optional)
	double Aperture = 0.0;
	if (Params->TryGetNumberField(TEXT("aperture"), Aperture))
	{
		CineComp->CurrentAperture = (float)Aperture;
	}

	CineCam->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("method"), MethodStr);
	Data->SetNumberField(TEXT("distance"), Distance);
	if (Aperture > 0.0) Data->SetNumberField(TEXT("aperture"), Aperture);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Focus on '%s' set: method=%s distance=%.1f"),
			*ActorPath, *MethodStr, (float)Distance),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_look_at
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_SetLookAt(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_look_at");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	const TSharedPtr<FJsonObject>* TargetObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("target"), TargetObj) || !TargetObj || !(*TargetObj).IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'target' {x,y,z}"));
	}

	double TX = 0.0, TY = 0.0, TZ = 0.0;
	(*TargetObj)->TryGetNumberField(TEXT("x"), TX);
	(*TargetObj)->TryGetNumberField(TEXT("y"), TY);
	(*TargetObj)->TryGetNumberField(TEXT("z"), TZ);

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	AActor* Actor = FindActorByLabel(World, ActorPath);
	if (!Actor)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Look At")));

	FVector CameraLocation = Actor->GetActorLocation();
	FVector TargetLocation((float)TX, (float)TY, (float)TZ);
	FVector Direction = (TargetLocation - CameraLocation).GetSafeNormal();

	if (Direction.IsNearlyZero())
	{
		return MakeError(DOMAIN, Action, 1001,
			TEXT("Target location is the same as camera location — cannot compute look-at rotation"));
	}

	FRotator LookAtRotation = Direction.Rotation();
	Actor->SetActorRotation(LookAtRotation);
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("pitch"), LookAtRotation.Pitch);
	Data->SetNumberField(TEXT("yaw"), LookAtRotation.Yaw);
	Data->SetNumberField(TEXT("roll"), LookAtRotation.Roll);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Camera '%s' now looking at (%.0f, %.0f, %.0f) — rotation (P=%.1f Y=%.1f R=%.1f)"),
			*ActorPath, (float)TX, (float)TY, (float)TZ,
			LookAtRotation.Pitch, LookAtRotation.Yaw, LookAtRotation.Roll),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// create_camera_rig
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_CreateCameraRig(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_camera_rig");

	FString Type;
	if (!Params->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'type' (\"rail\" or \"crane\")"));
	}

	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("location"), LocationObj) || !LocationObj || !(*LocationObj).IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'location' {x,y,z}"));
	}

	double X = 0.0, Y = 0.0, Z = 0.0;
	(*LocationObj)->TryGetNumberField(TEXT("x"), X);
	(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
	(*LocationObj)->TryGetNumberField(TEXT("z"), Z);

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Camera Rig")));

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));

	if (Type == TEXT("rail"))
	{
		ACameraRig_Rail* Rail = World->SpawnActor<ACameraRig_Rail>(
			ACameraRig_Rail::StaticClass(), SpawnTransform);
		if (!Rail)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to spawn ACameraRig_Rail"));
		}

		double RailLength = 500.0;
		Params->TryGetNumberField(TEXT("rail_length"), RailLength);

		// CameraRig_Rail doesn't have a direct "length" setter — the rail is a spline.
		// We set the second point of the spline to define the rail length along X.
		if (USplineComponent* Spline = Rail->GetRailSplineComponent())
		{
			Spline->SetLocationAtSplinePoint(1, FVector((float)RailLength, 0.0f, 0.0f), ESplineCoordinateSpace::Local);
			Spline->UpdateSpline();
		}

		Rail->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("type"), TEXT("rail"));
		Data->SetStringField(TEXT("label"), Rail->GetActorLabel());
		Data->SetNumberField(TEXT("rail_length"), RailLength);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("CameraRig_Rail '%s' created at (%.0f, %.0f, %.0f) length=%.0f"),
				*Rail->GetActorLabel(), (float)X, (float)Y, (float)Z, (float)RailLength),
			Data);
	}
	else if (Type == TEXT("crane"))
	{
		ACameraRig_Crane* Crane = World->SpawnActor<ACameraRig_Crane>(
			ACameraRig_Crane::StaticClass(), SpawnTransform);
		if (!Crane)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to spawn ACameraRig_Crane"));
		}

		double CraneLength = 500.0;
		Params->TryGetNumberField(TEXT("crane_length"), CraneLength);

		Crane->CraneArmLength = (float)CraneLength;
		Crane->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("type"), TEXT("crane"));
		Data->SetStringField(TEXT("label"), Crane->GetActorLabel());
		Data->SetNumberField(TEXT("crane_length"), CraneLength);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("CameraRig_Crane '%s' created at (%.0f, %.0f, %.0f) arm=%.0f"),
				*Crane->GetActorLabel(), (float)X, (float)Y, (float)Z, (float)CraneLength),
			Data);
	}
	else
	{
		return MakeError(DOMAIN, Action, 1001,
			FString::Printf(TEXT("Unknown rig type '%s' — expected 'rail' or 'crane'"), *Type));
	}
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UCameraHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a CineCameraActor in the editor world"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), true, TEXT("World location"))); Pr->SetObjectField(TEXT("rotation"), P(TEXT("object {pitch,yaw,roll}"), false, TEXT("Initial rotation"))); Pr->SetObjectField(TEXT("focal_length"), P(TEXT("float"), false, TEXT("Focal length in mm (default 35)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_cinecam"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the filmback preset on a CineCameraActor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Camera actor label"))); Pr->SetObjectField(TEXT("preset"), P(TEXT("string"), true, TEXT("Super16, Super35, IMAX, VistaVision, or Custom"))); Pr->SetObjectField(TEXT("sensor_width"), P(TEXT("float"), false, TEXT("Sensor width in mm (for Custom)"))); Pr->SetObjectField(TEXT("sensor_height"), P(TEXT("float"), false, TEXT("Sensor height in mm (for Custom)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_filmback"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure focus settings on a CineCameraActor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Camera actor label"))); Pr->SetObjectField(TEXT("distance"), P(TEXT("float"), true, TEXT("Manual focus distance"))); Pr->SetObjectField(TEXT("method"), P(TEXT("string"), false, TEXT("Manual or Tracking (default Manual)"))); Pr->SetObjectField(TEXT("aperture"), P(TEXT("float"), false, TEXT("F-stop aperture value"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_focus"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Rotate a camera actor to look at a world position"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Camera actor label"))); Pr->SetObjectField(TEXT("target"), P(TEXT("object {x,y,z}"), true, TEXT("World position to look at"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_look_at"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a camera rig (rail or crane) in the editor world"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("type"), P(TEXT("string"), true, TEXT("rail or crane"))); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), true, TEXT("World location"))); Pr->SetObjectField(TEXT("rail_length"), P(TEXT("float"), false, TEXT("Rail spline length (for rail type)"))); Pr->SetObjectField(TEXT("crane_length"), P(TEXT("float"), false, TEXT("Crane arm length (for crane type)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_camera_rig"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a CameraShake Blueprint asset (UCameraShakeBase subclass)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Asset path for the new Blueprint (e.g. /Game/Shakes/MyShake)"))); Pr->SetObjectField(TEXT("shake_type"), P(TEXT("string"), false, TEXT("Parent class name: MatineeCameraShake (default)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_camera_shake"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Preview a camera shake on the editor viewport (Python-dispatched; shake preview is runtime-only in PIE)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Path to the CameraShake asset to preview"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("preview_shake"), A); }

    return Root;
}

// ---------------------------------------------------------------------------
// get_camera_info
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_GetCameraInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_camera_info");

#if WITH_EDITOR
	FString Label;
	if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("'label' is required"));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	ACineCameraActor* Cam = nullptr;
	for (TActorIterator<ACineCameraActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			Cam = *It;
			break;
		}
	}
	if (!Cam)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No ACineCameraActor found with label '%s'"), *Label));
	}

	UCineCameraComponent* CC = Cam->GetCineCameraComponent();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Label);

	FVector Loc = Cam->GetActorLocation();
	TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Data->SetObjectField(TEXT("location"), LocObj);

	FRotator Rot = Cam->GetActorRotation();
	TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
	Data->SetObjectField(TEXT("rotation"), RotObj);

	if (CC)
	{
		Data->SetNumberField(TEXT("focal_length"),     CC->CurrentFocalLength);
		Data->SetNumberField(TEXT("focus_distance"),   CC->FocusSettings.ManualFocusDistance);
		Data->SetNumberField(TEXT("aperture"),         CC->CurrentAperture);
		Data->SetNumberField(TEXT("sensor_width"),     CC->Filmback.SensorWidth);
		Data->SetNumberField(TEXT("sensor_height"),    CC->Filmback.SensorHeight);
		Data->SetNumberField(TEXT("horizontal_fov"),   CC->GetHorizontalFieldOfView());
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("get_camera_info: '%s' focal=%.1fmm"), *Label, CC ? CC->CurrentFocalLength : 0.f));
	R.ExtraData = OutStr;
	return R;
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// list_cameras
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_ListCameras(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("list_cameras");

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> CamsArr;
	for (TActorIterator<ACineCameraActor> It(World); It; ++It)
	{
		ACineCameraActor* Cam = *It;
		UCineCameraComponent* CC = Cam->GetCineCameraComponent();

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"), Cam->GetActorLabel());

		FVector Loc = Cam->GetActorLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		Entry->SetObjectField(TEXT("location"), LocObj);

		if (CC)
		{
			Entry->SetNumberField(TEXT("focal_length"), CC->CurrentFocalLength);
		}
		CamsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("cameras"), CamsArr);
	Data->SetNumberField(TEXT("count"), CamsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("list_cameras: %d camera(s) in level"), CamsArr.Num()));
	R.ExtraData = OutStr;
	return R;
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Editor-only operation"));
#endif
}

// ---------------------------------------------------------------------------
// create_camera_shake — create a UCameraShakeBase Blueprint asset
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_CreateCameraShake(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_camera_shake");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

#if WITH_EDITOR
	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// UCameraShakeBase is abstract — Blueprint subclass is the concrete type.
	// Default parent: UCameraShakeBase (let the Blueprint itself remain customizable).
	UClass* ParentClass = UCameraShakeBase::StaticClass();

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* Asset = ATModule.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);

	if (!Asset)
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("create_camera_shake: failed to create Blueprint at '%s'"), *AssetPath));

	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),   AssetPath);
	Data->SetStringField(TEXT("parent_class"), ParentClass->GetName());

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("CameraShake Blueprint '%s' created (parent: %s)"),
			*AssetName, *ParentClass->GetName()),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// preview_shake — Python dispatch (viewport camera shake is runtime-only)
// ---------------------------------------------------------------------------

FBridgeResult UCameraHandler::Action_PreviewShake(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("preview_shake");
	FBridgeResult Result = MakeError(DOMAIN, Action, 3004,
		TEXT("preview_shake: Editor viewport camera shake preview has no stable C++ API in UE 5.7. "
		     "UCameraModifier_CameraShake operates at runtime (PIE) only. "
		     "To test a shake: enter PIE and call "
		     "unreal.GameplayStatics.play_world_camera_shake(world, shake_class, location, inner, outer, 1.0, false) "
		     "via the Python console."),
		TEXT("Enter PIE, then use the Python console: unreal.GameplayStatics.play_world_camera_shake(...)"));
	return Result;
}
