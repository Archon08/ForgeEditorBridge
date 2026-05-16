#include "Handlers/DDCHandler.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("ddc");

namespace
{
    bool RunCmd(const FString& Cmd)
    {
        if (!GEngine) return false;
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return GEngine->Exec(World, *Cmd);
    }
}

FBridgeResult UDDCHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("get_ddc_stats"))           return Action_GetStats(Params);
    if (Action == TEXT("refresh_shaders"))         return Action_RefreshShaders(Params);
    if (Action == TEXT("recompile_global_shaders")) return Action_RecompileGlobal(Params);
    if (Action == TEXT("trigger_resolve_dirty"))   return Action_TriggerResolveDirty(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("get_ddc_stats, refresh_shaders, recompile_global_shaders, trigger_resolve_dirty"));
}

FBridgeResult UDDCHandler::Action_GetStats(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunCmd(TEXT("DerivedDataCache.Stats"));
    return MakeSuccess(DOMAIN, TEXT("get_ddc_stats"),
        bOk ? TEXT("Stats dumped to log") : TEXT("Console exec returned false"));
}

FBridgeResult UDDCHandler::Action_RefreshShaders(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunCmd(TEXT("recompileshaders changed"));
    return MakeSuccess(DOMAIN, TEXT("refresh_shaders"),
        bOk ? TEXT("Recompile-changed shaders requested") : TEXT("Console exec returned false"));
}

FBridgeResult UDDCHandler::Action_RecompileGlobal(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunCmd(TEXT("recompileshaders global"));
    return MakeSuccess(DOMAIN, TEXT("recompile_global_shaders"),
        bOk ? TEXT("Recompile-global shaders requested") : TEXT("Console exec returned false"));
}

FBridgeResult UDDCHandler::Action_TriggerResolveDirty(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunCmd(TEXT("DerivedDataCache.MarkDirty"));
    return MakeSuccess(DOMAIN, TEXT("trigger_resolve_dirty"),
        bOk ? TEXT("DDC mark-dirty requested") : TEXT("Console exec returned false"));
}
