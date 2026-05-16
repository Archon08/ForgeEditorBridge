#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "EditorPrefsHandler.generated.h"

/**
 * EditorPrefsHandler — domain "editor_prefs" (v2.0.0 / UE 5.7)
 *
 * Read and write editor ini settings, UObject editor preferences, and viewport settings.
 *
 * Actions:
 *   get_ini_value         → section (string), key (string),
 *                           ini_file (DefaultEngine|DefaultGame|DefaultEditor|DefaultInput, opt = DefaultEngine)
 *
 *   set_ini_value         → section (string), key (string), value (string), ini_file (string, opt)
 *                           Writes to ini and flushes to disk.
 *
 *   list_ini_sections     → ini_file (string, opt) — returns all section names.
 *
 *   set_editor_preference → property_name (string), value (string),
 *                           class_name (string, opt = EditorPerProjectUserSettings)
 *                           Sets property via reflection; calls SaveConfig().
 *
 *   get_editor_preference → property_name (string), class_name (string, opt)
 *                           Reads property via reflection.
 *
 *   set_viewport_setting  → setting (realtime|show_fps|show_stats|fov), value (string)
 *                           Applies to all active editor viewports.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEditorPrefsHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("editor_prefs"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("get_ini_value"),
			TEXT("set_ini_value"),
			TEXT("list_ini_sections"),
			TEXT("set_editor_preference"),
			TEXT("get_editor_preference"),
			TEXT("set_viewport_setting"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetIniValue         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetIniValue         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListIniSections     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetEditorPreference (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetEditorPreference (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetViewportSetting  (TSharedPtr<FJsonObject> Params);
};
