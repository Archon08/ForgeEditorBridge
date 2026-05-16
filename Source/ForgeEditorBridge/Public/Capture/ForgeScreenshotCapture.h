#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeScreenshotCapture.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API UForgeScreenshotCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

    // Request an immediate viewport screenshot. Safe to call externally (e.g. after PCG generate).
    void RequestCapture();

private:
    FString OutputDir;
    FDelegateHandle ScreenshotCapturedHandle;
    FDelegateHandle PostLoadMapHandle;

    // Handler signature matches FOnScreenshotCaptured (non-dynamic multicast).
    // DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnScreenshotCaptured, int32, int32, const TArray<FColor>&)
    void OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Colors);
    void OnPostLoadMapWithWorld(UWorld* World);

    // Read-merge-write: updates only the screenshot_latest section in index.json.
    void UpdateIndexFile(const FString& Timestamp);
};
