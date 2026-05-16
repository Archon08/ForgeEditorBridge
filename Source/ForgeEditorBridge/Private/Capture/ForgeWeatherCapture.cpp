#include "Capture/ForgeWeatherCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Directional light (sun / moon) ---
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"

// --- Sky Light ---
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"

// --- Sky Atmosphere ---
// ASkyAtmosphere actor header path varies by UE5 version; use the component header instead
// and find it via TActorIterator<AActor> + FindComponentByClass.
#include "Components/SkyAtmosphereComponent.h"

// --- Volumetric Cloud ---
// Use FindComponentByClass approach (same reason as SkyAtmosphere above).
#include "Components/VolumetricCloudComponent.h"

// --- Fog ---
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"

// --- Post Process ---
#include "Engine/PostProcessVolume.h"

// --- UObject property reflection ---
#include "UObject/UnrealType.h"

// --- World / Actor iteration ---
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    // Build a {r,g,b} JSON object from FLinearColor
    TSharedRef<FJsonObject> MakeColorObj(const FLinearColor& C)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("r"), C.R);
        Obj->SetNumberField(TEXT("g"), C.G);
        Obj->SetNumberField(TEXT("b"), C.B);
        return Obj;
    }

    // Build a {pitch,yaw,roll} JSON object from FRotator
    TSharedRef<FJsonObject> MakeRotObj(const FRotator& R)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), R.Pitch);
        Obj->SetNumberField(TEXT("yaw"),   R.Yaw);
        Obj->SetNumberField(TEXT("roll"),  R.Roll);
        return Obj;
    }

    // Serialize a directional light into a JSON object (shared by sun + moon)
    TSharedRef<FJsonObject> SerializeDirectionalLight(ADirectionalLight* DL)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("found"), true);
        Obj->SetStringField(TEXT("actor_name"), DL->GetActorNameOrLabel());

        UDirectionalLightComponent* LC = Cast<UDirectionalLightComponent>(DL->GetLightComponent());
        if (LC)
        {
            Obj->SetNumberField(TEXT("intensity"), LC->Intensity);
            Obj->SetObjectField(TEXT("color"),     MakeColorObj(FLinearColor(LC->LightColor)));
        }
        Obj->SetObjectField(TEXT("rotation"), MakeRotObj(DL->GetActorRotation()));
        return Obj;
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeWeatherCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("weather")));
    UE_LOG(LogTemp, Log, TEXT("ForgeWeather: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportWeatherState
// ---------------------------------------------------------------------------

bool UForgeWeatherCapture::ExportWeatherState()
{
    if (!GEditor)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeWeather: GEditor is null"));
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeWeather: No editor world found"));
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("level_name"), World->GetName());

    TArray<TSharedPtr<FJsonValue>> Issues;

    // -------------------------------------------------------------------------
    // Sun + Moon — first and second ADirectionalLight in the level
    // -------------------------------------------------------------------------
    {
        ADirectionalLight* Sun  = nullptr;
        ADirectionalLight* Moon = nullptr;

        for (TActorIterator<ADirectionalLight> It(World); It; ++It)
        {
            if (!Sun)       { Sun  = *It; continue; }
            if (!Moon)      { Moon = *It; break; }
        }

        // Sun
        if (Sun)
        {
            Root->SetObjectField(TEXT("sun"), SerializeDirectionalLight(Sun));
        }
        else
        {
            TSharedRef<FJsonObject> SunObj = MakeShared<FJsonObject>();
            SunObj->SetBoolField(TEXT("found"), false);
            Root->SetObjectField(TEXT("sun"), SunObj);

            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("issue_type"), TEXT("NO_DIRECTIONAL_LIGHT"));
            Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
            Issue->SetStringField(TEXT("detail"),
                TEXT("No ADirectionalLight found in level. Scene may be unlit or relying solely on a sky light. "
                     "Add a directional light actor for sun simulation and shadow casting."));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }

        // Moon (second directional light, if present)
        if (Moon)
        {
            Root->SetObjectField(TEXT("moon"), SerializeDirectionalLight(Moon));
        }
        else
        {
            TSharedRef<FJsonObject> MoonObj = MakeShared<FJsonObject>();
            MoonObj->SetBoolField(TEXT("found"), false);
            Root->SetObjectField(TEXT("moon"), MoonObj);
        }
    }

    // -------------------------------------------------------------------------
    // Atmosphere — first actor carrying a USkyAtmosphereComponent
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> AtmObj = MakeShared<FJsonObject>();

        USkyAtmosphereComponent* AC = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (USkyAtmosphereComponent* Comp = (*It)->FindComponentByClass<USkyAtmosphereComponent>())
            {
                AC = Comp;
                break;
            }
        }

        if (AC)
        {
            AtmObj->SetBoolField(TEXT("found"), true);
            AtmObj->SetObjectField(TEXT("rayleigh_scattering"), MakeColorObj(FLinearColor(
                AC->RayleighScattering.R, AC->RayleighScattering.G, AC->RayleighScattering.B)));
            AtmObj->SetNumberField(TEXT("rayleigh_scattering_scale"), AC->RayleighScatteringScale);
            AtmObj->SetObjectField(TEXT("mie_scattering"), MakeColorObj(FLinearColor(
                AC->MieScattering.R, AC->MieScattering.G, AC->MieScattering.B)));
            AtmObj->SetNumberField(TEXT("mie_scattering_scale"), AC->MieScatteringScale);
        }
        else
        {
            AtmObj->SetBoolField(TEXT("found"), false);

            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("issue_type"), TEXT("NO_SKY_ATMOSPHERE"));
            Issue->SetStringField(TEXT("severity"),   TEXT("info"));
            Issue->SetStringField(TEXT("detail"),
                TEXT("No ASkyAtmosphere found in level. Without it, sky color and aerial perspective "
                     "are not physically simulated. Add a Sky Atmosphere actor for outdoor scenes."));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }

        Root->SetObjectField(TEXT("atmosphere"), AtmObj);
    }

    // -------------------------------------------------------------------------
    // Sky Light — first ASkyLight in the level
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> SLObj = MakeShared<FJsonObject>();

        ASkyLight* SkyLight = nullptr;
        for (TActorIterator<ASkyLight> It(World); It; ++It)
        {
            SkyLight = *It;
            break;
        }

        if (SkyLight)
        {
            USkyLightComponent* SLC = Cast<USkyLightComponent>(SkyLight->GetLightComponent());
            SLObj->SetBoolField(TEXT("found"), true);

            if (SLC)
            {
                SLObj->SetNumberField(TEXT("intensity"), SLC->Intensity);

                // Source type enum → readable string
                const FString SourceTypeStr =
                    (SLC->SourceType == ESkyLightSourceType::SLS_CapturedScene)    ? TEXT("SLS_CapturedScene")    :
                    (SLC->SourceType == ESkyLightSourceType::SLS_SpecifiedCubemap) ? TEXT("SLS_SpecifiedCubemap") :
                                                                                      TEXT("SLS_SpecifiedCubemapAngle");
                SLObj->SetStringField(TEXT("source_type"), SourceTypeStr);

                // Cubemap asset path — access via reflection to avoid UTextureCube header dependency
                FString CubemapPath = TEXT("(none)");
                if (const FObjectProperty* CubeProp = FindFProperty<FObjectProperty>(SLC->GetClass(), TEXT("Cubemap")))
                {
                    const UObject* CubeObj = CubeProp->GetObjectPropertyValue(CubeProp->ContainerPtrToValuePtr<void>(SLC));
                    if (CubeObj) CubemapPath = CubeObj->GetPathName();
                }
                SLObj->SetStringField(TEXT("cubemap_asset"), CubemapPath);

                // Recapture on load — try both UE5.x field name candidates via reflection
                bool bRecapture = false;
                if (const FBoolProperty* P = FindFProperty<FBoolProperty>(SkyLight->GetClass(), TEXT("bRecaptureOnLoad")))
                    bRecapture = P->GetPropertyValue(P->ContainerPtrToValuePtr<void>(SkyLight));
                else if (const FBoolProperty* P2 = FindFProperty<FBoolProperty>(SLC->GetClass(), TEXT("bRealTimeCaptureEnabled")))
                    bRecapture = P2->GetPropertyValue(P2->ContainerPtrToValuePtr<void>(SLC));
                SLObj->SetBoolField(TEXT("recapture_on_load"), bRecapture);

                SLObj->SetBoolField(TEXT("cast_shadows"), SLC->CastShadows);
            }
        }
        else
        {
            SLObj->SetBoolField(TEXT("found"), false);
        }

        Root->SetObjectField(TEXT("sky_light"), SLObj);
    }

    // -------------------------------------------------------------------------
    // Volumetric Cloud — first actor carrying a UVolumetricCloudComponent
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> VCObj = MakeShared<FJsonObject>();

        UVolumetricCloudComponent* VCC = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (UVolumetricCloudComponent* Comp = (*It)->FindComponentByClass<UVolumetricCloudComponent>())
            {
                VCC = Comp;
                break;
            }
        }

        if (VCC)
        {
            VCObj->SetBoolField(TEXT("found"), true);

            // Layer altitudes — use reflection; field names and types vary by UE version
            auto ReadFloatField = [&](const TCHAR* FieldName) -> float
            {
                if (const FFloatProperty* P = FindFProperty<FFloatProperty>(VCC->GetClass(), FieldName))
                    return P->GetPropertyValue(P->ContainerPtrToValuePtr<void>(VCC));
                if (const FDoubleProperty* P = FindFProperty<FDoubleProperty>(VCC->GetClass(), FieldName))
                    return (float)P->GetPropertyValue(P->ContainerPtrToValuePtr<void>(VCC));
                return 0.0f;
            };

            const float LayerBottom = ReadFloatField(TEXT("LayerBottomAltitudeKm"));
            const float LayerHeight = ReadFloatField(TEXT("LayerHeightKm"));
            VCObj->SetNumberField(TEXT("layer_bottom_altitude_km"), LayerBottom);
            VCObj->SetNumberField(TEXT("layer_height_km"),          LayerHeight);
            VCObj->SetNumberField(TEXT("layer_top_altitude_km"),    LayerBottom + LayerHeight);

            // Cloud material — access via reflection to avoid UMaterialInterface dependency issues
            FString MaterialPath = TEXT("(none)");
            if (const FObjectProperty* MatProp = FindFProperty<FObjectProperty>(VCC->GetClass(), TEXT("CloudVolumeMaterial")))
            {
                const UObject* MatObj = MatProp->GetObjectPropertyValue(MatProp->ContainerPtrToValuePtr<void>(VCC));
                if (MatObj) MaterialPath = MatObj->GetPathName();
            }
            VCObj->SetStringField(TEXT("cloud_material"), MaterialPath);

            // Shadow enabled — non-zero ShadowViewSampleCountScale means shadow sampling is active
            const float ShadowScale = ReadFloatField(TEXT("ShadowViewSampleCountScale"));
            VCObj->SetBoolField(TEXT("shadow_enabled"), ShadowScale > 0.0f);
        }
        else
        {
            VCObj->SetBoolField(TEXT("found"), false);
        }

        Root->SetObjectField(TEXT("volumetric_cloud"), VCObj);
    }

    // -------------------------------------------------------------------------
    // Fog — first AExponentialHeightFog in the level
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> FogObj = MakeShared<FJsonObject>();

        AExponentialHeightFog* Fog = nullptr;
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
        {
            Fog = *It;
            break;
        }

        if (Fog && Fog->GetComponent())
        {
            UExponentialHeightFogComponent* FC = Fog->GetComponent();

            FogObj->SetBoolField  (TEXT("found"),          true);
            FogObj->SetNumberField(TEXT("density"),        FC->FogDensity);
            FogObj->SetNumberField(TEXT("height_falloff"), FC->FogHeightFalloff);
            FogObj->SetNumberField(TEXT("start_distance"), FC->StartDistance);
            FogObj->SetNumberField(TEXT("max_opacity"),    FC->FogMaxOpacity);

            // FogInscatteringColor was renamed in UE5.x — try both names via reflection
            FLinearColor InscatterColor = FLinearColor::Black;
            if (const FStructProperty* P = FindFProperty<FStructProperty>(FC->GetClass(), TEXT("FogInscatteringColor")))
                InscatterColor = *P->ContainerPtrToValuePtr<FLinearColor>(FC);
            else if (const FStructProperty* P2 = FindFProperty<FStructProperty>(FC->GetClass(), TEXT("FogInscatteringLuminance")))
                InscatterColor = *P2->ContainerPtrToValuePtr<FLinearColor>(FC);
            FogObj->SetObjectField(TEXT("inscattering_color"), MakeColorObj(InscatterColor));

            // Directional inscattering — direct access (fields stable across UE5 versions)
            FogObj->SetNumberField(TEXT("directional_inscattering_exponent"),
                FC->DirectionalInscatteringExponent);

            FLinearColor DIColor = FLinearColor::Black;
            if (const FStructProperty* P = FindFProperty<FStructProperty>(FC->GetClass(), TEXT("DirectionalInscatteringColor")))
                DIColor = *P->ContainerPtrToValuePtr<FLinearColor>(FC);
            FogObj->SetObjectField(TEXT("directional_inscattering_color"), MakeColorObj(DIColor));

            // AUDIT: FOG_DENSITY_HIGH
            if (FC->FogDensity > 0.1f)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("FOG_DENSITY_HIGH"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Fog density is %.4f (> 0.1). This is extremely dense for most outdoor scenes "
                             "and will aggressively obscure distant geometry. Typical outdoor values: 0.002 - 0.02."),
                        FC->FogDensity));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }
        else
        {
            FogObj->SetBoolField(TEXT("found"), false);
        }

        Root->SetObjectField(TEXT("fog"), FogObj);
    }

    // -------------------------------------------------------------------------
    // Post Process — global unbound volume (backward-compat field)
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> PPObj = MakeShared<FJsonObject>();

        APostProcessVolume* PPV = nullptr;
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
        {
            if ((*It)->bUnbound)
            {
                PPV = *It;
                break;
            }
        }

        if (PPV)
        {
            const FPostProcessSettings& S = PPV->Settings;

            PPObj->SetBoolField(TEXT("found"), true);

            TSharedRef<FJsonObject> BloomObj = MakeShared<FJsonObject>();
            BloomObj->SetNumberField(TEXT("intensity"), S.BloomIntensity);
            BloomObj->SetNumberField(TEXT("threshold"), S.BloomThreshold);
            PPObj->SetObjectField(TEXT("bloom"), BloomObj);

            TSharedRef<FJsonObject> ExpObj = MakeShared<FJsonObject>();
            ExpObj->SetNumberField(TEXT("min_brightness"), S.AutoExposureMinBrightness);
            ExpObj->SetNumberField(TEXT("max_brightness"), S.AutoExposureMaxBrightness);
            PPObj->SetObjectField(TEXT("exposure"), ExpObj);

            TSharedRef<FJsonObject> WBObj = MakeShared<FJsonObject>();
            WBObj->SetNumberField(TEXT("temp"), S.WhiteTemp);
            WBObj->SetNumberField(TEXT("tint"), S.WhiteTint);
            PPObj->SetObjectField(TEXT("white_balance"), WBObj);

            // AUDIT: BLOOM_OVERDRIVEN
            if (S.BloomIntensity > 1.0f)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("BLOOM_OVERDRIVEN"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Bloom intensity is %.2f (> 1.0). Values above 1.0 create persistent visual blowout "
                             "on bright surfaces. Keep <= 1.0 for realistic results; reduce further for stylized looks."),
                        S.BloomIntensity));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }

            // AUDIT: EXPOSURE_RANGE_WIDE
            const float EVRange = S.AutoExposureMaxBrightness - S.AutoExposureMinBrightness;
            if (EVRange > 6.0f)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("EXPOSURE_RANGE_WIDE"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Auto-exposure range is %.1f EV stops (min=%.2f, max=%.2f). Ranges wider than 6 stops "
                             "cause jarring camera adaptation transitions. Typical outdoor range: 1.0 to 3.0 EV."),
                        EVRange, S.AutoExposureMinBrightness, S.AutoExposureMaxBrightness));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }
        else
        {
            PPObj->SetBoolField(TEXT("found"), false);
        }

        Root->SetObjectField(TEXT("post_process"), PPObj);
    }

    // -------------------------------------------------------------------------
    // Post Process — all bounded volumes as an array
    // (blend_radius, blend_weight, priority + key PP settings per volume)
    // -------------------------------------------------------------------------
    {
        TArray<TSharedPtr<FJsonValue>> BoundedArr;

        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
        {
            APostProcessVolume* PPV = *It;
            if (!PPV || PPV->bUnbound) continue;  // skip the global one

            TSharedRef<FJsonObject> VolObj = MakeShared<FJsonObject>();
            VolObj->SetStringField(TEXT("actor_name"),   PPV->GetActorNameOrLabel());
            VolObj->SetNumberField(TEXT("blend_radius"), PPV->BlendRadius);
            VolObj->SetNumberField(TEXT("blend_weight"), PPV->BlendWeight);
            VolObj->SetNumberField(TEXT("priority"),     PPV->Priority);

            const FPostProcessSettings& S = PPV->Settings;

            TSharedRef<FJsonObject> BloomObj = MakeShared<FJsonObject>();
            BloomObj->SetNumberField(TEXT("intensity"), S.BloomIntensity);
            BloomObj->SetNumberField(TEXT("threshold"), S.BloomThreshold);
            VolObj->SetObjectField(TEXT("bloom"), BloomObj);

            TSharedRef<FJsonObject> ExpObj = MakeShared<FJsonObject>();
            ExpObj->SetNumberField(TEXT("min_brightness"), S.AutoExposureMinBrightness);
            ExpObj->SetNumberField(TEXT("max_brightness"), S.AutoExposureMaxBrightness);
            VolObj->SetObjectField(TEXT("exposure"), ExpObj);

            BoundedArr.Add(MakeShared<FJsonValueObject>(VolObj));
        }

        Root->SetArrayField(TEXT("post_process_volumes"), BoundedArr);
    }

    // -------------------------------------------------------------------------
    // Audit summary
    // -------------------------------------------------------------------------
    TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
    AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
    AuditObj->SetArrayField (TEXT("issues"),       Issues);
    Root->SetObjectField(TEXT("audit"), AuditObj);

    bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("weather"), TEXT("current"), Root);
    if (bOK)
    {
        UE_LOG(LogTemp, Log,
            TEXT("ForgeWeather: Exported -> weather/current.json (%d issue(s))"), Issues.Num());
        UpdateIndexFile();
    }
    return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeWeatherCapture::UpdateIndexFile()
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
    {
        if (Found->IsValid() && (*Found)->Type == EJson::Object)
            Captures = (*Found)->AsObject();
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
    Section->SetStringField(TEXT("file"),         TEXT("weather/current.json"));
    Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    Captures->SetObjectField(TEXT("weather"), Section);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
