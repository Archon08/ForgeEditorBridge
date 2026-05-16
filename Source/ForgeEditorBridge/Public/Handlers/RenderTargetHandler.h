#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "RenderTargetHandler.generated.h"

/**
 * RenderTargetHandler — domain "rendertarget"  (UE 5.7)
 *
 * UTextureRenderTarget2D create / clear / draw_material / read_pixel.
 *
 * Actions:
 *   create_rt              → asset_path, size_x, size_y, format? ("RGBA8"|"RGBA16f"|"RGBA32f"|"R8")
 *   clear_rt               → asset_path, r? (0..1), g?, b?, a?
 *   draw_material_to_rt    → asset_path, material_path
 *   read_pixel             → asset_path, x, y → returns {r,g,b,a}
 *   read_pixels_summary    → asset_path → returns {min, max, avg} per channel (sampled)
 *   get_rt_info            → asset_path → returns {size_x, size_y, format}
 *   resize_rt              → asset_path, size_x, size_y
 *   set_clear_color        → asset_path, r, g, b, a (sets the default ClearColor)
 */
UCLASS()
class FORGEEDITORBRIDGE_API URenderTargetHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("rendertarget"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_rt"), TEXT("clear_rt"), TEXT("draw_material_to_rt"),
            TEXT("read_pixel"), TEXT("read_pixels_summary"), TEXT("get_rt_info"),
            TEXT("resize_rt"), TEXT("set_clear_color")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateRT             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearRT              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DrawMaterialToRT     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ReadPixel            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ReadPixelsSummary    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetRTInfo            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ResizeRT             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetClearColor        (TSharedPtr<FJsonObject> Params);
};
