#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "CameraHandler.generated.h"

/**
 * CameraHandler — domain "camera"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   create_cinecam      → location {x,y,z}, rotation {pitch,yaw,roll} (optional),
 *                         focal_length (float, default 35.0)
 *
 *   set_filmback        → actor_path, preset ("Super16"|"Super35"|"IMAX"|"VistaVision"|"Custom"),
 *                         sensor_width (float, if Custom), sensor_height (float, if Custom)
 *
 *   set_focus           → actor_path, method ("Manual"|"Tracking", default "Manual"),
 *                         distance (float), aperture (float, optional)
 *
 *   set_look_at         → actor_path, target {x,y,z}
 *
 *   create_camera_rig   → type ("rail"|"crane"), location {x,y,z},
 *                         rail_length (float, for rail), crane_length (float, for crane)
 *
 *   get_camera_info     → actor_path (string — CineCameraActor label)
 *                         Returns focal_length, aperture, sensor size, focus settings, location, rotation.
 *
 *   list_cameras        → (no params — lists all ACineCameraActor in the level)
 *                         Returns array of {label, location, focal_length}.
 *
 *   create_camera_shake → asset_path (string), shake_type ("MatineeCameraShake", default)
 *                         Creates a UCameraShakeBase Blueprint asset.
 *
 *   preview_shake       → asset_path (string — shake asset to preview)
 *                         Python-dispatched: viewport shake preview is runtime-only (PIE).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UCameraHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("camera"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_cinecam"), TEXT("set_filmback"), TEXT("set_focus"), TEXT("set_look_at"), TEXT("create_camera_rig"), TEXT("get_camera_info"), TEXT("list_cameras"), TEXT("create_camera_shake"), TEXT("preview_shake") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateCineCam   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetFilmback     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetFocus        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLookAt       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateCameraRig (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetCameraInfo    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListCameras      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateCameraShake(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_PreviewShake     (TSharedPtr<FJsonObject> Params);

	/** Helper: find an actor by label in the current editor world. */
	AActor* FindActorByLabel(UWorld* World, const FString& Label) const;
};
