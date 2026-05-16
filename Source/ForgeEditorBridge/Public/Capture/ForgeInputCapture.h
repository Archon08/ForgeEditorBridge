#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeInputCapture.generated.h"

/**
 * v1.12 — Enhanced Input Audit
 *
 * Exports the full Enhanced Input configuration: all UInputMappingContext assets,
 * all UInputAction assets, and (when PIE is active) the live applied-context state
 * for each local player.
 *
 * Output: {ProjectRoot}/Forge/ue-context/input/enhanced_input_audit.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.input_capture.export_input_audit()
 *
 * Exported JSON fields:
 *   generated, level_name,
 *   mapping_contexts[]{
 *     asset, name, mapping_count,
 *     mappings[]{action, action_name, key, modifier_count, modifiers[], trigger_count, triggers[]}
 *   },
 *   input_actions[]{
 *     asset, name, value_type, consume_input, trigger_when_paused,
 *     modifier_count, trigger_count, mapped_in_context_count
 *   },
 *   active_player_contexts[]{         (populated only during PIE)
 *     player_index,
 *     contexts[]{asset, name, priority}
 *   },
 *   audit{total_issues, issues[]{issue_type, severity, detail}}
 *
 * Audit rules (3):
 *   INPUT_CONFLICT       — Error: two contexts map the same key to different actions
 *                          (static analysis — runtime priority may resolve at play time)
 *   MISSING_CONSUMPTION  — Info: a high-impact key (Escape/Enter/Tab/Space/Backspace)
 *                          is mapped without bConsumeInput, risking unintended fallthrough
 *   DEAD_ACTION          — Warning: a UInputAction exists but has no mappings in any
 *                          project context (orphaned asset)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeInputCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Capture Enhanced Input configuration from the project asset registry.
	 * If PIE is active, also captures live applied-context state per local player.
	 * Writes input/enhanced_input_audit.json.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportInputAudit();

private:
	FString OutputDir;

	// READ-MERGE-WRITE index.json to add/update the "input" section
	void UpdateIndexFile();
};
