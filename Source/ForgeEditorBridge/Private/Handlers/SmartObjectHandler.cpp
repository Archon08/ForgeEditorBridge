#include "Handlers/SmartObjectHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// ---- Smart Object types ----------------------------------------------------
#include "SmartObjectDefinition.h"
#include "SmartObjectComponent.h"
#include "SmartObjectTypes.h"

// ---- Gameplay Tags ---------------------------------------------------------
#include "GameplayTagsManager.h"

// ---- Asset tools -----------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"

// ---- World / actors --------------------------------------------------------
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"

// ---- Transactions ----------------------------------------------------------
#include "ScopedTransaction.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Misc ------------------------------------------------------------------
#include "Misc/PackageName.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("smart_object");

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return UBridgeHandlerBase::GetSafeEditorWorld();
#else
	return nullptr;
#endif
}

/** Find an actor in the editor world by label. */
static AActor* FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			return *It;
		}
	}
	return nullptr;
}

FBridgeResult USmartObjectHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- create_smart_object ----
	if (Action == TEXT("create_smart_object"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString Path, Name;
		AssetPath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Name.IsEmpty() || Path.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Invalid asset_path format. Expected: /Game/Path/AssetName"));

		// Check if asset already exists
		FString FullPath = AssetPath + TEXT(".") + Name;
		UObject* Existing = LoadObject<UObject>(nullptr, *FullPath);
		if (Existing)
			return this->MakeError(DOMAIN, Action, 2002, FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Smart Object Definition")));

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* NewAsset = AT.CreateAsset(Name, Path, USmartObjectDefinition::StaticClass(), nullptr);
		if (!NewAsset)
			return this->MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to create SmartObjectDefinition at '%s'"), *AssetPath),
				TEXT("Ensure the SmartObjects plugin is enabled"));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Created SmartObjectDefinition '%s'"), *Name),
			Data);
	}

	// ---- add_slot ----
	if (Action == TEXT("add_slot"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		// Load the definition asset
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		USmartObjectDefinition* Definition = LoadObject<USmartObjectDefinition>(nullptr, *AssetPath);
		if (!Definition)
		{
			Definition = LoadObject<USmartObjectDefinition>(nullptr, *FullPath);
		}
		if (!Definition)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No USmartObjectDefinition found at '%s'"), *AssetPath),
				TEXT("Ensure the asset exists and is a SmartObjectDefinition"));

		// Parse offset
		double OffX = 0.0, OffY = 0.0, OffZ = 0.0;
		const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
		if (Params->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && (*OffsetObj).IsValid())
		{
			(*OffsetObj)->TryGetNumberField(TEXT("x"), OffX);
			(*OffsetObj)->TryGetNumberField(TEXT("y"), OffY);
			(*OffsetObj)->TryGetNumberField(TEXT("z"), OffZ);
		}

		// Parse rotation
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj).IsValid())
		{
			(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Add Smart Object Slot")));

		// Add a new slot to the definition
		// UE 5.7: GetMutableSlots() returns TArrayView, so we access the Slots TArray via reflection
		FProperty* SlotsProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("Slots"));
		TArray<FSmartObjectSlotDefinition>* SlotsPtr = nullptr;
		if (SlotsProp)
		{
			SlotsPtr = SlotsProp->ContainerPtrToValuePtr<TArray<FSmartObjectSlotDefinition>>(Definition);
		}
		if (!SlotsPtr)
			return this->MakeError(DOMAIN, Action, 3000, TEXT("Could not access Slots array on SmartObjectDefinition"));

		FSmartObjectSlotDefinition& NewSlot = SlotsPtr->AddDefaulted_GetRef();

		NewSlot.Offset = FVector3f((float)OffX, (float)OffY, (float)OffZ);
		NewSlot.Rotation = FRotator3f((float)Pitch, (float)Yaw, (float)Roll);

		Definition->MarkPackageDirty();

		int32 SlotIndex = SlotsPtr->Num() - 1;

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetNumberField(TEXT("slot_index"), SlotIndex);
		Data->SetNumberField(TEXT("offset_x"), OffX);
		Data->SetNumberField(TEXT("offset_y"), OffY);
		Data->SetNumberField(TEXT("offset_z"), OffZ);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Added slot %d at offset (%.1f, %.1f, %.1f) to '%s'"),
				SlotIndex, (float)OffX, (float)OffY, (float)OffZ, *AssetPath),
			Data);
	}

	// ---- set_slot_tag ----
	if (Action == TEXT("set_slot_tag"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		int32 SlotIndex = -1;
		if (!Params->TryGetNumberField(TEXT("slot_index"), *(double*)&SlotIndex))
		{
			double SlotIndexD = -1.0;
			if (!Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD))
				return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'slot_index'"));
			SlotIndex = (int32)SlotIndexD;
		}

		FString TagString;
		if (!Params->TryGetStringField(TEXT("tag"), TagString) || TagString.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'tag'"));

		// Load the definition asset
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		USmartObjectDefinition* Definition = LoadObject<USmartObjectDefinition>(nullptr, *AssetPath);
		if (!Definition)
		{
			Definition = LoadObject<USmartObjectDefinition>(nullptr, *FullPath);
		}
		if (!Definition)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No USmartObjectDefinition found at '%s'"), *AssetPath),
				TEXT("Ensure the asset exists and is a SmartObjectDefinition"));

		TArrayView<FSmartObjectSlotDefinition> Slots = Definition->GetMutableSlots();
		if (!Slots.IsValidIndex(SlotIndex))
			return this->MakeError(DOMAIN, Action, 1001,
				FString::Printf(TEXT("Slot index %d out of range (0-%d)"), SlotIndex, Slots.Num() - 1));

		// Resolve the gameplay tag
		FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagString), /*bErrorIfNotFound=*/false);
		if (!Tag.IsValid())
		{
			// Try adding it dynamically
			Tag = UGameplayTagsManager::Get().AddNativeGameplayTag(FName(*TagString));
		}
		if (!Tag.IsValid())
			return this->MakeError(DOMAIN, Action, 1001,
				FString::Printf(TEXT("Could not resolve gameplay tag '%s'"), *TagString),
				TEXT("Ensure the tag exists in your GameplayTags config or use a valid tag path"));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Smart Object Slot Tag")));

		Slots[SlotIndex].ActivityTags.AddTag(Tag);
		Definition->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetNumberField(TEXT("slot_index"), SlotIndex);
		Data->SetStringField(TEXT("tag"), TagString);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Added tag '%s' to slot %d of '%s'"), *TagString, SlotIndex, *AssetPath),
			Data);
	}

	// ---- place_smart_object ----
	if (Action == TEXT("place_smart_object"))
	{
		FString DefinitionPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("definition"), DefinitionPath) || DefinitionPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'definition'"));

		// Parse location
		double LocX = 0.0, LocY = 0.0, LocZ = 0.0;
		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj && (*LocObj).IsValid())
		{
			(*LocObj)->TryGetNumberField(TEXT("x"), LocX);
			(*LocObj)->TryGetNumberField(TEXT("y"), LocY);
			(*LocObj)->TryGetNumberField(TEXT("z"), LocZ);
		}

		// Parse rotation
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj).IsValid())
		{
			(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
		}

		// Load the SmartObjectDefinition
		FString FullPath = DefinitionPath + TEXT(".") + FPackageName::GetLongPackageAssetName(DefinitionPath);
		USmartObjectDefinition* Definition = LoadObject<USmartObjectDefinition>(nullptr, *DefinitionPath);
		if (!Definition)
		{
			Definition = LoadObject<USmartObjectDefinition>(nullptr, *FullPath);
		}
		if (!Definition)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No USmartObjectDefinition found at '%s'"), *DefinitionPath),
				TEXT("Ensure the asset exists and is a SmartObjectDefinition"));

		UWorld* World = GetEditorWorld();
		if (!World)
			return this->MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Place Smart Object")));

		// Spawn an actor with a SmartObjectComponent
		FTransform SpawnTransform;
		SpawnTransform.SetLocation(FVector((float)LocX, (float)LocY, (float)LocZ));
		SpawnTransform.SetRotation(FRotator((float)Pitch, (float)Yaw, (float)Roll).Quaternion());

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnTransform, SpawnParams);
		if (!NewActor)
			return this->MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to spawn actor at (%.0f, %.0f, %.0f)"), (float)LocX, (float)LocY, (float)LocZ));

		// Add root component if needed
		if (!NewActor->GetRootComponent())
		{
			USceneComponent* Root = NewObject<USceneComponent>(NewActor, TEXT("DefaultSceneRoot"));
			Root->SetMobility(EComponentMobility::Movable);
			NewActor->SetRootComponent(Root);
			Root->RegisterComponent();
		}

		// Add SmartObjectComponent and assign the definition
		USmartObjectComponent* SOComp = NewObject<USmartObjectComponent>(NewActor, TEXT("SmartObjectComponent"));
		SOComp->SetDefinition(Definition);
		SOComp->AttachToComponent(NewActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		SOComp->RegisterComponent();
		NewActor->AddInstanceComponent(SOComp);

		FString ActorLabel = FString::Printf(TEXT("SmartObject_%s"), *Definition->GetName());
		NewActor->SetActorLabel(ActorLabel);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("definition"), DefinitionPath);
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetNumberField(TEXT("location_x"), LocX);
		Data->SetNumberField(TEXT("location_y"), LocY);
		Data->SetNumberField(TEXT("location_z"), LocZ);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Placed SmartObject '%s' at (%.0f, %.0f, %.0f) as '%s'"),
				*Definition->GetName(), (float)LocX, (float)LocY, (float)LocZ, *ActorLabel),
			Data);
	}

	if (Action == TEXT("get_smart_object_info")) return Action_GetSmartObjectInfo(Params);
	if (Action == TEXT("list_slots"))            return Action_ListSlots(Params);

	return this->MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'"), *Action),
		TEXT("smart_object capabilities: create_smart_object, add_slot, set_slot_tag, place_smart_object"));
}

TSharedPtr<FJsonObject> USmartObjectHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a SmartObjectDefinition asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new definition"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_smart_object"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a slot to a SmartObjectDefinition")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the definition"))); Ps->SetObjectField(TEXT("offset"), P(TEXT("object"), false, TEXT("Slot offset {x,y,z}"))); Ps->SetObjectField(TEXT("rotation"), P(TEXT("object"), false, TEXT("Slot rotation {pitch,yaw,roll}"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_slot"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a gameplay tag to a smart object slot")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the definition"))); Ps->SetObjectField(TEXT("slot_index"), P(TEXT("int"), true, TEXT("Index of the slot"))); Ps->SetObjectField(TEXT("tag"), P(TEXT("string"), true, TEXT("Gameplay tag string"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_slot_tag"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn an actor with a SmartObjectComponent in the level")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("definition"), P(TEXT("string"), true, TEXT("Content path of the SmartObjectDefinition"))); Ps->SetObjectField(TEXT("location"), P(TEXT("object"), false, TEXT("Spawn location {x,y,z}"))); Ps->SetObjectField(TEXT("rotation"), P(TEXT("object"), false, TEXT("Spawn rotation {pitch,yaw,roll}"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("place_smart_object"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// get_smart_object_info
// ---------------------------------------------------------------------------

FBridgeResult USmartObjectHandler::Action_GetSmartObjectInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_smart_object_info");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	USmartObjectDefinition* Def = LoadObject<USmartObjectDefinition>(nullptr, *AssetPath);
	if (!Def) Def = LoadObject<USmartObjectDefinition>(nullptr, *FullPath);
	if (!Def)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No USmartObjectDefinition found at '%s'"), *AssetPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), Def->GetName());
	const int32 SlotCount = Def->GetSlots().Num();
	Data->SetNumberField(TEXT("slot_count"), SlotCount);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("get_smart_object_info: '%s' — %d slot(s)"), *AssetPath, SlotCount));
	R.ExtraData = OutStr;
	return R;
}

// ---------------------------------------------------------------------------
// list_slots
// ---------------------------------------------------------------------------

FBridgeResult USmartObjectHandler::Action_ListSlots(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("list_slots");

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

	FString FullPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	USmartObjectDefinition* Def = LoadObject<USmartObjectDefinition>(nullptr, *AssetPath);
	if (!Def) Def = LoadObject<USmartObjectDefinition>(nullptr, *FullPath);
	if (!Def)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No USmartObjectDefinition found at '%s'"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> SlotsArr;
	const TArrayView<const FSmartObjectSlotDefinition> Slots = Def->GetSlots();
	const int32 NumSlots = Slots.Num();
	for (int32 i = 0; i < NumSlots; ++i)
	{
		TSharedPtr<FJsonObject> SlotEntry = MakeShared<FJsonObject>();
		SlotEntry->SetNumberField(TEXT("index"), i);

		const FSmartObjectSlotDefinition& Slot = Slots[i];

		TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("x"), Slot.Offset.X);
		Offset->SetNumberField(TEXT("y"), Slot.Offset.Y);
		Offset->SetNumberField(TEXT("z"), Slot.Offset.Z);
		SlotEntry->SetObjectField(TEXT("offset"), Offset);

		// ActivityTags replaced Slot.Tags in UE 5.7
		TArray<TSharedPtr<FJsonValue>> TagsArr;
		for (const FGameplayTag& Tag : Slot.ActivityTags)
			TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		SlotEntry->SetArrayField(TEXT("tags"), TagsArr);
		SlotsArr.Add(MakeShared<FJsonValueObject>(SlotEntry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("slots"), SlotsArr);
	Data->SetNumberField(TEXT("count"), SlotsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = this->MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("list_slots: %d slot(s) in '%s'"), NumSlots, *AssetPath));
	R.ExtraData = OutStr;
	return R;
}
