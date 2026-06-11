#include "Capture/ForgeDataTableCapture.h"
#include "ForgeBridgeVersion.h"
#include "IO/ForgeContextWriter.h"

#include "Engine/DataTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "JsonObjectConverter.h"

#include "Editor.h"                         // FEditorDelegates
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgeDataTableCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;

    // Create output subdirectories up front
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("datatables")));
    PF.CreateDirectoryTree(*(OutputDir / TEXT("datatables/commands")));
    PF.CreateDirectoryTree(*(OutputDir / TEXT("datatables/commands/results")));
    PF.CreateDirectoryTree(*(OutputDir / TEXT("datatables/commands/archive")));

    // Suspend polling during PIE to avoid editor asset churn
    BeginPIEHandle = FEditorDelegates::BeginPIE.AddUObject(
        this, &UForgeDataTableCapture::OnBeginPIE);
    EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(
        this, &UForgeDataTableCapture::OnEndPIE);

    // 1-second tick for command polling
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UForgeDataTableCapture::OnTick), 1.0f);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeDataTable: Initialized — polling datatables/commands/import_rows.json every 1s"));
}

void UForgeDataTableCapture::Deinitialize()
{
    if (TickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

    FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
}

// ============================================================
// PIE suspend / resume
// ============================================================

void UForgeDataTableCapture::OnBeginPIE(bool bIsSimulating)
{
    bPIEActive = true;
}

void UForgeDataTableCapture::OnEndPIE(bool bIsSimulating)
{
    bPIEActive = false;
}

// ============================================================
// Tick poll
// ============================================================

bool UForgeDataTableCapture::OnTick(float DeltaTime)
{
    if (!bPIEActive)
    {
        PollImportCommands();
    }
    return true; // Keep ticking
}

// ============================================================
// Export — single table
// ============================================================

bool UForgeDataTableCapture::ExportDataTable(const FString& AssetPath)
{
    UDataTable* DataTable = Cast<UDataTable>(
        StaticLoadObject(UDataTable::StaticClass(), nullptr, *AssetPath));

    if (!DataTable)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeDataTable: Could not load DataTable at '%s'"), *AssetPath);
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    if (!SerializeDataTableToJSON(DataTable, AssetPath, Root))
    {
        return false;
    }

    const FString TableName = DataTable->GetName();
    const FString Filename  = TableName + TEXT(".json");
    bool bSuccess = FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("datatables"), Filename, Root);

    if (bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("ForgeDataTable: Exported '%s' (%d rows)"),
            *TableName, DataTable->GetRowMap().Num());
    }
    return bSuccess;
}

// ============================================================
// Export — all tables
// ============================================================

int32 UForgeDataTableCapture::ExportAllDataTables()
{
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = ARModule.Get();

    // Ensure registry scan is complete before querying
    AssetRegistry.SearchAllAssets(true);

    FARFilter Filter;
    Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(FName("/Game"));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    if (Assets.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: No DataTable assets found under /Game/"));
        return 0;
    }

    int32 Exported = 0;
    for (const FAssetData& Asset : Assets)
    {
        if (ExportDataTable(Asset.GetObjectPathString()))
        {
            Exported++;
        }
    }

    if (Exported > 0)
    {
        UpdateIndexFile(Exported);
    }

    UE_LOG(LogTemp, Log,
        TEXT("ForgeDataTable: ExportAll complete — %d / %d tables exported"),
        Exported, Assets.Num());

    return Exported;
}

// ============================================================
// Serialize DataTable rows to JSON
// ============================================================

bool UForgeDataTableCapture::SerializeDataTableToJSON(UDataTable* DataTable,
    const FString& AssetPath, TSharedRef<FJsonObject> OutRoot)
{
    if (!DataTable || !DataTable->RowStruct)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: DataTable or RowStruct is null"));
        return false;
    }

    OutRoot->SetStringField(TEXT("timestamp"),  FForgeContextWriter::NowISO8601());
    OutRoot->SetStringField(TEXT("asset_path"), AssetPath);
    OutRoot->SetStringField(TEXT("table_name"), DataTable->GetName());
    OutRoot->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
    OutRoot->SetNumberField(TEXT("row_count"),  DataTable->GetRowMap().Num());

    // Build rows object — keyed by row name, value = struct fields as JSON
    TSharedPtr<FJsonObject> RowsJson = MakeShared<FJsonObject>();
    for (const TPair<FName, uint8*>& Pair : DataTable->GetRowMap())
    {
        TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
        FJsonObjectConverter::UStructToJsonObject(
            DataTable->RowStruct, Pair.Value, RowJson.ToSharedRef(), 0, 0);
        RowsJson->SetObjectField(Pair.Key.ToString(), RowJson);
    }

    OutRoot->SetObjectField(TEXT("rows"), RowsJson);
    return true;
}

// ============================================================
// Poll import commands
// ============================================================

void UForgeDataTableCapture::PollImportCommands()
{
    const FString CommandFilePath =
        OutputDir / TEXT("datatables/commands/import_rows.json");

    if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*CommandFilePath))
    {
        return;
    }

    ExecuteImportCommandFile(CommandFilePath);
}

void UForgeDataTableCapture::ExecuteImportCommandFile(const FString& CommandFilePath)
{
    // Read and parse before moving — if read fails we leave the file in place
    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *CommandFilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: Could not read command file"));
        return;
    }

    TSharedPtr<FJsonObject> CmdRoot;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
    if (!FJsonSerializer::Deserialize(Reader, CmdRoot) || !CmdRoot.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: Failed to parse import_rows.json"));
        return;
    }

    // Archive the command BEFORE executing — prevents re-processing if execution crashes
    // Colons are illegal in Windows filenames; replace them to get a safe timestamp string
    const FString Timestamp     = FForgeContextWriter::NowISO8601();
    const FString SafeTimestamp = Timestamp
        .Replace(TEXT(":"), TEXT("-"))
        .Replace(TEXT("T"), TEXT("_"))
        .Replace(TEXT("Z"), TEXT(""));
    const FString ArchivePath   =
        OutputDir / TEXT("datatables/commands/archive") /
        (TEXT("import_rows_") + SafeTimestamp + TEXT(".json"));

    IFileManager::Get().Move(*ArchivePath, *CommandFilePath);

    // Extract required fields
    FString AssetPath;
    if (!CmdRoot->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: import_rows.json missing 'asset_path'"));
        return;
    }

    const TSharedPtr<FJsonObject>* RowsField;
    if (!CmdRoot->TryGetObjectField(TEXT("rows"), RowsField))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeDataTable: import_rows.json missing 'rows'"));
        return;
    }

    UDataTable* DataTable = Cast<UDataTable>(
        StaticLoadObject(UDataTable::StaticClass(), nullptr, *AssetPath));

    if (!DataTable || !DataTable->RowStruct)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeDataTable: Could not load target DataTable '%s'"), *AssetPath);
        return;
    }

    // Delegate to the shared ImportRowsFromObject helper
    TArray<FString> AddedRows, UpdatedRows, FailedRows;
    ImportRowsFromObject(AssetPath, *RowsField, AddedRows, UpdatedRows, FailedRows);

    // Helper: FString array -> JSON value array
    auto ToJsonArray = [](const TArray<FString>& In) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(In.Num());
        for (const FString& S : In) Out.Add(MakeShared<FJsonValueString>(S));
        return Out;
    };

    // Write result file (legacy path — CommandChannel writes its own result separately)
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("timestamp"),  Timestamp);
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("status"),     FailedRows.IsEmpty() ? TEXT("ok") : TEXT("partial"));
    Result->SetNumberField(TEXT("added"),      AddedRows.Num());
    Result->SetNumberField(TEXT("updated"),    UpdatedRows.Num());
    Result->SetNumberField(TEXT("failed"),     FailedRows.Num());
    Result->SetArrayField(TEXT("added_rows"),   ToJsonArray(AddedRows));
    Result->SetArrayField(TEXT("updated_rows"), ToJsonArray(UpdatedRows));
    Result->SetArrayField(TEXT("failed_rows"),  ToJsonArray(FailedRows));

    const FString ResultFile = FPaths::GetBaseFilename(AssetPath) + TEXT("_") + SafeTimestamp + TEXT(".json");
    FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("datatables/commands/results"), ResultFile, Result);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeDataTable: Import complete — +%d added, ~%d updated, %d failed on '%s'"),
        AddedRows.Num(), UpdatedRows.Num(), FailedRows.Num(), *FPaths::GetBaseFilename(AssetPath));
}

// ============================================================
// ImportRowsFromObject — shared row import (no file I/O)
// ============================================================

bool UForgeDataTableCapture::ImportRowsFromObject(
    const FString& AssetPath,
    const TSharedPtr<FJsonObject>& RowsJson,
    TArray<FString>& OutAdded,
    TArray<FString>& OutUpdated,
    TArray<FString>& OutFailed)
{
    UDataTable* DataTable = Cast<UDataTable>(
        StaticLoadObject(UDataTable::StaticClass(), nullptr, *AssetPath));

    if (!DataTable || !DataTable->RowStruct)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeDataTable: Could not load DataTable '%s' for ImportRowsFromObject"), *AssetPath);
        OutFailed.Add(FString::Printf(TEXT("DataTable load failed: %s"), *AssetPath));
        return false;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& RowPair : RowsJson->Values)
    {
        const FString& RowNameStr = RowPair.Key;
        const FName    RowFName   = FName(*RowNameStr);

        const TSharedPtr<FJsonObject> RowJsonObj = RowPair.Value->AsObject();
        if (!RowJsonObj.IsValid())
        {
            OutFailed.Add(RowNameStr + TEXT(": value is not a JSON object"));
            continue;
        }

        const bool bRowExists = DataTable->FindRowUnchecked(RowFName) != nullptr;

        const int32 StructSize = DataTable->RowStruct->GetStructureSize();
        uint8* RowData = static_cast<uint8*>(FMemory::Malloc(StructSize));
        DataTable->RowStruct->InitializeStruct(RowData);

        const bool bConverted = FJsonObjectConverter::JsonObjectToUStruct(
            RowJsonObj.ToSharedRef(), DataTable->RowStruct, RowData, 0, 0);

        if (bConverted)
        {
            DataTable->AddRow(RowFName, *reinterpret_cast<FTableRowBase*>(RowData));
            DataTable->MarkPackageDirty();
            (bRowExists ? OutUpdated : OutAdded).Add(RowNameStr);
        }
        else
        {
            OutFailed.Add(RowNameStr + TEXT(": JSON->struct conversion failed"));
        }

        DataTable->RowStruct->DestroyStruct(RowData);
        FMemory::Free(RowData);
    }

    const bool bAnySuccess = (OutAdded.Num() + OutUpdated.Num()) > 0;
    UE_LOG(LogTemp, Log,
        TEXT("ForgeDataTable: ImportRowsFromObject — +%d added, ~%d updated, %d failed on '%s'"),
        OutAdded.Num(), OutUpdated.Num(), OutFailed.Num(), *FPaths::GetBaseFilename(AssetPath));

    return bAnySuccess;
}

// ============================================================
// Index — READ-MERGE-WRITE preserving all other capture sections
// ============================================================

void UForgeDataTableCapture::UpdateIndexFile(int32 TableCount)
{
    const FString IndexPath = OutputDir / TEXT("index.json");
    const FString Timestamp = FForgeContextWriter::NowISO8601();

    TSharedPtr<FJsonObject> Root;
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *IndexPath))
    {
        TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExistingContent);
        FJsonSerializer::Deserialize(R, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
        Captures = *ExistingCaptures;
    else
        Captures = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> DTSection = MakeShared<FJsonObject>();
    DTSection->SetStringField(TEXT("output_dir"),   TEXT("datatables/"));
    DTSection->SetStringField(TEXT("commands_dir"), TEXT("datatables/commands/"));
    DTSection->SetNumberField(TEXT("table_count"),  TableCount);
    DTSection->SetStringField(TEXT("last_updated"), Timestamp);
    Captures->SetObjectField(TEXT("datatables"), DTSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), FORGE_BRIDGE_VERSION);

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
