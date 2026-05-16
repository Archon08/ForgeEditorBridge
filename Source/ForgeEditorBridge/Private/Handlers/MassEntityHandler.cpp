#include "Handlers/MassEntityHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// ---- Mass Entity types -----------------------------------------------------
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassEntityTypes.h"

// ---- StateTree -------------------------------------------------------------
#include "StateTree.h"

// ---- Asset tools -----------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---- Transactions ----------------------------------------------------------
#include "ScopedTransaction.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Misc ------------------------------------------------------------------
#include "Misc/PackageName.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("mass_entity");

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
	return nullptr;
#endif
}

FBridgeResult UMassEntityHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- create_mass_config ----
	if (Action == TEXT("create_mass_config"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString Path, Name;
		AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Name.IsEmpty() || Path.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1001, TEXT("Invalid asset_path format. Expected: /Game/Path/AssetName"));

		// Check if asset already exists
		FString FullPath = AssetPath + TEXT(".") + Name;
		UObject* Existing = LoadObject<UObject>(nullptr, *FullPath);
		if (Existing)
			return this->MakeError(DOMAIN, Action, 2002, FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Mass Config")));

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UClass* ConfigClass = UMassEntityConfigAsset::StaticClass();

		UObject* NewAsset = AT.CreateAsset(Name, Path, ConfigClass, nullptr);
		if (!NewAsset)
			return this->MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to create Mass config asset at '%s'"), *AssetPath));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("class"), NewAsset->GetClass()->GetName());

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Created Mass config '%s' (%s)"), *Name, *NewAsset->GetClass()->GetName()),
			Data);
	}

	// ---- add_trait ----
	if (Action == TEXT("add_trait"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString TraitClassName;
		if (!Params->TryGetStringField(TEXT("trait_class"), TraitClassName) || TraitClassName.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'trait_class'"));

		// Load the config asset
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *AssetPath);
		if (!Config)
		{
			Config = LoadObject<UMassEntityConfigAsset>(nullptr, *FullPath);
		}
		if (!Config)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No UMassEntityConfigAsset found at '%s'"), *AssetPath),
				TEXT("Ensure the asset exists and is a Mass Entity Config"));

		// Find the trait class by name
		UClass* TraitClass = FindFirstObject<UClass>(*TraitClassName, EFindFirstObjectOptions::ExactClass);
		if (!TraitClass)
		{
			// Try with prefix
			FString PrefixedName = FString::Printf(TEXT("U%s"), *TraitClassName);
			TraitClass = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::ExactClass);
		}
		if (!TraitClass)
		{
			// Try loading by path
			TraitClass = LoadObject<UClass>(nullptr, *TraitClassName);
		}
		if (!TraitClass || !TraitClass->IsChildOf(UMassEntityTraitBase::StaticClass()))
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("Trait class '%s' not found or not a UMassEntityTraitBase subclass"), *TraitClassName),
				TEXT("Use the full class name, e.g. 'MassVisualizationTrait'"));

		// Duplicate guard — reject if the entity config already has a trait of this class.
		for (const UMassEntityTraitBase* ExistingTrait : Config->GetConfig().GetTraits())
		{
			if (ExistingTrait && ExistingTrait->GetClass() == TraitClass)
			{
				return this->MakeError(DOMAIN, Action, 1001,
					FString::Printf(TEXT("Trait '%s' is already present on this entity config."), *TraitClassName),
					TEXT("Remove the existing trait first if you want to re-add it with different settings."));
			}
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Add Mass Trait")));

		// Create an instance of the trait and add it to the config
		UMassEntityTraitBase* NewTrait = NewObject<UMassEntityTraitBase>(Config, TraitClass);
		if (!NewTrait)
			return this->MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to instantiate trait '%s'"), *TraitClassName));

		Config->GetMutableConfig().AddTrait(*NewTrait);
		Config->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("trait_class"), TraitClass->GetName());

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Added trait '%s' to config '%s'"), *TraitClass->GetName(), *AssetPath),
			Data);
	}

	// ---- create_state_tree ----
	if (Action == TEXT("create_state_tree"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString Path, Name;
		AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Name.IsEmpty() || Path.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1001, TEXT("Invalid asset_path format. Expected: /Game/Path/AssetName"));

		// Check if asset already exists
		FString FullPath = AssetPath + TEXT(".") + Name;
		UObject* Existing = LoadObject<UObject>(nullptr, *FullPath);
		if (Existing)
			return this->MakeError(DOMAIN, Action, 2002, FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create StateTree")));

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* NewAsset = AT.CreateAsset(Name, Path, UStateTree::StaticClass(), nullptr);
		if (!NewAsset)
			return this->MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to create StateTree asset at '%s'"), *AssetPath),
				TEXT("Ensure the StateTree plugin is enabled"));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Created StateTree '%s'"), *Name),
			Data);
	}

	// ---- add_state ----
	if (Action == TEXT("add_state"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString StateName;
		if (!Params->TryGetStringField(TEXT("state_name"), StateName) || StateName.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'state_name'"));

		FString ParentState;
		Params->TryGetStringField(TEXT("parent_state"), ParentState);

		FString StateType = TEXT("State");
		Params->TryGetStringField(TEXT("type"), StateType);

		// Load the entity config asset (not a UStateTree — states in Mass Entity context
		// belong to entity configs that include UMassStateTreeProcessor).
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *AssetPath);
		if (!Config)
			Config = LoadObject<UMassEntityConfigAsset>(nullptr, *FullPath);
		if (!Config)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No UMassEntityConfigAsset found at '%s'"), *AssetPath),
				TEXT("add_state requires a UMassEntityConfigAsset, not a UStateTree. "
				     "Use create_state_tree to create the StateTree asset, then reference it via a StateTree trait."));

		// Guard: surface a clean error if the entity config has no traits yet.
		if (Config->GetConfig().GetTraits().Num() == 0)
			return this->MakeError(DOMAIN, Action, 3003,
				TEXT("Entity config has no traits. Add a MassStateTreeTrait first."),
				TEXT("Call mass_entity/add_trait with trait_class='MassStateTreeTrait' then retry add_state."));

		// Guard: only use FMassStateTreeInstanceData when UMassStateTreeProcessor is present.
		// Using this fragment on an entity without the StateTree processor causes an ensure/crash on cook.
		UClass* StateTreeProcClass = FindFirstObject<UClass>(TEXT("MassStateTreeProcessor"), EFindFirstObjectOptions::NativeFirst);
		bool bHasStateTreeProcessor = false;

		for (const UMassEntityTraitBase* Trait : Config->GetConfig().GetTraits())
		{
			if (!Trait) continue;
			const FString ClassName = Trait->GetClass()->GetName();
			if (ClassName.Contains(TEXT("StateTree")) ||
			    (StateTreeProcClass && Trait->GetClass()->IsChildOf(StateTreeProcClass)))
			{
				bHasStateTreeProcessor = true;
				break;
			}
		}

		if (!bHasStateTreeProcessor)
			return this->MakeError(DOMAIN, Action, 3003,
				FString::Printf(TEXT("Entity config '%s' does not include UMassStateTreeProcessor. "
				     "Add a UMassStateTreeTrait (or equivalent) to the entity config first."), *AssetPath),
				TEXT("Use mass_entity/add_trait with trait_class='MassStateTreeTrait' then retry add_state"));

		// StateTree state addition requires UStateTreeEditorData::AddChildState() which is in
		// the StateTreeEditorModule. That module and its API are available in UE 5.7.
		// The entity config links to a StateTree via its trait — edit the linked StateTree directly.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"),  AssetPath);
		Data->SetStringField(TEXT("state_name"),  StateName);
		Data->SetStringField(TEXT("parent_state"), ParentState);
		Data->SetStringField(TEXT("type"),         StateType);
		Data->SetStringField(TEXT("note"),
			TEXT("StateTree processor guard passed. To add the child state call "
			     "state_tree/add_state on the UStateTree asset linked by the StateTree trait, "
			     "not on the entity config asset."));

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Guard passed for '%s': UMassStateTreeProcessor present. "
			     "Direct child-state insertion must be performed on the linked UStateTree asset."), *AssetPath),
			Data);
	}

	if (Action == TEXT("remove_trait"))        return Action_RemoveTrait(Params);
	if (Action == TEXT("list_traits"))         return Action_ListTraits(Params);
	if (Action == TEXT("get_config_info"))     return Action_GetConfigInfo(Params);
	if (Action == TEXT("configure_time_slice"))return Action_ConfigureTimeSlice(Params);
	if (Action == TEXT("create_entity_query")) return Action_CreateEntityQuery(Params);

	return this->MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'"), *Action),
		TEXT("mass_entity capabilities: create_mass_config, add_trait, remove_trait, create_state_tree, add_state, list_traits, get_config_info, configure_time_slice, create_entity_query"));
}

// ---------------------------------------------------------------------------
// remove_trait
// ---------------------------------------------------------------------------

FBridgeResult UMassEntityHandler::Action_RemoveTrait(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("remove_trait");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	FString TraitClassName;
	if (!Params->TryGetStringField(TEXT("trait_class"), TraitClassName) || TraitClassName.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'trait_class'"));

	// Load the config asset
	FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *AssetPath);
	if (!Config)
	{
		Config = LoadObject<UMassEntityConfigAsset>(nullptr, *FullPath);
	}
	if (!Config)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No UMassEntityConfigAsset found at '%s'"), *AssetPath),
			TEXT("Ensure the asset exists and is a Mass Entity Config"));

	// Resolve the trait class by name — same pattern as add_trait.
	UClass* NewTraitClass = FindFirstObject<UClass>(*TraitClassName, EFindFirstObjectOptions::ExactClass);
	if (!NewTraitClass)
	{
		FString PrefixedName = FString::Printf(TEXT("U%s"), *TraitClassName);
		NewTraitClass = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::ExactClass);
	}
	if (!NewTraitClass)
	{
		NewTraitClass = LoadObject<UClass>(nullptr, *TraitClassName);
	}
	if (!NewTraitClass || !NewTraitClass->IsChildOf(UMassEntityTraitBase::StaticClass()))
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("Trait class '%s' not found or not a UMassEntityTraitBase subclass"), *TraitClassName),
			TEXT("Use the full class name, e.g. 'MassVisualizationTrait'"));

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Remove Mass Trait")));

	// FMassEntityConfig::Traits is protected (TArray<TObjectPtr<UMassEntityTraitBase>>). Access via reflection
	// on the FMassEntityConfig struct — the `Traits` UPROPERTY is declared EditAnywhere,Instanced so it's
	// reachable through FProperty lookup.
	FMassEntityConfig& MutableCfg = Config->GetMutableConfig();
	UScriptStruct* CfgStruct = FMassEntityConfig::StaticStruct();
	FArrayProperty* TraitsProp = CastField<FArrayProperty>(CfgStruct->FindPropertyByName(TEXT("Traits")));
	if (!TraitsProp)
		return this->MakeError(DOMAIN, Action, 3003,
			TEXT("remove_trait: FMassEntityConfig::Traits UPROPERTY not found via reflection"),
			TEXT("Engine version mismatch — MassEntity plugin layout may have changed."));

	int32 Removed = 0;
	{
		FScriptArrayHelper ArrayHelper(TraitsProp, TraitsProp->ContainerPtrToValuePtr<void>(&MutableCfg));
		for (int32 i = ArrayHelper.Num() - 1; i >= 0; --i)
		{
			FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(TraitsProp->Inner);
			if (!InnerObjProp) break;
			UObject* Elem = InnerObjProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
			UMassEntityTraitBase* Trait = Cast<UMassEntityTraitBase>(Elem);
			if (Trait && Trait->GetClass() == NewTraitClass)
			{
				ArrayHelper.RemoveValues(i, 1);
				++Removed;
			}
		}
	}

	if (Removed == 0)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("Trait '%s' not found on this config"), *TraitClassName),
			TEXT("Use mass_entity/list_traits to see which traits are currently present."));

	Config->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),  AssetPath);
	Data->SetStringField(TEXT("trait_class"), NewTraitClass->GetName());
	Data->SetNumberField(TEXT("removed"),     Removed);

	return this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("Removed %d trait(s) of class '%s' from config '%s'"),
			Removed, *NewTraitClass->GetName(), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// list_traits
// ---------------------------------------------------------------------------

FBridgeResult UMassEntityHandler::Action_ListTraits(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("list_traits");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *AssetPath);
	if (!Config) Config = LoadObject<UMassEntityConfigAsset>(nullptr, *FullPath);
	if (!Config)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No UMassEntityConfigAsset found at '%s'"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> TraitsArr;
	for (const UMassEntityTraitBase* Trait : Config->GetConfig().GetTraits())
	{
		if (!Trait) continue;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class"), Trait->GetClass()->GetName());
		TraitsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("traits"), TraitsArr);
	Data->SetNumberField(TEXT("count"), TraitsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("list_traits: %d trait(s) on '%s'"), TraitsArr.Num(), *AssetPath));
	R.ExtraData = OutStr;
	return R;
}

// ---------------------------------------------------------------------------
// get_config_info
// ---------------------------------------------------------------------------

FBridgeResult UMassEntityHandler::Action_GetConfigInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_config_info");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *AssetPath);
	if (!Config) Config = LoadObject<UMassEntityConfigAsset>(nullptr, *FullPath);
	if (!Config)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No UMassEntityConfigAsset found at '%s'"), *AssetPath));

	const int32 TraitCount = Config->GetConfig().GetTraits().Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"),       Config->GetName());
	Data->SetNumberField(TEXT("trait_count"), TraitCount);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("get_config_info: '%s' — %d trait(s)"), *AssetPath, TraitCount));
	R.ExtraData = OutStr;
	return R;
}

// ---------------------------------------------------------------------------
// configure_time_slice — set Mass processing budget via reflection + GConfig
// ---------------------------------------------------------------------------

FBridgeResult UMassEntityHandler::Action_ConfigureTimeSlice(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("configure_time_slice");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	int32 MaxEntitiesPerTick = 1000;
	double MaxEntitiesNum = 0.0;
	if (Params->TryGetNumberField(TEXT("max_entities_per_tick"), MaxEntitiesNum))
		MaxEntitiesPerTick = FMath::Max(1, (int32)MaxEntitiesNum);

	double TimeBudgetMs = 16.0;
	Params->TryGetNumberField(TEXT("time_budget_ms"), TimeBudgetMs);

	// ---- Probe for UMassEntitySettings class via reflection -----------------
	UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/MassEntity.MassEntitySettings"));
	if (!SettingsClass)
	{
		SettingsClass = LoadObject<UClass>(nullptr, TEXT("/Script/MassEntity.MassEntitySettings"));
	}

	TArray<FString> AppliedProps;

	if (SettingsClass)
	{
		UObject* Settings = SettingsClass->GetDefaultObject(/*bCreateIfNeeded*/ true);
		if (Settings)
		{
			auto TrySetIntProp = [&](const TCHAR* PropName, int32 Value) -> bool
			{
				if (FProperty* P = SettingsClass->FindPropertyByName(FName(PropName)))
				{
					if (FIntProperty* IP = CastField<FIntProperty>(P))
					{
						IP->SetPropertyValue_InContainer(Settings, Value);
						AppliedProps.Add(FString::Printf(TEXT("%s=%d (reflection)"), PropName, Value));
						return true;
					}
				}
				return false;
			};
			auto TrySetFloatProp = [&](const TCHAR* PropName, float Value) -> bool
			{
				if (FProperty* P = SettingsClass->FindPropertyByName(FName(PropName)))
				{
					if (FFloatProperty* FP = CastField<FFloatProperty>(P))
					{
						FP->SetPropertyValue_InContainer(Settings, Value);
						AppliedProps.Add(FString::Printf(TEXT("%s=%.2f (reflection)"), PropName, Value));
						return true;
					}
					if (FDoubleProperty* DP = CastField<FDoubleProperty>(P))
					{
						DP->SetPropertyValue_InContainer(Settings, (double)Value);
						AppliedProps.Add(FString::Printf(TEXT("%s=%.2f (reflection)"), PropName, Value));
						return true;
					}
				}
				return false;
			};

			// Try any of a set of known/plausible property names
			const TCHAR* IntCandidates[] = { TEXT("MaxEntitiesPerTick"), TEXT("MaxEntityCountPerChunk"), TEXT("ChunkSize") };
			const TCHAR* FloatCandidates[] = { TEXT("TimeBudgetMs"), TEXT("MaxProcessingTimeMs"), TEXT("ProcessingTimeBudgetMs") };

			bool bAnyIntSet = false, bAnyFloatSet = false;
			for (const TCHAR* N : IntCandidates)   { if (TrySetIntProp(N, MaxEntitiesPerTick))  { bAnyIntSet = true; break; } }
			for (const TCHAR* N : FloatCandidates) { if (TrySetFloatProp(N, (float)TimeBudgetMs)){ bAnyFloatSet = true; break; } }
			(void)bAnyIntSet; (void)bAnyFloatSet;
		}
	}
	else
	{
		return this->MakeError(DOMAIN, Action, 3003,
			TEXT("configure_time_slice requires MassEntity module and UMassEntitySettings"),
			TEXT("Ensure the MassEntity plugin is enabled and MassEntity module is linked."));
	}

	// ---- Always write to GConfig as a fallback / persistent layer -----------
	const TCHAR* Section = TEXT("/Script/MassEntity.MassEntitySettings");
	if (GConfig)
	{
		GConfig->SetInt(Section, TEXT("MaxEntityCountPerChunk"), MaxEntitiesPerTick, GEngineIni);
		GConfig->SetFloat(Section, TEXT("TimeBudgetMs"), (float)TimeBudgetMs, GEngineIni);
		GConfig->Flush(false, GEngineIni);
		AppliedProps.Add(FString::Printf(TEXT("GConfig[%s] MaxEntityCountPerChunk=%d"), Section, MaxEntitiesPerTick));
		AppliedProps.Add(FString::Printf(TEXT("GConfig[%s] TimeBudgetMs=%.2f"), Section, TimeBudgetMs));
	}

	// ---- Build result data --------------------------------------------------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("max_entities_per_tick"), MaxEntitiesPerTick);
	Data->SetNumberField(TEXT("time_budget_ms"), TimeBudgetMs);

	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	for (const FString& A : AppliedProps) AppliedArr.Add(MakeShared<FJsonValueString>(A));
	Data->SetArrayField(TEXT("applied"), AppliedArr);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("configure_time_slice: %d setting(s) applied (max=%d, budget=%.2fms)"),
			AppliedProps.Num(), MaxEntitiesPerTick, TimeBudgetMs));
	R.ExtraData = OutStr;
	return R;
}

// ---------------------------------------------------------------------------
// create_entity_query — descriptor JSON (FMassEntityQuery is code-only)
// ---------------------------------------------------------------------------

FBridgeResult UMassEntityHandler::Action_CreateEntityQuery(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_entity_query");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path' (used as query_name if no asset exists)"));

	TArray<FString> RequiredFragments;
	const TArray<TSharedPtr<FJsonValue>>* ReqArr = nullptr;
	if (Params->TryGetArrayField(TEXT("required_fragments"), ReqArr) && ReqArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *ReqArr)
		{
			if (V.IsValid() && V->Type == EJson::String) RequiredFragments.Add(V->AsString());
		}
	}

	TArray<FString> OptionalFragments;
	const TArray<TSharedPtr<FJsonValue>>* OptArr = nullptr;
	if (Params->TryGetArrayField(TEXT("optional_fragments"), OptArr) && OptArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *OptArr)
		{
			if (V.IsValid() && V->Type == EJson::String) OptionalFragments.Add(V->AsString());
		}
	}

	// Derive query_name from asset_path (use last path segment)
	FString QueryName = AssetPath;
	{
		FString L, R;
		if (AssetPath.Split(TEXT("/"), &L, &R, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !R.IsEmpty())
			QueryName = R;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("query_name"), QueryName);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> ReqOut;
	for (const FString& F : RequiredFragments) ReqOut.Add(MakeShared<FJsonValueString>(F));
	Data->SetArrayField(TEXT("required_fragments"), ReqOut);

	TArray<TSharedPtr<FJsonValue>> OptOut;
	for (const FString& F : OptionalFragments) OptOut.Add(MakeShared<FJsonValueString>(F));
	Data->SetArrayField(TEXT("optional_fragments"), OptOut);

	Data->SetStringField(TEXT("note"),
		TEXT("FMassEntityQuery is code-only; use this descriptor as a reference for C++ implementation"));

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("create_entity_query: descriptor for '%s' (%d required, %d optional) — FMassEntityQuery is code-only"),
			*QueryName, RequiredFragments.Num(), OptionalFragments.Num()));
	R.ExtraData = OutStr;
	return R;
}
