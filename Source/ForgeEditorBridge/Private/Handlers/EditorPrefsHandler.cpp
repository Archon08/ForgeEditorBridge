#include "Handlers/EditorPrefsHandler.h"
#include "ForgeAISubsystem.h"

// ---- Config -----------------------------------------------------------------
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Editor -----------------------------------------------------------------
#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("editor_prefs");

// ---------------------------------------------------------------------------
// Helper — resolve ini file name → full path
// ---------------------------------------------------------------------------

static FString ResolveIniFile(const FString& Name)
{
	if (Name.IsEmpty() || Name == TEXT("DefaultEngine")) return GEngineIni;
	if (Name == TEXT("DefaultGame"))                     return GGameIni;
	if (Name == TEXT("DefaultEditor"))                   return GEditorIni;
	if (Name == TEXT("DefaultInput"))                    return GInputIni;
	// Treat as a custom config file in the project Config dir
	return FPaths::Combine(FPaths::ProjectConfigDir(), Name.EndsWith(TEXT(".ini")) ? Name : (Name + TEXT(".ini")));
}

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("get_ini_value"))         return Action_GetIniValue(Params);
	if (Action == TEXT("set_ini_value"))         return Action_SetIniValue(Params);
	if (Action == TEXT("list_ini_sections"))     return Action_ListIniSections(Params);
	if (Action == TEXT("set_editor_preference")) return Action_SetEditorPreference(Params);
	if (Action == TEXT("get_editor_preference")) return Action_GetEditorPreference(Params);
	if (Action == TEXT("set_viewport_setting"))  return Action_SetViewportSetting(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: get_ini_value, set_ini_value, list_ini_sections, set_editor_preference, get_editor_preference, set_viewport_setting"), *Action));
}

// ---------------------------------------------------------------------------
// get_ini_value
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_GetIniValue(TSharedPtr<FJsonObject> Params)
{
	FString Section, Key, IniName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_ini_value"), 1000, TEXT("Missing required param: 'section'"));
	if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_ini_value"), 1000, TEXT("Missing required param: 'key'"));
	Params->TryGetStringField(TEXT("ini_file"), IniName);

	const FString IniPath = ResolveIniFile(IniName);
	FString Value;
	if (!GConfig->GetString(*Section, *Key, Value, IniPath))
		return MakeError(DOMAIN, TEXT("get_ini_value"), 2000,
			FString::Printf(TEXT("Key '%s' not found in section [%s] of %s"),
				*Key, *Section, IniName.IsEmpty() ? TEXT("DefaultEngine") : *IniName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("section"),  Section);
	Data->SetStringField(TEXT("key"),      Key);
	Data->SetStringField(TEXT("value"),    Value);
	Data->SetStringField(TEXT("ini_file"), IniName.IsEmpty() ? TEXT("DefaultEngine") : IniName);

	return MakeSuccess(DOMAIN, TEXT("get_ini_value"),
		FString::Printf(TEXT("[%s] %s = %s"), *Section, *Key, *Value), Data);
}

// ---------------------------------------------------------------------------
// set_ini_value
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_SetIniValue(TSharedPtr<FJsonObject> Params)
{
	FString Section, Key, Value, IniName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_ini_value"), 1000, TEXT("Missing required param: 'section'"));
	if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_ini_value"), 1000, TEXT("Missing required param: 'key'"));
	if (!Params->TryGetStringField(TEXT("value"), Value))
		return MakeError(DOMAIN, TEXT("set_ini_value"), 1000, TEXT("Missing required param: 'value'"));
	Params->TryGetStringField(TEXT("ini_file"), IniName);

	const FString IniPath = ResolveIniFile(IniName);
	const FString DisplayName = IniName.IsEmpty() ? TEXT("DefaultEngine") : IniName;

	GConfig->SetString(*Section, *Key, *Value, IniPath);
	GConfig->Flush(false, IniPath);

	return MakeSuccess(DOMAIN, TEXT("set_ini_value"),
		FString::Printf(TEXT("Wrote [%s] %s = %s to %s"), *Section, *Key, *Value, *DisplayName));
}

// ---------------------------------------------------------------------------
// list_ini_sections
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_ListIniSections(TSharedPtr<FJsonObject> Params)
{
	FString IniName;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("ini_file"), IniName);

	const FString IniPath = ResolveIniFile(IniName);
	const FString DisplayName = IniName.IsEmpty() ? TEXT("DefaultEngine") : IniName;

	// FConfigCacheIni : TMap<FString, FConfigFile>
	// FConfigFile : TMap<FString, FConfigSection>
	FConfigFile* ConfigFile = GConfig->FindConfigFile(*IniPath);
	if (!ConfigFile)
		return MakeError(DOMAIN, TEXT("list_ini_sections"), 2000,
			FString::Printf(TEXT("Ini file not loaded: %s. Use full path or one of: DefaultEngine, DefaultGame, DefaultEditor, DefaultInput"), *DisplayName));

	TArray<TSharedPtr<FJsonValue>> SectionArr;
	for (const auto& SectionPair : *ConfigFile)
	{
		SectionArr.Add(MakeShared<FJsonValueString>(SectionPair.Key));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("sections"), SectionArr);
	Data->SetNumberField(TEXT("count"),   SectionArr.Num());
	Data->SetStringField(TEXT("ini_file"),DisplayName);

	return MakeSuccess(DOMAIN, TEXT("list_ini_sections"),
		FString::Printf(TEXT("Found %d section(s) in %s"), SectionArr.Num(), *DisplayName), Data);
}

// ---------------------------------------------------------------------------
// set_editor_preference — reflection on UObject settings class
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_SetEditorPreference(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString PropName, Value, ClassName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("property_name"), PropName) || PropName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_editor_preference"), 1000, TEXT("Missing required param: 'property_name'"));
	if (!Params->TryGetStringField(TEXT("value"), Value))
		return MakeError(DOMAIN, TEXT("set_editor_preference"), 1000, TEXT("Missing required param: 'value'"));
	Params->TryGetStringField(TEXT("class_name"), ClassName);

	// Resolve settings class — try UnrealEd module first, then the caller's class_name
	UClass* SettingsClass = nullptr;
	if (!ClassName.IsEmpty())
	{
		// Try /Script/UnrealEd.ClassName first, then bare path
		SettingsClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UnrealEd.%s"), *ClassName));
		if (!SettingsClass)
			SettingsClass = FindObject<UClass>(nullptr, *ClassName);
	}
	if (!SettingsClass)
	{
		// Default: UEditorPerProjectUserSettings
		SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.EditorPerProjectUserSettings"));
	}
	if (!SettingsClass)
		return MakeError(DOMAIN, TEXT("set_editor_preference"), 3000,
			TEXT("Could not find editor settings class. Specify 'class_name' or ensure UnrealEd module is loaded."));

	UObject* Settings = GetMutableDefault<UObject>(SettingsClass);
	if (!Settings)
		return MakeError(DOMAIN, TEXT("set_editor_preference"), 3000,
			FString::Printf(TEXT("GetMutableDefault failed for class: %s"), *SettingsClass->GetName()));

	FProperty* Prop = SettingsClass->FindPropertyByName(FName(*PropName));
	if (!Prop)
		return MakeError(DOMAIN, TEXT("set_editor_preference"), 2000,
			FString::Printf(TEXT("Property '%s' not found on %s. Use reflection/list_properties to enumerate available properties."),
				*PropName, *SettingsClass->GetName()));

	void* PropPtr = Prop->ContainerPtrToValuePtr<void>(Settings);
	Prop->ImportText_Direct(*Value, PropPtr, nullptr, PPF_None);
	Settings->SaveConfig();

	return MakeSuccess(DOMAIN, TEXT("set_editor_preference"),
		FString::Printf(TEXT("%s.%s set to '%s'"), *SettingsClass->GetName(), *PropName, *Value));
#else
	return MakeError(DOMAIN, TEXT("set_editor_preference"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// get_editor_preference — reflection read
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_GetEditorPreference(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString PropName, ClassName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("property_name"), PropName) || PropName.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_editor_preference"), 1000, TEXT("Missing required param: 'property_name'"));
	Params->TryGetStringField(TEXT("class_name"), ClassName);

	UClass* SettingsClass = nullptr;
	if (!ClassName.IsEmpty())
	{
		SettingsClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UnrealEd.%s"), *ClassName));
		if (!SettingsClass)
			SettingsClass = FindObject<UClass>(nullptr, *ClassName);
	}
	if (!SettingsClass)
		SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.EditorPerProjectUserSettings"));
	if (!SettingsClass)
		return MakeError(DOMAIN, TEXT("get_editor_preference"), 3000, TEXT("Could not find editor settings class"));

	const UObject* Settings = GetDefault<UObject>(SettingsClass);
	FProperty* Prop = SettingsClass->FindPropertyByName(FName(*PropName));
	if (!Prop)
		return MakeError(DOMAIN, TEXT("get_editor_preference"), 2000,
			FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *SettingsClass->GetName()));

	FString ExportedValue;
	const void* PropPtr = Prop->ContainerPtrToValuePtr<void>(Settings);
	Prop->ExportText_Direct(ExportedValue, PropPtr, nullptr, nullptr, PPF_None);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("property_name"), PropName);
	Data->SetStringField(TEXT("value"),         ExportedValue);
	Data->SetStringField(TEXT("class"),         SettingsClass->GetName());
	Data->SetStringField(TEXT("type"),          Prop->GetCPPType());

	return MakeSuccess(DOMAIN, TEXT("get_editor_preference"),
		FString::Printf(TEXT("%s.%s = %s"), *SettingsClass->GetName(), *PropName, *ExportedValue), Data);
#else
	return MakeError(DOMAIN, TEXT("get_editor_preference"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// set_viewport_setting — apply to all FEditorViewportClient instances
// ---------------------------------------------------------------------------

FBridgeResult UEditorPrefsHandler::Action_SetViewportSetting(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString Setting, Value;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("setting"), Setting) || Setting.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_viewport_setting"), 1000,
			TEXT("Missing required param: 'setting' — valid: realtime, show_fps, show_stats, fov"));
	if (!Params->TryGetStringField(TEXT("value"), Value))
		return MakeError(DOMAIN, TEXT("set_viewport_setting"), 1000, TEXT("Missing required param: 'value'"));

	if (!GEditor)
		return MakeError(DOMAIN, TEXT("set_viewport_setting"), 3000, TEXT("GEditor not available"));

	const TArray<FEditorViewportClient*>& Clients = GEditor->GetAllViewportClients();
	if (Clients.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_viewport_setting"), 3000,
			TEXT("No active viewport clients found"));

	if (Setting != TEXT("realtime") && Setting != TEXT("show_fps") &&
		Setting != TEXT("show_stats") && Setting != TEXT("fov"))
	{
		return MakeError(DOMAIN, TEXT("set_viewport_setting"), 1000,
			FString::Printf(TEXT("Unknown viewport setting '%s'. Valid: realtime, show_fps, show_stats, fov"), *Setting));
	}

	int32 Modified = 0;
	for (FEditorViewportClient* VC : Clients)
	{
		if (!VC) continue;

		if (Setting == TEXT("realtime"))
		{
			VC->SetRealtime(Value.ToBool());
		}
		else if (Setting == TEXT("show_fps"))
		{
			// FPS display is toggled via ShowFlags
			VC->EngineShowFlags.SetMaterials(true); // Ensure viewport is active
			VC->SetShowStats(Value.ToBool());
		}
		else if (Setting == TEXT("show_stats"))
		{
			VC->SetShowStats(Value.ToBool());
		}
		else if (Setting == TEXT("fov"))
		{
			const float FOV = FCString::Atof(*Value);
			if (FOV < 5.f || FOV > 170.f)
			{
				return MakeError(DOMAIN, TEXT("set_viewport_setting"), 1000,
					FString::Printf(TEXT("FOV value %.1f out of range (5..170)"), FOV));
			}
			VC->ViewFOV = FOV;
			VC->Invalidate();
		}
		Modified++;
	}

	return MakeSuccess(DOMAIN, TEXT("set_viewport_setting"),
		FString::Printf(TEXT("Applied %s=%s to %d viewport(s)"), *Setting, *Value, Modified));
#else
	return MakeError(DOMAIN, TEXT("set_viewport_setting"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UEditorPrefsHandler::GetActionSchemas() const
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

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read a value from an ini config file"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("section"),  P(TEXT("string"), true,  TEXT("Ini section e.g. /Script/Engine.Engine")));
	  Pr->SetObjectField(TEXT("key"),      P(TEXT("string"), true,  TEXT("Key name")));
	  Pr->SetObjectField(TEXT("ini_file"), P(TEXT("string"), false, TEXT("DefaultEngine|DefaultGame|DefaultEditor|DefaultInput (default: DefaultEngine)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_ini_value"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Write a value to an ini config file and flush to disk"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("section"),  P(TEXT("string"), true,  TEXT("Ini section")));
	  Pr->SetObjectField(TEXT("key"),      P(TEXT("string"), true,  TEXT("Key name")));
	  Pr->SetObjectField(TEXT("value"),    P(TEXT("string"), true,  TEXT("Value to write")));
	  Pr->SetObjectField(TEXT("ini_file"), P(TEXT("string"), false, TEXT("DefaultEngine|DefaultGame|DefaultEditor|DefaultInput")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ini_value"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all section names in an ini file"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("ini_file"), P(TEXT("string"), false, TEXT("DefaultEngine|DefaultGame|DefaultEditor|DefaultInput (default: DefaultEngine)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_ini_sections"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set an editor settings property by name via UObject reflection, then SaveConfig()"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("property_name"), P(TEXT("string"), true,  TEXT("Property name on the settings object")));
	  Pr->SetObjectField(TEXT("value"),         P(TEXT("string"), true,  TEXT("New value as string")));
	  Pr->SetObjectField(TEXT("class_name"),    P(TEXT("string"), false, TEXT("Settings class (default: EditorPerProjectUserSettings)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_editor_preference"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read an editor settings property by name via UObject reflection"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("property_name"), P(TEXT("string"), true,  TEXT("Property name")));
	  Pr->SetObjectField(TEXT("class_name"),    P(TEXT("string"), false, TEXT("Settings class (default: EditorPerProjectUserSettings)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_editor_preference"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Apply a viewport setting to all active editor viewports"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("setting"), P(TEXT("string"), true, TEXT("realtime|show_fps|show_stats|fov")));
	  Pr->SetObjectField(TEXT("value"),   P(TEXT("string"), true, TEXT("true/false for bool settings; numeric for fov (5..170)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_viewport_setting"), A); }

	return Root;
}
