#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PixelStreamingHandler.generated.h"

/**
 * PixelStreamingHandler — domain "pixelstream"  (UE 5.7)
 *
 * Thin presence + control wrapper for the Pixel Streaming plugin. Most ops
 * route through console commands `PixelStreaming.*` since the runtime
 * IPixelStreamingModule API is plugin-internal.
 *
 * Actions:
 *   is_loaded            → returns whether PixelStreaming module is mounted
 *   start_streaming      → console: `PixelStreaming.StartStreaming`
 *   stop_streaming       → console: `PixelStreaming.StopStreaming`
 *   set_target_bitrate   → mbps (float) — sets `PixelStreaming.WebRTC.MaxBitrate`
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPixelStreamingHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("pixelstream"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return { TEXT("is_loaded"), TEXT("start_streaming"), TEXT("stop_streaming"), TEXT("set_target_bitrate") };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_IsLoaded         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_StartStreaming   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_StopStreaming    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetTargetBitrate (TSharedPtr<FJsonObject> Params);
};
