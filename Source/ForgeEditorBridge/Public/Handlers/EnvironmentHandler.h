#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "EnvironmentHandler.generated.h"

/**
 * EnvironmentHandler — domain "environment"  (v0.1.0 / UE 5.7)
 *
 * Controls the atmospheric/sky layer: SkyAtmosphere, ExponentialHeightFog,
 * VolumetricCloud, and SkyLight actors.  Complements the "lighting" domain
 * (which handles point/spot/directional lights) and "post_process" domain
 * (which handles post-process volumes).
 *
 * For all "set_*" actions, actor_label is optional.  If omitted or set to
 * "first", the first matching actor in the level is targeted.
 *
 * Actions:
 *   set_sky_atmosphere   → actor_label, rayleigh_scattering_scale,
 *                          mie_scattering_scale, mie_absorption_scale,
 *                          aerial_perspective_view_distance_scale,
 *                          sky_luminance_factor{r,g,b}
 *   set_fog              → actor_label, fog_density, fog_height_falloff,
 *                          fog_inscattering_color{r,g,b}, start_distance
 *   set_volumetric_clouds → actor_label, layer_bottom_altitude_km,
 *                           layer_height_km, tracing_max_distance_km,
 *                           cloud_material_path (optional asset path)
 *   set_sky_light        → actor_label, intensity, light_color{r,g,b},
 *                          source_type ("Captured"|"SpecifiedCubemap"),
 *                          cubemap_path (optional, required for SpecifiedCubemap)
 *   apply_time_of_day    → time_hours (0–24), auto_adjust_fog (bool, default true)
 *                          Rotates the first DirectionalLight sun pitch.
 *                          When auto_adjust_fog=true also scales fog density.
 *   get_environment_info → (no params) — current state of all sky/fog/cloud/skylight
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEnvironmentHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("environment"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("set_sky_atmosphere"),
			TEXT("set_fog"),
			TEXT("set_volumetric_clouds"),
			TEXT("set_sky_light"),
			TEXT("apply_time_of_day"),
			TEXT("get_environment_info"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_SetSkyAtmosphere   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetFog             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetVolumetricClouds(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetSkyLight        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ApplyTimeOfDay     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetEnvironmentInfo (TSharedPtr<FJsonObject> Params);

	static UWorld* GetEditorWorld();
};
