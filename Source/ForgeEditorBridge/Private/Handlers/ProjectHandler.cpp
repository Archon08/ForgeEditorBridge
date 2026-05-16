#include "Handlers/ProjectHandler.h"
#include "ForgeAISubsystem.h"
#include "Capture/ForgePerformanceCapture.h"
#include "Capture/ForgeNetworkCapture.h"
#include "Misc/FileHelper.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ProjectDescriptor.h"
#include "GeneralProjectSettings.h"
#include "BridgeSessionStore.h"

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(TEXT("project"), Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("get_project_info"))   return Action_GetProjectInfo(Params);
	if (Action == TEXT("list_plugins"))       return Action_ListPlugins(Params);
	if (Action == TEXT("enable_plugin"))      return Action_EnablePlugin(Params);
	if (Action == TEXT("set_plugin_enabled")) return Action_EnablePlugin(Params);  // spec name alias
	if (Action == TEXT("package"))            return Action_Package(Params);
	if (Action == TEXT("get_default_map"))  return Action_GetDefaultMap(Params);
	if (Action == TEXT("set_default_map"))  return Action_SetDefaultMap(Params);
	if (Action == TEXT("set_editor_preference")) return Action_SetEditorPreference(Params);
	if (Action == TEXT("get_editor_preference")) return Action_GetEditorPreference(Params);
	if (Action == TEXT("get_job_status"))   return Action_GetJobStatus(Params);
	if (Action == TEXT("cancel_job"))       return Action_CancelJob(Params);
	if (Action == TEXT("list_jobs"))        return Action_ListJobs(Params);

	if (Action == TEXT("read_perf_capture"))
	{
		if (Subsystem && Subsystem->PerformanceCapture)
			Subsystem->PerformanceCapture->ExportPerformanceSnapshot();
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("perf/latest.json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(TEXT("project"), Action, TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}

	if (Action == TEXT("read_network_capture"))
	{
		if (Subsystem && Subsystem->NetworkCapture)
			Subsystem->NetworkCapture->ExportNetworkAudit();
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("network/audit.json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(TEXT("project"), Action, TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}

	return MakeError(TEXT("project"), Action, 1000,
		FString::Printf(TEXT("Unknown project action '%s'. Valid: get_project_info, list_plugins, enable_plugin, get_default_map, set_default_map, read_perf_capture, read_network_capture, set_editor_preference, get_editor_preference, get_job_status, cancel_job, list_jobs"), *Action));
}

// ---------------------------------------------------------------------------
// get_project_info
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_GetProjectInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("get_project_info");

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Project name
	Data->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	// Engine version
	Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

	// Project file path
	Data->SetStringField(TEXT("project_file"), FPaths::GetProjectFilePath());

	// Plugin count
	TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetDiscoveredPlugins();
	Data->SetNumberField(TEXT("plugin_count"), AllPlugins.Num());

	// Default maps from config
	FString EditorStartupMap;
	FString GameDefaultMap;
	if (GConfig)
	{
		GConfig->GetString(
			TEXT("/Script/EngineSettings.GameMapsSettings"),
			TEXT("EditorStartupMap"),
			EditorStartupMap,
			GEngineIni);

		GConfig->GetString(
			TEXT("/Script/EngineSettings.GameMapsSettings"),
			TEXT("GameDefaultMap"),
			GameDefaultMap,
			GEngineIni);
	}
	Data->SetStringField(TEXT("editor_startup_map"), EditorStartupMap);
	Data->SetStringField(TEXT("game_default_map"), GameDefaultMap);

	return MakeSuccess(Domain, Action, TEXT("Project info retrieved"), Data);
}

// ---------------------------------------------------------------------------
// list_plugins
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_ListPlugins(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("list_plugins");

	bool bEnabledOnly = false;
	Params->TryGetBoolField(TEXT("enabled_only"), bEnabledOnly);

	TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetDiscoveredPlugins();
	TArray<TSharedPtr<FJsonValue>> PluginArray;

	for (const TSharedRef<IPlugin>& Plugin : AllPlugins)
	{
		const bool bEnabled = Plugin->IsEnabled();
		if (bEnabledOnly && !bEnabled)
		{
			continue;
		}

		TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
		PluginObj->SetStringField(TEXT("name"), Plugin->GetName());
		PluginObj->SetBoolField(TEXT("enabled"), bEnabled);
		PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
		PluginObj->SetStringField(TEXT("version"), Plugin->GetDescriptor().VersionName);

		PluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("plugins"), PluginArray);
	Data->SetNumberField(TEXT("count"), PluginArray.Num());

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Found %d plugin(s)"), PluginArray.Num()), Data);
}

// ---------------------------------------------------------------------------
// enable_plugin
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_EnablePlugin(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("enable_plugin");

	FString PluginName;
	if (!Params->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'plugin_name' is required"));
	}

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	// Load the .uproject descriptor
	FString ProjectFilePath = FPaths::GetProjectFilePath();
	FProjectDescriptor ProjectDesc;
	FText LoadError;
	if (!ProjectDesc.Load(ProjectFilePath, LoadError))
	{
		return MakeError(Domain, Action, 3000,
			FString::Printf(TEXT("Failed to load project descriptor: %s"), *LoadError.ToString()));
	}

	// Find or add the plugin reference
	bool bFound = false;
	for (FPluginReferenceDescriptor& PluginRef : ProjectDesc.Plugins)
	{
		if (PluginRef.Name.Equals(PluginName, ESearchCase::IgnoreCase))
		{
			PluginRef.bEnabled = bEnabled;
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		FPluginReferenceDescriptor NewRef;
		NewRef.Name = PluginName;
		NewRef.bEnabled = bEnabled;
		ProjectDesc.Plugins.Add(MoveTemp(NewRef));
	}

	// Save back
	if (!ProjectDesc.Save(ProjectFilePath, LoadError))
	{
		return MakeError(Domain, Action, 3000,
			FString::Printf(TEXT("Failed to save project descriptor: %s"), *LoadError.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("plugin_name"), PluginName);
	Data->SetBoolField(TEXT("enabled"), bEnabled);
	Data->SetBoolField(TEXT("was_existing_entry"), bFound);
	Data->SetStringField(TEXT("warning"), TEXT("Editor restart may be required for changes to take effect"));

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Plugin '%s' set to %s. Restart may be required."),
			*PluginName, bEnabled ? TEXT("enabled") : TEXT("disabled")),
		Data);
}

// ---------------------------------------------------------------------------
// get_default_map
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_GetDefaultMap(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("get_default_map");

	FString EditorStartupMap;
	FString GameDefaultMap;

	if (GConfig)
	{
		GConfig->GetString(
			TEXT("/Script/EngineSettings.GameMapsSettings"),
			TEXT("EditorStartupMap"),
			EditorStartupMap,
			GEngineIni);

		GConfig->GetString(
			TEXT("/Script/EngineSettings.GameMapsSettings"),
			TEXT("GameDefaultMap"),
			GameDefaultMap,
			GEngineIni);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("editor_startup_map"), EditorStartupMap);
	Data->SetStringField(TEXT("game_default_map"), GameDefaultMap);

	return MakeSuccess(Domain, Action, TEXT("Default map settings retrieved"), Data);
}

// ---------------------------------------------------------------------------
// set_default_map
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_SetDefaultMap(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("set_default_map");

	FString MapPath;
	if (!Params->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'map_path' is required"),
			TEXT("Provide an asset path, e.g. /Game/Maps/MyLevel"));
	}

	FString Target;
	if (!Params->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'target' is required (\"editor\" or \"game\")"));
	}

	if (!GConfig)
	{
		return MakeError(Domain, Action, 3000, TEXT("GConfig is not available"));
	}

	const FString Section = TEXT("/Script/EngineSettings.GameMapsSettings");
	bool bWrote = false;

	if (Target.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
	{
		GConfig->SetString(*Section, TEXT("EditorStartupMap"), *MapPath, GEngineIni);
		bWrote = true;
	}
	else if (Target.Equals(TEXT("game"), ESearchCase::IgnoreCase))
	{
		GConfig->SetString(*Section, TEXT("GameDefaultMap"), *MapPath, GEngineIni);
		bWrote = true;
	}
	else
	{
		return MakeError(Domain, Action, 1000,
			FString::Printf(TEXT("Invalid target '%s'. Must be 'editor' or 'game'"), *Target));
	}

	if (bWrote)
	{
		GConfig->Flush(false, GEngineIni);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("map_path"), MapPath);
	Data->SetStringField(TEXT("target"), Target);

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Set %s default map to '%s'"), *Target, *MapPath), Data);
}

// ---------------------------------------------------------------------------
// set_editor_preference
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_SetEditorPreference(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("set_editor_preference");

	FString Section;
	if (!Params->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'section' is required"));
	}

	FString Key;
	if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'key' is required"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeError(Domain, Action, 1000, TEXT("'value' is required"));
	}

	FString ConfigFile = TEXT("EditorPerProjectUserSettings");
	Params->TryGetStringField(TEXT("config_file"), ConfigFile);

	if (!GConfig)
	{
		return MakeError(Domain, Action, 3000, TEXT("GConfig is not available"));
	}

	GConfig->SetString(*Section, *Key, *Value, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("section"), Section);
	Data->SetStringField(TEXT("key"), Key);
	Data->SetStringField(TEXT("value"), Value);
	Data->SetStringField(TEXT("config_file"), ConfigFile);

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Set %s/%s"), *Section, *Key), Data);
}

// ---------------------------------------------------------------------------
// get_editor_preference
// ---------------------------------------------------------------------------

FBridgeResult UProjectHandler::Action_GetEditorPreference(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("get_editor_preference");

	FString Section;
	if (!Params->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'section' is required"));
	}

	FString Key;
	if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'key' is required"));
	}

	FString ConfigFile = TEXT("EditorPerProjectUserSettings");
	Params->TryGetStringField(TEXT("config_file"), ConfigFile);

	if (!GConfig)
	{
		return MakeError(Domain, Action, 3000, TEXT("GConfig is not available"));
	}

	FString OutValue;
	const bool bFound = GConfig->GetString(*Section, *Key, OutValue, GEditorPerProjectIni);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("section"), Section);
	Data->SetStringField(TEXT("key"), Key);
	Data->SetStringField(TEXT("value"), OutValue);
	Data->SetStringField(TEXT("config_file"), ConfigFile);
	Data->SetBoolField(TEXT("found"), bFound);

	return MakeSuccess(Domain, Action,
		bFound
			? FString::Printf(TEXT("Read %s/%s"), *Section, *Key)
			: FString::Printf(TEXT("Key %s/%s not found"), *Section, *Key),
		Data);
}

// ---------------------------------------------------------------------------
// job helpers + get_job_status / cancel_job / list_jobs
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> SerializeBridgeJob(const FBridgeJob& Job, bool bIncludeLogTail)
{
	TSharedPtr<FJsonObject> JobObj = MakeShared<FJsonObject>();
	JobObj->SetStringField(TEXT("job_id"), Job.JobId);
	JobObj->SetStringField(TEXT("action_name"), Job.ActionName);
	JobObj->SetStringField(TEXT("status"), BridgeJobStatusToString(Job.Status));
	JobObj->SetNumberField(TEXT("progress_percent"), Job.ProgressPercent);
	JobObj->SetStringField(TEXT("created_at"), Job.CreatedAt.ToString());

	if (bIncludeLogTail)
	{
		TArray<TSharedPtr<FJsonValue>> LogArr;
		for (const FString& Line : Job.LogTail)
		{
			LogArr.Add(MakeShared<FJsonValueString>(Line));
		}
		JobObj->SetArrayField(TEXT("log_tail"), LogArr);
	}

	return JobObj;
}

FBridgeResult UProjectHandler::Action_GetJobStatus(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("get_job_status");

	FString JobId;
	if (!Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'job_id' is required"));
	}

	TOptional<FBridgeJob> MaybeJob = FBridgeSessionStore::Get().GetJob(JobId);
	if (!MaybeJob.IsSet())
	{
		return MakeError(Domain, Action, 2000,
			FString::Printf(TEXT("Job not found: %s"), *JobId));
	}

	TSharedPtr<FJsonObject> Data = SerializeBridgeJob(MaybeJob.GetValue(), /*bIncludeLogTail=*/true);

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Job %s status: %s"),
			*JobId,
			*BridgeJobStatusToString(MaybeJob.GetValue().Status)),
		Data);
}

FBridgeResult UProjectHandler::Action_CancelJob(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("cancel_job");

	FString JobId;
	if (!Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'job_id' is required"));
	}

	const bool bCancelled = FBridgeSessionStore::Get().CancelJob(JobId);
	if (!bCancelled)
	{
		return MakeError(Domain, Action, 3000,
			TEXT("cancel_job: job not found or already terminal"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"), JobId);
	Data->SetBoolField(TEXT("cancelled"), true);

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Job %s cancelled"), *JobId), Data);
}

FBridgeResult UProjectHandler::Action_ListJobs(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("list_jobs");

	TArray<FBridgeJob> Jobs = FBridgeSessionStore::Get().ListJobs();
	TArray<TSharedPtr<FJsonValue>> JobArray;
	for (const FBridgeJob& Job : Jobs)
	{
		JobArray.Add(MakeShared<FJsonValueObject>(SerializeBridgeJob(Job, /*bIncludeLogTail=*/false)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("jobs"), JobArray);
	Data->SetNumberField(TEXT("count"), JobArray.Num());

	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Found %d job(s)"), JobArray.Num()), Data);
}

// ---------------------------------------------------------------------------
// package  (project/package — async, wires to FBridgeSessionStore)
// Params: platform (string, optional, default Win64), config (string, optional, default Development),
//         output_dir (string, optional)
// ---------------------------------------------------------------------------
FBridgeResult UProjectHandler::Action_Package(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("project");
	const FString Action = TEXT("package");

	FString Platform = TEXT("Win64");
	FString Config   = TEXT("Development");
	FString OutputDir;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("platform"),   Platform);
		Params->TryGetStringField(TEXT("config"),     Config);
		Params->TryGetStringField(TEXT("output_dir"), OutputDir);
	}
	if (OutputDir.IsEmpty())
		OutputDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Packaged"), Platform);

	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("project/package"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Package queued"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"),     JobId);
	Data->SetStringField(TEXT("platform"),   Platform);
	Data->SetStringField(TEXT("config"),     Config);
	Data->SetStringField(TEXT("output_dir"), OutputDir);

	FBridgeResult R = MakeSuccess(Domain, Action, TEXT(""), Data);
	R.Message = FString::Printf(TEXT(
		"project/package queued (job_id=%s). Full packaging requires UAT:\n"
		"  RunUAT.bat BuildCookRun -project=\"%s\" -noP4 "
		"-platform=%s -clientconfig=%s -cook -build -stage -pak "
		"-archive -archivedirectory=\"%s\"\n"
		"Poll status via project/get_job_status with job_id."), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*JobId, *Platform, *Config, *OutputDir);
	R.RecoveryHint = TEXT("Save and cook content first. Ensure SDK for the target platform is installed.");
	return R;
}

TSharedPtr<FJsonObject> UProjectHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get project metadata (name, engine version, plugins, maps)")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("get_project_info"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all discovered plugins")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("enabled_only"), P(TEXT("bool"), false, TEXT("Only list enabled plugins (default false)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("list_plugins"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Enable or disable a plugin in the .uproject")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("plugin_name"), P(TEXT("string"), true, TEXT("Plugin name"))); Ps->SetObjectField(TEXT("enabled"), P(TEXT("bool"), false, TEXT("Enable or disable (default true)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("enable_plugin"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get current default map settings from config")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("get_default_map"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the default editor or game map")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("map_path"), P(TEXT("string"), true, TEXT("Content path of the map"))); Ps->SetObjectField(TEXT("target"), P(TEXT("string"), true, TEXT("Target: editor or game"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_default_map"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a named editor preference in GConfig (EditorPerProjectUserSettings)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("section"), P(TEXT("string"), true, TEXT("Config section"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("Config key"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value to write"))); Ps->SetObjectField(TEXT("config_file"), P(TEXT("string"), false, TEXT("Config file (default EditorPerProjectUserSettings)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_editor_preference"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read a named editor preference from GConfig")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("section"), P(TEXT("string"), true, TEXT("Config section"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("Config key"))); Ps->SetObjectField(TEXT("config_file"), P(TEXT("string"), false, TEXT("Config file (default EditorPerProjectUserSettings)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_editor_preference"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Query an async job's status from FBridgeSessionStore")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("job_id"), P(TEXT("string"), true, TEXT("Job identifier"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_job_status"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Cancel a pending or running job")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("job_id"), P(TEXT("string"), true, TEXT("Job identifier"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("cancel_job"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all non-expired jobs")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("list_jobs"), A); }

	return Root;
}
