#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "LightingHandler.generated.h"

/**
 * LightingHandler — domain "lighting"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   create_light         → light_type ("directional"|"point"|"spot"|"rect", default "point"),
 *                          label (string, optional actor label),
 *                          x (float), y (float), z (float),
 *                          pitch (float), yaw (float), roll (float),
 *                          intensity (float, default 1.0)
 *
 *   set_light_intensity  → label (string — actor label of an ALight in the current level),
 *                          intensity (float)
 *
 *   create_camera        → label (string, optional),
 *                          x (float), y (float), z (float),
 *                          pitch (float), yaw (float), roll (float),
 *                          focal_length (float, mm — default 35),
 *                          aperture (float — default 2.8)
 *
 *   set_light_color      → actor_path (string — actor label), color {r,g,b} (0-1 float),
 *                          srgb (bool, default true)
 *
 *   set_light_attenuation → actor_path (string — actor label), radius (float)
 *
 *   create_sky_light     → x (float), y (float), z (float),
 *                          intensity (float, default 1.0),
 *                          cubemap (string — asset path, optional),
 *                          label (string, optional)
 *
 *   set_volumetric_fog   → actor_path (string — ExponentialHeightFog actor label),
 *                          enabled (bool), scattering_distribution (float),
 *                          albedo {r,g,b} (0-1 float), extinction_scale (float)
 *
 *   bake_lighting        → quality (string — "Preview"|"Medium"|"High"|"Production", default "Preview")
 *
 *   delete_light         → label (string — actor label of any ALight in the current level)
 *                          Destroys the actor. Irreversible in non-undoable editor context.
 *
 *   list_lights          → type (string, optional — "directional"|"point"|"spot"|"rect"|"sky"; omit for all)
 *                          Returns all matching ALight actors: label, type, location, intensity, color.
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULightingHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("lighting"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_light"), TEXT("set_light_intensity"), TEXT("create_camera"), TEXT("set_light_color"), TEXT("set_light_attenuation"), TEXT("create_sky_light"), TEXT("set_volumetric_fog"), TEXT("bake_lighting"), TEXT("delete_light"), TEXT("list_lights"), TEXT("delete_sky_light") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateLight         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLightIntensity   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateCamera        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLightColor       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLightAttenuation (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateSkyLight      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetVolumetricFog    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_BakeLighting        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DeleteLight         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListLights          (TSharedPtr<FJsonObject> Params);

	/** Helper: find an actor by label in the current editor world. */
	AActor* FindActorByLabel(UWorld* World, const FString& Label) const;
};
