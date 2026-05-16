#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "DataTableHandler.generated.h"

/**
 * DataTableHandler — domain "data_table"  (v0.7.0 / UE 5.7)
 *
 * Creates and authors UDataTable assets programmatically through the Bridge.
 *
 * Actions:
 *   create_data_table  → asset_path (string), row_struct_path (string)
 *                        Creates a new UDataTable asset backed by the specified UScriptStruct.
 *
 *   add_row            → asset_path (string), row_name (string),
 *                        row_data (string, optional JSON object matching row struct fields)
 *                        Appends a new empty row, then applies row_data if provided.
 *
 *   edit_row           → asset_path (string), row_name (string),
 *                        row_data (string, JSON object with fields to overwrite)
 *                        Overwrites fields on an existing row from a JSON object.
 *
 *   get_row            → asset_path (string), row_name (string)
 *                        Returns the row as a JSON string in ExtraData.
 *
 *   read_datatable_capture → asset_path (string, optional)
 *                            If given: exports that single table; returns datatables/{TableName}.json.
 *                            If omitted: exports all tables; returns list of files in datatables/.
 *
 *   trigger_import_poll → no params; calls DataTableCapture->PollImportCommands()
 *
 *   rename_row          → asset_path (string), old_name (string), new_name (string)
 *                         Copies the old row to a new name and removes the old row.
 *
 *   import_csv          → asset_path (string), csv_string (string)
 *                         Replaces table contents from CSV data; returns any import problems.
 *
 *   export_csv          → asset_path (string)
 *                         Returns the table as a CSV string in ExtraData.
 *
 *   export_json         → asset_path (string)
 *                         Returns the table as a pretty JSON string in ExtraData.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UDataTableHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("data_table"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_data_table"), TEXT("add_row"), TEXT("edit_row"), TEXT("update_row"), TEXT("get_row"), TEXT("list_rows"), TEXT("delete_row"), TEXT("remove_row"), TEXT("read_datatable_capture"), TEXT("trigger_import_poll"), TEXT("rename_row"), TEXT("import_csv"), TEXT("export_csv"), TEXT("export_json") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateDataTable(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddRow         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_EditRow        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetRow         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListRows       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DeleteRow             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadDataTableCapture  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_TriggerImportPoll     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RenameRow             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportCsv             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExportCsv             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExportJson            (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a UDataTable from a content path.
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UDataTable* LoadDataTable(const FString& AssetPath, FBridgeResult& Result);

	/**
	 * Apply a JSON object string to a raw struct pointer using the table's RowStruct.
	 * Returns false and sets OutError on parse/apply failure.
	 */
	bool ApplyJsonToRow(UDataTable* DataTable, uint8* RowPtr,
	                    const FString& JsonStr, FString& OutError);
};
