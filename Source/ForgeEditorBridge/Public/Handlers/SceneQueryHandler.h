#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "SceneQueryHandler.generated.h"

/**
 * SceneQueryHandler — domain "scene_query"  (v0.1.0 / UE 5.7)
 *
 * Read-only spatial queries against the current editor world.
 * No writes, no transactions, no world mutation.
 *
 * Actions:
 *   find_actors        → class_filter, name_filter, tag_filter (all optional)
 *                        → [{label, class, location{x,y,z}}]
 *   get_actor_details  → actor_label (required)
 *                        → transform, components[{name,class}], tags, bHidden, bCastShadow
 *   get_component_tree → actor_label (required)
 *                        → root scene-component tree with types + relative transforms,
 *                          plus flat list of non-scene components
 *   get_scene_bounds   → (no params)
 *                        → {min,max,center,extent}, actor_count, static_mesh_count
 *   get_actors_in_radius → center{x,y,z}, radius
 *                          → [{label, class, distance}] sorted nearest first
 *   raycast            → start{x,y,z}, end{x,y,z}, channel (optional, default "Visibility")
 *                        → {hit, actor_label, component, location, normal, distance}
 *   overlap_query      → center{x,y,z}, extent{x,y,z}
 *                        → [{actor_label, class}] whose bounds intersect the box
 */
UCLASS()
class FORGEEDITORBRIDGE_API USceneQueryHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("scene_query"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("find_actors"),
			TEXT("get_actor_details"),
			TEXT("get_component_tree"),
			TEXT("get_scene_bounds"),
			TEXT("get_actors_in_radius"),
			TEXT("raycast"),
			TEXT("overlap_query"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_FindActors       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetActorDetails  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetComponentTree (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetSceneBounds   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetActorsInRadius(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Raycast          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_OverlapQuery     (TSharedPtr<FJsonObject> Params);

	static UWorld* GetEditorWorld();
};
