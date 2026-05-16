#include "Handlers/NiagaraBakerHandler.h"
#include "NiagaraSystem.h"
#include "NiagaraBakerSettings.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("niagara_baker");

namespace
{
    UNiagaraSystem* LoadSys(const FString& Path)
    {
        if (UNiagaraSystem* S = LoadObject<UNiagaraSystem>(nullptr, *Path)) return S;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UNiagaraSystem>(nullptr, *Suffix);
    }
}

FBridgeResult UNiagaraBakerHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("get_baker_info"))      return Action_GetBakerInfo(Params);
    if (Action == TEXT("set_baker_settings"))  return Action_SetBakerSettings(Params);
    if (Action == TEXT("set_output_texture"))  return Action_SetOutputTexture(Params);
    if (Action == TEXT("list_baker_outputs"))  return Action_ListBakerOutputs(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("get_baker_info, set_baker_settings, set_output_texture, list_baker_outputs"));
}

FBridgeResult UNiagaraBakerHandler::Action_GetBakerInfo(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_baker_info"), 1000, TEXT("'asset_path' is required"));
    UNiagaraSystem* Sys = LoadSys(AssetPath);
    if (!Sys) return MakeError(DOMAIN, TEXT("get_baker_info"), 2000, TEXT("System not found"));

    UNiagaraBakerSettings* Settings = Sys->GetBakerSettings();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("has_settings"), Settings != nullptr);
    if (Settings)
    {
        Data->SetNumberField(TEXT("frames_per_dimension_x"), Settings->FramesPerDimension.X);
        Data->SetNumberField(TEXT("frames_per_dimension_y"), Settings->FramesPerDimension.Y);
        Data->SetNumberField(TEXT("duration_seconds"), Settings->DurationSeconds);
        Data->SetNumberField(TEXT("frames_per_second"), Settings->FramesPerSecond);
        Data->SetNumberField(TEXT("output_count"), Settings->Outputs.Num());
    }
    return MakeSuccess(DOMAIN, TEXT("get_baker_info"),
        Settings ? TEXT("Baker settings present") : TEXT("No baker settings"), Data);
}

FBridgeResult UNiagaraBakerHandler::Action_SetBakerSettings(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Niagara Baker: settings"));
    FString AssetPath;
    int32 FramesX = 8, FramesY = 8;
    double Duration = 1.0, FPS = 30.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_baker_settings"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetNumberField(TEXT("frames_per_dimension_x"), FramesX);
    Params->TryGetNumberField(TEXT("frames_per_dimension_y"), FramesY);
    Params->TryGetNumberField(TEXT("duration_seconds"), Duration);
    Params->TryGetNumberField(TEXT("frames_per_second"), FPS);

    UNiagaraSystem* Sys = LoadSys(AssetPath);
    if (!Sys) return MakeError(DOMAIN, TEXT("set_baker_settings"), 2000, TEXT("System not found"));

    UNiagaraBakerSettings* Settings = Sys->GetBakerSettings();
    if (!Settings)
    {
        // 5.7: there is no public SetBakerSettings on UNiagaraSystem (only
        // SetBakerGeneratedSettings for output generation). The editor-side
        // settings come into existence when the Niagara Baker tab is opened
        // on the system; we cannot mint and bind a fresh one here.
        return MakeError(DOMAIN, TEXT("set_baker_settings"), 3003,
            TEXT("UNiagaraSystem has no public setter for editor-side baker settings in 5.7"),
            TEXT("Open the Niagara Baker tool tab once on the system to materialize the baker settings, then call set_baker_settings to mutate them"));
    }
    Settings->FramesPerDimension = FIntPoint(FramesX, FramesY);
    Settings->DurationSeconds = (float)Duration;
    Settings->FramesPerSecond = (float)FPS;
    Sys->MarkPackageDirty();

    return MakeSuccess(DOMAIN, TEXT("set_baker_settings"),
        FString::Printf(TEXT("Baker: %dx%d frames, %.2fs at %.1f FPS"),
            FramesX, FramesY, Duration, FPS));
}

FBridgeResult UNiagaraBakerHandler::Action_SetOutputTexture(TSharedPtr<FJsonObject> Params)
{
    return MakeError(DOMAIN, TEXT("set_output_texture"), 3003,
        TEXT("Niagara baker output binding requires editor-tool dispatch in 5.7"),
        TEXT("Use the Niagara Baker UI to bind output textures; or use the bridge's PythonHandler with unreal.NiagaraBakerSettings"));
}

FBridgeResult UNiagaraBakerHandler::Action_ListBakerOutputs(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_baker_outputs"), 1000, TEXT("'asset_path' is required"));
    UNiagaraSystem* Sys = LoadSys(AssetPath);
    if (!Sys) return MakeError(DOMAIN, TEXT("list_baker_outputs"), 2000, TEXT("System not found"));
    UNiagaraBakerSettings* Settings = Sys->GetBakerSettings();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("output_count"), Settings ? Settings->Outputs.Num() : 0);
    return MakeSuccess(DOMAIN, TEXT("list_baker_outputs"),
        FString::Printf(TEXT("%d output(s)"), Settings ? Settings->Outputs.Num() : 0), Data);
}
