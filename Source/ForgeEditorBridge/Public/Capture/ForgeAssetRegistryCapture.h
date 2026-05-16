#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeAssetRegistryCapture.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API UForgeAssetRegistryCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

    // Scan all assets under /Game/ and write assets/registry.json.
    // Returns the number of assets written, or -1 on failure.
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAssetRegistry();

    // Export only assets matching a class name filter (e.g. "Blueprint", "StaticMesh")
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAssetsByFilter(const FString& ClassName);

private:
    FString OutputDir;

    // READ-MERGE-WRITE index.json preserving all other capture sections
    void UpdateIndexFile(int32 TotalAssets);
};
