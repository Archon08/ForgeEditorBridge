#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "NDisplayHandler.generated.h"

/**
 * NDisplayHandler — domain "ndisplay"  (UE 5.7)
 *
 * Thin coverage of the nDisplay multi-projector plugin. nDisplay is highly
 * project-specific; this handler exposes presence + the documented runtime
 * console hooks so AI can detect availability and trigger high-level ops.
 *
 * Actions:
 *   is_ndisplay_loaded   → returns whether DisplayCluster module is mounted
 *   get_runtime_state    → returns role / cluster ID / config path if running
 *   exec_cluster_event   → event_name (Cluster.<name>) — runtime cluster event
 */
UCLASS()
class FORGEEDITORBRIDGE_API UNDisplayHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("ndisplay"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return { TEXT("is_ndisplay_loaded"), TEXT("get_runtime_state"), TEXT("exec_cluster_event") };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_IsLoaded         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetRuntimeState  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ExecClusterEvent (TSharedPtr<FJsonObject> Params);
};
