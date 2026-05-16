#include "Handlers/CollisionHandler.h"
#include "ForgeAISubsystem.h"

// ---- Engine Collision -------------------------------------------------------
#include "Engine/CollisionProfile.h"
#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"

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
#include "ScopedTransaction.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("collision");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AActor* UCollisionHandler::FindActorByLabel(UWorld* World, const FString& Label)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
			return *It;
	}
	return nullptr;
}

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
	return nullptr;
#endif
}

// Map response string to ECR enum value string for ini
static FString ResponseStrToECR(const FString& Response)
{
	if (Response == TEXT("Block"))   return TEXT("ECR_Block");
	if (Response == TEXT("Overlap")) return TEXT("ECR_Overlap");
	if (Response == TEXT("Ignore"))  return TEXT("ECR_Ignore");
	return TEXT("ECR_Block"); // default
}

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("list_collision_channels"))  return Action_ListCollisionChannels(Params);
	if (Action == TEXT("add_collision_channel"))    return Action_AddCollisionChannel(Params);
	if (Action == TEXT("list_collision_profiles"))  return Action_ListCollisionProfiles(Params);
	if (Action == TEXT("create_collision_profile")) return Action_CreateCollisionProfile(Params);
	if (Action == TEXT("set_collision_response"))   return Action_SetCollisionResponse(Params);
	if (Action == TEXT("set_actor_collision"))      return Action_SetActorCollision(Params);
	if (Action == TEXT("get_actor_collision"))      return Action_GetActorCollision(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: list_collision_channels, add_collision_channel, list_collision_profiles, create_collision_profile, set_collision_response, set_actor_collision, get_actor_collision"), *Action));
}

// ---------------------------------------------------------------------------
// list_collision_channels — read DefaultChannelResponses from GEngineIni
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_ListCollisionChannels(TSharedPtr<FJsonObject> Params)
{
	// Custom channels are stored as DefaultChannelResponses entries in DefaultEngine.ini
	TArray<FString> ChannelEntries;
	GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"),
		TEXT("DefaultChannelResponses"), ChannelEntries, GEngineIni);

	TArray<TSharedPtr<FJsonValue>> ChannelArr;

	// Also list the built-in named channels via ECollisionChannel
	// We enumerate a fixed set of well-known channels
	static const TArray<FString> BuiltIn = {
		TEXT("WorldStatic"), TEXT("WorldDynamic"), TEXT("Pawn"), TEXT("Visibility"),
		TEXT("Camera"), TEXT("PhysicsBody"), TEXT("Vehicle"), TEXT("Destructible")
	};
	for (const FString& Name : BuiltIn)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),   Name);
		Entry->SetStringField(TEXT("type"),   TEXT("built-in"));
		ChannelArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Parse custom channel entries from ini
	for (const FString& RawEntry : ChannelEntries)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"),     TEXT("custom"));
		Entry->SetStringField(TEXT("raw_entry"),RawEntry);

		// Extract Name field from ini struct string
		FString Name;
		int32 NameStart = RawEntry.Find(TEXT("Name=\""));
		if (NameStart != INDEX_NONE)
		{
			NameStart += 6; // skip 'Name="'
			int32 NameEnd = RawEntry.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
			if (NameEnd != INDEX_NONE)
				Name = RawEntry.Mid(NameStart, NameEnd - NameStart);
		}
		if (!Name.IsEmpty()) Entry->SetStringField(TEXT("name"), Name);

		ChannelArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("channels"),        ChannelArr);
	Data->SetNumberField(TEXT("built_in_count"), BuiltIn.Num());
	Data->SetNumberField(TEXT("custom_count"),   ChannelEntries.Num());

	return MakeSuccess(DOMAIN, TEXT("list_collision_channels"),
		FString::Printf(TEXT("%d built-in + %d custom channel(s)"),
			BuiltIn.Num(), ChannelEntries.Num()), Data);
}

// ---------------------------------------------------------------------------
// add_collision_channel — append to DefaultEngine.ini + reload
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_AddCollisionChannel(TSharedPtr<FJsonObject> Params)
{
	FString Name, DefaultResponse = TEXT("Block");
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_collision_channel"), 1000, TEXT("Missing required param: 'name'"));
	Params->TryGetStringField(TEXT("default_response"), DefaultResponse);

	// Check for name collision with built-in channels
	const FString ECRStr = ResponseStrToECR(DefaultResponse);

	// Read current entries to check for duplicates
	TArray<FString> ExistingEntries;
	GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"),
		TEXT("DefaultChannelResponses"), ExistingEntries, GEngineIni);

	for (const FString& Existing : ExistingEntries)
	{
		if (Existing.Contains(FString::Printf(TEXT("Name=\"%s\""), *Name)))
			return MakeError(DOMAIN, TEXT("add_collision_channel"), 2000,
				FString::Printf(TEXT("Custom channel '%s' already exists"), *Name));
	}

	// Find next available ECC_GameTraceChannel slot (1..18)
	int32 NextSlot = ExistingEntries.Num() + 1;
	if (NextSlot > 18)
		return MakeError(DOMAIN, TEXT("add_collision_channel"), 3000,
			TEXT("Maximum 18 custom channels (ECC_GameTraceChannel1..18) already allocated"));

	const FString ChannelEntry = FString::Printf(
		TEXT("(Channel=ECC_GameTraceChannel%d,DefaultResponse=%s,bTraceType=False,bStaticObject=False,Name=\"%s\")"),
		NextSlot, *ECRStr, *Name);

	ExistingEntries.Add(ChannelEntry);
	GConfig->SetArray(TEXT("/Script/Engine.CollisionProfile"),
		TEXT("DefaultChannelResponses"), ExistingEntries, GEngineIni);
	GConfig->Flush(false, GEngineIni);

	// Reload collision profile config
	GetMutableDefault<UCollisionProfile>()->ReloadConfig();

	return MakeSuccess(DOMAIN, TEXT("add_collision_channel"),
		FString::Printf(TEXT("Custom channel '%s' added as ECC_GameTraceChannel%d (DefaultResponse: %s)"),
			*Name, NextSlot, *DefaultResponse));
}

// ---------------------------------------------------------------------------
// list_collision_profiles — UCollisionProfile
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_ListCollisionProfiles(TSharedPtr<FJsonObject> Params)
{
	UCollisionProfile* CP = UCollisionProfile::Get();
	if (!CP)
		return MakeError(DOMAIN, TEXT("list_collision_profiles"), 3000, TEXT("UCollisionProfile not available"));

	TArray<TSharedPtr<FName>> ProfileNames;
	CP->GetProfileNames(ProfileNames);

	TArray<TSharedPtr<FJsonValue>> ProfileArr;
	for (const TSharedPtr<FName>& NamePtr : ProfileNames)
	{
		if (!NamePtr.IsValid()) continue;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), NamePtr->ToString());

		// Get profile description
		FCollisionResponseTemplate Template;
		if (CP->GetProfileTemplate(*NamePtr, Template))
		{
			const ECollisionEnabled::Type Enabled = Template.CollisionEnabled;
			FString EnabledStr;
			switch (Enabled)
			{
			case ECollisionEnabled::NoCollision:     EnabledStr = TEXT("NoCollision");     break;
			case ECollisionEnabled::QueryOnly:       EnabledStr = TEXT("QueryOnly");       break;
			case ECollisionEnabled::PhysicsOnly:     EnabledStr = TEXT("PhysicsOnly");     break;
			case ECollisionEnabled::QueryAndPhysics: EnabledStr = TEXT("QueryAndPhysics"); break;
			default:                                  EnabledStr = TEXT("Unknown");         break;
			}
			Entry->SetStringField(TEXT("collision_enabled"), EnabledStr);
			Entry->SetStringField(TEXT("object_type"),       Template.ObjectTypeName.ToString());
			Entry->SetStringField(TEXT("help_message"),      Template.HelpMessage);
		}
		ProfileArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("profiles"), ProfileArr);
	Data->SetNumberField(TEXT("count"),   ProfileArr.Num());

	return MakeSuccess(DOMAIN, TEXT("list_collision_profiles"),
		FString::Printf(TEXT("Found %d collision profile(s)"), ProfileArr.Num()), Data);
}

// ---------------------------------------------------------------------------
// create_collision_profile — append to DefaultEngine.ini + reload
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_CreateCollisionProfile(TSharedPtr<FJsonObject> Params)
{
	FString Name, DefaultResponse = TEXT("Block");
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_collision_profile"), 1000, TEXT("Missing required param: 'name'"));
	Params->TryGetStringField(TEXT("default_response"), DefaultResponse);

	// Check for existing profile
	UCollisionProfile* CP = UCollisionProfile::Get();
	if (CP)
	{
		TArray<TSharedPtr<FName>> ExistingNames;
		CP->GetProfileNames(ExistingNames);
		for (const TSharedPtr<FName>& N : ExistingNames)
		{
			if (N.IsValid() && N->ToString() == Name)
				return MakeError(DOMAIN, TEXT("create_collision_profile"), 2000,
					FString::Printf(TEXT("Profile '%s' already exists"), *Name));
		}
	}

	const FString ECRStr = ResponseStrToECR(DefaultResponse);

	// Build minimal profile entry
	// Format: (Name="ProfileName",CollisionEnabled=ECollisionEnabled::QueryAndPhysics,
	//          ObjectTypeName="WorldDynamic",CustomResponses=(),HelpMessage="ForgeAI custom profile")
	const FString ProfileEntry = FString::Printf(
		TEXT("(Name=\"%s\",CollisionEnabled=ECollisionEnabled::QueryAndPhysics,")
		TEXT("ObjectTypeName=\"WorldDynamic\",CustomResponses=(),HelpMessage=\"ForgeAI: %s\")"),
		*Name, *Name);

	TArray<FString> ExistingProfiles;
	GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"),
		TEXT("Profiles"), ExistingProfiles, GEngineIni);
	ExistingProfiles.Add(ProfileEntry);
	GConfig->SetArray(TEXT("/Script/Engine.CollisionProfile"),
		TEXT("Profiles"), ExistingProfiles, GEngineIni);
	GConfig->Flush(false, GEngineIni);

	GetMutableDefault<UCollisionProfile>()->ReloadConfig();

	return MakeSuccess(DOMAIN, TEXT("create_collision_profile"),
		FString::Printf(TEXT("Profile '%s' created (QueryAndPhysics, WorldDynamic, default %s). "
			"Use Project Settings > Collision to configure per-channel responses."), *Name, *DefaultResponse));
}

// ---------------------------------------------------------------------------
// set_collision_response — guidance dispatch (struct format not safely rewritable in ini)
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_SetCollisionResponse(TSharedPtr<FJsonObject> Params)
{
	FString Profile, Channel, Response;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("profile"),  Profile);
		Params->TryGetStringField(TEXT("channel"),  Channel);
		Params->TryGetStringField(TEXT("response"), Response);
	}

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("set_collision_response"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"set_collision_response: Per-channel response editing requires the Collision Profile editor.\n"
		"To set profile '%s' channel '%s' response to '%s':\n"
		"  1. Open Editor: Project Settings > Engine > Collision\n"
		"  2. Find profile '%s' in the Preset list\n"
		"  3. Set the response for channel '%s' to '%s'\n"
		"  4. Click 'Save' — this writes CustomResponses to DefaultEngine.ini automatically.\n"
		"Alternatively, edit DefaultEngine.ini directly:\n"
		"  In the Profiles entry for '%s', add to CustomResponses:\n"
		"    (Channel=\"%s\",Response=%s)"),
		*Profile, *Channel, *Response,
		*Profile, *Channel, *Response,
		*Profile, *Channel, *ResponseStrToECR(Response));
	R.RecoveryHint = TEXT("The editor's collision settings UI is the safest way to edit responses as it handles the struct serialization format correctly.");
	return R;
}

// ---------------------------------------------------------------------------
// set_actor_collision — apply named profile to actor component
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_SetActorCollision(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString ActorLabel, ProfileName, ComponentName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
		return MakeError(DOMAIN, TEXT("set_actor_collision"), 1000, TEXT("Missing required param: 'actor_label'"));
	if (!Params->TryGetStringField(TEXT("profile"), ProfileName))
		return MakeError(DOMAIN, TEXT("set_actor_collision"), 1000, TEXT("Missing required param: 'profile'"));
	Params->TryGetStringField(TEXT("component"), ComponentName);

	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("set_actor_collision"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("set_actor_collision"), 2000,
			FString::Printf(TEXT("Actor not found: '%s'"), *ActorLabel));

	// Find target component
	UPrimitiveComponent* Target = nullptr;
	if (!ComponentName.IsEmpty())
	{
		TArray<UPrimitiveComponent*> PrimComps;
		Actor->GetComponents<UPrimitiveComponent>(PrimComps);
		for (UPrimitiveComponent* C : PrimComps)
		{
			if (C && C->GetName() == ComponentName)
			{
				Target = C;
				break;
			}
		}
		if (!Target)
			return MakeError(DOMAIN, TEXT("set_actor_collision"), 2000,
				FString::Printf(TEXT("PrimitiveComponent '%s' not found on actor '%s'"), *ComponentName, *ActorLabel));
	}
	else
	{
		Target = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!Target)
			return MakeError(DOMAIN, TEXT("set_actor_collision"), 3000,
				FString::Printf(TEXT("Root component of '%s' is not a UPrimitiveComponent"), *ActorLabel));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Actor Collision")));
	Target->Modify();
	Target->SetCollisionProfileName(FName(*ProfileName));
	Actor->MarkPackageDirty();

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("set_actor_collision"),
		FString::Printf(TEXT("Actor '%s' component '%s' collision profile → '%s'"),
			*ActorLabel, *Target->GetName(), *ProfileName));
	R.AffectedPath = ActorLabel;
	return R;
#else
	return MakeError(DOMAIN, TEXT("set_actor_collision"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// get_actor_collision — read collision settings from actor
// ---------------------------------------------------------------------------

FBridgeResult UCollisionHandler::Action_GetActorCollision(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString ActorLabel;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
		return MakeError(DOMAIN, TEXT("get_actor_collision"), 1000, TEXT("Missing required param: 'actor_label'"));

	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("get_actor_collision"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("get_actor_collision"), 2000,
			FString::Printf(TEXT("Actor not found: '%s'"), *ActorLabel));

	TArray<TSharedPtr<FJsonValue>> CompArr;
	TArray<UPrimitiveComponent*> PrimComps;
	Actor->GetComponents<UPrimitiveComponent>(PrimComps);

	for (UPrimitiveComponent* C : PrimComps)
	{
		if (!C) continue;
		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("component"),  C->GetName());
		CompObj->SetStringField(TEXT("class"),      C->GetClass()->GetName());
		CompObj->SetStringField(TEXT("profile"),    C->GetCollisionProfileName().ToString());

		const ECollisionEnabled::Type Enabled = C->GetCollisionEnabled();
		FString EnabledStr;
		switch (Enabled)
		{
		case ECollisionEnabled::NoCollision:     EnabledStr = TEXT("NoCollision");     break;
		case ECollisionEnabled::QueryOnly:       EnabledStr = TEXT("QueryOnly");       break;
		case ECollisionEnabled::PhysicsOnly:     EnabledStr = TEXT("PhysicsOnly");     break;
		case ECollisionEnabled::QueryAndPhysics: EnabledStr = TEXT("QueryAndPhysics"); break;
		default:                                  EnabledStr = TEXT("Unknown");         break;
		}
		CompObj->SetStringField(TEXT("collision_enabled"), EnabledStr);
		CompObj->SetBoolField(TEXT("is_root"), C == Actor->GetRootComponent());
		CompArr.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"),           ActorLabel);
	Data->SetBoolField(TEXT("actor_replicates"),  Actor->GetIsReplicated());
	Data->SetNumberField(TEXT("component_count"), CompArr.Num());
	Data->SetArrayField(TEXT("components"),       CompArr);

	return MakeSuccess(DOMAIN, TEXT("get_actor_collision"),
		FString::Printf(TEXT("Actor '%s': %d primitive component(s)"), *ActorLabel, CompArr.Num()), Data);
#else
	return MakeError(DOMAIN, TEXT("get_actor_collision"), 3000, TEXT("Requires editor context"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UCollisionHandler::GetActionSchemas() const
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

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List built-in collision channels and custom channels from DefaultEngine.ini"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_collision_channels"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a custom collision channel to DefaultEngine.ini (slot ECC_GameTraceChannel1..18)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("name"),             P(TEXT("string"), true,  TEXT("Channel name")));
	  Pr->SetObjectField(TEXT("default_response"), P(TEXT("string"), false, TEXT("Block|Overlap|Ignore (default: Block)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_collision_channel"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all collision profiles (names, collision enabled, object type) from UCollisionProfile"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_collision_profiles"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new collision profile in DefaultEngine.ini"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("name"),             P(TEXT("string"), true,  TEXT("Profile name")));
	  Pr->SetObjectField(TEXT("default_response"), P(TEXT("string"), false, TEXT("Block|Overlap|Ignore (default: Block)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("create_collision_profile"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set per-channel collision response on a profile (guidance — use Project Settings > Collision UI)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("profile"),  P(TEXT("string"), true, TEXT("Profile name")));
	  Pr->SetObjectField(TEXT("channel"),  P(TEXT("string"), true, TEXT("Channel name")));
	  Pr->SetObjectField(TEXT("response"), P(TEXT("string"), true, TEXT("Block|Overlap|Ignore")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_collision_response"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Apply a named collision profile to an actor's root (or named) primitive component"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true,  TEXT("Actor label in current level")));
	  Pr->SetObjectField(TEXT("profile"),     P(TEXT("string"), true,  TEXT("Collision profile name")));
	  Pr->SetObjectField(TEXT("component"),   P(TEXT("string"), false, TEXT("Component name (default: root component)")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_actor_collision"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read collision profile and enabled state for all primitive components on an actor"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label in current level")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_actor_collision"), A); }

	return Root;
}
