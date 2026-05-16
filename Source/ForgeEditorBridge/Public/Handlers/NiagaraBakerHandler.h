#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "NiagaraBakerHandler.generated.h"

/**
 * NiagaraBakerHandler — domain "niagara_baker"  (UE 5.7)
 *
 * Configures UNiagaraBakerSettings on a Niagara System asset. The actual bake
 * dispatch is editor-tool side (UI-driven) — programmatic bake invocation
 * is not exposed in 5.7's public API. This handler covers config:
 * frames-per-dim, output textures, simulation duration.
 *
 * Actions:
 *   get_baker_info       → asset_path → returns current baker settings
 *   set_baker_settings   → asset_path, frames_per_dim, sim_duration?, frames_per_second?
 *   set_output_texture   → asset_path, output_index, texture_path (UTextureRenderTarget2D)
 *   list_baker_outputs   → asset_path → arrays
 */
UCLASS()
class FORGEEDITORBRIDGE_API UNiagaraBakerHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("niagara_baker"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("get_baker_info"), TEXT("set_baker_settings"),
            TEXT("set_output_texture"), TEXT("list_baker_outputs")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_GetBakerInfo      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetBakerSettings  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetOutputTexture  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListBakerOutputs  (TSharedPtr<FJsonObject> Params);
};
