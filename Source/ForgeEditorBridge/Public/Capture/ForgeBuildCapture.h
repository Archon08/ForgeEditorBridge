#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Misc/OutputDevice.h"
#include "ForgeBuildCapture.generated.h"

// Forward declarations
class UBlueprint;

/**
 * GLog interceptor - filters and accumulates build/compile messages.
 * Thread-safe: Serialize() can be called from any thread.
 */
class FForgeLogInterceptor : public FOutputDevice
{
public:
    explicit FForgeLogInterceptor(const FString& InOutputDir);
    virtual ~FForgeLogInterceptor() override;

    void StartCapturing();
    void StopCapturing();
    void FlushToDisk(const FString& Trigger);

    int32 GetErrorCount() const;
    int32 GetWarningCount() const;

    // FOutputDevice interface
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                           const FName& Category) override;
    virtual bool CanBeUsedOnAnyThread() const override { return true; }

private:
    FString OutputDir;
    bool bCapturing = false;

    mutable FCriticalSection DataLock;
    TArray<TSharedPtr<FJsonObject>> PendingErrors;
    TArray<TSharedPtr<FJsonObject>> PendingWarnings;

    static bool IsBuildCategory(const FName& Category);
    static void ParseCompilerLine(const FString& Raw,
                                  FString& OutFile, int32& OutLine, FString& OutText);
};


/**
 * Owns the log interceptor and all build-event delegate bindings.
 * Lives as a UPROPERTY on UForgeAISubsystem - GC managed.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeBuildCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);
    void Deinitialize();

private:
    FString OutputDir;
    TUniquePtr<FForgeLogInterceptor> LogInterceptor;

    // IHotReloadInterface::OnHotReload() - void(bool)
    FDelegateHandle HotReloadDelegateHandle;

    // ILiveCodingModule::GetOnPatchCompleteDelegate() - void() - UE 5.7 verified name
    FDelegateHandle LiveCodingDelegateHandle;

    // GEditor->OnBlueprintCompiled() - void()
    FDelegateHandle BlueprintCompiledDelegateHandle;

    void OnHotReloadFinished(bool bWasTriggeredAutomatically);
    void OnLiveCodingPatchComplete();   // void() - FOnPatchCompleteDelegate has no params
    void OnAnyBlueprintCompiled();

    void WriteIndexFile();
};
