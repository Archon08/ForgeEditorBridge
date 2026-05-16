#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "AnimationHandler.generated.h"

/**
 * AnimationHandler — domain "animation"
 *
 * Ported from UmgMcpSequencerCommands.cpp (MIT).
 * Works exclusively on UWidgetBlueprint animations (UWidgetAnimation / UMovieScene).
 *
 * CRITICAL: New animations MUST be added to Blueprint->WidgetVariableNameToGuidMap.
 *
 * Actions:
 *   create_animation         → animation_name → creates new UWidgetAnimation
 *   delete_animation         → animation_name → removes animation + GUID entry
 *   set_animation_scope      → animation_name → AttentionManager scope
 *   set_widget_scope         → widget_name → which widget's tracks to target
 *   set_property_keys        → widget_name, property_name, keyframes[] → Float/Color/Vector2D
 *   remove_property_track    → widget_name, property_name
 *   get_all_animations       → lists animation names in target WBP
 *   get_animation_keyframes  → animation_name, widget_name, property_name → keyframe array
 *   get_animated_widgets     → animation_name → list of animated widget names
 *   read_animation_capture   → (no params) → reads animation/skeletal_data.json from capture output
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAnimationHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("animation"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			// UMG widget animation actions (existing)
			TEXT("set_animation_scope"), TEXT("set_widget_scope"),
			TEXT("get_all_animations"), TEXT("get_animated_widgets"), TEXT("get_animation_keyframes"),
			TEXT("create_animation"), TEXT("delete_animation"),
			TEXT("set_property_keys"), TEXT("remove_property_track"),
			TEXT("read_animation_capture"),
			// Phase 1d: skeletal animation asset management
			TEXT("set_motion_matching_db"),
			TEXT("set_control_rig_link"),
			TEXT("create_selection_set"),
			TEXT("set_retarget_lod"),
		};
	}
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    // Phase 1d: skeletal animation asset management
    FBridgeResult Action_SetMotionMatchingDB  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetControlRigLink    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateSelectionSet   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetRetargetLOD       (TSharedPtr<FJsonObject> Params);
};
