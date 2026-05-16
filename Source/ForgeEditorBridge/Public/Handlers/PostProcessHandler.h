#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "PostProcessHandler.generated.h"

/**
 * PostProcessHandler — domain "post_process"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   create_pp_volume       → location {x,y,z}, extent {x,y,z} (optional),
 *                            infinite (bool, default false)
 *
 *   set_bloom              → actor_path (string — actor label), method (int, optional),
 *                            intensity (float), threshold (float, optional),
 *                            size_scale (float, optional)
 *
 *   set_exposure           → actor_path, method ("Manual"|"Auto", optional),
 *                            compensation (float), min_brightness (float, optional),
 *                            max_brightness (float, optional)
 *
 *   set_color_grading      → actor_path, temperature (float, optional), tint (float, optional),
 *                            saturation {r,g,b,a} (optional), contrast {r,g,b,a} (optional),
 *                            gain {r,g,b,a} (optional), lut_texture (string, optional)
 *
 *   set_ambient_occlusion  → actor_path, intensity (float), radius (float, optional),
 *                            quality (float, optional), power (float, optional)
 *
 *   read_weather_capture   → (no params) → reads weather/current.json from capture output
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPostProcessHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("post_process"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_pp_volume"), TEXT("set_bloom"), TEXT("set_exposure"), TEXT("set_color_grading"), TEXT("set_ambient_occlusion"), TEXT("read_weather_capture"), TEXT("set_pp_property"), TEXT("set_motion_blur"), TEXT("set_chromatic_aberration"), TEXT("set_vignette"), TEXT("set_lumen") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreatePPVolume     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetBloom           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetExposure        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetColorGrading    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAmbientOcclusion(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPPProperty       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMotionBlur       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetChromaticAberration(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetVignette         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLumen            (TSharedPtr<FJsonObject> Params);

	/** Helper: find an actor by label in the current editor world. */
	AActor* FindActorByLabel(UWorld* World, const FString& Label) const;
};
