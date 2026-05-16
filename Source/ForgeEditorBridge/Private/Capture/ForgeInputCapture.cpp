#include "Capture/ForgeInputCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Enhanced Input ---
#include "InputMappingContext.h"
#include "InputAction.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedActionKeyMapping.h"
#include "InputTriggers.h"
#include "InputModifiers.h"

// --- Player / Local Player ---
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"

// --- World / Editor ---
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/TopLevelAssetPath.h"

// --- UObject reflection ---
#include "UObject/UnrealType.h"

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
	// Keys that, if not consumed, can trigger unintended system-level behaviour
	const TSet<FString> HighImpactKeys =
	{
		TEXT("Escape"), TEXT("Enter"), TEXT("SpaceBar"),
		TEXT("Tab"),    TEXT("BackSpace")
	};

	FString ValueTypeToString(EInputActionValueType T)
	{
		switch (T)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
		case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
		case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
		default:                             return TEXT("Unknown");
		}
	}

	// Serialize a single FEnhancedActionKeyMapping into a JSON object
	TSharedRef<FJsonObject> SerializeMapping(const FEnhancedActionKeyMapping& M)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

		const UInputAction* Action = M.Action.Get();
		Obj->SetStringField(TEXT("action"),      Action ? Action->GetPathName() : TEXT("(none)"));
		Obj->SetStringField(TEXT("action_name"), Action ? Action->GetName()     : TEXT("(none)"));
		Obj->SetStringField(TEXT("key"),         M.Key.ToString());

		// Modifiers
		TArray<TSharedPtr<FJsonValue>> ModsArr;
		for (const TObjectPtr<UInputModifier>& Mod : M.Modifiers)
			if (Mod) ModsArr.Add(MakeShared<FJsonValueString>(Mod->GetClass()->GetName()));
		Obj->SetNumberField(TEXT("modifier_count"), ModsArr.Num());
		Obj->SetArrayField (TEXT("modifiers"),      ModsArr);

		// Triggers
		TArray<TSharedPtr<FJsonValue>> TrigsArr;
		for (const TObjectPtr<UInputTrigger>& Trig : M.Triggers)
			if (Trig) TrigsArr.Add(MakeShared<FJsonValueString>(Trig->GetClass()->GetName()));
		Obj->SetNumberField(TEXT("trigger_count"), TrigsArr.Num());
		Obj->SetArrayField (TEXT("triggers"),      TrigsArr);

		return Obj;
	}

	// Load all project assets of a given class via Asset Registry (UE5.1+ ClassPaths API)
	TArray<FAssetData> GetProjectAssets(
		IAssetRegistry& AR, const FString& ScriptPackage, const FString& ClassName)
	{
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.ClassPaths.Add(FTopLevelAssetPath(*ScriptPackage, *ClassName));

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);
		return Assets;
	}
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeInputCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("input")));
	UE_LOG(LogTemp, Log, TEXT("ForgeInput: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportInputAudit
// ---------------------------------------------------------------------------

bool UForgeInputCapture::ExportInputAudit()
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeInput: GEditor is null"));
		return false;
	}

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
	Root->SetStringField(TEXT("level_name"),
		EditorWorld ? EditorWorld->GetName() : TEXT("(none)"));

	TArray<TSharedPtr<FJsonValue>> Issues;

	IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// -------------------------------------------------------------------------
	// Load all UInputMappingContext assets
	// -------------------------------------------------------------------------
	TArray<FAssetData> ContextAssets =
		GetProjectAssets(AR, TEXT("/Script/EnhancedInput"), TEXT("InputMappingContext"));

	// For INPUT_CONFLICT: track Key -> [(ContextName, ActionPath)]
	TMap<FString, TArray<TPair<FString, FString>>> KeyToActionMap;

	// For DEAD_ACTION: track which action paths appear in at least one mapping
	TSet<FString> MappedActionPaths;

	TArray<TSharedPtr<FJsonValue>> ContextsArr;

	for (const FAssetData& AD : ContextAssets)
	{
		UInputMappingContext* IMC = Cast<UInputMappingContext>(AD.GetAsset());
		if (!IMC) continue;

		TSharedRef<FJsonObject> IMCObj = MakeShared<FJsonObject>();
		IMCObj->SetStringField(TEXT("asset"), AD.PackageName.ToString());
		IMCObj->SetStringField(TEXT("name"),  AD.AssetName.ToString());

		const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
		IMCObj->SetNumberField(TEXT("mapping_count"), Mappings.Num());

		TArray<TSharedPtr<FJsonValue>> MappingsArr;
		for (const FEnhancedActionKeyMapping& M : Mappings)
		{
			MappingsArr.Add(MakeShared<FJsonValueObject>(SerializeMapping(M)));

			const UInputAction* Action = M.Action.Get();
			const FString ActionPath  = Action ? Action->GetPathName() : TEXT("(none)");
			const FString KeyStr      = M.Key.ToString();
			const FString ContextName = AD.AssetName.ToString();

			// Accumulate for conflict detection
			KeyToActionMap.FindOrAdd(KeyStr).Add(
				TPair<FString, FString>(ContextName, ActionPath));

			// Mark this action as mapped
			if (Action) MappedActionPaths.Add(ActionPath);

			// AUDIT: MISSING_CONSUMPTION
			if (Action && !Action->bConsumeInput && HighImpactKeys.Contains(KeyStr))
			{
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("issue_type"), TEXT("MISSING_CONSUMPTION"));
				Issue->SetStringField(TEXT("severity"),   TEXT("info"));
				Issue->SetStringField(TEXT("detail"),
					FString::Printf(
						TEXT("Context '%s' maps high-impact key '%s' to action '%s' "
						     "but the action has bConsumeInput=false. This key event will "
						     "fall through to lower-priority contexts or Slate focus handlers, "
						     "which may cause unexpected UI interactions."),
						*ContextName, *KeyStr,
						Action ? *Action->GetName() : TEXT("(none)")));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}

		IMCObj->SetArrayField(TEXT("mappings"), MappingsArr);
		ContextsArr.Add(MakeShared<FJsonValueObject>(IMCObj));
	}

	Root->SetArrayField(TEXT("mapping_contexts"), ContextsArr);

	// -------------------------------------------------------------------------
	// AUDIT: INPUT_CONFLICT
	// Same key maps to different actions across project contexts (static analysis)
	// -------------------------------------------------------------------------
	for (auto& Pair : KeyToActionMap)
	{
		const FString& Key          = Pair.Key;
		const TArray<TPair<FString, FString>>& Entries = Pair.Value;
		if (Entries.Num() < 2) continue;

		// Collect distinct action paths for this key
		TSet<FString> UniqueActions;
		for (const TPair<FString, FString>& E : Entries)
			UniqueActions.Add(E.Value);

		if (UniqueActions.Num() < 2) continue; // same action in multiple contexts — fine

		// Build a human-readable summary: "ContextA -> ActionX, ContextB -> ActionY"
		TArray<FString> Lines;
		for (const TPair<FString, FString>& E : Entries)
			Lines.Add(FString::Printf(TEXT("%s -> %s"), *E.Key, *E.Value));

		TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("issue_type"), TEXT("INPUT_CONFLICT"));
		Issue->SetStringField(TEXT("severity"),   TEXT("error"));
		Issue->SetStringField(TEXT("detail"),
			FString::Printf(
				TEXT("Key '%s' is mapped to %d different actions across project contexts. "
				     "At runtime the highest-priority active context wins, but this may "
				     "produce unintended behaviour if both contexts are applied simultaneously. "
				     "Mappings: %s"),
				*Key, UniqueActions.Num(),
				*FString::Join(Lines, TEXT(" | "))));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	// -------------------------------------------------------------------------
	// Load all UInputAction assets + DEAD_ACTION audit
	// -------------------------------------------------------------------------
	TArray<FAssetData> ActionAssets =
		GetProjectAssets(AR, TEXT("/Script/EnhancedInput"), TEXT("InputAction"));

	TArray<TSharedPtr<FJsonValue>> ActionsArr;

	for (const FAssetData& AD : ActionAssets)
	{
		UInputAction* IA = Cast<UInputAction>(AD.GetAsset());
		if (!IA) continue;

		const int32 ModCount  = IA->Modifiers.Num();
		const int32 TrigCount = IA->Triggers.Num();

		// Count how many contexts reference this action
		int32 ContextRefCount = 0;
		for (const FAssetData& CxtAD : ContextAssets)
		{
			if (UInputMappingContext* IMC = Cast<UInputMappingContext>(CxtAD.GetAsset()))
			{
				for (const FEnhancedActionKeyMapping& M : IMC->GetMappings())
					if (M.Action.Get() == IA) { ++ContextRefCount; break; }
			}
		}

		TSharedRef<FJsonObject> IAObj = MakeShared<FJsonObject>();
		IAObj->SetStringField(TEXT("asset"),               AD.PackageName.ToString());
		IAObj->SetStringField(TEXT("name"),                AD.AssetName.ToString());
		IAObj->SetStringField(TEXT("value_type"),          ValueTypeToString(IA->ValueType));
		IAObj->SetBoolField  (TEXT("consume_input"),       IA->bConsumeInput);
		IAObj->SetBoolField  (TEXT("trigger_when_paused"), IA->bTriggerWhenPaused);
		IAObj->SetNumberField(TEXT("modifier_count"),      ModCount);
		IAObj->SetNumberField(TEXT("trigger_count"),       TrigCount);
		IAObj->SetNumberField(TEXT("mapped_in_context_count"), ContextRefCount);
		ActionsArr.Add(MakeShared<FJsonValueObject>(IAObj));

		// AUDIT: DEAD_ACTION
		if (ContextRefCount == 0)
		{
			TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("issue_type"), TEXT("DEAD_ACTION"));
			Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
			Issue->SetStringField(TEXT("detail"),
				FString::Printf(
					TEXT("InputAction '%s' (%s) exists in the project but is not mapped in any "
					     "UInputMappingContext. It can never be triggered. Either map it or delete "
					     "the asset to avoid confusion."),
					*AD.AssetName.ToString(),
					*ValueTypeToString(IA->ValueType)));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	Root->SetArrayField(TEXT("input_actions"), ActionsArr);

	// -------------------------------------------------------------------------
	// Active Player Contexts — PIE only
	// Iterates APlayerControllers in the play world and queries the
	// UEnhancedInputLocalPlayerSubsystem for each applied context + priority.
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ActivePlayersArr;

	UWorld* PlayWorld = GEditor->PlayWorld;
	if (PlayWorld)
	{
		int32 PlayerIndex = 0;
		for (TActorIterator<APlayerController> It(PlayWorld); It; ++It, ++PlayerIndex)
		{
			APlayerController* PC = *It;
			if (!PC) continue;

			ULocalPlayer* LP = PC->GetLocalPlayer();
			if (!LP) continue;

			UEnhancedInputLocalPlayerSubsystem* EISubsystem =
				LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
			if (!EISubsystem) continue;

			TSharedRef<FJsonObject> PlayerObj = MakeShared<FJsonObject>();
			PlayerObj->SetNumberField(TEXT("player_index"), PlayerIndex);

			TArray<TSharedPtr<FJsonValue>> AppliedArr;

			// Check each known project context against the subsystem
			for (const FAssetData& CxtAD : ContextAssets)
			{
				UInputMappingContext* IMC = Cast<UInputMappingContext>(CxtAD.GetAsset());
				if (!IMC) continue;

				int32 OutPriority = -1;
				if (EISubsystem->HasMappingContext(IMC, OutPriority))
				{
					TSharedRef<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
					AppliedObj->SetStringField(TEXT("asset"),    CxtAD.PackageName.ToString());
					AppliedObj->SetStringField(TEXT("name"),     CxtAD.AssetName.ToString());
					AppliedObj->SetNumberField(TEXT("priority"), OutPriority);
					AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedObj));
				}
			}

			// Sort applied contexts by priority descending for readability
			AppliedArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				const double PriA = A->AsObject()->GetNumberField(TEXT("priority"));
				const double PriB = B->AsObject()->GetNumberField(TEXT("priority"));
				return PriA > PriB;
			});

			PlayerObj->SetArrayField(TEXT("contexts"), AppliedArr);
			ActivePlayersArr.Add(MakeShared<FJsonValueObject>(PlayerObj));
		}
	}
	else
	{
		// Not in PIE — emit a single informational entry so the consumer knows why it's empty
		TSharedRef<FJsonObject> InfoObj = MakeShared<FJsonObject>();
		InfoObj->SetStringField(TEXT("note"),
			TEXT("Active player context state requires PIE. "
			     "Start Play-In-Editor and re-run export_input_audit() to capture live state."));
		ActivePlayersArr.Add(MakeShared<FJsonValueObject>(InfoObj));
	}

	Root->SetArrayField(TEXT("active_player_contexts"), ActivePlayersArr);

	// -------------------------------------------------------------------------
	// Audit summary
	// -------------------------------------------------------------------------
	{
		TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
		AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
		AuditObj->SetArrayField (TEXT("issues"),       Issues);
		Root->SetObjectField(TEXT("audit"), AuditObj);
	}

	const int32 ContextCount = ContextsArr.Num();
	const int32 ActionCount  = ActionsArr.Num();

	bool bOK = FForgeContextWriter::WriteJSON(
		OutputDir / TEXT("input"), TEXT("enhanced_input_audit"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log,
			TEXT("ForgeInput: Exported -> input/enhanced_input_audit.json "
			     "(%d context(s), %d action(s), %d issue(s))"),
			ContextCount, ActionCount, Issues.Num());
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeInputCapture::UpdateIndexFile()
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
	if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
	{
		if (Found->IsValid() && (*Found)->Type == EJson::Object)
			Captures = (*Found)->AsObject();
	}
	if (!Captures.IsValid())
	{
		Captures = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("captures_available"), Captures);
	}

	TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
	Section->SetStringField(TEXT("file"),         TEXT("input/enhanced_input_audit.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("input"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
