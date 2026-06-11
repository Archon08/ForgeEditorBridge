#include "Handlers/RenderTargetHandler.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("rendertarget");

namespace
{
    UTextureRenderTarget2D* LoadRT(const FString& Path)
    {
        if (UTextureRenderTarget2D* RT = LoadObject<UTextureRenderTarget2D>(nullptr, *Path)) return RT;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UTextureRenderTarget2D>(nullptr, *Suffix);
    }

    UWorld* GetWorld_Editor()
    {
        return UBridgeHandlerBase::GetSafeEditorWorld();
    }

    ETextureRenderTargetFormat ParseFormat(const FString& Name)
    {
        const FString N = Name.IsEmpty() ? TEXT("RGBA8") : Name;
        if (N == TEXT("RGBA16f")) return RTF_RGBA16f;
        if (N == TEXT("RGBA32f")) return RTF_RGBA32f;
        if (N == TEXT("R8"))      return RTF_R8;
        if (N == TEXT("R16f"))    return RTF_R16f;
        if (N == TEXT("R32f"))    return RTF_R32f;
        if (N == TEXT("RG8"))     return RTF_RG8;
        if (N == TEXT("RG16f"))   return RTF_RG16f;
        if (N == TEXT("RG32f"))   return RTF_RG32f;
        return RTF_RGBA8;
    }

    FString FormatToString(ETextureRenderTargetFormat F)
    {
        switch (F)
        {
        case RTF_R8: return TEXT("R8");
        case RTF_R16f: return TEXT("R16f");
        case RTF_R32f: return TEXT("R32f");
        case RTF_RG8: return TEXT("RG8");
        case RTF_RG16f: return TEXT("RG16f");
        case RTF_RG32f: return TEXT("RG32f");
        case RTF_RGBA16f: return TEXT("RGBA16f");
        case RTF_RGBA32f: return TEXT("RGBA32f");
        case RTF_RGBA8:
        default: return TEXT("RGBA8");
        }
    }
}

FBridgeResult URenderTargetHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_rt"))             return Action_CreateRT(Params);
    if (Action == TEXT("clear_rt"))              return Action_ClearRT(Params);
    if (Action == TEXT("draw_material_to_rt"))   return Action_DrawMaterialToRT(Params);
    if (Action == TEXT("read_pixel"))            return Action_ReadPixel(Params);
    if (Action == TEXT("read_pixels_summary"))   return Action_ReadPixelsSummary(Params);
    if (Action == TEXT("get_rt_info"))           return Action_GetRTInfo(Params);
    if (Action == TEXT("resize_rt"))             return Action_ResizeRT(Params);
    if (Action == TEXT("set_clear_color"))       return Action_SetClearColor(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_rt, clear_rt, draw_material_to_rt, read_pixel, read_pixels_summary, get_rt_info, resize_rt, set_clear_color"));
}

FBridgeResult URenderTargetHandler::Action_CreateRT(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath, FormatName;
    int32 SizeX = 256, SizeY = 256;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_rt"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetNumberField(TEXT("size_x"), SizeX);
    Params->TryGetNumberField(TEXT("size_y"), SizeY);
    Params->TryGetStringField(TEXT("format"), FormatName);

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return MakeError(DOMAIN, TEXT("create_rt"), 3000,
        FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
    Package->FullyLoad();

    UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    if (!RT) return MakeError(DOMAIN, TEXT("create_rt"), 3000, TEXT("NewObject failed"));

    RT->RenderTargetFormat = ParseFormat(FormatName);
    RT->ClearColor = FLinearColor::Black;
    RT->bAutoGenerateMips = false;
    RT->InitAutoFormat(SizeX, SizeY);
    RT->UpdateResourceImmediate(true);

    FAssetRegistryModule::AssetCreated(RT);
    Package->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetNumberField(TEXT("size_x"), SizeX);
    Data->SetNumberField(TEXT("size_y"), SizeY);
    Data->SetStringField(TEXT("format"), FormatToString(RT->RenderTargetFormat));
    return MakeSuccess(DOMAIN, TEXT("create_rt"),
        FString::Printf(TEXT("Created RT '%s' (%dx%d %s)"),
            *AssetPath, SizeX, SizeY, *FormatToString(RT->RenderTargetFormat)), Data);
}

FBridgeResult URenderTargetHandler::Action_ClearRT(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("clear_rt"), 1000, TEXT("'asset_path' is required"));
    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("clear_rt"), 2000, TEXT("RT not found"));
    UWorld* World = GetWorld_Editor();
    if (!World) return MakeError(DOMAIN, TEXT("clear_rt"), 3000, TEXT("No world available"));

    double R=0, G=0, B=0, A=1;
    Params->TryGetNumberField(TEXT("r"), R);
    Params->TryGetNumberField(TEXT("g"), G);
    Params->TryGetNumberField(TEXT("b"), B);
    Params->TryGetNumberField(TEXT("a"), A);
    UKismetRenderingLibrary::ClearRenderTarget2D(World, RT, FLinearColor((float)R,(float)G,(float)B,(float)A));
    return MakeSuccess(DOMAIN, TEXT("clear_rt"),
        FString::Printf(TEXT("Cleared '%s' to (%.2f,%.2f,%.2f,%.2f)"), *AssetPath, R, G, B, A));
}

FBridgeResult URenderTargetHandler::Action_DrawMaterialToRT(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath, MaterialPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("draw_material_to_rt"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("draw_material_to_rt"), 1000, TEXT("'material_path' is required"));

    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("draw_material_to_rt"), 2000, TEXT("RT not found"));
    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
    {
        const FString Suffix = MaterialPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPath);
        Mat = LoadObject<UMaterialInterface>(nullptr, *Suffix);
    }
    if (!Mat) return MakeError(DOMAIN, TEXT("draw_material_to_rt"), 2000,
        FString::Printf(TEXT("Material not found: %s"), *MaterialPath));

    UWorld* World = GetWorld_Editor();
    if (!World) return MakeError(DOMAIN, TEXT("draw_material_to_rt"), 3000, TEXT("No world available"));
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT, Mat);
    return MakeSuccess(DOMAIN, TEXT("draw_material_to_rt"),
        FString::Printf(TEXT("Drew '%s' to '%s'"), *MaterialPath, *AssetPath));
}

FBridgeResult URenderTargetHandler::Action_ReadPixel(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    int32 X = 0, Y = 0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("read_pixel"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);

    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("read_pixel"), 2000, TEXT("RT not found"));
    UWorld* World = GetWorld_Editor();
    if (!World) return MakeError(DOMAIN, TEXT("read_pixel"), 3000, TEXT("No world available"));

    const FLinearColor LC = UKismetRenderingLibrary::ReadRenderTargetPixel(World, RT, X, Y);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("r"), LC.R);
    Data->SetNumberField(TEXT("g"), LC.G);
    Data->SetNumberField(TEXT("b"), LC.B);
    Data->SetNumberField(TEXT("a"), LC.A);
    return MakeSuccess(DOMAIN, TEXT("read_pixel"),
        FString::Printf(TEXT("(%.3f,%.3f,%.3f,%.3f)"), LC.R, LC.G, LC.B, LC.A), Data);
}

FBridgeResult URenderTargetHandler::Action_ReadPixelsSummary(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("read_pixels_summary"), 1000, TEXT("'asset_path' is required"));
    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("read_pixels_summary"), 2000, TEXT("RT not found"));
    UWorld* World = GetWorld_Editor();
    if (!World) return MakeError(DOMAIN, TEXT("read_pixels_summary"), 3000, TEXT("No world available"));

    TArray<FColor> Pixels;
    UKismetRenderingLibrary::ReadRenderTarget(World, RT, Pixels, /*bNormalize=*/false);
    if (Pixels.Num() == 0)
        return MakeSuccess(DOMAIN, TEXT("read_pixels_summary"), TEXT("No pixels read"));

    int32 MinR=255, MinG=255, MinB=255, MinA=255;
    int32 MaxR=0,   MaxG=0,   MaxB=0,   MaxA=0;
    int64 SumR=0, SumG=0, SumB=0, SumA=0;
    for (const FColor& C : Pixels)
    {
        MinR = FMath::Min<int32>(MinR, C.R); MaxR = FMath::Max<int32>(MaxR, C.R); SumR += C.R;
        MinG = FMath::Min<int32>(MinG, C.G); MaxG = FMath::Max<int32>(MaxG, C.G); SumG += C.G;
        MinB = FMath::Min<int32>(MinB, C.B); MaxB = FMath::Max<int32>(MaxB, C.B); SumB += C.B;
        MinA = FMath::Min<int32>(MinA, C.A); MaxA = FMath::Max<int32>(MaxA, C.A); SumA += C.A;
    }
    const double N = (double)Pixels.Num();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Pixels.Num());
    Data->SetStringField(TEXT("min"), FString::Printf(TEXT("R=%d G=%d B=%d A=%d"), MinR, MinG, MinB, MinA));
    Data->SetStringField(TEXT("max"), FString::Printf(TEXT("R=%d G=%d B=%d A=%d"), MaxR, MaxG, MaxB, MaxA));
    Data->SetStringField(TEXT("avg"), FString::Printf(TEXT("R=%.1f G=%.1f B=%.1f A=%.1f"),
        SumR/N, SumG/N, SumB/N, SumA/N));
    return MakeSuccess(DOMAIN, TEXT("read_pixels_summary"),
        FString::Printf(TEXT("Sampled %d pixel(s)"), Pixels.Num()), Data);
}

FBridgeResult URenderTargetHandler::Action_GetRTInfo(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_rt_info"), 1000, TEXT("'asset_path' is required"));
    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("get_rt_info"), 2000, TEXT("RT not found"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("size_x"), RT->SizeX);
    Data->SetNumberField(TEXT("size_y"), RT->SizeY);
    Data->SetStringField(TEXT("format"), FormatToString(RT->RenderTargetFormat));
    Data->SetNumberField(TEXT("clear_r"), RT->ClearColor.R);
    Data->SetNumberField(TEXT("clear_g"), RT->ClearColor.G);
    Data->SetNumberField(TEXT("clear_b"), RT->ClearColor.B);
    Data->SetNumberField(TEXT("clear_a"), RT->ClearColor.A);
    return MakeSuccess(DOMAIN, TEXT("get_rt_info"),
        FString::Printf(TEXT("%dx%d %s"), RT->SizeX, RT->SizeY, *FormatToString(RT->RenderTargetFormat)), Data);
}

FBridgeResult URenderTargetHandler::Action_ResizeRT(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("RT: resize"));
    FString AssetPath;
    int32 SizeX = 0, SizeY = 0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("resize_rt"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("size_x"), SizeX) || SizeX <= 0)
        return MakeError(DOMAIN, TEXT("resize_rt"), 1000, TEXT("'size_x' (>0) is required"));
    if (!Params->TryGetNumberField(TEXT("size_y"), SizeY) || SizeY <= 0)
        return MakeError(DOMAIN, TEXT("resize_rt"), 1000, TEXT("'size_y' (>0) is required"));

    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("resize_rt"), 2000, TEXT("RT not found"));
    RT->ResizeTarget(SizeX, SizeY);
    RT->UpdateResourceImmediate(true);
    RT->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("resize_rt"),
        FString::Printf(TEXT("Resized to %dx%d"), SizeX, SizeY));
}

FBridgeResult URenderTargetHandler::Action_SetClearColor(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("RT: set_clear_color"));
    FString AssetPath;
    double R=0, G=0, B=0, A=1;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_clear_color"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetNumberField(TEXT("r"), R);
    Params->TryGetNumberField(TEXT("g"), G);
    Params->TryGetNumberField(TEXT("b"), B);
    Params->TryGetNumberField(TEXT("a"), A);

    UTextureRenderTarget2D* RT = LoadRT(AssetPath);
    if (!RT) return MakeError(DOMAIN, TEXT("set_clear_color"), 2000, TEXT("RT not found"));
    RT->ClearColor = FLinearColor((float)R, (float)G, (float)B, (float)A);
    RT->UpdateResourceImmediate(true);
    RT->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_clear_color"),
        FString::Printf(TEXT("ClearColor=(%.2f,%.2f,%.2f,%.2f)"), R, G, B, A));
}
