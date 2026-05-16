#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "DDCHandler.generated.h"

/**
 * DDCHandler — domain "ddc"  (UE 5.7)
 *
 * Wraps DerivedDataCache console commands. The DDC C++ API surface is
 * limited at runtime; this handler routes through the documented console
 * commands (DerivedDataCache.*).
 *
 * Actions:
 *   get_ddc_stats   → returns cached DDC stats from `DerivedDataCache.Stats` console command
 *   refresh_shaders → executes `recompileshaders changed`
 *   recompile_global_shaders → executes `recompileshaders global`
 *   trigger_resolve_dirty   → executes `dpcvar Console.Variables.Save`
 */
UCLASS()
class FORGEEDITORBRIDGE_API UDDCHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("ddc"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("get_ddc_stats"), TEXT("refresh_shaders"),
            TEXT("recompile_global_shaders"), TEXT("trigger_resolve_dirty")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_GetStats          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RefreshShaders    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RecompileGlobal   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_TriggerResolveDirty(TSharedPtr<FJsonObject> Params);
};
