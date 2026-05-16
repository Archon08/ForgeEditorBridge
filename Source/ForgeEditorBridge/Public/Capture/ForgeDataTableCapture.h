#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Containers/Ticker.h"
#include "ForgeDataTableCapture.generated.h"

class UDataTable;

UCLASS()
class FORGEEDITORBRIDGE_API UForgeDataTableCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

    // Export a single DataTable by asset path (e.g. "/Game/Data/DT_Items")
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportDataTable(const FString& AssetPath);

    // Export all UDataTable assets found under /Game/
    // Returns the number of tables successfully exported
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAllDataTables();

    // Manually trigger a single poll cycle for import commands
    UFUNCTION(BlueprintCallable, Category = "Forge")
    void PollImportCommands();

    /**
     * Import rows from a pre-parsed JSON rows object into the named DataTable.
     * Called by UForgeCommandChannel for the "import_datatable_rows" command.
     * OutAdded / OutUpdated / OutFailed receive the row names by outcome.
     * Returns true if at least one row was successfully added or updated.
     */
    bool ImportRowsFromObject(const FString& AssetPath,
                              const TSharedPtr<FJsonObject>& RowsJson,
                              TArray<FString>& OutAdded,
                              TArray<FString>& OutUpdated,
                              TArray<FString>& OutFailed);

private:
    FString OutputDir;
    FTSTicker::FDelegateHandle TickHandle;

    bool bPIEActive = false;
    FDelegateHandle BeginPIEHandle;
    FDelegateHandle EndPIEHandle;

    // Ticker callback - fires every 1 second, returns true to keep ticking
    bool OnTick(float DeltaTime);

    void OnBeginPIE(bool bIsSimulating);
    void OnEndPIE(bool bIsSimulating);

    // Populate OutRoot with timestamp, asset_path, row_struct, row_count, and rows object
    bool SerializeDataTableToJSON(UDataTable* DataTable, const FString& AssetPath,
                                  TSharedRef<FJsonObject> OutRoot);

    // Move command file to archive/, process each row, write results/
    void ExecuteImportCommandFile(const FString& CommandFilePath);

    // READ-MERGE-WRITE index.json preserving all other capture sections
    void UpdateIndexFile(int32 TableCount);
};
