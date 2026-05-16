#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ProjectHandler.generated.h"

/**
 * ProjectHandler — domain "project"  (v0.4.0)
 *
 * Exposes project-level metadata and configuration over the bridge:
 * engine version, plugin management, default map settings.
 *
 * Actions:
 *   get_project_info — (no params)                → project name, engine version, plugin count, etc.
 *   list_plugins     — enabled_only? (bool)       → array of plugin name/enabled/category/version
 *   enable_plugin    — plugin_name, enabled (bool) → modifies .uproject plugin references
 *   get_default_map  — (no params)                → editor startup map + game default map
 *   set_default_map  — map_path, target           → writes to GConfig and flushes
 *   read_perf_capture    — (no params)            → reads perf/latest.json from capture output
 *   read_network_capture — (no params)            → reads network/audit.json from capture output
 */
UCLASS()
class FORGEEDITORBRIDGE_API UProjectHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("project"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("get_project_info"), TEXT("list_plugins"), TEXT("enable_plugin"), TEXT("set_plugin_enabled"), TEXT("get_default_map"), TEXT("set_default_map"), TEXT("read_perf_capture"), TEXT("read_network_capture"), TEXT("set_editor_preference"), TEXT("get_editor_preference"), TEXT("get_job_status"), TEXT("cancel_job"), TEXT("list_jobs"), TEXT("package") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetProjectInfo(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListPlugins(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_EnablePlugin(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetDefaultMap(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetDefaultMap(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetEditorPreference(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetEditorPreference(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetJobStatus(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CancelJob(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListJobs(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Package(TSharedPtr<FJsonObject> Params);
};
