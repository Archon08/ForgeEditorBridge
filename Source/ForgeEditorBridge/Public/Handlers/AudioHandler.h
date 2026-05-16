#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "AudioHandler.generated.h"

/**
 * AudioHandler — domain "audio"  (v0.12.0 / Wave J)
 *
 * Creates and authors SoundCue and MetaSound assets programmatically.
 *
 * Actions:
 *   create_cue    → asset_path (string), sound_wave_path (string, optional)
 *                   Creates a new USoundCue. If sound_wave_path is given,
 *                   wires a SoundNodeWavePlayer referencing that wave.
 *
 *   add_ms_node   → asset_path (string), node_class (string), node_name (string, optional)
 *                   Adds a MetaSound graph node by class name.
 *
 *   connect_ms    → asset_path (string), source_node (string), source_pin (string),
 *                   dest_node (string), dest_pin (string)
 *                   Connects two MetaSound graph node pins.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAudioHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("audio"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_cue"), TEXT("add_ms_node"), TEXT("connect_ms"), TEXT("set_attenuation"), TEXT("create_sound_class"), TEXT("create_sound_mix"), TEXT("place_audio_volume"), TEXT("set_concurrency"), TEXT("create_dialogue_wave"), TEXT("list_sound_cues"), TEXT("get_cue_info"), TEXT("create_sound_cue"), TEXT("create_attenuation"), TEXT("set_reverb"), TEXT("set_mix_class"), TEXT("create_metasound"), TEXT("set_metasound_param"), TEXT("add_metasound_input") }; }
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateCue          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddMSNode          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ConnectMS          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAttenuation     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateSoundClass   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateSoundMix     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_PlaceAudioVolume   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetConcurrency     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateDialogueWave (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListSoundCues      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetCueInfo         (TSharedPtr<FJsonObject> Params);
	// Phase 1b additions
	FBridgeResult Action_CreateAttenuation  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetReverb          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMixClass        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateMetaSound    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMetaSoundParam  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddMetaSoundInput  (TSharedPtr<FJsonObject> Params);
};
