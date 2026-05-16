#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "SequencerHandler.generated.h"

/**
 * SequencerHandler — domain "sequencer"  (v0.12.0 / Wave J)
 *
 * Programmatic cinematic authoring via Level Sequences.
 *
 * Actions:
 *   create_sequence → asset_path (string)
 *                     Creates a new ULevelSequence asset.
 *
 *   add_track       → asset_path (string), track_type (string), binding_name (string, optional)
 *                     Adds a track (e.g. "Transform", "Float", "Event", "Audio", "Fade")
 *                     to the movie scene or to a named possessable binding.
 *
 *   add_key         → asset_path (string), track_type (string), binding_name (string, optional),
 *                     time (float, seconds), value (string)
 *                     Adds a keyframe at the given time with the given value.
 */
UCLASS()
class FORGEEDITORBRIDGE_API USequencerHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("sequencer"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_sequence"), TEXT("add_track"), TEXT("add_key"), TEXT("add_camera_cut"), TEXT("add_possessable"), TEXT("set_playback_range"), TEXT("remove_track"), TEXT("add_section"), TEXT("add_subsequence"), TEXT("add_event_key"), TEXT("add_audio_track"), TEXT("get_sequence_topology"), TEXT("create_level_sequence"), TEXT("add_actor_track"), TEXT("add_keyframe"), TEXT("bake_transform"), TEXT("add_spawnable"), TEXT("export_fbx"), TEXT("remove_section"), TEXT("remove_possessable"), TEXT("remove_spawnable"), TEXT("remove_camera_cut") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateSequence (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddTrack       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddKey          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddCameraCut    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddPossessable  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPlaybackRange(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveTrack        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddSection         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddSubsequence     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddEventKey        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddAudioTrack      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetSequenceTopology(TSharedPtr<FJsonObject> Params);
	// Phase 1b additions
	FBridgeResult Action_CreateLevelSequence(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddActorTrack      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddKeyframe        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_BakeTransform      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddSpawnable       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExportFBX          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveSection      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemovePossessable  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveSpawnable    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveCameraCut    (TSharedPtr<FJsonObject> Params);

	class ULevelSequence* LoadLevelSequence(const FString& AssetPath, FBridgeResult& Result);
};
