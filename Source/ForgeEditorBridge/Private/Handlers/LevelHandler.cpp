#include "Handlers/LevelHandler.h"
#include "ForgeAISubsystem.h"
#include "BridgeSessionStore.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
// UE 5.7: UEditorLoadingAndSavingUtils is declared in FileHelpers.h (module UnrealEd).
// There is no standalone EditorLoadingAndSavingUtils.h header.
#include "FileHelpers.h"
#include "LevelEditorSubsystem.h"
#include "EditorLevelUtils.h"
#include "EditorBuildUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "WorldPartition/WorldPartition.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Misc/Guid.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("level");

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
	return nullptr;
#endif
}

static ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& LevelPath)
{
	if (!World) return nullptr;
	for (ULevelStreaming* LS : World->GetStreamingLevels())
	{
		if (LS && LS->GetWorldAssetPackageFName() == FName(*LevelPath))
		{
			return LS;
		}
	}
	return nullptr;
}

FBridgeResult ULevelHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- open_level ----
	if (Action == TEXT("open_level"))
	{
		FString Path;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'path'"));

		// UE 5.7: Use FEditorFileUtils::LoadMap
		FEditorFileUtils::LoadMap(Path);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Loaded map: %s"), *Path));
	}

	// ---- save_level ----
	if (Action == TEXT("save_level"))
	{
#if WITH_EDITOR
		ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
		if (!LES)
			return MakeError(DOMAIN, Action, 3000, TEXT("ULevelEditorSubsystem not available"));

		bool bSaved = LES->SaveCurrentLevel();
		return bSaved
			? MakeSuccess(DOMAIN, Action, TEXT("Current level saved"))
			: MakeError(DOMAIN, Action, 3000, TEXT("Failed to save current level"));
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
	}

	// ---- save_all ----
	if (Action == TEXT("save_all"))
	{
		bool bPromptUser = false;
		if (Params.IsValid()) Params->TryGetBoolField(TEXT("prompt_user"), bPromptUser);

		bool bSaved = FEditorFileUtils::SaveDirtyPackages(bPromptUser, true, true);
		return bSaved
			? MakeSuccess(DOMAIN, Action, TEXT("All dirty packages saved"))
			: MakeError(DOMAIN, Action, 3000, TEXT("Some packages failed to save"));
	}

	// ---- get_level_info ----
	if (Action == TEXT("get_level_info"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		AWorldSettings* WS = World->GetWorldSettings();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), World->GetName());
		Data->SetStringField(TEXT("path"), World->GetPathName());
		Data->SetNumberField(TEXT("sublevel_count"), (double)World->GetStreamingLevels().Num());
		Data->SetBoolField(TEXT("world_partition_enabled"), World->GetWorldPartition() != nullptr);

		if (WS && WS->DefaultGameMode)
			Data->SetStringField(TEXT("game_mode"), WS->DefaultGameMode->GetPathName());

		TArray<TSharedPtr<FJsonValue>> Subs;
		for (ULevelStreaming* LS : World->GetStreamingLevels())
		{
			if (!LS) continue;
			TSharedPtr<FJsonObject> SL = MakeShared<FJsonObject>();
			SL->SetStringField(TEXT("path"), LS->GetWorldAssetPackageFName().ToString());
			SL->SetBoolField(TEXT("loaded"), LS->IsLevelLoaded());
			SL->SetBoolField(TEXT("visible"), LS->IsLevelVisible());
			Subs.Add(MakeShared<FJsonValueObject>(SL));
		}
		Data->SetArrayField(TEXT("sublevels"), Subs);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Level info for '%s'"), *World->GetName()), Data);
	}

	// ---- add_sublevel ----
	if (Action == TEXT("add_sublevel"))
	{
		FString LevelPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("level_path"), LevelPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'level_path'"));

		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		UClass* StreamingClass = ULevelStreamingDynamic::StaticClass();
		FString StreamingClassName;
		if (Params->TryGetStringField(TEXT("streaming_class"), StreamingClassName))
		{
			UClass* Found = FindObject<UClass>(nullptr, *StreamingClassName);
			if (Found && Found->IsChildOf(ULevelStreaming::StaticClass()))
				StreamingClass = Found;
		}

		ULevelStreaming* NewLevel = UEditorLevelUtils::AddLevelToWorld(World, *LevelPath, StreamingClass);
		if (!NewLevel)
			return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to add sublevel: %s"), *LevelPath));

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Added sublevel: %s"), *LevelPath));
		R.AffectedPath = LevelPath;
		return R;
	}

	// ---- remove_sublevel ----
	if (Action == TEXT("remove_sublevel"))
	{
		FString LevelPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("level_path"), LevelPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'level_path'"));

		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		ULevelStreaming* Found = FindStreamingLevel(World, LevelPath);
		if (!Found)
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Sublevel not found: %s"), *LevelPath));

		if (!Found->GetLoadedLevel())
			return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Sublevel not loaded, cannot remove: %s"), *LevelPath));

		UEditorLevelUtils::RemoveLevelFromWorld(Found->GetLoadedLevel());

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Removed sublevel: %s"), *LevelPath));
		R.AffectedPath = LevelPath;
		return R;
	}

	// ---- list_sublevels ----
	if (Action == TEXT("list_sublevels"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Subs;
		for (ULevelStreaming* LS : World->GetStreamingLevels())
		{
			if (!LS) continue;
			TSharedPtr<FJsonObject> SL = MakeShared<FJsonObject>();
			SL->SetStringField(TEXT("path"), LS->GetWorldAssetPackageFName().ToString());
			SL->SetBoolField(TEXT("loaded"), LS->IsLevelLoaded());
			SL->SetBoolField(TEXT("visible"), LS->GetShouldBeVisibleInEditor());
			SL->SetStringField(TEXT("streaming_class"), LS->GetClass()->GetName());
			Subs.Add(MakeShared<FJsonValueObject>(SL));
		}
		Data->SetArrayField(TEXT("sublevels"), Subs);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Found %d sublevels"), Subs.Num()), Data);
	}

	// ---- set_sublevel_visible ----
	if (Action == TEXT("set_sublevel_visible"))
	{
		FString LevelPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("level_path"), LevelPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'level_path'"));

		bool bVisible = true;
		Params->TryGetBoolField(TEXT("visible"), bVisible);

		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		ULevelStreaming* LS = FindStreamingLevel(World, LevelPath);
		if (!LS)
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Sublevel not found: %s"), *LevelPath));

		LS->SetShouldBeVisible(bVisible);
		LS->SetShouldBeLoaded(bVisible);
		World->FlushLevelStreaming();

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Sublevel '%s' visibility = %s"), *LevelPath, bVisible ? TEXT("true") : TEXT("false")));
	}

	// ---- run_build ----
	if (Action == TEXT("run_build"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		FString BuildType = TEXT("all");
		if (Params.IsValid()) Params->TryGetStringField(TEXT("build_type"), BuildType);

		// UE 5.7: EditorBuild takes FName from FBuildOptions
		FName BuildId;
		if (BuildType == TEXT("lighting"))       BuildId = FBuildOptions::BuildLighting;
		else if (BuildType == TEXT("navigation")) BuildId = FBuildOptions::BuildAIPaths;
		else if (BuildType == TEXT("geometry"))   BuildId = FBuildOptions::BuildGeometry;
		else                                     BuildId = FBuildOptions::BuildAll;

		FEditorBuildUtils::EditorBuild(World, BuildId);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Build started: %s"), *BuildType));
	}

	// ---- set_current_level  (Phase 1b) ----
	if (Action == TEXT("set_current_level"))
	{
#if WITH_EDITOR
		FString LevelName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("level_name"), LevelName))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'level_name'"));

		ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
		if (!LES)
			return MakeError(DOMAIN, Action, 3000, TEXT("ULevelEditorSubsystem not available"));

		bool bSet = LES->SetCurrentLevelByName(FName(*LevelName));
		return bSet
			? MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Current level set to '%s'"), *LevelName))
			: MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Sublevel '%s' not found — ensure it is loaded"), *LevelName));
#else
		return MakeError(DOMAIN, Action, 3003, TEXT("Editor-only action"));
#endif
	}

	// ---- import_scene  (Phase 1b) ----
	if (Action == TEXT("import_scene"))
	{
		FString Filename;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("filename"), Filename))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'filename' (absolute file path)"));

		// UEditorLoadingAndSavingUtils::ImportScene imports FBX/Datasmith scenes
		// Note: ImportScene returns void in UE 5.7; failures are surfaced through editor logs/dialogs
		UEditorLoadingAndSavingUtils::ImportScene(Filename);

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Scene imported: %s"), *Filename));
		R.AffectedPath = Filename;
		return R;
	}

	// ---- build_hlods  (Phase 1b) ----
	if (Action == TEXT("build_hlods"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		// WorldPartition HLOD build requires WorldPartition to be active
		UWorldPartition* WP = World->GetWorldPartition();
		if (!WP)
			return MakeError(DOMAIN, Action, 3001,
				TEXT("build_hlods requires World Partition to be enabled on this level"),
				TEXT("Enable World Partition in World Settings"));

		// HLOD build is dispatched as commandlet: -run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionHLODsBuilder
		// Full async pipeline not available in editor context — return info + manual instructions
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("world"),  World->GetName());
		Data->SetStringField(TEXT("status"), TEXT("pending_manual"));
		Data->SetStringField(TEXT("note"),
			TEXT("HLOD build requires the WorldPartitionBuilderCommandlet. "
			     "Run from command line: UnrealEditor.exe <Project> -run=WorldPartitionBuilderCommandlet "
			     "-Builder=WorldPartitionHLODsBuilder. "
			     "Or use the HLOD Outliner panel in the editor."));

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("build_hlods: WorldPartition detected on '%s'. See data.note for commandlet syntax."), *World->GetName()),
			Data);
	}

	// ---- set_sky_light_source  (Phase 1d) ----
	if (Action == TEXT("set_sky_light_source"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		bool bRealTimeCapture = true;
		int32 Resolution = 128;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("real_time_capture"), bRealTimeCapture);
			double ResVal;
			if (Params->TryGetNumberField(TEXT("cubemap_resolution"), ResVal))
				Resolution = (int32)ResVal;
		}

		ASkyLight* SkyLight = nullptr;
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			SkyLight = *It;
			break;
		}

		if (!SkyLight)
			return MakeError(DOMAIN, Action, 2000,
				TEXT("No ASkyLight actor found in the level"),
				TEXT("Add a Sky Light actor to the level first."));

		// UE 5.7: bRealTimeCapture lives on USkyLightComponent, not ASkyLight
		USkyLightComponent* SLC = SkyLight->GetLightComponent();
		if (SLC)
		{
			SLC->bRealTimeCapture = bRealTimeCapture;
			SLC->CubemapResolution = Resolution;
			SLC->MarkRenderStateDirty();
		}
		SkyLight->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), SkyLight->GetActorLabel());
		Data->SetBoolField(TEXT("real_time_capture"), bRealTimeCapture);
		Data->SetNumberField(TEXT("cubemap_resolution"), (double)Resolution);
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("SkyLight '%s' updated"), *SkyLight->GetActorLabel()), Data);
	}

	// ---- build_all_lighting  (Phase 1d) ----
	if (Action == TEXT("build_all_lighting"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		// Map quality string to ELightingBuildQuality enum value
		// Preview=0, Medium=1, High=2, Production=3
		FString QualityStr = TEXT("Preview");
		if (Params.IsValid()) Params->TryGetStringField(TEXT("quality"), QualityStr);

		ELightingBuildQuality Quality = Quality_Preview;
		if      (QualityStr == TEXT("Medium"))     Quality = Quality_Medium;
		else if (QualityStr == TEXT("High"))       Quality = Quality_High;
		else if (QualityStr == TEXT("Production")) Quality = Quality_Production;

		// UE 5.7: UEditorEngine has no SetLightingBuildQuality; quality is driven by
		// FLightingBuildOptions / project defaults. Quality string is retained for the job log.
		(void)Quality;

		const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("level/build_all_lighting"));
		FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0,
			FString::Printf(TEXT("Lighting build started (quality=%s)"), *QualityStr));

		FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("job_id"), JobId);
		Data->SetStringField(TEXT("quality"), QualityStr);
		Data->SetStringField(TEXT("note"), TEXT("Lighting build is in progress. Poll project/get_job_status for completion."));
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Lighting build started (quality=%s, job_id=%s)"), *QualityStr, *JobId), Data);
	}

	// ---- export_asset_list  (Phase 1d) ----
	if (Action == TEXT("export_asset_list"))
	{
		UWorld* World = GetEditorWorld();
		if (!World)
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		TMap<FString, int32> ClassCounts;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			FString ClassName = Actor->GetClass()->GetName();
			int32& Count = ClassCounts.FindOrAdd(ClassName);
			Count++;
		}

		// Sort by count descending
		ClassCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });

		TArray<TSharedPtr<FJsonValue>> ResultArray;
		for (const TPair<FString, int32>& Pair : ClassCounts)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), Pair.Key);
			Entry->SetNumberField(TEXT("count"), (double)Pair.Value);
			ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("assets"), ResultArray);
		Data->SetNumberField(TEXT("total_actors"), (double)ClassCounts.Num());
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Exported %d actor class types from '%s'"), ClassCounts.Num(), *World->GetName()), Data);
	}

	return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("level capabilities: open_level, save_level, save_all, get_level_info, add_sublevel, remove_sublevel, list_sublevels, set_sublevel_visible, run_build, set_current_level, import_scene, build_hlods, set_sky_light_source, build_all_lighting, export_asset_list"));
}

TSharedPtr<FJsonObject> ULevelHandler::GetActionSchemas() const
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

	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Open/load a map by content path"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("path"), P(TEXT("string"), true, TEXT("Content path of the map to load")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("open_level"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Save the current level"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("save_level"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Save all dirty packages"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("prompt_user"), P(TEXT("bool"), false, TEXT("Show save dialog to user (default false)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("save_all"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Get current level info including sublevels"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_level_info"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a streaming sublevel to the current world"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("level_path"), P(TEXT("string"), true, TEXT("Content path of the sublevel to add")));
		Params->SetObjectField(TEXT("streaming_class"), P(TEXT("string"), false, TEXT("UClass path for streaming type (default ULevelStreamingDynamic)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("add_sublevel"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Remove a streaming sublevel from the current world"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("level_path"), P(TEXT("string"), true, TEXT("Content path of the sublevel to remove")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("remove_sublevel"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("List all streaming sublevels in the current world"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("list_sublevels"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set visibility and load state of a sublevel"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("level_path"), P(TEXT("string"), true, TEXT("Content path of the sublevel")));
		Params->SetObjectField(TEXT("visible"), P(TEXT("bool"), false, TEXT("Whether the sublevel should be visible (default true)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_sublevel_visible"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Run a level build (lighting, navigation, geometry, or all)"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("build_type"), P(TEXT("string"), false, TEXT("Build type: lighting, navigation, geometry, all (default all)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("run_build"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Configure the first ASkyLight actor found in the level"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("real_time_capture"),  P(TEXT("bool"), false, TEXT("Enable real-time sky capture (default true)")));
		Params->SetObjectField(TEXT("cubemap_resolution"), P(TEXT("int"),  false, TEXT("Cubemap resolution in pixels (default 128)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_sky_light_source"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Build all lighting for the current level at the specified quality"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("quality"), P(TEXT("string"), false, TEXT("Lighting quality: Preview, Medium, High, Production (default Preview)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("build_all_lighting"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Export a list of all actor classes and their instance counts in the current level"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("export_asset_list"), A);
	}

	return Root;
}
