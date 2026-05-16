#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeWeatherCapture.generated.h"

/**
 * v1.3 — Weather & Environment Capture
 *
 * Exports the visual world state: directional light (sun), sky atmosphere, exponential
 * height fog, and the global unbound post-process volume. Allows an AI consumer to audit and
 * recommend lighting/atmosphere improvements.
 *
 * Output: {ProjectRoot}/Forge/ue-context/weather/current.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.weather_capture.export_weather_state()
 *
 * Exported JSON fields:
 *   generated, level_name,
 *   sun{found, actor_name, intensity, color{r,g,b}, rotation{pitch,yaw,roll}},
 *   moon{found, actor_name, intensity, color{r,g,b}, rotation{pitch,yaw,roll}},
 *   atmosphere{found, rayleigh_scattering{r,g,b}, rayleigh_scattering_scale,
 *              mie_scattering{r,g,b}, mie_scattering_scale},
 *   sky_light{found, intensity, source_type, cubemap_asset, recapture_on_load, cast_shadows},
 *   volumetric_cloud{found, layer_bottom_altitude_km, layer_top_altitude_km,
 *                    layer_height_km, cloud_material, shadow_enabled},
 *   fog{found, density, height_falloff, start_distance, inscattering_color{r,g,b},
 *       directional_inscattering_color{r,g,b}, directional_inscattering_exponent,
 *       max_opacity},
 *   post_process{found, bloom{intensity,threshold},
 *                exposure{min_brightness,max_brightness},
 *                white_balance{temp,tint}},
 *   post_process_volumes[]{actor_name, blend_radius, blend_weight, priority,
 *                          bloom{intensity,threshold}, exposure{min,max}},
 *   audit{total_issues, issues[]{issue_type, severity, detail}}
 *
 * Audit rules (5):
 *   NO_DIRECTIONAL_LIGHT  — no ADirectionalLight in level (unlit or missing sun)
 *   NO_SKY_ATMOSPHERE     — no ASkyAtmosphere in level (no physical sky)
 *   FOG_DENSITY_HIGH      — fog density > 0.1 (too dense for most outdoor scenes)
 *   BLOOM_OVERDRIVEN      — bloom intensity > 1.0 (visual blowout on bright surfaces)
 *   EXPOSURE_RANGE_WIDE   — auto-exposure range > 6 EV stops (jarring adaptation)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeWeatherCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Capture the current weather/environment state from the editor level.
     * Writes weather/current.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportWeatherState();

private:
    FString OutputDir;

    // READ-MERGE-WRITE index.json to add/update the "weather" section
    void UpdateIndexFile();
};
