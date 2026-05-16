#include "Handlers/PostProcessHandler.h"
#include "ForgeAISubsystem.h"
#include "Capture/ForgeWeatherCapture.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---- Post process ----------------------------------------------------------
#include "Engine/PostProcessVolume.h"

// ---- World / editor --------------------------------------------------------
#include "EngineUtils.h"            // TActorIterator

#if WITH_EDITOR
#include "Editor.h"                 // GEditor
#include "ScopedTransaction.h"
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("post_process");

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("create_pp_volume"))      return Action_CreatePPVolume(Params);
	if (Action == TEXT("set_bloom"))             return Action_SetBloom(Params);
	if (Action == TEXT("set_exposure"))          return Action_SetExposure(Params);
	if (Action == TEXT("set_color_grading"))     return Action_SetColorGrading(Params);
	if (Action == TEXT("set_ambient_occlusion")) return Action_SetAmbientOcclusion(Params);

	if (Action == TEXT("read_weather_capture"))
	{
		if (Subsystem && Subsystem->WeatherCapture)
			Subsystem->WeatherCapture->ExportWeatherState();
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("weather/current.json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(DOMAIN, Action, TEXT("Weather/environment state captured: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}

	if (Action == TEXT("set_pp_property"))         return Action_SetPPProperty(Params);
	if (Action == TEXT("set_motion_blur"))         return Action_SetMotionBlur(Params);
	if (Action == TEXT("set_chromatic_aberration")) return Action_SetChromaticAberration(Params);
	if (Action == TEXT("set_vignette"))            return Action_SetVignette(Params);
	if (Action == TEXT("set_lumen"))               return Action_SetLumen(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown post_process action '%s'"), *Action),
		TEXT("post_process capabilities"));
}

// ---------------------------------------------------------------------------
// Helper: find actor by label
// ---------------------------------------------------------------------------

AActor* UPostProcessHandler::FindActorByLabel(UWorld* World, const FString& Label) const
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
// Helper: parse FVector4 from JSON object {r,g,b,a}
// ---------------------------------------------------------------------------

static FVector4 ParseVector4(const TSharedPtr<FJsonObject>& Obj, const FVector4& Default = FVector4(1.0, 1.0, 1.0, 1.0))
{
	if (!Obj.IsValid()) return Default;
	double R = Default.X, G = Default.Y, B = Default.Z, A = Default.W;
	Obj->TryGetNumberField(TEXT("r"), R);
	Obj->TryGetNumberField(TEXT("g"), G);
	Obj->TryGetNumberField(TEXT("b"), B);
	Obj->TryGetNumberField(TEXT("a"), A);
	return FVector4(R, G, B, A);
}

// ---------------------------------------------------------------------------
// create_pp_volume
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::Action_CreatePPVolume(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_pp_volume");

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
	}

	// Parse location
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("location"), LocationObj) || !LocationObj || !(*LocationObj).IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'location' {x,y,z}"));
	}

	double X = 0.0, Y = 0.0, Z = 0.0;
	(*LocationObj)->TryGetNumberField(TEXT("x"), X);
	(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
	(*LocationObj)->TryGetNumberField(TEXT("z"), Z);

	// Parse optional extent
	double EX = 200.0, EY = 200.0, EZ = 200.0;
	const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
	if (Params->TryGetObjectField(TEXT("extent"), ExtentObj) && ExtentObj && (*ExtentObj).IsValid())
	{
		(*ExtentObj)->TryGetNumberField(TEXT("x"), EX);
		(*ExtentObj)->TryGetNumberField(TEXT("y"), EY);
		(*ExtentObj)->TryGetNumberField(TEXT("z"), EZ);
	}

	bool bInfinite = false;
	Params->TryGetBoolField(TEXT("infinite"), bInfinite);

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create PostProcessVolume")));

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));

	APostProcessVolume* Vol = World->SpawnActor<APostProcessVolume>(
		APostProcessVolume::StaticClass(), SpawnTransform);
	if (!Vol)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to spawn APostProcessVolume at (%.0f, %.0f, %.0f)"),
				(float)X, (float)Y, (float)Z));
	}

	Vol->bUnbound = bInfinite;

	if (!bInfinite)
	{
		// Set brush extent via the root component's bounds
		Vol->SetActorScale3D(FVector((float)EX / 100.0f, (float)EY / 100.0f, (float)EZ / 100.0f));
	}

	Vol->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Vol->GetActorLabel());
	Data->SetBoolField(TEXT("infinite"), bInfinite);

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("PostProcessVolume '%s' created at (%.0f, %.0f, %.0f) infinite=%s"),
			*Vol->GetActorLabel(), (float)X, (float)Y, (float)Z,
			bInfinite ? TEXT("true") : TEXT("false")),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_bloom
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::Action_SetBloom(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_bloom");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double Intensity = 0.0;
	if (!Params->TryGetNumberField(TEXT("intensity"), Intensity))
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'intensity'"));
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

	APostProcessVolume* Vol = Cast<APostProcessVolume>(Actor);
	if (!Vol)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an APostProcessVolume"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Bloom")));

	FPostProcessSettings& PP = Vol->Settings;

	// Method (optional)
	double Method = 0.0;
	if (Params->TryGetNumberField(TEXT("method"), Method))
	{
		PP.bOverride_BloomMethod = true;
		PP.BloomMethod = (EBloomMethod)(int32)Method;
	}

	// Intensity (required)
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = (float)Intensity;

	// Threshold (optional)
	double Threshold = -1.0;
	if (Params->TryGetNumberField(TEXT("threshold"), Threshold))
	{
		PP.bOverride_BloomThreshold = true;
		PP.BloomThreshold = (float)Threshold;
	}

	// SizeScale (optional)
	double SizeScale = 0.0;
	if (Params->TryGetNumberField(TEXT("size_scale"), SizeScale))
	{
		PP.bOverride_BloomSizeScale = true;
		PP.BloomSizeScale = (float)SizeScale;
	}

	Vol->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("intensity"), Intensity);
	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Bloom set on '%s': intensity=%.2f"), *ActorPath, (float)Intensity),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_exposure
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::Action_SetExposure(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_exposure");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double Compensation = 0.0;
	if (!Params->TryGetNumberField(TEXT("compensation"), Compensation))
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'compensation'"));
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

	APostProcessVolume* Vol = Cast<APostProcessVolume>(Actor);
	if (!Vol)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an APostProcessVolume"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Exposure")));

	FPostProcessSettings& PP = Vol->Settings;

	// Method (optional)
	FString MethodStr;
	if (Params->TryGetStringField(TEXT("method"), MethodStr))
	{
		PP.bOverride_AutoExposureMethod = true;
		if (MethodStr == TEXT("Manual"))
		{
			PP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		}
		else
		{
			PP.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
		}
	}

	// Compensation (required)
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = (float)Compensation;

	// MinBrightness (optional)
	double MinBrightness = 0.0;
	if (Params->TryGetNumberField(TEXT("min_brightness"), MinBrightness))
	{
		PP.bOverride_AutoExposureMinBrightness = true;
		PP.AutoExposureMinBrightness = (float)MinBrightness;
	}

	// MaxBrightness (optional)
	double MaxBrightness = 0.0;
	if (Params->TryGetNumberField(TEXT("max_brightness"), MaxBrightness))
	{
		PP.bOverride_AutoExposureMaxBrightness = true;
		PP.AutoExposureMaxBrightness = (float)MaxBrightness;
	}

	Vol->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("compensation"), Compensation);
	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Exposure set on '%s': compensation=%.2f"), *ActorPath, (float)Compensation),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_color_grading
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::Action_SetColorGrading(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_color_grading");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
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

	APostProcessVolume* Vol = Cast<APostProcessVolume>(Actor);
	if (!Vol)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an APostProcessVolume"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Color Grading")));

	FPostProcessSettings& PP = Vol->Settings;

	// Temperature (optional)
	double Temperature = 0.0;
	if (Params->TryGetNumberField(TEXT("temperature"), Temperature))
	{
		PP.bOverride_WhiteTemp = true;
		PP.WhiteTemp = (float)Temperature;
	}

	// Tint (optional)
	double Tint = 0.0;
	if (Params->TryGetNumberField(TEXT("tint"), Tint))
	{
		PP.bOverride_WhiteTint = true;
		PP.WhiteTint = (float)Tint;
	}

	// Saturation (optional)
	const TSharedPtr<FJsonObject>* SatObj = nullptr;
	if (Params->TryGetObjectField(TEXT("saturation"), SatObj) && SatObj && (*SatObj).IsValid())
	{
		FVector4 V = ParseVector4(*SatObj);
		PP.bOverride_ColorSaturation = true;
		PP.ColorSaturation = V;
	}

	// Contrast (optional)
	const TSharedPtr<FJsonObject>* ConObj = nullptr;
	if (Params->TryGetObjectField(TEXT("contrast"), ConObj) && ConObj && (*ConObj).IsValid())
	{
		FVector4 V = ParseVector4(*ConObj);
		PP.bOverride_ColorContrast = true;
		PP.ColorContrast = V;
	}

	// Gain (optional)
	const TSharedPtr<FJsonObject>* GainObj = nullptr;
	if (Params->TryGetObjectField(TEXT("gain"), GainObj) && GainObj && (*GainObj).IsValid())
	{
		FVector4 V = ParseVector4(*GainObj);
		PP.bOverride_ColorGain = true;
		PP.ColorGain = V;
	}

	// LUT Texture (optional)
	FString LUTPath;
	if (Params->TryGetStringField(TEXT("lut_texture"), LUTPath) && !LUTPath.IsEmpty())
	{
		UTexture* LUT = LoadObject<UTexture>(nullptr, *LUTPath);
		if (LUT)
		{
			PP.bOverride_ColorGradingLUT = true;
			PP.ColorGradingLUT = LUT;
		}
		else
		{
			Vol->MarkPackageDirty();
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("LUT texture not found at '%s'"), *LUTPath));
		}
	}

	Vol->MarkPackageDirty();

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Color grading updated on '%s'"), *ActorPath));
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_ambient_occlusion
// ---------------------------------------------------------------------------

FBridgeResult UPostProcessHandler::Action_SetAmbientOcclusion(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ambient_occlusion");

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
	}

	double Intensity = 0.0;
	if (!Params->TryGetNumberField(TEXT("intensity"), Intensity))
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'intensity'"));
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

	APostProcessVolume* Vol = Cast<APostProcessVolume>(Actor);
	if (!Vol)
	{
		return MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' is not an APostProcessVolume"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Ambient Occlusion")));

	FPostProcessSettings& PP = Vol->Settings;

	// Intensity (required)
	PP.bOverride_AmbientOcclusionIntensity = true;
	PP.AmbientOcclusionIntensity = (float)Intensity;

	// Radius (optional)
	double Radius = 0.0;
	if (Params->TryGetNumberField(TEXT("radius"), Radius))
	{
		PP.bOverride_AmbientOcclusionRadius = true;
		PP.AmbientOcclusionRadius = (float)Radius;
	}

	// Quality (optional)
	double Quality = 0.0;
	if (Params->TryGetNumberField(TEXT("quality"), Quality))
	{
		PP.bOverride_AmbientOcclusionQuality = true;
		PP.AmbientOcclusionQuality = (float)Quality;
	}

	// Power (optional)
	double Power = 0.0;
	if (Params->TryGetNumberField(TEXT("power"), Power))
	{
		PP.bOverride_AmbientOcclusionPower = true;
		PP.AmbientOcclusionPower = (float)Power;
	}

	Vol->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("intensity"), Intensity);
	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Ambient occlusion set on '%s': intensity=%.2f"), *ActorPath, (float)Intensity),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UPostProcessHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a PostProcessVolume in the editor world"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), true, TEXT("World location"))); Pr->SetObjectField(TEXT("extent"), P(TEXT("object {x,y,z}"), false, TEXT("Volume half-extents (default 200,200,200)"))); Pr->SetObjectField(TEXT("infinite"), P(TEXT("bool"), false, TEXT("Unbound/infinite volume (default false)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_pp_volume"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure bloom settings on a PostProcessVolume"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the PP volume"))); Pr->SetObjectField(TEXT("intensity"), P(TEXT("float"), true, TEXT("Bloom intensity"))); Pr->SetObjectField(TEXT("method"), P(TEXT("int"), false, TEXT("Bloom method enum value"))); Pr->SetObjectField(TEXT("threshold"), P(TEXT("float"), false, TEXT("Bloom threshold"))); Pr->SetObjectField(TEXT("size_scale"), P(TEXT("float"), false, TEXT("Bloom size scale"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_bloom"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure exposure settings on a PostProcessVolume"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("compensation"), P(TEXT("float"), true, TEXT("Exposure compensation (EV bias)"))); Pr->SetObjectField(TEXT("method"), P(TEXT("string"), false, TEXT("Manual or Auto"))); Pr->SetObjectField(TEXT("min_brightness"), P(TEXT("float"), false, TEXT("Auto exposure min brightness"))); Pr->SetObjectField(TEXT("max_brightness"), P(TEXT("float"), false, TEXT("Auto exposure max brightness"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_exposure"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure color grading on a PostProcessVolume"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("temperature"), P(TEXT("float"), false, TEXT("White balance temperature"))); Pr->SetObjectField(TEXT("tint"), P(TEXT("float"), false, TEXT("White balance tint"))); Pr->SetObjectField(TEXT("saturation"), P(TEXT("object {r,g,b,a}"), false, TEXT("Color saturation"))); Pr->SetObjectField(TEXT("contrast"), P(TEXT("object {r,g,b,a}"), false, TEXT("Color contrast"))); Pr->SetObjectField(TEXT("gain"), P(TEXT("object {r,g,b,a}"), false, TEXT("Color gain"))); Pr->SetObjectField(TEXT("lut_texture"), P(TEXT("string"), false, TEXT("LUT texture asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_color_grading"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure ambient occlusion on a PostProcessVolume"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("intensity"), P(TEXT("float"), true, TEXT("AO intensity"))); Pr->SetObjectField(TEXT("radius"), P(TEXT("float"), false, TEXT("AO radius"))); Pr->SetObjectField(TEXT("quality"), P(TEXT("float"), false, TEXT("AO quality"))); Pr->SetObjectField(TEXT("power"), P(TEXT("float"), false, TEXT("AO power"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ambient_occlusion"), A); }

    return Root;
}

// ===========================================================================
// Phase 3: deeper PP coverage via reflection
// ===========================================================================

#include "Engine/PostProcessVolume.h"

namespace
{
	APostProcessVolume* FindPP(UWorld* World, const FString& Label)
	{
		if (!World) return nullptr;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	bool SetPPFloat(APostProcessVolume* Vol, const FString& PropName, float Value)
	{
		if (!Vol) return false;
		FStructProperty* SettingsProp = FindFProperty<FStructProperty>(APostProcessVolume::StaticClass(), TEXT("Settings"));
		if (!SettingsProp) return false;
		void* SettingsPtr = SettingsProp->ContainerPtrToValuePtr<void>(Vol);
		UScriptStruct* SettingsStruct = SettingsProp->Struct;
		FProperty* P = SettingsStruct->FindPropertyByName(FName(*PropName));
		if (!P) return false;
		if (FFloatProperty* FP = CastField<FFloatProperty>(P))
		{
			FP->SetPropertyValue_InContainer(SettingsPtr, Value);
		}
		else
		{
			return false;
		}
		const FString OvName = TEXT("bOverride_") + PropName;
		if (FBoolProperty* BP = CastField<FBoolProperty>(SettingsStruct->FindPropertyByName(FName(*OvName))))
		{
			BP->SetPropertyValue_InContainer(SettingsPtr, true);
		}
		Vol->MarkPackageDirty();
		return true;
	}
}

FBridgeResult UPostProcessHandler::Action_SetPPProperty(TSharedPtr<FJsonObject> Params)
{
	FString Label, PropName;
	double Value = 0.0;
	if (!Params->TryGetStringField(TEXT("actor_path"), Label))
		Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_pp_property"), 1000, TEXT("'actor_label' is required"));
	if (!Params->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_pp_property"), 1000, TEXT("'property' is required (FPostProcessSettings field)"));
	if (!Params->TryGetNumberField(TEXT("value"), Value))
		return MakeError(DOMAIN, TEXT("set_pp_property"), 1000, TEXT("'value' (number) is required"));

	APostProcessVolume* PP = FindPP(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, Label);
	if (!PP) return MakeError(DOMAIN, TEXT("set_pp_property"), 2000, TEXT("PostProcessVolume not found"));
	if (!SetPPFloat(PP, PropName, (float)Value))
		return MakeError(DOMAIN, TEXT("set_pp_property"), 2001,
			FString::Printf(TEXT("Property '%s' not found or not a float"), *PropName));
	return MakeSuccess(DOMAIN, TEXT("set_pp_property"),
		FString::Printf(TEXT("'%s'.%s = %.4f (override=true)"), *Label, *PropName, Value));
}

FBridgeResult UPostProcessHandler::Action_SetMotionBlur(TSharedPtr<FJsonObject> Params)
{
	FString Label;
	double Amount = 0.5, Max = 5.0, TargetFPS = 30.0;
	if (!Params->TryGetStringField(TEXT("actor_path"), Label))
		Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_motion_blur"), 1000, TEXT("'actor_label' is required"));
	Params->TryGetNumberField(TEXT("amount"), Amount);
	Params->TryGetNumberField(TEXT("max"), Max);
	Params->TryGetNumberField(TEXT("target_fps"), TargetFPS);
	APostProcessVolume* PP = FindPP(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, Label);
	if (!PP) return MakeError(DOMAIN, TEXT("set_motion_blur"), 2000, TEXT("PP not found"));
	SetPPFloat(PP, TEXT("MotionBlurAmount"), (float)Amount);
	SetPPFloat(PP, TEXT("MotionBlurMax"), (float)Max);
	SetPPFloat(PP, TEXT("MotionBlurTargetFPS"), (float)TargetFPS);
	return MakeSuccess(DOMAIN, TEXT("set_motion_blur"),
		FString::Printf(TEXT("'%s' motion_blur amount=%.2f max=%.2f target=%.0f"), *Label, Amount, Max, TargetFPS));
}

FBridgeResult UPostProcessHandler::Action_SetChromaticAberration(TSharedPtr<FJsonObject> Params)
{
	FString Label;
	double Intensity = 0.0, StartOffset = 0.0;
	if (!Params->TryGetStringField(TEXT("actor_path"), Label))
		Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_chromatic_aberration"), 1000, TEXT("'actor_label' is required"));
	Params->TryGetNumberField(TEXT("intensity"), Intensity);
	Params->TryGetNumberField(TEXT("start_offset"), StartOffset);
	APostProcessVolume* PP = FindPP(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, Label);
	if (!PP) return MakeError(DOMAIN, TEXT("set_chromatic_aberration"), 2000, TEXT("PP not found"));
	SetPPFloat(PP, TEXT("SceneFringeIntensity"), (float)Intensity);
	SetPPFloat(PP, TEXT("ChromaticAberrationStartOffset"), (float)StartOffset);
	return MakeSuccess(DOMAIN, TEXT("set_chromatic_aberration"),
		FString::Printf(TEXT("'%s' chromatic intensity=%.3f start=%.3f"), *Label, Intensity, StartOffset));
}

FBridgeResult UPostProcessHandler::Action_SetVignette(TSharedPtr<FJsonObject> Params)
{
	FString Label;
	double Intensity = 0.4;
	if (!Params->TryGetStringField(TEXT("actor_path"), Label))
		Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_vignette"), 1000, TEXT("'actor_label' is required"));
	Params->TryGetNumberField(TEXT("intensity"), Intensity);
	APostProcessVolume* PP = FindPP(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, Label);
	if (!PP) return MakeError(DOMAIN, TEXT("set_vignette"), 2000, TEXT("PP not found"));
	SetPPFloat(PP, TEXT("VignetteIntensity"), (float)Intensity);
	return MakeSuccess(DOMAIN, TEXT("set_vignette"),
		FString::Printf(TEXT("'%s' vignette=%.3f"), *Label, Intensity));
}

FBridgeResult UPostProcessHandler::Action_SetLumen(TSharedPtr<FJsonObject> Params)
{
	FString Label;
	double SceneViewExtensionDistance = 0.0;
	bool bDoSceneViewExtensionDistance = Params->TryGetNumberField(TEXT("scene_view_extension_distance"), SceneViewExtensionDistance);
	double FinalGatherQuality = 1.0, ReflectionQuality = 1.0, MaxTraceDistance = 20000.0;
	if (!Params->TryGetStringField(TEXT("actor_path"), Label))
		Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_lumen"), 1000, TEXT("'actor_label' is required"));
	Params->TryGetNumberField(TEXT("final_gather_quality"), FinalGatherQuality);
	Params->TryGetNumberField(TEXT("reflection_quality"), ReflectionQuality);
	Params->TryGetNumberField(TEXT("max_trace_distance"), MaxTraceDistance);
	APostProcessVolume* PP = FindPP(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, Label);
	if (!PP) return MakeError(DOMAIN, TEXT("set_lumen"), 2000, TEXT("PP not found"));
	SetPPFloat(PP, TEXT("LumenFinalGatherQuality"), (float)FinalGatherQuality);
	SetPPFloat(PP, TEXT("LumenReflectionQuality"), (float)ReflectionQuality);
	SetPPFloat(PP, TEXT("LumenMaxTraceDistance"), (float)MaxTraceDistance);
	if (bDoSceneViewExtensionDistance)
	{
		SetPPFloat(PP, TEXT("LumenSceneViewDistance"), (float)SceneViewExtensionDistance);
	}
	return MakeSuccess(DOMAIN, TEXT("set_lumen"),
		FString::Printf(TEXT("'%s' lumen fg=%.2f refl=%.2f trace=%.0f"),
			*Label, FinalGatherQuality, ReflectionQuality, MaxTraceDistance));
}
