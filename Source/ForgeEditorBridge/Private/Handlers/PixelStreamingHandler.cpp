#include "Handlers/PixelStreamingHandler.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("pixelstream");

namespace
{
    bool IsLoaded()
    {
        return FModuleManager::Get().IsModuleLoaded(TEXT("PixelStreaming"));
    }
    bool RunCmd(const FString& Cmd)
    {
        if (!GEngine) return false;
        UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return GEngine->Exec(W, *Cmd);
    }
}

FBridgeResult UPixelStreamingHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("is_loaded"))           return Action_IsLoaded(Params);
    if (Action == TEXT("start_streaming"))     return Action_StartStreaming(Params);
    if (Action == TEXT("stop_streaming"))      return Action_StopStreaming(Params);
    if (Action == TEXT("set_target_bitrate"))  return Action_SetTargetBitrate(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("is_loaded, start_streaming, stop_streaming, set_target_bitrate"));
}

FBridgeResult UPixelStreamingHandler::Action_IsLoaded(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("loaded"), IsLoaded());
    return MakeSuccess(DOMAIN, TEXT("is_loaded"),
        IsLoaded() ? TEXT("PixelStreaming mounted") : TEXT("PixelStreaming not mounted"), Data);
}

FBridgeResult UPixelStreamingHandler::Action_StartStreaming(TSharedPtr<FJsonObject> Params)
{
    if (!IsLoaded())
        return MakeError(DOMAIN, TEXT("start_streaming"), 3003, TEXT("PixelStreaming plugin not mounted"));
    const bool bOk = RunCmd(TEXT("PixelStreaming.StartStreaming"));
    return MakeSuccess(DOMAIN, TEXT("start_streaming"),
        bOk ? TEXT("Stream start requested") : TEXT("Console exec returned false"));
}

FBridgeResult UPixelStreamingHandler::Action_StopStreaming(TSharedPtr<FJsonObject> Params)
{
    if (!IsLoaded())
        return MakeError(DOMAIN, TEXT("stop_streaming"), 3003, TEXT("PixelStreaming plugin not mounted"));
    const bool bOk = RunCmd(TEXT("PixelStreaming.StopStreaming"));
    return MakeSuccess(DOMAIN, TEXT("stop_streaming"),
        bOk ? TEXT("Stream stop requested") : TEXT("Console exec returned false"));
}

FBridgeResult UPixelStreamingHandler::Action_SetTargetBitrate(TSharedPtr<FJsonObject> Params)
{
    double Mbps = 0.0;
    if (!Params->TryGetNumberField(TEXT("mbps"), Mbps) || Mbps <= 0.0)
        return MakeError(DOMAIN, TEXT("set_target_bitrate"), 1000, TEXT("'mbps' (>0) is required"));
    const int32 BitsPerSecond = (int32)(Mbps * 1000000.0);
    const FString Cmd = FString::Printf(TEXT("PixelStreaming.WebRTC.MaxBitrate %d"), BitsPerSecond);
    const bool bOk = RunCmd(Cmd);
    return MakeSuccess(DOMAIN, TEXT("set_target_bitrate"),
        bOk ? FString::Printf(TEXT("Set bitrate to %.2f Mbps"), Mbps) : TEXT("Console exec returned false"));
}
