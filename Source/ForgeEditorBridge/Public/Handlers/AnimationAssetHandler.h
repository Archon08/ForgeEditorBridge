#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "AnimationAssetHandler.generated.h"

/**
 * AnimationAssetHandler — domain "animation_asset"  (Phase 4 / UE 5.7)
 *
 * Actions:
 *   create_montage        → asset_path (string), skeleton (string), source_sequence (string, optional)
 *   add_notify            → asset_path (string), notify_class (string), time (float), track_name (string)
 *   create_blendspace     → asset_path (string), skeleton (string), axis_x/axis_y (string),
 *                           min_x/max_x/min_y/max_y (float)
 *   add_blendspace_sample → asset_path (string), sequence (string), x (float), y (float)
 *   get_sequence_info     → asset_path (string) → play_length, frame_count, etc.
 *   get_montage_sections  → asset_path (string) → CompositeSections, slot_count
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAnimationAssetHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("animation_asset"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_montage"), TEXT("add_notify"), TEXT("create_blendspace"), TEXT("add_blendspace_sample"), TEXT("get_sequence_info"), TEXT("get_montage_sections"), TEXT("remove_notify"), TEXT("remove_blendspace_sample") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateMontage       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddNotify           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateBlendSpace    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddBlendSpaceSample (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetSequenceInfo     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMontageSections  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveNotify        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveBlendSpaceSample(TSharedPtr<FJsonObject> Params);
};
