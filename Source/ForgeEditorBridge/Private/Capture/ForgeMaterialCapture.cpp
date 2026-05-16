#include "Capture/ForgeMaterialCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Material types ---
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"

// --- Expression types ---
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionReflectionVectorWS.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Materials/MaterialExpressionCustom.h"               // v1.0 — CUSTOM_HLSL audit
#include "Materials/MaterialExpressionMaterialFunctionCall.h" // v1.0 — function call detection
#include "Materials/MaterialExpressionTextureCoordinate.h"    // v1.0 — UV channel capture
#include "Materials/MaterialFunctionInterface.h"              // v1.0 — function path extraction

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeMaterialCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("materials")));
    UE_LOG(LogTemp, Log, TEXT("ForgeMaterial: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportMaterial — single asset dispatch
// ---------------------------------------------------------------------------

bool UForgeMaterialCapture::ExportMaterial(const FString& AssetPath)
{
    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeMaterial: Failed to load: %s"), *AssetPath);
        return false;
    }

    TSharedPtr<FJsonObject> Root;

    if (UMaterial* Mat = Cast<UMaterial>(Asset))
    {
        Root = ExportUMaterial(Mat);
    }
    else if (UMaterialInstance* MI = Cast<UMaterialInstance>(Asset))
    {
        Root = ExportUMaterialInstance(MI);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeMaterial: Not a Material or MaterialInstance: %s"), *AssetPath);
        return false;
    }

    if (!Root.IsValid()) return false;

    const FString AssetName = FPaths::GetBaseFilename(AssetPath);
    bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("materials"), AssetName, Root.ToSharedRef());
    if (bOK)
    {
        UE_LOG(LogTemp, Log, TEXT("ForgeMaterial: Exported -> materials/%s.json"), *AssetName);
    }
    return bOK;
}

// ---------------------------------------------------------------------------
// ExportAllMaterials / ExportMaterialsByPrefix
// ---------------------------------------------------------------------------

int32 UForgeMaterialCapture::ExportAllMaterials()
{
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));

    const int32 Count = ExportAssetsWithFilter(Filter);
    UpdateIndexFile(Count);
    return Count;
}

int32 UForgeMaterialCapture::ExportMaterialsByPrefix(const FString& Prefix)
{
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(*Prefix);

    const int32 Count = ExportAssetsWithFilter(Filter);
    UpdateIndexFile(Count);
    return Count;
}

int32 UForgeMaterialCapture::ExportAssetsWithFilter(const FARFilter& Filter)
{
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    int32 Count = 0;
    for (const FAssetData& AssetData : Assets)
    {
        if (ExportMaterial(AssetData.GetObjectPathString()))
        {
            ++Count;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("ForgeMaterial: Exported %d material(s)."), Count);
    return Count;
}

// ---------------------------------------------------------------------------
// ExportUMaterial
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeMaterialCapture::ExportUMaterial(UMaterial* Mat)
{
    if (!Mat) return nullptr;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("asset_path"), Mat->GetPathName());
    Root->SetStringField(TEXT("asset_type"), TEXT("UMaterial"));

    // -------------------------------------------------------------------------
    // Properties
    // -------------------------------------------------------------------------
    TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

    // Domain — Surface / DeferredDecal / LightFunction / Volume / PostProcess / UI / VirtualTexture
    if (const UEnum* DomainEnum = StaticEnum<EMaterialDomain>())
    {
        Props->SetStringField(TEXT("domain"),
            DomainEnum->GetNameStringByValue(static_cast<int64>(Mat->MaterialDomain)));
    }

    if (const UEnum* ShadingEnum = StaticEnum<EMaterialShadingModel>())
    {
        const EMaterialShadingModel SM = Mat->GetShadingModels().GetFirstShadingModel();
        Props->SetStringField(TEXT("shading_model"),
            ShadingEnum->GetNameStringByValue(static_cast<int64>(SM)));
    }

    if (const UEnum* BlendEnum = StaticEnum<EBlendMode>())
    {
        Props->SetStringField(TEXT("blend_mode"),
            BlendEnum->GetNameStringByValue(static_cast<int64>(Mat->GetBlendMode())));
    }

    Props->SetBoolField(TEXT("two_sided"),         Mat->IsTwoSided());
    Props->SetNumberField(TEXT("num_customized_uvs"), Mat->NumCustomizedUVs);

    // OpacityMaskClipValue — only meaningful for Masked blend mode
    if (Mat->GetBlendMode() == BLEND_Masked)
    {
        Props->SetNumberField(TEXT("opacity_mask_clip_value"), Mat->OpacityMaskClipValue);
    }

    // TranslucencyLightingMode — only meaningful for Translucent blend mode
    if (Mat->GetBlendMode() == BLEND_Translucent)
    {
        if (const UEnum* TLMEnum = StaticEnum<ETranslucencyLightingMode>())
        {
            Props->SetStringField(TEXT("translucency_lighting_mode"),
                TLMEnum->GetNameStringByValue(static_cast<int64>(Mat->TranslucencyLightingMode)));
        }
    }

    // Usage flags — which mesh/particle/system types this material is flagged for
    TSharedRef<FJsonObject> UsageFlags = MakeShared<FJsonObject>();
    UsageFlags->SetBoolField(TEXT("skeletal_mesh"),              !!Mat->bUsedWithSkeletalMesh);
    UsageFlags->SetBoolField(TEXT("static_lighting"),            !!Mat->bUsedWithStaticLighting);
    UsageFlags->SetBoolField(TEXT("morph_targets"),              !!Mat->bUsedWithMorphTargets);
    UsageFlags->SetBoolField(TEXT("spline_meshes"),              !!Mat->bUsedWithSplineMeshes);
    UsageFlags->SetBoolField(TEXT("instanced_static_meshes"),    !!Mat->bUsedWithInstancedStaticMeshes);
    UsageFlags->SetBoolField(TEXT("geometry_collections"),       !!Mat->bUsedWithGeometryCollections);
    UsageFlags->SetBoolField(TEXT("particle_sprites"),           !!Mat->bUsedWithParticleSprites);
    UsageFlags->SetBoolField(TEXT("beam_trails"),                !!Mat->bUsedWithBeamTrails);
    UsageFlags->SetBoolField(TEXT("mesh_particles"),             !!Mat->bUsedWithMeshParticles);
    UsageFlags->SetBoolField(TEXT("niagara_sprites"),            !!Mat->bUsedWithNiagaraSprites);
    UsageFlags->SetBoolField(TEXT("niagara_ribbons"),            !!Mat->bUsedWithNiagaraRibbons);
    UsageFlags->SetBoolField(TEXT("niagara_mesh_particles"),     !!Mat->bUsedWithNiagaraMeshParticles);
    UsageFlags->SetBoolField(TEXT("hair_strands"),               !!Mat->bUsedWithHairStrands);
    UsageFlags->SetBoolField(TEXT("water"),                      !!Mat->bUsedWithWater);
    Props->SetObjectField(TEXT("usage_flags"), UsageFlags);

    Root->SetObjectField(TEXT("properties"), Props);

    // -------------------------------------------------------------------------
    // Expression scan
    // -------------------------------------------------------------------------
    int32 TextureSampleCount = 0;
    int32 CustomHLSLCount    = 0;
    bool  bHasReflectionNode = false;
    bool  bHasSceneColorNode = false;
    TArray<FString> FunctionCallPaths;

    TArray<TSharedPtr<FJsonValue>> ExprArray;

    // GetExpressions() is the UE5.4+ API.
    // If building on pre-5.4, swap to: Mat->GetExpressionCollection().Expressions
    for (UMaterialExpression* Expr : Mat->GetExpressions())
    {
        if (!Expr) continue;

        TSharedRef<FJsonObject> ExprObj = MakeShared<FJsonObject>();
        ExprObj->SetStringField(TEXT("type"), Expr->GetClass()->GetName());

        // --- Texture sample (and texture sample parameters) ---
        if (Expr->IsA<UMaterialExpressionTextureSample>())
        {
            ++TextureSampleCount;

            const UMaterialExpressionTextureSample* TS =
                Cast<UMaterialExpressionTextureSample>(Expr);

            // Bound texture asset path — tells us which actual texture is wired in
            if (TS->Texture)
            {
                ExprObj->SetStringField(TEXT("texture_asset"), TS->Texture->GetPathName());
            }

            // Parameter name, if this is a named parameter
            if (const UMaterialExpressionTextureSampleParameter* TP =
                    Cast<UMaterialExpressionTextureSampleParameter>(Expr))
            {
                ExprObj->SetStringField(TEXT("parameter_name"), TP->ParameterName.ToString());
            }
        }
        // --- Scalar parameter ---
        else if (const UMaterialExpressionScalarParameter* SP =
                     Cast<UMaterialExpressionScalarParameter>(Expr))
        {
            ExprObj->SetStringField(TEXT("parameter_name"), SP->ParameterName.ToString());
        }
        // --- Vector parameter ---
        else if (const UMaterialExpressionVectorParameter* VP =
                     Cast<UMaterialExpressionVectorParameter>(Expr))
        {
            ExprObj->SetStringField(TEXT("parameter_name"), VP->ParameterName.ToString());
        }
        // --- Texture coordinate — which UV channel is being read ---
        else if (const UMaterialExpressionTextureCoordinate* TC =
                     Cast<UMaterialExpressionTextureCoordinate>(Expr))
        {
            ExprObj->SetNumberField(TEXT("uv_channel"), TC->CoordinateIndex);
            ExprObj->SetNumberField(TEXT("u_tiling"),   TC->UTiling);
            ExprObj->SetNumberField(TEXT("v_tiling"),   TC->VTiling);
        }
        // --- Custom HLSL — opaque to the shader optimizer ---
        else if (const UMaterialExpressionCustom* CH =
                     Cast<UMaterialExpressionCustom>(Expr))
        {
            ++CustomHLSLCount;
            // Capture description and input count, NOT the raw HLSL code
            // (code can be large and is not useful for static audit)
            ExprObj->SetStringField(TEXT("hlsl_description"), CH->Description);
            ExprObj->SetNumberField(TEXT("hlsl_input_count"), CH->Inputs.Num());
        }
        // --- Material function call — hides subgraph complexity ---
        else if (const UMaterialExpressionMaterialFunctionCall* MFC =
                     Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
        {
            if (MFC->MaterialFunction)
            {
                const FString FuncPath = MFC->MaterialFunction->GetPathName();
                FunctionCallPaths.Add(FuncPath);
                ExprObj->SetStringField(TEXT("function_path"), FuncPath);
            }
        }
        // --- Reflection / screen-space flags ---
        else if (Expr->IsA<UMaterialExpressionReflectionVectorWS>())
        {
            bHasReflectionNode = true;
        }
        else if (Expr->IsA<UMaterialExpressionSceneColor>())
        {
            bHasSceneColorNode = true;
        }

        ExprArray.Add(MakeShared<FJsonValueObject>(ExprObj));
    }

    Root->SetNumberField(TEXT("texture_sample_count"),   TextureSampleCount);
    Root->SetNumberField(TEXT("custom_hlsl_count"),      CustomHLSLCount);
    Root->SetNumberField(TEXT("function_call_count"),    FunctionCallPaths.Num());
    Root->SetArrayField (TEXT("expressions"),            ExprArray);

    // -------------------------------------------------------------------------
    // Audit
    // -------------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> Issues = AuditMaterial(
        Mat, TextureSampleCount, bHasReflectionNode, bHasSceneColorNode,
        CustomHLSLCount, FunctionCallPaths);

    TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
    AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
    AuditObj->SetArrayField(TEXT("issues"), Issues);
    Root->SetObjectField(TEXT("audit"), AuditObj);

    return Root;
}

// ---------------------------------------------------------------------------
// ExportUMaterialInstance
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeMaterialCapture::ExportUMaterialInstance(UMaterialInstance* MI)
{
    if (!MI) return nullptr;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("asset_path"), MI->GetPathName());
    Root->SetStringField(TEXT("asset_type"), TEXT("UMaterialInstance"));
    Root->SetStringField(TEXT("parent"),
        MI->Parent ? MI->Parent->GetPathName() : TEXT("(none)"));
    Root->SetNumberField(TEXT("instance_chain_depth"), ComputeInstanceChainDepth(MI));

    // -------------------------------------------------------------------------
    // Runtime parameter overrides (scalar / vector / texture)
    // -------------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> ScalarArr;
    for (const FScalarParameterValue& SV : MI->ScalarParameterValues)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),       SV.ParameterInfo.Name.ToString());
        Obj->SetNumberField(TEXT("value"),      SV.ParameterValue);
        Obj->SetBoolField  (TEXT("overridden"), true);
        ScalarArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TArray<TSharedPtr<FJsonValue>> VectorArr;
    for (const FVectorParameterValue& VV : MI->VectorParameterValues)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), VV.ParameterInfo.Name.ToString());

        TSharedRef<FJsonObject> Color = MakeShared<FJsonObject>();
        Color->SetNumberField(TEXT("r"), VV.ParameterValue.R);
        Color->SetNumberField(TEXT("g"), VV.ParameterValue.G);
        Color->SetNumberField(TEXT("b"), VV.ParameterValue.B);
        Color->SetNumberField(TEXT("a"), VV.ParameterValue.A);
        Obj->SetObjectField(TEXT("value"), Color);
        Obj->SetBoolField(TEXT("overridden"), true);
        VectorArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TArray<TSharedPtr<FJsonValue>> TextureArr;
    for (const FTextureParameterValue& TV : MI->TextureParameterValues)
    {
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),
            TV.ParameterInfo.Name.ToString());
        Obj->SetStringField(TEXT("value"),
            TV.ParameterValue ? TV.ParameterValue->GetPathName() : TEXT("(none)"));
        Obj->SetBoolField(TEXT("overridden"), true);
        TextureArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("scalar"),  ScalarArr);
    Params->SetArrayField(TEXT("vector"),  VectorArr);
    Params->SetArrayField(TEXT("texture"), TextureArr);
    Root->SetObjectField(TEXT("parameter_overrides"), Params);

    // -------------------------------------------------------------------------
    // Static switch parameter overrides
    // These drive shader permutation selection — higher impact than runtime params.
    // FStaticParameterSet::EditorOnly is the UE5.4+ location for switch/mask params.
    // -------------------------------------------------------------------------
#if WITH_EDITORONLY_DATA
    {
        FStaticParameterSet StaticParams;
        MI->GetStaticParameterValues(StaticParams);

        TArray<TSharedPtr<FJsonValue>> SwitchArr;
        for (const FStaticSwitchParameter& Param : StaticParams.StaticSwitchParameters)
        {
            if (!Param.bOverride) continue;
            TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"),       Param.ParameterInfo.Name.ToString());
            Obj->SetBoolField  (TEXT("value"),      Param.Value);
            Obj->SetBoolField  (TEXT("overridden"), true);
            SwitchArr.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TArray<TSharedPtr<FJsonValue>> MaskArr;
        for (const FStaticComponentMaskParameter& Param : StaticParams.EditorOnly.StaticComponentMaskParameters)
        {
            if (!Param.bOverride) continue;
            TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
            // Store as "RGBA" string for readability
            FString Mask;
            if (Param.R) Mask += TEXT("R");
            if (Param.G) Mask += TEXT("G");
            if (Param.B) Mask += TEXT("B");
            if (Param.A) Mask += TEXT("A");
            Obj->SetStringField(TEXT("mask"),       Mask.IsEmpty() ? TEXT("none") : Mask);
            Obj->SetBoolField  (TEXT("overridden"), true);
            MaskArr.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedRef<FJsonObject> StaticOverrides = MakeShared<FJsonObject>();
        StaticOverrides->SetArrayField(TEXT("static_switches"),       SwitchArr);
        StaticOverrides->SetArrayField(TEXT("component_mask_params"),  MaskArr);
        Root->SetObjectField(TEXT("static_parameter_overrides"), StaticOverrides);
    }
#endif

    // -------------------------------------------------------------------------
    // Audit
    // -------------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> Issues = AuditMaterialInstance(MI);

    TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
    AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
    AuditObj->SetArrayField(TEXT("issues"), Issues);
    Root->SetObjectField(TEXT("audit"), AuditObj);

    return Root;
}

// ---------------------------------------------------------------------------
// AuditMaterial — rules 1, 2, 4, 5, 6
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeMaterialCapture::AuditMaterial(
    UMaterial* Mat,
    int32 TextureSampleCount,
    bool bHasReflectionNode,
    bool bHasSceneColorNode,
    int32 CustomHLSLCount,
    const TArray<FString>& FunctionCallPaths)
{
    TArray<TSharedPtr<FJsonValue>> Issues;
    if (!Mat) return Issues;

    const FString AssetPath = Mat->GetPathName();

    // RULE 1: TEXTURE_SAMPLE_LIMIT
    if (TextureSampleCount > 16)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("TEXTURE_SAMPLE_LIMIT"),
            TEXT("error"),
            AssetPath,
            FString::Printf(
                TEXT("Material uses %d texture samples, exceeding the 16-sample limit for mobile and DX11. Reduce or consolidate texture reads."),
                TextureSampleCount))));
    }

    // RULE 2: TRANSLUCENCY_OVERUSE
    if (Mat->GetBlendMode() == BLEND_Translucent && TextureSampleCount > 8)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("TRANSLUCENCY_OVERUSE"),
            TEXT("warning"),
            AssetPath,
            FString::Printf(
                TEXT("Translucent material uses %d texture samples. Translucent surfaces are sorted and re-drawn each frame — high sample counts multiply cost significantly. Consider reducing samples or switching to masked if alpha cutout is sufficient."),
                TextureSampleCount))));
    }

    // RULE 4: UNOPTIMIZED_REFLECTIONS
    if (bHasSceneColorNode)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("UNOPTIMIZED_REFLECTIONS"),
            TEXT("warning"),
            AssetPath,
            TEXT("Material samples SceneColor (screen-space read). This breaks render pass optimizations, is unavailable in some deferred paths, and is expensive on mobile. Ensure a cost-aware fallback exists for lower-end hardware."))));
    }
    else if (bHasReflectionNode)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("UNOPTIMIZED_REFLECTIONS"),
            TEXT("info"),
            AssetPath,
            TEXT("Material uses ReflectionVectorWS. Verify the reflection capture setup is appropriate for the target platform."))));
    }

    // RULE 5: CUSTOM_HLSL
    // Custom HLSL bypasses the shader graph optimizer entirely — cannot be analyzed,
    // cannot be cross-compiled to non-HLSL targets, and hides actual instruction count.
    if (CustomHLSLCount > 0)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("CUSTOM_HLSL"),
            TEXT("error"),
            AssetPath,
            FString::Printf(
                TEXT("%d custom HLSL node(s) present. Custom nodes bypass the shader graph optimizer, break cross-compilation (mobile/Vulkan/Metal), and are opaque to static analysis. Audit manually."),
                CustomHLSLCount))));
    }

    // RULE 6: MATERIAL_FUNCTION_COMPLEXITY
    // Function calls fold complexity into a black box from the outside view of this material.
    if (FunctionCallPaths.Num() > 0)
    {
        // Build a deduplicated list of called function paths for context
        TSet<FString> Unique(FunctionCallPaths);
        FString FuncList;
        for (const FString& P : Unique)
        {
            FuncList += FPaths::GetBaseFilename(P) + TEXT(", ");
        }
        FuncList.RemoveFromEnd(TEXT(", "));

        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("MATERIAL_FUNCTION_COMPLEXITY"),
            TEXT("info"),
            AssetPath,
            FString::Printf(
                TEXT("%d material function call(s) hide subgraph complexity from this material's expression count. Functions called: %s. Open each to assess instruction contribution."),
                FunctionCallPaths.Num(), *FuncList))));
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// AuditMaterialInstance — rules 3, 7
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeMaterialCapture::AuditMaterialInstance(UMaterialInstance* MI)
{
    TArray<TSharedPtr<FJsonValue>> Issues;
    if (!MI || !MI->Parent) return Issues;

    const FString AssetPath  = MI->GetPathName();
    const FString ParentName = MI->Parent->GetName();

    // RULE 7: DEEP_INSTANCE_CHAIN
    const int32 Depth = ComputeInstanceChainDepth(MI);
    if (Depth > 3)
    {
        Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
            TEXT("DEEP_INSTANCE_CHAIN"),
            TEXT("warning"),
            AssetPath,
            FString::Printf(
                TEXT("Instance chain is %d hops deep. Deep chains make it hard to trace which parameters are actually in effect and increase load-time resolution cost. Consider flattening to <= 3 levels."),
                Depth))));
    }

    // RULE 3: MISSING_INSTANCE_PARAM — orphaned overrides
    TArray<FMaterialParameterInfo> ValidScalar, ValidVector, ValidTexture;
    TArray<FGuid> Dummy;
    MI->Parent->GetAllScalarParameterInfo (ValidScalar,  Dummy);
    MI->Parent->GetAllVectorParameterInfo (ValidVector,  Dummy);
    MI->Parent->GetAllTextureParameterInfo(ValidTexture, Dummy);

    auto ParamExists = [](const TArray<FMaterialParameterInfo>& Infos, const FName& Name) -> bool
    {
        for (const FMaterialParameterInfo& Info : Infos)
        {
            if (Info.Name == Name) return true;
        }
        return false;
    };

    for (const FScalarParameterValue& SV : MI->ScalarParameterValues)
    {
        if (!ParamExists(ValidScalar, SV.ParameterInfo.Name))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_INSTANCE_PARAM"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("Scalar parameter '%s' is overridden but does not exist in parent '%s'. Orphaned override — has no effect."),
                    *SV.ParameterInfo.Name.ToString(), *ParentName))));
        }
    }

    for (const FVectorParameterValue& VV : MI->VectorParameterValues)
    {
        if (!ParamExists(ValidVector, VV.ParameterInfo.Name))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_INSTANCE_PARAM"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("Vector parameter '%s' is overridden but does not exist in parent '%s'. Orphaned override — has no effect."),
                    *VV.ParameterInfo.Name.ToString(), *ParentName))));
        }
    }

    for (const FTextureParameterValue& TV : MI->TextureParameterValues)
    {
        if (!ParamExists(ValidTexture, TV.ParameterInfo.Name))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_INSTANCE_PARAM"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("Texture parameter '%s' is overridden but does not exist in parent '%s'. Orphaned override — has no effect."),
                    *TV.ParameterInfo.Name.ToString(), *ParentName))));
        }
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// ComputeInstanceChainDepth
// ---------------------------------------------------------------------------

int32 UForgeMaterialCapture::ComputeInstanceChainDepth(UMaterialInstance* MI)
{
    int32 Depth = 0;
    UMaterialInterface* Current = MI ? MI->Parent : nullptr;
    while (Current)
    {
        ++Depth;
        if (Current->IsA<UMaterial>()) break;
        UMaterialInstance* ParentMI = Cast<UMaterialInstance>(Current);
        Current = ParentMI ? ParentMI->Parent : nullptr;
    }
    return Depth;
}

// ---------------------------------------------------------------------------
// MakeIssue
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeMaterialCapture::MakeIssue(
    const FString& IssueType,
    const FString& Severity,
    const FString& AssetPath,
    const FString& Detail)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("issue_type"), IssueType);
    Obj->SetStringField(TEXT("severity"),   Severity);
    Obj->SetStringField(TEXT("asset_path"), AssetPath);
    Obj->SetStringField(TEXT("detail"),     Detail);
    return Obj;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeMaterialCapture::UpdateIndexFile(int32 ExportedCount)
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
    {
        if (Found->IsValid() && (*Found)->Type == EJson::Object)
        {
            Captures = (*Found)->AsObject();
        }
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> MatSection = MakeShared<FJsonObject>();
    MatSection->SetStringField(TEXT("directory"),      TEXT("materials/"));
    MatSection->SetNumberField(TEXT("exported_count"), ExportedCount);
    MatSection->SetStringField(TEXT("last_updated"),   FForgeContextWriter::NowISO8601());
    Captures->SetObjectField(TEXT("materials"), MatSection);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
