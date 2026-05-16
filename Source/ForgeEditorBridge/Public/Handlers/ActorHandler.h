#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ActorHandler.generated.h"

/**
 * ActorHandler — domain "actor"
 *
 * Level/world actor operations.
 * Note: delete_actor routes to QuarantineHandler (never directly destroys).
 *
 * Actions (Phase 1):
 *   spawn_actor              → class_name, location{x,y,z}, rotation{pitch,yaw,roll}?, label?, static_mesh?
 *   set_actor_transform      → actor_label, location?, rotation?, scale?,
 *                              rotation_from_normal{x,y,z}?, rotation_from_normal_preserve_yaw?
 *   find_actors_by_name      → name_pattern → JSON array of matching actors
 *   get_actors_in_level      → returns all actors in the current level
 *   add_component_to_blueprint → blueprint_path, component_class, component_name
 *   spawn_blueprint_actor    → blueprint_path, location?, rotation?
 *
 * Actions (Phase 3):
 *   get_actor_transform      → actor_path|actor_label → location/rotation/scale
 *   get_actor_properties     → actor_path, properties[] → reflected property values
 *   set_actor_property       → actor_path, property, value → reflected property write (transacted)
 *   attach_to                → child_path, parent_path, socket?, rule?
 *   detach                   → actor_path, rule?
 *   set_mobility             → actor_path, mobility (Static|Stationary|Movable)
 *   set_tag                  → actor_path, tag, add?
 *   get_selected_actors      → returns editor selection
 *   select_actors            → actor_paths[], deselect_others?
 *   focus_actor              → actor_path → moves viewport cameras to actor
 *
 * Actions (Phase 1d):
 *   set_lod_settings         → actor_label, lod_index (int), screen_size (float)
 *   find_actors_of_class     → class_name (string) → array of {label, path, location}
 *   set_actor_label          → actor_label (current), new_label
 *   duplicate                → actor_label, offset? ("X,Y,Z", default "100,0,0")
 *   replace_class            → actor_label, new_class — returns error_code 3003 (not feasible in UE 5.7 without factory)
 *   set_replication_settings → actor_label, replicates (bool), net_update_frequency (float),
 *                              always_relevant (bool), net_priority (float)
 *
 * Actions (Phase 4):
 *   spawn_with_retry         → asset_path, location, rotation?, retry_offset_range?,
 *                              max_retries?, collision_box_extent? — spawn with overlap-avoidance
 */
UCLASS()
class FORGEEDITORBRIDGE_API UActorHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("actor"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("spawn_actor"), TEXT("set_actor_transform"), TEXT("find_actors_by_name"),
			TEXT("get_actors_in_level"), TEXT("add_component_to_blueprint"), TEXT("spawn_blueprint_actor"),
			// Phase 3
			TEXT("get_actor_transform"), TEXT("get_actor_properties"), TEXT("set_actor_property"),
			TEXT("attach_to"), TEXT("detach"), TEXT("set_mobility"), TEXT("set_tag"),
			TEXT("get_selected_actors"), TEXT("select_actors"), TEXT("focus_actor"),
			// Phase 1d
			TEXT("set_lod_settings"), TEXT("find_actors_of_class"), TEXT("set_actor_label"),
			TEXT("duplicate"), TEXT("replace_class"), TEXT("set_replication_settings"),
			// Phase 4
			TEXT("spawn_with_retry"),
			// Destruction (UE 5.7 gap closure)
			TEXT("destroy_actor"), TEXT("destroy_actors")
		};
	}
    virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
