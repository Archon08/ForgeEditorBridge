#include "Handlers/LightingHandler.h"
#include "ForgeAISubsystem.h"

// ---- Light types -----------------------------------------------------------
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"

// ---- Sky light -------------------------------------------------------------
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/TextureCube.h"

// ---- Fog -------------------------------------------------------------------
#include "Components/ExponentialHeightFogComponent.h"

// ---- CineCamera ------------------------------------------------------------
#include "CineCameraActor.h"
#include "CineCameraComponent.h"

// ---- World / editor --------------------------------------------------------
#include "Editor.h"                 // GEditor
#include "EngineUtils.h"            // TActorIterator

#if WITH_EDITOR
#include "EditorBuildUtils.h"       // FEditorBuildUtils
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("lighting");

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void ULightingHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("lighting"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_light"))          return Action_CreateLight(Params);
	if (Action == TEXT("set_light_intensity"))   return Action_SetLightIntensity(Params);
	if (Action == TEXT("create_camera"))         return Action_CreateCamera(Params);
	if (Action == TEXT("set_light_color"))       return Action_SetLightColor(Params);
	if (Action == TEXT("set_light_attenuation")) return Action_SetLightAttenuation(Params);
	if (Action == TEXT("create_sky_light"))      return Action_CreateSkyLight(Params);
	if (Action == TEXT("set_volumetric_fog"))    return Action_SetVolumetricFog(Params);
	if (Action == TEXT("bake_lighting"))         return Action_BakeLighting(Params);
	if (Action == TEXT("delete_light"))          return Action_DeleteLight(Params);
	if (Action == TEXT("list_lights"))           return Action_ListLights(Params);
	if (Action == TEXT("delete_sky_light"))      return Action_DeleteLight(Params);  // alias — DeleteLight handles any ALight

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown lighting action '%s'"), *Action),
		TEXT("system/capabilities"));
}

// ---------------------------------------------------------------------------
// create_light
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_CreateLight(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("lighting"), TEXT("create_light"));

	FString LightType = TEXT("point");
	FString Label;
	double X = 0.0, Y = 0.0, Z = 0.0, Pitch = 0.0, Yaw = 0.0, Roll = 0.0, Intensity = 1.0;

	Params->TryGetStringField(TEXT("light_type"), LightType);
	Params->TryGetStringField(TEXT("label"),      Label);
	Params->TryGetNumberField(TEXT("x"),          X);
	Params->TryGetNumberField(TEXT("y"),          Y);
	Params->TryGetNumberField(TEXT("z"),          Z);
	Params->TryGetNumberField(TEXT("pitch"),      Pitch);
	Params->TryGetNumberField(TEXT("yaw"),        Yaw);
	Params->TryGetNumberField(TEXT("roll"),       Roll);
	Params->TryGetNumberField(TEXT("intensity"),  Intensity);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("create_light: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));
	SpawnTransform.SetRotation(FQuat(FRotator((float)Pitch, (float)Yaw, (float)Roll)));

	AActor* SpawnedActor = nullptr;

	if (LightType == TEXT("directional"))
	{
		ADirectionalLight* L = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(), SpawnTransform);
		if (L) { L->GetLightComponent()->Intensity = (float)Intensity; SpawnedActor = L; }
	}
	else if (LightType == TEXT("spot"))
	{
		ASpotLight* L = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), SpawnTransform);
		if (L) { L->GetLightComponent()->Intensity = (float)Intensity; SpawnedActor = L; }
	}
	else if (LightType == TEXT("rect"))
	{
		ARectLight* L = World->SpawnActor<ARectLight>(ARectLight::StaticClass(), SpawnTransform);
		if (L) { L->GetLightComponent()->Intensity = (float)Intensity; SpawnedActor = L; }
	}
	else // "point" — default
	{
		APointLight* L = World->SpawnActor<APointLight>(APointLight::StaticClass(), SpawnTransform);
		if (L) { L->GetLightComponent()->Intensity = (float)Intensity; SpawnedActor = L; }
	}

	if (!SpawnedActor)
	{
		Result.Message = FString::Printf(
			TEXT("create_light: failed to spawn %s light at (%.0f, %.0f, %.0f)"),
			*LightType, (float)X, (float)Y, (float)Z);
		Result.ErrorCode = 3000;
		return Result;
	}

	if (!Label.IsEmpty())
	{
		SpawnedActor->SetActorLabel(Label);
	}

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("%s light '%s' spawned at (%.0f, %.0f, %.0f) intensity=%.2f"),
		*LightType, *SpawnedActor->GetActorLabel(), (float)X, (float)Y, (float)Z, (float)Intensity);
	return Result;
}

// ---------------------------------------------------------------------------
// set_light_intensity
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_SetLightIntensity(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("lighting"), TEXT("set_light_intensity"));

	FString Label;
	double Intensity = 1.0;
	if (!Params->TryGetStringField(TEXT("label"),     Label)     || Label.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("intensity"), Intensity))
	{
		Result.Message = TEXT("set_light_intensity: 'label' and 'intensity' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("set_light_intensity: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	ALight* FoundLight = nullptr;
	for (TActorIterator<ALight> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			FoundLight = *It;
			break;
		}
	}

	if (!FoundLight)
	{
		Result.Message = FString::Printf(
			TEXT("set_light_intensity: no ALight actor found with label '%s' in the current level"),
			*Label);
		Result.ErrorCode = 2000;
		return Result;
	}

	FoundLight->GetLightComponent()->Intensity = (float)Intensity;
	FoundLight->MarkPackageDirty();

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("Light '%s' intensity set to %.2f"), *Label, (float)Intensity);
	return Result;
}

// ---------------------------------------------------------------------------
// create_camera
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_CreateCamera(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("lighting"), TEXT("create_camera"));

	FString Label;
	double X = 0.0, Y = 0.0, Z = 0.0, Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	double FocalLength = 35.0, Aperture = 2.8;

	Params->TryGetStringField(TEXT("label"),        Label);
	Params->TryGetNumberField(TEXT("x"),            X);
	Params->TryGetNumberField(TEXT("y"),            Y);
	Params->TryGetNumberField(TEXT("z"),            Z);
	Params->TryGetNumberField(TEXT("pitch"),        Pitch);
	Params->TryGetNumberField(TEXT("yaw"),          Yaw);
	Params->TryGetNumberField(TEXT("roll"),         Roll);
	Params->TryGetNumberField(TEXT("focal_length"), FocalLength);
	Params->TryGetNumberField(TEXT("aperture"),     Aperture);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("create_camera: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));
	SpawnTransform.SetRotation(FQuat(FRotator((float)Pitch, (float)Yaw, (float)Roll)));

	ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(
		ACineCameraActor::StaticClass(), SpawnTransform);
	if (!Camera)
	{
		Result.Message = FString::Printf(
			TEXT("create_camera: failed to spawn ACineCameraActor at (%.0f, %.0f, %.0f)"),
			(float)X, (float)Y, (float)Z);
		Result.ErrorCode = 3000;
		return Result;
	}

	if (!Label.IsEmpty())
	{
		Camera->SetActorLabel(Label);
	}

	if (UCineCameraComponent* CineComp = Camera->GetCineCameraComponent())
	{
		CineComp->CurrentFocalLength = (float)FocalLength;
		CineComp->CurrentAperture    = (float)Aperture;
	}

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("CineCamera '%s' created at (%.0f, %.0f, %.0f) focal=%.1fmm f/%.1f"),
		*Camera->GetActorLabel(), (float)X, (float)Y, (float)Z,
		(float)FocalLength, (float)Aperture);
	return Result;
}

// ---------------------------------------------------------------------------
// Helper: find actor by label
// ---------------------------------------------------------------------------

AActor* ULightingHandler::FindActorByLabel(UWorld* World, const FString& Label) const
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
// set_light_color
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_SetLightColor(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_light_color");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("color"), ColorObj) || !ColorObj || !(*ColorObj).IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'color' {r,g,b}"));
	}

	double R = 1.0, G = 1.0, B = 1.0;
	(*ColorObj)->TryGetNumberField(TEXT("r"), R);
	(*ColorObj)->TryGetNumberField(TEXT("g"), G);
	(*ColorObj)->TryGetNumberField(TEXT("b"), B);

	bool bSRGB = true;
	Params->TryGetBoolField(TEXT("srgb"), bSRGB);

#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
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

	ULightComponent* LightComp = Actor->FindComponentByClass<ULightComponent>();
	if (!LightComp)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' has no ULightComponent"), *ActorPath));
	}

	LightComp->SetLightColor(FLinearColor((float)R, (float)G, (float)B), bSRGB);
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("r"), R);
	Data->SetNumberField(TEXT("g"), G);
	Data->SetNumberField(TEXT("b"), B);
	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Light '%s' color set to (%.2f, %.2f, %.2f) sRGB=%s"),
			*ActorPath, (float)R, (float)G, (float)B, bSRGB ? TEXT("true") : TEXT("false")),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_light_attenuation
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_SetLightAttenuation(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_light_attenuation");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double Radius = 0.0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius))
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'radius'"));
	}

#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
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

	// Try point light first, then spot light
	UPointLightComponent* PointComp = Actor->FindComponentByClass<UPointLightComponent>();
	if (PointComp)
	{
		PointComp->SetAttenuationRadius((float)Radius);
		Actor->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("PointLight '%s' attenuation radius set to %.1f"), *ActorPath, (float)Radius));
	}

	USpotLightComponent* SpotComp = Actor->FindComponentByClass<USpotLightComponent>();
	if (SpotComp)
	{
		SpotComp->SetAttenuationRadius((float)Radius);
		Actor->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("SpotLight '%s' attenuation radius set to %.1f"), *ActorPath, (float)Radius));
	}

	return MakeError(DOMAIN, Action, 2001,
		FString::Printf(TEXT("Actor '%s' has no UPointLightComponent or USpotLightComponent"), *ActorPath),
		TEXT("Only point and spot lights support attenuation radius"));
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// create_sky_light
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_CreateSkyLight(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_sky_light");

#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	double X = 0.0, Y = 0.0, Z = 0.0, Intensity = 1.0;
	FString Label, CubemapPath;

	Params->TryGetNumberField(TEXT("x"),         X);
	Params->TryGetNumberField(TEXT("y"),         Y);
	Params->TryGetNumberField(TEXT("z"),         Z);
	Params->TryGetNumberField(TEXT("intensity"), Intensity);
	Params->TryGetStringField(TEXT("label"),     Label);
	Params->TryGetStringField(TEXT("cubemap"),   CubemapPath);

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));

	ASkyLight* SkyLight = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), SpawnTransform);
	if (!SkyLight)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to spawn ASkyLight at (%.0f, %.0f, %.0f)"),
				(float)X, (float)Y, (float)Z));
	}

	if (!Label.IsEmpty())
	{
		SkyLight->SetActorLabel(Label);
	}

	USkyLightComponent* SkyComp = SkyLight->GetLightComponent();
	if (SkyComp)
	{
		SkyComp->Intensity = (float)Intensity;

		if (!CubemapPath.IsEmpty())
		{
			UTextureCube* Cubemap = LoadObject<UTextureCube>(nullptr, *CubemapPath);
			if (Cubemap)
			{
				SkyComp->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
				SkyComp->Cubemap = Cubemap;
			}
			else
			{
				// Non-fatal: sky light created but cubemap not found
				SkyComp->SourceType = ESkyLightSourceType::SLS_CapturedScene;
			}
		}

		SkyComp->MarkRenderStateDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), SkyLight->GetActorLabel());
	Data->SetNumberField(TEXT("intensity"), Intensity);
	if (!CubemapPath.IsEmpty())
	{
		Data->SetStringField(TEXT("cubemap"), CubemapPath);
		Data->SetBoolField(TEXT("cubemap_loaded"),
			SkyComp && SkyComp->SourceType == ESkyLightSourceType::SLS_SpecifiedCubemap);
	}

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("SkyLight '%s' created at (%.0f, %.0f, %.0f) intensity=%.2f"),
			*SkyLight->GetActorLabel(), (float)X, (float)Y, (float)Z, (float)Intensity),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_volumetric_fog
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_SetVolumetricFog(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_volumetric_fog");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
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

	UExponentialHeightFogComponent* FogComp = Actor->FindComponentByClass<UExponentialHeightFogComponent>();
	if (!FogComp)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' has no UExponentialHeightFogComponent"), *ActorPath),
			TEXT("Target must be an ExponentialHeightFog actor"));
	}

	bool bEnabled = true;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		FogComp->SetVolumetricFog(bEnabled);
	}

	double ScatteringDistribution = 0.0;
	if (Params->TryGetNumberField(TEXT("scattering_distribution"), ScatteringDistribution))
	{
		FogComp->VolumetricFogScatteringDistribution = (float)ScatteringDistribution;
	}

	const TSharedPtr<FJsonObject>* AlbedoObj = nullptr;
	if (Params->TryGetObjectField(TEXT("albedo"), AlbedoObj) && AlbedoObj && (*AlbedoObj).IsValid())
	{
		double R = 1.0, G = 1.0, B = 1.0;
		(*AlbedoObj)->TryGetNumberField(TEXT("r"), R);
		(*AlbedoObj)->TryGetNumberField(TEXT("g"), G);
		(*AlbedoObj)->TryGetNumberField(TEXT("b"), B);
		FogComp->VolumetricFogAlbedo = FColor(
			(uint8)FMath::Clamp((int32)(R * 255.0), 0, 255),
			(uint8)FMath::Clamp((int32)(G * 255.0), 0, 255),
			(uint8)FMath::Clamp((int32)(B * 255.0), 0, 255));
	}

	double ExtinctionScale = 0.0;
	if (Params->TryGetNumberField(TEXT("extinction_scale"), ExtinctionScale))
	{
		FogComp->VolumetricFogExtinctionScale = (float)ExtinctionScale;
	}

	FogComp->MarkRenderStateDirty();
	Actor->MarkPackageDirty();

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Volumetric fog updated on '%s'"), *ActorPath));
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// bake_lighting
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_BakeLighting(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("bake_lighting");

#if WITH_EDITOR
	if (!GEditor)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("GEditor not available"));
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	FString Quality = TEXT("Preview");
	Params->TryGetStringField(TEXT("quality"), Quality);

	// Note: Build quality is controlled by Lightmass world settings, not EditorBuild params directly.
	// The quality param is informational; actual quality depends on the level's Lightmass settings.
	// We trigger the build via FEditorBuildUtils.

	// UE 5.7: EditorBuild takes FName from FBuildOptions
	bool bSuccess = FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting);

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("quality_hint"), Quality);
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Lighting build initiated (quality hint: %s)"), *Quality), Data);
	}
	else
	{
		return MakeError(DOMAIN, Action, 3000,
			TEXT("Lighting build failed — check Output Log for details"),
			TEXT("Check output log for Lightmass errors"));
	}
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

TSharedPtr<FJsonObject> ULightingHandler::GetActionSchemas() const
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

	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Spawn a light actor in the current level"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("light_type"), P(TEXT("string"), false, TEXT("directional, point, spot, or rect (default point)")));
		Params->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Actor label")));
		Params->SetObjectField(TEXT("x"), P(TEXT("float"), false, TEXT("Location X")));
		Params->SetObjectField(TEXT("y"), P(TEXT("float"), false, TEXT("Location Y")));
		Params->SetObjectField(TEXT("z"), P(TEXT("float"), false, TEXT("Location Z")));
		Params->SetObjectField(TEXT("pitch"), P(TEXT("float"), false, TEXT("Rotation pitch")));
		Params->SetObjectField(TEXT("yaw"), P(TEXT("float"), false, TEXT("Rotation yaw")));
		Params->SetObjectField(TEXT("roll"), P(TEXT("float"), false, TEXT("Rotation roll")));
		Params->SetObjectField(TEXT("intensity"), P(TEXT("float"), false, TEXT("Light intensity (default 1.0)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("create_light"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the intensity of a light actor by label"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("label"), P(TEXT("string"), true, TEXT("Actor label of the light")));
		Params->SetObjectField(TEXT("intensity"), P(TEXT("float"), true, TEXT("New intensity value")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_light_intensity"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Spawn a CineCameraActor"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Actor label")));
		Params->SetObjectField(TEXT("x"), P(TEXT("float"), false, TEXT("Location X")));
		Params->SetObjectField(TEXT("y"), P(TEXT("float"), false, TEXT("Location Y")));
		Params->SetObjectField(TEXT("z"), P(TEXT("float"), false, TEXT("Location Z")));
		Params->SetObjectField(TEXT("pitch"), P(TEXT("float"), false, TEXT("Rotation pitch")));
		Params->SetObjectField(TEXT("yaw"), P(TEXT("float"), false, TEXT("Rotation yaw")));
		Params->SetObjectField(TEXT("roll"), P(TEXT("float"), false, TEXT("Rotation roll")));
		Params->SetObjectField(TEXT("focal_length"), P(TEXT("float"), false, TEXT("Focal length in mm (default 35)")));
		Params->SetObjectField(TEXT("aperture"), P(TEXT("float"), false, TEXT("Aperture f-stop (default 2.8)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("create_camera"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the color of a light actor"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the light")));
		Params->SetObjectField(TEXT("color"), P(TEXT("object"), true, TEXT("Color {r,g,b} as 0-1 linear floats")));
		Params->SetObjectField(TEXT("srgb"), P(TEXT("bool"), false, TEXT("Interpret as sRGB (default true)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_light_color"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set attenuation radius on a point or spot light"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the light")));
		Params->SetObjectField(TEXT("radius"), P(TEXT("float"), true, TEXT("Attenuation radius")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_light_attenuation"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Spawn a SkyLight actor"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("x"), P(TEXT("float"), false, TEXT("Location X")));
		Params->SetObjectField(TEXT("y"), P(TEXT("float"), false, TEXT("Location Y")));
		Params->SetObjectField(TEXT("z"), P(TEXT("float"), false, TEXT("Location Z")));
		Params->SetObjectField(TEXT("intensity"), P(TEXT("float"), false, TEXT("Sky light intensity (default 1.0)")));
		Params->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Actor label")));
		Params->SetObjectField(TEXT("cubemap"), P(TEXT("string"), false, TEXT("Cubemap asset path (optional)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("create_sky_light"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Configure volumetric fog on an ExponentialHeightFog actor"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the fog actor")));
		Params->SetObjectField(TEXT("enabled"), P(TEXT("bool"), false, TEXT("Enable volumetric fog (default true)")));
		Params->SetObjectField(TEXT("scattering_distribution"), P(TEXT("float"), false, TEXT("Scattering distribution")));
		Params->SetObjectField(TEXT("albedo"), P(TEXT("object"), false, TEXT("Fog albedo {r,g,b} as 0-1 floats")));
		Params->SetObjectField(TEXT("extinction_scale"), P(TEXT("float"), false, TEXT("Extinction scale")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_volumetric_fog"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Trigger a Lightmass lighting build"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("quality"), P(TEXT("string"), false, TEXT("Quality hint: Preview, Medium, High, Production (default Preview)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("bake_lighting"), A);
	}

	return Root;
}

// ---------------------------------------------------------------------------
// delete_light
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_DeleteLight(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("delete_light");

	FString Label;
	if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("'label' is required"));
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	// Search all ALight subclasses by actor label
	AActor* Found = nullptr;
	for (TActorIterator<ALight> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			Found = *It;
			break;
		}
	}
	if (!Found)
	{
		// Also check sky lights
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == Label)
			{
				Found = *It;
				break;
			}
		}
	}

	if (!Found)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No light actor found with label '%s'"), *Label));
	}

	World->DestroyActor(Found);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Deleted light actor '%s'"), *Label));
}

// ---------------------------------------------------------------------------
// list_lights
// ---------------------------------------------------------------------------

FBridgeResult ULightingHandler::Action_ListLights(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("list_lights");

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> LightsArr;

	auto AddLight = [&](AActor* Actor, const FString& Type)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Entry->SetStringField(TEXT("type"),  Type);
		FVector Loc = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		Entry->SetObjectField(TEXT("location"), LocObj);

		if (ULightComponent* LC = Actor->FindComponentByClass<ULightComponent>())
		{
			Entry->SetNumberField(TEXT("intensity"), LC->Intensity);
			FLinearColor C = LC->GetLightColor();
			TSharedPtr<FJsonObject> Col = MakeShared<FJsonObject>();
			Col->SetNumberField(TEXT("r"), C.R);
			Col->SetNumberField(TEXT("g"), C.G);
			Col->SetNumberField(TEXT("b"), C.B);
			Entry->SetObjectField(TEXT("color"), Col);
			Entry->SetBoolField(TEXT("visible"), LC->IsVisible());
		}
		LightsArr.Add(MakeShared<FJsonValueObject>(Entry));
	};

	for (TActorIterator<ADirectionalLight> It(World); It; ++It) AddLight(*It, TEXT("Directional"));
	for (TActorIterator<APointLight>       It(World); It; ++It) AddLight(*It, TEXT("Point"));
	for (TActorIterator<ASpotLight>        It(World); It; ++It) AddLight(*It, TEXT("Spot"));
	for (TActorIterator<ARectLight>        It(World); It; ++It) AddLight(*It, TEXT("Rect"));
	for (TActorIterator<ASkyLight>         It(World); It; ++It)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"), (*It)->GetActorLabel());
		Entry->SetStringField(TEXT("type"),  TEXT("Sky"));
		LightsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("lights"), LightsArr);
	Data->SetNumberField(TEXT("count"), LightsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("list_lights: %d light(s) in level"), LightsArr.Num()));
	R.ExtraData = OutStr;
	return R;
}
