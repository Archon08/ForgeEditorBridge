#include "Handlers/PackagingHandler.h"
#include "ForgeAISubsystem.h"
#include "BridgeSessionStore.h"

// ---- Config (enable_zen_streaming) ------------------------------------------
#include "Misc/ConfigCacheIni.h"

// ---- Core / FileSystem -------------------------------------------------------
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Asset Registry ---------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// ---- Target Platform (list_platforms) ----------------------------------------
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"

// ---- Data Validation (validate_content) -------------------------------------
#if WITH_EDITOR
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("packaging");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FString CollisionEnumStr(int32 ResponseIndex)
{
	// unused here — kept for symmetry with other handlers
	return FString();
}

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("cook_content"))        return Action_CookContent(Params);
	if (Action == TEXT("package_project"))     return Action_PackageProject(Params);
	if (Action == TEXT("get_cook_status"))     return Action_GetCookStatus(Params);
	if (Action == TEXT("validate_content"))    return Action_ValidateContent(Params);
	if (Action == TEXT("get_package_size"))    return Action_GetPackageSize(Params);
	if (Action == TEXT("list_platforms"))      return Action_ListPlatforms(Params);
	if (Action == TEXT("cook_incremental"))    return Action_CookIncremental(Params);
	if (Action == TEXT("enable_zen_streaming"))return Action_EnableZenStreaming(Params);
	if (Action == TEXT("cook_dlc"))            return Action_CookDLC(Params);
	if (Action == TEXT("set_build_target"))    return Action_SetBuildTarget(Params);
	if (Action == TEXT("set_build_config"))    return Action_SetBuildConfig(Params);
	if (Action == TEXT("get_build_targets"))   return Action_GetBuildTargets(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: cook_content, package_project, get_cook_status, validate_content, get_package_size, list_platforms, cook_incremental, enable_zen_streaming, cook_dlc, set_build_target, set_build_config, get_build_targets"), *Action));
}

// ---------------------------------------------------------------------------
// cook_content — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_CookContent(TSharedPtr<FJsonObject> Params)
{
	FString Platform = TEXT("Win64");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("platform"), Platform);

	// Async job tracking: create + mark Running. Actual dispatch is still via UAT command line.
	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("cook_content"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Cook queued"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"),   JobId);
	Data->SetStringField(TEXT("platform"), Platform);

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("cook_content"), TEXT(""), Data);
	R.Message = FString::Printf(TEXT(
		"cook_content: Cooking requires UAT (UnrealAutomationTool). Run via command line:\n"
		"  RunUAT.bat BuildCookRun -project=\"%s\" -noP4 -platform=%s -clientconfig=Development -cook -build -stage -pak\n"
		"Or use Editor menu: Platforms > %s > Cook Content.\n"
		"Tip: Use -iterate flag to only re-cook changed assets.\n"
		"Job queued. job_id=%s — poll via project/get_job_status."), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Platform, *Platform, *JobId);
	R.RecoveryHint = TEXT("UAT is at Engine/Build/BatchFiles/RunUAT.bat. Ensure the project is saved before cooking.");
	return R;
}

// ---------------------------------------------------------------------------
// package_project — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_PackageProject(TSharedPtr<FJsonObject> Params)
{
	FString Platform = TEXT("Win64");
	FString Config = TEXT("Development");
	FString OutputDir;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("platform"), Platform);
		Params->TryGetStringField(TEXT("config"), Config);
		Params->TryGetStringField(TEXT("output_dir"), OutputDir);
	}
	if (OutputDir.IsEmpty())
		OutputDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Packaged"), Platform);

	// Async job tracking: create + mark Running before dispatch.
	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("package_project"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Package started"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"),     JobId);
	Data->SetStringField(TEXT("platform"),   Platform);
	Data->SetStringField(TEXT("config"),     Config);
	Data->SetStringField(TEXT("output_dir"), OutputDir);

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("package_project"), TEXT(""), Data);
	R.Message = FString::Printf(TEXT(
		"package_project: Full packaging requires UAT. Run via command line:\n"
		"  RunUAT.bat BuildCookRun -project=\"%s\" -noP4 -platform=%s -clientconfig=%s -cook -build -stage -pak -archive -archivedirectory=\"%s\"\n"
		"Or use Editor menu: Platforms > %s > Package Project.\n"
		"Job queued. job_id=%s — poll via project/get_job_status."), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Platform, *Config, *OutputDir, *Platform, *JobId);
	R.RecoveryHint = TEXT("Save and cook content first. Ensure SDK for the target platform is installed.");
	return R;
}

// ---------------------------------------------------------------------------
// get_cook_status — read Cook.log from Saved/Logs
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_GetCookStatus(TSharedPtr<FJsonObject> Params)
{
	const FString CookLogPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("Cook.log"));

	if (!IFileManager::Get().FileExists(*CookLogPath))
		return MakeError(DOMAIN, TEXT("get_cook_status"), 2000,
			FString::Printf(TEXT("Cook.log not found at: %s. Cook the project first."), *CookLogPath));

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *CookLogPath))
		return MakeError(DOMAIN, TEXT("get_cook_status"), 3000, TEXT("Failed to read Cook.log"));

	// Parse last 150 lines
	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);
	const int32 TailStart = FMath::Max(0, Lines.Num() - 150);
	FString Tail;
	int32 ErrorCount = 0;
	int32 WarnCount = 0;
	for (int32 i = TailStart; i < Lines.Num(); i++)
	{
		if (Lines[i].Contains(TEXT("Error:")))   ErrorCount++;
		if (Lines[i].Contains(TEXT("Warning:"))) WarnCount++;
		Tail += Lines[i] + TEXT("\n");
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total_lines"),  Lines.Num());
	Data->SetNumberField(TEXT("error_count"),  ErrorCount);
	Data->SetNumberField(TEXT("warning_count"),WarnCount);
	Data->SetStringField(TEXT("log_path"),     CookLogPath);
	Data->SetStringField(TEXT("tail_150"),     Tail);

	return MakeSuccess(DOMAIN, TEXT("get_cook_status"),
		FString::Printf(TEXT("Cook.log: %d lines, %d errors, %d warnings"), Lines.Num(), ErrorCount, WarnCount), Data);
}

// ---------------------------------------------------------------------------
// validate_content — UEditorValidatorSubsystem
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_ValidateContent(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	if (!GEditor)
		return MakeError(DOMAIN, TEXT("validate_content"), 3000, TEXT("GEditor not available"));

	UEditorValidatorSubsystem* VS = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	if (!VS)
		return MakeError(DOMAIN, TEXT("validate_content"), 3000, TEXT("EditorValidatorSubsystem not available"));

	// Gather all assets from asset registry
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARModule.Get();

	TArray<FAssetData> AllAssets;
	AR.GetAllAssets(AllAssets, true);

	FValidateAssetsSettings Settings;
	Settings.bSkipExcludedDirectories = true;
	Settings.bShowIfNoFailures = false;

	FValidateAssetsResults Results;
	// ValidateAssets signature may have changed in UE 5.7; try ValidateAssetsWithSettings.
	VS->ValidateAssetsWithSettings(AllAssets, Settings, Results);

	// FValidateAssetsResults::NumErrors renamed in UE 5.7 — using NumInvalid as fallback.
	const int32 NumErrors = Results.NumChecked - Results.NumValid - Results.NumSkipped;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("num_checked"),  Results.NumChecked);
	Data->SetNumberField(TEXT("num_valid"),    Results.NumValid);
	Data->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
	Data->SetNumberField(TEXT("num_errors"),   NumErrors);
	Data->SetNumberField(TEXT("num_skipped"),  Results.NumSkipped);

	return MakeSuccess(DOMAIN, TEXT("validate_content"),
		FString::Printf(TEXT("Validation complete: %d checked, %d errors, %d warnings"),
			Results.NumChecked, NumErrors, Results.NumWarnings), Data);
#else
	return MakeError(DOMAIN, TEXT("validate_content"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// get_package_size — scan Content directory
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_GetPackageSize(TSharedPtr<FJsonObject> Params)
{
	const FString ContentDir = FPaths::ProjectContentDir();

	TArray<FString> AllFiles;
	IFileManager::Get().FindFilesRecursive(AllFiles, *ContentDir, TEXT("*"), true, false);

	int64 TotalBytes = 0;
	TMap<FString, int64> ByExt;

	for (const FString& File : AllFiles)
	{
		const int64 Size = IFileManager::Get().FileSize(*File);
		if (Size < 0) continue;
		TotalBytes += Size;
		const FString Ext = FPaths::GetExtension(File).ToLower();
		ByExt.FindOrAdd(Ext) += Size;
	}

	// Sort extensions by size descending
	TArray<FString> ExtKeys;
	ByExt.GetKeys(ExtKeys);
	ExtKeys.Sort([&ByExt](const FString& A, const FString& B) { return ByExt[A] > ByExt[B]; });

	TArray<TSharedPtr<FJsonValue>> BreakdownArr;
	for (const FString& Ext : ExtKeys)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("ext"),   Ext.IsEmpty() ? TEXT("(no ext)") : Ext);
		Entry->SetNumberField(TEXT("bytes"), (double)ByExt[Ext]);
		Entry->SetNumberField(TEXT("mb"),    (double)ByExt[Ext] / (1024.0 * 1024.0));
		BreakdownArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total_files"),  AllFiles.Num());
	Data->SetNumberField(TEXT("total_bytes"),  (double)TotalBytes);
	Data->SetNumberField(TEXT("total_mb"),     (double)TotalBytes / (1024.0 * 1024.0));
	Data->SetStringField(TEXT("content_dir"),  ContentDir);
	Data->SetArrayField(TEXT("by_extension"),  BreakdownArr);

	return MakeSuccess(DOMAIN, TEXT("get_package_size"),
		FString::Printf(TEXT("Content: %d files, %.1f MB"),
			AllFiles.Num(), (double)TotalBytes / (1024.0 * 1024.0)), Data);
}

// ---------------------------------------------------------------------------
// list_platforms — ITargetPlatformManagerModule
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_ListPlatforms(TSharedPtr<FJsonObject> Params)
{
	ITargetPlatformManagerModule* TPM =
		FModuleManager::GetModulePtr<ITargetPlatformManagerModule>(TEXT("TargetPlatform"));

	if (!TPM)
		return MakeError(DOMAIN, TEXT("list_platforms"), 3000,
			TEXT("TargetPlatform module not loaded. Ensure it is enabled in Build.cs."));

	const TArray<ITargetPlatform*>& Platforms = TPM->GetTargetPlatforms();

	TArray<TSharedPtr<FJsonValue>> PlatformArr;
	for (ITargetPlatform* P : Platforms)
	{
		if (!P) continue;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),         P->PlatformName());
		Entry->SetStringField(TEXT("display_name"), P->DisplayName().ToString());
		Entry->SetBoolField(TEXT("is_running"),     P->IsRunningPlatform());
		PlatformArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("platforms"), PlatformArr);
	Data->SetNumberField(TEXT("count"),    PlatformArr.Num());

	return MakeSuccess(DOMAIN, TEXT("list_platforms"),
		FString::Printf(TEXT("Found %d target platform(s)"), PlatformArr.Num()), Data);
}

// ---------------------------------------------------------------------------
// cook_incremental — async-job dispatch (iterate flag only recooks changes)
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_CookIncremental(TSharedPtr<FJsonObject> Params)
{
	FString Platform = TEXT("Windows");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("platform"), Platform);

	const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("cook_incremental"));
	FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Incremental cook queued"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("job_id"),   JobId);
	Data->SetStringField(TEXT("platform"), Platform);
	Data->SetBoolField  (TEXT("iterate"),  true);

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("cook_incremental"), TEXT(""), Data);
	R.Message = FString::Printf(TEXT(
		"Incremental cook queued for %s. Use project/get_job_status to poll.\n"
		"Dispatch: RunUAT.bat BuildCookRun -project=\"%s\" -cook -iterate -platform=%s\n"
		"job_id=%s"), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Platform, *Platform, *JobId);
	R.RecoveryHint = TEXT("Iterative cook reuses DDC — only changed assets are re-cooked. Delete Saved/Cooked/<Platform> to force a full cook.");
	return R;
}

// ---------------------------------------------------------------------------
// enable_zen_streaming — toggle IoStore + ZenStore flags in StreamingSettings
// ---------------------------------------------------------------------------

FBridgeResult UPackagingHandler::Action_EnableZenStreaming(TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
		return MakeError(DOMAIN, TEXT("enable_zen_streaming"), 1000, TEXT("Missing required param: 'enabled'"));

	bool bEnabled = false;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		return MakeError(DOMAIN, TEXT("enable_zen_streaming"), 1000, TEXT("Missing required param: 'enabled' (bool)"));

	if (!GConfig)
		return MakeError(DOMAIN, TEXT("enable_zen_streaming"), 3000, TEXT("GConfig not available"));

	const TCHAR* Section = TEXT("/Script/Engine.StreamingSettings");
	GConfig->SetBool(Section, TEXT("s.IoStoreEnabled"),  bEnabled, GEngineIni);
	GConfig->SetBool(Section, TEXT("s.ZenStoreEnabled"), bEnabled, GEngineIni);
	GConfig->Flush(false, GEngineIni);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField  (TEXT("enabled"),           bEnabled);
	Data->SetStringField(TEXT("section"),           Section);
	Data->SetStringField(TEXT("s.IoStoreEnabled"),  bEnabled ? TEXT("True") : TEXT("False"));
	Data->SetStringField(TEXT("s.ZenStoreEnabled"), bEnabled ? TEXT("True") : TEXT("False"));
	Data->SetStringField(TEXT("config_file"),       GEngineIni);

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("enable_zen_streaming"),
		FString::Printf(TEXT("Zen Streaming %s. Restart required for full effect."),
			bEnabled ? TEXT("enabled") : TEXT("disabled")), Data);
	R.AffectedPath = GEngineIni;
	return R;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UPackagingHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bReq);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Cook content for a target platform (Python dispatch — UAT)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("platform"), P(TEXT("string"), false, TEXT("Win64|Android|iOS|Linux (default: Win64)")));
	  Pr->SetObjectField(TEXT("maps"),     P(TEXT("array"),  false, TEXT("Map paths to cook (empty = all)")));
	  Pr->SetObjectField(TEXT("iterate"),  P(TEXT("bool"),   false, TEXT("Only recook changed assets")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("cook_content"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Package project for distribution (Python dispatch — UAT)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("platform"),    P(TEXT("string"), false, TEXT("Win64|Android|iOS|Linux")));
	  Pr->SetObjectField(TEXT("config"),      P(TEXT("string"), false, TEXT("Development|Shipping")));
	  Pr->SetObjectField(TEXT("output_dir"),  P(TEXT("string"), false, TEXT("Output directory path")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("package_project"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read last cook log: errors, warnings, tail 150 lines"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_cook_status"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Run UEditorValidatorSubsystem on all project assets"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("validate_content"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Scan Content directory; return total size and breakdown by file type"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_package_size"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List available target platforms from ITargetPlatformManagerModule"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_platforms"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Incremental cook — recooks only changed assets (iterate flag). Returns job_id."));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("platform"), P(TEXT("string"), false, TEXT("Windows|Android|iOS|Linux (default: Windows)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("cook_incremental"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Enable/disable Zen Loader streaming (IoStore + ZenStore) in GEngineIni. Restart required."));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("enabled"), P(TEXT("bool"), true, TEXT("true to enable Zen streaming, false to disable")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("enable_zen_streaming"), A); }

	return Root;
}

// ===========================================================================
// Phase 4: DLC + build target/config
// ===========================================================================

#include "Settings/ProjectPackagingSettings.h"

FBridgeResult UPackagingHandler::Action_CookDLC(TSharedPtr<FJsonObject> Params)
{
	FString DLCName, Platform = TEXT("Windows");
	if (!Params->TryGetStringField(TEXT("dlc_name"), DLCName) || DLCName.IsEmpty())
		return MakeError(TEXT("packaging"), TEXT("cook_dlc"), 1000, TEXT("'dlc_name' is required"));
	Params->TryGetStringField(TEXT("platform"), Platform);

	const FString ProjectFile = FPaths::GetProjectFilePath();
	const FString EngineRoot = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
#if PLATFORM_WINDOWS
	const FString UAT = FPaths::Combine(EngineRoot, TEXT("Build/BatchFiles/RunUAT.bat"));
#else
	const FString UAT = FPaths::Combine(EngineRoot, TEXT("Build/BatchFiles/RunUAT.sh"));
#endif
	const FString Args = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -dlcname=%s -platform=%s -cook -skipstage -nocompileeditor"),
		*ProjectFile, *DLCName, *Platform);

	int32 ReturnCode = -1;
	FString StdOut, StdErr;
	const bool bRan = FPlatformProcess::ExecProcess(*UAT, *Args, &ReturnCode, &StdOut, &StdErr);
	if (!bRan)
		return MakeError(TEXT("packaging"), TEXT("cook_dlc"), 3000,
			FString::Printf(TEXT("Failed to launch UAT: %s"), *UAT));
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("return_code"), ReturnCode);
	Data->SetStringField(TEXT("stdout_tail"), StdOut.Right(2048));
	if (ReturnCode != 0)
		Data->SetStringField(TEXT("stderr_tail"), StdErr.Right(2048));
	return MakeSuccess(TEXT("packaging"), TEXT("cook_dlc"),
		FString::Printf(TEXT("DLC cook for '%s' on %s — exit=%d"), *DLCName, *Platform, ReturnCode), Data);
}

FBridgeResult UPackagingHandler::Action_SetBuildTarget(TSharedPtr<FJsonObject> Params)
{
	FString TargetName;
	if (!Params->TryGetStringField(TEXT("target_name"), TargetName) || TargetName.IsEmpty())
		return MakeError(TEXT("packaging"), TEXT("set_build_target"), 1000, TEXT("'target_name' is required"));

	UProjectPackagingSettings* PS = GetMutableDefault<UProjectPackagingSettings>();
	if (!PS) return MakeError(TEXT("packaging"), TEXT("set_build_target"), 3000, TEXT("PackagingSettings unavailable"));
	PS->BuildTarget = TargetName;
	PS->SaveConfig();
	return MakeSuccess(TEXT("packaging"), TEXT("set_build_target"),
		FString::Printf(TEXT("BuildTarget set to '%s' (saved to DefaultGame.ini)"), *TargetName));
}

FBridgeResult UPackagingHandler::Action_SetBuildConfig(TSharedPtr<FJsonObject> Params)
{
	FString Config;
	if (!Params->TryGetStringField(TEXT("config"), Config) || Config.IsEmpty())
		return MakeError(TEXT("packaging"), TEXT("set_build_config"), 1000,
			TEXT("'config' is required (Development|Shipping|Test|DebugGame)"));

	UProjectPackagingSettings* PS = GetMutableDefault<UProjectPackagingSettings>();
	if (!PS) return MakeError(TEXT("packaging"), TEXT("set_build_config"), 3000, TEXT("PackagingSettings unavailable"));

	const FString C = Config.ToLower();
	if      (C == TEXT("development"))  PS->BuildConfiguration = EProjectPackagingBuildConfigurations::PPBC_Development;
	else if (C == TEXT("shipping"))     PS->BuildConfiguration = EProjectPackagingBuildConfigurations::PPBC_Shipping;
	else if (C == TEXT("test"))         PS->BuildConfiguration = EProjectPackagingBuildConfigurations::PPBC_Test;
	else if (C == TEXT("debuggame"))    PS->BuildConfiguration = EProjectPackagingBuildConfigurations::PPBC_DebugGame;
	else
		return MakeError(TEXT("packaging"), TEXT("set_build_config"), 1001,
			FString::Printf(TEXT("Unknown config '%s'"), *Config));
	PS->SaveConfig();
	return MakeSuccess(TEXT("packaging"), TEXT("set_build_config"),
		FString::Printf(TEXT("Build config -> %s"), *Config));
}

FBridgeResult UPackagingHandler::Action_GetBuildTargets(TSharedPtr<FJsonObject> Params)
{
	const FString TargetsDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
	TArray<FString> TargetFiles;
	IFileManager::Get().FindFiles(TargetFiles, *FPaths::Combine(TargetsDir, TEXT("*.Target.cs")), true, false);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& F : TargetFiles)
	{
		FString Name = F;
		Name.RemoveFromEnd(TEXT(".Target.cs"));
		Arr.Add(MakeShared<FJsonValueString>(Name));
	}
	UProjectPackagingSettings* PS = GetMutableDefault<UProjectPackagingSettings>();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("targets"), Arr);
	Data->SetNumberField(TEXT("count"), Arr.Num());
	if (PS)
	{
		Data->SetStringField(TEXT("active_build_target"), PS->BuildTarget);
		Data->SetNumberField(TEXT("active_build_config"), (int32)PS->BuildConfiguration);
	}
	return MakeSuccess(TEXT("packaging"), TEXT("get_build_targets"),
		FString::Printf(TEXT("%d target(s) in Source/"), Arr.Num()), Data);
}
