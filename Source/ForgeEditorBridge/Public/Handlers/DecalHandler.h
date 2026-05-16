#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "DecalHandler.generated.h"

/**
 * DecalHandler — domain "decal"  (UE 5.7)
 *
 * Spawn and configure ADecalActor instances. Generic actor verbs work but
 * miss the typed decal knobs (sort order, fade, screen size).
 *
 * Actions:
 *   spawn_decal           → material_path, location {x,y,z}, rotation? {pitch,yaw,roll},
 *                           size_x?, size_y?, size_z? (defaults 256/256/256),
 *                           label?, sort_order?, fade_in_duration?
 *   set_decal_size        → actor_label, size_x, size_y, size_z (full extent)
 *   set_decal_material    → actor_label, material_path
 *   set_sort_order        → actor_label, sort_order (int)
 *   set_fade_in           → actor_label, duration (sec), start_delay? (sec)
 *   set_fade_out          → actor_label, duration (sec), screen_size? (0..1)
 *   set_fade_screen_size  → actor_label, fade_screen_size (0..1)
 *   list_decals           → returns all ADecalActors in the current level
 */
UCLASS()
class FORGEEDITORBRIDGE_API UDecalHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("decal"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("spawn_decal"), TEXT("set_decal_size"), TEXT("set_decal_material"),
            TEXT("set_sort_order"), TEXT("set_fade_in"), TEXT("set_fade_out"),
            TEXT("set_fade_screen_size"), TEXT("list_decals")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_SpawnDecal          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetDecalSize        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetDecalMaterial    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetSortOrder        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetFadeIn           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetFadeOut          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetFadeScreenSize   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListDecals          (TSharedPtr<FJsonObject> Params);
};
