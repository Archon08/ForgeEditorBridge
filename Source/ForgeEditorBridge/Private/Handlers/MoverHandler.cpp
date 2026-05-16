#include "Handlers/MoverHandler.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "UObject/Class.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("mover");

namespace
{
    bool IsMoverMounted()
    {
        return FModuleManager::Get().IsModuleLoaded(TEXT("Mover"));
    }

    UClass* FindMoverComponentClass()
    {
        return FindObject<UClass>(nullptr, TEXT("/Script/Mover.MoverComponent"));
    }

    UWorld* GetEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    AActor* FindActorByLabel(UWorld* World, const FString& Label)
    {
        if (!World) return nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Label) return *It;
        }
        return nullptr;
    }
}

FBridgeResult UMoverHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("is_mover_loaded"))    return Action_IsMoverLoaded(Params);
    if (Action == TEXT("list_mover_actors"))  return Action_ListMoverActors(Params);
    if (Action == TEXT("get_mover_info"))     return Action_GetMoverInfo(Params);
    if (Action == TEXT("set_movement_mode"))  return Action_SetMovementMode(Params);
    if (Action == TEXT("add_movement_mode"))  return Action_AddMovementMode(Params);
    if (Action == TEXT("remove_movement_mode")) return Action_RemoveMovementMode(Params);
    if (Action == TEXT("list_movement_modes")) return Action_ListMovementModes(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("is_mover_loaded, list_mover_actors, get_mover_info, set_movement_mode, add_movement_mode, remove_movement_mode, list_movement_modes"));
}

FBridgeResult UMoverHandler::Action_IsMoverLoaded(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("loaded"), IsMoverMounted());
    Data->SetBoolField(TEXT("component_class_resolvable"), FindMoverComponentClass() != nullptr);
    return MakeSuccess(DOMAIN, TEXT("is_mover_loaded"),
        IsMoverMounted() ? TEXT("Mover plugin mounted") : TEXT("Mover plugin not mounted"), Data);
}

FBridgeResult UMoverHandler::Action_ListMoverActors(TSharedPtr<FJsonObject> Params)
{
    UClass* MoverCls = FindMoverComponentClass();
    if (!MoverCls)
        return MakeError(DOMAIN, TEXT("list_mover_actors"), 3003,
            TEXT("UMoverComponent class not resolvable"),
            TEXT("Enable the Mover plugin in Plugins > Experimental > Mover"));

    UWorld* World = GetEditorWorld();
    if (!World) return MakeError(DOMAIN, TEXT("list_mover_actors"), 3000, TEXT("No editor world"));

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->FindComponentByClass(MoverCls))
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("label"), It->GetActorLabel());
            O->SetStringField(TEXT("path"),  It->GetPathName());
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("actors"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_mover_actors"),
        FString::Printf(TEXT("%d actor(s) with UMoverComponent"), Arr.Num()), Data);
}

FBridgeResult UMoverHandler::Action_GetMoverInfo(TSharedPtr<FJsonObject> Params)
{
    FString Label;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_mover_info"), 1000, TEXT("'actor_label' is required"));

    UClass* MoverCls = FindMoverComponentClass();
    if (!MoverCls) return MakeError(DOMAIN, TEXT("get_mover_info"), 3003, TEXT("Mover plugin unavailable"));

    AActor* Actor = FindActorByLabel(GetEditorWorld(), Label);
    if (!Actor) return MakeError(DOMAIN, TEXT("get_mover_info"), 2000, TEXT("Actor not found"));
    UActorComponent* C = Actor->FindComponentByClass(MoverCls);
    if (!C) return MakeError(DOMAIN, TEXT("get_mover_info"), 2001, TEXT("Actor has no UMoverComponent"));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Label);
    Data->SetStringField(TEXT("component_class"), C->GetClass()->GetName());
    Data->SetStringField(TEXT("note"),
        TEXT("Property reads/writes use asset/set_prop or a Python escape — Mover's per-mode struct layout is not bridged directly"));
    return MakeSuccess(DOMAIN, TEXT("get_mover_info"), Label, Data);
}

// ===========================================================================
// Wave 9: Mover real impls (UMoverComponent::QueueNextMode + AddMovementModeFromClass)
// ===========================================================================

#include "MoverComponent.h"
// DefaultMovementSet/CharacterMoverComponent.h not available; UMoverComponent is enough

namespace
{
    UMoverComponent* GetMoverOnActor(AActor* Actor)
    {
        if (!Actor) return nullptr;
        return Actor->FindComponentByClass<UMoverComponent>();
    }
}

FBridgeResult UMoverHandler::Action_SetMovementMode(TSharedPtr<FJsonObject> Params)
{
    FString Label, ModeName;
    bool bShouldReenter = false;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_movement_mode"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetStringField(TEXT("mode_name"), ModeName) || ModeName.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_movement_mode"), 1000, TEXT("'mode_name' is required"));
    Params->TryGetBoolField(TEXT("should_reenter"), bShouldReenter);

    AActor* Actor = FindActorByLabel(GetEditorWorld(), Label);
    if (!Actor) return MakeError(DOMAIN, TEXT("set_movement_mode"), 2000, TEXT("Actor not found"));
    UMoverComponent* Mover = GetMoverOnActor(Actor);
    if (!Mover) return MakeError(DOMAIN, TEXT("set_movement_mode"), 2001, TEXT("Actor has no UMoverComponent"));

    Mover->QueueNextMode(FName(*ModeName), bShouldReenter);
    return MakeSuccess(DOMAIN, TEXT("set_movement_mode"),
        FString::Printf(TEXT("Queued '%s' on '%s' (reenter=%s)"),
            *ModeName, *Label, bShouldReenter ? TEXT("true") : TEXT("false")));
}

FBridgeResult UMoverHandler::Action_AddMovementMode(TSharedPtr<FJsonObject> Params)
{
    FString Label, ModeName, ClassPath;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_movement_mode"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetStringField(TEXT("mode_name"), ModeName) || ModeName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_movement_mode"), 1000, TEXT("'mode_name' is required"));
    if (!Params->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_movement_mode"), 1000,
            TEXT("'class_path' is required (UBaseMovementMode subclass path)"));

    AActor* Actor = FindActorByLabel(GetEditorWorld(), Label);
    if (!Actor) return MakeError(DOMAIN, TEXT("add_movement_mode"), 2000, TEXT("Actor not found"));
    UMoverComponent* Mover = GetMoverOnActor(Actor);
    if (!Mover) return MakeError(DOMAIN, TEXT("add_movement_mode"), 2001, TEXT("Actor has no UMoverComponent"));

    UClass* ModeClass = LoadClass<UBaseMovementMode>(nullptr, *ClassPath);
    if (!ModeClass) return MakeError(DOMAIN, TEXT("add_movement_mode"), 2000,
        FString::Printf(TEXT("Class not found or not a UBaseMovementMode: %s"), *ClassPath));

    UBaseMovementMode* Mode = Mover->AddMovementModeFromClass(FName(*ModeName), ModeClass);
    if (!Mode)
        return MakeError(DOMAIN, TEXT("add_movement_mode"), 3000, TEXT("AddMovementModeFromClass returned null"));
    return MakeSuccess(DOMAIN, TEXT("add_movement_mode"),
        FString::Printf(TEXT("Added mode '%s' (%s) on '%s'"), *ModeName, *ModeClass->GetName(), *Label));
}

FBridgeResult UMoverHandler::Action_RemoveMovementMode(TSharedPtr<FJsonObject> Params)
{
    FString Label, ModeName;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_movement_mode"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetStringField(TEXT("mode_name"), ModeName) || ModeName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_movement_mode"), 1000, TEXT("'mode_name' is required"));
    AActor* Actor = FindActorByLabel(GetEditorWorld(), Label);
    if (!Actor) return MakeError(DOMAIN, TEXT("remove_movement_mode"), 2000, TEXT("Actor not found"));
    UMoverComponent* Mover = GetMoverOnActor(Actor);
    if (!Mover) return MakeError(DOMAIN, TEXT("remove_movement_mode"), 2001, TEXT("Actor has no UMoverComponent"));

    const bool bRemoved = Mover->RemoveMovementMode(FName(*ModeName));
    return bRemoved
        ? MakeSuccess(DOMAIN, TEXT("remove_movement_mode"),
            FString::Printf(TEXT("Removed mode '%s' from '%s'"), *ModeName, *Label))
        : MakeError(DOMAIN, TEXT("remove_movement_mode"), 2000,
            FString::Printf(TEXT("Mode '%s' not present on '%s'"), *ModeName, *Label));
}

FBridgeResult UMoverHandler::Action_ListMovementModes(TSharedPtr<FJsonObject> Params)
{
    FString Label;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_movement_modes"), 1000, TEXT("'actor_label' is required"));
    AActor* Actor = FindActorByLabel(GetEditorWorld(), Label);
    if (!Actor) return MakeError(DOMAIN, TEXT("list_movement_modes"), 2000, TEXT("Actor not found"));
    UMoverComponent* Mover = GetMoverOnActor(Actor);
    if (!Mover) return MakeError(DOMAIN, TEXT("list_movement_modes"), 2001, TEXT("Actor has no UMoverComponent"));

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FName, TObjectPtr<UBaseMovementMode>>& Pair : Mover->MovementModes)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), Pair.Key.ToString());
        if (Pair.Value)
        {
            O->SetStringField(TEXT("class"), Pair.Value->GetClass()->GetName());
        }
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("modes"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Data->SetStringField(TEXT("active_mode"), Mover->GetMovementModeName().ToString());
    return MakeSuccess(DOMAIN, TEXT("list_movement_modes"),
        FString::Printf(TEXT("%d mode(s) on '%s'"), Arr.Num(), *Label), Data);
}
