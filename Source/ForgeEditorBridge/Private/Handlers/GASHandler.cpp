#include "Handlers/GASHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

// ---- GAS -------------------------------------------------------------------
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "AttributeSet.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"

// ---- GE Tag Components (UE 5.3+ architecture) ------------------------------
// Tags are no longer direct UGameplayEffect properties; they live on
// UGameplayEffectComponent subclasses stored in GE->GameplayEffectComponents.
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AbilitiesGameplayEffectComponent.h"
// Note: UE 5.7 has no SourceTagRequirementsGameplayEffectComponent.
// Source tag requirements are not modelled as a GE component in this engine version.

// ---- Gameplay Abilities ----------------------------------------------------
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayAbilitySpec.h"

// ---- Blueprint editing (configure_asc) -------------------------------------
#include "Kismet2/BlueprintEditorUtils.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---- File I/O (create_attribute_set) ---------------------------------------
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

// ---- Capture ---------------------------------------------------------------
#include "Capture/ForgeGASCapture.h"
#include "Serialization/JsonReader.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UGASHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("gas"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_gameplay_effect"))  return Action_CreateGameplayEffect(Params);
	if (Action == TEXT("create_gameplay_ability")) return Action_CreateGameplayAbility(Params);
	if (Action == TEXT("add_ge_modifier"))         return Action_AddGEModifier(Params);
	if (Action == TEXT("set_ge_tags"))            return Action_SetGETags(Params);
	if (Action == TEXT("create_gameplay_cue"))    return Action_CreateGameplayCue(Params);
	if (Action == TEXT("create_attribute_set"))   return Action_CreateAttributeSet(Params);
	if (Action == TEXT("set_ge_duration"))        return Action_SetGEDuration(Params);
	if (Action == TEXT("set_ge_period"))          return Action_SetGEPeriod(Params);
	if (Action == TEXT("set_ge_stacking"))        return Action_SetGEStacking(Params);
	if (Action == TEXT("get_ge_info"))             return Action_GetGEInfo(Params);
	if (Action == TEXT("read_audit"))              return Action_ReadAudit(Params);
	if (Action == TEXT("read_gas_capture"))           return Action_ReadGASCapture(Params);
	if (Action == TEXT("list_abilities_on_class"))    return Action_ListAbilitiesOnClass(Params);
	if (Action == TEXT("get_attribute_defaults"))     return Action_GetAttributeDefaults(Params);
	if (Action == TEXT("create_ability_task"))        return Action_CreateAbilityTask(Params);
	if (Action == TEXT("configure_asc"))              return Action_ConfigureASC(Params);
	if (Action == TEXT("add_ability_to_asc"))         return Action_AddAbilityToASC(Params);
	if (Action == TEXT("remove_ability_from_asc"))    return Action_RemoveAbilityFromASC(Params);
	if (Action == TEXT("set_ability_tags"))           return Action_SetAbilityTags(Params);
	// Phase 1c — aliases
	if (Action == TEXT("create_ability"))             return Action_CreateGameplayAbility(Params);
	if (Action == TEXT("add_modifier_to_ge"))         return Action_AddGEModifier(Params);
	// Phase 1c — new actions
	if (Action == TEXT("add_ability_cost"))           return Action_AddAbilityCost(Params);
	if (Action == TEXT("set_ability_cooldown"))       return Action_SetAbilityCooldown(Params);
	if (Action == TEXT("add_granted_ability"))        return Action_AddGrantedAbility(Params);
	if (Action == TEXT("list_attribute_sets"))        return Action_ListAttributeSets(Params);
	// Phase 0 gap — PIE-guarded runtime actions
	if (Action == TEXT("get_active_effects"))         return Action_GetActiveEffects(Params);
	if (Action == TEXT("apply_gameplay_effect"))      return Action_ApplyGameplayEffect(Params);
	if (Action == TEXT("remove_ge_modifier"))         return Action_RemoveGEModifier(Params);

	FBridgeResult R = CreateResult(TEXT("gas"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown gas action '%s'. Use system/capabilities for the full list."), *Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// read_gas_capture
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_ReadGASCapture(TSharedPtr<FJsonObject> Params)
{
	if (Subsystem->GASCapture)
		Subsystem->GASCapture->ExportGASAudit();

	FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("gas/audit.json"));
	FString FileContent;
	FBridgeResult Res = MakeSuccess(GetDomainName(), TEXT("read_gas_capture"), TEXT("Capture complete"));
	if (FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
			Res.Data = JsonObj;
	}
	else
	{
		Res.Message = FString::Printf(
			TEXT("Capture complete — file not yet available for reading at: %s"), *FilePath);
	}
	return Res;
}

// ---------------------------------------------------------------------------
// create_gameplay_effect
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_CreateGameplayEffect(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("create_gameplay_effect"));

	FString AssetPath, DurationPolicyStr;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_gameplay_effect: 'asset_path' is required (e.g. '/Game/GAS/GE_Burn')");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("duration_policy"), DurationPolicyStr);

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UBlueprintFactory* Factory  = NewObject<UBlueprintFactory>();
	Factory->ParentClass        = UGameplayEffect::StaticClass();

	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UBlueprint::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_gameplay_effect: failed to create asset at '%s' (path may already exist or be invalid)"),
			*AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UBlueprint* NewBP = CastChecked<UBlueprint>(CreatedAsset);

	// Compile so GeneratedClass and CDO are valid
	FKismetEditorUtilities::CompileBlueprint(NewBP);

	if (NewBP->GeneratedClass)
	{
		if (UGameplayEffect* CDO = Cast<UGameplayEffect>(NewBP->GeneratedClass->GetDefaultObject()))
		{
			if (DurationPolicyStr == TEXT("Infinite"))
				CDO->DurationPolicy = EGameplayEffectDurationType::Infinite;
			else if (DurationPolicyStr == TEXT("HasDuration"))
				CDO->DurationPolicy = EGameplayEffectDurationType::HasDuration;
			else
				CDO->DurationPolicy = EGameplayEffectDurationType::Instant;
		}
	}

	NewBP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(NewBP);  // recompile to flush CDO changes

	const FString DPLabel = DurationPolicyStr.IsEmpty() ? TEXT("Instant") : *DurationPolicyStr;
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("GameplayEffect blueprint created at %s (DurationPolicy=%s)"), *AssetPath, *DPLabel);
	return Result;
}

// ---------------------------------------------------------------------------
// add_ge_modifier
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_AddGEModifier(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("add_ge_modifier"));

	FString AssetPath, AttributeStr, OpStr;
	double  Magnitude = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("add_ge_modifier: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("attribute"), AttributeStr) || AttributeStr.IsEmpty())
	{
		Result.Message = TEXT("add_ge_modifier: 'attribute' required (format: 'UMyAttributeSet.Health')");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("op"), OpStr))
	{
		Result.Message = TEXT("add_ge_modifier: 'op' required (Add|Multiply|Override)");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetNumberField(TEXT("magnitude"), Magnitude);

	UBlueprint* BP = nullptr;
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	// Parse "ClassName.PropertyName"
	FString ClassName, PropName;
	if (!AttributeStr.Split(TEXT("."), &ClassName, &PropName) || ClassName.IsEmpty() || PropName.IsEmpty())
	{
		Result.Message = FString::Printf(
			TEXT("add_ge_modifier: 'attribute' must be 'ClassName.PropertyName', got '%s'"), *AttributeStr);
		Result.ErrorCode = 1001;
		return Result;
	}

	// Resolve attribute set class — search loaded classes for an UAttributeSet subclass
	UClass* AttrClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ClassName && It->IsChildOf(UAttributeSet::StaticClass()))
		{
			AttrClass = *It;
			break;
		}
	}
	if (!AttrClass)
	{
		Result.Message = FString::Printf(
			TEXT("add_ge_modifier: AttributeSet class '%s' not found among loaded classes"), *ClassName);
		Result.ErrorCode = 2000;
		return Result;
	}

	FProperty* Prop = FindFProperty<FProperty>(AttrClass, *PropName);
	if (!Prop)
	{
		Result.Message = FString::Printf(
			TEXT("add_ge_modifier: Property '%s' not found on class '%s'"), *PropName, *ClassName);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Map op string → EGameplayModOp (note: UE API has intentional "Multiplicitive" typo)
	EGameplayModOp::Type ModOp = EGameplayModOp::Additive;
	if (OpStr == TEXT("Multiply"))  ModOp = EGameplayModOp::Multiplicitive;
	if (OpStr == TEXT("Override"))  ModOp = EGameplayModOp::Override;

	FGameplayModifierInfo ModInfo;
	ModInfo.Attribute         = FGameplayAttribute(Prop);
	ModInfo.ModifierOp        = ModOp;
	// FScalableFloat constructor from float for UE 5.7
	ModInfo.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat((float)Magnitude));

	CDO->Modifiers.Add(ModInfo);

	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Modifier added: %s %s magnitude=%.4f on %s"),
		*AttributeStr, *OpStr, Magnitude, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_ge_tags  (UE 5.3+ / 5.7 — component-based tag architecture)
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetGETags(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("set_ge_tags"));

	FString AssetPath, TagType;
	const TArray<TSharedPtr<FJsonValue>>* TagsJsonArray = nullptr;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("set_ge_tags: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetArrayField(TEXT("tags"), TagsJsonArray) || !TagsJsonArray)
	{
		Result.Message = TEXT("set_ge_tags: 'tags' string array is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("tag_type"), TagType);
	if (TagType.IsEmpty()) TagType = TEXT("Owned");

	const bool bValidType = (TagType == TEXT("Owned") || TagType == TEXT("Granted") ||
	                         TagType == TEXT("Required") || TagType == TEXT("Ignored"));
	if (!bValidType)
	{
		Result.Message = FString::Printf(
			TEXT("set_ge_tags: tag_type '%s' is invalid. Valid values: Owned, Granted, Required, Ignored."),
			*TagType);
		Result.ErrorCode = 1001;
		return Result;
	}

	UBlueprint* BP = nullptr;
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	// Build tag container from string array
	FGameplayTagContainer TagContainer;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	int32 SkippedCount = 0;
	for (const TSharedPtr<FJsonValue>& TagVal : *TagsJsonArray)
	{
		FString TagStr;
		if (!TagVal->TryGetString(TagStr) || TagStr.IsEmpty()) continue;

		FGameplayTag Tag = TagsMgr.RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
			TagContainer.AddTag(Tag);
		else
			++SkippedCount;
	}

	if (TagContainer.Num() == 0 && SkippedCount > 0)
	{
		Result.Message = FString::Printf(
			TEXT("set_ge_tags: all %d provided tag(s) were skipped — none are registered in the project. "
			     "Register tags via DefaultGameplayTags.ini or the Gameplay Tags editor first."),
			SkippedCount);
		Result.ErrorCode = 1001;
		return Result;
	}

	// ---- UE 5.3+ tag component architecture --------------------------------
	if (TagType == TEXT("Owned"))
	{
		// Asset tags: own tags on the GE asset itself
		UAssetTagsGameplayEffectComponent& Comp = CDO->FindOrAddComponent<UAssetTagsGameplayEffectComponent>();
		FInheritedTagContainer Changes;
		Changes.Added = TagContainer;
		Comp.SetAndApplyAssetTagChanges(Changes);
	}
	else if (TagType == TEXT("Granted"))
	{
		// Tags granted to the Target actor when the effect is applied
		UTargetTagsGameplayEffectComponent& Comp = CDO->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();
		FInheritedTagContainer Changes;
		Changes.Added = TagContainer;
		Comp.SetAndApplyTargetTagChanges(Changes);
	}
	else if (TagType == TEXT("Required"))
	{
		// Target must have these tags for the GE to apply
		UTargetTagRequirementsGameplayEffectComponent& Comp =
			CDO->FindOrAddComponent<UTargetTagRequirementsGameplayEffectComponent>();
		FGameplayTagRequirements& Reqs = Comp.ApplicationTagRequirements;
		for (const FGameplayTag& Tag : TagContainer)
			Reqs.RequireTags.AddTag(Tag);
	}
	else // Ignored
	{
		// UE 5.7 has no USourceTagRequirementsGameplayEffectComponent.
		// Source-tag blocking is not available as a GE component in this engine version.
		return MakeError(TEXT("gas"), TEXT("set_ge_tags"), 3003,
			TEXT("set_ge_tags: tag_type 'Ignored' (source tag requirements) is not supported in UE 5.7 — "
			     "no SourceTagRequirementsGameplayEffectComponent exists. "
			     "Use UImmunityGameplayEffectComponent with a FGameplayEffectQuery instead."),
			TEXT("Use tag_type 'Required' or 'Granted', or handle source immunity via a separate GE with Immunity component."));
	}

	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	Result.Message = FString::Printf(
		TEXT("set_ge_tags: applied %d '%s' tag(s) to '%s'"),
		TagContainer.Num(), *TagType, *AssetPath);
	Result.bSuccess    = true;
	Result.AffectedPath = AssetPath;
	return Result;
}

// ---------------------------------------------------------------------------
// create_gameplay_cue
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_CreateGameplayCue(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("create_gameplay_cue"));

	FString AssetPath, CueType;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_gameplay_cue: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("cue_type"), CueType);
	if (CueType.IsEmpty()) CueType = TEXT("Static");

	UClass* ParentClass = nullptr;
	if (CueType == TEXT("Actor"))
		ParentClass = AGameplayCueNotify_Actor::StaticClass();
	else
		ParentClass = UGameplayCueNotify_Static::StaticClass();

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UBlueprintFactory* Factory  = NewObject<UBlueprintFactory>();
	Factory->ParentClass        = ParentClass;

	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UBlueprint::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_gameplay_cue: failed to create asset at '%s' (path may already exist)"),
			*AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	UBlueprint* NewBP = CastChecked<UBlueprint>(CreatedAsset);
	FKismetEditorUtilities::CompileBlueprint(NewBP);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("GameplayCue blueprint created at %s (cue_type=%s, parent=%s)"),
		*AssetPath, *CueType, *ParentClass->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// create_attribute_set  (scaffolds C++ header + source in Source/)
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_CreateAttributeSet(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("create_attribute_set"));

	FString ClassName, ModuleName;
	const TArray<TSharedPtr<FJsonValue>>* AttrsJson = nullptr;

	if (!Params->TryGetStringField(TEXT("class_name"),   ClassName)   || ClassName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("module_name"),  ModuleName)  || ModuleName.IsEmpty())
	{
		Result.Message = TEXT("create_attribute_set: 'class_name' and 'module_name' are required. "
		                      "Optional: 'attributes' (string[]), 'api_macro', 'rel_path_h', 'rel_path_cpp'");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Normalise class name: strip leading 'U' for file naming; ensure header/source names match
	FString BaseName = ClassName;
	if (BaseName.StartsWith(TEXT("U"))) BaseName = BaseName.RightChop(1);
	const FString FullClassName = ClassName.StartsWith(TEXT("U")) ? ClassName : (TEXT("U") + ClassName);

	// Optional: override paths
	FString RelPathH, RelPathCpp;
	if (!Params->TryGetStringField(TEXT("rel_path_h"),   RelPathH))
		RelPathH   = FString::Printf(TEXT("%s/Public/%s.h"),   *ModuleName, *BaseName);
	if (!Params->TryGetStringField(TEXT("rel_path_cpp"), RelPathCpp))
		RelPathCpp = FString::Printf(TEXT("%s/Private/%s.cpp"), *ModuleName, *BaseName);

	// Optional: API export macro (e.g. "MYGAME_API")
	FString ApiMacro;
	Params->TryGetStringField(TEXT("api_macro"), ApiMacro);
	const FString ApiPrefix = ApiMacro.IsEmpty() ? TEXT("") : (ApiMacro + TEXT(" "));

	// Collect attributes
	TArray<FString> Attributes;
	if (Params->TryGetArrayField(TEXT("attributes"), AttrsJson) && AttrsJson)
	{
		for (const TSharedPtr<FJsonValue>& V : *AttrsJson)
		{
			FString AttrName;
			if (V->TryGetString(AttrName) && !AttrName.IsEmpty())
				Attributes.Add(AttrName);
		}
	}
	if (Attributes.IsEmpty()) Attributes.Add(TEXT("Value")); // guaranteed non-empty boilerplate

	// ---- Build header -------------------------------------------------------
	FString HeaderGuard = (ModuleName + TEXT("_") + BaseName).ToUpper() + TEXT("_H");

	FString HContent;
	HContent += FString::Printf(TEXT("#pragma once\n"));
	HContent += FString::Printf(TEXT("#include \"CoreMinimal.h\"\n"));
	HContent += FString::Printf(TEXT("#include \"AttributeSet.h\"\n"));
	HContent += FString::Printf(TEXT("#include \"AbilitySystemComponent.h\"\n"));
	HContent += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *BaseName);

	HContent += TEXT(
		"// Convenience macro — exposes property getter + value getter/setter/initter\n"
		"#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \\\n"
		"\tGAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \\\n"
		"\tGAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \\\n"
		"\tGAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \\\n"
		"\tGAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)\n\n"
	);

	HContent += FString::Printf(TEXT("UCLASS()\n"));
	HContent += FString::Printf(TEXT("class %s%s : public UAttributeSet\n{\n\tGENERATED_BODY()\n\npublic:\n"),
	                             *ApiPrefix, *FullClassName);
	HContent += FString::Printf(TEXT("\t%s();\n\n"), *FullClassName);
	HContent += TEXT("\tvirtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\n\n");

	for (const FString& Attr : Attributes)
	{
		HContent += FString::Printf(
			TEXT("\tUPROPERTY(BlueprintReadOnly, Category = \"Attributes\", ReplicatedUsing = OnRep_%s)\n"
			     "\tFGameplayAttributeData %s;\n"
			     "\tATTRIBUTE_ACCESSORS(%s, %s)\n\n"
			     "\tUFUNCTION()\n"
			     "\tvirtual void OnRep_%s(const FGameplayAttributeData& OldValue);\n\n"),
			*Attr, *Attr, *FullClassName, *Attr, *Attr);
	}
	HContent += TEXT("};\n");

	// ---- Build source -------------------------------------------------------
	FString CppContent;
	CppContent += FString::Printf(TEXT("#include \"%s.h\"\n"), *BaseName);
	CppContent += TEXT("#include \"Net/UnrealNetwork.h\"\n");
	CppContent += TEXT("#include \"GameplayEffectExtension.h\"\n\n");

	CppContent += FString::Printf(TEXT("%s::%s() {}\n\n"), *FullClassName, *FullClassName);
	CppContent += FString::Printf(
		TEXT("void %s::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const\n{\n"
		     "\tSuper::GetLifetimeReplicatedProps(OutLifetimeProps);\n"),
		*FullClassName);
	for (const FString& Attr : Attributes)
	{
		CppContent += FString::Printf(
			TEXT("\tDOREPLIFETIME_CONDITION_NOTIFY(%s, %s, COND_None, REPNOTIFY_Always);\n"),
			*FullClassName, *Attr);
	}
	CppContent += TEXT("}\n\n");

	for (const FString& Attr : Attributes)
	{
		CppContent += FString::Printf(
			TEXT("void %s::OnRep_%s(const FGameplayAttributeData& OldValue)\n{\n"
			     "\tGAMEPLAYATTRIBUTE_REPNOTIFY(%s, %s, OldValue);\n}\n\n"),
			*FullClassName, *Attr, *FullClassName, *Attr);
	}

	// ---- Write files --------------------------------------------------------
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	auto WriteFile = [&](const FString& RelPath, const FString& Content, FString& OutAbsPath) -> bool
	{
		if (RelPath.Contains(TEXT("..")) || !FPaths::IsRelative(RelPath)) return false;
		OutAbsPath = FPaths::ConvertRelativePathToFull(SourceRoot / RelPath);
		if (!OutAbsPath.StartsWith(SourceRoot)) return false;
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString Dir = FPaths::GetPath(OutAbsPath);
		if (!PF.DirectoryExists(*Dir)) PF.CreateDirectoryTree(*Dir);
		return FFileHelper::SaveStringToFile(Content, *OutAbsPath,
		                                     FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};

	FString AbsH, AbsCpp;
	if (!WriteFile(RelPathH,   HContent,   AbsH))
	{
		Result.Message = FString::Printf(
			TEXT("create_attribute_set: failed to write header to '%s'"), *RelPathH);
		Result.ErrorCode = 3000;
		return Result;
	}
	if (!WriteFile(RelPathCpp, CppContent, AbsCpp))
	{
		Result.Message = FString::Printf(
			TEXT("create_attribute_set: failed to write source to '%s'"), *RelPathCpp);
		Result.ErrorCode = 3000;
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = AbsH;
	Result.Message      = FString::Printf(
		TEXT("AttributeSet scaffolded — %s (%d attributes):\n  H:   %s\n  CPP: %s"),
		*FullClassName, Attributes.Num(), *AbsH, *AbsCpp);
	return Result;
}

// ---------------------------------------------------------------------------
// LoadGEBlueprint — helper
// ---------------------------------------------------------------------------

UGameplayEffect* UGASHandler::LoadGEBlueprint(const FString& AssetPath,
                                               UBlueprint*&    OutBP,
                                               FBridgeResult&  Result)
{
	OutBP = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!OutBP)
	{
		// Try a suffix fallback: '/Game/Path/Name' -> '/Game/Path/Name.Name'
		const FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		OutBP = LoadObject<UBlueprint>(nullptr, *FullPath);
	}
	if (!OutBP)
	{
		Result.Message = FString::Printf(TEXT("LoadGEBlueprint: no Blueprint at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		return nullptr;
	}

	// Compile if GeneratedClass is missing or stale
	if (!OutBP->GeneratedClass || !OutBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(OutBP);
	}

	if (!OutBP->GeneratedClass)
	{
		Result.Message = FString::Printf(
			TEXT("LoadGEBlueprint: Blueprint at '%s' has no GeneratedClass after compile"), *AssetPath);
		Result.ErrorCode = 2000;
		return nullptr;
	}

	UGameplayEffect* CDO = Cast<UGameplayEffect>(OutBP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		Result.Message = FString::Printf(
			TEXT("LoadGEBlueprint: GeneratedClass of '%s' is not a UGameplayEffect"), *AssetPath);
		Result.ErrorCode = 2001;
		return nullptr;
	}
	return CDO;
}

// ===========================================================================
// Domain constant
// ===========================================================================

static const FString GAS_DOMAIN = TEXT("gas");

// ---------------------------------------------------------------------------
// set_ge_duration
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetGEDuration(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ge_duration");

	FString AssetPath, DurationPolicyStr;
	double DurationMagnitude = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetStringField(TEXT("duration_policy"), DurationPolicyStr) || DurationPolicyStr.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000,
			TEXT("Missing required param: 'duration_policy' (Instant|Infinite|HasDuration)"));

	bool bHasMagnitude = Params->TryGetNumberField(TEXT("duration_magnitude"), DurationMagnitude);

	UBlueprint* BP = nullptr;
	FBridgeResult Result = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	// Set duration policy
	if (DurationPolicyStr == TEXT("Instant"))
		CDO->DurationPolicy = EGameplayEffectDurationType::Instant;
	else if (DurationPolicyStr == TEXT("Infinite"))
		CDO->DurationPolicy = EGameplayEffectDurationType::Infinite;
	else if (DurationPolicyStr == TEXT("HasDuration"))
	{
		CDO->DurationPolicy = EGameplayEffectDurationType::HasDuration;
		if (bHasMagnitude)
		{
			CDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat((float)DurationMagnitude));
		}
	}
	else
	{
		return MakeError(GAS_DOMAIN, Action, 1001,
			FString::Printf(TEXT("Invalid duration_policy '%s'. Must be Instant, Infinite, or HasDuration"), *DurationPolicyStr),
			TEXT("Use one of: Instant, Infinite, HasDuration"));
	}

	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("duration_policy"), DurationPolicyStr);
	if (bHasMagnitude)
		Data->SetNumberField(TEXT("duration_magnitude"), DurationMagnitude);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Duration set to %s%s on '%s'"),
			*DurationPolicyStr,
			(DurationPolicyStr == TEXT("HasDuration") && bHasMagnitude)
				? *FString::Printf(TEXT(" (magnitude=%.2f)"), DurationMagnitude) : TEXT(""),
			*AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_ge_period
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetGEPeriod(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ge_period");

	FString AssetPath;
	double Period = 0.0;
	bool bExecuteOnApplication = false;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetNumberField(TEXT("period"), Period))
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'period' (float, seconds)"));
	Params->TryGetBoolField(TEXT("execute_on_application"), bExecuteOnApplication);

	UBlueprint* BP = nullptr;
	FBridgeResult Result = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	CDO->Period.Value = (float)Period;
	CDO->bExecutePeriodicEffectOnApplication = bExecuteOnApplication;

	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("period"), Period);
	Data->SetBoolField(TEXT("execute_on_application"), bExecuteOnApplication);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Period set to %.3fs (execute_on_application=%s) on '%s'"),
			Period, bExecuteOnApplication ? TEXT("true") : TEXT("false"), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_ge_stacking
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetGEStacking(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ge_stacking");

	FString AssetPath, StackingTypeStr, DurationRefreshStr, PeriodResetStr;
	double StackLimitD = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetStringField(TEXT("stacking_type"), StackingTypeStr) || StackingTypeStr.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000,
			TEXT("Missing required param: 'stacking_type' (None|AggregateBySource|AggregateByTarget)"));

	bool bHasLimit = Params->TryGetNumberField(TEXT("stack_limit"), StackLimitD);
	Params->TryGetStringField(TEXT("duration_refresh"), DurationRefreshStr);
	Params->TryGetStringField(TEXT("period_reset"), PeriodResetStr);

	UBlueprint* BP = nullptr;
	FBridgeResult Result = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	// Set stacking type (UE 5.7: getter/setter not exported, use deprecated member directly)
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (StackingTypeStr == TEXT("None"))
		CDO->StackingType = EGameplayEffectStackingType::None;
	else if (StackingTypeStr == TEXT("AggregateBySource"))
		CDO->StackingType = EGameplayEffectStackingType::AggregateBySource;
	else if (StackingTypeStr == TEXT("AggregateByTarget"))
		CDO->StackingType = EGameplayEffectStackingType::AggregateByTarget;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	else
		return MakeError(GAS_DOMAIN, Action, 1001,
			FString::Printf(TEXT("Invalid stacking_type '%s'"), *StackingTypeStr),
			TEXT("Use one of: None, AggregateBySource, AggregateByTarget"));

	if (bHasLimit)
		CDO->StackLimitCount = (int32)StackLimitD;

	// Duration refresh policy
	if (!DurationRefreshStr.IsEmpty())
	{
		if (DurationRefreshStr == TEXT("RefreshOnSuccessfulApplication"))
			CDO->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
		else if (DurationRefreshStr == TEXT("NeverRefresh"))
			CDO->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::NeverRefresh;
		else
			return MakeError(GAS_DOMAIN, Action, 1001,
				FString::Printf(TEXT("Invalid duration_refresh '%s'"), *DurationRefreshStr),
				TEXT("Use: RefreshOnSuccessfulApplication or NeverRefresh"));
	}

	// Period reset policy
	if (!PeriodResetStr.IsEmpty())
	{
		if (PeriodResetStr == TEXT("ResetOnSuccessfulApplication"))
			CDO->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;
		else if (PeriodResetStr == TEXT("NeverReset"))
			CDO->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
		else
			return MakeError(GAS_DOMAIN, Action, 1001,
				FString::Printf(TEXT("Invalid period_reset '%s'"), *PeriodResetStr),
				TEXT("Use: ResetOnSuccessfulApplication or NeverReset"));
	}

	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("stacking_type"), StackingTypeStr);
	if (bHasLimit) Data->SetNumberField(TEXT("stack_limit"), StackLimitD);
	if (!DurationRefreshStr.IsEmpty()) Data->SetStringField(TEXT("duration_refresh"), DurationRefreshStr);
	if (!PeriodResetStr.IsEmpty()) Data->SetStringField(TEXT("period_reset"), PeriodResetStr);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Stacking set to %s (limit=%d) on '%s'"),
			*StackingTypeStr, (int32)StackLimitD, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// get_ge_info
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_GetGEInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_ge_info");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	UBlueprint* BP = nullptr;
	FBridgeResult Result = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class"), CDO->GetClass()->GetName());

	// Duration policy
	FString DPStr;
	switch (CDO->DurationPolicy)
	{
	case EGameplayEffectDurationType::Instant:     DPStr = TEXT("Instant"); break;
	case EGameplayEffectDurationType::Infinite:    DPStr = TEXT("Infinite"); break;
	case EGameplayEffectDurationType::HasDuration: DPStr = TEXT("HasDuration"); break;
	default: DPStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("duration_policy"), DPStr);

	// Duration magnitude (only meaningful for HasDuration)
	if (CDO->DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		// Extract the scalable float value
		float DurMag = 0.f;
		CDO->DurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, DurMag);
		Data->SetNumberField(TEXT("duration_magnitude"), DurMag);
	}

	// Period
	Data->SetNumberField(TEXT("period"), CDO->Period.Value);
	Data->SetBoolField(TEXT("execute_periodic_on_application"), CDO->bExecutePeriodicEffectOnApplication);

	// Stacking
	TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
	FString StackTypeStr;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	switch (CDO->StackingType)
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	{
	case EGameplayEffectStackingType::None:              StackTypeStr = TEXT("None"); break;
	case EGameplayEffectStackingType::AggregateBySource: StackTypeStr = TEXT("AggregateBySource"); break;
	case EGameplayEffectStackingType::AggregateByTarget: StackTypeStr = TEXT("AggregateByTarget"); break;
	default: StackTypeStr = TEXT("Unknown"); break;
	}
	StackObj->SetStringField(TEXT("type"), StackTypeStr);
	StackObj->SetNumberField(TEXT("limit"), CDO->StackLimitCount);
	Data->SetObjectField(TEXT("stacking"), StackObj);

	// Modifiers
	TArray<TSharedPtr<FJsonValue>> ModArray;
	for (const FGameplayModifierInfo& Mod : CDO->Modifiers)
	{
		TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
		ModObj->SetStringField(TEXT("attribute"), Mod.Attribute.GetName());

		FString OpStr;
		switch (Mod.ModifierOp)
		{
		case EGameplayModOp::Additive:        OpStr = TEXT("Add"); break;
		case EGameplayModOp::Multiplicitive:  OpStr = TEXT("Multiply"); break;
		case EGameplayModOp::Override:         OpStr = TEXT("Override"); break;
		case EGameplayModOp::Division:         OpStr = TEXT("Division"); break;
		default: OpStr = FString::Printf(TEXT("Op_%d"), (int32)Mod.ModifierOp); break;
		}
		ModObj->SetStringField(TEXT("op"), OpStr);

		// Try to get the magnitude value
		float MagValue = 0.0f;
		Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.0f, MagValue);
		ModObj->SetNumberField(TEXT("magnitude"), MagValue);

		ModArray.Add(MakeShared<FJsonValueObject>(ModObj));
	}
	Data->SetArrayField(TEXT("modifiers"), ModArray);

	// Tags — read from InheritableOwnedTagsContainer on the CDO
	TArray<TSharedPtr<FJsonValue>> TagsArray;
	{
		const FGameplayTagContainer& GrantedTags = CDO->GetGrantedTags();
		for (const FGameplayTag& Tag : GrantedTags)
		{
			TagsArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
	}
	Data->SetArrayField(TEXT("tags"), TagsArray);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("GE info for '%s': %s, %d modifier(s), %d tag(s)"),
			*AssetPath, *DPStr, CDO->Modifiers.Num(), TagsArray.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// create_gameplay_ability
// Params: asset_path (string), parent_class (string, optional), tags (string[], optional)
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_CreateGameplayAbility(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_gameplay_ability");

	FString AssetPath, ParentClassStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	Params->TryGetStringField(TEXT("parent_class"), ParentClassStr);

	// Resolve parent class — default to UGameplayAbility if not specified
	UClass* ParentClass = UGameplayAbility::StaticClass();
	if (!ParentClassStr.IsEmpty())
	{
		UClass* Resolved = LoadClass<UGameplayAbility>(nullptr, *ParentClassStr);
		if (!Resolved)
			return MakeError(GAS_DOMAIN, Action, 2000,
				FString::Printf(TEXT("Could not load parent_class '%s' as UGameplayAbility subclass"), *ParentClassStr));
		ParentClass = Resolved;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UBlueprintFactory* Factory  = NewObject<UBlueprintFactory>();
	Factory->ParentClass        = ParentClass;

	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UBlueprint::StaticClass(), Factory);
	if (!CreatedAsset)
		return MakeError(GAS_DOMAIN, Action, 2002,
			FString::Printf(TEXT("Failed to create ability asset at '%s' (path may already exist)"), *AssetPath));

	UBlueprint* NewBP = CastChecked<UBlueprint>(CreatedAsset);
	FKismetEditorUtilities::CompileBlueprint(NewBP);

	// Apply ability tags from the optional 'tags' array
	const TArray<TSharedPtr<FJsonValue>>* TagsJsonArray = nullptr;
	int32 AppliedTagCount = 0;
	if (NewBP->GeneratedClass && Params->TryGetArrayField(TEXT("tags"), TagsJsonArray) && TagsJsonArray)
	{
		if (UGameplayAbility* CDO = Cast<UGameplayAbility>(NewBP->GeneratedClass->GetDefaultObject()))
		{
			UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
			for (const TSharedPtr<FJsonValue>& TagVal : *TagsJsonArray)
			{
				FString TagStr;
				if (!TagVal->TryGetString(TagStr) || TagStr.IsEmpty()) continue;
				FGameplayTag Tag = TagsMgr.RequestGameplayTag(FName(*TagStr), false);
				if (Tag.IsValid())
				{
					CDO->EditorGetAssetTags().AddTag(Tag);
					++AppliedTagCount;
				}
			}

			if (AppliedTagCount > 0)
			{
				NewBP->MarkPackageDirty();
				FKismetEditorUtilities::CompileBlueprint(NewBP);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),   AssetPath);
	Data->SetStringField(TEXT("parent_class"),  ParentClass->GetName());
	Data->SetNumberField(TEXT("tags_applied"),  (double)AppliedTagCount);

	FBridgeResult R = MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("GameplayAbility '%s' created (parent: %s, %d tag(s) applied)"),
			*AssetName, *ParentClass->GetName(), AppliedTagCount),
		Data);
	R.AffectedPath = AssetPath;
	return R;
}

// ---------------------------------------------------------------------------
// read_audit
// Params: asset_path (string)
// Returns a comprehensive audit of a GE: tags per category, all modifiers, duration, stacking.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_ReadAudit(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("read_audit");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	UBlueprint* BP = nullptr;
	FBridgeResult Result = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* CDO = LoadGEBlueprint(AssetPath, BP, Result);
	if (!CDO) return Result;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class"),      CDO->GetClass()->GetName());

	// ---- Duration ----------------------------------------------------------
	FString DPStr;
	switch (CDO->DurationPolicy)
	{
	case EGameplayEffectDurationType::Instant:     DPStr = TEXT("Instant");     break;
	case EGameplayEffectDurationType::Infinite:    DPStr = TEXT("Infinite");    break;
	case EGameplayEffectDurationType::HasDuration: DPStr = TEXT("HasDuration"); break;
	default: DPStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("duration_policy"), DPStr);
	if (CDO->DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		float DurMag = 0.f;
		CDO->DurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, DurMag);
		Data->SetNumberField(TEXT("duration_magnitude"), DurMag);
	}

	// ---- Period ------------------------------------------------------------
	TSharedPtr<FJsonObject> PeriodObj = MakeShared<FJsonObject>();
	PeriodObj->SetNumberField(TEXT("value"),                 CDO->Period.Value);
	PeriodObj->SetBoolField  (TEXT("execute_on_application"), CDO->bExecutePeriodicEffectOnApplication);
	Data->SetObjectField(TEXT("period"), PeriodObj);

	// ---- Stacking ----------------------------------------------------------
	FString StackTypeStr;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	switch (CDO->StackingType)
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	{
	case EGameplayEffectStackingType::None:              StackTypeStr = TEXT("None"); break;
	case EGameplayEffectStackingType::AggregateBySource: StackTypeStr = TEXT("AggregateBySource"); break;
	case EGameplayEffectStackingType::AggregateByTarget: StackTypeStr = TEXT("AggregateByTarget"); break;
	default: StackTypeStr = TEXT("Unknown"); break;
	}
	TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
	StackObj->SetStringField(TEXT("type"),  StackTypeStr);
	StackObj->SetNumberField(TEXT("limit"), CDO->StackLimitCount);
	Data->SetObjectField(TEXT("stacking"), StackObj);

	// ---- Modifiers ---------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ModArray;
	for (const FGameplayModifierInfo& Mod : CDO->Modifiers)
	{
		TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
		ModObj->SetStringField(TEXT("attribute"), Mod.Attribute.GetName());
		FString OpStr;
		switch (Mod.ModifierOp)
		{
		case EGameplayModOp::Additive:       OpStr = TEXT("Add");      break;
		case EGameplayModOp::Multiplicitive: OpStr = TEXT("Multiply"); break;
		case EGameplayModOp::Override:       OpStr = TEXT("Override"); break;
		case EGameplayModOp::Division:       OpStr = TEXT("Divide");   break;
		default: OpStr = FString::Printf(TEXT("Op_%d"), (int32)Mod.ModifierOp); break;
		}
		ModObj->SetStringField(TEXT("op"), OpStr);
		float MagValue = 0.f;
		Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, MagValue);
		ModObj->SetNumberField(TEXT("magnitude"), MagValue);
		ModArray.Add(MakeShared<FJsonValueObject>(ModObj));
	}
	Data->SetArrayField(TEXT("modifiers"), ModArray);

	// ---- Tags per category (read from GE components) -----------------------
	auto MakeTagArray = [](const FGameplayTagContainer& Container) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FGameplayTag& Tag : Container)
			Arr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		return Arr;
	};

	TSharedPtr<FJsonObject> TagsObj = MakeShared<FJsonObject>();

	// Owned (asset tags)
	if (const UAssetTagsGameplayEffectComponent* Comp =
		CDO->FindComponent<UAssetTagsGameplayEffectComponent>())
	{
		TagsObj->SetArrayField(TEXT("Owned"), MakeTagArray(Comp->GetConfiguredAssetTagChanges().CombinedTags));
	}
	else TagsObj->SetArrayField(TEXT("Owned"), TArray<TSharedPtr<FJsonValue>>());

	// Granted (target tags)
	if (const UTargetTagsGameplayEffectComponent* Comp =
		CDO->FindComponent<UTargetTagsGameplayEffectComponent>())
	{
		TagsObj->SetArrayField(TEXT("Granted"), MakeTagArray(Comp->GetConfiguredTargetTagChanges().CombinedTags));
	}
	else TagsObj->SetArrayField(TEXT("Granted"), TArray<TSharedPtr<FJsonValue>>());

	// Required (target requirements)
	if (const UTargetTagRequirementsGameplayEffectComponent* Comp =
		CDO->FindComponent<UTargetTagRequirementsGameplayEffectComponent>())
	{
		TagsObj->SetArrayField(TEXT("Required"), MakeTagArray(Comp->ApplicationTagRequirements.RequireTags));
	}
	else TagsObj->SetArrayField(TEXT("Required"), TArray<TSharedPtr<FJsonValue>>());

	// Ignored (source requirements) — not available in UE 5.7 as a GE component
	TagsObj->SetArrayField(TEXT("Ignored"), TArray<TSharedPtr<FJsonValue>>());

	Data->SetObjectField(TEXT("tags"), TagsObj);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Audit: '%s' — %s, %d modifier(s)"),
			*AssetPath, *DPStr, CDO->Modifiers.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UGASHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a UGameplayEffect Blueprint asset"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new GE"))); Pr->SetObjectField(TEXT("duration_policy"), P(TEXT("string"), false, TEXT("Instant, Infinite, or HasDuration (default Instant)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_gameplay_effect"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a UGameplayAbility Blueprint asset with optional parent class and ability tags"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new ability"))); Pr->SetObjectField(TEXT("parent_class"), P(TEXT("string"), false, TEXT("Full class path of parent UGameplayAbility (default UGameplayAbility)"))); Pr->SetObjectField(TEXT("tags"), P(TEXT("array<string>"), false, TEXT("Gameplay tags applied to CDO->AbilityTags"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_gameplay_ability"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a modifier to a GameplayEffect (attribute + operation + magnitude)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("attribute"), P(TEXT("string"), true, TEXT("ClassName.PropertyName (e.g. UMyAttributeSet.Health)"))); Pr->SetObjectField(TEXT("op"), P(TEXT("string"), true, TEXT("Add, Multiply, or Override"))); Pr->SetObjectField(TEXT("magnitude"), P(TEXT("float"), false, TEXT("Modifier magnitude value (default 0)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_ge_modifier"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add gameplay tags to a GE via the UE 5.3+ component architecture. tag_type controls which component is targeted."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("tags"), P(TEXT("array<string>"), true, TEXT("Array of gameplay tag strings"))); Pr->SetObjectField(TEXT("tag_type"), P(TEXT("string"), false, TEXT("Owned (default) | Granted | Required | Ignored"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ge_tags"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a GameplayCue Blueprint (Static or Actor)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new cue"))); Pr->SetObjectField(TEXT("cue_type"), P(TEXT("string"), false, TEXT("Static or Actor (default Static)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_gameplay_cue"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Scaffold a replicated UAttributeSet C++ header and source file"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("class_name"), P(TEXT("string"), true, TEXT("Class name (e.g. UMyAttributeSet)"))); Pr->SetObjectField(TEXT("module_name"), P(TEXT("string"), true, TEXT("Module name for file paths"))); Pr->SetObjectField(TEXT("attributes"), P(TEXT("array<string>"), false, TEXT("Attribute names (default: Value)"))); Pr->SetObjectField(TEXT("api_macro"), P(TEXT("string"), false, TEXT("API export macro (e.g. MYGAME_API)"))); Pr->SetObjectField(TEXT("rel_path_h"), P(TEXT("string"), false, TEXT("Relative header path override"))); Pr->SetObjectField(TEXT("rel_path_cpp"), P(TEXT("string"), false, TEXT("Relative source path override"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_attribute_set"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the duration policy and optional magnitude on a GameplayEffect"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("duration_policy"), P(TEXT("string"), true, TEXT("Instant, Infinite, or HasDuration"))); Pr->SetObjectField(TEXT("duration_magnitude"), P(TEXT("float"), false, TEXT("Duration in seconds (for HasDuration)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ge_duration"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the periodic execution interval on a GameplayEffect"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("period"), P(TEXT("float"), true, TEXT("Period in seconds"))); Pr->SetObjectField(TEXT("execute_on_application"), P(TEXT("bool"), false, TEXT("Execute periodic effect on initial application (default false)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ge_period"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure stacking behavior on a GameplayEffect"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("stacking_type"), P(TEXT("string"), true, TEXT("None, AggregateBySource, or AggregateByTarget"))); Pr->SetObjectField(TEXT("stack_limit"), P(TEXT("int"), false, TEXT("Maximum stack count"))); Pr->SetObjectField(TEXT("duration_refresh"), P(TEXT("string"), false, TEXT("RefreshOnSuccessfulApplication or NeverRefresh"))); Pr->SetObjectField(TEXT("period_reset"), P(TEXT("string"), false, TEXT("ResetOnSuccessfulApplication or NeverReset"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ge_stacking"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read basic info about a GameplayEffect (duration, modifiers, stacking)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_ge_info"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Full GE audit: tags per category (Owned/Granted/Required/Ignored), all modifiers, duration, period, stacking — for AI read-back loop"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("read_audit"), A); }

    // Phase 1c aliases
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Alias for create_gameplay_ability — creates a UGameplayAbility Blueprint asset"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new ability"))); Pr->SetObjectField(TEXT("parent_class"), P(TEXT("string"), false, TEXT("Full class path of parent UGameplayAbility (default UGameplayAbility)"))); Pr->SetObjectField(TEXT("tags"), P(TEXT("array<string>"), false, TEXT("Gameplay tags applied to CDO->AbilityTags"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_ability"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Alias for add_ge_modifier — add a modifier to a GameplayEffect (attribute + operation + magnitude)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("GE Blueprint asset path"))); Pr->SetObjectField(TEXT("attribute"), P(TEXT("string"), true, TEXT("ClassName.PropertyName (e.g. UMyAttributeSet.Health)"))); Pr->SetObjectField(TEXT("op"), P(TEXT("string"), true, TEXT("Add, Multiply, or Override"))); Pr->SetObjectField(TEXT("magnitude"), P(TEXT("float"), false, TEXT("Modifier magnitude value (default 0)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_modifier_to_ge"), A); }

    // Phase 1c new actions
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set or create a UGameplayEffect Blueprint as the cost for a UGameplayAbility. Creates the cost GE as Instant if it doesn't exist."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("ability_path"), P(TEXT("string"), true, TEXT("UGameplayAbility Blueprint asset path"))); Pr->SetObjectField(TEXT("cost_asset_path"), P(TEXT("string"), true, TEXT("UGameplayEffect Blueprint path for the cost (created as Instant GE if absent)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_ability_cost"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set or create a HasDuration UGameplayEffect Blueprint as the cooldown for a UGameplayAbility."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("ability_path"), P(TEXT("string"), true, TEXT("UGameplayAbility Blueprint asset path"))); Pr->SetObjectField(TEXT("cooldown_asset_path"), P(TEXT("string"), true, TEXT("UGameplayEffect Blueprint path for the cooldown (created as HasDuration GE if absent)"))); Pr->SetObjectField(TEXT("duration"), P(TEXT("float"), true, TEXT("Cooldown duration in seconds"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_ability_cooldown"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Append a FGameplayAbilitySpecDef to a GameplayEffect's GrantedAbilities array — grants the ability when the GE is applied."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("ge_asset_path"), P(TEXT("string"), true, TEXT("UGameplayEffect Blueprint asset path"))); Pr->SetObjectField(TEXT("ability_class_path"), P(TEXT("string"), true, TEXT("UGameplayAbility Blueprint path to grant"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_granted_ability"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all UAttributeSet Blueprint subclasses in the project via the Asset Registry."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("base_class_path"), P(TEXT("string"), false, TEXT("Optional Blueprint path to further filter to a specific UAttributeSet subclass hierarchy"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_attribute_sets"), A); }

    return Root;
}

// ---------------------------------------------------------------------------
// list_abilities_on_class
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_ListAbilitiesOnClass(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("list_abilities_on_class"));

	// Optional filter: only return abilities that are subclasses of the given parent class path
	FString ParentClassPath;
	Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);

	UClass* FilterClass = UGameplayAbility::StaticClass();
	if (!ParentClassPath.IsEmpty())
	{
		UClass* Loaded = LoadObject<UClass>(nullptr, *ParentClassPath);
		if (Loaded && Loaded->IsChildOf(UGameplayAbility::StaticClass()))
		{
			FilterClass = Loaded;
		}
	}

	// Use AssetRegistry to find all Blueprint assets whose parent is UGameplayAbility
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	TArray<TSharedPtr<FJsonValue>> AbilitiesArr;
	for (const FAssetData& AD : Assets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
		if (!BP || !BP->GeneratedClass) continue;
		if (!BP->GeneratedClass->IsChildOf(FilterClass)) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), BP->GetName());
		Entry->SetStringField(TEXT("path"), AD.GetObjectPathString());
		Entry->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None"));
		AbilitiesArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("abilities"), AbilitiesArr);
	Data->SetNumberField(TEXT("count"), AbilitiesArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("list_abilities_on_class: %d GameplayAbility Blueprint(s) found"), AbilitiesArr.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// get_attribute_defaults
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_GetAttributeDefaults(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("get_attribute_defaults"));

	FString ClassPath;
	if (!Params->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		Result.Message = TEXT("get_attribute_defaults: 'class_path' is required (UAttributeSet subclass asset path)");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Load the Blueprint and get its generated class
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassPath);
	// BP->GeneratedClass is TSubclassOf<UObject> in UE 5.7; explicit cast needed for ternary.
	UClass* AttrClass = BP ? (UClass*)BP->GeneratedClass : LoadObject<UClass>(nullptr, *ClassPath);

	if (!AttrClass || !AttrClass->IsChildOf(UAttributeSet::StaticClass()))
	{
		Result.Message = FString::Printf(
			TEXT("get_attribute_defaults: '%s' is not a valid UAttributeSet subclass"), *ClassPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	UAttributeSet* CDO = Cast<UAttributeSet>(AttrClass->GetDefaultObject());
	if (!CDO)
	{
		Result.Message = TEXT("get_attribute_defaults: could not get CDO for attribute set");
		Result.ErrorCode = 3000;
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> AttrsArr;
	for (TFieldIterator<FProperty> It(AttrClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		// FGameplayAttributeData is the standard attribute struct
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp) continue;

		// Check if it's a FGameplayAttributeData
		const FString StructName = StructProp->Struct->GetName();
		if (StructName != TEXT("GameplayAttributeData") && StructName != TEXT("GameplayClampedAttributeData"))
			continue;

		// Read BaseValue from the struct
		void* PropData = StructProp->ContainerPtrToValuePtr<void>(CDO);
		FGameplayAttributeData* AttrData = static_cast<FGameplayAttributeData*>(PropData);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),       Prop->GetName());
		Entry->SetNumberField(TEXT("base_value"),  AttrData->GetBaseValue());
		Entry->SetNumberField(TEXT("current_value"), AttrData->GetCurrentValue());
		AttrsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_path"), ClassPath);
	Data->SetArrayField(TEXT("attributes"), AttrsArr);
	Data->SetNumberField(TEXT("count"), AttrsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("get_attribute_defaults: %d attribute(s) on '%s'"), AttrsArr.Num(), *ClassPath);
	return Result;
}

// ---------------------------------------------------------------------------
// create_ability_task
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_CreateAbilityTask(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("create_ability_task"));

	FString AssetPath;
	if ((!Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !Params->TryGetStringField(TEXT("path"), AssetPath)) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_ability_task: 'asset_path' is required (e.g. '/Game/GAS/Tasks/ABT_WaitForEvent')");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Resolve parent class — default to UAbilityTask if not specified
	FString ParentClassStr = TEXT("AbilityTask");
	Params->TryGetStringField(TEXT("parent_class"), ParentClassStr);

	// Find the parent UClass — try with U prefix first
	UClass* ParentClass = FindFirstObject<UClass>(*ParentClassStr, EFindFirstObjectOptions::ExactClass);
	if (!ParentClass)
	{
		FString Prefixed = FString::Printf(TEXT("U%s"), *ParentClassStr);
		ParentClass = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::ExactClass);
	}
	if (!ParentClass)
	{
		// Try loading by path (for Blueprint parent classes)
		ParentClass = LoadObject<UClass>(nullptr, *ParentClassStr);
	}
	if (!ParentClass)
	{
		Result.Message = FString::Printf(TEXT("create_ability_task: parent class '%s' not found"), *ParentClassStr);
		Result.ErrorCode = 2000;
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass      = ParentClass;
	Factory->BlueprintType    = BPTYPE_Normal;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);

	if (!Created)
	{
		Result.Message = FString::Printf(TEXT("create_ability_task: failed to create Blueprint at '%s'"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	Created->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("AbilityTask Blueprint '%s' created (parent=%s) at '%s'"),
	                                       *AssetName, *ParentClass->GetName(), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// configure_asc
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_ConfigureASC(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("configure_asc"));

	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		Result.Message = TEXT("configure_asc: 'blueprint_path' is required (Actor Blueprint with an AbilitySystemComponent)");
		Result.ErrorCode = 1000;
		return Result;
	}

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		const FString Suffix = BlueprintPath + TEXT(".") + FPackageName::GetLongPackageAssetName(BlueprintPath);
		BP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!BP || !BP->GeneratedClass)
	{
		Result.Message = FString::Printf(TEXT("configure_asc: no Blueprint found at '%s'"), *BlueprintPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Find the AbilitySystemComponent on the CDO
	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		Result.Message = FString::Printf(TEXT("configure_asc: Blueprint '%s' CDO is not an AActor"), *BlueprintPath);
		Result.ErrorCode = 2001;
		return Result;
	}

	UAbilitySystemComponent* ASC = CDO->FindComponentByClass<UAbilitySystemComponent>();
	if (!ASC)
	{
		Result.Message = FString::Printf(TEXT("configure_asc: no UAbilitySystemComponent found on '%s'. Add the component first."), *BlueprintPath);
		Result.ErrorCode = 2001;
		return Result;
	}

	bool bChanged = false;

	// ReplicationMode: Minimal | Mixed | Full
	FString ReplicationModeStr;
	if (Params->TryGetStringField(TEXT("replication_mode"), ReplicationModeStr))
	{
		if      (ReplicationModeStr == TEXT("Minimal")) ASC->ReplicationMode = EGameplayEffectReplicationMode::Minimal;
		else if (ReplicationModeStr == TEXT("Mixed"))   ASC->ReplicationMode = EGameplayEffectReplicationMode::Mixed;
		else if (ReplicationModeStr == TEXT("Full"))    ASC->ReplicationMode = EGameplayEffectReplicationMode::Full;
		bChanged = true;
	}

	if (bChanged)
	{
		BP->GeneratedClass->GetDefaultObject()->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}

	Result.bSuccess     = true;
	Result.AffectedPath = BlueprintPath;
	Result.Message      = FString::Printf(TEXT("configure_asc: ASC on '%s' configured (replication_mode=%s)"),
	                                       *BlueprintPath, *ReplicationModeStr);
	return Result;
}

// ---------------------------------------------------------------------------
// add_ability_to_asc
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_AddAbilityToASC(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("add_ability_to_asc"));

	FString BlueprintPath, AbilityPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("ability_path"),   AbilityPath)   || AbilityPath.IsEmpty())
	{
		Result.Message = TEXT("add_ability_to_asc: 'blueprint_path' and 'ability_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		const FString Suffix = BlueprintPath + TEXT(".") + FPackageName::GetLongPackageAssetName(BlueprintPath);
		BP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!BP || !BP->GeneratedClass)
	{
		Result.Message = FString::Printf(TEXT("add_ability_to_asc: no Blueprint found at '%s'"), *BlueprintPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		Result.Message = FString::Printf(TEXT("add_ability_to_asc: Blueprint CDO is not an AActor"));
		Result.ErrorCode = 2001;
		return Result;
	}

	UAbilitySystemComponent* ASC = CDO->FindComponentByClass<UAbilitySystemComponent>();
	if (!ASC)
	{
		Result.Message = FString::Printf(TEXT("add_ability_to_asc: no UAbilitySystemComponent on '%s'"), *BlueprintPath);
		Result.ErrorCode = 2001;
		return Result;
	}

	// Load the ability class
	UClass* AbilityClass = nullptr;
	UBlueprint* AbilityBP = LoadObject<UBlueprint>(nullptr, *AbilityPath);
	if (!AbilityBP)
	{
		const FString Suffix = AbilityPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AbilityPath);
		AbilityBP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (AbilityBP && AbilityBP->GeneratedClass)
		AbilityClass = AbilityBP->GeneratedClass;

	if (!AbilityClass || !AbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		Result.Message = FString::Printf(TEXT("add_ability_to_asc: no UGameplayAbility Blueprint found at '%s'"), *AbilityPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Add to ActivatableAbilities DefaultStartingData on the CDO
	// DefaultStartingData is not the right place — abilities are granted at runtime.
	// At CDO level, we can add to ASC->ActivatableAbilities.Items if available,
	// but the canonical way is to store them in a TArray<TSubclassOf<UGameplayAbility>>
	// member on the owning Actor's Blueprint CDO.
	// Runtime granting requires an active ASC, which is only available in PIE.
	return MakeError(TEXT("gas"), TEXT("add_ability_to_asc"), 3004,
		TEXT("add_ability_to_asc: Runtime GiveAbility requires an active AbilitySystemComponent (PIE only)."),
		TEXT("Call ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1)) in your character's BeginPlay or via GameMode in PIE."));
}

// ---------------------------------------------------------------------------
// remove_ability_from_asc
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_RemoveAbilityFromASC(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("remove_ability_from_asc"));

	FString BlueprintPath, AbilityPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("ability_path"),   AbilityPath)   || AbilityPath.IsEmpty())
	{
		Result.Message = TEXT("remove_ability_from_asc: 'blueprint_path' and 'ability_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Ability removal at design-time is not directly possible via CDO editing
	// (abilities are granted at runtime via GiveAbility / ClearAllAbilities).
	// Runtime removal requires an active ASC — PIE only.
	return MakeError(TEXT("gas"), TEXT("remove_ability_from_asc"), 3004,
		TEXT("remove_ability_from_asc: Runtime ClearAbility requires an active AbilitySystemComponent (PIE only)."),
		TEXT("Call ASC->ClearAbility(AbilityHandle) or ASC->ClearAllAbilities() in PIE. To prevent an ability from being granted at startup, remove it from your character's startup ability array."));
}

// ---------------------------------------------------------------------------
// set_ability_tags
// Params: asset_path (string), tags (string[]), tag_type (string, optional):
//   AbilityTags (default) | CancelAbilitiesWithTag | BlockAbilitiesWithTag
//   | ActivationOwnedTags | ActivationRequiredTags | ActivationBlockedTags
// Writes to CDO via GetDefaultObject() — NOT to the spec copy.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetAbilityTags(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ability_tags");

	FString AssetPath, TagTypeStr;
	const TArray<TSharedPtr<FJsonValue>>* TagsJsonArray = nullptr;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	if (!Params->TryGetArrayField(TEXT("tags"), TagsJsonArray) || !TagsJsonArray)
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'tags' (string array)"));

	Params->TryGetStringField(TEXT("tag_type"), TagTypeStr);
	if (TagTypeStr.IsEmpty()) TagTypeStr = TEXT("AbilityTags");

	const TArray<FString> ValidTypes = {
		TEXT("AbilityTags"), TEXT("CancelAbilitiesWithTag"), TEXT("BlockAbilitiesWithTag"),
		TEXT("ActivationOwnedTags"), TEXT("ActivationRequiredTags"), TEXT("ActivationBlockedTags")
	};
	if (!ValidTypes.Contains(TagTypeStr))
		return MakeError(GAS_DOMAIN, Action, 1001,
			FString::Printf(TEXT("Invalid tag_type '%s'"), *TagTypeStr),
			TEXT("Valid: AbilityTags|CancelAbilitiesWithTag|BlockAbilitiesWithTag|"
			     "ActivationOwnedTags|ActivationRequiredTags|ActivationBlockedTags"));

	// Load blueprint and get the CDO — must use GetDefaultObject() NOT the Blueprint spec object
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!BP)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!BP)
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("No Blueprint found at '%s'"), *AssetPath));

	if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
		if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
			return MakeError(GAS_DOMAIN, Action, 2001,
				FString::Printf(TEXT("'%s' is not a UGameplayAbility Blueprint"), *AssetPath));
	}

	// Get the CDO — this is where persistent data lives
	UGameplayAbility* CDO = Cast<UGameplayAbility>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
		return MakeError(GAS_DOMAIN, Action, 3000, TEXT("Could not get CDO for ability Blueprint"));

	// Build tag container
	FGameplayTagContainer TagContainer;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	int32 SkippedCount = 0;
	for (const TSharedPtr<FJsonValue>& TagVal : *TagsJsonArray)
	{
		FString TagStr;
		if (!TagVal->TryGetString(TagStr) || TagStr.IsEmpty()) continue;
		FGameplayTag Tag = TagsMgr.RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
			TagContainer.AddTag(Tag);
		else
			++SkippedCount;
	}

	if (TagContainer.Num() == 0 && SkippedCount > 0)
		return MakeError(GAS_DOMAIN, Action, 1001,
			FString::Printf(TEXT("All %d tag(s) are unregistered — none applied. "
			                     "Register tags in DefaultGameplayTags.ini first."), SkippedCount));

	// Write to CDO property by tag_type
	// UE 5.7: AbilityTags is deprecated -> use EditorGetAssetTags() (mutable) / GetAssetTags() (const).
	// Other tag containers are protected with NO public getters — use reflection via FStructProperty
	// to reach FGameplayTagContainer fields directly on the CDO.
	auto AppendTagsByProperty = [&](const TCHAR* PropName) -> bool
	{
		if (FStructProperty* Prop = FindFProperty<FStructProperty>(UGameplayAbility::StaticClass(), PropName))
		{
			if (FGameplayTagContainer* TagsPtr = Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO))
			{
				TagsPtr->AppendTags(TagContainer);
				return true;
			}
		}
		return false;
	};

	if (TagTypeStr == TEXT("AbilityTags"))
	{
		FGameplayTagContainer& AssetTags = CDO->EditorGetAssetTags();
		// Remove existing tags that are in the new set to prevent duplicates
		for (const FGameplayTag& Tag : TagContainer)
		{
			AssetTags.RemoveTag(Tag);
		}
		AssetTags.AppendTags(TagContainer);
	}
	else if (TagTypeStr == TEXT("CancelAbilitiesWithTag"))
	{
		if (!AppendTagsByProperty(TEXT("CancelAbilitiesWithTag")))
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'CancelAbilitiesWithTag' on UGameplayAbility (UE 5.7 rename?)."));
	}
	else if (TagTypeStr == TEXT("BlockAbilitiesWithTag"))
	{
		if (!AppendTagsByProperty(TEXT("BlockAbilitiesWithTag")))
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'BlockAbilitiesWithTag' on UGameplayAbility (UE 5.7 rename?)."));
	}
	else if (TagTypeStr == TEXT("ActivationOwnedTags"))
	{
		if (!AppendTagsByProperty(TEXT("ActivationOwnedTags")))
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'ActivationOwnedTags' on UGameplayAbility (UE 5.7 rename?)."));
	}
	else if (TagTypeStr == TEXT("ActivationRequiredTags"))
	{
		if (!AppendTagsByProperty(TEXT("ActivationRequiredTags")))
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'ActivationRequiredTags' on UGameplayAbility (UE 5.7 rename?)."));
	}
	else if (TagTypeStr == TEXT("ActivationBlockedTags"))
	{
		if (!AppendTagsByProperty(TEXT("ActivationBlockedTags")))
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'ActivationBlockedTags' on UGameplayAbility (UE 5.7 rename?)."));
	}

	// Mark the outer package dirty so the CDO changes persist
	CDO->GetOuter()->MarkPackageDirty();
	BP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(BP);

	FString SkippedNote = (SkippedCount > 0)
		? FString::Printf(TEXT(" (%d unregistered tag(s) skipped)"), SkippedCount)
		: TEXT("");

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("tag_type"), TagTypeStr);
	Data->SetNumberField(TEXT("tags_applied"), TagContainer.Num());
	Data->SetNumberField(TEXT("tags_skipped"), SkippedCount);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Applied %d '%s' tag(s) to CDO of '%s'%s"),
			TagContainer.Num(), *TagTypeStr, *AssetPath, *SkippedNote),
		Data);
}

// ---------------------------------------------------------------------------
// add_ability_cost  (Phase 1c)
// Params: ability_path (UGameplayAbility BP), cost_asset_path (UGameplayEffect BP, create if missing)
// Sets CostGameplayEffectClass on the ability CDO.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_AddAbilityCost(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("add_ability_cost");

	FString AbilityPath, CostAssetPath;
	if (!Params->TryGetStringField(TEXT("ability_path"), AbilityPath) || AbilityPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'ability_path'"));
	if (!Params->TryGetStringField(TEXT("cost_asset_path"), CostAssetPath) || CostAssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'cost_asset_path'"));

	// ---- Resolve or create the cost GE Blueprint ----------------------------
	UBlueprint* CostBP = LoadObject<UBlueprint>(nullptr, *CostAssetPath);
	if (!CostBP)
	{
		// Create a new Instant GE Blueprint at cost_asset_path
		const FString CostName    = FPackageName::GetLongPackageAssetName(CostAssetPath);
		const FString CostPkgPath = FPackageName::GetLongPackagePath(CostAssetPath);

		FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UBlueprintFactory* Factory  = NewObject<UBlueprintFactory>();
		Factory->ParentClass        = UGameplayEffect::StaticClass();

		UObject* Created = ATModule.Get().CreateAsset(CostName, CostPkgPath, UBlueprint::StaticClass(), Factory);
		if (!Created)
			return MakeError(GAS_DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to create cost GE at '%s'"), *CostAssetPath));

		CostBP = CastChecked<UBlueprint>(Created);
		FKismetEditorUtilities::CompileBlueprint(CostBP);

		// Set Instant duration policy on the new GE CDO
		if (CostBP->GeneratedClass)
		{
			if (UGameplayEffect* GeCDO = Cast<UGameplayEffect>(CostBP->GeneratedClass->GetDefaultObject()))
				GeCDO->DurationPolicy = EGameplayEffectDurationType::Instant;
		}
		CostBP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(CostBP);
	}

	if (!CostBP->GeneratedClass || !CostBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("'%s' is not a UGameplayEffect Blueprint"), *CostAssetPath));

	// ---- Load the ability Blueprint -----------------------------------------
	UBlueprint* AbilityBP = LoadObject<UBlueprint>(nullptr, *AbilityPath);
	if (!AbilityBP)
	{
		const FString Suffix = AbilityPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AbilityPath);
		AbilityBP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!AbilityBP)
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("No Blueprint found at '%s'"), *AbilityPath));

	if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(AbilityBP);
		if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
			return MakeError(GAS_DOMAIN, Action, 2001,
				FString::Printf(TEXT("'%s' is not a UGameplayAbility Blueprint"), *AbilityPath));
	}

	UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(AbilityBP->GeneratedClass->GetDefaultObject());
	if (!AbilityCDO)
		return MakeError(GAS_DOMAIN, Action, 3000, TEXT("Could not get ability CDO"));

	// Set cost GE class.
	// UE 5.7: CostGameplayEffectClass is protected. The public getter is
	// GetCostGameplayEffect() (returns the GE CDO). For writes on the ability CDO
	// we use reflection against the FObjectProperty on UGameplayAbility.
	{
		FObjectProperty* CostProp = FindFProperty<FObjectProperty>(
			UGameplayAbility::StaticClass(), TEXT("CostGameplayEffectClass"));
		if (CostProp)
		{
			CostProp->SetObjectPropertyValue_InContainer(AbilityCDO, CostBP->GeneratedClass);
		}
		else
		{
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'CostGameplayEffectClass' on UGameplayAbility (UE 5.7 rename?)."));
		}
	}

	AbilityBP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(AbilityBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ability_path"),    AbilityPath);
	Data->SetStringField(TEXT("cost_asset_path"), CostAssetPath);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("CostGameplayEffectClass set to '%s' on ability '%s'"),
			*CostAssetPath, *AbilityPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_ability_cooldown  (Phase 1c)
// Params: ability_path, cooldown_asset_path, duration (float, seconds)
// Loads or creates a HasDuration GE at cooldown_asset_path, sets its duration,
// then sets CooldownGameplayEffectClass on the ability CDO.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_SetAbilityCooldown(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_ability_cooldown");

	FString AbilityPath, CooldownAssetPath;
	double Duration = 0.0;

	if (!Params->TryGetStringField(TEXT("ability_path"), AbilityPath) || AbilityPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'ability_path'"));
	if (!Params->TryGetStringField(TEXT("cooldown_asset_path"), CooldownAssetPath) || CooldownAssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'cooldown_asset_path'"));
	if (!Params->TryGetNumberField(TEXT("duration"), Duration))
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'duration' (float, seconds)"));

	// ---- Resolve or create the cooldown GE Blueprint ------------------------
	UBlueprint* CooldownBP = LoadObject<UBlueprint>(nullptr, *CooldownAssetPath);
	if (!CooldownBP)
	{
		const FString CdName    = FPackageName::GetLongPackageAssetName(CooldownAssetPath);
		const FString CdPkgPath = FPackageName::GetLongPackagePath(CooldownAssetPath);

		FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UBlueprintFactory* Factory  = NewObject<UBlueprintFactory>();
		Factory->ParentClass        = UGameplayEffect::StaticClass();

		UObject* Created = ATModule.Get().CreateAsset(CdName, CdPkgPath, UBlueprint::StaticClass(), Factory);
		if (!Created)
			return MakeError(GAS_DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to create cooldown GE at '%s'"), *CooldownAssetPath));

		CooldownBP = CastChecked<UBlueprint>(Created);
		FKismetEditorUtilities::CompileBlueprint(CooldownBP);
	}

	if (!CooldownBP->GeneratedClass || !CooldownBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("'%s' is not a UGameplayEffect Blueprint"), *CooldownAssetPath));

	// Set HasDuration + magnitude on the cooldown GE CDO
	if (UGameplayEffect* CdCDO = Cast<UGameplayEffect>(CooldownBP->GeneratedClass->GetDefaultObject()))
	{
		CdCDO->DurationPolicy    = EGameplayEffectDurationType::HasDuration;
		CdCDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat((float)Duration));
	}
	CooldownBP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(CooldownBP);

	// ---- Load the ability Blueprint -----------------------------------------
	UBlueprint* AbilityBP = LoadObject<UBlueprint>(nullptr, *AbilityPath);
	if (!AbilityBP)
	{
		const FString Suffix = AbilityPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AbilityPath);
		AbilityBP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!AbilityBP)
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("No Blueprint found at '%s'"), *AbilityPath));

	if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(AbilityBP);
		if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
			return MakeError(GAS_DOMAIN, Action, 2001,
				FString::Printf(TEXT("'%s' is not a UGameplayAbility Blueprint"), *AbilityPath));
	}

	UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(AbilityBP->GeneratedClass->GetDefaultObject());
	if (!AbilityCDO)
		return MakeError(GAS_DOMAIN, Action, 3000, TEXT("Could not get ability CDO"));

	// Set cooldown GE class.
	// UE 5.7: CooldownGameplayEffectClass is protected. Use reflection to write
	// the TSubclassOf<UGameplayEffect> field on the ability CDO.
	{
		FObjectProperty* CdProp = FindFProperty<FObjectProperty>(
			UGameplayAbility::StaticClass(), TEXT("CooldownGameplayEffectClass"));
		if (CdProp)
		{
			CdProp->SetObjectPropertyValue_InContainer(AbilityCDO, CooldownBP->GeneratedClass);
		}
		else
		{
			return MakeError(GAS_DOMAIN, Action, 3000,
				TEXT("Could not reflect 'CooldownGameplayEffectClass' on UGameplayAbility (UE 5.7 rename?)."));
		}
	}

	AbilityBP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(AbilityBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ability_path"),        AbilityPath);
	Data->SetStringField(TEXT("cooldown_asset_path"), CooldownAssetPath);
	Data->SetNumberField(TEXT("duration"),            Duration);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("CooldownGameplayEffectClass set to '%s' (duration=%.2fs) on ability '%s'"),
			*CooldownAssetPath, Duration, *AbilityPath),
		Data);
}

// ---------------------------------------------------------------------------
// add_granted_ability  (Phase 1c)
// Params: ge_asset_path (UGameplayEffect BP), ability_class_path (UGameplayAbility BP)
// Appends a FGameplayAbilitySpecDef to CDO->GrantedAbilities on the GE.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_AddGrantedAbility(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("add_granted_ability");

	FString GEAssetPath, AbilityClassPath;
	if (!Params->TryGetStringField(TEXT("ge_asset_path"), GEAssetPath) || GEAssetPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'ge_asset_path'"));
	if (!Params->TryGetStringField(TEXT("ability_class_path"), AbilityClassPath) || AbilityClassPath.IsEmpty())
		return MakeError(GAS_DOMAIN, Action, 1000, TEXT("Missing required param: 'ability_class_path'"));

	// Load the GE Blueprint
	UBlueprint* GEBP = nullptr;
	FBridgeResult TmpResult = CreateResult(GAS_DOMAIN, Action);
	UGameplayEffect* GECDO  = LoadGEBlueprint(GEAssetPath, GEBP, TmpResult);
	if (!GECDO) return TmpResult;

	// Load the ability Blueprint to get its generated class
	UBlueprint* AbilityBP = LoadObject<UBlueprint>(nullptr, *AbilityClassPath);
	if (!AbilityBP)
	{
		const FString Suffix = AbilityClassPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AbilityClassPath);
		AbilityBP = LoadObject<UBlueprint>(nullptr, *Suffix);
	}
	if (!AbilityBP)
		return MakeError(GAS_DOMAIN, Action, 2000,
			FString::Printf(TEXT("No Blueprint found at '%s'"), *AbilityClassPath));

	if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(AbilityBP);
		if (!AbilityBP->GeneratedClass || !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
			return MakeError(GAS_DOMAIN, Action, 2001,
				FString::Printf(TEXT("'%s' is not a UGameplayAbility Blueprint"), *AbilityClassPath));
	}

	// UE 5.7: UGameplayEffect::GrantedAbilities is deprecated and lives on
	// UAbilitiesGameplayEffectComponent. Find-or-add the component on the GE CDO
	// and read/write its GrantAbilityConfigs array (TArray<FGameplayAbilitySpecConfig>).
	// NOTE: FindOrAddComponent<T>() returns T& (not T*).
	// UE 5.7: FindComponent<T>() returns const T*. const_cast to get a writable pointer
	// since AddGrantedAbilityConfig is a non-const public API on the component.
	UAbilitiesGameplayEffectComponent* AbilComp = const_cast<UAbilitiesGameplayEffectComponent*>(
		GECDO->FindComponent<UAbilitiesGameplayEffectComponent>());
	if (!AbilComp)
	{
		// FindOrAddComponent creates and registers the component if missing. Returns T&.
		UAbilitiesGameplayEffectComponent& AbilCompRef =
			GECDO->FindOrAddComponent<UAbilitiesGameplayEffectComponent>();
		AbilComp = &AbilCompRef;
	}
	if (!AbilComp)
		return MakeError(GAS_DOMAIN, Action, 3000,
			TEXT("Failed to resolve UAbilitiesGameplayEffectComponent on GE CDO."));

	// NOTE: Cannot guard against duplicates — GrantAbilityConfigs is protected in UE 5.7
	// and has no public read accessor. AddGrantedAbilityConfig is the only public write API,
	// so duplicate grants are a possible trade-off.

	// Build and append the spec config.
	// UE 5.7: FGameplayAbilitySpecConfig has Ability + LevelScalableFloat.
	FGameplayAbilitySpecConfig AbilitySpecConfig;
	AbilitySpecConfig.Ability            = AbilityBP->GeneratedClass;
	AbilitySpecConfig.LevelScalableFloat = FScalableFloat(1.0f);
	// InputID deprecated in UE 5.3+ — omitted; tag-based input is the UE 5.7 path

	AbilComp->AddGrantedAbilityConfig(AbilitySpecConfig);

	// count unavailable via public API; report fixed message

	if (GEBP)
	{
		GEBP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(GEBP);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ge_asset_path"),      GEAssetPath);
	Data->SetStringField(TEXT("ability_class_path"), AbilityClassPath);

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("Ability '%s' granted on GE '%s' successfully."),
			*AbilityClassPath, *GEAssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// list_attribute_sets  (Phase 1c)
// Params: base_class_path (optional) — filters to subclasses of the given UAttributeSet subclass
// Uses AssetRegistry to enumerate all Blueprint assets that derive from UAttributeSet.
// ---------------------------------------------------------------------------

FBridgeResult UGASHandler::Action_ListAttributeSets(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("list_attribute_sets");

	// Optional filter class
	FString BaseClassPath;
	Params->TryGetStringField(TEXT("base_class_path"), BaseClassPath);

	UClass* FilterClass = UAttributeSet::StaticClass();
	if (!BaseClassPath.IsEmpty())
	{
		UBlueprint* FilterBP = LoadObject<UBlueprint>(nullptr, *BaseClassPath);
		UClass* Loaded = FilterBP ? (UClass*)FilterBP->GeneratedClass
		                          : LoadObject<UClass>(nullptr, *BaseClassPath);
		if (Loaded && Loaded->IsChildOf(UAttributeSet::StaticClass()))
			FilterClass = Loaded;
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Search for all Blueprint assets — UAttributeSet subclasses are Blueprints in content
	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	TArray<TSharedPtr<FJsonValue>> SetsArr;
	for (const FAssetData& AD : Assets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
		if (!BP || !BP->GeneratedClass) continue;
		if (!BP->GeneratedClass->IsChildOf(FilterClass)) continue;
		// Exclude UAttributeSet itself (only subclasses)
		if (BP->GeneratedClass == UAttributeSet::StaticClass()) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),        BP->GetName());
		Entry->SetStringField(TEXT("path"),        AD.GetObjectPathString());
		Entry->SetStringField(TEXT("class"),       BP->GeneratedClass->GetName());
		Entry->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None"));
		SetsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("attribute_sets"), SetsArr);
	Data->SetNumberField(TEXT("count"), SetsArr.Num());

	return MakeSuccess(GAS_DOMAIN, Action,
		FString::Printf(TEXT("list_attribute_sets: %d UAttributeSet Blueprint(s) found"), SetsArr.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_active_effects  (Phase 0 gap — PIE guard)
// Returns PIE_REQUIRED note in editor mode; active effects only exist at runtime.
// Params: target_actor (string, optional — actor label; reserved for PIE use)
// ---------------------------------------------------------------------------
FBridgeResult UGASHandler::Action_GetActiveEffects(TSharedPtr<FJsonObject> Params)
{
	return MakeError(TEXT("gas"), TEXT("get_active_effects"), 3004,
		TEXT("get_active_effects requires PIE — AbilitySystemComponent state is only available at runtime."),
		TEXT("Start Play In Editor and call this action again to query active effects."));
}

// ---------------------------------------------------------------------------
// apply_gameplay_effect  (Phase 0 gap — hard block outside PIE)
// Applying a GE requires a live ASC target — always blocked in editor mode.
// Returns error 3004 per spec.
// ---------------------------------------------------------------------------
FBridgeResult UGASHandler::Action_ApplyGameplayEffect(TSharedPtr<FJsonObject> Params)
{
	return MakeError(GAS_DOMAIN, TEXT("apply_gameplay_effect"), 3004,
		TEXT("apply_gameplay_effect requires Play In Editor — "
		     "cannot apply to an active AbilitySystemComponent outside PIE"),
		TEXT("Start PIE, then call apply_gameplay_effect with "
		     "'target_actor' (actor label) and 'ge_class_path' (GE Blueprint path) params"));
}

// ===========================================================================
// CRUD-symmetry: GE modifier removal
// ===========================================================================

FBridgeResult UGASHandler::Action_RemoveGEModifier(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	int32 ModifierIndex = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("gas"), TEXT("remove_ge_modifier"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetNumberField(TEXT("modifier_index"), ModifierIndex) || ModifierIndex < 0)
		return MakeError(TEXT("gas"), TEXT("remove_ge_modifier"), 1000, TEXT("'modifier_index' (>=0) is required"));

	FBridgeResult Result = CreateResult(TEXT("gas"), TEXT("remove_ge_modifier"));
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = LoadGEBlueprint(AssetPath, BP, Result);
	if (!GE) return Result;
	if (!GE->Modifiers.IsValidIndex(ModifierIndex))
		return MakeError(TEXT("gas"), TEXT("remove_ge_modifier"), 1001,
			FString::Printf(TEXT("modifier_index %d out of range (have %d)"), ModifierIndex, GE->Modifiers.Num()));

	GE->Modifiers.RemoveAt(ModifierIndex);
	if (BP)
	{
		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);
	}
	else
	{
		GE->MarkPackageDirty();
	}
	return MakeSuccess(TEXT("gas"), TEXT("remove_ge_modifier"),
		FString::Printf(TEXT("Removed GE modifier at index %d"), ModifierIndex));
}
