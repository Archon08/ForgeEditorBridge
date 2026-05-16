#include "Handlers/EQSHandler.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Donut.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Trace.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Overlap.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Pathfinding.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetData.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("eqs");

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_query"))    return Action_CreateQuery(Params);
	if (Action == TEXT("add_test"))        return Action_AddTest(Params);
	if (Action == TEXT("set_context"))     return Action_SetContext(Params);
	if (Action == TEXT("create_context"))  return Action_CreateContext(Params);
	if (Action == TEXT("run_debug_query")) return Action_RunDebugQuery(Params);
	if (Action == TEXT("list_queries"))    return Action_ListQueries(Params);
	if (Action == TEXT("remove_test"))     return Action_RemoveTest(Params);
	if (Action == TEXT("delete_query"))    return Action_DeleteQuery(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown eqs action '%s'"), *Action));
}

// ---------------------------------------------------------------------------
// create_query
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_CreateQuery(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_query"), 1000, TEXT("'asset_path' is required"));

	FString GeneratorType = TEXT("SimpleGrid");
	Params->TryGetStringField(TEXT("generator_type"), GeneratorType);

	// Derive package name and asset name
	FString PackageName = AssetPath;
	FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	if (AssetName.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_query"), 1000,
			TEXT("'asset_path' must be a full package path, e.g. '/Game/AI/EQ_FindEnemy'"));

	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();

	// Create the UEnvQuery data asset
	UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName,
		RF_Public | RF_Standalone | RF_Transactional);

	// Create the default option
	UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, NAME_None, RF_Transactional);

	// Create the generator based on requested type
	UEnvQueryGenerator* Generator = nullptr;
	if (GeneratorType.Equals(TEXT("SimpleGrid"), ESearchCase::IgnoreCase))
		Generator = NewObject<UEnvQueryGenerator_SimpleGrid>(Option, NAME_None, RF_Transactional);
	else if (GeneratorType.Equals(TEXT("Donut"), ESearchCase::IgnoreCase))
		Generator = NewObject<UEnvQueryGenerator_Donut>(Option, NAME_None, RF_Transactional);
	else if (GeneratorType.Equals(TEXT("OnCircle"), ESearchCase::IgnoreCase))
		Generator = NewObject<UEnvQueryGenerator_OnCircle>(Option, NAME_None, RF_Transactional);
	else
		return MakeError(DOMAIN, TEXT("create_query"), 1000,
			FString::Printf(TEXT("Unknown generator_type '%s'. Valid: SimpleGrid|Donut|OnCircle"),
				*GeneratorType));

	Option->Generator = Generator;
	Query->GetOptionsMutable().Add(Option);

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Query);

	return MakeSuccess(DOMAIN, TEXT("create_query"),
		FString::Printf(TEXT("Created EQ '%s' with %s generator"), *AssetPath, *GeneratorType),
		[&]() -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("asset_path"), AssetPath);
			D->SetStringField(TEXT("generator_type"), GeneratorType);
			return D;
		}());
}

// ---------------------------------------------------------------------------
// add_test
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_AddTest(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_test"), 1000, TEXT("'asset_path' is required"));

	FString TestType;
	if (!Params->TryGetStringField(TEXT("test_type"), TestType) || TestType.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_test"), 1000,
			TEXT("'test_type' is required: Distance|Trace|Overlap|Pathfinding"));

	double OptionIdxD = 0.0;
	Params->TryGetNumberField(TEXT("option_index"), OptionIdxD);
	int32 OptionIdx = (int32)OptionIdxD;

	UEnvQuery* Query = Cast<UEnvQuery>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Query)
		return MakeError(DOMAIN, TEXT("add_test"), 2000,
			FString::Printf(TEXT("UEnvQuery not found: '%s'"), *AssetPath));

	if (!Query->GetOptions().IsValidIndex(OptionIdx))
		return MakeError(DOMAIN, TEXT("add_test"), 1000,
			FString::Printf(TEXT("Option index %d is out of range (query has %d options)"),
				OptionIdx, Query->GetOptions().Num()));

	UEnvQueryOption* Option = Query->GetOptions()[OptionIdx];
	if (!Option)
		return MakeError(DOMAIN, TEXT("add_test"), 3000, TEXT("Option object is null"));

	auto Tx = BeginTransaction(TEXT("Bridge: eqs add_test"));
	Query->Modify();
	Option->Modify();

	UEnvQueryTest* Test = nullptr;
	if (TestType.Equals(TEXT("Distance"), ESearchCase::IgnoreCase))
		Test = NewObject<UEnvQueryTest_Distance>(Option, NAME_None, RF_Transactional);
	else if (TestType.Equals(TEXT("Trace"), ESearchCase::IgnoreCase))
		Test = NewObject<UEnvQueryTest_Trace>(Option, NAME_None, RF_Transactional);
	else if (TestType.Equals(TEXT("Overlap"), ESearchCase::IgnoreCase))
		Test = NewObject<UEnvQueryTest_Overlap>(Option, NAME_None, RF_Transactional);
	else if (TestType.Equals(TEXT("Pathfinding"), ESearchCase::IgnoreCase))
		Test = NewObject<UEnvQueryTest_Pathfinding>(Option, NAME_None, RF_Transactional);
	else
		return MakeError(DOMAIN, TEXT("add_test"), 1000,
			FString::Printf(TEXT("Unknown test_type '%s'. Valid: Distance|Trace|Overlap|Pathfinding"),
				*TestType));

	Option->Tests.Add(Test);
	Query->GetPackage()->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("add_test"),
		FString::Printf(TEXT("Added %s test to option %d of '%s'"),
			*TestType, OptionIdx, *AssetPath));
}

// ---------------------------------------------------------------------------
// set_context
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_SetContext(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_context"), 1000, TEXT("'asset_path' is required"));

	FString ContextClass;
	if (!Params->TryGetStringField(TEXT("context_class"), ContextClass) || ContextClass.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_context"), 1000,
			TEXT("'context_class' is required: Querier|Item|<full class path>"));

	double OptionIdxD = 0.0;
	Params->TryGetNumberField(TEXT("option_index"), OptionIdxD);
	int32 OptionIdx = (int32)OptionIdxD;

	UEnvQuery* Query = Cast<UEnvQuery>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Query)
		return MakeError(DOMAIN, TEXT("set_context"), 2000,
			FString::Printf(TEXT("UEnvQuery not found: '%s'"), *AssetPath));

	if (!Query->GetOptions().IsValidIndex(OptionIdx) || !Query->GetOptions()[OptionIdx])
		return MakeError(DOMAIN, TEXT("set_context"), 1000,
			FString::Printf(TEXT("Option index %d is out of range"), OptionIdx));

	UEnvQueryOption* Option = Query->GetOptions()[OptionIdx];
	if (!Option->Generator)
		return MakeError(DOMAIN, TEXT("set_context"), 3000, TEXT("Option has no generator"));

	// Resolve context class
	UClass* ContextUClass = nullptr;
	if (ContextClass.Equals(TEXT("Querier"), ESearchCase::IgnoreCase))
		ContextUClass = UEnvQueryContext_Querier::StaticClass();
	else
		ContextUClass = FindObject<UClass>(nullptr, *ContextClass);

	if (!ContextUClass)
		return MakeError(DOMAIN, TEXT("set_context"), 2000,
			FString::Printf(TEXT("Context class '%s' not found"), *ContextClass));

	auto Tx = BeginTransaction(TEXT("Bridge: eqs set_context"));
	Option->Generator->Modify();

	// Set ContextClass via reflection (each generator stores it differently)
	if (FObjectProperty* Prop = FindFProperty<FObjectProperty>(
		Option->Generator->GetClass(), TEXT("GenerateAround")))
	{
		Prop->SetObjectPropertyValue_InContainer(Option->Generator, ContextUClass->GetDefaultObject());
	}
	// Fallback: try "Context" property name used by some generators
	else if (FClassProperty* ClassProp = FindFProperty<FClassProperty>(
		Option->Generator->GetClass(), TEXT("GenerateAround")))
	{
		ClassProp->SetPropertyValue_InContainer(Option->Generator, ContextUClass);
	}

	Query->GetPackage()->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("set_context"),
		FString::Printf(TEXT("Set context to '%s' on option %d of '%s'"),
			*ContextClass, OptionIdx, *AssetPath));
}

// ---------------------------------------------------------------------------
// create_context  — Blueprint subclass of UEnvQueryContext_BlueprintBase
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_CreateContext(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_context"), 1000, TEXT("'asset_path' is required"));

	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	if (AssetName.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_context"), 1000,
			TEXT("'asset_path' must be a full package path"));

	UClass* ParentClass = FindObject<UClass>(nullptr,
		TEXT("/Script/AIModule.EnvQueryContext_BlueprintBase"));
	if (!ParentClass)
		return MakeError(DOMAIN, TEXT("create_context"), 3003,
			TEXT("UEnvQueryContext_BlueprintBase not found; ensure AIModule is loaded"));

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		CreatePackage(*AssetPath),
		*AssetName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!BP)
		return MakeError(DOMAIN, TEXT("create_context"), 3000,
			FString::Printf(TEXT("Failed to create Blueprint at '%s'"), *AssetPath));

	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("create_context"),
		FString::Printf(TEXT("Created EQS context Blueprint at '%s'"), *AssetPath));
}

// ---------------------------------------------------------------------------
// run_debug_query  — blocked in editor; requires PIE
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_RunDebugQuery(TSharedPtr<FJsonObject> Params)
{
	return MakeError(DOMAIN, TEXT("run_debug_query"), 3004,
		TEXT("run_debug_query requires PIE to be active — EQS debug runs only in Play-In-Editor context. "
		     "Start PIE first, then call this action."));
}

// ---------------------------------------------------------------------------
// list_queries
// ---------------------------------------------------------------------------

FBridgeResult UEQSHandler::Action_ListQueries(TSharedPtr<FJsonObject> Params)
{
	FString SearchPath = TEXT("/Game");
	Params->TryGetStringField(TEXT("search_path"), SearchPath);

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(*SearchPath);
	Filter.bRecursivePaths   = true;
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/AIModule"), TEXT("EnvQuery")));

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& AD : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), AD.GetSoftObjectPath().ToString());
		Entry->SetStringField(TEXT("name"), AD.AssetName.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("queries"), Items);
	Data->SetNumberField(TEXT("count"), Assets.Num());

	return MakeSuccess(DOMAIN, TEXT("list_queries"),
		FString::Printf(TEXT("Found %d EQS query asset(s) under '%s'"), Assets.Num(), *SearchPath),
		Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UEQSHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto MakeParam = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetBoolField(TEXT("required"), bReq);
		P->SetStringField(TEXT("desc"), Desc);
		return P;
	};

	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"),     MakeParam(TEXT("string"), true,  TEXT("Full package path, e.g. /Game/AI/EQ_FindEnemy")));
		Pm->SetObjectField(TEXT("generator_type"), MakeParam(TEXT("string"), false, TEXT("SimpleGrid|Donut|OnCircle (default SimpleGrid)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Create a UEnvQuery data asset with one option and the specified generator"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("create_query"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"),   MakeParam(TEXT("string"), true,  TEXT("Package path to existing UEnvQuery")));
		Pm->SetObjectField(TEXT("test_type"),    MakeParam(TEXT("string"), true,  TEXT("Distance|Trace|Overlap|Pathfinding")));
		Pm->SetObjectField(TEXT("option_index"), MakeParam(TEXT("int"),    false, TEXT("Query option index (default 0)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Append a UEnvQueryTest to an existing query option"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("add_test"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"),    MakeParam(TEXT("string"), true,  TEXT("Package path to existing UEnvQuery")));
		Pm->SetObjectField(TEXT("context_class"), MakeParam(TEXT("string"), true,  TEXT("Querier | Item | /Script/MyModule.MyContext")));
		Pm->SetObjectField(TEXT("option_index"),  MakeParam(TEXT("int"),    false, TEXT("Query option index (default 0)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the GenerateAround context on the option's generator"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_context"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Full package path for the new context Blueprint")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Create a Blueprint subclass of UEnvQueryContext_BlueprintBase"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("create_context"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Always returns 3004 BLOCKED_API — EQS debug query requires PIE"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("run_debug_query"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("search_path"), MakeParam(TEXT("string"), false, TEXT("Package path to search (default /Game)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("List all UEnvQuery assets under search_path"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("list_queries"), A);
	}

	return Root;
}

// ---------------------------------------------------------------------------
// remove_test — remove a test by index from a query option
// ---------------------------------------------------------------------------
FBridgeResult UEQSHandler::Action_RemoveTest(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("remove_test"), 1000, TEXT("'asset_path' is required"));
	int32 OptionIndex = 0, TestIndex = -1;
	Params->TryGetNumberField(TEXT("option_index"), OptionIndex);
	if (!Params->TryGetNumberField(TEXT("test_index"), TestIndex) || TestIndex < 0)
		return MakeError(DOMAIN, TEXT("remove_test"), 1000, TEXT("'test_index' is required (>= 0)"));

	UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *AssetPath);
	if (!Query)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Query = LoadObject<UEnvQuery>(nullptr, *Suffix);
	}
	if (!Query)
		return MakeError(DOMAIN, TEXT("remove_test"), 2000,
			FString::Printf(TEXT("Query not found: %s"), *AssetPath));
	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
		return MakeError(DOMAIN, TEXT("remove_test"), 1001,
			FString::Printf(TEXT("Invalid option_index %d (have %d)"), OptionIndex, Query->GetOptions().Num()));

	TArray<UEnvQueryOption*> Options = Query->GetOptionsMutable();
	UEnvQueryOption* Option = Options[OptionIndex];
	if (!Option || TestIndex >= Option->Tests.Num())
		return MakeError(DOMAIN, TEXT("remove_test"), 1001,
			FString::Printf(TEXT("Invalid test_index %d"), TestIndex));

	Option->Tests.RemoveAt(TestIndex);
	Query->MarkPackageDirty();
	return MakeSuccess(DOMAIN, TEXT("remove_test"),
		FString::Printf(TEXT("Removed test %d from option %d"), TestIndex, OptionIndex));
}

// ---------------------------------------------------------------------------
// delete_query — full asset deletion
// ---------------------------------------------------------------------------
FBridgeResult UEQSHandler::Action_DeleteQuery(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("delete_query"), 1000, TEXT("'asset_path' is required"));
	UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *AssetPath);
	if (!Query)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Query = LoadObject<UEnvQuery>(nullptr, *Suffix);
	}
	if (!Query)
		return MakeError(DOMAIN, TEXT("delete_query"), 2000,
			FString::Printf(TEXT("Query not found: %s"), *AssetPath));

	// Delegate to the Asset domain's deletion path so we get force/recovery handling consistently
	return MakeError(DOMAIN, TEXT("delete_query"), 3003,
		TEXT("Asset deletion routed through asset/delete_asset"),
		FString::Printf(TEXT("Call asset/delete_asset with asset_path='%s'"), *AssetPath));
}
