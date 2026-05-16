#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "LocalizationHandler.generated.h"

/**
 * LocalizationHandler — domain "localization" (v2.0.0 / UE 5.7)
 *
 * Manage project text localization, cultures, and PO workflows.
 *
 * Actions:
 *   list_cultures              → (no params) — returns configured cultures + discovered culture dirs.
 *
 *   gather_text                → target (string, opt) — Python dispatch: GatherText commandlet.
 *
 *   export_po                  → culture (string), target (string, opt) — Python dispatch: ExportDialogue commandlet.
 *
 *   import_po                  → culture (string), po_path (string) — Python dispatch: ImportDialogue commandlet.
 *
 *   add_culture                → culture (string) — Python dispatch: add via localization editor or commandlet.
 *
 *   find_missing_translations  → culture (string, opt) — Python dispatch: InternationalizationExport commandlet.
 *
 *   create_localization_target → name (string), native_culture (string) — Python dispatch: editor wizard.
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULocalizationHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("localization"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("list_cultures"),
			TEXT("gather_text"),
			TEXT("export_po"),
			TEXT("import_po"),
			TEXT("add_culture"),
			TEXT("find_missing_translations"),
			TEXT("create_localization_target"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_ListCultures             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GatherText               (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExportPO                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportPO                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddCulture               (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_FindMissingTranslations   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateLocalizationTarget  (TSharedPtr<FJsonObject> Params);
};
