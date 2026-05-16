#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeMaterialCapture.generated.h"

class UMaterial;
class UMaterialInstance;

/**
 * v1.0 — Material & Material Instance Capture + Static Audit
 *
 * Exports the internal structure of UMaterial and UMaterialInstance assets,
 * including expression node lists, parameter overrides, and a static performance
 * audit. Targets UE 5.7.
 *
 * Output: {ProjectRoot}/Forge/ue-context/materials/{AssetName}.json
 *
 * Trigger from Python:
 *   subsystem = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   subsystem.material_capture.export_all_materials()
 *   subsystem.material_capture.export_material("/Game/Path/M_MyMaterial")
 *   subsystem.material_capture.export_materials_by_prefix("/Game/YourProject/Materials")
 *
 * Data captured — UMaterial:
 *   properties  : domain, blend_mode, shading_model, two_sided, opacity_mask_clip_value,
 *                 translucency_lighting_mode, num_customized_uvs, usage_flags
 *   expressions : type, parameter_name, texture_asset (samples), uv_channel (TexCoord),
 *                 hlsl_description + input_count (Custom), function_path (MFCall)
 *
 * Data captured — UMaterialInstance:
 *   parameter_overrides : scalar, vector, texture (all flagged overridden=true)
 *   static_switches     : name + value for all overridden static switch parameters
 *   instance_chain_depth: hops from this instance to the root UMaterial
 *
 * Audit rules:
 *   TEXTURE_SAMPLE_LIMIT          — > 16 texture samples (mobile / DX11 limit)
 *   TRANSLUCENCY_OVERUSE          — Translucent + > 8 texture samples
 *   MISSING_INSTANCE_PARAM        — Instance overrides a parameter absent from the parent
 *   UNOPTIMIZED_REFLECTIONS       — SceneColor read or bare ReflectionVectorWS
 *   CUSTOM_HLSL                   — UMaterialExpressionCustom present (opaque to optimizer)
 *   MATERIAL_FUNCTION_COMPLEXITY  — Material function calls that hide graph complexity
 *   DEEP_INSTANCE_CHAIN           — Instance chain depth > 3
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeMaterialCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Export a single material or material instance by asset path.
     * Writes to materials/{AssetName}.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportMaterial(const FString& AssetPath);

    /**
     * Export all UMaterial and UMaterialInstanceConstant assets under /Game/.
     * Returns the number of assets successfully exported.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAllMaterials();

    /**
     * Export all materials whose package path begins with Prefix.
     * Example: Prefix = "/Game/YourProject/Materials"
     * Returns the number of assets successfully exported.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportMaterialsByPrefix(const FString& Prefix);

private:
    FString OutputDir;

    // Per-type export — returns a populated JSON object or nullptr on failure
    TSharedPtr<FJsonObject> ExportUMaterial(UMaterial* Mat);
    TSharedPtr<FJsonObject> ExportUMaterialInstance(UMaterialInstance* MI);

    // Audit helpers — return arrays of issue JSON value objects
    TArray<TSharedPtr<FJsonValue>> AuditMaterial(
        UMaterial* Mat,
        int32 TextureSampleCount,
        bool bHasReflectionNode,
        bool bHasSceneColorNode,
        int32 CustomHLSLCount,
        const TArray<FString>& FunctionCallPaths);
    TArray<TSharedPtr<FJsonValue>> AuditMaterialInstance(UMaterialInstance* MI);

    static TSharedPtr<FJsonObject> MakeIssue(
        const FString& IssueType,
        const FString& Severity,
        const FString& AssetPath,
        const FString& Detail);

    // Walk MI->Parent chain until UMaterial; return hop count
    static int32 ComputeInstanceChainDepth(UMaterialInstance* MI);

    // Internal bulk export using an already-configured asset registry filter
    int32 ExportAssetsWithFilter(const FARFilter& Filter);

    // READ-MERGE-WRITE index.json to add/update the "materials" section
    void UpdateIndexFile(int32 ExportedCount);
};
