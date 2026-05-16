#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "LandscapeHandler.generated.h"

class ALandscape;

/**
 * LandscapeHandler — domain "landscape"  (v0.12.0 / UE 5.7)
 *
 * All modify-actions locate the landscape by actor label in the current editor level.
 * Pass "first" as landscape_name to target the first landscape found.
 *
 * x/y coordinates for sculpt/paint are section-space vertex indices (same as
 * GetBoundingRect() + GetSectionBase()). For a landscape at origin with default
 * settings, (0,0) is the top-left corner.
 *
 * Actions:
 *   create_landscape → name, component_count_x/y, sections_per_component,
 *                      quads_per_section, location_x/y/z, scale_x/y/z,
 *                      heightmap_path (optional .r16/.raw/.png)
 *
 *   get_landscape_info → landscape_name
 *
 *   sculpt_height → landscape_name, x, y, radius (default 10),
 *                   strength (0–1, default 1.0),
 *                   mode ("raise"|"lower"|"flatten"|"smooth", default "raise"),
 *                   delta (world cm, default 1000.0, used by raise/lower),
 *                   target_height (world cm, optional, used by flatten — omit to use center height)
 *
 *   paint_layer  → landscape_name, layer_name, x, y,
 *                  radius (default 10), strength (0–1, default 1.0)
 *
 *   set_material → landscape_name, material_path
 *
 *   add_spline   → landscape_name, x, y, z
 *
 *   read_heightmap_capture → resolution (int32, default 256),
 *                            landscape_name (string, optional — if provided uses ExportHeightmapForActor)
 *                            Triggers HeightmapCapture export and returns landscape/height-slice.json.
 *
 *   apply_noise  → landscape_name, x, y, radius, strength (0–1, default 1.0),
 *                  noise_type ("perlin"|"fbm", default "perlin"),
 *                  frequency (default 1.0), amplitude (world cm, default 500.0),
 *                  mode ("add"|"set", default "add"), octaves (fbm only, default 4)
 *                  Procedurally displaces height using Perlin noise or FBM within a
 *                  circular brush. mode=add offsets current height; mode=set replaces it.
 *
 *   auto_paint   → landscape_name,
 *                  rules[{layer_name, height_min, height_max, slope_min (opt),
 *                         slope_max (opt), blend_width (default 10.0)}]
 *                  Iterates all landscape vertices, evaluates height/slope rules in order,
 *                  and paints the matching layer. First matching rule per vertex wins.
 *
 *   set_wpo_disable_distance → actor_label (string), distance (float)
 *                  Set WPODisableDistance on ALandscape. Pass "first" or actor label.
 *
 *   build_hlods      → (no params) Returns a PARTIALLY_FEASIBLE job descriptor for the
 *                  WorldPartitionHLODsBuilder commandlet.
 *
 *   import_heightmap → actor_label (string), file_path (string, absolute .r16/.png path)
 *                  PARTIALLY_FEASIBLE: no direct C++ API in UE 5.7; returns Python script.
 *
 *   get_terrain_data → actor_label (string, optional — defaults to "first")
 *                  Returns component count, section size, world partition flag, etc.
 *
 *   set_layer_info   → actor_label (string), layer_name (string), info_asset_path (string)
 *                  Load ULandscapeLayerInfoObject and assign to named layer in EditorLayerSettings.
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULandscapeHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("landscape"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("create_landscape"),
			TEXT("get_landscape_info"),
			TEXT("sculpt_height"),
			TEXT("paint_layer"),
			TEXT("set_material"),
			TEXT("add_spline"),
			TEXT("read_heightmap_capture"),
			TEXT("apply_noise"),
			TEXT("auto_paint"),
			TEXT("set_hole_mask"),
			TEXT("import_weightmap"),
			TEXT("export_heightmap"),
			// Phase 1d additions
			TEXT("set_wpo_disable_distance"),
			TEXT("build_hlods"),
			TEXT("import_heightmap"),
			TEXT("get_terrain_data"),
			TEXT("set_layer_info"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateLandscape  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetLandscapeInfo (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SculptHeight     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_PaintLayer       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMaterial      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddSpline              (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadHeightmapCapture   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ApplyNoise             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AutoPaint              (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetHoleMask            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportWeightmap        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExportHeightmap        (TSharedPtr<FJsonObject> Params);

	/** Find ALandscape by actor label (or "first"). Populates Result.Message on failure. */
	// Phase 1d additions
	FBridgeResult Action_SetWPODisableDistance (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_BuildHLODs            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportHeightmap       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetTerrainData        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetLayerInfo          (TSharedPtr<FJsonObject> Params);

	/** Find ALandscape by actor label (or "first"). Populates Result.Message on failure. */
	ALandscape* FindLandscape(const FString& LandscapeName, FBridgeResult& Result);
};
