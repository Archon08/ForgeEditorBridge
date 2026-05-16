#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "TestingHandler.generated.h"

/**
 * TestingHandler — domain "testing" (v2.0.0 / UE 5.7)
 *
 * Run and inspect Unreal automation tests.
 *
 * Actions:
 *   list_tests            → filter (string, opt) — list all registered automation tests.
 *                           Uses FAutomationTestFramework::Get().GetValidTestNames().
 *
 *   run_test              → test_name (string) — run a single test by exact name.
 *                           Uses FAutomationTestFramework::Get().RunTestByName().
 *
 *   run_test_suite        → suite (string) — run all tests matching a name prefix.
 *
 *   get_test_results      → (no params) — return last run pass/fail summary.
 *
 *   create_functional_test → name (string), location {x,y,z} (opt)
 *                            Spawns AFunctionalTest actor in current level (requires FunctionalTesting plugin).
 *
 *   run_map_check         → (no params) — execute MAP CHECK on current level; results go to MapCheck message log.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UTestingHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("testing"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("list_tests"),
			TEXT("run_test"),
			TEXT("run_test_suite"),
			TEXT("get_test_results"),
			TEXT("create_functional_test"),
			TEXT("run_map_check"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_ListTests           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunTest             (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunTestSuite        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetTestResults      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateFunctionalTest(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunMapCheck         (TSharedPtr<FJsonObject> Params);
};
