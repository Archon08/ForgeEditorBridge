#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeGASCapture.generated.h"

/**
 * v0.9 / v1.0+ — Gameplay Ability System (GAS) Audit
 *
 * Performs a static analysis of all GAS assets in the project:
 *   - Ability cost / cooldown / instancing / EndAbility checks
 *   - Commit pattern validation (CommitAbility called + return value checked)
 *   - AbilityTask lifecycle (tasks created + ReadyForActivation called)
 *   - Cost GE duration policy / Cooldown GE tag presence
 *   - GameplayEffect stacking and negative period checks
 *   - AttributeSet clamping function presence
 *   - Tag validity across ability containers
 *
 * Output: {ProjectRoot}/Forge/ue-context/gas/audit.json
 *
 * Trigger manually from Python:
 *   subsystem = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   subsystem.gas_capture.export_gas_audit()
 *
 * Audit rules (v0.9):
 *   MISSING_COST              — Ability has no cost GE
 *   MISSING_COOLDOWN          — Ability has no cooldown GE
 *   NONINSTANCED_WITH_STATE   — NonInstanced ability carries BP variables
 *   MISSING_END_ABILITY       — Ability graph has no EndAbility call
 *   NO_CLAMPING_FUNCTIONS     — AttributeSet has no Pre/PostAttributeChange overrides
 *   MISSING_PRE_ATTRIBUTE_CHANGE — Only PostGameplayEffectExecute present
 *   INVALID_TAG               — Unregistered tag in ability containers
 *   UNTAGGED_ABILITY          — AssetTags empty; ability can't be queried by tag
 *
 * Audit rules (v1.0+):
 *   MISSING_COMMIT_ABILITY    — Ability has cost/cooldown but no CommitAbility call
 *   UNCHECKED_COMMIT_RESULT   — CommitAbility return value is not connected to anything
 *   COST_GE_NOT_INSTANT       — Cost GE has non-Instant duration policy (causes permanent stat loss)
 *   COOLDOWN_GE_NO_TAG        — Cooldown GE grants no tags; ASC can't detect cooldown state
 *   TASK_CREATED_NOT_READY    — AbilityTask usage detected but ReadyForActivation never called
 *   INFINITE_STACKING_GE      — GameplayEffect stacks without a StackLimitCount
 *   GE_NEGATIVE_PERIOD        — GameplayEffect has a negative period value
 *
 * Audit rules (v1.1 / Wave 5):
 *   ABILITY_NO_INPUT          — Ability has no event triggers and no auto-grant; relies on external ASC code
 *   GE_MODIFIER_NO_ATTRIBUTE  — Modifier targets an invalid or missing attribute; has no effect
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeGASCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Run the full GAS audit and write gas/audit.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportGASAudit();

private:
    FString OutputDir;

    // Sub-auditors — each returns an array of issue JSON value objects
    TArray<TSharedPtr<FJsonValue>> AuditAbilities();
    TArray<TSharedPtr<FJsonValue>> AuditGameplayEffects();
    TArray<TSharedPtr<FJsonValue>> AuditAttributeSets();
    TArray<TSharedPtr<FJsonValue>> AuditTags();

    // Build a single issue JSON object
    static TSharedPtr<FJsonObject> MakeIssue(
        const FString& IssueType,
        const FString& Severity,
        const FString& AssetPath,
        const FString& Detail);

    // READ-MERGE-WRITE index.json to add the "gas" section
    void UpdateIndexFile(int32 IssueCount, const FString& AuditPath);
};
