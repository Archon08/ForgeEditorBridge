#include "Handlers/StringTableHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    UStringTable* LoadStringTableAt(const FString& AssetPath)
    {
        UStringTable* ST = LoadObject<UStringTable>(nullptr, *AssetPath);
        if (!ST)
        {
            FString Path, Name;
            if (AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
            {
                const FString FullPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);
                ST = LoadObject<UStringTable>(nullptr, *FullPath);
            }
        }
        return ST;
    }
}

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("string_table");

FBridgeResult UStringTableHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("create_string_table"))
	{
		if (!Params->HasField(TEXT("asset_path")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'asset_path'"), TEXT("Provide asset_path e.g. /Game/Data/ST_MyTable"));
		}

		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		const FString Namespace = Params->HasField(TEXT("namespace")) ? Params->GetStringField(TEXT("namespace")) : FString();

		FString Path, Name;
		if (!AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return MakeError(DOMAIN, Action, 1001, TEXT("Invalid asset_path format — could not split into path and name"), TEXT("Use format /Game/Folder/AssetName"));
		}

		FString PackagePath = FString::Printf(TEXT("%s/%s"), *Path, *Name);
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create package"), TEXT("Check that the path is valid"));
		}

		UStringTable* ST = NewObject<UStringTable>(Package, *Name, RF_Public | RF_Standalone);
		if (!ST)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("Failed to create UStringTable object"), TEXT("Check engine logs for details"));
		}

		if (!Namespace.IsEmpty())
		{
			ST->GetMutableStringTable()->SetNamespace(Namespace);
		}

		ST->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(ST);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		if (!Namespace.IsEmpty())
		{
			Data->SetStringField(TEXT("namespace"), Namespace);
		}

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Created string table '%s'"), *AssetPath), Data);
	}

	if (Action == TEXT("add_entry"))
	{
		if (!Params->HasField(TEXT("asset_path")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'asset_path'"), TEXT("Provide asset_path"));
		}
		if (!Params->HasField(TEXT("key")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'key'"), TEXT("Provide key"));
		}
		if (!Params->HasField(TEXT("value")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'value'"), TEXT("Provide value"));
		}

		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		const FString Key = Params->GetStringField(TEXT("key"));
		const FString Value = Params->GetStringField(TEXT("value"));

		UStringTable* ST = LoadObject<UStringTable>(nullptr, *AssetPath);
		if (!ST)
		{
			// Fallback: try appending ".AssetName"
			FString Path, Name;
			if (AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				FString FullPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);
				ST = LoadObject<UStringTable>(nullptr, *FullPath);
			}
		}
		if (!ST)
		{
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("String table not found at '%s'"), *AssetPath), TEXT("Verify asset_path or create it first"));
		}

		ST->GetMutableStringTable()->SetSourceString(Key, Value);
		ST->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("key"), Key);
		Data->SetStringField(TEXT("value"), Value);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Set entry '%s' in '%s'"), *Key, *AssetPath), Data);
	}

	if (Action == TEXT("get_entry"))
	{
		if (!Params->HasField(TEXT("asset_path")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'asset_path'"), TEXT("Provide asset_path"));
		}
		if (!Params->HasField(TEXT("key")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'key'"), TEXT("Provide key"));
		}

		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		const FString Key = Params->GetStringField(TEXT("key"));

		UStringTable* ST = LoadObject<UStringTable>(nullptr, *AssetPath);
		if (!ST)
		{
			FString Path, Name;
			if (AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				FString FullPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);
				ST = LoadObject<UStringTable>(nullptr, *FullPath);
			}
		}
		if (!ST)
		{
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("String table not found at '%s'"), *AssetPath), TEXT("Verify asset_path or create it first"));
		}

		FStringTableEntryConstPtr Entry = ST->GetStringTable()->FindEntry(Key);
		if (!Entry.IsValid())
		{
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("Key '%s' not found in string table '%s'"), *Key, *AssetPath), TEXT("Check key spelling or list_entries first"));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("key"), Key);
		Data->SetStringField(TEXT("value"), Entry->GetSourceString());

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Retrieved entry '%s'"), *Key), Data);
	}

	if (Action == TEXT("list_entries"))
	{
		if (!Params->HasField(TEXT("asset_path")))
		{
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param 'asset_path'"), TEXT("Provide asset_path"));
		}

		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

		UStringTable* ST = LoadObject<UStringTable>(nullptr, *AssetPath);
		if (!ST)
		{
			FString Path, Name;
			if (AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				FString FullPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);
				ST = LoadObject<UStringTable>(nullptr, *FullPath);
			}
		}
		if (!ST)
		{
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("String table not found at '%s'"), *AssetPath), TEXT("Verify asset_path or create it first"));
		}

		TSharedPtr<FJsonObject> EntriesObj = MakeShared<FJsonObject>();
		int32 Count = 0;

		ST->GetStringTable()->EnumerateSourceStrings([&](const FString& EntryKey, const FString& EntryValue) -> bool
		{
			EntriesObj->SetStringField(EntryKey, EntryValue);
			Count++;
			return true;
		});

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("entries"), EntriesObj);
		Data->SetNumberField(TEXT("count"), Count);

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Found %d entries in '%s'"), Count, *AssetPath), Data);
	}

	if (Action == TEXT("remove_entry"))
	{
		FString AssetPath, Key;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
		if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'key' is required"));
		UStringTable* ST = LoadStringTableAt(AssetPath);
		if (!ST) return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("String table not found at '%s'"), *AssetPath));
		const bool bHad = ST->GetStringTable()->FindEntry(Key).IsValid();
		ST->GetMutableStringTable()->RemoveSourceString(Key);
		ST->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action,
			bHad
				? FString::Printf(TEXT("Removed entry '%s' from '%s'"), *Key, *AssetPath)
				: FString::Printf(TEXT("Key '%s' not present (no-op)"), *Key));
	}

	if (Action == TEXT("update_entry"))
	{
		// Functional alias for add_entry — SetSourceString does upsert. We surface
		// it as a distinct verb so callers can express the intent explicitly.
		FString AssetPath, Key, Value;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
		if (!Params->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'key' is required"));
		if (!Params->TryGetStringField(TEXT("value"), Value))
			return MakeError(DOMAIN, Action, 1000, TEXT("'value' is required"));
		UStringTable* ST = LoadStringTableAt(AssetPath);
		if (!ST) return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("String table not found at '%s'"), *AssetPath));
		if (!ST->GetStringTable()->FindEntry(Key).IsValid())
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("Key '%s' does not exist — use add_entry to create"), *Key),
				TEXT("update_entry only succeeds on existing keys"));
		ST->GetMutableStringTable()->SetSourceString(Key, Value);
		ST->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Updated entry '%s' in '%s'"), *Key, *AssetPath));
	}

	if (Action == TEXT("import_csv"))
	{
		FString AssetPath, CsvPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
		if (!Params->TryGetStringField(TEXT("csv_path"), CsvPath) || CsvPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'csv_path' is required (absolute filesystem path)"));
		bool bReplace = false;
		Params->TryGetBoolField(TEXT("replace_existing"), bReplace);

		UStringTable* ST = LoadStringTableAt(AssetPath);
		if (!ST) return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("String table not found at '%s'"), *AssetPath));

		FString CsvText;
		if (!FFileHelper::LoadFileToString(CsvText, *CsvPath))
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("Could not read CSV file: %s"), *CsvPath));

		FStringTableRef Mut = ST->GetMutableStringTable();
		if (bReplace)
		{
			TArray<FString> ExistingKeys;
			ST->GetStringTable()->EnumerateSourceStrings([&](const FString& K, const FString&) -> bool
			{
				ExistingKeys.Add(K); return true;
			});
			for (const FString& K : ExistingKeys) Mut->RemoveSourceString(K);
		}

		TArray<FString> Lines;
		CsvText.ParseIntoArrayLines(Lines, true);
		int32 Imported = 0, Skipped = 0;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			// Skip header row when first line looks like Key,Value
			if (i == 0 && Lines[i].StartsWith(TEXT("Key"), ESearchCase::IgnoreCase)) continue;
			FString K, V;
			if (!Lines[i].Split(TEXT(","), &K, &V)) { ++Skipped; continue; }
			K = K.TrimQuotes().TrimStartAndEnd();
			V = V.TrimQuotes().TrimStartAndEnd();
			if (K.IsEmpty()) { ++Skipped; continue; }
			Mut->SetSourceString(K, V);
			++Imported;
		}
		ST->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("imported"), Imported);
		Data->SetNumberField(TEXT("skipped"), Skipped);
		Data->SetBoolField(TEXT("replaced_existing"), bReplace);
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Imported %d row(s) into '%s' (skipped %d)"), Imported, *AssetPath, Skipped),
			Data);
	}

	if (Action == TEXT("export_csv"))
	{
		FString AssetPath, CsvPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));
		if (!Params->TryGetStringField(TEXT("csv_path"), CsvPath) || CsvPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'csv_path' is required (absolute filesystem path)"));
		UStringTable* ST = LoadStringTableAt(AssetPath);
		if (!ST) return MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("String table not found at '%s'"), *AssetPath));

		FString Out = TEXT("Key,Value\n");
		int32 Count = 0;
		ST->GetStringTable()->EnumerateSourceStrings([&](const FString& K, const FString& V) -> bool
		{
			FString EK = K; EK.ReplaceInline(TEXT("\""), TEXT("\"\""));
			FString EV = V; EV.ReplaceInline(TEXT("\""), TEXT("\"\""));
			Out += FString::Printf(TEXT("\"%s\",\"%s\"\n"), *EK, *EV);
			++Count;
			return true;
		});
		if (!FFileHelper::SaveStringToFile(Out, *CsvPath))
			return MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to write CSV: %s"), *CsvPath));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("rows"), Count);
		Data->SetStringField(TEXT("csv_path"), CsvPath);
		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Exported %d entries to '%s'"), Count, *CsvPath), Data);
	}

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("string_table capabilities"));
}

TSharedPtr<FJsonObject> UStringTableHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new UStringTable asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the string table"))); Ps->SetObjectField(TEXT("namespace"), P(TEXT("string"), false, TEXT("Localization namespace"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_string_table"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add or update an entry in a string table")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the string table"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("Entry key"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Entry source string value"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_entry"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get a single entry from a string table")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the string table"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("Entry key to retrieve"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_entry"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all entries in a string table")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the string table"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("list_entries"), A); }

	return Root;
}
