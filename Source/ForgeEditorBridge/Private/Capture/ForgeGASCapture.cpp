#include "Capture/ForgeGASCapture.h"
#include "IO/ForgeContextWriter.h"

// --- GAS ---
#include "Abilities/GameplayAbility.h"
#include "Abilities/Tasks/AbilityTask.h"          // v1.0 — task lifecycle detection
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"

// --- Blueprint graph scanning ---
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"                   // v1.0 — pin connection inspection
#include "K2Node_CallFunction.h"

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// --- UObject iteration ---
#include "UObject/UObjectIterator.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    /** Collect all EdGraph nodes of type T from every graph in a Blueprint. */
    template<typename T>
    TArray<T*> CollectNodes(UBlueprint* BP)
    {
        TArray<T*> Results;
        if (!BP) return Results;

        auto ScanGraph = [&](UEdGraph* Graph)
        {
            if (!Graph) return;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (T* Typed = Cast<T>(Node))
                {
                    Results.Add(Typed);
                }
            }
        };

        for (UEdGraph* G : BP->UbergraphPages)  { ScanGraph(G); }
        for (UEdGraph* G : BP->FunctionGraphs)  { ScanGraph(G); }
        for (UEdGraph* G : BP->MacroGraphs)     { ScanGraph(G); }
        return Results;
    }

    /** Return true if BP has a function graph with the given name. */
    bool HasFunctionGraph(UBlueprint* BP, const FName& FuncName)
    {
        if (!BP) return false;
        for (UEdGraph* G : BP->FunctionGraphs)
        {
            if (G && G->GetFName() == FuncName) return true;
        }
        return false;
    }

    /** Return true if any CallFunction node in BP calls FuncName. */
    bool HasCallToFunction(UBlueprint* BP, const FName& FuncName)
    {
        TArray<UK2Node_CallFunction*> Calls = CollectNodes<UK2Node_CallFunction>(BP);
        for (UK2Node_CallFunction* Call : Calls)
        {
            if (Call->GetFunctionName() == FuncName) return true;
        }
        return false;
    }

    /**
     * Return true if BP calls any function that belongs to a UAbilityTask subclass.
     * This detects task creation (e.g., PlayMontageAndWait::CreateProxy) without needing
     * to enumerate every task type.
     */
    bool HasAbilityTaskUsage(UBlueprint* BP)
    {
        TArray<UK2Node_CallFunction*> Calls = CollectNodes<UK2Node_CallFunction>(BP);
        for (UK2Node_CallFunction* Call : Calls)
        {
            UFunction* Func = Call->GetTargetFunction();
            if (!Func) continue;
            UClass* FuncClass = Func->GetOuterUClass();
            if (FuncClass && FuncClass->IsChildOf(UAbilityTask::StaticClass()))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * Return true if the CommitAbility call's bool return value is wired to something.
     * An unconnected return means the ability proceeds even if cost/cooldown application fails.
     */
    bool IsCommitResultChecked(UBlueprint* BP)
    {
        TArray<UK2Node_CallFunction*> Calls = CollectNodes<UK2Node_CallFunction>(BP);
        for (UK2Node_CallFunction* Call : Calls)
        {
            if (Call->GetFunctionName() != FName("CommitAbility")) continue;

            // "ReturnValue" is the standard pin name for a function's bool return
            UEdGraphPin* ReturnPin = Call->FindPin(FName(TEXT("ReturnValue")), EGPD_Output);
            if (ReturnPin && ReturnPin->LinkedTo.Num() > 0)
            {
                return true;  // at least one CommitAbility has its result consumed
            }
        }
        return false;
    }

    /** Validate every tag in a container; return names of any invalid ones. */
    TArray<FString> InvalidTagsIn(const FGameplayTagContainer& Container)
    {
        TArray<FString> Bad;
        for (const FGameplayTag& Tag : Container)
        {
            if (!Tag.IsValid())
            {
                Bad.Add(TEXT("(invalid)"));
            }
        }
        return Bad;
    }

    /** Return total node count across all event graphs in a Blueprint. */
    int32 CountEventGraphNodes(UBlueprint* BP)
    {
        int32 Total = 0;
        for (UEdGraph* G : BP->UbergraphPages)
        {
            if (G) Total += G->Nodes.Num();
        }
        return Total;
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeGASCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("gas")));
    UE_LOG(LogTemp, Log, TEXT("ForgeGAS: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportGASAudit
// ---------------------------------------------------------------------------

bool UForgeGASCapture::ExportGASAudit()
{
    TArray<TSharedPtr<FJsonValue>> AbilityIssues = AuditAbilities();
    TArray<TSharedPtr<FJsonValue>> GEIssues      = AuditGameplayEffects();
    TArray<TSharedPtr<FJsonValue>> AttrSetIssues = AuditAttributeSets();
    TArray<TSharedPtr<FJsonValue>> TagIssues     = AuditTags();

    TArray<TSharedPtr<FJsonValue>> AllIssues;
    AllIssues.Append(AbilityIssues);
    AllIssues.Append(GEIssues);
    AllIssues.Append(AttrSetIssues);
    AllIssues.Append(TagIssues);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),       FForgeContextWriter::NowISO8601());
    Root->SetNumberField(TEXT("total_issues"),     AllIssues.Num());
    Root->SetNumberField(TEXT("ability_issues"),   AbilityIssues.Num());
    Root->SetNumberField(TEXT("ge_issues"),        GEIssues.Num());
    Root->SetNumberField(TEXT("attr_set_issues"),  AttrSetIssues.Num());
    Root->SetNumberField(TEXT("tag_issues"),       TagIssues.Num());
    Root->SetArrayField(TEXT("issues"),            AllIssues);

    const FString RelPath = TEXT("gas/audit.json");
    bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("gas"), TEXT("audit"), Root);

    if (bOK)
    {
        UpdateIndexFile(AllIssues.Num(), RelPath);
        UE_LOG(LogTemp, Log, TEXT("ForgeGAS: Wrote %d issues to %s"),
               AllIssues.Num(), *RelPath);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("ForgeGAS: Failed to write audit.json"));
    }

    return bOK;
}

// ---------------------------------------------------------------------------
// AuditAbilities
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeGASCapture::AuditAbilities()
{
    TArray<TSharedPtr<FJsonValue>> Issues;

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    for (const FAssetData& AssetData : Assets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
        if (!BP || !BP->GeneratedClass) continue;
        if (!BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass())) continue;

        const FString AssetPath = AssetData.GetObjectPathString();
        UGameplayAbility* CDO = Cast<UGameplayAbility>(BP->GeneratedClass->GetDefaultObject());
        if (!CDO) continue;

        const UGameplayEffect* CostClass     = CDO->GetCostGameplayEffect();
        const UGameplayEffect* CooldownClass = CDO->GetCooldownGameplayEffect();

        // --- MISSING_COST ---
        if (!CostClass)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_COST"),
                TEXT("warning"),
                AssetPath,
                TEXT("Ability has no CostGameplayEffectClass. Intentional? Flag if mana/stamina cost expected."))));
        }

        // --- MISSING_COOLDOWN ---
        if (!CooldownClass)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_COOLDOWN"),
                TEXT("info"),
                AssetPath,
                TEXT("Ability has no CooldownGameplayEffectClass."))));
        }

        // --- COST_GE_NOT_INSTANT ---
        // A cost GE must be Instant so the stat is deducted once and immediately.
        // HasDuration or Infinite = permanent stat loss on each cast.
        if (CostClass)
        {
            if (CostClass->DurationPolicy != EGameplayEffectDurationType::Instant)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                    TEXT("COST_GE_NOT_INSTANT"),
                    TEXT("error"),
                    AssetPath,
                    FString::Printf(
                        TEXT("Cost GE '%s' has a non-Instant duration policy. This will apply a persistent modifier every time the ability is used, causing permanent stat loss."),
                        *GetNameSafe(CostClass->GetClass())))));
            }
        }

        // --- COOLDOWN_GE_NO_TAG ---
        // The ASC detects whether an ability is on cooldown by querying for tags the
        // cooldown GE grants. If no tags are granted, HasCooldown() always returns false.
        if (CooldownClass)
        {
            const FGameplayTagContainer* CooldownTags = CDO->GetCooldownTags();
            if (!CooldownTags || CooldownTags->IsEmpty())
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                    TEXT("COOLDOWN_GE_NO_TAG"),
                    TEXT("warning"),
                    AssetPath,
                    FString::Printf(
                        TEXT("Cooldown GE '%s' grants no gameplay tags. The ASC detects active cooldowns by tag presence — without tags, HasCooldown() always returns false and the ability can be reactivated immediately."),
                        *GetNameSafe(CooldownClass->GetClass())))));
            }
        }

        // --- NONINSTANCED_WITH_STATE ---
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        const bool bIsNonInstanced =
            (CDO->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::NonInstanced);
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
        if (bIsNonInstanced && BP->NewVariables.Num() > 0)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("NONINSTANCED_WITH_STATE"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("NonInstanced ability has %d Blueprint variable(s). State is shared across all actors — potential cross-actor contamination."),
                    BP->NewVariables.Num()))));
        }

        // --- MISSING_END_ABILITY ---
        const int32 TotalNodes = CountEventGraphNodes(BP);
        const bool bHasEndAbility = HasCallToFunction(BP, FName("EndAbility"));
        if (!bHasEndAbility && TotalNodes > 2)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_END_ABILITY"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("No EndAbility call found in %d graph node(s). Ability may never terminate."),
                    TotalNodes))));
        }

        // --- MISSING_COMMIT_ABILITY ---
        // An ability that has cost or cooldown MUST call CommitAbility to apply them.
        // Without it, the ability fires for free regardless of resource or cooldown state.
        const bool bHasCommit = HasCallToFunction(BP, FName("CommitAbility"));
        if (!bHasCommit && (CostClass || CooldownClass) && TotalNodes > 2)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_COMMIT_ABILITY"),
                TEXT("error"),
                AssetPath,
                TEXT("Ability has a cost or cooldown GE but no CommitAbility call. Cost is never deducted and cooldown never starts. Add CommitAbility and branch on its return value."))));
        }

        // --- UNCHECKED_COMMIT_RESULT ---
        // CommitAbility returns false when the cost cannot be paid or cooldown hasn't expired.
        // Ignoring the return value means the ability body executes even when resources are insufficient.
        if (bHasCommit && !IsCommitResultChecked(BP))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("UNCHECKED_COMMIT_RESULT"),
                TEXT("warning"),
                AssetPath,
                TEXT("CommitAbility is called but its return value (bool) is not connected to any node. If cost cannot be paid or cooldown is active, CommitAbility returns false — but the ability continues executing regardless."))));
        }

        // --- TASK_CREATED_NOT_READY ---
        // AbilityTask instances must have ReadyForActivation() called after creation.
        // Without it, the task is allocated but never begins executing.
        if (HasAbilityTaskUsage(BP) && !HasCallToFunction(BP, FName("ReadyForActivation")))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("TASK_CREATED_NOT_READY"),
                TEXT("error"),
                AssetPath,
                TEXT("AbilityTask usage detected but ReadyForActivation() is never called. Tasks created without ReadyForActivation() are allocated but never executed — the ability will stall silently."))));
        }

        // --- ABILITY_NO_INPUT ---
        // Disabled in UE 5.7: UGameplayAbility::AbilityTriggers and bActivateAbilityOnGranted
        // are protected members in 5.7 and cannot be accessed directly from outside the class.
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// AuditGameplayEffects
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeGASCapture::AuditGameplayEffects()
{
    TArray<TSharedPtr<FJsonValue>> Issues;

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    for (const FAssetData& AssetData : Assets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
        if (!BP || !BP->GeneratedClass) continue;
        if (!BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass())) continue;

        const UGameplayEffect* GE = BP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
        if (!GE) continue;

        const FString AssetPath = AssetData.GetObjectPathString();

        // --- INFINITE_STACKING_GE ---
        // Stacking enabled with no limit means a loop that applies this GE will grow unbounded.
        // StackLimitCount == 0 means unlimited when StackingType != None.
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        const EGameplayEffectStackingType CachedStackType = GE->StackingType;
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
        if (CachedStackType != EGameplayEffectStackingType::None && GE->StackLimitCount == 0)
        {
            const UEnum* StackEnum = StaticEnum<EGameplayEffectStackingType>();
            const FString StackTypeName = StackEnum
                ? StackEnum->GetNameStringByValue(static_cast<int64>(CachedStackType))
                : TEXT("unknown");

            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("INFINITE_STACKING_GE"),
                TEXT("warning"),
                AssetPath,
                FString::Printf(
                    TEXT("GameplayEffect uses stacking type '%s' with StackLimitCount == 0 (unlimited). A GE application loop will grow the stack without bound. Set StackLimitCount to an explicit cap."),
                    *StackTypeName))));
        }

        // --- GE_NEGATIVE_PERIOD ---
        // Period is a ScalableFloat. A raw negative value with no curve is unconditionally wrong —
        // the GAS periodic tick timer cannot run backwards.
        // Only flag when there is no curve override (raw Value is authoritative).
        if (GE->Period.Value < 0.f && !GE->Period.Curve.CurveTable)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("GE_NEGATIVE_PERIOD"),
                TEXT("error"),
                AssetPath,
                FString::Printf(
                    TEXT("GameplayEffect has a negative Period value (%.4f). The GAS periodic tick timer cannot use a negative interval. This GE will never tick."),
                    GE->Period.Value))));
        }

        // --- GE_MODIFIER_NO_ATTRIBUTE ---
        // A modifier that references an invalid or missing attribute has no effect.
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        for (int32 ModIdx = 0; ModIdx < GE->Modifiers.Num(); ++ModIdx)
        {
            if (!GE->Modifiers[ModIdx].Attribute.IsValid())
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                    TEXT("GE_MODIFIER_NO_ATTRIBUTE"),
                    TEXT("error"),
                    AssetPath,
                    FString::Printf(
                        TEXT("Modifier [%d] on '%s' targets an invalid or missing attribute. This modifier will have no effect."),
                        ModIdx, *GetNameSafe(GE->GetClass())))));
            }
        }
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// AuditAttributeSets
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeGASCapture::AuditAttributeSets()
{
    TArray<TSharedPtr<FJsonValue>> Issues;

    TArray<UClass*> AttrSetClasses;
    GetDerivedClasses(UAttributeSet::StaticClass(), AttrSetClasses, /*bRecursive=*/true);

    for (UClass* AttrClass : AttrSetClasses)
    {
        if (!AttrClass || AttrClass->HasAnyClassFlags(CLASS_Abstract)) continue;

        // Only deeply analyse Blueprint-generated attribute sets.
        // C++ subclasses are opaque to static graph inspection.
        UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(AttrClass);
        if (!BPGC) continue;

        UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
        if (!BP) continue;

        const FString AssetPath = BP->GetPathName();

        const bool bHasPreAttr  = HasFunctionGraph(BP, FName("PreAttributeChange"));
        const bool bHasPostExec = HasFunctionGraph(BP, FName("PostGameplayEffectExecute"));

        if (!bHasPreAttr && !bHasPostExec)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("NO_CLAMPING_FUNCTIONS"),
                TEXT("warning"),
                AssetPath,
                TEXT("AttributeSet has neither PreAttributeChange nor PostGameplayEffectExecute overrides. Attribute values (e.g. Health) may exceed min/max bounds."))));
        }
        else if (!bHasPreAttr)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("MISSING_PRE_ATTRIBUTE_CHANGE"),
                TEXT("info"),
                AssetPath,
                TEXT("AttributeSet implements PostGameplayEffectExecute but not PreAttributeChange. Direct SetAttributeBase calls won't be clamped."))));
        }
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// AuditTags
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeGASCapture::AuditTags()
{
    TArray<TSharedPtr<FJsonValue>> Issues;

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    for (const FAssetData& AssetData : Assets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
        if (!BP || !BP->GeneratedClass) continue;
        if (!BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass())) continue;

        UGameplayAbility* CDO = Cast<UGameplayAbility>(BP->GeneratedClass->GetDefaultObject());
        if (!CDO) continue;

        const FString AssetPath = AssetData.GetObjectPathString();

        // Use reflection to access ALL FGameplayTagContainer properties on the CDO.
        // This avoids protected-member C2248 errors and catches custom containers too.
        for (TFieldIterator<FStructProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
        {
            FStructProperty* Prop = *PropIt;
            if (Prop->Struct != FGameplayTagContainer::StaticStruct()) continue;

            const FGameplayTagContainer* Container =
                Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
            if (!Container) continue;

            TArray<FString> Bad = InvalidTagsIn(*Container);
            for (const FString& BadTag : Bad)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                    TEXT("INVALID_TAG"),
                    TEXT("error"),
                    AssetPath,
                    FString::Printf(TEXT("Invalid/unregistered tag in %s: '%s'"),
                                    *Prop->GetName(), *BadTag))));
            }
        }

        // GetAssetTags() is the UE5.5+ replacement for the deprecated AbilityTags field.
        if (CDO->GetAssetTags().IsEmpty())
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(
                TEXT("UNTAGGED_ABILITY"),
                TEXT("info"),
                AssetPath,
                TEXT("AssetTags is empty. This ability cannot be queried, cancelled, or blocked by tag."))));
        }
    }

    return Issues;
}

// ---------------------------------------------------------------------------
// MakeIssue
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeGASCapture::MakeIssue(
    const FString& IssueType,
    const FString& Severity,
    const FString& AssetPath,
    const FString& Detail)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("issue_type"), IssueType);
    Obj->SetStringField(TEXT("severity"),   Severity);
    Obj->SetStringField(TEXT("asset_path"), AssetPath);
    Obj->SetStringField(TEXT("detail"),     Detail);
    return Obj;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeGASCapture::UpdateIndexFile(int32 IssueCount, const FString& AuditPath)
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* FoundField = Root->Values.Find(TEXT("captures_available")))
    {
        if (FoundField->IsValid() && (*FoundField)->Type == EJson::Object)
        {
            Captures = (*FoundField)->AsObject();
        }
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> GasSection = MakeShared<FJsonObject>();
    GasSection->SetStringField(TEXT("file"),         AuditPath);
    GasSection->SetNumberField(TEXT("total_issues"), IssueCount);
    GasSection->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    Captures->SetObjectField(TEXT("gas"), GasSection);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
