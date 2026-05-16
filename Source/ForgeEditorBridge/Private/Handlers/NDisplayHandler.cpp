#include "Handlers/NDisplayHandler.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("ndisplay");

namespace
{
    bool IsLoaded()
    {
        return FModuleManager::Get().IsModuleLoaded(TEXT("DisplayCluster"));
    }
    bool RunCmd(const FString& Cmd)
    {
        if (!GEngine) return false;
        UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return GEngine->Exec(W, *Cmd);
    }
}

FBridgeResult UNDisplayHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("is_ndisplay_loaded")) return Action_IsLoaded(Params);
    if (Action == TEXT("get_runtime_state"))  return Action_GetRuntimeState(Params);
    if (Action == TEXT("exec_cluster_event")) return Action_ExecClusterEvent(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("is_ndisplay_loaded, get_runtime_state, exec_cluster_event"));
}

FBridgeResult UNDisplayHandler::Action_IsLoaded(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("loaded"), IsLoaded());
    return MakeSuccess(DOMAIN, TEXT("is_ndisplay_loaded"),
        IsLoaded() ? TEXT("DisplayCluster mounted") : TEXT("DisplayCluster not mounted"), Data);
}

FBridgeResult UNDisplayHandler::Action_GetRuntimeState(TSharedPtr<FJsonObject> Params)
{
    if (!IsLoaded())
        return MakeError(DOMAIN, TEXT("get_runtime_state"), 3003, TEXT("DisplayCluster not loaded"));
    return MakeError(DOMAIN, TEXT("get_runtime_state"), 3003,
        TEXT("Runtime state requires IDisplayCluster which is not in bridge dependencies"),
        TEXT("Use console commands like nDisplay.cluster.eventjson or run via PythonHandler with unreal.DisplayClusterRootActor"));
}

FBridgeResult UNDisplayHandler::Action_ExecClusterEvent(TSharedPtr<FJsonObject> Params)
{
    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
        return MakeError(DOMAIN, TEXT("exec_cluster_event"), 1000, TEXT("'event_name' is required"));
    const FString Cmd = FString::Printf(TEXT("nDisplay.cluster.eventjson %s"), *EventName);
    const bool bOk = RunCmd(Cmd);
    return MakeSuccess(DOMAIN, TEXT("exec_cluster_event"),
        bOk ? FString::Printf(TEXT("Cluster event '%s' issued"), *EventName) : TEXT("Console exec returned false"));
}
