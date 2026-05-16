#include "Handlers/MediaHandler.h"
#include "ForgeAISubsystem.h"

// ---- Media -----------------------------------------------------------------
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "FileMediaSource.h"
#include "StreamMediaSource.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_EDITOR
#include "Editor.h"                 // GEditor
#include "ScopedTransaction.h"
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("media");

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UMediaHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("create_media_player"))  return Action_CreateMediaPlayer(Params);
	if (Action == TEXT("create_media_texture")) return Action_CreateMediaTexture(Params);
	if (Action == TEXT("create_media_source"))  return Action_CreateMediaSource(Params);
	if (Action == TEXT("get_media_info"))       return Action_GetMediaInfo(Params);
	if (Action == TEXT("open_source"))          return Action_OpenSource(Params);
	if (Action == TEXT("play"))                 return Action_Play(Params);
	if (Action == TEXT("pause"))                return Action_Pause(Params);
	if (Action == TEXT("stop"))                 return Action_Stop(Params);
	if (Action == TEXT("seek"))                 return Action_Seek(Params);
	if (Action == TEXT("set_loop"))             return Action_SetLoop(Params);
	if (Action == TEXT("get_position"))         return Action_GetPosition(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown media action '%s'"), *Action),
		TEXT("media capabilities"));
}

// ---------------------------------------------------------------------------
// Helper: split asset path into package path and asset name
// ---------------------------------------------------------------------------

static void SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	int32 LastSlash = INDEX_NONE;
	AssetPath.FindLastChar('/', LastSlash);

	if (LastSlash != INDEX_NONE)
	{
		OutPackagePath = AssetPath;
		OutAssetName = AssetPath.Mid(LastSlash + 1);
	}
	else
	{
		OutPackagePath = AssetPath;
		OutAssetName = AssetPath;
	}
}

// ---------------------------------------------------------------------------
// create_media_player
// ---------------------------------------------------------------------------

FBridgeResult UMediaHandler::Action_CreateMediaPlayer(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_media_player");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

#if WITH_EDITOR
	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Media Player")));

	FString PackagePath, AssetName;
	SplitAssetPath(AssetPath, PackagePath, AssetName);

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
	}

	UMediaPlayer* Asset = NewObject<UMediaPlayer>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Asset)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create UMediaPlayer object"));
	}

	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class"), TEXT("UMediaPlayer"));

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("MediaPlayer created at '%s'"), *AssetPath),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// create_media_texture
// ---------------------------------------------------------------------------

FBridgeResult UMediaHandler::Action_CreateMediaTexture(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_media_texture");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	FString MediaPlayerPath;
	if (!Params->TryGetStringField(TEXT("media_player"), MediaPlayerPath) || MediaPlayerPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'media_player'"));
	}

#if WITH_EDITOR
	// Load the media player asset
	UMediaPlayer* Player = LoadObject<UMediaPlayer>(nullptr, *MediaPlayerPath);
	if (!Player)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("MediaPlayer not found at '%s'"), *MediaPlayerPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Media Texture")));

	FString PackagePath, AssetName;
	SplitAssetPath(AssetPath, PackagePath, AssetName);

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
	}

	UMediaTexture* Asset = NewObject<UMediaTexture>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Asset)
	{
		return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create UMediaTexture object"));
	}

	Asset->SetMediaPlayer(Player);
	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("media_player"), MediaPlayerPath);
	Data->SetStringField(TEXT("class"), TEXT("UMediaTexture"));

	return MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("MediaTexture created at '%s' linked to '%s'"), *AssetPath, *MediaPlayerPath),
		Data);
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// create_media_source
// ---------------------------------------------------------------------------

FBridgeResult UMediaHandler::Action_CreateMediaSource(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_media_source");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	}

	FString Type;
	if (!Params->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'type' (\"file\" or \"stream\")"));
	}

#if WITH_EDITOR
	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Media Source")));

	FString PackagePath, AssetName;
	SplitAssetPath(AssetPath, PackagePath, AssetName);

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeError(DOMAIN, Action, 3000,
			FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
	}

	if (Type == TEXT("file"))
	{
		FString FilePath;
		if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'file_path' for file source"));
		}

		UFileMediaSource* Source = NewObject<UFileMediaSource>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Source)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create UFileMediaSource object"));
		}

		Source->SetFilePath(FilePath);
		Source->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Source);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("type"), TEXT("file"));
		Data->SetStringField(TEXT("file_path"), FilePath);
		Data->SetStringField(TEXT("class"), TEXT("UFileMediaSource"));

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("FileMediaSource created at '%s' pointing to '%s'"), *AssetPath, *FilePath),
			Data);
	}
	else if (Type == TEXT("stream"))
	{
		FString StreamUrl;
		if (!Params->TryGetStringField(TEXT("stream_url"), StreamUrl) || StreamUrl.IsEmpty())
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'stream_url' for stream source"));
		}

		UStreamMediaSource* Source = NewObject<UStreamMediaSource>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Source)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create UStreamMediaSource object"));
		}

		Source->StreamUrl = StreamUrl;
		Source->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Source);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("type"), TEXT("stream"));
		Data->SetStringField(TEXT("stream_url"), StreamUrl);
		Data->SetStringField(TEXT("class"), TEXT("UStreamMediaSource"));

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("StreamMediaSource created at '%s' url='%s'"), *AssetPath, *StreamUrl),
			Data);
	}
	else
	{
		return MakeError(DOMAIN, Action, 1001,
			FString::Printf(TEXT("Unknown source type '%s' — expected 'file' or 'stream'"), *Type));
	}
#else
	return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// get_media_info
// ---------------------------------------------------------------------------

FBridgeResult UMediaHandler::Action_GetMediaInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_media_info");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
	}

	UMediaPlayer* Player = LoadObject<UMediaPlayer>(nullptr, *AssetPath);
	if (!Player)
	{
		FString Suffix = AssetPath + TEXT(".") + AssetPath.RightChop(AssetPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1);
		Player = LoadObject<UMediaPlayer>(nullptr, *Suffix);
	}
	if (!Player)
	{
		return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No UMediaPlayer found at '%s'"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"),       Player->GetName());
	Data->SetBoolField(TEXT("is_looping"),   Player->IsLooping());
	Data->SetBoolField(TEXT("plays_on_open"), Player->PlayOnOpen);

	// Current URL (if a source is open)
	const FString URL = Player->GetUrl();
	if (!URL.IsEmpty())
	{
		Data->SetStringField(TEXT("current_url"), URL);
	}

	// Native player name
	const FName NativePlayer = Player->GetPlayerName();
	Data->SetStringField(TEXT("player_name"), NativePlayer.IsNone() ? TEXT("(none)") : NativePlayer.ToString());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("get_media_info: '%s' looping=%s"), *AssetPath, Player->IsLooping() ? TEXT("true") : TEXT("false")));
	R.ExtraData = OutStr;
	return R;
}

// ===========================================================================
// Phase 3: media playback control
// ===========================================================================

#include "MediaPlayer.h"
#include "MediaSource.h"
#include "FileMediaSource.h"
#include "StreamMediaSource.h"
#include "Misc/Timespan.h"

namespace
{
	UMediaPlayer* LoadMediaPlayerAt(const FString& Path)
	{
		if (UMediaPlayer* P = LoadObject<UMediaPlayer>(nullptr, *Path)) return P;
		const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		return LoadObject<UMediaPlayer>(nullptr, *Suffix);
	}
	UMediaSource* LoadMediaSourceAt(const FString& Path)
	{
		if (UMediaSource* S = LoadObject<UMediaSource>(nullptr, *Path)) return S;
		const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		return LoadObject<UMediaSource>(nullptr, *Suffix);
	}
}

FBridgeResult UMediaHandler::Action_OpenSource(TSharedPtr<FJsonObject> Params)
{
	FString PlayerPath, SourcePath;
	if (!Params->TryGetStringField(TEXT("media_player_path"), PlayerPath) || PlayerPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("open_source"), 1000, TEXT("'media_player_path' is required"));
	if (!Params->TryGetStringField(TEXT("media_source_path"), SourcePath) || SourcePath.IsEmpty())
		return MakeError(DOMAIN, TEXT("open_source"), 1000, TEXT("'media_source_path' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(PlayerPath);
	if (!P) return MakeError(DOMAIN, TEXT("open_source"), 2000, TEXT("MediaPlayer not found"));
	UMediaSource* S = LoadMediaSourceAt(SourcePath);
	if (!S) return MakeError(DOMAIN, TEXT("open_source"), 2000, TEXT("MediaSource not found"));
	const bool bOk = P->OpenSource(S);
	return bOk
		? MakeSuccess(DOMAIN, TEXT("open_source"), FString::Printf(TEXT("Opened '%s' on '%s'"), *SourcePath, *PlayerPath))
		: MakeError(DOMAIN, TEXT("open_source"), 3000, TEXT("OpenSource returned false"));
}

FBridgeResult UMediaHandler::Action_Play(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("play"), 1000, TEXT("'media_player_path' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("play"), 2000, TEXT("MediaPlayer not found"));
	P->Play();
	return MakeSuccess(DOMAIN, TEXT("play"), TEXT("Play requested"));
}

FBridgeResult UMediaHandler::Action_Pause(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("pause"), 1000, TEXT("'media_player_path' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("pause"), 2000, TEXT("MediaPlayer not found"));
	P->Pause();
	return MakeSuccess(DOMAIN, TEXT("pause"), TEXT("Pause requested"));
}

FBridgeResult UMediaHandler::Action_Stop(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("stop"), 1000, TEXT("'media_player_path' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("stop"), 2000, TEXT("MediaPlayer not found"));
	P->Close();
	return MakeSuccess(DOMAIN, TEXT("stop"), TEXT("Closed"));
}

FBridgeResult UMediaHandler::Action_Seek(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	double Seconds = 0.0;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("seek"), 1000, TEXT("'media_player_path' is required"));
	if (!Params->TryGetNumberField(TEXT("time_seconds"), Seconds))
		return MakeError(DOMAIN, TEXT("seek"), 1000, TEXT("'time_seconds' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("seek"), 2000, TEXT("MediaPlayer not found"));
	P->Seek(FTimespan::FromSeconds(Seconds));
	return MakeSuccess(DOMAIN, TEXT("seek"),
		FString::Printf(TEXT("Seek to %.3fs"), Seconds));
}

FBridgeResult UMediaHandler::Action_SetLoop(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	bool bLoop = true;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_loop"), 1000, TEXT("'media_player_path' is required"));
	if (!Params->TryGetBoolField(TEXT("loop"), bLoop))
		return MakeError(DOMAIN, TEXT("set_loop"), 1000, TEXT("'loop' (bool) is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("set_loop"), 2000, TEXT("MediaPlayer not found"));
	P->SetLooping(bLoop);
	return MakeSuccess(DOMAIN, TEXT("set_loop"),
		FString::Printf(TEXT("loop=%s"), bLoop ? TEXT("true") : TEXT("false")));
}

FBridgeResult UMediaHandler::Action_GetPosition(TSharedPtr<FJsonObject> Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("media_player_path"), Path) || Path.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_position"), 1000, TEXT("'media_player_path' is required"));
	UMediaPlayer* P = LoadMediaPlayerAt(Path);
	if (!P) return MakeError(DOMAIN, TEXT("get_position"), 2000, TEXT("MediaPlayer not found"));
	const FTimespan Pos = P->GetTime();
	const FTimespan Dur = P->GetDuration();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("position_seconds"), Pos.GetTotalSeconds());
	Data->SetNumberField(TEXT("duration_seconds"), Dur.GetTotalSeconds());
	Data->SetBoolField(TEXT("is_playing"), P->IsPlaying());
	Data->SetBoolField(TEXT("is_paused"), P->IsPaused());
	return MakeSuccess(DOMAIN, TEXT("get_position"),
		FString::Printf(TEXT("%.3fs / %.3fs"), Pos.GetTotalSeconds(), Dur.GetTotalSeconds()), Data);
}
