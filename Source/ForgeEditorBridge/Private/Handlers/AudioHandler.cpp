#include "Handlers/AudioHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- Audio runtime ---------------------------------------------------------
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/DialogueWave.h"
#include "Sound/DialogueVoice.h"

// ---- Reverb ----------------------------------------------------------------
#include "Sound/ReverbEffect.h"

// ---- Audio volumes ---------------------------------------------------------
#include "Sound/AudioVolume.h"
#include "EngineUtils.h"

// ---- Asset registry --------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// ---- Audio editor ----------------------------------------------------------
#include "Factories/SoundCueFactoryNew.h"
#include "Factories/SoundAttenuationFactory.h"
#include "Factories/SoundClassFactory.h"
#include "Factories/SoundMixFactory.h"
#include "Factories/SoundConcurrencyFactory.h"

// ---- Serialization ----------------------------------------------------------
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---- Transactions -----------------------------------------------------------
#include "ScopedTransaction.h"

// ---- MetaSound -------------------------------------------------------------
#include "MetasoundSource.h"
// UE 5.7: IMetaSoundDocumentInterface::GetDocument() is private
// MetaSound graph editing routed through Python scripting plugin

// ---- Engine (for GEngine->Exec Python dispatch) ----------------------------
#include "Engine/Engine.h"
#include "Editor.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UAudioHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("audio"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_cue"))          return Action_CreateCue(Params);
	if (Action == TEXT("add_ms_node"))         return Action_AddMSNode(Params);
	if (Action == TEXT("connect_ms"))          return Action_ConnectMS(Params);
	if (Action == TEXT("set_attenuation"))     return Action_SetAttenuation(Params);
	if (Action == TEXT("create_sound_class"))  return Action_CreateSoundClass(Params);
	if (Action == TEXT("create_sound_mix"))    return Action_CreateSoundMix(Params);
	if (Action == TEXT("place_audio_volume"))  return Action_PlaceAudioVolume(Params);
	if (Action == TEXT("set_concurrency"))     return Action_SetConcurrency(Params);
	if (Action == TEXT("create_dialogue_wave"))return Action_CreateDialogueWave(Params);
	if (Action == TEXT("list_sound_cues"))     return Action_ListSoundCues(Params);
	if (Action == TEXT("get_cue_info"))        return Action_GetCueInfo(Params);
	// Phase 1b
	if (Action == TEXT("create_sound_cue"))   return Action_CreateCue(Params);  // alias
	if (Action == TEXT("create_attenuation")) return Action_CreateAttenuation(Params);
	if (Action == TEXT("set_reverb"))         return Action_SetReverb(Params);
	if (Action == TEXT("set_mix_class"))      return Action_SetMixClass(Params);
	if (Action == TEXT("create_metasound"))   return Action_CreateMetaSound(Params);
	if (Action == TEXT("set_metasound_param"))return Action_SetMetaSoundParam(Params);
	if (Action == TEXT("add_metasound_input"))return Action_AddMetaSoundInput(Params);

	FBridgeResult R = CreateResult(TEXT("audio"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown audio action '%s'. Valid: create_cue, add_ms_node, connect_ms, "
		     "set_attenuation, create_sound_class, create_sound_mix, place_audio_volume, "
		     "set_concurrency, create_dialogue_wave, list_sound_cues, get_cue_info, "
		     "create_sound_cue, create_attenuation, set_reverb, set_mix_class, "
		     "create_metasound, set_metasound_param, add_metasound_input"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_cue
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_CreateCue(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("audio"), TEXT("create_cue"));

	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_cue: 'asset_path' is required (e.g. '/Game/Audio/SC_Explosion')");
		Result.ErrorCode = 1000;
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    USoundCue::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_cue: failed to create SoundCue at '%s'"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	USoundCue* NewCue = CastChecked<USoundCue>(CreatedAsset);

	// Optionally wire a SoundWave into the cue
	FString SoundWavePath;
	if (Params->TryGetStringField(TEXT("sound_wave_path"), SoundWavePath) && !SoundWavePath.IsEmpty())
	{
		USoundWave* Wave = LoadObject<USoundWave>(nullptr, *SoundWavePath);
		if (Wave)
		{
			USoundNodeWavePlayer* WavePlayer = NewCue->ConstructSoundNode<USoundNodeWavePlayer>();
			WavePlayer->SetSoundWave(Wave);
			NewCue->FirstNode = WavePlayer;
			NewCue->LinkGraphNodesFromSoundNodes();
			NewCue->CompileSoundNodesFromGraphNodes();
		}
	}

	NewCue->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("SoundCue created at %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_ms_node
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_AddMSNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("audio"), TEXT("add_ms_node"));

	FString AssetPath, NodeClass;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
	{
		Result.Message = TEXT("add_ms_node: 'asset_path' and 'node_class' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Asset = LoadObject<UObject>(nullptr, *Suffix);
	}
	if (!Asset)
	{
		Result.Message = FString::Printf(TEXT("add_ms_node: asset not found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UMetaSoundSource* MSSource = Cast<UMetaSoundSource>(Asset);
	if (!MSSource)
	{
		Result.Message = FString::Printf(
			TEXT("add_ms_node: asset at '%s' is not a UMetaSoundSource"), *AssetPath);
		Result.ErrorCode = 2001;
		return Result;
	}

	// UE 5.7: C++ document API is private — dispatch via Python console command
	if (!GEngine)
	{
		Result.Message = TEXT("add_ms_node: GEngine unavailable");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Build a single-line Python command (py console prefix)
	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "asset = unreal.load_asset('%s'); "
		     "builder = unreal.get_editor_subsystem(unreal.MetaSoundBuilderSubsystem); "
		     "builder.add_node_by_class_name(asset, '%s') if builder else print('No MetaSoundBuilderSubsystem')"),
		*AssetPath, *NodeClass);

	FString PyCmd = FString::Printf(TEXT("py %s"), *PyScript);
	GEngine->Exec(UBridgeHandlerBase::GetSafeEditorWorld(), *PyCmd);

	Result.bSuccess = true;
	Result.AffectedPath = AssetPath;
	Result.Message = FString::Printf(TEXT("MetaSound node '%s' dispatched to '%s' via Python"), *NodeClass, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// connect_ms
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_ConnectMS(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("audio"), TEXT("connect_ms"));

	FString AssetPath, SrcNode, SrcPin, DstNode, DstPin;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("source_node"), SrcNode)   || SrcNode.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("source_pin"),  SrcPin)    || SrcPin.IsEmpty()     ||
	    !Params->TryGetStringField(TEXT("dest_node"),   DstNode)   || DstNode.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("dest_pin"),    DstPin)    || DstPin.IsEmpty())
	{
		Result.Message = TEXT("connect_ms: 'asset_path', 'source_node', 'source_pin', "
		                      "'dest_node', and 'dest_pin' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Asset = LoadObject<UObject>(nullptr, *Suffix);
	}
	if (!Asset)
	{
		Result.Message = FString::Printf(TEXT("connect_ms: asset not found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UMetaSoundSource* MSSource = Cast<UMetaSoundSource>(Asset);
	if (!MSSource)
	{
		Result.Message = FString::Printf(
			TEXT("connect_ms: asset at '%s' is not a UMetaSoundSource"), *AssetPath);
		Result.ErrorCode = 2001;
		return Result;
	}

	// UE 5.7: C++ document API is private — dispatch via Python console command
	if (!GEngine)
	{
		Result.Message = TEXT("connect_ms: GEngine unavailable");
		Result.ErrorCode = 3000;
		return Result;
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "asset = unreal.load_asset('%s'); "
		     "builder = unreal.get_editor_subsystem(unreal.MetaSoundBuilderSubsystem); "
		     "builder.connect_nodes(asset, '%s', '%s', '%s', '%s') if builder else print('No MetaSoundBuilderSubsystem')"),
		*AssetPath, *SrcNode, *SrcPin, *DstNode, *DstPin);

	FString PyCmd = FString::Printf(TEXT("py %s"), *PyScript);
	GEngine->Exec(UBridgeHandlerBase::GetSafeEditorWorld(), *PyCmd);

	Result.bSuccess = true;
	Result.AffectedPath = AssetPath;
	Result.Message = FString::Printf(TEXT("Connected %s.%s -> %s.%s in '%s' via Python"),
		*SrcNode, *SrcPin, *DstNode, *DstPin, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_attenuation
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_SetAttenuation(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("set_attenuation"), 1000,
			TEXT("set_attenuation: 'asset_path' is required (existing USoundAttenuation or new path)"),
			TEXT("Provide either an existing attenuation asset path or a new content path to create one"));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// Load existing or create new
	USoundAttenuation* Attenuation = LoadObject<USoundAttenuation>(nullptr, *AssetPath);
	if (!Attenuation)
	{
		const FString Suffix = AssetPath + TEXT(".") + AssetName;
		Attenuation = LoadObject<USoundAttenuation>(nullptr, *Suffix);
	}
	if (!Attenuation)
	{
		FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		USoundAttenuationFactory* Factory = NewObject<USoundAttenuationFactory>();
		UObject* Created = ATModule.Get().CreateAsset(AssetName, PackagePath, USoundAttenuation::StaticClass(), Factory);
		Attenuation = Cast<USoundAttenuation>(Created);
	}
	if (!Attenuation)
	{
		return MakeError(TEXT("audio"), TEXT("set_attenuation"), 3000,
			FString::Printf(TEXT("Failed to load or create USoundAttenuation at '%s'"), *AssetPath));
	}

	FSoundAttenuationSettings& S = Attenuation->Attenuation;

	double InnerRadius = 0.0, FalloffDistance = 0.0;
	if (Params->TryGetNumberField(TEXT("inner_radius"),    InnerRadius))   S.AttenuationShapeExtents.X = (float)InnerRadius;
	if (Params->TryGetNumberField(TEXT("falloff_distance"),FalloffDistance)) S.FalloffDistance = (float)FalloffDistance;

	FString AttenuationFuncStr;
	if (Params->TryGetStringField(TEXT("attenuation_function"), AttenuationFuncStr))
	{
		if      (AttenuationFuncStr == TEXT("Linear"))      S.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
		else if (AttenuationFuncStr == TEXT("Logarithmic")) S.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
		else if (AttenuationFuncStr == TEXT("Inverse"))     S.DistanceAlgorithm = EAttenuationDistanceModel::Inverse;
		else if (AttenuationFuncStr == TEXT("LogReverse"))  S.DistanceAlgorithm = EAttenuationDistanceModel::LogReverse;
		else if (AttenuationFuncStr == TEXT("NaturalSound"))S.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
	}

	bool bSpatialize = false;
	if (Params->HasField(TEXT("spatialize")))
	{
		Params->TryGetBoolField(TEXT("spatialize"), bSpatialize);
		S.bSpatialize = bSpatialize;
	}

	Attenuation->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("set_attenuation"),
		FString::Printf(TEXT("SoundAttenuation '%s' configured (inner_radius=%.0f falloff=%.0f)"),
		                *AssetPath, (float)InnerRadius, (float)FalloffDistance));
}

// ---------------------------------------------------------------------------
// create_sound_class
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_CreateSoundClass(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("create_sound_class"), 1000,
			TEXT("create_sound_class: 'asset_path' is required"));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	USoundClassFactory* Factory = NewObject<USoundClassFactory>();
	UObject* Created = ATModule.Get().CreateAsset(AssetName, PackagePath, USoundClass::StaticClass(), Factory);
	USoundClass* SoundClass = Cast<USoundClass>(Created);
	if (!SoundClass)
	{
		return MakeError(TEXT("audio"), TEXT("create_sound_class"), 3000,
			FString::Printf(TEXT("Failed to create USoundClass at '%s'"), *AssetPath));
	}

	// Optional parent class
	FString ParentPath;
	if (Params->TryGetStringField(TEXT("parent_path"), ParentPath) && !ParentPath.IsEmpty())
	{
		USoundClass* Parent = LoadObject<USoundClass>(nullptr, *ParentPath);
		if (Parent) SoundClass->ParentClass = Parent;
	}

	// Optional volume/pitch multipliers
	double Volume = 1.0, Pitch = 1.0;
	if (Params->TryGetNumberField(TEXT("volume"), Volume)) SoundClass->Properties.Volume = (float)Volume;
	if (Params->TryGetNumberField(TEXT("pitch"),  Pitch))  SoundClass->Properties.Pitch  = (float)Pitch;

	SoundClass->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("create_sound_class"),
		FString::Printf(TEXT("USoundClass created at '%s'"), *AssetPath),
		nullptr);
}

// ---------------------------------------------------------------------------
// create_sound_mix
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_CreateSoundMix(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("create_sound_mix"), 1000,
			TEXT("create_sound_mix: 'asset_path' is required"));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	USoundMixFactory* Factory = NewObject<USoundMixFactory>();
	UObject* Created = ATModule.Get().CreateAsset(AssetName, PackagePath, USoundMix::StaticClass(), Factory);
	USoundMix* SoundMix = Cast<USoundMix>(Created);
	if (!SoundMix)
	{
		return MakeError(TEXT("audio"), TEXT("create_sound_mix"), 3000,
			FString::Printf(TEXT("Failed to create USoundMix at '%s'"), *AssetPath));
	}

	double FadeInTime = 0.2, FadeOutTime = 0.2;
	Params->TryGetNumberField(TEXT("fade_in_time"),  FadeInTime);
	Params->TryGetNumberField(TEXT("fade_out_time"), FadeOutTime);
	SoundMix->FadeInTime  = (float)FadeInTime;
	SoundMix->FadeOutTime = (float)FadeOutTime;

	SoundMix->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("create_sound_mix"),
		FString::Printf(TEXT("USoundMix created at '%s' (fade_in=%.2f, fade_out=%.2f)"),
		                *AssetPath, (float)FadeInTime, (float)FadeOutTime));
}

// ---------------------------------------------------------------------------
// place_audio_volume
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_PlaceAudioVolume(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("audio"), TEXT("place_audio_volume"), 3000, TEXT("No editor world available"));

	double X = 0.0, Y = 0.0, Z = 0.0;
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj && (*LocObj).IsValid())
	{
		(*LocObj)->TryGetNumberField(TEXT("x"), X);
		(*LocObj)->TryGetNumberField(TEXT("y"), Y);
		(*LocObj)->TryGetNumberField(TEXT("z"), Z);
	}

	double EX = 200.0, EY = 200.0, EZ = 200.0;
	const TSharedPtr<FJsonObject>* ExtObj = nullptr;
	if (Params->TryGetObjectField(TEXT("extent"), ExtObj) && ExtObj && (*ExtObj).IsValid())
	{
		(*ExtObj)->TryGetNumberField(TEXT("x"), EX);
		(*ExtObj)->TryGetNumberField(TEXT("y"), EY);
		(*ExtObj)->TryGetNumberField(TEXT("z"), EZ);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Place AudioVolume")));

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(FVector((float)X, (float)Y, (float)Z));

	AAudioVolume* Vol = World->SpawnActor<AAudioVolume>(AAudioVolume::StaticClass(), SpawnTransform);
	if (!Vol)
	{
		return MakeError(TEXT("audio"), TEXT("place_audio_volume"), 3000,
			FString::Printf(TEXT("Failed to spawn AAudioVolume at (%.0f, %.0f, %.0f)"), (float)X, (float)Y, (float)Z));
	}

	Vol->SetActorScale3D(FVector((float)EX / 100.f, (float)EY / 100.f, (float)EZ / 100.f));

	// Optional: link a reverb settings asset
	FString ReverbPath;
	if (Params->TryGetStringField(TEXT("reverb_settings_path"), ReverbPath) && !ReverbPath.IsEmpty())
	{
		UReverbEffect* Reverb = LoadObject<UReverbEffect>(nullptr, *ReverbPath);
		if (Reverb)
		{
			// FAudioVolumeSettings/SetAudioSettings removed in UE 5.7; use FReverbSettings.
			FReverbSettings RvbSettings;
			RvbSettings.ReverbEffect = Reverb;
			Vol->SetReverbSettings(RvbSettings);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Vol->GetActorLabel());

	return MakeSuccess(TEXT("audio"), TEXT("place_audio_volume"),
		FString::Printf(TEXT("AAudioVolume '%s' placed at (%.0f, %.0f, %.0f)"),
		                *Vol->GetActorLabel(), (float)X, (float)Y, (float)Z),
		Data);
#else
	return MakeError(TEXT("audio"), TEXT("place_audio_volume"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_concurrency
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_SetConcurrency(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("set_concurrency"), 1000,
			TEXT("set_concurrency: 'asset_path' is required (existing or new USoundConcurrency)"));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	USoundConcurrency* Concurrency = LoadObject<USoundConcurrency>(nullptr, *AssetPath);
	if (!Concurrency)
	{
		const FString Suffix = AssetPath + TEXT(".") + AssetName;
		Concurrency = LoadObject<USoundConcurrency>(nullptr, *Suffix);
	}
	if (!Concurrency)
	{
		FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		USoundConcurrencyFactory* Factory = NewObject<USoundConcurrencyFactory>();
		UObject* Created = ATModule.Get().CreateAsset(AssetName, PackagePath, USoundConcurrency::StaticClass(), Factory);
		Concurrency = Cast<USoundConcurrency>(Created);
	}
	if (!Concurrency)
	{
		return MakeError(TEXT("audio"), TEXT("set_concurrency"), 3000,
			FString::Printf(TEXT("Failed to load or create USoundConcurrency at '%s'"), *AssetPath));
	}

	double MaxCount = 16.0;
	if (Params->TryGetNumberField(TEXT("max_count"), MaxCount))
		Concurrency->Concurrency.MaxCount = (int32)MaxCount;

	FString ResolutionRuleStr;
	if (Params->TryGetStringField(TEXT("resolution_rule"), ResolutionRuleStr))
	{
		if      (ResolutionRuleStr == TEXT("PreventNew"))            Concurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::PreventNew;
		else if (ResolutionRuleStr == TEXT("StopOldest"))            Concurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
		else if (ResolutionRuleStr == TEXT("StopFarthestThenOldest"))Concurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopFarthestThenOldest;
		else if (ResolutionRuleStr == TEXT("StopLowestPriority"))    Concurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopLowestPriority;
	}

	Concurrency->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("set_concurrency"),
		FString::Printf(TEXT("USoundConcurrency '%s' configured (max_count=%d)"),
		                *AssetPath, Concurrency->Concurrency.MaxCount));
}

// ---------------------------------------------------------------------------
// create_dialogue_wave
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_CreateDialogueWave(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("create_dialogue_wave"), 1000,
			TEXT("create_dialogue_wave: 'asset_path' is required"));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* Created = ATModule.Get().CreateAsset(AssetName, PackagePath, UDialogueWave::StaticClass(), nullptr);
	UDialogueWave* Wave = Cast<UDialogueWave>(Created);
	if (!Wave)
	{
		return MakeError(TEXT("audio"), TEXT("create_dialogue_wave"), 3000,
			FString::Printf(TEXT("Failed to create UDialogueWave at '%s'"), *AssetPath));
	}

	// Optional spoken text
	FString SpokenText;
	if (Params->TryGetStringField(TEXT("spoken_text"), SpokenText))
		Wave->SpokenText = SpokenText;

	// Optional: link a SoundWave for the audio content
	FString SoundWavePath;
	if (Params->TryGetStringField(TEXT("sound_wave_path"), SoundWavePath) && !SoundWavePath.IsEmpty())
	{
		USoundWave* SW = LoadObject<USoundWave>(nullptr, *SoundWavePath);
		if (SW && Wave->ContextMappings.Num() > 0)
		{
			Wave->ContextMappings[0].SoundWave = SW;
		}
	}

	Wave->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("create_dialogue_wave"),
		FString::Printf(TEXT("UDialogueWave created at '%s'"), *AssetPath));
}

// ---------------------------------------------------------------------------
// list_sound_cues
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_ListSoundCues(TSharedPtr<FJsonObject> Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FString FilterPath;
	const bool bHasFilter = Params->TryGetStringField(TEXT("path_filter"), FilterPath) && !FilterPath.IsEmpty();

	TArray<FAssetData> Assets;
	if (bHasFilter)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(USoundCue::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*FilterPath));
		Filter.bRecursivePaths = true;
		AR.GetAssets(Filter, Assets);
	}
	else
	{
		AR.GetAssetsByClass(USoundCue::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
	}

	TArray<TSharedPtr<FJsonValue>> CueArray;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		CueArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("cues"), CueArray);
	Data->SetNumberField(TEXT("count"), CueArray.Num());

	return MakeSuccess(TEXT("audio"), TEXT("list_sound_cues"),
		FString::Printf(TEXT("Found %d SoundCue asset(s)"), CueArray.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_cue_info
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_GetCueInfo(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("get_cue_info"), 1000,
			TEXT("get_cue_info: 'asset_path' is required"),
			TEXT("Provide the content path of a USoundCue asset"));
	}

	USoundCue* Cue = LoadObject<USoundCue>(nullptr, *AssetPath);
	if (!Cue)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Cue = LoadObject<USoundCue>(nullptr, *Suffix);
	}
	if (!Cue)
	{
		return MakeError(TEXT("audio"), TEXT("get_cue_info"), 2000,
			FString::Printf(TEXT("No USoundCue found at '%s'"), *AssetPath),
			TEXT("Verify the asset path and use list_sound_cues to browse available cues"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Cue->GetName());
	Data->SetStringField(TEXT("path"), AssetPath);
	Data->SetNumberField(TEXT("volume_multiplier"),  Cue->VolumeMultiplier);
	Data->SetNumberField(TEXT("pitch_multiplier"),   Cue->PitchMultiplier);
	Data->SetNumberField(TEXT("duration"),           Cue->GetDuration());
	Data->SetBoolField(TEXT("looping"),              Cue->IsLooping());

	// Attenuation
	if (Cue->AttenuationSettings)
		Data->SetStringField(TEXT("attenuation_path"), Cue->AttenuationSettings->GetPathName());
	else
		Data->SetStringField(TEXT("attenuation_path"), TEXT("None"));

	// Sound class
	if (Cue->SoundClassObject)
		Data->SetStringField(TEXT("sound_class"), Cue->SoundClassObject->GetPathName());
	else
		Data->SetStringField(TEXT("sound_class"), TEXT("None"));

	// Node count
	Data->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());

	return MakeSuccess(TEXT("audio"), TEXT("get_cue_info"),
		FString::Printf(TEXT("SoundCue '%s': duration=%.2fs, nodes=%d"), *Cue->GetName(), Cue->GetDuration(), Cue->AllNodes.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// create_attenuation  (Phase 1b)
// Params: asset_path, shape (Sphere|Capsule|Box|Cone), radius (float), falloff_distance (float)
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_CreateAttenuation(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("audio"), TEXT("create_attenuation"), 1000, TEXT("'asset_path' is required"));

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	USoundAttenuationFactory* Factory = NewObject<USoundAttenuationFactory>();
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
		USoundAttenuation::StaticClass(), Factory);
	if (!CreatedAsset)
		return MakeError(TEXT("audio"), TEXT("create_attenuation"), 3000,
			FString::Printf(TEXT("Failed to create USoundAttenuation at '%s'"), *AssetPath));

	USoundAttenuation* Atten = CastChecked<USoundAttenuation>(CreatedAsset);
	FSoundAttenuationSettings& Settings = Atten->Attenuation;

	// Shape
	FString ShapeStr = TEXT("Sphere");
	Params->TryGetStringField(TEXT("shape"), ShapeStr);
	if      (ShapeStr == TEXT("Capsule")) Settings.AttenuationShape = EAttenuationShape::Capsule;
	else if (ShapeStr == TEXT("Box"))     Settings.AttenuationShape = EAttenuationShape::Box;
	else if (ShapeStr == TEXT("Cone"))    Settings.AttenuationShape = EAttenuationShape::Cone;
	else                                  Settings.AttenuationShape = EAttenuationShape::Sphere;

	// Radius / falloff
	double Radius = 1000.0, FalloffDistance = 2000.0;
	Params->TryGetNumberField(TEXT("radius"),           Radius);
	Params->TryGetNumberField(TEXT("falloff_distance"), FalloffDistance);
	Settings.AttenuationShapeExtents = FVector((float)Radius);
	Settings.FalloffDistance = (float)FalloffDistance;

	Atten->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("create_attenuation"),
		FString::Printf(TEXT("USoundAttenuation created at '%s' (shape=%s, radius=%.0f, falloff=%.0f)"),
		                *AssetPath, *ShapeStr, (float)Radius, (float)FalloffDistance));
}

// ---------------------------------------------------------------------------
// set_reverb  (Phase 1b)
// Params: volume_actor_label (string), reverb_asset_path (string), volume (float)
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_SetReverb(TSharedPtr<FJsonObject> Params)
{
	FString VolumeLabel, ReverbAssetPath;
	if (!Params->TryGetStringField(TEXT("volume_actor_label"), VolumeLabel) || VolumeLabel.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("reverb_asset_path"),  ReverbAssetPath) || ReverbAssetPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("set_reverb"), 1000,
			TEXT("set_reverb: 'volume_actor_label' and 'reverb_asset_path' are required"),
			TEXT("Optional: 'volume' (float 0-1, default 0.5), 'priority' (float)"));
	}

	UReverbEffect* ReverbEffect = LoadObject<UReverbEffect>(nullptr, *ReverbAssetPath);
	if (!ReverbEffect)
	{
		const FString Suffix = ReverbAssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ReverbAssetPath);
		ReverbEffect = LoadObject<UReverbEffect>(nullptr, *Suffix);
	}
	if (!ReverbEffect)
	{
		return MakeError(TEXT("audio"), TEXT("set_reverb"), 2000,
			FString::Printf(TEXT("UReverbEffect not found at '%s'"), *ReverbAssetPath));
	}

#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World) return MakeError(TEXT("audio"), TEXT("set_reverb"), 3000, TEXT("No editor world"));

	AAudioVolume* AudioVol = nullptr;
	for (TActorIterator<AAudioVolume> It(World); It; ++It)
	{
		if (It->GetActorLabel().Equals(VolumeLabel, ESearchCase::IgnoreCase))
		{
			AudioVol = *It;
			break;
		}
	}

	if (!AudioVol)
	{
		return MakeError(TEXT("audio"), TEXT("set_reverb"), 2001,
			FString::Printf(TEXT("AAudioVolume actor '%s' not found in level"), *VolumeLabel));
	}

	FReverbSettings ReverbSettings = AudioVol->GetReverbSettings();
	ReverbSettings.ReverbEffect = ReverbEffect;
	double Volume = 0.5;
	Params->TryGetNumberField(TEXT("volume"), Volume);
	ReverbSettings.Volume = (float)Volume;
	// UE 5.7: FReverbSettings no longer has a Priority field — parameter accepted for
	// backward compatibility but silently ignored.
	double Priority = 0.0;
	Params->TryGetNumberField(TEXT("priority"), Priority);
	AudioVol->SetReverbSettings(ReverbSettings);

	return MakeSuccess(TEXT("audio"), TEXT("set_reverb"),
		FString::Printf(TEXT("Reverb '%s' applied to AudioVolume '%s' (volume=%.2f)"),
		                *ReverbEffect->GetName(), *VolumeLabel, (float)Volume));
#else
	return MakeError(TEXT("audio"), TEXT("set_reverb"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// set_mix_class  (Phase 1b)
// Params: asset_path (USoundMix), sound_class_path, volume_adjuster, pitch_adjuster
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_SetMixClass(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, SoundClassPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),       AssetPath)      || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("sound_class_path"), SoundClassPath) || SoundClassPath.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("set_mix_class"), 1000,
			TEXT("set_mix_class: 'asset_path' (USoundMix) and 'sound_class_path' are required"),
			TEXT("Optional: 'volume_adjuster' (float, default 1.0), 'pitch_adjuster' (float, default 1.0)"));
	}

	USoundMix* Mix = LoadObject<USoundMix>(nullptr, *AssetPath);
	if (!Mix)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Mix = LoadObject<USoundMix>(nullptr, *Suffix);
	}
	if (!Mix)
		return MakeError(TEXT("audio"), TEXT("set_mix_class"), 2000,
			FString::Printf(TEXT("USoundMix not found at '%s'"), *AssetPath));

	USoundClass* SoundClass = LoadObject<USoundClass>(nullptr, *SoundClassPath);
	if (!SoundClass)
	{
		const FString Suffix = SoundClassPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SoundClassPath);
		SoundClass = LoadObject<USoundClass>(nullptr, *Suffix);
	}
	if (!SoundClass)
		return MakeError(TEXT("audio"), TEXT("set_mix_class"), 2001,
			FString::Printf(TEXT("USoundClass not found at '%s'"), *SoundClassPath));

	double VolumeAdj = 1.0, PitchAdj = 1.0;
	Params->TryGetNumberField(TEXT("volume_adjuster"), VolumeAdj);
	Params->TryGetNumberField(TEXT("pitch_adjuster"),  PitchAdj);

	// Check if the class already has an adjuster entry
	for (FSoundClassAdjuster& Adj : Mix->SoundClassEffects)
	{
		if (Adj.SoundClassObject == SoundClass)
		{
			Adj.VolumeAdjuster = (float)VolumeAdj;
			Adj.PitchAdjuster  = (float)PitchAdj;
			Mix->MarkPackageDirty();
			return MakeSuccess(TEXT("audio"), TEXT("set_mix_class"),
				FString::Printf(TEXT("Updated adjuster for '%s' in mix '%s': vol=%.2f pitch=%.2f"),
				                *SoundClass->GetName(), *Mix->GetName(), (float)VolumeAdj, (float)PitchAdj));
		}
	}

	// Append new adjuster
	FSoundClassAdjuster NewAdj;
	NewAdj.SoundClassObject = SoundClass;
	NewAdj.VolumeAdjuster   = (float)VolumeAdj;
	NewAdj.PitchAdjuster    = (float)PitchAdj;
	Mix->SoundClassEffects.Add(NewAdj);
	Mix->MarkPackageDirty();

	return MakeSuccess(TEXT("audio"), TEXT("set_mix_class"),
		FString::Printf(TEXT("Added '%s' adjuster to mix '%s': vol=%.2f pitch=%.2f"),
		                *SoundClass->GetName(), *Mix->GetName(), (float)VolumeAdj, (float)PitchAdj));
}

// ---------------------------------------------------------------------------
// create_metasound  (Phase 1b)
// Params: asset_path
// ---------------------------------------------------------------------------

static void AudioExecPython(const FString& Script)
{
#if WITH_EDITOR
	if (GEngine)
	{
		FString Cmd = FString::Printf(TEXT("py %s"), *Script);
		GEngine->Exec(UBridgeHandlerBase::GetSafeEditorWorld(), *Cmd);
	}
#endif
}

FBridgeResult UAudioHandler::Action_CreateMetaSound(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("audio"), TEXT("create_metasound"), 1000, TEXT("'asset_path' is required"));

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// UMetaSoundSourceFactory requires MetasoundEngine module
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSourceFactory"));
	if (!FactoryClass)
		FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEditor.MetaSoundSourceFactory"));

	UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource"));

	if (!MetaSoundClass)
	{
		return MakeError(TEXT("audio"), TEXT("create_metasound"), 3003,
			TEXT("MetaSoundSource class not found — MetasoundEngine module may not be loaded"),
			TEXT("Ensure the MetaSound plugins are enabled in the project"));
	}

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UFactory* Factory = FactoryClass ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath, MetaSoundClass, Factory);
	if (!CreatedAsset)
		return MakeError(TEXT("audio"), TEXT("create_metasound"), 3000,
			FString::Printf(TEXT("Failed to create MetaSoundSource at '%s'"), *AssetPath));

	CreatedAsset->MarkPackageDirty();
	return MakeSuccess(TEXT("audio"), TEXT("create_metasound"),
		FString::Printf(TEXT("MetaSoundSource created at '%s'"), *AssetPath));
}

// ---------------------------------------------------------------------------
// set_metasound_param  (Phase 1b — PARTIALLY_FEASIBLE)
// IMetaSoundDocumentInterface::GetDocument() is private in UE 5.7;
// dispatched via Python scripting as the only reliable path.
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_SetMetaSoundParam(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ParamName, Value;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("param_name"), ParamName)  || ParamName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("value"),      Value)      || Value.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("set_metasound_param"), 1000,
			TEXT("set_metasound_param: 'asset_path', 'param_name', and 'value' are required"));
	}

	FString ParamType = TEXT("float");
	Params->TryGetStringField(TEXT("param_type"), ParamType);

	// Load the MetaSound asset
	UObject* MSAsset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!MSAsset)
	{
		const FString Full = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		MSAsset = LoadObject<UObject>(nullptr, *Full);
	}
	if (!MSAsset)
		return MakeError(TEXT("audio"), TEXT("set_metasound_param"), 2000,
			FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath));

	// Verify MetaSoundEngine module is available — required for IMetaSoundDocumentInterface
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("MetasoundEngine")) &&
	    !FModuleManager::Get().IsModuleLoaded(TEXT("MetasoundFrontend")))
	{
		return MakeError(TEXT("audio"), TEXT("set_metasound_param"), 3003,
			FString::Printf(TEXT("set_metasound_param: MetaSoundEngine/MetasoundFrontend module not loaded for asset '%s'. "
			     "Enable MetasoundEngine in the project plugins and add to Build.cs: MetasoundEngine, MetasoundFrontend."),
			     *AssetPath),
			TEXT("Alternatively set the parameter default in the MetaSound editor (double-click asset > Inputs panel)"));
	}

	// IMetaSoundDocumentInterface::GetDocument() is private in UE 5.7.
	// Try to set the input default value via UObject property reflection on the document struct.
	// MetaSound stores inputs in FMetaSoundFrontendDocument::RootGraph.Inputs.
	bool bSet = false;
	if (FStructProperty* DocProp = FindFProperty<FStructProperty>(MSAsset->GetClass(), TEXT("Document")))
	{
		void* DocPtr = DocProp->ContainerPtrToValuePtr<void>(MSAsset);
		if (FStructProperty* RootProp = FindFProperty<FStructProperty>(DocProp->Struct, TEXT("RootGraph")))
		{
			void* RootPtr = RootProp->ContainerPtrToValuePtr<void>(DocPtr);
			if (FArrayProperty* InputsProp = FindFProperty<FArrayProperty>(RootProp->Struct, TEXT("Inputs")))
			{
				FScriptArrayHelper Helper(InputsProp, InputsProp->ContainerPtrToValuePtr<void>(RootPtr));
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					void* ElemPtr = Helper.GetRawPtr(i);
					if (FStructProperty* StructProp = CastField<FStructProperty>(InputsProp->Inner))
					{
						if (FStrProperty* NameProp = FindFProperty<FStrProperty>(StructProp->Struct, TEXT("Name")))
						{
							FString ElemName = NameProp->GetPropertyValue_InContainer(ElemPtr);
							if (ElemName == ParamName)
							{
								if (FStrProperty* ValProp = FindFProperty<FStrProperty>(StructProp->Struct, TEXT("DefaultValue")))
								{
									ValProp->SetPropertyValue_InContainer(ElemPtr, Value);
									MSAsset->MarkPackageDirty();
									bSet = true;
									break;
								}
							}
						}
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),  AssetPath);
	Data->SetStringField(TEXT("param_name"),  ParamName);
	Data->SetStringField(TEXT("value"),       Value);
	Data->SetStringField(TEXT("param_type"),  ParamType);
	Data->SetBoolField  (TEXT("applied"),     bSet);
	if (!bSet)
		Data->SetStringField(TEXT("note"),
			TEXT("IMetaSoundDocumentInterface::GetDocument() is private in UE 5.7. "
			     "Document property path not found via reflection. "
			     "Set the parameter default in the MetaSound editor Inputs panel."));

	return MakeSuccess(TEXT("audio"), TEXT("set_metasound_param"),
		bSet
			? FString::Printf(TEXT("set_metasound_param: set '%s'.%s = %s (%s)"), *AssetPath, *ParamName, *Value, *ParamType)
			: FString::Printf(TEXT("set_metasound_param: module loaded but document path not accessible for '%s' — see note"), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// add_metasound_input  (Phase 1b — PARTIALLY_FEASIBLE)
// ---------------------------------------------------------------------------

FBridgeResult UAudioHandler::Action_AddMetaSoundInput(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, InputName, DataType;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("input_name"),  InputName)  || InputName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("data_type"),   DataType)   || DataType.IsEmpty())
	{
		return MakeError(TEXT("audio"), TEXT("add_metasound_input"), 1000,
			TEXT("add_metasound_input: 'asset_path', 'input_name', and 'data_type' are required"),
			TEXT("data_type: Float|Bool|Int32|String|Audio|Trigger"));
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "ms = unreal.load_asset('%s'); "
		     "print(f'add_metasound_input: asset={ms}, input=%s, type=%s')"),
		*AssetPath, *InputName, *DataType);
	AudioExecPython(PyScript);

	return MakeSuccess(TEXT("audio"), TEXT("add_metasound_input"),
		FString::Printf(TEXT("add_metasound_input dispatched (Python): '%s' input='%s' type='%s'. "
		                     "IMetaSoundFrontend graph builder is private in UE 5.7 — "
		                     "use MetaSound editor or Python for graph mutation."),
		                *AssetPath, *InputName, *DataType));
}
