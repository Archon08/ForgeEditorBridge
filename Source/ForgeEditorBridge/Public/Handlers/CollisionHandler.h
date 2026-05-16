#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "CollisionHandler.generated.h"

/**
 * CollisionHandler — domain "collision" (v2.0.0 / UE 5.7)
 *
 * Manage project collision channels, profiles, and per-actor collision settings.
 *
 * Actions:
 *   list_collision_channels   → (no params) — returns custom channel entries from DefaultEngine.ini.
 *
 *   add_collision_channel     → name (string), default_response (Block|Overlap|Ignore, opt = Block)
 *                               Appends a new GameTraceChannel entry to DefaultEngine.ini.
 *
 *   list_collision_profiles   → (no params) — returns all profile names via UCollisionProfile.
 *
 *   create_collision_profile  → name (string), default_response (Block|Overlap|Ignore, opt = Block)
 *                               Appends a new profile entry to DefaultEngine.ini.
 *
 *   set_collision_response    → profile (string), channel (string), response (Block|Overlap|Ignore)
 *                               Guidance dispatch — profile struct modification requires ini editing.
 *
 *   set_actor_collision       → actor_label (string), profile (string), component (string, opt)
 *                               Apply named collision profile to actor's root (or named) component.
 *
 *   get_actor_collision       → actor_label (string) — read collision profile + enabled state from actor.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UCollisionHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("collision"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("list_collision_channels"),
			TEXT("add_collision_channel"),
			TEXT("list_collision_profiles"),
			TEXT("create_collision_profile"),
			TEXT("set_collision_response"),
			TEXT("set_actor_collision"),
			TEXT("get_actor_collision"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_ListCollisionChannels  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddCollisionChannel    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListCollisionProfiles  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateCollisionProfile (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetCollisionResponse   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetActorCollision      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetActorCollision      (TSharedPtr<FJsonObject> Params);

	static AActor* FindActorByLabel(UWorld* World, const FString& Label);
};
