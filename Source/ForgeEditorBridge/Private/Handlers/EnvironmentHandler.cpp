#include "Handlers/EnvironmentHandler.h"

// --- Sky Atmosphere ---
// ASkyAtmosphere actor header path varies by UE5 build; use component header +
// TActorIterator<AActor> + FindComponentByClass (same pattern as WeatherCapture).
#include "Components/SkyAtmosphereComponent.h"

// --- Volumetric Cloud ---
#include "Components/VolumetricCloudComponent.h"

// --- Fog ---
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"

// --- Sky + Directional light ---
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"

// --- Material (for cloud material path) ---
#include "Materials/MaterialInterface.h"

// --- World / editor ---
#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"

#if WITH_EDITOR
#include "Editor.h"
#include "ScopedTransaction.h"
#endif

// --- UObject property reflection (sky light cubemap) ---
#include "UObject/UnrealType.h"

// --- JSON ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("environment");

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

/** Find actor by label or return the first actor of a given class. */
template<typename T>
static T* FindOrFirstTyped(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	if (!Label.IsEmpty() && !Label.Equals(TEXT("first"), ESearchCase::IgnoreCase))
	{
		for (TActorIterator<T> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
				return *It;
		}
		return nullptr;
	}
	for (TActorIterator<T> It(World); It; ++It)
		return *It;
	return nullptr;
}

/** Find the first actor in the level carrying a component of type TComp. */
template<typename TComp>
static TComp* FindFirstComponent(UWorld* World, const FString& Label = FString())
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!Label.IsEmpty() && !Label.Equals(TEXT("first"), ESearchCase::IgnoreCase) &&
			!A->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) continue;
		if (TComp* C = A->FindComponentByClass<TComp>())
			return C;
	}
	return nullptr;
}

static TSharedPtr<FJsonObject> MakeColorObj(float R, float G, float B)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("r"), R);
	O->SetNumberField(TEXT("g"), G);
	O->SetNumberField(TEXT("b"), B);
	return O;
}

static bool ReadColorObj(TSharedPtr<FJsonObject> Params, const FString& Key,
	double& R, double& G, double& B)
{
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Params->TryGetObjectField(Key, Sub)) return false;
	(*Sub)->TryGetNumberField(TEXT("r"), R);
	(*Sub)->TryGetNumberField(TEXT("g"), G);
	(*Sub)->TryGetNumberField(TEXT("b"), B);
	return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// GetEditorWorld
// ---------------------------------------------------------------------------

UWorld* UEnvironmentHandler::GetEditorWorld()
{
	return GetEditorWorldLocal();
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("set_sky_atmosphere"))    return Action_SetSkyAtmosphere(Params);
	if (Action == TEXT("set_fog"))               return Action_SetFog(Params);
	if (Action == TEXT("set_volumetric_clouds")) return Action_SetVolumetricClouds(Params);
	if (Action == TEXT("set_sky_light"))         return Action_SetSkyLight(Params);
	if (Action == TEXT("apply_time_of_day"))     return Action_ApplyTimeOfDay(Params);
	if (Action == TEXT("get_environment_info"))  return Action_GetEnvironmentInfo(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown environment action '%s'. Valid: set_sky_atmosphere, set_fog, "
			"set_volumetric_clouds, set_sky_light, apply_time_of_day, get_environment_info"), *Action));
}

// ---------------------------------------------------------------------------
// set_sky_atmosphere
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_SetSkyAtmosphere(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_sky_atmosphere"), 3003, TEXT("No editor world available"));

	FString Label;
	Params->TryGetStringField(TEXT("actor_label"), Label);

	USkyAtmosphereComponent* AC = FindFirstComponent<USkyAtmosphereComponent>(World, Label);
	if (!AC)
		return MakeError(DOMAIN, TEXT("set_sky_atmosphere"), 2000,
			TEXT("No SkyAtmosphere actor found in the level"));

#if WITH_EDITOR
	auto Tx = BeginTransaction(TEXT("Set Sky Atmosphere"));
	AC->Modify();
#endif

	double V = 0.0;
	if (Params->TryGetNumberField(TEXT("rayleigh_scattering_scale"), V))
		AC->RayleighScatteringScale = (float)V;
	if (Params->TryGetNumberField(TEXT("mie_scattering_scale"), V))
		AC->MieScatteringScale = (float)V;
	if (Params->TryGetNumberField(TEXT("mie_absorption_scale"), V))
		AC->MieAbsorptionScale = (float)V;
	// AerialPerspectiveViewDistanceScale removed in UE 5.7 — property no longer exists.
	// if (Params->TryGetNumberField(TEXT("aerial_perspective_view_distance_scale"), V))
	//     AC->AerialPerspectiveViewDistanceScale = (float)V;

	double R = 1.0, G = 1.0, B = 1.0;
	if (ReadColorObj(Params, TEXT("sky_luminance_factor"), R, G, B))
		AC->SkyLuminanceFactor = FLinearColor((float)R, (float)G, (float)B);

#if WITH_EDITOR
	AC->PostEditChange();
	if (AActor* Owner = AC->GetOwner()) Owner->PostEditChange();
#endif

	return MakeSuccess(DOMAIN, TEXT("set_sky_atmosphere"), TEXT("SkyAtmosphere updated"));
}

// ---------------------------------------------------------------------------
// set_fog
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_SetFog(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_fog"), 3003, TEXT("No editor world available"));

	FString Label;
	Params->TryGetStringField(TEXT("actor_label"), Label);

	AExponentialHeightFog* FogActor = FindOrFirstTyped<AExponentialHeightFog>(World, Label);
	if (!FogActor)
		return MakeError(DOMAIN, TEXT("set_fog"), 2000,
			TEXT("No ExponentialHeightFog actor found in the level"));

	UExponentialHeightFogComponent* FC =
		FogActor->FindComponentByClass<UExponentialHeightFogComponent>();
	if (!FC)
		return MakeError(DOMAIN, TEXT("set_fog"), 2001,
			TEXT("ExponentialHeightFog actor has no FogComponent"));

#if WITH_EDITOR
	auto Tx = BeginTransaction(TEXT("Set Exponential Height Fog"));
	FC->Modify();
#endif

	double V = 0.0;
	if (Params->TryGetNumberField(TEXT("fog_density"),       V)) FC->FogDensity      = (float)V;
	if (Params->TryGetNumberField(TEXT("fog_height_falloff"), V)) FC->FogHeightFalloff = (float)V;
	if (Params->TryGetNumberField(TEXT("start_distance"),    V)) FC->StartDistance    = (float)V;

	double R = 0.4, G = 0.65, B = 1.0;
	// FogInscatteringColor removed in UE 5.7.
	// if (ReadColorObj(Params, TEXT("fog_inscattering_color"), R, G, B))
	//     FC->FogInscatteringColor = FLinearColor((float)R, (float)G, (float)B);

#if WITH_EDITOR
	FC->PostEditChange();
	FogActor->PostEditChange();
#endif

	FogActor->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("set_fog"),
		FString::Printf(TEXT("Fog updated on '%s'"), *FogActor->GetActorLabel()));
}

// ---------------------------------------------------------------------------
// set_volumetric_clouds
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_SetVolumetricClouds(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_volumetric_clouds"), 3003, TEXT("No editor world available"));

	FString Label;
	Params->TryGetStringField(TEXT("actor_label"), Label);

	UVolumetricCloudComponent* CC = FindFirstComponent<UVolumetricCloudComponent>(World, Label);
	if (!CC)
		return MakeError(DOMAIN, TEXT("set_volumetric_clouds"), 2000,
			TEXT("No VolumetricCloud actor found in the level"));

#if WITH_EDITOR
	auto Tx = BeginTransaction(TEXT("Set Volumetric Clouds"));
	CC->Modify();
#endif

	double V = 0.0;
	// VolumetricCloud km properties removed/renamed in UE 5.7 — commenting out.
	// if (Params->TryGetNumberField(TEXT("layer_bottom_altitude_km"),  V)) CC->LayerBottomAltitudeKm = (float)V;
	// if (Params->TryGetNumberField(TEXT("layer_height_km"),           V)) CC->LayerHeightKm          = (float)V;
	// if (Params->TryGetNumberField(TEXT("tracing_max_distance_km"),   V)) CC->TracingMaxDistanceKm   = (float)V;

	FString MaterialPath;
	if (Params->TryGetStringField(TEXT("cloud_material_path"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath))
			CC->Material = Mat;
		else
			return MakeError(DOMAIN, TEXT("set_volumetric_clouds"), 3000,
				FString::Printf(TEXT("Could not load material at '%s'"), *MaterialPath));
	}

#if WITH_EDITOR
	CC->PostEditChange();
	if (AActor* Owner = CC->GetOwner()) Owner->PostEditChange();
#endif

	return MakeSuccess(DOMAIN, TEXT("set_volumetric_clouds"), TEXT("VolumetricCloud updated"));
}

// ---------------------------------------------------------------------------
// set_sky_light
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_SetSkyLight(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_sky_light"), 3003, TEXT("No editor world available"));

	FString Label;
	Params->TryGetStringField(TEXT("actor_label"), Label);

	ASkyLight* SkyLightActor = FindOrFirstTyped<ASkyLight>(World, Label);
	if (!SkyLightActor)
		return MakeError(DOMAIN, TEXT("set_sky_light"), 2000,
			TEXT("No SkyLight actor found in the level"));

	USkyLightComponent* SLC = Cast<USkyLightComponent>(SkyLightActor->GetLightComponent());
	if (!SLC)
		return MakeError(DOMAIN, TEXT("set_sky_light"), 2001,
			TEXT("SkyLight actor has no SkyLightComponent"));

#if WITH_EDITOR
	auto Tx = BeginTransaction(TEXT("Set Sky Light"));
	SLC->Modify();
#endif

	double V = 0.0;
	if (Params->TryGetNumberField(TEXT("intensity"), V))
		SLC->Intensity = (float)V;

	double R = 1.0, G = 1.0, B = 1.0;
	if (ReadColorObj(Params, TEXT("light_color"), R, G, B))
	{
		SLC->LightColor = FColor(
			(uint8)FMath::Clamp((int32)(R * 255.0), 0, 255),
			(uint8)FMath::Clamp((int32)(G * 255.0), 0, 255),
			(uint8)FMath::Clamp((int32)(B * 255.0), 0, 255));
	}

	FString SourceTypeStr;
	if (Params->TryGetStringField(TEXT("source_type"), SourceTypeStr))
	{
		if (SourceTypeStr.Equals(TEXT("SpecifiedCubemap"), ESearchCase::IgnoreCase))
			SLC->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
		else
			SLC->SourceType = ESkyLightSourceType::SLS_CapturedScene;
	}

	// Set cubemap via property reflection — avoids needing Engine/TextureCube.h
	FString CubemapPath;
	if (Params->TryGetStringField(TEXT("cubemap_path"), CubemapPath) && !CubemapPath.IsEmpty())
	{
		if (UObject* CubeObj = LoadObject<UObject>(nullptr, *CubemapPath))
		{
			if (FObjectProperty* CubeProp =
				FindFProperty<FObjectProperty>(SLC->GetClass(), TEXT("Cubemap")))
			{
				CubeProp->SetObjectPropertyValue_InContainer(SLC, CubeObj);
			}
		}
		else
		{
			return MakeError(DOMAIN, TEXT("set_sky_light"), 3000,
				FString::Printf(TEXT("Could not load cubemap at '%s'"), *CubemapPath));
		}
	}

#if WITH_EDITOR
	SLC->PostEditChange();
	SkyLightActor->PostEditChange();
#endif

	SkyLightActor->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("set_sky_light"),
		FString::Printf(TEXT("SkyLight '%s' updated"), *SkyLightActor->GetActorLabel()));
}

// ---------------------------------------------------------------------------
// apply_time_of_day
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_ApplyTimeOfDay(TSharedPtr<FJsonObject> Params)
{
	double TimeHours = 12.0;
	if (!Params->TryGetNumberField(TEXT("time_hours"), TimeHours))
		return MakeError(DOMAIN, TEXT("apply_time_of_day"), 1000, TEXT("'time_hours' (0–24) is required"));

	TimeHours = FMath::Clamp(TimeHours, 0.0, 24.0);

	bool bAutoFog = true;
	Params->TryGetBoolField(TEXT("auto_adjust_fog"), bAutoFog);

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("apply_time_of_day"), 3003, TEXT("No editor world available"));

	// --- Rotate the first DirectionalLight (sun) ---
	// Formula: pitch 0 at 6 AM (horizon), -90 at 12 PM (overhead), +90 at midnight (below ground)
	// SunPitch = -(time / 24) * 360 + 90
	const float SunPitch = -(float)(TimeHours / 24.0) * 360.0f + 90.0f;

	ADirectionalLight* SunLight = nullptr;
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		SunLight = *It;
		break;
	}

	if (!SunLight)
		return MakeError(DOMAIN, TEXT("apply_time_of_day"), 2000,
			TEXT("No DirectionalLight actor found — add a sun directional light first"));

#if WITH_EDITOR
	auto Tx = BeginTransaction(TEXT("Apply Time Of Day"));
	SunLight->Modify();
#endif

	FRotator NewRot = SunLight->GetActorRotation();
	NewRot.Pitch = SunPitch;
	SunLight->SetActorRotation(NewRot);

	FString Msg = FString::Printf(
		TEXT("Sun pitch set to %.1f° (time %.1fh)"), SunPitch, (float)TimeHours);

	// --- Optional fog density scale based on time ---
	// Dawn/dusk get 40% more fog; noon and midnight are baseline.
	// Scale = 1.0 + 0.4 * |sin( (time/12) * PI )|
	//   At  6h / 18h: scale ≈ 1.4  (atmospheric haze near horizon)
	//   At 12h / 0h:  scale = 1.0
	if (bAutoFog)
	{
		UExponentialHeightFogComponent* FC =
			FindFirstComponent<UExponentialHeightFogComponent>(World);
		if (FC)
		{
#if WITH_EDITOR
			FC->Modify();
#endif
			const float Scale = 1.0f + 0.4f * FMath::Abs(
				FMath::Sin((float)(TimeHours / 12.0) * PI));
			FC->FogDensity *= Scale;

#if WITH_EDITOR
			FC->PostEditChange();
			if (AActor* Owner = FC->GetOwner()) Owner->PostEditChange();
#endif
			Msg += FString::Printf(TEXT(", fog scale %.2f"), Scale);
		}
	}

#if WITH_EDITOR
	SunLight->PostEditChange();
#endif

	SunLight->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("apply_time_of_day"), Msg);
}

// ---------------------------------------------------------------------------
// get_environment_info
// ---------------------------------------------------------------------------

FBridgeResult UEnvironmentHandler::Action_GetEnvironmentInfo(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("get_environment_info"), 3003, TEXT("No editor world available"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// --- SkyAtmosphere ---
	{
		TSharedPtr<FJsonObject> Atm = MakeShared<FJsonObject>();
		USkyAtmosphereComponent* AC = FindFirstComponent<USkyAtmosphereComponent>(World);
		Atm->SetBoolField(TEXT("found"), AC != nullptr);
		if (AC)
		{
			Atm->SetStringField(TEXT("actor"), AC->GetOwner() ? AC->GetOwner()->GetActorLabel() : TEXT("(?)"));
			Atm->SetNumberField(TEXT("rayleigh_scattering_scale"), AC->RayleighScatteringScale);
			Atm->SetNumberField(TEXT("mie_scattering_scale"),      AC->MieScatteringScale);
			Atm->SetNumberField(TEXT("mie_absorption_scale"),      AC->MieAbsorptionScale);
			// AerialPerspectiveViewDistanceScale removed in UE 5.7
			Atm->SetObjectField(TEXT("sky_luminance_factor"),
				MakeColorObj(AC->SkyLuminanceFactor.R, AC->SkyLuminanceFactor.G, AC->SkyLuminanceFactor.B));
		}
		Data->SetObjectField(TEXT("sky_atmosphere"), Atm);
	}

	// --- ExponentialHeightFog ---
	{
		TSharedPtr<FJsonObject> FogInfo = MakeShared<FJsonObject>();
		UExponentialHeightFogComponent* FC = FindFirstComponent<UExponentialHeightFogComponent>(World);
		FogInfo->SetBoolField(TEXT("found"), FC != nullptr);
		if (FC)
		{
			FogInfo->SetStringField(TEXT("actor"), FC->GetOwner() ? FC->GetOwner()->GetActorLabel() : TEXT("(?)"));
			FogInfo->SetNumberField(TEXT("fog_density"),       FC->FogDensity);
			FogInfo->SetNumberField(TEXT("fog_height_falloff"), FC->FogHeightFalloff);
			FogInfo->SetNumberField(TEXT("start_distance"),    FC->StartDistance);
			// FogInscatteringColor removed in UE 5.7
		}
		Data->SetObjectField(TEXT("fog"), FogInfo);
	}

	// --- VolumetricCloud ---
	{
		TSharedPtr<FJsonObject> CloudInfo = MakeShared<FJsonObject>();
		UVolumetricCloudComponent* CC = FindFirstComponent<UVolumetricCloudComponent>(World);
		CloudInfo->SetBoolField(TEXT("found"), CC != nullptr);
		if (CC)
		{
			CloudInfo->SetStringField(TEXT("actor"), CC->GetOwner() ? CC->GetOwner()->GetActorLabel() : TEXT("(?)"));
			// VolumetricCloud km properties removed/renamed in UE 5.7
			CloudInfo->SetStringField(TEXT("material"),
				CC->Material ? CC->Material->GetPathName() : TEXT("(none)"));
		}
		Data->SetObjectField(TEXT("volumetric_clouds"), CloudInfo);
	}

	// --- SkyLight ---
	{
		TSharedPtr<FJsonObject> SLInfo = MakeShared<FJsonObject>();
		ASkyLight* SkyLightActor = nullptr;
		for (TActorIterator<ASkyLight> It(World); It; ++It) { SkyLightActor = *It; break; }
		SLInfo->SetBoolField(TEXT("found"), SkyLightActor != nullptr);
		if (SkyLightActor)
		{
			SLInfo->SetStringField(TEXT("actor"), SkyLightActor->GetActorLabel());
			if (USkyLightComponent* SLC = Cast<USkyLightComponent>(SkyLightActor->GetLightComponent()))
			{
				SLInfo->SetNumberField(TEXT("intensity"), SLC->Intensity);
				SLInfo->SetObjectField(TEXT("light_color"),
					MakeColorObj(SLC->LightColor.R / 255.f, SLC->LightColor.G / 255.f, SLC->LightColor.B / 255.f));
				const FString SrcType =
					(SLC->SourceType == ESkyLightSourceType::SLS_SpecifiedCubemap)
						? TEXT("SpecifiedCubemap") : TEXT("Captured");
				SLInfo->SetStringField(TEXT("source_type"), SrcType);

				// Cubemap path via reflection
				FString CubemapPath = TEXT("(none)");
				if (const FObjectProperty* CubeProp =
					FindFProperty<FObjectProperty>(SLC->GetClass(), TEXT("Cubemap")))
				{
					const UObject* Cube = CubeProp->GetObjectPropertyValue_InContainer(SLC);
					if (Cube) CubemapPath = Cube->GetPathName();
				}
				SLInfo->SetStringField(TEXT("cubemap"), CubemapPath);
			}
		}
		Data->SetObjectField(TEXT("sky_light"), SLInfo);
	}

	// --- DirectionalLight (sun) ---
	{
		TSharedPtr<FJsonObject> SunInfo = MakeShared<FJsonObject>();
		ADirectionalLight* Sun = nullptr;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It) { Sun = *It; break; }
		SunInfo->SetBoolField(TEXT("found"), Sun != nullptr);
		if (Sun)
		{
			SunInfo->SetStringField(TEXT("actor"),    Sun->GetActorLabel());
			SunInfo->SetNumberField(TEXT("pitch"),    Sun->GetActorRotation().Pitch);
			SunInfo->SetNumberField(TEXT("yaw"),      Sun->GetActorRotation().Yaw);
			if (UDirectionalLightComponent* DLC =
				Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				SunInfo->SetNumberField(TEXT("intensity"), DLC->Intensity);
			}
		}
		Data->SetObjectField(TEXT("directional_light"), SunInfo);
	}

	return MakeSuccess(DOMAIN, TEXT("get_environment_info"), TEXT("Environment state captured"), Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UEnvironmentHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto P = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"),   Type);
		O->SetBoolField(TEXT("required"), bReq);
		O->SetStringField(TEXT("desc"),   Desc);
		return O;
	};

	// set_sky_atmosphere
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"),                          P(TEXT("string"), false, TEXT("Target actor label. Omit or 'first' to use first found")));
		Ps->SetObjectField(TEXT("rayleigh_scattering_scale"),            P(TEXT("float"),  false, TEXT("Rayleigh scattering intensity scale (default 1.0)")));
		Ps->SetObjectField(TEXT("mie_scattering_scale"),                 P(TEXT("float"),  false, TEXT("Mie scattering scale (default 0.003996)")));
		Ps->SetObjectField(TEXT("mie_absorption_scale"),                 P(TEXT("float"),  false, TEXT("Mie absorption scale (default 0.000444)")));
		Ps->SetObjectField(TEXT("aerial_perspective_view_distance_scale"),P(TEXT("float"),  false, TEXT("Aerial perspective strength (default 1.0)")));
		Ps->SetObjectField(TEXT("sky_luminance_factor"),                 P(TEXT("object{r,g,b}"), false, TEXT("Overall sky color tint (0-1 per channel)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Modify the physically-based sky atmosphere simulation"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_sky_atmosphere"), A);
	}

	// set_fog
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"),             P(TEXT("string"),         false, TEXT("Target ExponentialHeightFog actor label. Omit for first found")));
		Ps->SetObjectField(TEXT("fog_density"),             P(TEXT("float"),          false, TEXT("Global fog density (typical: 0.001 – 0.05)")));
		Ps->SetObjectField(TEXT("fog_height_falloff"),      P(TEXT("float"),          false, TEXT("Density falloff with height (typical: 0.01 – 0.2)")));
		Ps->SetObjectField(TEXT("fog_inscattering_color"),  P(TEXT("object{r,g,b}"), false, TEXT("Fog scattering color (0-1)")));
		Ps->SetObjectField(TEXT("start_distance"),          P(TEXT("float"),          false, TEXT("Distance (cm) before fog starts")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Modify exponential height fog density, falloff, color and start distance"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_fog"), A);
	}

	// set_volumetric_clouds
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"),              P(TEXT("string"), false, TEXT("Target VolumetricCloud actor label")));
		Ps->SetObjectField(TEXT("layer_bottom_altitude_km"), P(TEXT("float"),  false, TEXT("Cloud layer base altitude in km (default ~5)")));
		Ps->SetObjectField(TEXT("layer_height_km"),          P(TEXT("float"),  false, TEXT("Cloud layer thickness in km (default ~10)")));
		Ps->SetObjectField(TEXT("tracing_max_distance_km"),  P(TEXT("float"),  false, TEXT("Max ray-march distance in km (default ~300)")));
		Ps->SetObjectField(TEXT("cloud_material_path"),      P(TEXT("string"), false, TEXT("Optional asset path to cloud material (e.g. /Game/VFX/M_Clouds)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Modify volumetric cloud layer altitude, height, tracing distance and material"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_volumetric_clouds"), A);
	}

	// set_sky_light
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("actor_label"),  P(TEXT("string"),         false, TEXT("Target SkyLight actor label")));
		Ps->SetObjectField(TEXT("intensity"),    P(TEXT("float"),          false, TEXT("Sky light intensity")));
		Ps->SetObjectField(TEXT("light_color"),  P(TEXT("object{r,g,b}"), false, TEXT("Tint color (0-1 per channel)")));
		Ps->SetObjectField(TEXT("source_type"),  P(TEXT("string"),         false, TEXT("'Captured' (default) or 'SpecifiedCubemap'")));
		Ps->SetObjectField(TEXT("cubemap_path"), P(TEXT("string"),         false, TEXT("Asset path for cubemap when source_type='SpecifiedCubemap'")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Modify sky light intensity, color, source type and cubemap"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_sky_light"), A);
	}

	// apply_time_of_day
	{
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		Ps->SetObjectField(TEXT("time_hours"),     P(TEXT("float"), true,  TEXT("Time of day 0–24. 6=sunrise, 12=noon, 18=sunset, 0/24=midnight")));
		Ps->SetObjectField(TEXT("auto_adjust_fog"),P(TEXT("bool"),  false, TEXT("Scale fog density by time (more at dawn/dusk). Default true")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Rotate the first DirectionalLight to simulate the sun position for a given hour. Optionally scales fog density"));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("apply_time_of_day"), A);
	}

	// get_environment_info
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Read current state of SkyAtmosphere, ExponentialHeightFog, VolumetricCloud, SkyLight and DirectionalLight"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_environment_info"), A);
	}

	return Root;
}
