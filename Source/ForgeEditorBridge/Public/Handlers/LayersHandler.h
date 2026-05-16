#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "LayersHandler.generated.h"

/**
 * LayersHandler — domain "layers"  (UE 5.7)
 *
 * Editor Layers (the legacy "Layers" panel in UE) — for organizing actors into
 * named groups with toggleable visibility.
 *
 * Actions:
 *   create_layer            → layer_name
 *   delete_layer            → layer_name
 *   rename_layer            → old_name, new_name
 *   add_actor_to_layer      → actor_label or actor_path, layer_name
 *   add_actors_to_layer     → actor_labels (string[]) or actor_paths (string[]), layer_name
 *   remove_actor_from_layer → actor_label or actor_path, layer_name
 *   set_layer_visibility    → layer_name, visible (bool)
 *   list_layers             → returns array of {name, visible, actor_count}
 *   get_actors_for_layer    → layer_name → returns array of actor labels/paths
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULayersBridgeHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("layers"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_layer"), TEXT("delete_layer"), TEXT("rename_layer"),
            TEXT("add_actor_to_layer"), TEXT("add_actors_to_layer"),
            TEXT("remove_actor_from_layer"), TEXT("set_layer_visibility"),
            TEXT("list_layers"), TEXT("get_actors_for_layer")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateLayer         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DeleteLayer         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RenameLayer         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddActorToLayer     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddActorsToLayer    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveActorFromLayer(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetLayerVisibility  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListLayers          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetActorsForLayer   (TSharedPtr<FJsonObject> Params);
};