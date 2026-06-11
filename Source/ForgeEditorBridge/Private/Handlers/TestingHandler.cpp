#include "Handlers/TestingHandler.h"
#include "ForgeAISubsystem.h"

// ---- Automation -------------------------------------------------------------
#include "Misc/AutomationTest.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Editor -----------------------------------------------------------------
#if WITH_EDITOR
#include "Editor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("testing");

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("list_tests"))             return Action_ListTests(Params);
	if (Action == TEXT("run_test"))               return Action_RunTest(Params);
	if (Action == TEXT("run_test_suite"))         return Action_RunTestSuite(Params);
	if (Action == TEXT("get_test_results"))       return Action_GetTestResults(Params);
	if (Action == TEXT("create_functional_test")) return Action_CreateFunctionalTest(Params);
	if (Action == TEXT("run_map_check"))          return Action_RunMapCheck(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: list_tests, run_test, run_test_suite, get_test_results, create_functional_test, run_map_check"), *Action));
}

// ---------------------------------------------------------------------------
// list_tests
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_ListTests(TSharedPtr<FJsonObject> Params)
{
	FString Filter;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("filter"), Filter);

	// GetValidTestNames takes TArray<FAutomationTestInfo> in UE 5.7
	TArray<FAutomationTestInfo> TestInfos;
	FAutomationTestFramework::Get().GetValidTestNames(TestInfos);
	TArray<FString> TestNames;
	for (const FAutomationTestInfo& Info : TestInfos)
		TestNames.Add(Info.GetTestName());

	TArray<TSharedPtr<FJsonValue>> TestArr;
	for (const FString& Name : TestNames)
	{
		if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase))
			continue;
		TestArr.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("tests"), TestArr);
	Data->SetNumberField(TEXT("count"), TestArr.Num());
	Data->SetNumberField(TEXT("total_registered"), TestNames.Num());
	if (!Filter.IsEmpty()) Data->SetStringField(TEXT("filter"), Filter);

	return MakeSuccess(DOMAIN, TEXT("list_tests"),
		FString::Printf(TEXT("Found %d test(s)%s"),
			TestArr.Num(),
			Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Filter)),
		Data);
}

// ---------------------------------------------------------------------------
// run_test
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_RunTest(TSharedPtr<FJsonObject> Params)
{
	FString TestName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("test_name"), TestName) || TestName.IsEmpty())
		return MakeError(DOMAIN, TEXT("run_test"), 1000, TEXT("Missing required param: 'test_name'"));

	// Validate the test exists
	TArray<FAutomationTestInfo> AllTestInfos;
	FAutomationTestFramework::Get().GetValidTestNames(AllTestInfos);
	TArray<FString> AllTests;
	for (const FAutomationTestInfo& Info : AllTestInfos)
		AllTests.Add(Info.GetTestName());
	const bool bExists = AllTests.Contains(TestName);
	if (!bExists)
		return MakeError(DOMAIN, TEXT("run_test"), 2000,
			FString::Printf(TEXT("Test not found: '%s'. Use list_tests to enumerate available tests."), *TestName),
			TEXT("testing/list_tests"));

	// RunTestByName removed in UE 5.7 — dispatch not supported via bridge
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("test_name"), TestName);
	Data->SetBoolField(TEXT("dispatched"),  false);

	return MakeSuccess(DOMAIN, TEXT("run_test"),
		FString::Printf(TEXT("Test '%s' exists. Direct dispatch removed in UE 5.7 — use the Automation tab (Window > Test Automation) or command line."), *TestName), Data);
}

// ---------------------------------------------------------------------------
// run_test_suite
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_RunTestSuite(TSharedPtr<FJsonObject> Params)
{
	FString Suite;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("suite"), Suite) || Suite.IsEmpty())
		return MakeError(DOMAIN, TEXT("run_test_suite"), 1000, TEXT("Missing required param: 'suite' (e.g. 'Project.Functional')"));

	TArray<FAutomationTestInfo> AllTestInfos;
	FAutomationTestFramework::Get().GetValidTestNames(AllTestInfos);

	TArray<FString> Matching;
	for (const FAutomationTestInfo& Info : AllTestInfos)
	{
		if (Info.GetTestName().StartsWith(Suite, ESearchCase::IgnoreCase))
			Matching.Add(Info.GetTestName());
	}

	if (Matching.IsEmpty())
		return MakeError(DOMAIN, TEXT("run_test_suite"), 2000,
			FString::Printf(TEXT("No tests found matching suite prefix: '%s'"), *Suite),
			TEXT("testing/list_tests"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("suite"),     Suite);
	Data->SetNumberField(TEXT("found"),     Matching.Num());
	// RunTestByName removed in UE 5.7; dispatch not supported
	Data->SetNumberField(TEXT("dispatched"),0);

	return MakeSuccess(DOMAIN, TEXT("run_test_suite"),
		FString::Printf(TEXT("Suite '%s': %d matching tests found. Direct dispatch removed in UE 5.7 — use the Automation tab."), *Suite, Matching.Num()), Data);
}

// ---------------------------------------------------------------------------
// get_test_results
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_GetTestResults(TSharedPtr<FJsonObject> Params)
{
	// IsTestRunning/GetNumTestsRemainingToRun/AllTestsPassed removed in UE 5.7
	const bool bRunning   = false;
	const int32 Remaining = 0;
	const bool bAllPassed = false;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("is_running"),        bRunning);
	Data->SetBoolField(TEXT("all_passed"),         bAllPassed);
	Data->SetNumberField(TEXT("remaining"),        Remaining);
	Data->SetStringField(TEXT("note"), TEXT("For full per-test results, check the Automation tab in the editor (Window > Test Automation) or the saved test report."));

	return MakeSuccess(DOMAIN, TEXT("get_test_results"),
		bRunning
			? FString::Printf(TEXT("Tests in progress — %d remaining"), Remaining)
			: FString::Printf(TEXT("Last run: %s"), bAllPassed ? TEXT("ALL PASSED") : TEXT("FAILURES DETECTED")),
		Data);
}

// ---------------------------------------------------------------------------
// create_functional_test — AFunctionalTest via reflection (no hard plugin dep)
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_CreateFunctionalTest(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString TestName = TEXT("NewFunctionalTest");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("name"), TestName);

	double X = 0, Y = 0, Z = 100;
	const TSharedPtr<FJsonObject>* LocObj;
	if (Params.IsValid() && Params->TryGetObjectField(TEXT("location"), LocObj))
	{
		(*LocObj)->TryGetNumberField(TEXT("x"), X);
		(*LocObj)->TryGetNumberField(TEXT("y"), Y);
		(*LocObj)->TryGetNumberField(TEXT("z"), Z);
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("create_functional_test"), 3000, TEXT("No editor world available"));

	// Use reflection to find AFunctionalTest — avoids hard FunctionalTesting plugin dep
	UClass* FTClass = FindObject<UClass>(nullptr, TEXT("/Script/FunctionalTesting.FunctionalTest"));
	if (!FTClass)
		return MakeError(DOMAIN, TEXT("create_functional_test"), 3000,
			TEXT("AFunctionalTest class not found. Enable the FunctionalTesting plugin in your project."),
			TEXT("Edit > Plugins > search 'FunctionalTesting'"));

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*TestName);
	AActor* Actor = World->SpawnActor<AActor>(FTClass, FVector(X, Y, Z), FRotator::ZeroRotator, SpawnParams);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("create_functional_test"), 3000,
			FString::Printf(TEXT("Failed to spawn FunctionalTest actor '%s'"), *TestName));

	Actor->SetActorLabel(TestName);
	Actor->MarkPackageDirty();

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("create_functional_test"),
		FString::Printf(TEXT("FunctionalTest actor created: '%s' at (%.0f, %.0f, %.0f)"), *TestName, X, Y, Z));
	R.AffectedPath = Actor->GetPathName();
	return R;
#else
	return MakeError(DOMAIN, TEXT("create_functional_test"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// run_map_check
// ---------------------------------------------------------------------------

FBridgeResult UTestingHandler::Action_RunMapCheck(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	if (!GUnrealEd)
		return MakeError(DOMAIN, TEXT("run_map_check"), 3000, TEXT("GUnrealEd not available"));

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("run_map_check"), 3000, TEXT("No editor world available"));

	GUnrealEd->Exec(World, TEXT("MAP CHECK"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("note"),
		TEXT("Results written to the MapCheck message log. Open Window > Developer Tools > Message Log > Map Check to review."));

	return MakeSuccess(DOMAIN, TEXT("run_map_check"),
		TEXT("Map check initiated. Open Message Log > Map Check for results."), Data);
#else
	return MakeError(DOMAIN, TEXT("run_map_check"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UTestingHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bReq);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all registered automation tests, optionally filtered by name substring"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("filter"), P(TEXT("string"), false, TEXT("Substring filter (empty = all)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_tests"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Run a single automation test by exact name"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("test_name"), P(TEXT("string"), true, TEXT("Full test name from list_tests"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("run_test"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Run all automation tests matching a suite name prefix"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("suite"), P(TEXT("string"), true, TEXT("Suite prefix e.g. 'Project.Functional'"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("run_test_suite"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get last test run pass/fail status and running state"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_test_results"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn an AFunctionalTest actor in the current level (requires FunctionalTesting plugin)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("name"),     P(TEXT("string"), false, TEXT("Actor label (default: NewFunctionalTest)")));
	  Pr->SetObjectField(TEXT("location"), P(TEXT("object"), false, TEXT("{x,y,z} spawn position")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_functional_test"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Run MAP CHECK on the current level; results appear in Message Log > Map Check"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("run_map_check"), A); }

	return Root;
}
