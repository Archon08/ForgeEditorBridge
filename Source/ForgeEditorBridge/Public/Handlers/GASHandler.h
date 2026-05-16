#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "GASHandler.generated.h"

/**
 * GASHandler — domain "gas"  (v0.4.0 / UE 5.7)
 *
 * Creates and modifies Gameplay Ability System assets through the Bridge.
 * Tags use the UE 5.3+ GameplayEffectComponent architecture — direct tag
 * properties were removed from UGameplayEffect in favour of UGameplayEffectComponent
 * subclasses (e.g. UAssetTagsGameplayEffectComponent).
 *
 * Actions:
 *   create_gameplay_effect  → asset_path, duration_policy (Instant|Infinite|HasDuration)
 *   create_gameplay_ability → asset_path, parent_class (optional), tags (string[], optional)
 *                             Creates a UGameplayAbility Blueprint; sets AbilityTag on CDO.
 *   add_ge_modifier         → asset_path, attribute ("ClassName.PropertyName"),
 *                             op (Add|Multiply|Override), magnitude (float)
 *   set_ge_tags             → asset_path, tags (string[]),
 *                             tag_type (Owned|Granted|Required|Ignored)
 *                             Owned   → UAssetTagsGameplayEffectComponent
 *                             Granted → UTargetTagsGameplayEffectComponent
 *                             Required → UTargetTagRequirementsGameplayEffectComponent
 *                             Ignored  → USourceTagRequirementsGameplayEffectComponent
 *   create_gameplay_cue     → asset_path, cue_type (Static|Actor)
 *   create_attribute_set    → class_name, module_name, attributes[], api_macro, rel_path_h, rel_path_cpp
 *   set_ge_duration         → asset_path, duration_policy, duration_magnitude
 *   set_ge_period           → asset_path, period, execute_on_application
 *   set_ge_stacking         → asset_path, stacking_type, stack_limit, duration_refresh, period_reset
 *   get_ge_info             → asset_path — basic GE read (duration, modifiers, stacking)
 *   read_audit              → asset_path — full audit: tags per category, all modifiers, duration settings
 *   read_gas_capture        → triggers GASCapture export and returns gas/audit.json as structured JSON
 *
 * Phase 1c additions:
 *   create_ability          → alias for create_gameplay_ability
 *   add_modifier_to_ge      → alias for add_ge_modifier
 *   add_ability_cost        → ability_path, cost_asset_path — sets CostGameplayEffectClass on ability CDO
 *   set_ability_cooldown    → ability_path, cooldown_asset_path, duration — sets CooldownGameplayEffectClass
 *   add_granted_ability     → ge_asset_path, ability_class_path — appends FGameplayAbilitySpecDef to GE
 *   list_attribute_sets     → (optional: base_class_path) — lists all UAttributeSet Blueprint subclasses
 */
UCLASS()
class FORGEEDITORBRIDGE_API UGASHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()
public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("gas"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_gameplay_effect"), TEXT("create_gameplay_ability"), TEXT("add_ge_modifier"), TEXT("set_ge_tags"), TEXT("create_gameplay_cue"), TEXT("create_attribute_set"), TEXT("set_ge_duration"), TEXT("set_ge_period"), TEXT("set_ge_stacking"), TEXT("get_ge_info"), TEXT("read_audit"), TEXT("read_gas_capture"), TEXT("list_abilities_on_class"), TEXT("get_attribute_defaults"), TEXT("create_ability_task"), TEXT("configure_asc"), TEXT("add_ability_to_asc"), TEXT("remove_ability_from_asc"), TEXT("set_ability_tags"), TEXT("create_ability"), TEXT("add_modifier_to_ge"), TEXT("add_ability_cost"), TEXT("set_ability_cooldown"), TEXT("add_granted_ability"), TEXT("list_attribute_sets"), TEXT("get_active_effects"), TEXT("apply_gameplay_effect"), TEXT("remove_ge_modifier") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action,
	                                    TSharedPtr<FJsonObject> Params) override;
private:
	FBridgeResult Action_CreateGameplayEffect (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateGameplayAbility(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddGEModifier       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetGETags           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateGameplayCue   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateAttributeSet  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetGEDuration       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetGEPeriod         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetGEStacking       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetGEInfo           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadAudit             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadGASCapture        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListAbilitiesOnClass  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetAttributeDefaults  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateAbilityTask     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ConfigureASC          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddAbilityToASC       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveAbilityFromASC  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAbilityTags        (TSharedPtr<FJsonObject> Params);
	// Phase 1c — new actions
	FBridgeResult Action_AddAbilityCost        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetAbilityCooldown    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddGrantedAbility     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListAttributeSets     (TSharedPtr<FJsonObject> Params);
	// Phase 0 gap — PIE-guarded runtime actions
	FBridgeResult Action_GetActiveEffects      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ApplyGameplayEffect   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveGEModifier      (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a GE Blueprint asset, compile if needed, and return the CDO as UGameplayEffect*.
	 * Also sets OutBP so callers can mark it dirty / re-compile after modifications.
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UGameplayEffect* LoadGEBlueprint(const FString& AssetPath,
	                                        class UBlueprint*& OutBP,
	                                        FBridgeResult& Result);
};
