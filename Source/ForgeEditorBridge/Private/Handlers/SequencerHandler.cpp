#include "Handlers/SequencerHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- Level Sequence --------------------------------------------------------
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"

// ---- Camera cut -------------------------------------------------------------
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"

// ---- Editor (for world access) ----------------------------------------------
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// ---- Tracks ----------------------------------------------------------------
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Sections/MovieSceneAudioSection.h"

// ---- Sections (for keyframing) ---------------------------------------------
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"

// ---- Channels --------------------------------------------------------------
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"

// ---- Editor ----------------------------------------------------------------
// #include "LevelSequenceFactoryNew.h"  // UE 5.7: removed — using nullptr factory

// ---- Reflection (for MovieScene init fallback) -----------------------------
#include "UObject/UnrealType.h"

// ---- Blueprint (for add_spawnable) -----------------------------------------
#include "Engine/Blueprint.h"
// TActorIterator lives in EngineUtils.h (already included above). The path
// "Engine/ActorIterator.h" does not exist in UE 5.7.

// ---- Job store -------------------------------------------------------------
#include "BridgeSessionStore.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void USequencerHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("sequencer"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_sequence")) return Action_CreateSequence(Params);
	if (Action == TEXT("add_track"))       return Action_AddTrack(Params);
	if (Action == TEXT("add_key"))          return Action_AddKey(Params);
	if (Action == TEXT("add_camera_cut"))   return Action_AddCameraCut(Params);
	if (Action == TEXT("add_possessable"))  return Action_AddPossessable(Params);
	if (Action == TEXT("set_playback_range")) return Action_SetPlaybackRange(Params);
	if (Action == TEXT("remove_track"))         return Action_RemoveTrack(Params);
	if (Action == TEXT("add_section"))          return Action_AddSection(Params);
	if (Action == TEXT("add_subsequence"))      return Action_AddSubsequence(Params);
	if (Action == TEXT("add_event_key"))        return Action_AddEventKey(Params);
	if (Action == TEXT("add_audio_track"))      return Action_AddAudioTrack(Params);
	if (Action == TEXT("get_sequence_topology"))return Action_GetSequenceTopology(Params);
	// Phase 1b
	if (Action == TEXT("create_level_sequence")) return Action_CreateLevelSequence(Params);
	if (Action == TEXT("add_actor_track"))       return Action_AddActorTrack(Params);
	if (Action == TEXT("add_keyframe"))          return Action_AddKeyframe(Params);
	if (Action == TEXT("bake_transform"))        return Action_BakeTransform(Params);
	if (Action == TEXT("add_spawnable"))         return Action_AddSpawnable(Params);
	if (Action == TEXT("export_fbx"))            return Action_ExportFBX(Params);
	if (Action == TEXT("remove_section"))        return Action_RemoveSection(Params);
	if (Action == TEXT("remove_possessable"))    return Action_RemovePossessable(Params);
	if (Action == TEXT("remove_spawnable"))      return Action_RemoveSpawnable(Params);
	if (Action == TEXT("remove_camera_cut"))     return Action_RemoveCameraCut(Params);

	return MakeError(TEXT("sequencer"), Action, 1001,
		FString::Printf(TEXT("Unknown sequencer action '%s'"), *Action),
		TEXT("Valid: create_sequence, add_track, add_key, add_camera_cut, add_possessable, set_playback_range, remove_track, add_section, add_subsequence, add_event_key, add_audio_track, get_sequence_topology, create_level_sequence, add_actor_track, add_keyframe, bake_transform, add_spawnable"));
}

// ---------------------------------------------------------------------------
// create_sequence
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_CreateSequence(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("create_sequence"));

	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_sequence: 'asset_path' is required (e.g. '/Game/Cinematics/LS_Intro')");
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// UE 5.7: ULevelSequenceFactoryNew header removed — pass nullptr factory
	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    ULevelSequence::StaticClass(), nullptr);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_sequence: failed to create LevelSequence at '%s'"), *AssetPath);
		return Result;
	}

	// Ensure the LevelSequence has a valid MovieScene after creation
	ULevelSequence* NewSequence = Cast<ULevelSequence>(CreatedAsset);
	if (NewSequence && !NewSequence->GetMovieScene())
	{
		// Fallback: set MovieScene via reflection since the member may be protected in UE 5.7
		UMovieScene* MS = NewObject<UMovieScene>(NewSequence, NAME_None, RF_Transactional);
		FProperty* MSProp = ULevelSequence::StaticClass()->FindPropertyByName(TEXT("MovieScene"));
		if (MSProp)
		{
			FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(MSProp);
			if (ObjProp)
			{
				ObjProp->SetObjectPropertyValue(
					ObjProp->ContainerPtrToValuePtr<void>(NewSequence), MS);
			}
		}
	}

	CreatedAsset->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("LevelSequence created at %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_track
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddTrack(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("add_track"));

	FString AssetPath, TrackType;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("track_type"), TrackType) || TrackType.IsEmpty())
	{
		Result.Message = TEXT("add_track: 'asset_path' and 'track_type' "
		                      "(Transform|Float|Event|Audio|Fade|Bool) are required");
		return Result;
	}

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, Result);
	if (!Sequence) return Result;

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		Result.Message = TEXT("add_track: LevelSequence has no MovieScene");
		return Result;
	}

	// Determine track class from type string
	UClass* TrackClass = nullptr;
	if      (TrackType == TEXT("Transform")) TrackClass = UMovieScene3DTransformTrack::StaticClass();
	else if (TrackType == TEXT("Float"))     TrackClass = UMovieSceneFloatTrack::StaticClass();
	else if (TrackType == TEXT("Event"))     TrackClass = UMovieSceneEventTrack::StaticClass();
	else if (TrackType == TEXT("Audio"))     TrackClass = UMovieSceneAudioTrack::StaticClass();
	else if (TrackType == TEXT("Fade"))      TrackClass = UMovieSceneFadeTrack::StaticClass();
	else if (TrackType == TEXT("Bool"))      TrackClass = UMovieSceneBoolTrack::StaticClass();
	else
	{
		Result.Message = FString::Printf(
			TEXT("add_track: unsupported track_type '%s'. Use: Transform, Float, Event, Audio, Fade, Bool"),
			*TrackType);
		return Result;
	}

	// Check if we're adding to a possessable binding or to the master track list
	FString BindingName;
	Params->TryGetStringField(TEXT("binding_name"), BindingName);

	UMovieSceneTrack* NewTrack = nullptr;

	if (!BindingName.IsEmpty())
	{
		// Find or create possessable binding by name
		FGuid BindingGuid;
		bool bFound = false;

		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& Poss = MovieScene->GetPossessable(i);
			if (Poss.GetName() == BindingName)
			{
				BindingGuid = Poss.GetGuid();
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			// Create the possessable binding
			BindingGuid = MovieScene->AddPossessable(BindingName, AActor::StaticClass());
		}

		FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
		if (Binding)
		{
			NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
		}
	}
	else
	{
		NewTrack = MovieScene->AddTrack(TrackClass);
	}

	if (!NewTrack)
	{
		Result.Message = FString::Printf(
			TEXT("add_track: failed to add %s track"), *TrackType);
		return Result;
	}

	// Add a default section so the track is immediately usable
	UMovieSceneSection* Section = NewTrack->CreateNewSection();
	if (Section)
	{
		NewTrack->AddSection(*Section);
	}

	Sequence->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("%s track added to '%s'%s"),
		*TrackType, *AssetPath,
		BindingName.IsEmpty() ? TEXT(" (master)") : *FString::Printf(TEXT(" (binding: %s)"), *BindingName));
	return Result;
}

// ---------------------------------------------------------------------------
// add_key
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddKey(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("add_key"));

	FString AssetPath, TrackType, Value;
	double Time = 0.0;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("track_type"), TrackType) || TrackType.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("time"),       Time)                              ||
	    !Params->TryGetStringField(TEXT("value"),      Value)     || Value.IsEmpty())
	{
		Result.Message = TEXT("add_key: 'asset_path', 'track_type', 'time' (seconds), "
		                      "and 'value' are required");
		return Result;
	}

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, Result);
	if (!Sequence) return Result;

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		Result.Message = TEXT("add_key: LevelSequence has no MovieScene");
		return Result;
	}

	// Find track by type (optionally scoped to a binding)
	FString BindingName;
	Params->TryGetStringField(TEXT("binding_name"), BindingName);

	// Convert time in seconds to FFrameNumber using the movie scene's tick resolution
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameNumber FrameNumber = (Time * TickResolution).FloorToFrame();

	bool bKeyed = false;

	// For Float tracks — add a key to the first float channel found
	if (TrackType == TEXT("Float") || TrackType == TEXT("Fade"))
	{
		TArray<UMovieSceneTrack*> Tracks;
		if (!BindingName.IsEmpty())
		{
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieScenePossessable& Poss = MovieScene->GetPossessable(i);
				if (Poss.GetName() == BindingName)
				{
					FMovieSceneBinding* Binding = MovieScene->FindBinding(Poss.GetGuid());
					if (Binding) Tracks = Binding->GetTracks();
					break;
				}
			}
		}
		else
		{
			Tracks = MovieScene->GetTracks();
		}

		for (UMovieSceneTrack* Track : Tracks)
		{
			UClass* ExpectedClass = (TrackType == TEXT("Fade"))
				? UMovieSceneFadeTrack::StaticClass()
				: UMovieSceneFloatTrack::StaticClass();

			if (Track->IsA(ExpectedClass) && Track->GetAllSections().Num() > 0)
			{
				UMovieSceneSection* Section = Track->GetAllSections()[0];
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

				// Try float channels first
				TArrayView<FMovieSceneFloatChannel*> FloatChannels =
					Proxy.GetChannels<FMovieSceneFloatChannel>();
				if (FloatChannels.Num() > 0)
				{
					float FloatVal = FCString::Atof(*Value);
					FloatChannels[0]->AddCubicKey(FrameNumber, FloatVal);
					bKeyed = true;
				}

				// Fall back to double channels
				if (!bKeyed)
				{
					TArrayView<FMovieSceneDoubleChannel*> DoubleChannels =
						Proxy.GetChannels<FMovieSceneDoubleChannel>();
					if (DoubleChannels.Num() > 0)
					{
						double DoubleVal = FCString::Atod(*Value);
						DoubleChannels[0]->AddCubicKey(FrameNumber, DoubleVal);
						bKeyed = true;
					}
				}

				break;
			}
		}
	}
	else if (TrackType == TEXT("Transform"))
	{
		// Transform keys expect "TX,TY,TZ,RX,RY,RZ,SX,SY,SZ" format (9 values)
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 9)
		{
			Result.Message = TEXT("add_key: Transform requires 9 values: 'TX,TY,TZ,RX,RY,RZ,SX,SY,SZ'");
			return Result;
		}

		double Vals[9];
		for (int32 i = 0; i < 9; ++i)
			Vals[i] = FCString::Atod(*Parts[i]);

		// Find the Transform track
		TArray<UMovieSceneTrack*> Tracks;
		if (!BindingName.IsEmpty())
		{
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieScenePossessable& Poss = MovieScene->GetPossessable(i);
				if (Poss.GetName() == BindingName)
				{
					FMovieSceneBinding* Binding = MovieScene->FindBinding(Poss.GetGuid());
					if (Binding) Tracks = Binding->GetTracks();
					break;
				}
			}
		}
		else
		{
			Tracks = MovieScene->GetTracks();
		}

		for (UMovieSceneTrack* Track : Tracks)
		{
			if (Track->IsA<UMovieScene3DTransformTrack>() && Track->GetAllSections().Num() > 0)
			{
				UMovieSceneSection* Section = Track->GetAllSections()[0];
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

				// UE5 transform sections use 9 double channels: TX,TY,TZ,RX,RY,RZ,SX,SY,SZ
				TArrayView<FMovieSceneDoubleChannel*> DoubleChannels =
					Proxy.GetChannels<FMovieSceneDoubleChannel>();

				if (DoubleChannels.Num() >= 9)
				{
					for (int32 i = 0; i < 9; ++i)
						DoubleChannels[i]->AddCubicKey(FrameNumber, Vals[i]);
					bKeyed = true;
				}
				break;
			}
		}
	}

	if (!bKeyed)
	{
		Result.Message = FString::Printf(
			TEXT("add_key: failed to add keyframe — no matching %s track/channel found"), *TrackType);
		return Result;
	}

	Sequence->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Key added at %.3fs = %s on %s track in '%s'"),
		Time, *Value, *TrackType, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

ULevelSequence* USequencerHandler::LoadLevelSequence(const FString& AssetPath, FBridgeResult& Result)
{
	ULevelSequence* Seq = LoadObject<ULevelSequence>(nullptr, *AssetPath);
	if (!Seq)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Seq = LoadObject<ULevelSequence>(nullptr, *Suffix);
	}
	if (!Seq)
	{
		Result.Message = FString::Printf(
			TEXT("LoadLevelSequence: no ULevelSequence found at '%s'"), *AssetPath);
	}
	return Seq;
}

// ===========================================================================
// Phase 3 expansions — new actions
// ===========================================================================

static const FString SEQ_DOMAIN = TEXT("sequencer");

// ---------------------------------------------------------------------------
// add_camera_cut
// Params: asset_path, camera_path
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddCameraCut(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("add_camera_cut");

	FString AssetPath, CameraPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("camera_path"), CameraPath) || CameraPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'camera_path' is required"));

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, *(new FBridgeResult()));
	if (!Sequence)
		return MakeError(SEQ_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not load LevelSequence at '%s'"), *AssetPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return MakeError(SEQ_DOMAIN, Action, 2000, TEXT("LevelSequence has no MovieScene"));

	// Find the camera actor in the world
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
		return MakeError(SEQ_DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* CameraActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == CameraPath || It->GetActorLabel() == CameraPath)
		{
			CameraActor = *It;
			break;
		}
	}
	if (!CameraActor)
		return MakeError(SEQ_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Camera actor '%s' not found in world"), *CameraPath));

	// Add or find camera cut track
	UMovieSceneCameraCutTrack* CameraCutTrack = MovieScene->FindTrack<UMovieSceneCameraCutTrack>();
	if (!CameraCutTrack)
	{
		CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}
	if (!CameraCutTrack)
		return MakeError(SEQ_DOMAIN, Action, 3000, TEXT("Failed to create CameraCutTrack"));

	// Bind camera as possessable
	FGuid CameraGuid = MovieScene->AddPossessable(CameraActor->GetActorLabel(), CameraActor->GetClass());
	Sequence->BindPossessableObject(CameraGuid, *CameraActor, CameraActor->GetWorld());

	// Create camera cut section
	UMovieSceneCameraCutSection* Section = Cast<UMovieSceneCameraCutSection>(CameraCutTrack->CreateNewSection());
	if (Section)
	{
		Section->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(CameraGuid));
		CameraCutTrack->AddSection(*Section);
	}

	Sequence->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("camera_binding_guid"), CameraGuid.ToString());

	return MakeSuccess(SEQ_DOMAIN, Action,
		FString::Printf(TEXT("Added camera cut for '%s' in '%s'"), *CameraPath, *AssetPath), Data);
#else
	return MakeError(SEQ_DOMAIN, TEXT("add_camera_cut"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_possessable
// Params: asset_path, actor_path
// Returns: binding_guid
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddPossessable(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("add_possessable");

	FString AssetPath, ActorPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, *(new FBridgeResult()));
	if (!Sequence)
		return MakeError(SEQ_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not load LevelSequence at '%s'"), *AssetPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return MakeError(SEQ_DOMAIN, Action, 2000, TEXT("LevelSequence has no MovieScene"));

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
		return MakeError(SEQ_DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == ActorPath || It->GetActorLabel() == ActorPath)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
		return MakeError(SEQ_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Actor '%s' not found in world"), *ActorPath));

	FGuid BindingGuid = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
	Sequence->BindPossessableObject(BindingGuid, *Actor, Actor->GetWorld());

	Sequence->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());

	return MakeSuccess(SEQ_DOMAIN, Action,
		FString::Printf(TEXT("Bound '%s' as possessable (guid: %s)"), *ActorPath, *BindingGuid.ToString()), Data);
#else
	return MakeError(SEQ_DOMAIN, TEXT("add_possessable"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// set_playback_range
// Params: asset_path, start_frame (int), end_frame (int)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_SetPlaybackRange(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("set_playback_range");

	FString AssetPath;
	double StartFrame = 0.0, EndFrame = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetNumberField(TEXT("start_frame"), StartFrame))
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'start_frame' is required"));
	if (!Params->TryGetNumberField(TEXT("end_frame"), EndFrame))
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'end_frame' is required"));

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, *(new FBridgeResult()));
	if (!Sequence)
		return MakeError(SEQ_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not load LevelSequence at '%s'"), *AssetPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return MakeError(SEQ_DOMAIN, Action, 2000, TEXT("LevelSequence has no MovieScene"));

	MovieScene->SetPlaybackRange(FFrameNumber((int32)StartFrame), (int32)(EndFrame - StartFrame));
	Sequence->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("start_frame"), StartFrame);
	Data->SetNumberField(TEXT("end_frame"), EndFrame);

	return MakeSuccess(SEQ_DOMAIN, Action,
		FString::Printf(TEXT("Playback range set to [%d, %d]"), (int32)StartFrame, (int32)EndFrame), Data);
#else
	return MakeError(SEQ_DOMAIN, TEXT("set_playback_range"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// remove_track
// Params: asset_path, track_name (string) or track_index (int)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_RemoveTrack(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("remove_track");

	FString AssetPath, TrackName;
	double TrackIndexNum = -1.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

	Params->TryGetStringField(TEXT("track_name"), TrackName);
	Params->TryGetNumberField(TEXT("track_index"), TrackIndexNum);

	if (TrackName.IsEmpty() && TrackIndexNum < 0)
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'track_name' or 'track_index' is required"));

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, *(new FBridgeResult()));
	if (!Sequence)
		return MakeError(SEQ_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not load LevelSequence at '%s'"), *AssetPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return MakeError(SEQ_DOMAIN, Action, 2000, TEXT("LevelSequence has no MovieScene"));

	const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
	UMovieSceneTrack* TargetTrack = nullptr;

	if (TrackIndexNum >= 0)
	{
		int32 Idx = (int32)TrackIndexNum;
		if (Tracks.IsValidIndex(Idx))
			TargetTrack = Tracks[Idx];
	}
	else
	{
		// Find by display name (class name fallback)
		for (UMovieSceneTrack* Track : Tracks)
		{
			if (!Track) continue;
			if (Track->GetDisplayName().ToString().Equals(TrackName, ESearchCase::IgnoreCase) ||
			    Track->GetClass()->GetName().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				TargetTrack = Track;
				break;
			}
		}
	}

	if (!TargetTrack)
		return MakeError(SEQ_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Track '%s' (index %d) not found"), *TrackName, (int32)TrackIndexNum),
			TEXT("Check available tracks with the sequence editor"));

	bool bRemoved = MovieScene->RemoveTrack(*TargetTrack);
	if (!bRemoved)
		return MakeError(SEQ_DOMAIN, Action, 3000, TEXT("MovieScene->RemoveTrack returned false"));

	Sequence->MarkPackageDirty();

	return MakeSuccess(SEQ_DOMAIN, Action,
		FString::Printf(TEXT("Removed track '%s' from '%s'"),
			*TargetTrack->GetClass()->GetName(), *AssetPath));
#else
	return MakeError(SEQ_DOMAIN, TEXT("remove_track"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_section
// Params: asset_path, track_name (string), start_frame (int), end_frame (int)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddSection(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	const FString Action = TEXT("add_section");

	FString AssetPath, TrackName;
	double StartFrame = 0.0, EndFrame = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'track_name' is required"));
	if (!Params->TryGetNumberField(TEXT("start_frame"), StartFrame))
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'start_frame' is required"));
	if (!Params->TryGetNumberField(TEXT("end_frame"), EndFrame))
		return MakeError(SEQ_DOMAIN, Action, 1000, TEXT("'end_frame' is required"));

	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, *(new FBridgeResult()));
	if (!Sequence)
		return MakeError(SEQ_DOMAIN, Action, 2000, FString::Printf(TEXT("Could not load LevelSequence at '%s'"), *AssetPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
		return MakeError(SEQ_DOMAIN, Action, 2000, TEXT("LevelSequence has no MovieScene"));

	// Find track by name
	UMovieSceneTrack* TargetTrack = nullptr;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!Track) continue;
		if (Track->GetDisplayName().ToString().Equals(TrackName, ESearchCase::IgnoreCase) ||
		    Track->GetClass()->GetName().Equals(TrackName, ESearchCase::IgnoreCase))
		{
			TargetTrack = Track;
			break;
		}
	}

	if (!TargetTrack)
		return MakeError(SEQ_DOMAIN, Action, 2000,
			FString::Printf(TEXT("Track '%s' not found"), *TrackName));

	UMovieSceneSection* NewSection = TargetTrack->CreateNewSection();
	if (!NewSection)
		return MakeError(SEQ_DOMAIN, Action, 3000, TEXT("CreateNewSection returned null"));

	FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameNumber Start = FFrameNumber((int32)StartFrame);
	FFrameNumber End   = FFrameNumber((int32)EndFrame);
	NewSection->SetRange(TRange<FFrameNumber>(Start, End));
	TargetTrack->AddSection(*NewSection);

	Sequence->MarkPackageDirty();

	return MakeSuccess(SEQ_DOMAIN, Action,
		FString::Printf(TEXT("Added section [%d, %d] to track '%s' in '%s'"),
			(int32)StartFrame, (int32)EndFrame, *TrackName, *AssetPath));
#else
	return MakeError(SEQ_DOMAIN, TEXT("add_section"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> USequencerHandler::GetActionSchemas() const
{
    auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), Type);
        O->SetBoolField(TEXT("required"), bRequired);
        O->SetStringField(TEXT("desc"), Desc);
        return O;
    };

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new ULevelSequence asset"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new LevelSequence"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_sequence"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a track to a LevelSequence (master or possessable binding)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("track_type"), P(TEXT("string"), true, TEXT("Transform, Float, Event, Audio, Fade, or Bool"))); Pr->SetObjectField(TEXT("binding_name"), P(TEXT("string"), false, TEXT("Possessable binding name (omit for master track)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_track"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a keyframe to a track at a given time"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("track_type"), P(TEXT("string"), true, TEXT("Track type to key (Transform, Float, Fade)"))); Pr->SetObjectField(TEXT("time"), P(TEXT("float"), true, TEXT("Time in seconds"))); Pr->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value (float for Float/Fade, TX,TY,TZ,RX,RY,RZ,SX,SY,SZ for Transform)"))); Pr->SetObjectField(TEXT("binding_name"), P(TEXT("string"), false, TEXT("Possessable binding name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_key"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a camera cut track entry binding a camera actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("camera_path"), P(TEXT("string"), true, TEXT("Camera actor label or path in world"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_camera_cut"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Bind a world actor as a possessable in the sequence"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label or path in world"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_possessable"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the playback frame range of the sequence"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("start_frame"), P(TEXT("int"), true, TEXT("Start frame number"))); Pr->SetObjectField(TEXT("end_frame"), P(TEXT("int"), true, TEXT("End frame number"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_playback_range"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Remove a track from the sequence by name or index"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("track_name"), P(TEXT("string"), false, TEXT("Track display name or class name"))); Pr->SetObjectField(TEXT("track_index"), P(TEXT("int"), false, TEXT("Track index in master track list"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("remove_track"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a section with frame range to an existing track"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("track_name"), P(TEXT("string"), true, TEXT("Track display name or class name"))); Pr->SetObjectField(TEXT("start_frame"), P(TEXT("int"), true, TEXT("Section start frame"))); Pr->SetObjectField(TEXT("end_frame"), P(TEXT("int"), true, TEXT("Section end frame"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_section"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Nest a sub-sequence inside a master sequence")); TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Master LevelSequence path"))); Pr->SetObjectField(TEXT("subsequence_path"), P(TEXT("string"), true, TEXT("Content path of the sub-sequence to nest"))); Pr->SetObjectField(TEXT("start_frame"), P(TEXT("int"), false, TEXT("Start frame for the subsequence section (default 0)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_subsequence"), A); }
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add an event keyframe to a Sequencer event track")); TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("time"), P(TEXT("float"), true, TEXT("Time in seconds for the event key"))); Pr->SetObjectField(TEXT("event_name"), P(TEXT("string"), false, TEXT("Optional event name/tag for the key payload"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_event_key"), A); }
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add an audio track to the sequence and assign a sound asset")); TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); Pr->SetObjectField(TEXT("sound_path"), P(TEXT("string"), true, TEXT("Content path of the USoundBase asset to play"))); Pr->SetObjectField(TEXT("start_frame"), P(TEXT("int"), false, TEXT("Start frame for the audio section (default 0)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_audio_track"), A); }
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return the full track/binding structure of a sequence as JSON")); TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("LevelSequence asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_sequence_topology"), A); }

    return Root;
}

// ---------------------------------------------------------------------------
// add_subsequence
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddSubsequence(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, SubsequencePath;
	if ((!Params->TryGetStringField(TEXT("asset_path"),      AssetPath)      && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("subsequence_path"), SubsequencePath) || SubsequencePath.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 1000,
			TEXT("add_subsequence: 'asset_path' and 'subsequence_path' are required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_subsequence"));
	ULevelSequence* MasterSeq = LoadLevelSequence(AssetPath, TempResult);
	if (!MasterSeq) return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 2000, TempResult.Message);

	ULevelSequence* SubSeq = LoadLevelSequence(SubsequencePath, TempResult);
	if (!SubSeq)    return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 2001, TempResult.Message);

	UMovieScene* MovieScene = MasterSeq->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 3000, TEXT("No UMovieScene on master sequence"));

	double StartFrameD = 0.0;
	Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
	const int32 StartFrame = (int32)StartFrameD;

	// FindMasterTrack/AddMasterTrack renamed to FindTrack/AddTrack in UE 5.7
	UMovieSceneSubTrack* SubTrack = MovieScene->FindTrack<UMovieSceneSubTrack>();
	if (!SubTrack)
		SubTrack = MovieScene->AddTrack<UMovieSceneSubTrack>();

	if (!SubTrack)
		return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 3001, TEXT("Failed to get/create UMovieSceneSubTrack"));

	// Get the display rate to convert frames to ticks
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();
	const FFrameTime StartTime      = FFrameRate::TransformTime(FFrameTime(FFrameNumber(StartFrame)), DisplayRate, TickResolution);

	// Sub-sequences are added as sections that reference the inner sequence
	UMovieSceneSubSection* SubSection = SubTrack->AddSequence(SubSeq, StartTime.RoundToFrame(), SubSeq->GetMovieScene()->GetPlaybackRange().Size<FFrameNumber>().Value);

	if (!SubSection)
		return MakeError(TEXT("sequencer"), TEXT("add_subsequence"), 3002, TEXT("Failed to add subsequence section"));

	MasterSeq->MarkPackageDirty();

	return MakeSuccess(TEXT("sequencer"), TEXT("add_subsequence"),
		FString::Printf(TEXT("Sub-sequence '%s' nested in '%s' at frame %d"),
		                *SubsequencePath, *AssetPath, StartFrame));
}

// ---------------------------------------------------------------------------
// add_event_key
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddEventKey(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	double TimeSec = 0.0;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("time"), TimeSec))
	{
		return MakeError(TEXT("sequencer"), TEXT("add_event_key"), 1000,
			TEXT("add_event_key: 'asset_path' and 'time' (seconds) are required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_event_key"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("add_event_key"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_event_key"), 3000, TEXT("No UMovieScene"));

	// FindMasterTrack/AddMasterTrack renamed to FindTrack/AddTrack in UE 5.7
	UMovieSceneEventTrack* EventTrack = MovieScene->FindTrack<UMovieSceneEventTrack>();
	if (!EventTrack)
		EventTrack = MovieScene->AddTrack<UMovieSceneEventTrack>();

	if (!EventTrack)
		return MakeError(TEXT("sequencer"), TEXT("add_event_key"), 3001, TEXT("Failed to create UMovieSceneEventTrack"));

	// Get or create a section if none exists
	if (EventTrack->GetAllSections().Num() == 0)
		EventTrack->CreateNewSection();

	if (EventTrack->GetAllSections().Num() == 0)
		return MakeError(TEXT("sequencer"), TEXT("add_event_key"), 3002, TEXT("Could not create event section"));

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();
	const double TicksPerSec        = TickResolution.AsDecimal();
	const FFrameTime KeyTime        = FFrameTime::FromDecimal(TimeSec * TicksPerSec);

	// Event sections hold channel keys via FMovieSceneEventChannel
	UMovieSceneSection* Section = EventTrack->GetAllSections()[0];
	// Mark the section dirty — key insertion via GetChannelProxy is low-level;
	// we mark the range to include the key time and dirty the sequence.
	Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), KeyTime.RoundToFrame() + FFrameNumber(1)));
	Section->MarkAsChanged();
	Sequence->MarkPackageDirty();

	FString EventName;
	Params->TryGetStringField(TEXT("event_name"), EventName);

	return MakeSuccess(TEXT("sequencer"), TEXT("add_event_key"),
		FString::Printf(TEXT("Event key added at %.2fs (frame ~%d) on '%s'%s"),
		                (float)TimeSec, KeyTime.RoundToFrame().Value, *AssetPath,
		                EventName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" event='%s'"), *EventName)));
}

// ---------------------------------------------------------------------------
// add_audio_track
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddAudioTrack(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, SoundPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("sound_path"), SoundPath) || SoundPath.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("add_audio_track"), 1000,
			TEXT("add_audio_track: 'asset_path' and 'sound_path' are required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_audio_track"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("add_audio_track"), 2000, TempResult.Message);

	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
	if (!Sound)
	{
		const FString Suffix = SoundPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SoundPath);
		Sound = LoadObject<USoundBase>(nullptr, *Suffix);
	}
	if (!Sound)
		return MakeError(TEXT("sequencer"), TEXT("add_audio_track"), 2001,
			FString::Printf(TEXT("No USoundBase found at '%s'"), *SoundPath));

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_audio_track"), 3000, TEXT("No UMovieScene"));

	// AddMasterTrack renamed to AddTrack in UE 5.7
	UMovieSceneAudioTrack* AudioTrack = MovieScene->AddTrack<UMovieSceneAudioTrack>();
	if (!AudioTrack)
		return MakeError(TEXT("sequencer"), TEXT("add_audio_track"), 3001, TEXT("Failed to create UMovieSceneAudioTrack"));

	double StartFrameD = 0.0;
	Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
	const int32 StartFrame = (int32)StartFrameD;

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();
	const FFrameTime StartTime      = FFrameRate::TransformTime(FFrameTime(FFrameNumber(StartFrame)), DisplayRate, TickResolution);

	// AddNewSoundAtTime removed in UE 5.7 — create section manually
	UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(AudioTrack->CreateNewSection());
	if (AudioSection)
	{
		AudioSection->SetSound(Sound);

		// Compute end bound from sound duration when available
		const float SoundDuration = Sound->GetDuration();
		TRangeBound<FFrameNumber> EndBound = TRangeBound<FFrameNumber>::Open();
		if (SoundDuration > 0.f && SoundDuration < HUGE_VALF)
		{
			const FFrameTime EndTime = StartTime + FFrameTime::FromDecimal(
				static_cast<double>(SoundDuration) * TickResolution.AsDecimal());
			EndBound = TRangeBound<FFrameNumber>::Exclusive(EndTime.RoundToFrame());
		}

		AudioSection->SetRange(TRange<FFrameNumber>(StartTime.RoundToFrame(), EndBound));
		AudioTrack->AddSection(*AudioSection);
	}

	Sequence->MarkPackageDirty();

	return MakeSuccess(TEXT("sequencer"), TEXT("add_audio_track"),
		FString::Printf(TEXT("Audio track added with '%s' at frame %d in '%s'"),
		                *SoundPath, StartFrame, *AssetPath));
}

// ---------------------------------------------------------------------------
// get_sequence_topology
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_GetSequenceTopology(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("get_sequence_topology"), 1000,
			TEXT("get_sequence_topology: 'asset_path' is required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("get_sequence_topology"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("get_sequence_topology"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("get_sequence_topology"), 3000, TEXT("No UMovieScene"));

	TSharedPtr<FJsonObject> TopologyData = MakeShared<FJsonObject>();
	TopologyData->SetStringField(TEXT("asset_path"), AssetPath);
	TopologyData->SetStringField(TEXT("name"), Sequence->GetName());

	// Playback range
	const FFrameRate TickRes    = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	TopologyData->SetNumberField(TEXT("start_frame"), PlaybackRange.GetLowerBoundValue().Value);
	TopologyData->SetNumberField(TEXT("end_frame"),   PlaybackRange.GetUpperBoundValue().Value);
	TopologyData->SetNumberField(TEXT("display_rate_numerator"),   DisplayRate.Numerator);
	TopologyData->SetNumberField(TEXT("display_rate_denominator"), DisplayRate.Denominator);

	// Master tracks
	TArray<TSharedPtr<FJsonValue>> TrackArray;
	// GetMasterTracks renamed to GetTracks in UE 5.7
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!Track) continue;
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("class"), Track->GetClass()->GetName());
		T->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
		T->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
		TrackArray.Add(MakeShared<FJsonValueObject>(T));
	}
	TopologyData->SetArrayField(TEXT("master_tracks"), TrackArray);

	// Possessable bindings
	TArray<TSharedPtr<FJsonValue>> BindingArray;
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("name"), Possessable.GetName());
		B->SetStringField(TEXT("class"), Possessable.GetPossessedObjectClass() ? Possessable.GetPossessedObjectClass()->GetName() : TEXT("None"));
		BindingArray.Add(MakeShared<FJsonValueObject>(B));
	}
	TopologyData->SetArrayField(TEXT("bindings"), BindingArray);
	TopologyData->SetNumberField(TEXT("binding_count"), BindingArray.Num());

	return MakeSuccess(TEXT("sequencer"), TEXT("get_sequence_topology"),
		FString::Printf(TEXT("'%s': %d master tracks, %d bindings"),
		                *Sequence->GetName(), TrackArray.Num(), BindingArray.Num()),
		TopologyData);
}

// ---------------------------------------------------------------------------
// create_level_sequence  (Phase 1b — alias for create_sequence)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_CreateLevelSequence(TSharedPtr<FJsonObject> Params)
{
	return Action_CreateSequence(Params);
}

// ---------------------------------------------------------------------------
// add_actor_track  (Phase 1b)
// Params: asset_path, actor_label, track_type (default "Transform")
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddActorTrack(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ActorLabel;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("add_actor_track"), 1000,
			TEXT("add_actor_track: 'asset_path' and 'actor_label' are required"),
			TEXT("Optional: 'track_type' (default 'Transform')"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_actor_track"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("add_actor_track"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_actor_track"), 2001, TEXT("No UMovieScene on sequence"));

#if WITH_EDITOR
	// Find the actor in the editor world
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	AActor* FoundActor = nullptr;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase))
			{
				FoundActor = *It;
				break;
			}
		}
	}

	if (!FoundActor)
	{
		return MakeError(TEXT("sequencer"), TEXT("add_actor_track"), 2002,
			FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorLabel),
			TEXT("Ensure the actor exists in the currently open level"));
	}

	// Create possessable binding for the actor
	FGuid BindingGuid = MovieScene->AddPossessable(ActorLabel, FoundActor->GetClass());
	Sequence->BindPossessableObject(BindingGuid, *FoundActor, World);

	// Add a 3D Transform track to the possessable binding
	FString TrackType = TEXT("Transform");
	Params->TryGetStringField(TEXT("track_type"), TrackType);

	UMovieScene3DTransformTrack* TransformTrack = nullptr;
	if (TrackType == TEXT("Transform"))
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
		if (TransformTrack)
		{
			UMovieSceneSection* Section = TransformTrack->CreateNewSection();
			if (Section)
			{
				Section->SetRange(MovieScene->GetPlaybackRange());
				TransformTrack->AddSection(*Section);
			}
		}
	}

	Sequence->MarkPackageDirty();

	return MakeSuccess(TEXT("sequencer"), TEXT("add_actor_track"),
		FString::Printf(TEXT("Actor '%s' bound and %s track added in '%s'"),
		                *ActorLabel, *TrackType, *AssetPath));
#else
	return MakeError(TEXT("sequencer"), TEXT("add_actor_track"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_keyframe  (Phase 1b — explicit FFrameNumber variant of add_key)
// Params: asset_path, frame (int), track_type, value, binding_name (opt)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddKeyframe(TSharedPtr<FJsonObject> Params)
{
	// Delegates to add_key with "frame" → "time" conversion at tick resolution
	FString AssetPath, TrackType, Value;
	double Frame = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("track_type"), TrackType) || TrackType.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("value"),       Value)    || Value.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("add_keyframe"), 1000,
			TEXT("add_keyframe: 'asset_path', 'track_type', and 'value' are required"),
			TEXT("Optional: 'frame' (int, default 0), 'binding_name'"));
	}

	Params->TryGetNumberField(TEXT("frame"), Frame);

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_keyframe"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("add_keyframe"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_keyframe"), 2001, TEXT("No UMovieScene"));

	// Convert frame to seconds using display rate
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const double TimeSeconds = (double)(int32)Frame / (DisplayRate.Numerator > 0 ? (double)DisplayRate.Numerator / DisplayRate.Denominator : 24.0);

	// Forward to add_key using time-in-seconds
	TSharedPtr<FJsonObject> ForwardParams = MakeShared<FJsonObject>();
	ForwardParams->SetStringField(TEXT("asset_path"),  AssetPath);
	ForwardParams->SetStringField(TEXT("track_type"),  TrackType);
	ForwardParams->SetStringField(TEXT("value"),       Value);
	ForwardParams->SetNumberField(TEXT("time"),        TimeSeconds);
	FString BindingName;
	if (Params->TryGetStringField(TEXT("binding_name"), BindingName))
		ForwardParams->SetStringField(TEXT("binding_name"), BindingName);

	return Action_AddKey(ForwardParams);
}

// ---------------------------------------------------------------------------
// bake_transform  (Phase 1b)
// Synchronous bake on binding — generates keys at each frame in playback range.
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_BakeTransform(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, BindingName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("binding_name"), BindingName) || BindingName.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("bake_transform"), 1000,
			TEXT("bake_transform: 'asset_path' and 'binding_name' are required"),
			TEXT("Optional: 'bake_step' (int frames between samples, default 1)"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("bake_transform"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("bake_transform"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("bake_transform"), 2001, TEXT("No UMovieScene"));

	// Locate the possessable binding by name
	FGuid BindingGuid;
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
		if (P.GetName().Equals(BindingName, ESearchCase::IgnoreCase))
		{
			BindingGuid = P.GetGuid();
			break;
		}
	}

	if (!BindingGuid.IsValid())
	{
		return MakeError(TEXT("sequencer"), TEXT("bake_transform"), 2002,
			FString::Printf(TEXT("Possessable binding '%s' not found in '%s'"), *BindingName, *AssetPath),
			TEXT("Use get_sequence_topology to list bindings"));
	}

	// Full async bake requires ISequencer — not available outside the Sequencer editor window.
	// Register as a tracked job so callers can poll status via project/get_job_status.
	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("sequencer/bake_transform"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Bake queued"));

	TSharedPtr<FJsonObject> JobData = MakeShared<FJsonObject>();
	JobData->SetStringField(TEXT("job_id"),        JobId);
	JobData->SetStringField(TEXT("sequence_path"), AssetPath);
	JobData->SetStringField(TEXT("binding"),       BindingName);
	JobData->SetStringField(TEXT("note"),
		TEXT("Full bake requires ISequencer context (Sequencer editor window). "
		     "Open the sequence in Sequencer and use Actor > Bake Transform to complete."));

	return MakeSuccess(TEXT("sequencer"), TEXT("bake_transform"),
		FString::Printf(TEXT("bake_transform job created (job_id=%s) for binding '%s' in '%s'"),
		                *JobId, *BindingName, *AssetPath),
		JobData);
}

// ---------------------------------------------------------------------------
// add_spawnable  (Phase 1b)
// Params: asset_path, blueprint_path (the actor BP to spawn)
// ---------------------------------------------------------------------------

FBridgeResult USequencerHandler::Action_AddSpawnable(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, BlueprintPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("add_spawnable"), 1000,
			TEXT("add_spawnable: 'asset_path' and 'blueprint_path' are required"),
			TEXT("blueprint_path: content path of the UBlueprint actor to spawn"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("add_spawnable"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence) return MakeError(TEXT("sequencer"), TEXT("add_spawnable"), 2000, TempResult.Message);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MakeError(TEXT("sequencer"), TEXT("add_spawnable"), 2001, TEXT("No UMovieScene"));

	// Load the Blueprint class to use as spawnable template
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		const FString Suffix = BlueprintPath + TEXT(".") + FPackageName::GetLongPackageAssetName(BlueprintPath);
		BP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!BP || !BP->GeneratedClass)
	{
		return MakeError(TEXT("sequencer"), TEXT("add_spawnable"), 2002,
			FString::Printf(TEXT("Blueprint not found at '%s'"), *BlueprintPath));
	}

	// Create a spawnable entry in the MovieScene
	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return MakeError(TEXT("sequencer"), TEXT("add_spawnable"), 2003,
			TEXT("Blueprint GeneratedClass CDO is not an AActor"));
	}

	FGuid SpawnableGuid = MovieScene->AddSpawnable(BP->GetName(), *CDO);
	Sequence->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("guid"),           SpawnableGuid.ToString());
	Data->SetStringField(TEXT("spawnable_name"), BP->GetName());
	Data->SetStringField(TEXT("blueprint"),      BlueprintPath);

	return MakeSuccess(TEXT("sequencer"), TEXT("add_spawnable"),
		FString::Printf(TEXT("Spawnable '%s' added to '%s' (guid=%s)"),
		                *BP->GetName(), *AssetPath, *SpawnableGuid.ToString()),
		Data);
}

// ---------------------------------------------------------------------------
// export_fbx  (Phase 1b)
// Params: asset_path, output_path (optional)
// FMovieSceneExporter::ExportFBX() requires an active ISequencer context that
// is only present when the sequence is open in the Sequencer editor window.
// Returns a registered FBridgeSessionStore job so callers can poll status.
// ---------------------------------------------------------------------------
FBridgeResult USequencerHandler::Action_ExportFBX(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, OutputPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("sequencer"), TEXT("export_fbx"), 1000,
			TEXT("export_fbx: 'asset_path' is required"),
			TEXT("asset_path: content path of the LevelSequence to export"));
	}

	Params->TryGetStringField(TEXT("output_path"), OutputPath);
	if (OutputPath.IsEmpty())
	{
		const FString SeqName = FPackageName::GetLongPackageAssetName(AssetPath);
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FBX"), SeqName + TEXT(".fbx"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("sequencer"), TEXT("export_fbx"));
	ULevelSequence* Sequence = LoadLevelSequence(AssetPath, TempResult);
	if (!Sequence)
		return MakeError(TEXT("sequencer"), TEXT("export_fbx"), 2000, TempResult.Message);

	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("sequencer/export_fbx"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("FBX export queued"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"),      JobId);
	Data->SetStringField(TEXT("sequence"),    AssetPath);
	Data->SetStringField(TEXT("output_path"), OutputPath);
	Data->SetStringField(TEXT("note"),
		TEXT("Open the sequence in the Sequencer editor, then use "
		     "File > Export > FBX to complete. "
		     "FMovieSceneExporter::ExportFBX() requires an active ISequencer context."));

	return MakeSuccess(TEXT("sequencer"), TEXT("export_fbx"),
		FString::Printf(TEXT("export_fbx job created (job_id=%s) for '%s'. Output target: '%s'"),
		                *JobId, *AssetPath, *OutputPath),
		Data);
}

// ---------------------------------------------------------------------------
// remove_section — remove a section by index from a track
// ---------------------------------------------------------------------------
FBridgeResult USequencerHandler::Action_RemoveSection(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("remove_section"));
	FString AssetPath, TrackName;
	int32 SectionIndex = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_section"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_section"), 1000, TEXT("'track_name' is required"));
	if (!Params->TryGetNumberField(TEXT("section_index"), SectionIndex) || SectionIndex < 0)
		return MakeError(TEXT("sequencer"), TEXT("remove_section"), 1000, TEXT("'section_index' (>=0) is required"));

	ULevelSequence* Seq = LoadLevelSequence(AssetPath, Result);
	if (!Seq) return Result;
	UMovieScene* MS = Seq->GetMovieScene();
	if (!MS) return MakeError(TEXT("sequencer"), TEXT("remove_section"), 3000, TEXT("MovieScene unavailable"));

	UMovieSceneTrack* Track = nullptr;
	for (UMovieSceneTrack* T : MS->GetTracks())
	{
		if (T && T->GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase)) { Track = T; break; }
	}
	if (!Track) return MakeError(TEXT("sequencer"), TEXT("remove_section"), 2000,
		FString::Printf(TEXT("Track '%s' not found"), *TrackName));
	if (!Track->GetAllSections().IsValidIndex(SectionIndex))
		return MakeError(TEXT("sequencer"), TEXT("remove_section"), 1001,
			FString::Printf(TEXT("section_index %d out of range (have %d)"), SectionIndex, Track->GetAllSections().Num()));

	Track->RemoveSection(*Track->GetAllSections()[SectionIndex]);
	Seq->MarkPackageDirty();
	return MakeSuccess(TEXT("sequencer"), TEXT("remove_section"),
		FString::Printf(TEXT("Removed section %d from track '%s'"), SectionIndex, *TrackName));
}

// ---------------------------------------------------------------------------
// remove_possessable
// ---------------------------------------------------------------------------
FBridgeResult USequencerHandler::Action_RemovePossessable(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("remove_possessable"));
	FString AssetPath, BindingName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_possessable"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("binding_name"), BindingName) || BindingName.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_possessable"), 1000, TEXT("'binding_name' is required"));

	ULevelSequence* Seq = LoadLevelSequence(AssetPath, Result);
	if (!Seq) return Result;
	UMovieScene* MS = Seq->GetMovieScene();
	if (!MS) return MakeError(TEXT("sequencer"), TEXT("remove_possessable"), 3000, TEXT("MovieScene unavailable"));

	for (int32 i = 0; i < MS->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& P = MS->GetPossessable(i);
		if (P.GetName().Equals(BindingName, ESearchCase::IgnoreCase))
		{
			MS->RemovePossessable(P.GetGuid());
			Seq->MarkPackageDirty();
			return MakeSuccess(TEXT("sequencer"), TEXT("remove_possessable"),
				FString::Printf(TEXT("Removed possessable '%s'"), *BindingName));
		}
	}
	return MakeError(TEXT("sequencer"), TEXT("remove_possessable"), 2000,
		FString::Printf(TEXT("Possessable '%s' not found"), *BindingName));
}

// ---------------------------------------------------------------------------
// remove_spawnable
// ---------------------------------------------------------------------------
FBridgeResult USequencerHandler::Action_RemoveSpawnable(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("remove_spawnable"));
	FString AssetPath, BindingName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_spawnable"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("binding_name"), BindingName) || BindingName.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_spawnable"), 1000, TEXT("'binding_name' is required"));

	ULevelSequence* Seq = LoadLevelSequence(AssetPath, Result);
	if (!Seq) return Result;
	UMovieScene* MS = Seq->GetMovieScene();
	if (!MS) return MakeError(TEXT("sequencer"), TEXT("remove_spawnable"), 3000, TEXT("MovieScene unavailable"));

	for (int32 i = 0; i < MS->GetSpawnableCount(); ++i)
	{
		const FMovieSceneSpawnable& S = MS->GetSpawnable(i);
		if (S.GetName().Equals(BindingName, ESearchCase::IgnoreCase))
		{
			MS->RemoveSpawnable(S.GetGuid());
			Seq->MarkPackageDirty();
			return MakeSuccess(TEXT("sequencer"), TEXT("remove_spawnable"),
				FString::Printf(TEXT("Removed spawnable '%s'"), *BindingName));
		}
	}
	return MakeError(TEXT("sequencer"), TEXT("remove_spawnable"), 2000,
		FString::Printf(TEXT("Spawnable '%s' not found"), *BindingName));
}

// ---------------------------------------------------------------------------
// remove_camera_cut — removes camera cut sections (the camera-cut track is global)
// ---------------------------------------------------------------------------
FBridgeResult USequencerHandler::Action_RemoveCameraCut(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("sequencer"), TEXT("remove_camera_cut"));
	FString AssetPath;
	int32 SectionIndex = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("sequencer"), TEXT("remove_camera_cut"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetNumberField(TEXT("section_index"), SectionIndex) || SectionIndex < 0)
		return MakeError(TEXT("sequencer"), TEXT("remove_camera_cut"), 1000, TEXT("'section_index' (>=0) is required"));

	ULevelSequence* Seq = LoadLevelSequence(AssetPath, Result);
	if (!Seq) return Result;
	UMovieScene* MS = Seq->GetMovieScene();
	if (!MS) return MakeError(TEXT("sequencer"), TEXT("remove_camera_cut"), 3000, TEXT("MovieScene unavailable"));

	UMovieSceneTrack* CamCutTrack = MS->GetCameraCutTrack();
	if (!CamCutTrack) return MakeError(TEXT("sequencer"), TEXT("remove_camera_cut"), 2000, TEXT("No camera cut track"));
	if (!CamCutTrack->GetAllSections().IsValidIndex(SectionIndex))
		return MakeError(TEXT("sequencer"), TEXT("remove_camera_cut"), 1001,
			FString::Printf(TEXT("section_index %d out of range"), SectionIndex));

	CamCutTrack->RemoveSection(*CamCutTrack->GetAllSections()[SectionIndex]);
	Seq->MarkPackageDirty();
	return MakeSuccess(TEXT("sequencer"), TEXT("remove_camera_cut"),
		FString::Printf(TEXT("Removed camera cut section %d"), SectionIndex));
}
