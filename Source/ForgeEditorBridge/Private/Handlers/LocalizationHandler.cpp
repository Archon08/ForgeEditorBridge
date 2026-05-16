#include "Handlers/LocalizationHandler.h"
#include "ForgeAISubsystem.h"

// ---- Internationalization (Core) --------------------------------------------
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

// ---- FileSystem -------------------------------------------------------------
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("localization");

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("list_cultures"))             return Action_ListCultures(Params);
	if (Action == TEXT("gather_text"))               return Action_GatherText(Params);
	if (Action == TEXT("export_po"))                 return Action_ExportPO(Params);
	if (Action == TEXT("import_po"))                 return Action_ImportPO(Params);
	if (Action == TEXT("add_culture"))               return Action_AddCulture(Params);
	if (Action == TEXT("find_missing_translations")) return Action_FindMissingTranslations(Params);
	if (Action == TEXT("create_localization_target"))return Action_CreateLocalizationTarget(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: list_cultures, gather_text, export_po, import_po, add_culture, find_missing_translations, create_localization_target"), *Action));
}

// ---------------------------------------------------------------------------
// list_cultures — FInternationalization + Config/Localization dir scan
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_ListCultures(TSharedPtr<FJsonObject> Params)
{
	FInternationalization& I18N = FInternationalization::Get();

	// Current culture
	const FString CurrentCulture  = I18N.GetCurrentCulture()->GetName();
	const FString DefaultCulture  = I18N.GetDefaultCulture()->GetName();

	// All cultures known to the engine
	TArray<FString> AllCultures;
	I18N.GetCultureNames(AllCultures);

	// Scan Config/Localization for project-configured culture directories
	const FString LocalizationDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Localization"));
	TArray<FString> TargetDirs;
	IFileManager::Get().FindFiles(TargetDirs, *FPaths::Combine(LocalizationDir, TEXT("*")), false, true);

	TMap<FString, TArray<FString>> TargetCultureMap; // target -> cultures found on disk
	for (const FString& TargetDir : TargetDirs)
	{
		const FString FullTargetPath = FPaths::Combine(LocalizationDir, TargetDir);
		TArray<FString> CultureDirs;
		IFileManager::Get().FindFiles(CultureDirs, *FPaths::Combine(FullTargetPath, TEXT("*")), false, true);
		TargetCultureMap.Add(TargetDir, CultureDirs);
	}

	// Build response
	TArray<TSharedPtr<FJsonValue>> TargetArr;
	for (const auto& Pair : TargetCultureMap)
	{
		TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("target"), Pair.Key);
		TArray<TSharedPtr<FJsonValue>> CultArr;
		for (const FString& C : Pair.Value)
			CultArr.Add(MakeShared<FJsonValueString>(C));
		TargetObj->SetArrayField(TEXT("cultures"), CultArr);
		TargetArr.Add(MakeShared<FJsonValueObject>(TargetObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("current_culture"), CurrentCulture);
	Data->SetStringField(TEXT("default_culture"), DefaultCulture);
	Data->SetNumberField(TEXT("engine_cultures_total"), AllCultures.Num());
	Data->SetArrayField(TEXT("localization_targets"), TargetArr);
	Data->SetStringField(TEXT("localization_dir"), LocalizationDir);

	return MakeSuccess(DOMAIN, TEXT("list_cultures"),
		FString::Printf(TEXT("Current: %s | Default: %s | %d localization target(s)"),
			*CurrentCulture, *DefaultCulture, TargetDirs.Num()), Data);
}

// ---------------------------------------------------------------------------
// gather_text — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_GatherText(TSharedPtr<FJsonObject> Params)
{
	FString Target = TEXT("Game");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("target"), Target);

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("gather_text"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"gather_text: Text gathering requires the GatherText commandlet.\n"
		"Run via commandlet:\n"
		"  UnrealEditor-Cmd.exe %s -run=GatherText -config=Config/Localization/%s/%s_Gather.ini\n"
		"Or use Editor: Tools > Localization Dashboard > Gather Text button on target '%s'."), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Target, *Target, *Target);
	R.RecoveryHint = TEXT("Ensure the localization target is configured in the Localization Dashboard before gathering.");
	return R;
}

// ---------------------------------------------------------------------------
// export_po — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_ExportPO(TSharedPtr<FJsonObject> Params)
{
	FString Culture = TEXT("fr");
	FString Target = TEXT("Game");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("culture"), Culture);
		Params->TryGetStringField(TEXT("target"), Target);
	}

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("export_po"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"export_po: PO export requires the InternationalizationExport commandlet.\n"
		"Run via commandlet:\n"
		"  UnrealEditor-Cmd.exe %s -run=InternationalizationExport -ExportType=PO -SourcePath=Content/Localization/%s -DestinationPath=Content/Localization/%s/%s -Culture=%s\n"
		"Or use Editor: Localization Dashboard > Export (PO) on target '%s' for culture '%s'."), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Target, *Target, *Culture, *Culture, *Target, *Culture);
	R.RecoveryHint = TEXT("Gather text first to ensure all source strings are up to date.");
	return R;
}

// ---------------------------------------------------------------------------
// import_po — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_ImportPO(TSharedPtr<FJsonObject> Params)
{
	FString Culture = TEXT("fr");
	FString POPath;
	FString Target = TEXT("Game");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("culture"), Culture);
		Params->TryGetStringField(TEXT("po_path"), POPath);
		Params->TryGetStringField(TEXT("target"), Target);
	}

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("import_po"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"import_po: PO import requires the InternationalizationExport commandlet.\n"
		"Run via commandlet:\n"
		"  UnrealEditor-Cmd.exe %s -run=InternationalizationExport -ImportType=PO -SourcePath=Content/Localization/%s/%s -DestinationPath=Content/Localization/%s -Culture=%s\n"
		"Or use Editor: Localization Dashboard > Import (PO) on target '%s' for culture '%s'.\n"
		"PO file path provided: %s"), *FPaths::GetCleanFilename(FPaths::GetProjectFilePath()),
		*Target, *Culture, *Target, *Culture, *Target, *Culture,
		POPath.IsEmpty() ? TEXT("(none)") : *POPath);
	R.RecoveryHint = TEXT("After import, compile the localization data to apply translations.");
	return R;
}

// ---------------------------------------------------------------------------
// add_culture — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_AddCulture(TSharedPtr<FJsonObject> Params)
{
	FString Culture;
	FString Target = TEXT("Game");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("culture"), Culture);
		Params->TryGetStringField(TEXT("target"), Target);
	}
	if (Culture.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_culture"), 1000, TEXT("Missing required param: 'culture' (e.g. 'fr', 'de', 'ja')"));

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("add_culture"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"add_culture: Adding a new culture requires editor configuration.\n"
		"Steps:\n"
		"  1. Open Editor: Tools > Localization Dashboard\n"
		"  2. Select target '%s'\n"
		"  3. Click 'Add New Culture' and enter '%s'\n"
		"  4. Run Gather Text to populate source strings for the new culture\n"
		"  5. Translate the exported PO file and import it back"),
		*Target, *Culture);
	R.RecoveryHint = TEXT("Culture code must be a valid BCP-47 tag (e.g. 'fr', 'fr-FR', 'zh-Hans').");
	return R;
}

// ---------------------------------------------------------------------------
// find_missing_translations — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_FindMissingTranslations(TSharedPtr<FJsonObject> Params)
{
	FString Culture;
	FString Target = TEXT("Game");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("culture"), Culture);
		Params->TryGetStringField(TEXT("target"), Target);
	}

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("find_missing_translations"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"find_missing_translations: Missing translation analysis requires the InternationalizationExport commandlet.\n"
		"Quick approach via Python in editor:\n"
		"  import unreal\n"
		"  # Check Content/Localization/%s/ — any key present in en/ but absent in %s/ is untranslated.\n"
		"  # Use the Localization Dashboard's 'Word Count' column as a coverage indicator.\n"
		"Full approach: export PO for all cultures and compare key counts against native culture."),
		*Target, Culture.IsEmpty() ? TEXT("<culture>") : *Culture);
	R.RecoveryHint = TEXT("The Localization Dashboard shows word count per culture — compare against native culture count for coverage %.");
	return R;
}

// ---------------------------------------------------------------------------
// create_localization_target — Python dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULocalizationHandler::Action_CreateLocalizationTarget(TSharedPtr<FJsonObject> Params)
{
	FString TargetName;
	FString NativeCulture = TEXT("en");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("name"), TargetName);
		Params->TryGetStringField(TEXT("native_culture"), NativeCulture);
	}
	if (TargetName.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_localization_target"), 1000, TEXT("Missing required param: 'name'"));

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("create_localization_target"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"create_localization_target: Creating a new localization target requires editor UI.\n"
		"Steps:\n"
		"  1. Open Editor: Tools > Localization Dashboard\n"
		"  2. Click 'Add New Target'\n"
		"  3. Enter target name: '%s'\n"
		"  4. Set native culture: '%s'\n"
		"  5. Configure gather paths and rules\n"
		"  6. Save and run Gather Text\n"
		"This creates Config/Localization/%s/%s.ini and the corresponding Content/Localization/%s/ directory."),
		*TargetName, *NativeCulture, *TargetName, *TargetName, *TargetName);
	R.RecoveryHint = TEXT("The localization target config .ini is the authoritative source — do not manually edit unless you understand the format.");
	return R;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> ULocalizationHandler::GetActionSchemas() const
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

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List current + default culture, all localization targets and their culture directories"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_cultures"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Gather source text for a localization target (Python dispatch — GatherText commandlet)"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("target"), P(TEXT("string"), false, TEXT("Target name (default: Game)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("gather_text"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Export PO files for a culture (Python dispatch — InternationalizationExport commandlet)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("culture"), P(TEXT("string"), true,  TEXT("Culture code e.g. 'fr'")));
	  Pr->SetObjectField(TEXT("target"),  P(TEXT("string"), false, TEXT("Target name (default: Game)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("export_po"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Import translated PO file for a culture (Python dispatch)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("culture"), P(TEXT("string"), true,  TEXT("Culture code")));
	  Pr->SetObjectField(TEXT("po_path"), P(TEXT("string"), false, TEXT("Path to PO file")));
	  Pr->SetObjectField(TEXT("target"),  P(TEXT("string"), false, TEXT("Target name (default: Game)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("import_po"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a new culture to a localization target (Python dispatch — Localization Dashboard)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("culture"), P(TEXT("string"), true,  TEXT("BCP-47 culture code e.g. 'fr', 'zh-Hans'")));
	  Pr->SetObjectField(TEXT("target"),  P(TEXT("string"), false, TEXT("Target name (default: Game)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_culture"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Find untranslated keys for a culture (Python dispatch — compare PO exports)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("culture"), P(TEXT("string"), false, TEXT("Culture code to check")));
	  Pr->SetObjectField(TEXT("target"),  P(TEXT("string"), false, TEXT("Target name (default: Game)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("find_missing_translations"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new localization target (Python dispatch — Localization Dashboard)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("name"),            P(TEXT("string"), true,  TEXT("Target name")));
	  Pr->SetObjectField(TEXT("native_culture"),  P(TEXT("string"), false, TEXT("Source culture (default: en)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_localization_target"), A); }

	return Root;
}
