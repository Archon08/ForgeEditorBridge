#include "Handlers/GameplayTagHandler.h"
#include "ForgeAISubsystem.h"

// ---- Gameplay Tags ---------------------------------------------------------
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsSettings.h"

// ---- File I/O (INI write) --------------------------------------------------
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UGameplayTagHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("gameplay_tag"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("add_tag_to_config")) return Action_AddTagToConfig(Params);
	if (Action == TEXT("rename_tag"))        return Action_RenameTag(Params);
	if (Action == TEXT("list_tags"))         return Action_ListTags(Params);
	if (Action == TEXT("remove_tag"))        return Action_RemoveTag(Params);
	if (Action == TEXT("validate_tags"))     return Action_ValidateTags(Params);
	if (Action == TEXT("get_tag_hierarchy")) return Action_GetTagHierarchy(Params);

	FBridgeResult R = CreateResult(TEXT("gameplay_tag"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown gameplay_tag action '%s'. Valid: add_tag_to_config, rename_tag, list_tags, remove_tag, validate_tags, get_tag_hierarchy"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// add_tag_to_config
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_AddTagToConfig(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("add_tag_to_config"));

	FString Tag, Comment, Source;
	if (!Params->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
	{
		Result.Message = TEXT("add_tag_to_config: 'tag' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("comment"), Comment);
	Params->TryGetStringField(TEXT("source"),  Source);

	const FName SourceName = Source.IsEmpty() ? NAME_None : FName(*Source);

	UGameplayTagsManager& GTM = UGameplayTagsManager::Get();

	// Check if tag already exists
	if (GTM.RequestGameplayTag(FName(*Tag), false).IsValid())
	{
		Result.bSuccess = true;
		Result.Message  = FString::Printf(TEXT("add_tag_to_config: tag '%s' already registered (no-op)"), *Tag);
		return Result;
	}

	// Write tag to DefaultGameplayTags.ini
	const FString IniPath = FPaths::ProjectConfigDir() / TEXT("DefaultGameplayTags.ini");
	const FString Section = TEXT("/Script/GameplayTags.GameplayTagsSettings");
	const FString Key     = TEXT("+GameplayTagList");
	const FString Entry   = FString::Printf(TEXT("(Tag=\"%s\",DevComment=\"%s\")"), *Tag, *Comment);

	// Append the tag entry to the INI
	GConfig->SetString(*Section, *Key, *Entry, IniPath);
	GConfig->Flush(false, IniPath);

	// Reload tags so the manager picks up the new entry
	GTM.EditorRefreshGameplayTagTree();

	Result.bSuccess = true;
	Result.Message = FString::Printf(TEXT("add_tag_to_config: tag '%s' added to DefaultGameplayTags.ini"), *Tag);
	return Result;
}

// ---------------------------------------------------------------------------
// rename_tag
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_RenameTag(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("rename_tag"));

	FString OldName, NewName;
	if (!Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		Result.Message = TEXT("rename_tag: 'old_name' and 'new_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UGameplayTagsManager& GTM = UGameplayTagsManager::Get();

	// Write the redirect regardless of whether the tag is currently registered
	// (it may have been added via INI this session and not yet refreshed)
	// This is how tag renames are persisted — via GameplayTagRedirects
	const FString IniPath = FPaths::ProjectConfigDir() / TEXT("DefaultGameplayTags.ini");
	const FString Section = TEXT("/Script/GameplayTags.GameplayTagsSettings");
	const FString RedirectEntry = FString::Printf(
		TEXT("(OldTagName=\"%s\",NewTagName=\"%s\")"), *OldName, *NewName);

	GConfig->SetString(*Section, TEXT("+GameplayTagRedirects"), *RedirectEntry, IniPath);
	GConfig->Flush(false, IniPath);

	// Also add the new tag if it doesn't exist
	if (!GTM.RequestGameplayTag(FName(*NewName), false).IsValid())
	{
		const FString TagEntry = FString::Printf(TEXT("(Tag=\"%s\",DevComment=\"Renamed from %s\")"), *NewName, *OldName);
		GConfig->SetString(*Section, TEXT("+GameplayTagList"), *TagEntry, IniPath);
		GConfig->Flush(false, IniPath);
	}

	GTM.EditorRefreshGameplayTagTree();

	Result.bSuccess = true;
	Result.Message = FString::Printf(
		TEXT("rename_tag: redirect added %s → %s in DefaultGameplayTags.ini"), *OldName, *NewName);
	return Result;
}

// ---------------------------------------------------------------------------
// list_tags
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_ListTags(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("list_tags"));

	FString Prefix;
	Params->TryGetStringField(TEXT("prefix"), Prefix);

	FGameplayTagContainer AllTags;
	UGameplayTagsManager::Get().RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/false);

	TArray<TSharedPtr<FJsonValue>> JsonTags;
	for (const FGameplayTag& T : AllTags)
	{
		const FString TagStr = T.ToString();
		if (Prefix.IsEmpty() || TagStr.StartsWith(Prefix))
		{
			JsonTags.Add(MakeShared<FJsonValueString>(TagStr));
		}
	}

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	JsonObj->SetArrayField(TEXT("tags"),  JsonTags);
	JsonObj->SetNumberField(TEXT("count"), JsonTags.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("Found %d tag(s)%s"),
		JsonTags.Num(),
		Prefix.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" with prefix '%s'"), *Prefix));
	return Result;
}

// ---------------------------------------------------------------------------
// remove_tag
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_RemoveTag(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("remove_tag"));

	FString Tag;
	if (!Params->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
	{
		Result.Message = TEXT("remove_tag: 'tag' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	const FString IniPath = FPaths::ProjectConfigDir() / TEXT("DefaultGameplayTags.ini");
	const FString Section = TEXT("/Script/GameplayTags.GameplayTagsSettings");

	TArray<FString> Entries;
	GConfig->GetArray(*Section, TEXT("+GameplayTagList"), Entries, IniPath);

	int32 RemovedCount = 0;
	TArray<FString> Remaining;
	for (const FString& Entry : Entries)
	{
		if (Entry.Contains(Tag))
		{
			++RemovedCount;
		}
		else
		{
			Remaining.Add(Entry);
		}
	}

	if (RemovedCount == 0)
	{
		Result.bSuccess = true;
		Result.Message  = FString::Printf(TEXT("remove_tag: tag '%s' not found in INI (no-op)"), *Tag);
		return Result;
	}

	GConfig->RemoveKey(*Section, TEXT("+GameplayTagList"), IniPath);
	for (const FString& Entry : Remaining)
	{
		GConfig->SetString(*Section, TEXT("+GameplayTagList"), *Entry, IniPath);
	}
	GConfig->Flush(false, IniPath);

	UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("remove_tag: removed '%s' from DefaultGameplayTags.ini (%d entries removed)"),
		*Tag, RemovedCount);
	return Result;
}

// ---------------------------------------------------------------------------
// validate_tags
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_ValidateTags(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("validate_tags"));

	const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("tags"), TagsArray) || !TagsArray || TagsArray->Num() == 0)
	{
		Result.Message = TEXT("validate_tags: 'tags' array is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UGameplayTagsManager& GTM = UGameplayTagsManager::Get();

	FGameplayTagContainer AllTags;
	GTM.RequestAllGameplayTags(AllTags, false);

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 ValidCount = 0;
	for (const TSharedPtr<FJsonValue>& Val : *TagsArray)
	{
		FString TagStr;
		Val->TryGetString(TagStr);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag"), TagStr);

		const bool bValid = GTM.RequestGameplayTag(FName(*TagStr), false).IsValid();
		Entry->SetBoolField(TEXT("valid"), bValid);

		if (!bValid)
		{
			TArray<FString> Parts;
			TagStr.ParseIntoArray(Parts, TEXT("."));
			TArray<FString> Suggestions;
			if (Parts.Num() > 0)
			{
				for (const FGameplayTag& T : AllTags)
				{
					const FString TS = T.ToString();
					if (TS.StartsWith(Parts[0]) && Suggestions.Num() < 3)
					{
						Suggestions.Add(TS);
					}
				}
			}
			TArray<TSharedPtr<FJsonValue>> SugArr;
			for (const FString& S : Suggestions) SugArr.Add(MakeShared<FJsonValueString>(S));
			Entry->SetArrayField(TEXT("suggestions"), SugArr);
		}
		else
		{
			++ValidCount;
		}

		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetNumberField(TEXT("total"), TagsArray->Num());
	Data->SetNumberField(TEXT("valid_count"), ValidCount);
	Data->SetNumberField(TEXT("invalid_count"), TagsArray->Num() - ValidCount);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("validate_tags: %d/%d tags valid"), ValidCount, TagsArray->Num());
	return Result;
}

// ---------------------------------------------------------------------------
// get_tag_hierarchy
// ---------------------------------------------------------------------------

FBridgeResult UGameplayTagHandler::Action_GetTagHierarchy(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gameplay_tag"), TEXT("get_tag_hierarchy"));

	FString Root;
	Params->TryGetStringField(TEXT("root"), Root);

	FGameplayTagContainer AllTags;
	UGameplayTagsManager::Get().RequestAllGameplayTags(AllTags, false);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const FGameplayTag& T : AllTags)
	{
		const FString TagStr = T.ToString();
		if (!Root.IsEmpty() && !TagStr.StartsWith(Root))
		{
			continue;
		}

		int32 Depth = 0;
		for (TCHAR C : TagStr) { if (C == TEXT('.')) ++Depth; }

		int32 ChildCount = 0;
		const FString Prefix = TagStr + TEXT(".");
		for (const FGameplayTag& Other : AllTags)
		{
			const FString OtherStr = Other.ToString();
			if (OtherStr.StartsWith(Prefix))
			{
				int32 OtherDots = 0;
				for (TCHAR C : OtherStr) { if (C == TEXT('.')) ++OtherDots; }
				if (OtherDots == Depth + 1) ++ChildCount;
			}
		}

		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("tag"), TagStr);
		Node->SetNumberField(TEXT("depth"), Depth);
		Node->SetNumberField(TEXT("children_count"), ChildCount);
		Node->SetBoolField(TEXT("is_leaf"), ChildCount == 0);
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("nodes"), Nodes);
	Data->SetNumberField(TEXT("total"), Nodes.Num());
	if (!Root.IsEmpty()) Data->SetStringField(TEXT("root_filter"), Root);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("get_tag_hierarchy: %d nodes%s"),
		Nodes.Num(), Root.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" under '%s'"), *Root));
	return Result;
}
