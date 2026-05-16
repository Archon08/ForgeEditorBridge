#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeHeightmapCapture.generated.h"

class ALandscape;

UCLASS()
class FORGEEDITORBRIDGE_API UForgeHeightmapCapture : public UObject
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Forge")
    void Setup(const FString& InOutputDir);
    void Deinitialize();

    // Export heightmap for the first landscape found in the current level
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportHeightmap(int32 Resolution = 256);

    // Export heightmap for a specific landscape actor
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportHeightmapForActor(ALandscape* Landscape, int32 Resolution = 256);

private:
    FString OutputDir;

    // Read height data from landscape at uniform grid resolution
    bool SampleHeightGrid(ALandscape* Landscape, int32 Resolution,
                          TArray<TArray<float>>& OutHeights,
                          FVector2D& OutWorldMin, FVector2D& OutWorldMax,
                          float& OutMinHeightCm, float& OutMaxHeightCm);

    // Find the first ALandscape in the current editor world (safe GEngine iteration)
    ALandscape* FindFirstLandscape() const;

    // Write landscape/height-slice.json
    bool WriteHeightJSON(ALandscape* Landscape, const TArray<TArray<float>>& Heights,
                         const FVector2D& WorldMin, const FVector2D& WorldMax,
                         float MinHeightCm, float MaxHeightCm, int32 Resolution);

    // Write grayscale thumbnail landscape/height-thumb.png
    bool WriteHeightThumbnail(const TArray<TArray<float>>& Heights, int32 Resolution);

    // READ-MERGE-WRITE index.json preserving other capture sections
    void UpdateIndexFile();
};
