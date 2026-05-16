#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "MediaHandler.generated.h"

/**
 * MediaHandler — domain "media"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   create_media_player  → asset_path (string — content browser path)
 *
 *   create_media_texture → asset_path (string), media_player (string — asset path of UMediaPlayer)
 *
 *   create_media_source  → asset_path (string), type ("file"|"stream"),
 *                          file_path (string, if file), stream_url (string, if stream)
 *
 *   get_media_info       → asset_path (string — UMediaPlayer asset)
 *                          Returns: is_looping, playback_url (if file/stream source is linked),
 *                          player_name, and whether a linked MediaTexture exists.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMediaHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("media"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_media_player"), TEXT("create_media_texture"), TEXT("create_media_source"), TEXT("get_media_info"), TEXT("open_source"), TEXT("play"), TEXT("pause"), TEXT("stop"), TEXT("seek"), TEXT("set_loop"), TEXT("get_position") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateMediaPlayer  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateMediaTexture (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateMediaSource  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMediaInfo       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_OpenSource         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Play               (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Pause              (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Stop               (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Seek               (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLoop            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPosition        (TSharedPtr<FJsonObject> Params);
};
