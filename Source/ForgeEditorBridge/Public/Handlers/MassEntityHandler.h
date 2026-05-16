#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "MassEntityHandler.generated.h"

/**
 * MassEntityHandler — domain "mass_entity"
 *
 * Mass Entity framework and StateTree asset management.
 *
 * Actions:
 *   create_mass_config  → asset_path
 *   add_trait           → asset_path, trait_class
 *   remove_trait        → asset_path, trait_class
 *   create_state_tree   → asset_path
 *   add_state           → asset_path, state_name, parent_state?, type?
 *   list_traits         → asset_path — returns all traits on the UMassEntityConfigAsset
 *   get_config_info     → asset_path — returns trait count, config class name, package path
 *   configure_time_slice→ asset_path, max_entities_per_tick?, time_budget_ms? — tune Mass processing budget
 *   create_entity_query → asset_path, required_fragments?, optional_fragments? — descriptor JSON for C++ codegen
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMassEntityHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("mass_entity"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_mass_config"), TEXT("add_trait"), TEXT("remove_trait"), TEXT("create_state_tree"), TEXT("add_state"), TEXT("list_traits"), TEXT("get_config_info"), TEXT("configure_time_slice"), TEXT("create_entity_query") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_RemoveTrait(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListTraits(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetConfigInfo(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ConfigureTimeSlice(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateEntityQuery(TSharedPtr<FJsonObject> Params);
};
