#include "Handlers/DataTableHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"

// ---- Capture ---------------------------------------------------------------
#include "Capture/ForgeDataTableCapture.h"

// ---- DataTable -------------------------------------------------------------
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "DataTableEditorUtils.h"           // FDataTableEditorUtils

// ---- JSON ↔ Struct ---------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"            // FJsonObjectConverter

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UDataTableHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("data_table"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_data_table")) return Action_CreateDataTable(Params);
	if (Action == TEXT("add_row"))           return Action_AddRow(Params);
	if (Action == TEXT("edit_row"))          return Action_EditRow(Params);
	if (Action == TEXT("update_row"))        return Action_EditRow(Params);   // spec name alias
	if (Action == TEXT("get_row"))           return Action_GetRow(Params);
	if (Action == TEXT("list_rows"))               return Action_ListRows(Params);
	if (Action == TEXT("delete_row"))              return Action_DeleteRow(Params);
	if (Action == TEXT("remove_row"))              return Action_DeleteRow(Params);  // spec name alias
	if (Action == TEXT("read_datatable_capture"))  return Action_ReadDataTableCapture(Params);
	if (Action == TEXT("trigger_import_poll"))     return Action_TriggerImportPoll(Params);
	if (Action == TEXT("rename_row"))              return Action_RenameRow(Params);
	if (Action == TEXT("import_csv"))              return Action_ImportCsv(Params);
	if (Action == TEXT("export_csv"))              return Action_ExportCsv(Params);
	if (Action == TEXT("export_json"))             return Action_ExportJson(Params);

	FBridgeResult R = CreateResult(TEXT("data_table"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown data_table action '%s'. Valid: create_data_table, add_row, edit_row, get_row, list_rows, delete_row, read_datatable_capture, trigger_import_poll, rename_row, import_csv, export_csv, export_json"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_data_table
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_CreateDataTable(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("data_table"), TEXT("create_data_table"));

	FString AssetPath, RowStructPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),     AssetPath)     || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("row_struct_path"), RowStructPath) || RowStructPath.IsEmpty())
	{
		Result.Message = TEXT("create_data_table: 'asset_path' and 'row_struct_path' are required "
		                      "(e.g. '/Game/Data/DT_Items', '/Game/Data/S_ItemRow.S_ItemRow')");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Resolve row struct — try bare path first, then with asset name suffix
	UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
	if (!RowStruct)
	{
		const FString Suffix = RowStructPath + TEXT(".") +
		                       FPackageName::GetLongPackageAssetName(RowStructPath);
		RowStruct = LoadObject<UScriptStruct>(nullptr, *Suffix);
	}
	if (!RowStruct)
	{
		Result.Message = FString::Printf(
			TEXT("create_data_table: UScriptStruct not found at '%s'"), *RowStructPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UDataTableFactory* Factory = NewObject<UDataTableFactory>();
	Factory->Struct = RowStruct;

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UDataTable::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_data_table: failed to create asset at '%s' (path may already exist or be invalid)"),
			*AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UDataTable* NewTable = CastChecked<UDataTable>(CreatedAsset);
	NewTable->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("DataTable created at %s (RowStruct=%s)"), *AssetPath, *RowStruct->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// add_row
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_AddRow(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("data_table"), TEXT("add_row"));

	FString AssetPath, RowName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("row_name"),   RowName)   || RowName.IsEmpty())
	{
		Result.Message = TEXT("add_row: 'asset_path' and 'row_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UDataTable* DataTable = LoadDataTable(AssetPath, Result);
	if (!DataTable) return Result;

	const FName RowFName(*RowName);
	if (DataTable->FindRowUnchecked(RowFName))
	{
		Result.Message = FString::Printf(
			TEXT("add_row: row '%s' already exists in '%s'"), *RowName, *AssetPath);
		Result.ErrorCode = 2002;
		return Result;
	}

	FDataTableEditorUtils::AddRow(DataTable, RowFName);

	// Optionally populate from row_data JSON
	FString RowDataStr;
	if (Params->TryGetStringField(TEXT("row_data"), RowDataStr) && !RowDataStr.IsEmpty())
	{
		uint8* RowPtr = DataTable->FindRowUnchecked(RowFName);
		if (RowPtr)
		{
			FString ApplyError;
			if (!ApplyJsonToRow(DataTable, RowPtr, RowDataStr, ApplyError))
			{
				// Non-fatal: row was created, just warn about the data
				Result.bSuccess     = true;
				Result.AffectedPath = AssetPath;
				Result.Message      = FString::Printf(
					TEXT("add_row: row '%s' created but row_data apply failed: %s"), *RowName, *ApplyError);
				DataTable->MarkPackageDirty();
				return Result;
			}
		}
	}

	DataTable->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Row '%s' added to DataTable '%s'"), *RowName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// edit_row
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_EditRow(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("data_table"), TEXT("edit_row"));

	FString AssetPath, RowName, RowDataStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("row_name"),   RowName)    || RowName.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("row_data"),   RowDataStr) || RowDataStr.IsEmpty())
	{
		Result.Message = TEXT("edit_row: 'asset_path', 'row_name', and 'row_data' (JSON) are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UDataTable* DataTable = LoadDataTable(AssetPath, Result);
	if (!DataTable) return Result;

	const FName RowFName(*RowName);
	uint8* RowPtr = DataTable->FindRowUnchecked(RowFName);
	if (!RowPtr)
	{
		Result.Message = FString::Printf(
			TEXT("edit_row: row '%s' not found in '%s'"), *RowName, *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	FString ApplyError;
	if (!ApplyJsonToRow(DataTable, RowPtr, RowDataStr, ApplyError))
	{
		Result.Message = FString::Printf(TEXT("edit_row: %s"), *ApplyError);
		Result.ErrorCode = 1000;
		return Result;
	}

	DataTable->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Row '%s' updated in DataTable '%s'"), *RowName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_row
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_GetRow(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("data_table"), TEXT("get_row"));

	FString AssetPath, RowName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("row_name"),   RowName)   || RowName.IsEmpty())
	{
		Result.Message = TEXT("get_row: 'asset_path' and 'row_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UDataTable* DataTable = LoadDataTable(AssetPath, Result);
	if (!DataTable) return Result;

	const FName RowFName(*RowName);
	uint8* RowPtr = DataTable->FindRowUnchecked(RowFName);
	if (!RowPtr)
	{
		Result.Message = FString::Printf(
			TEXT("get_row: row '%s' not found in '%s'"), *RowName, *AssetPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	if (!DataTable->RowStruct)
	{
		Result.Message = FString::Printf(TEXT("get_row: DataTable '%s' has no RowStruct"), *AssetPath);
		Result.ErrorCode = 2003;
		return Result;
	}

	FString RowJson;
	if (!FJsonObjectConverter::UStructToJsonObjectString(DataTable->RowStruct, RowPtr, RowJson, 0, 0))
	{
		Result.Message = FString::Printf(
			TEXT("get_row: failed to serialize row '%s' to JSON"), *RowName);
		Result.ErrorCode = 3000;
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.ExtraData    = RowJson;
	Result.Message      = FString::Printf(TEXT("Row '%s' serialized from '%s'"), *RowName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// list_rows
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_ListRows(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("list_rows"),
			1000, TEXT("list_rows: 'asset_path' is required"),
			TEXT("Provide the content path of a UDataTable asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("list_rows"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("list_rows"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	TArray<FName> RowNames = DataTable->GetRowNames();

	TArray<TSharedPtr<FJsonValue>> JsonNames;
	for (const FName& Name : RowNames)
	{
		JsonNames.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("rows"), JsonNames);
	Data->SetNumberField(TEXT("count"), RowNames.Num());

	return MakeSuccess(TEXT("data_table"), TEXT("list_rows"),
		FString::Printf(TEXT("Found %d row(s) in '%s'"), RowNames.Num(), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// delete_row
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_DeleteRow(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, RowName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("row_name"),   RowName)   || RowName.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("delete_row"),
			1000, TEXT("delete_row: 'asset_path' and 'row_name' are required"),
			TEXT("Provide asset_path and the row_name to delete"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("delete_row"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("delete_row"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	const FName RowFName(*RowName);
	if (!DataTable->FindRowUnchecked(RowFName))
	{
		return MakeError(TEXT("data_table"), TEXT("delete_row"),
			2000, FString::Printf(TEXT("Row '%s' not found in '%s'"), *RowName, *AssetPath),
			TEXT("Use list_rows to see available row names"));
	}

	DataTable->RemoveRow(RowFName);
	DataTable->MarkPackageDirty();

	return MakeSuccess(TEXT("data_table"), TEXT("delete_row"),
		FString::Printf(TEXT("Row '%s' deleted from '%s'"), *RowName, *AssetPath));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

UDataTable* UDataTableHandler::LoadDataTable(const FString& AssetPath, FBridgeResult& Result)
{
	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *AssetPath);
	if (!DataTable)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		DataTable = LoadObject<UDataTable>(nullptr, *Suffix);
	}
	if (!DataTable)
	{
		Result.Message = FString::Printf(
			TEXT("LoadDataTable: no UDataTable found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
	}
	return DataTable;
}

bool UDataTableHandler::ApplyJsonToRow(UDataTable* DataTable, uint8* RowPtr,
                                        const FString& JsonStr, FString& OutError)
{
	if (!DataTable->RowStruct)
	{
		OutError = TEXT("DataTable has no RowStruct");
		return false;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse row_data JSON: %s"), *JsonStr.Left(80));
		return false;
	}

	// JsonObjectToUStruct writes matching fields into the struct; mismatched fields are ignored.
	if (!FJsonObjectConverter::JsonObjectToUStruct(JsonObj.ToSharedRef(),
	                                               DataTable->RowStruct, RowPtr, 0, 0))
	{
		OutError = TEXT("FJsonObjectConverter::JsonObjectToUStruct returned false");
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// read_datatable_capture
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_ReadDataTableCapture(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);

	if (Subsystem->DataTableCapture)
	{
		if (!AssetPath.IsEmpty())
			Subsystem->DataTableCapture->ExportDataTable(AssetPath);
		else
			Subsystem->DataTableCapture->ExportAllDataTables();
	}

	FBridgeResult Res = MakeSuccess(GetDomainName(), TEXT("read_datatable_capture"), TEXT("Capture complete"));

	if (!AssetPath.IsEmpty())
	{
		// Single table: return the named file
		const FString TableName = FPackageName::GetLongPackageAssetName(AssetPath);
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory,
			FString::Printf(TEXT("datatables/%s.json"), *TableName));
		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		else
		{
			Res.Message = FString::Printf(
				TEXT("Capture complete — file not yet available for reading at: %s"), *FilePath);
		}
	}
	else
	{
		// All tables: enumerate files in the datatables/ directory
		FString DirPath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("datatables"));
		TArray<FString> FoundFiles;
		IFileManager::Get().FindFiles(FoundFiles, *(DirPath / TEXT("*.json")), true, false);

		TArray<TSharedPtr<FJsonValue>> FileArray;
		for (const FString& FileName : FoundFiles)
			FileArray.Add(MakeShared<FJsonValueString>(FileName));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("files"), FileArray);
		Data->SetStringField(TEXT("directory"), DirPath);
		Data->SetNumberField(TEXT("count"), FileArray.Num());
		Res.Data = Data;
		Res.Message = FString::Printf(TEXT("Capture complete — found %d file(s) in datatables/"), FileArray.Num());
	}
	return Res;
}

// ---------------------------------------------------------------------------
// trigger_import_poll
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_TriggerImportPoll(TSharedPtr<FJsonObject> Params)
{
	if (Subsystem->DataTableCapture)
		Subsystem->DataTableCapture->PollImportCommands();

	return MakeSuccess(GetDomainName(), TEXT("trigger_import_poll"), TEXT("Import poll triggered"));
}

// ---------------------------------------------------------------------------
// rename_row
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_RenameRow(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, OldName, NewName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("old_name"),   OldName)   || OldName.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("new_name"),   NewName)   || NewName.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("rename_row"),
			1000, TEXT("rename_row: 'asset_path', 'old_name', and 'new_name' are required"),
			TEXT("Provide all three parameters"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("rename_row"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("rename_row"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	const FName OldFName(*OldName);
	const FName NewFName(*NewName);

	uint8* OldRowPtr = DataTable->FindRowUnchecked(OldFName);
	if (!OldRowPtr)
	{
		return MakeError(TEXT("data_table"), TEXT("rename_row"),
			2000, FString::Printf(TEXT("rename_row: source row '%s' not found in '%s'"), *OldName, *AssetPath),
			TEXT("Use list_rows to see available row names"));
	}

	if (DataTable->FindRowUnchecked(NewFName))
	{
		return MakeError(TEXT("data_table"), TEXT("rename_row"),
			3000, FString::Printf(TEXT("rename_row: destination row '%s' already exists in '%s'"), *NewName, *AssetPath),
			TEXT("Remove the destination row first or choose a different new_name"));
	}

	if (!DataTable->RowStruct)
	{
		return MakeError(TEXT("data_table"), TEXT("rename_row"),
			3000, TEXT("rename_row: DataTable has no RowStruct"),
			TEXT("Ensure the DataTable was created with a valid struct"));
	}

	// Allocate a temporary buffer, copy the row data, remove old, insert under new name.
	const int32 StructSize = DataTable->RowStruct->GetStructureSize();
	void* TempBuf = FMemory::Malloc(StructSize, DataTable->RowStruct->GetMinAlignment());
	DataTable->RowStruct->InitializeStruct(TempBuf);
	DataTable->RowStruct->CopyScriptStruct(TempBuf, OldRowPtr);

	DataTable->RemoveRow(OldFName);
	DataTable->AddRow(NewFName, *static_cast<FTableRowBase*>(TempBuf));

	DataTable->RowStruct->DestroyStruct(TempBuf);
	FMemory::Free(TempBuf);

	DataTable->MarkPackageDirty();

	return MakeSuccess(TEXT("data_table"), TEXT("rename_row"),
		FString::Printf(TEXT("Row '%s' renamed to '%s' in '%s'"), *OldName, *NewName, *AssetPath));
}

// ---------------------------------------------------------------------------
// import_csv
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_ImportCsv(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, CsvString;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("csv_string"),  CsvString)  || CsvString.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("import_csv"),
			1000, TEXT("import_csv: 'asset_path' and 'csv_string' are required"),
			TEXT("Provide the DataTable asset path and the CSV data as a string"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("import_csv"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("import_csv"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	// UDataTable::CreateTableFromCSVString replaces the entire table content.
	// It returns an array of problem strings (empty on full success).
	// NOTE: In UE 5.7 this API is confirmed on UDataTable.
	// If a linker error occurs, the fallback is DataTableEditorUtils or FDataTableImporterCSV
	// (include "DataTableCSV.h" from the DataTableEditor module — Editor-only).
	TArray<FString> Problems = DataTable->CreateTableFromCSVString(CsvString);

	DataTable->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> ProblemValues;
	for (const FString& P : Problems)
	{
		ProblemValues.Add(MakeShared<FJsonValueString>(P));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("problems"), ProblemValues);
	Data->SetNumberField(TEXT("problem_count"), Problems.Num());

	const FString Msg = Problems.IsEmpty()
		? FString::Printf(TEXT("CSV imported successfully into '%s'"), *AssetPath)
		: FString::Printf(TEXT("CSV imported into '%s' with %d problem(s)"), *AssetPath, Problems.Num());

	return MakeSuccess(TEXT("data_table"), TEXT("import_csv"), Msg, Data);
}

// ---------------------------------------------------------------------------
// export_csv
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_ExportCsv(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("export_csv"),
			1000, TEXT("export_csv: 'asset_path' is required"),
			TEXT("Provide the content path of a UDataTable asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("export_csv"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("export_csv"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	FBridgeResult Result = MakeSuccess(TEXT("data_table"), TEXT("export_csv"),
		FString::Printf(TEXT("CSV exported from '%s'"), *AssetPath));
	Result.AffectedPath = AssetPath;
	Result.ExtraData    = DataTable->GetTableAsCSV();
	return Result;
}

// ---------------------------------------------------------------------------
// export_json
// ---------------------------------------------------------------------------

FBridgeResult UDataTableHandler::Action_ExportJson(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("data_table"), TEXT("export_json"),
			1000, TEXT("export_json: 'asset_path' is required"),
			TEXT("Provide the content path of a UDataTable asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("data_table"), TEXT("export_json"));
	UDataTable* DataTable = LoadDataTable(AssetPath, TempResult);
	if (!DataTable)
	{
		return MakeError(TEXT("data_table"), TEXT("export_json"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UDataTable"));
	}

	FBridgeResult Result = MakeSuccess(TEXT("data_table"), TEXT("export_json"),
		FString::Printf(TEXT("JSON exported from '%s'"), *AssetPath));
	Result.AffectedPath = AssetPath;
	// UE 5.7: EDataTableExportFlags::UsePrettyPropertyNames was removed — fall back to
	// the default flags. Field names will use internal identifiers.
	Result.ExtraData    = DataTable->GetTableAsJSON(EDataTableExportFlags::None);
	return Result;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UDataTableHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new UDataTable asset backed by a specified row struct"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new DataTable"))); Pr->SetObjectField(TEXT("row_struct_path"), P(TEXT("string"), true, TEXT("Path to the UScriptStruct"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_data_table"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a new row to a DataTable, optionally populating with JSON data"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("row_name"), P(TEXT("string"), true, TEXT("Unique row name"))); Pr->SetObjectField(TEXT("row_data"), P(TEXT("string"), false, TEXT("JSON string matching row struct fields"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_row"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Overwrite fields on an existing row from a JSON object"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("row_name"), P(TEXT("string"), true, TEXT("Row to edit"))); Pr->SetObjectField(TEXT("row_data"), P(TEXT("string"), true, TEXT("JSON string with fields to overwrite"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("edit_row"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Serialize a DataTable row to JSON"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("row_name"), P(TEXT("string"), true, TEXT("Row to retrieve"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_row"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all row names in a DataTable"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_rows"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Delete a row from a DataTable"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("row_name"), P(TEXT("string"), true, TEXT("Row to delete"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("delete_row"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Rename an existing row (copy data, remove old, insert under new name)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("old_name"), P(TEXT("string"), true, TEXT("Current row name"))); Pr->SetObjectField(TEXT("new_name"), P(TEXT("string"), true, TEXT("New row name — must not already exist"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("rename_row"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Replace the DataTable contents by importing a CSV string; returns any import problems"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); Pr->SetObjectField(TEXT("csv_string"), P(TEXT("string"), true, TEXT("Full CSV text (header row + data rows)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("import_csv"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Export the DataTable as a CSV string returned in ExtraData"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("export_csv"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Export the DataTable as a pretty JSON string returned in ExtraData"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("DataTable asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("export_json"), A); }

    return Root;
}
