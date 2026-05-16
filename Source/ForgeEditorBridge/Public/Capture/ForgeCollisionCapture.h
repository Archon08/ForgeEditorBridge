#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeCollisionCapture.generated.h"

/**
 * v1.0 — Collision Configuration Capture
 *
 * Exports the project's collision channels, profiles, and a per-actor
 * collision audit for the current level.
 *
 * Output: {ProjectRoot}/Forge/ue-context/collision/profiles.json
 *
 * Trigger from Python:
 *   sub = unreal.get_editor_subsystem(unreal.ForgeAISubsystem)
 *   sub.collision_capture.export_collision_profiles()
 *
 * JSON structure:
 *   generated, channels[], profiles[], actor_audit[], unused_channels[], summary{}
 *
 * Audit rules (applied at profile and actor level):
 *   OVERLAP_ALL     — profile/actor overlaps all channels (query hit on everything; perf risk)
 *   MISSING_PROFILE — actor using "Default" or "Custom" instead of a named profile
 *   CHANNEL_UNUSED  — custom channel defined in ini but not referenced by any profile
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeCollisionCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Export collision channels, profiles, and actor audit to collision/profiles.json.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportCollisionProfiles();

private:
	FString OutputDir;

	void UpdateIndexFile();
};
