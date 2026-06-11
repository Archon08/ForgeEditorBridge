#include "Handlers/PIEHandler.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "LevelEditorSubsystem.h"
#include "UnrealEdMisc.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("pie");

namespace
{
    bool IsPIEWorldActive()
    {
        if (!GEditor) return false;
        for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
            {
                return true;
            }
        }
        return false;
    }

    UWorld* GetFirstPIEWorld()
    {
        if (!GEditor) return nullptr;
        for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
            {
                return Ctx.World();
            }
        }
        return nullptr;
    }

    int32 CountPIEWorlds()
    {
        if (!GEditor) return 0;
        int32 N = 0;
        for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE && Ctx.World()) ++N;
        }
        return N;
    }

    AActor* FindActorByLabelOrPath(UWorld* World, const FString& Selector)
    {
        if (!World || Selector.IsEmpty()) return nullptr;
        // Try as full path first
        if (AActor* Actor = FindObject<AActor>(nullptr, *Selector))
        {
            return Actor;
        }
        // Otherwise scan by label
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Selector)
            {
                return *It;
            }
        }
        return nullptr;
    }
}

FBridgeResult UPIEHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    }

    if (Action == TEXT("start_pie"))      return Action_StartPIE(Params);
    if (Action == TEXT("stop_pie"))       return Action_StopPIE(Params);
    if (Action == TEXT("pause_pie"))      return Action_PausePIE(Params);
    if (Action == TEXT("resume_pie"))     return Action_ResumePIE(Params);
    if (Action == TEXT("is_pie_active"))  return Action_IsPIEActive(Params);
    if (Action == TEXT("get_pie_info"))   return Action_GetPIEInfo(Params);
    if (Action == TEXT("pilot_actor"))    return Action_PilotActor(Params);
    if (Action == TEXT("eject_pilot"))    return Action_EjectPilot(Params);
    if (Action == TEXT("set_simulating")) return Action_SetSimulating(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("start_pie, stop_pie, pause_pie, resume_pie, is_pie_active, get_pie_info, pilot_actor, eject_pilot, set_simulating"));
}

FBridgeResult UPIEHandler::Action_StartPIE(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor)
    {
        return MakeError(DOMAIN, TEXT("start_pie"), 3000, TEXT("GEditor not available"));
    }

    if (IsPIEWorldActive())
    {
        return MakeSuccess(DOMAIN, TEXT("start_pie"),
            TEXT("PIE already active — call stop_pie first if you want to restart"));
    }

    FString Mode = TEXT("play");
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();

    ULevelEditorSubsystem* LE = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (!LE)
    {
        return MakeError(DOMAIN, TEXT("start_pie"), 3000, TEXT("ULevelEditorSubsystem unavailable"));
    }

    LE->EditorPlaySimulate();

    // Note: EditorPlaySimulate launches PIE/Simulate based on the current editor mode toggle.
    // Use set_simulating to flip the mode before this call if needed.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("mode_requested"), Mode);
    Data->SetBoolField(TEXT("requested"), true);
    return MakeSuccess(DOMAIN, TEXT("start_pie"),
        FString::Printf(TEXT("PIE start requested (mode=%s)"), *Mode), Data);
}

FBridgeResult UPIEHandler::Action_StopPIE(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor)
    {
        return MakeError(DOMAIN, TEXT("stop_pie"), 3000, TEXT("GEditor not available"));
    }

    if (!IsPIEWorldActive())
    {
        return MakeSuccess(DOMAIN, TEXT("stop_pie"), TEXT("No active PIE — nothing to stop"));
    }

    if (ULevelEditorSubsystem* LE = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
    {
        LE->EditorRequestEndPlay();
    }
    else
    {
        GEditor->RequestEndPlayMap();
    }

    return MakeSuccess(DOMAIN, TEXT("stop_pie"), TEXT("PIE stop requested"));
}

FBridgeResult UPIEHandler::Action_PausePIE(TSharedPtr<FJsonObject> Params)
{
    UWorld* World = GetFirstPIEWorld();
    if (!World)
    {
        return MakeError(DOMAIN, TEXT("pause_pie"), 3004, TEXT("No active PIE"));
    }

    int32 Paused = 0;
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        if (APlayerController* PC = It->Get())
        {
            PC->SetPause(true);
            ++Paused;
        }
    }

    return MakeSuccess(DOMAIN, TEXT("pause_pie"),
        FString::Printf(TEXT("Paused %d PlayerController(s)"), Paused));
}

FBridgeResult UPIEHandler::Action_ResumePIE(TSharedPtr<FJsonObject> Params)
{
    UWorld* World = GetFirstPIEWorld();
    if (!World)
    {
        return MakeError(DOMAIN, TEXT("resume_pie"), 3004, TEXT("No active PIE"));
    }

    int32 Resumed = 0;
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        if (APlayerController* PC = It->Get())
        {
            PC->SetPause(false);
            ++Resumed;
        }
    }

    return MakeSuccess(DOMAIN, TEXT("resume_pie"),
        FString::Printf(TEXT("Resumed %d PlayerController(s)"), Resumed));
}

FBridgeResult UPIEHandler::Action_IsPIEActive(TSharedPtr<FJsonObject> Params)
{
    const bool bActive = IsPIEWorldActive();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("active"), bActive);
    Data->SetNumberField(TEXT("world_count"), CountPIEWorlds());
    return MakeSuccess(DOMAIN, TEXT("is_pie_active"),
        bActive ? TEXT("PIE active") : TEXT("PIE not active"), Data);
}

FBridgeResult UPIEHandler::Action_GetPIEInfo(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    const bool bActive = IsPIEWorldActive();
    Data->SetBoolField(TEXT("active"), bActive);
    Data->SetNumberField(TEXT("num_pie_worlds"), CountPIEWorlds());

    int32 NumClients = 0;
    if (UWorld* World = GetFirstPIEWorld())
    {
        for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
        {
            if (It->Get()) ++NumClients;
        }
        Data->SetStringField(TEXT("map_name"), World->GetMapName());
    }
    Data->SetNumberField(TEXT("num_clients"), NumClients);
    Data->SetBoolField(TEXT("simulating"), GEditor ? GEditor->bIsSimulatingInEditor : false);

    return MakeSuccess(DOMAIN, TEXT("get_pie_info"), TEXT("PIE info"), Data);
}

FBridgeResult UPIEHandler::Action_PilotActor(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor)
    {
        return MakeError(DOMAIN, TEXT("pilot_actor"), 3000, TEXT("GEditor not available"));
    }

    FString Selector;
    if (!Params->TryGetStringField(TEXT("actor_label"), Selector))
    {
        Params->TryGetStringField(TEXT("actor_path"), Selector);
    }
    if (Selector.IsEmpty())
    {
        return MakeError(DOMAIN, TEXT("pilot_actor"), 1000,
            TEXT("'actor_label' or 'actor_path' is required"));
    }

    UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
    AActor* Target = FindActorByLabelOrPath(World, Selector);
    if (!Target)
    {
        return MakeError(DOMAIN, TEXT("pilot_actor"), 2000,
            FString::Printf(TEXT("Actor not found: %s"), *Selector),
            TEXT("Use actor/find_actors_by_name to discover labels"));
    }

    ULevelEditorSubsystem* LE = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (!LE)
    {
        return MakeError(DOMAIN, TEXT("pilot_actor"), 3000, TEXT("ULevelEditorSubsystem unavailable"));
    }
    LE->PilotLevelActor(Target);

    return MakeSuccess(DOMAIN, TEXT("pilot_actor"),
        FString::Printf(TEXT("Piloting actor '%s'"), *Target->GetActorLabel()));
}

FBridgeResult UPIEHandler::Action_EjectPilot(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor)
    {
        return MakeError(DOMAIN, TEXT("eject_pilot"), 3000, TEXT("GEditor not available"));
    }
    ULevelEditorSubsystem* LE = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (!LE)
    {
        return MakeError(DOMAIN, TEXT("eject_pilot"), 3000, TEXT("ULevelEditorSubsystem unavailable"));
    }
    LE->EjectPilotLevelActor();
    return MakeSuccess(DOMAIN, TEXT("eject_pilot"), TEXT("Ejected pilot"));
}

FBridgeResult UPIEHandler::Action_SetSimulating(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor)
    {
        return MakeError(DOMAIN, TEXT("set_simulating"), 3000, TEXT("GEditor not available"));
    }
    bool bSimulating = false;
    if (!Params->TryGetBoolField(TEXT("simulating"), bSimulating))
    {
        return MakeError(DOMAIN, TEXT("set_simulating"), 1000,
            TEXT("'simulating' (bool) is required"));
    }

    // If PIE is running and mode mismatches, request a clean stop. Caller should restart.
    if (IsPIEWorldActive() && GEditor->bIsSimulatingInEditor != bSimulating)
    {
        GEditor->RequestEndPlayMap();
    }
    GEditor->bIsSimulatingInEditor = bSimulating;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("simulating"), bSimulating);
    return MakeSuccess(DOMAIN, TEXT("set_simulating"),
        FString::Printf(TEXT("Simulate mode = %s"), bSimulating ? TEXT("true") : TEXT("false")),
        Data);
}