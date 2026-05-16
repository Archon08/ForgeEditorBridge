#include "Handlers/LayersHandler.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Layers/LayersSubsystem.h"
#include "Layers/Layer.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("layers");

namespace
{
    ULayersSubsystem* GetLayersSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<ULayersSubsystem>() : nullptr;
    }

    AActor* FindActorByLabelOrPath(UWorld* World, const FString& Selector)
    {
        if (!World || Selector.IsEmpty()) return nullptr;
        if (AActor* Actor = FindObject<AActor>(nullptr, *Selector)) return Actor;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Selector) return *It;
        }
        return nullptr;
    }
}

FBridgeResult ULayersBridgeHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_layer"))            return Action_CreateLayer(Params);
    if (Action == TEXT("delete_layer"))            return Action_DeleteLayer(Params);
    if (Action == TEXT("rename_layer"))            return Action_RenameLayer(Params);
    if (Action == TEXT("add_actor_to_layer"))      return Action_AddActorToLayer(Params);
    if (Action == TEXT("add_actors_to_layer"))     return Action_AddActorsToLayer(Params);
    if (Action == TEXT("remove_actor_from_layer")) return Action_RemoveActorFromLayer(Params);
    if (Action == TEXT("set_layer_visibility"))    return Action_SetLayerVisibility(Params);
    if (Action == TEXT("list_layers"))             return Action_ListLayers(Params);
    if (Action == TEXT("get_actors_for_layer"))    return Action_GetActorsForLayer(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_layer, delete_layer, rename_layer, add_actor_to_layer, add_actors_to_layer, remove_actor_from_layer, set_layer_visibility, list_layers, get_actors_for_layer"));
}

FBridgeResult ULayersBridgeHandler::Action_CreateLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Create Layer"));
    FString LayerName;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_layer"), 1000, TEXT("'layer_name' is required"));
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("create_layer"), 3000, TEXT("ULayersSubsystem unavailable"));

    const FName Name(*LayerName);
    if (LS->GetLayer(Name))
        return MakeSuccess(DOMAIN, TEXT("create_layer"),
            FString::Printf(TEXT("Layer '%s' already exists"), *LayerName));
    LS->CreateLayer(Name);
    return MakeSuccess(DOMAIN, TEXT("create_layer"),
        FString::Printf(TEXT("Created layer '%s'"), *LayerName));
}

FBridgeResult ULayersBridgeHandler::Action_DeleteLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Delete Layer"));
    FString LayerName;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("delete_layer"), 1000, TEXT("'layer_name' is required"));
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("delete_layer"), 3000, TEXT("ULayersSubsystem unavailable"));

    TArray<FName> Names; Names.Add(FName(*LayerName));
    LS->DeleteLayers(Names);
    return MakeSuccess(DOMAIN, TEXT("delete_layer"),
        FString::Printf(TEXT("Deleted layer '%s'"), *LayerName));
}

FBridgeResult ULayersBridgeHandler::Action_RenameLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Rename Layer"));
    FString OldName, NewName;
    if (!Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
        return MakeError(DOMAIN, TEXT("rename_layer"), 1000, TEXT("'old_name' is required"));
    if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
        return MakeError(DOMAIN, TEXT("rename_layer"), 1000, TEXT("'new_name' is required"));
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("rename_layer"), 3000, TEXT("ULayersSubsystem unavailable"));
    LS->RenameLayer(FName(*OldName), FName(*NewName));
    return MakeSuccess(DOMAIN, TEXT("rename_layer"),
        FString::Printf(TEXT("Renamed '%s' -> '%s'"), *OldName, *NewName));
}

FBridgeResult ULayersBridgeHandler::Action_AddActorToLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Add Actor To Layer"));
    FString Selector, LayerName;
    if (!Params->TryGetStringField(TEXT("actor_label"), Selector))
        Params->TryGetStringField(TEXT("actor_path"), Selector);
    if (Selector.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_actor_to_layer"), 1000,
            TEXT("'actor_label' or 'actor_path' is required"));
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_actor_to_layer"), 1000, TEXT("'layer_name' is required"));

    ULayersSubsystem* LS = GetLayersSubsystem();
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!LS || !World) return MakeError(DOMAIN, TEXT("add_actor_to_layer"), 3000, TEXT("Layers subsystem or world unavailable"));

    AActor* A = FindActorByLabelOrPath(World, Selector);
    if (!A) return MakeError(DOMAIN, TEXT("add_actor_to_layer"), 2000,
        FString::Printf(TEXT("Actor not found: %s"), *Selector));

    LS->AddActorToLayer(A, FName(*LayerName));
    return MakeSuccess(DOMAIN, TEXT("add_actor_to_layer"),
        FString::Printf(TEXT("Added '%s' to layer '%s'"), *A->GetActorLabel(), *LayerName));
}

FBridgeResult ULayersBridgeHandler::Action_AddActorsToLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Add Actors To Layer"));
    FString LayerName;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_actors_to_layer"), 1000, TEXT("'layer_name' is required"));
    const TArray<TSharedPtr<FJsonValue>>* LabelsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_labels"), LabelsArr))
        Params->TryGetArrayField(TEXT("actor_paths"), LabelsArr);
    if (!LabelsArr)
        return MakeError(DOMAIN, TEXT("add_actors_to_layer"), 1000,
            TEXT("'actor_labels' or 'actor_paths' (array) is required"));

    ULayersSubsystem* LS = GetLayersSubsystem();
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!LS || !World) return MakeError(DOMAIN, TEXT("add_actors_to_layer"), 3000, TEXT("Layers subsystem or world unavailable"));

    TArray<TWeakObjectPtr<AActor>> Actors;
    for (const TSharedPtr<FJsonValue>& V : *LabelsArr)
    {
        const FString S = V->AsString();
        if (AActor* A = FindActorByLabelOrPath(World, S)) Actors.Add(A);
    }
    LS->AddActorsToLayer(Actors, FName(*LayerName));
    return MakeSuccess(DOMAIN, TEXT("add_actors_to_layer"),
        FString::Printf(TEXT("Added %d actor(s) to '%s'"), Actors.Num(), *LayerName));
}

FBridgeResult ULayersBridgeHandler::Action_RemoveActorFromLayer(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Remove Actor From Layer"));
    FString Selector, LayerName;
    if (!Params->TryGetStringField(TEXT("actor_label"), Selector))
        Params->TryGetStringField(TEXT("actor_path"), Selector);
    if (Selector.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_actor_from_layer"), 1000,
            TEXT("'actor_label' or 'actor_path' is required"));
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_actor_from_layer"), 1000, TEXT("'layer_name' is required"));

    ULayersSubsystem* LS = GetLayersSubsystem();
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!LS || !World) return MakeError(DOMAIN, TEXT("remove_actor_from_layer"), 3000, TEXT("Layers subsystem or world unavailable"));

    AActor* A = FindActorByLabelOrPath(World, Selector);
    if (!A) return MakeError(DOMAIN, TEXT("remove_actor_from_layer"), 2000,
        FString::Printf(TEXT("Actor not found: %s"), *Selector));

    LS->RemoveActorFromLayer(A, FName(*LayerName));
    return MakeSuccess(DOMAIN, TEXT("remove_actor_from_layer"),
        FString::Printf(TEXT("Removed '%s' from '%s'"), *A->GetActorLabel(), *LayerName));
}

FBridgeResult ULayersBridgeHandler::Action_SetLayerVisibility(TSharedPtr<FJsonObject> Params)
{
    FString LayerName;
    bool bVisible = true;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_layer_visibility"), 1000, TEXT("'layer_name' is required"));
    if (!Params->TryGetBoolField(TEXT("visible"), bVisible))
        return MakeError(DOMAIN, TEXT("set_layer_visibility"), 1000, TEXT("'visible' (bool) is required"));
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("set_layer_visibility"), 3000, TEXT("ULayersSubsystem unavailable"));
    LS->SetLayerVisibility(FName(*LayerName), bVisible);
    return MakeSuccess(DOMAIN, TEXT("set_layer_visibility"),
        FString::Printf(TEXT("Layer '%s' visible=%s"), *LayerName, bVisible ? TEXT("true") : TEXT("false")));
}

FBridgeResult ULayersBridgeHandler::Action_ListLayers(TSharedPtr<FJsonObject> Params)
{
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("list_layers"), 3000, TEXT("ULayersSubsystem unavailable"));

    // 5.7: AddAllLayerNamesTo writes FName entries; resolve each to its ULayer for visibility.
    TArray<FName> LayerNames;
    LS->AddAllLayerNamesTo(LayerNames);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FName& LayerName : LayerNames)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), LayerName.ToString());
        if (ULayer* L = LS->GetLayer(LayerName))
        {
            Entry->SetBoolField(TEXT("visible"), L->IsVisible());
        }
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("layers"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_layers"),
        FString::Printf(TEXT("%d layer(s)"), Arr.Num()), Data);
}

FBridgeResult ULayersBridgeHandler::Action_GetActorsForLayer(TSharedPtr<FJsonObject> Params)
{
    FString LayerName;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_actors_for_layer"), 1000, TEXT("'layer_name' is required"));
    ULayersSubsystem* LS = GetLayersSubsystem();
    if (!LS) return MakeError(DOMAIN, TEXT("get_actors_for_layer"), 3000, TEXT("ULayersSubsystem unavailable"));

    TArray<TWeakObjectPtr<AActor>> Actors;
    LS->AppendActorsFromLayer(FName(*LayerName), Actors);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TWeakObjectPtr<AActor>& W : Actors)
    {
        AActor* A = W.Get();
        if (!A) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("label"), A->GetActorLabel());
        Entry->SetStringField(TEXT("path"), A->GetPathName());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("actors"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Data->SetStringField(TEXT("layer_name"), LayerName);
    return MakeSuccess(DOMAIN, TEXT("get_actors_for_layer"),
        FString::Printf(TEXT("%d actor(s) in layer '%s'"), Arr.Num(), *LayerName), Data);
}