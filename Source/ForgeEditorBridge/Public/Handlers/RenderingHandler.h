#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "RenderingHandler.generated.h"

/**
 * RenderingHandler — domain "rendering"  (v0.3.0-dev / UE 5.8)
 *
 * Actions:
 *   set_nanite          → asset_path (StaticMesh package path), enabled (bool)
 *   set_lumen           → enabled (bool)
 *   set_shadow_method   → method (VSM|Cascaded|RayTraced)
 *   capture_reflection  → (no params) — triggers rebuild of all captures in current level
 *   set_post_process_blend → actor_name (optional), blend_weight, unbound (bool), priority
 *   set_console_var     → var_name, value (string), persist (bool, optional)
 *   get_render_stats    → returns Nanite/Lumen/VSM/PP state as structured JSON
 *   set_lod_screen_size → asset_path (StaticMesh), lod_index (int), screen_size (float 0-1)
 *   set_ray_tracing     → enabled (bool)
 *   set_anti_aliasing   → method (None|FXAA|TAA|MSAA|TSR)
 *   set_nanite_settings → asset_path, enabled (bool), max_wpo_displacement (float), allow_masked_materials (bool), max_pixels_per_edge (float)
 *   set_megalights      → enabled (bool)  [UE 5.7 Beta — r.MegaLights.Enable]
 *   set_smaa            → quality (0-3), edge_mode (Luma|Color|Depth)  [r.AntiAliasingMethod=5]
 *   set_affect_dynamic_lighting → actor_name, enabled (bool), component_name (optional)
 */
UCLASS()
class FORGEEDITORBRIDGE_API URenderingHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()
public:
	virtual FString GetDomainName() const override { return TEXT("rendering"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return { TEXT("set_nanite"), TEXT("set_lumen"), TEXT("set_shadow_method"),
		         TEXT("capture_reflection"), TEXT("set_post_process_blend"),
		         TEXT("set_console_var"), TEXT("get_render_stats"), TEXT("set_lod_screen_size"),
		         TEXT("set_ray_tracing"), TEXT("set_anti_aliasing"),
		         TEXT("set_nanite_settings"), TEXT("set_megalights"),
		         TEXT("set_smaa"), TEXT("set_affect_dynamic_lighting") };
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_SetNanite                (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLumen                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetShadowMethod          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CaptureReflection        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPostProcessBlend      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetConsoleVar            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetRenderStats           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLODScreenSize         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetRayTracing            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAntiAliasing          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetNaniteSettings        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMegaLights            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetSMAA                  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAffectDynamicLighting (TSharedPtr<FJsonObject> Params);

	UWorld* GetEditorWorld() const;
};
