#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "MoverHandler.generated.h"

/**
 * MoverHandler — domain "mover"  (UE 5.7, EXPERIMENTAL)
 *
 * Mover is the experimental UE 5.7 replacement for CharacterMovementComponent.
 * The plugin's API is in flux; this handler exposes safe runtime queries and
 * defers complex authoring to reflection on UMoverComponent.
 *
 * Actions:
 *   is_mover_loaded     → returns whether the Mover plugin is mounted
 *   list_mover_actors   → enumerates actors with UMoverComponent in current level
 *   get_mover_info      → actor_label → returns MovementMode + mover_class via reflection
 *   set_movement_mode   → actor_label, mode_name (depends on mover ruleset) via reflection
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMoverHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("mover"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("is_mover_loaded"), TEXT("list_mover_actors"),
            TEXT("get_mover_info"), TEXT("set_movement_mode"),
            TEXT("add_movement_mode"), TEXT("remove_movement_mode"),
            TEXT("list_movement_modes")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_IsMoverLoaded     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListMoverActors   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetMoverInfo      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetMovementMode   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddMovementMode   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveMovementMode(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListMovementModes (TSharedPtr<FJsonObject> Params);
};
