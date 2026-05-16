#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "EQSHandler.generated.h"

/**
 * EQSHandler — domain "eqs"  (v0.2.6 / UE 5.7)
 *
 * Creates and modifies Environment Query System assets through the Bridge.
 * NOTE: UEnvQueryGenerator_SimpleGrid is the correct UE 5.4+ class name
 *       (renamed from the deprecated UEnvQueryGenerator_Grid).
 *
 * Actions:
 *   create_query    → asset_path, generator_type (SimpleGrid|Donut|OnCircle)
 *                     Creates a UEnvQuery data asset with one option + the requested generator.
 *   add_test        → asset_path, test_type (Distance|Trace|Overlap|Pathfinding),
 *                     option_index (int, default 0)
 *                     Appends a UEnvQueryTest_* to an existing query option.
 *   set_context     → asset_path, context_class (Querier|Item|<full class path>),
 *                     option_index (int, default 0)
 *                     Sets the generator's ContextClass on the specified option.
 *   create_context  → asset_path — Blueprint subclass of UEnvQueryContext_BlueprintBase
 *   run_debug_query → always returns 3004 BLOCKED_API; EQS debug runs only in PIE
 *   list_queries    → search_path (optional, default "/Game") — list all UEnvQuery assets
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEQSHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()
public:
	virtual FString GetDomainName() const override { return TEXT("eqs"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return { TEXT("create_query"), TEXT("add_test"), TEXT("set_context"),
		         TEXT("create_context"), TEXT("run_debug_query"), TEXT("list_queries"),
		         TEXT("remove_test"), TEXT("delete_query") };
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateQuery   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddTest       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetContext    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateContext (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunDebugQuery (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListQueries   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveTest    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DeleteQuery   (TSharedPtr<FJsonObject> Params);
};
