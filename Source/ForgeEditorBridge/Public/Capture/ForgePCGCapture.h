#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgePCGCapture.generated.h"

class UPCGComponent;

UCLASS()
class FORGEEDITORBRIDGE_API UForgePCGCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

    // Points beyond this count are dropped; metadata (total count, histogram) is always written.
    static constexpr int32 MaxExportPoints = 5000;

private:
    FString OutputDir;
    FDelegateHandle MapOpenedHandle;
    FDelegateHandle PostLoadMapHandle;
    FDelegateHandle ActorSpawnedHandle;
    TWeakObjectPtr<UWorld> SpawnListenerWorld;

    // Per-component delegate handle - required for non-dynamic multicast removal.
    TMap<TWeakObjectPtr<UPCGComponent>, FDelegateHandle> BoundComponents;

    // No UFUNCTION needed - OnPCGGraphGeneratedDelegate is a standard (non-dynamic) multicast.
    void OnPCGGraphGenerated(UPCGComponent* PCGComponent);
    void OnMapOpened(const FString& Filename, bool bAsTemplate);
    void OnPostLoadMapWithWorld(UWorld* World);
    void OnActorSpawnedInWorld(AActor* Actor);

    void BindToAllPCGComponents();
    void UnbindAllPCGComponents();

    TSharedPtr<FJsonObject> BuildPointCloudJSON(UPCGComponent* PCGComponent);
    TSharedPtr<FJsonObject> BuildDensityHistogram(const TArray<float>& Densities);
    void UpdateIndexFile();
};
