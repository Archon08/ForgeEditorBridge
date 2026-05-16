#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Misc/OutputDevice.h"
#include "HAL/CriticalSection.h"
#include "ForgeRuntimeCapture.generated.h"

// ---------------------------------------------------------------------------
// FForgeRuntimeLogInterceptor
// Hooks into GLog during PIE to filter and persist log lines.
// Serialize() can be called from non-game-threads - FileMutex guards file I/O.
// ---------------------------------------------------------------------------
class FForgeRuntimeLogInterceptor : public FOutputDevice
{
public:
    explicit FForgeRuntimeLogInterceptor(const FString& InOutputDir);
    virtual ~FForgeRuntimeLogInterceptor();

    // Called on BeginPIE: clears the log file and registers with GLog
    void StartCapturing();
    // Called on EndPIE: writes session-end marker and removes from GLog
    void StopCapturing();

    // FOutputDevice interface
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                           const FName& Category) override;
    virtual bool CanBeUsedOnAnyThread() const override { return true; }

    // Categories that pass through the filter (written during PIE)
    TArray<FName> FilterCategories;

    // Max lines written per session - guards against log floods
    static constexpr int32 MaxLinesPerSession = 10000;

private:
    FString OutputDir;
    bool bCapturing = false;
    int32 LineCount = 0;
    FCriticalSection FileMutex;

    static FString VerbosityToString(ELogVerbosity::Type Verbosity);
};

// ---------------------------------------------------------------------------
// UForgeRuntimeCapture
// Owned by UForgeAISubsystem. Manages PIE lifecycle and variable snapshots.
// ---------------------------------------------------------------------------
UCLASS()
class FORGEEDITORBRIDGE_API UForgeRuntimeCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

    // Snapshot actor state for all actors tagged "ForgeDebug" in the PIE world.
    // Writes runtime/variables.json. Safe to call via Python during PIE.
    UFUNCTION(BlueprintCallable, Category = "Forge")
    void CaptureVariableSnapshot();

    // Dynamically add/remove filter categories without restarting PIE
    void AddFilterCategory(FName Category);
    void RemoveFilterCategory(FName Category);

private:
    FString OutputDir;
    TUniquePtr<FForgeRuntimeLogInterceptor> LogInterceptor;

    FDelegateHandle BeginPIEHandle;
    FDelegateHandle EndPIEHandle;

    void OnBeginPIE(const bool bIsSimulating);
    void OnEndPIE(const bool bIsSimulating);

    TSharedPtr<FJsonObject> BuildActorSnapshot(AActor* Actor);
    UWorld* GetPIEWorld() const;
    void UpdateIndexFile();
};
