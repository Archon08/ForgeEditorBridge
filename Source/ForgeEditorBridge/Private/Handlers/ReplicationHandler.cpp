#include "Handlers/ReplicationHandler.h"
#include "ForgeAISubsystem.h"

// ---- Engine / Reflection ----------------------------------------------------
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Engine/NetDriver.h"

// ---- JSON -------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Console + Module (for Iris probing) -----------------------------------
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

// ---- Editor -----------------------------------------------------------------
#if WITH_EDITOR
#include "Editor.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("replication");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AActor* UReplicationHandler::FindActorByLabel(UWorld* World, const FString& Label)
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
	return UBridgeHandlerBase::GetSafeEditorWorld();
#else
	return nullptr;
#endif
}

static FString CollisionEnabledStr(ECollisionEnabled::Type Type)
{
	switch (Type)
	{
	case ECollisionEnabled::NoCollision:         return TEXT("NoCollision");
	case ECollisionEnabled::QueryOnly:           return TEXT("QueryOnly");
	case ECollisionEnabled::PhysicsOnly:         return TEXT("PhysicsOnly");
	case ECollisionEnabled::QueryAndPhysics:     return TEXT("QueryAndPhysics");
	default:                                     return TEXT("Unknown");
	}
}

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("get_replicated_properties")) return Action_GetReplicatedProperties(Params);
	if (Action == TEXT("get_rpcs"))                  return Action_GetRPCs(Params);
	if (Action == TEXT("set_net_update_frequency"))  return Action_SetNetUpdateFrequency(Params);
	if (Action == TEXT("set_relevancy"))             return Action_SetRelevancy(Params);
	if (Action == TEXT("set_replication_condition")) return Action_SetReplicationCondition(Params);
	if (Action == TEXT("get_replication_graph_info"))return Action_GetReplicationGraphInfo(Params);
	if (Action == TEXT("audit_replication"))         return Action_AuditReplication(Params);
	if (Action == TEXT("set_iris_filter"))           return Action_SetIrisFilter(Params);
	if (Action == TEXT("get_iris_stats"))            return Action_GetIrisStats(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: get_replicated_properties, get_rpcs, set_net_update_frequency, set_relevancy, set_replication_condition, get_replication_graph_info, audit_replication, set_iris_filter, get_iris_stats"), *Action));
}

// ---------------------------------------------------------------------------
// get_replicated_properties — reflection CPF_Net scan
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_GetReplicatedProperties(TSharedPtr<FJsonObject> Params)
{
	FString ClassPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_replicated_properties"), 1000,
			TEXT("Missing required param: 'class_path' (e.g. '/Script/MyGame.MyActor' or '/Game/BP/BP_Actor.BP_Actor_C')"));

	UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
	if (!Class)
	{
		// Try loading
		Class = LoadObject<UClass>(nullptr, *ClassPath);
	}
	if (!Class)
		return MakeError(DOMAIN, TEXT("get_replicated_properties"), 2000,
			FString::Printf(TEXT("Class not found: '%s'"), *ClassPath),
			TEXT("Use reflection/list_classes to find the correct class path"));

	TArray<TSharedPtr<FJsonValue>> PropArr;
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		if (!(Prop->PropertyFlags & CPF_Net)) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),           Prop->GetName());
		Entry->SetStringField(TEXT("type"),           Prop->GetCPPType());
		Entry->SetStringField(TEXT("owner_class"),    Prop->GetOwnerClass() ? Prop->GetOwnerClass()->GetName() : TEXT("?"));
		Entry->SetBoolField(TEXT("rep_notify"),       !!(Prop->PropertyFlags & CPF_RepNotify));
		Entry->SetStringField(TEXT("rep_notify_func"),
			(Prop->PropertyFlags & CPF_RepNotify) ? Prop->RepNotifyFunc.ToString() : TEXT(""));
		// CPF_NetRequest removed in UE 5.7
		Entry->SetBoolField(TEXT("is_net_request"),   false);
		PropArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class"),              ClassPath);
	Data->SetNumberField(TEXT("count"),              PropArr.Num());
	Data->SetArrayField(TEXT("replicated_properties"),PropArr);
	Data->SetStringField(TEXT("note"),               TEXT("Lifetime conditions (COND_OwnerOnly etc.) are set in GetLifetimeReplicatedProps() and cannot be read without source analysis."));

	return MakeSuccess(DOMAIN, TEXT("get_replicated_properties"),
		FString::Printf(TEXT("Found %d replicated propert%s on %s"),
			PropArr.Num(), PropArr.Num() == 1 ? TEXT("y") : TEXT("ies"), *ClassPath), Data);
}

// ---------------------------------------------------------------------------
// get_rpcs — reflection FUNC_Net scan
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_GetRPCs(TSharedPtr<FJsonObject> Params)
{
	FString ClassPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_rpcs"), 1000,
			TEXT("Missing required param: 'class_path'"));

	UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
	if (!Class) Class = LoadObject<UClass>(nullptr, *ClassPath);
	if (!Class)
		return MakeError(DOMAIN, TEXT("get_rpcs"), 2000,
			FString::Printf(TEXT("Class not found: '%s'"), *ClassPath));

	TArray<TSharedPtr<FJsonValue>> RPCArr;
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* Func = *It;
		if (!(Func->FunctionFlags & FUNC_Net)) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),         Func->GetName());
		Entry->SetStringField(TEXT("owner_class"),  Func->GetOuterUClass() ? Func->GetOuterUClass()->GetName() : TEXT("?"));
		Entry->SetBoolField(TEXT("server"),         !!(Func->FunctionFlags & FUNC_NetServer));
		Entry->SetBoolField(TEXT("client"),         !!(Func->FunctionFlags & FUNC_NetClient));
		Entry->SetBoolField(TEXT("multicast"),      !!(Func->FunctionFlags & FUNC_NetMulticast));
		Entry->SetBoolField(TEXT("reliable"),       !!(Func->FunctionFlags & FUNC_NetReliable));
		Entry->SetBoolField(TEXT("validates"),      !!(Func->FunctionFlags & FUNC_NetValidate));

		FString RPCType;
		if (Func->FunctionFlags & FUNC_NetServer)    RPCType = TEXT("Server");
		else if (Func->FunctionFlags & FUNC_NetClient)    RPCType = TEXT("Client");
		else if (Func->FunctionFlags & FUNC_NetMulticast) RPCType = TEXT("NetMulticast");
		Entry->SetStringField(TEXT("rpc_type"), RPCType);

		RPCArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class"), ClassPath);
	Data->SetNumberField(TEXT("count"), RPCArr.Num());
	Data->SetArrayField(TEXT("rpcs"),   RPCArr);

	return MakeSuccess(DOMAIN, TEXT("get_rpcs"),
		FString::Printf(TEXT("Found %d RPC(s) on %s"), RPCArr.Num(), *ClassPath), Data);
}

// ---------------------------------------------------------------------------
// set_net_update_frequency
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_SetNetUpdateFrequency(TSharedPtr<FJsonObject> Params)
{
	FString ActorLabel;
	double Frequency = 10.0;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
		return MakeError(DOMAIN, TEXT("set_net_update_frequency"), 1000, TEXT("Missing required param: 'actor_label'"));
	if (!Params->TryGetNumberField(TEXT("frequency"), Frequency))
		return MakeError(DOMAIN, TEXT("set_net_update_frequency"), 1000, TEXT("Missing required param: 'frequency' (Hz)"));

	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("set_net_update_frequency"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("set_net_update_frequency"), 2000,
			FString::Printf(TEXT("Actor not found: '%s'"), *ActorLabel));

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Net Update Frequency")));
	Actor->Modify();
	Actor->SetNetUpdateFrequency((float)Frequency);
	Actor->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("set_net_update_frequency"),
		FString::Printf(TEXT("Actor '%s' NetUpdateFrequency → %.1f Hz"), *ActorLabel, Frequency));
}

// ---------------------------------------------------------------------------
// set_relevancy
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_SetRelevancy(TSharedPtr<FJsonObject> Params)
{
	FString ActorLabel;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
		return MakeError(DOMAIN, TEXT("set_relevancy"), 1000, TEXT("Missing required param: 'actor_label'"));

	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("set_relevancy"), 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor)
		return MakeError(DOMAIN, TEXT("set_relevancy"), 2000,
			FString::Printf(TEXT("Actor not found: '%s'"), *ActorLabel));

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Relevancy")));
	Actor->Modify();

	bool bChanged = false;
	bool bVal;
	double Dist;

	if (Params->TryGetBoolField(TEXT("always_relevant"), bVal))
	{
		Actor->bAlwaysRelevant = bVal;
		bChanged = true;
	}
	if (Params->TryGetBoolField(TEXT("owner_only"), bVal))
	{
		Actor->bOnlyRelevantToOwner = bVal;
		bChanged = true;
	}
	if (Params->TryGetNumberField(TEXT("cull_distance"), Dist))
	{
		Actor->SetNetCullDistanceSquared((float)(Dist * Dist));
		bChanged = true;
	}

	if (!bChanged)
		return MakeError(DOMAIN, TEXT("set_relevancy"), 1000,
			TEXT("No relevancy params provided. Use: always_relevant (bool), owner_only (bool), cull_distance (number, cm)"));

	Actor->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("set_relevancy"),
		FString::Printf(TEXT("Actor '%s' relevancy updated (always_relevant=%s, owner_only=%s, cull_dist=%.0f)"),
			*ActorLabel,
			Actor->bAlwaysRelevant ? TEXT("true") : TEXT("false"),
			Actor->bOnlyRelevantToOwner ? TEXT("true") : TEXT("false"),
			FMath::Sqrt(Actor->GetNetCullDistanceSquared())));
}

// ---------------------------------------------------------------------------
// set_replication_condition — Python dispatch (compile-time only)
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_SetReplicationCondition(TSharedPtr<FJsonObject> Params)
{
	FString PropName, Condition, ClassPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("property_name"), PropName);
		Params->TryGetStringField(TEXT("condition"),     Condition);
		Params->TryGetStringField(TEXT("class_path"),    ClassPath);
	}

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("set_replication_condition"), TEXT(""));
	R.Message = FString::Printf(TEXT(
		"set_replication_condition: Lifetime conditions are compile-time macros in C++ source.\n"
		"To set a condition on property '%s' of class '%s' to '%s':\n"
		"  In GetLifetimeReplicatedProps():\n"
		"    DOREPLIFETIME_CONDITION(UMyClass, %s, %s);\n"
		"Available conditions: COND_None, COND_OwnerOnly, COND_SkipOwner, COND_SimulatedOnly,\n"
		"  COND_AutonomousOnly, COND_SimulatedOrPhysics, COND_InitialOnly, COND_ReplayOrOwner,\n"
		"  COND_Custom, COND_Never, COND_NetGroup"),
		PropName.IsEmpty() ? TEXT("<property>") : *PropName,
		ClassPath.IsEmpty() ? TEXT("<class>") : *ClassPath,
		Condition.IsEmpty() ? TEXT("<condition>") : *Condition,
		PropName.IsEmpty() ? TEXT("MyProperty") : *PropName,
		Condition.IsEmpty() ? TEXT("COND_OwnerOnly") : *Condition);
	R.RecoveryHint = TEXT("Use cpp/edit_file to modify the source, then trigger Live Coding to rebuild.");
	return R;
}

// ---------------------------------------------------------------------------
// get_replication_graph_info — net driver + graph info
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_GetReplicationGraphInfo(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("get_replication_graph_info"), 3000, TEXT("No editor world available"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	UNetDriver* NetDriver = World->GetNetDriver();
	if (NetDriver)
	{
		Data->SetStringField(TEXT("net_driver_class"),        NetDriver->GetClass()->GetName());
		Data->SetStringField(TEXT("net_driver_name"),         NetDriver->NetDriverName.ToString());
		Data->SetNumberField(TEXT("max_client_rate"),         NetDriver->MaxClientRate);
		Data->SetNumberField(TEXT("max_internet_client_rate"),NetDriver->MaxInternetClientRate);
		Data->SetNumberField(TEXT("server_travel_pause"),     NetDriver->ServerTravelPause);
		Data->SetBoolField(TEXT("has_replication_graph"),
			NetDriver->GetClass()->GetName().Contains(TEXT("ReplicationGraph")));
	}
	else
	{
		Data->SetStringField(TEXT("net_driver"), TEXT("null (no net driver in editor — run PIE with Listen Server to inspect)"));
	}

	// Check if UReplicationGraph is loaded
	UClass* RepGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/ReplicationGraph.ReplicationGraph"));
	Data->SetBoolField(TEXT("replication_graph_plugin_loaded"), RepGraphClass != nullptr);
	Data->SetStringField(TEXT("note"),
		TEXT("Full ReplicationGraph inspection requires PIE with Listen Server or Dedicated Server. "
		     "ReplicationGraph plugin must be enabled in your .uproject for graph-based replication."));

	return MakeSuccess(DOMAIN, TEXT("get_replication_graph_info"),
		NetDriver ? TEXT("Net driver info retrieved") : TEXT("No net driver (editor mode)"), Data);
}

// ---------------------------------------------------------------------------
// audit_replication — scan world actors for issues
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_AuditReplication(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World) return MakeError(DOMAIN, TEXT("audit_replication"), 3000, TEXT("No editor world available"));

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 ReplicatedActors = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !Actor->GetIsReplicated()) continue;
		ReplicatedActors++;

		const FString Label = Actor->GetActorLabel();

		// Issue: very high net update frequency
		if (Actor->GetNetUpdateFrequency() > 60.f)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Label);
			Issue->SetStringField(TEXT("issue"), TEXT("HIGH_UPDATE_FREQUENCY"));
			Issue->SetStringField(TEXT("detail"), FString::Printf(
				TEXT("NetUpdateFrequency=%.0f Hz (>60). Consider reducing for non-critical state."),
				Actor->GetNetUpdateFrequency()));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Issue: always_relevant + finite cull distance (contradictory)
		if (Actor->bAlwaysRelevant && Actor->GetNetCullDistanceSquared() > 0.f)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Label);
			Issue->SetStringField(TEXT("issue"), TEXT("RELEVANCY_CONTRADICTION"));
			Issue->SetStringField(TEXT("detail"),
				TEXT("bAlwaysRelevant=true but NetCullDistanceSquared>0. Cull distance is ignored when always relevant."));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Issue: many replicated properties (reflection count)
		int32 NetPropCount = 0;
		for (TFieldIterator<FProperty> PropIt(Actor->GetClass(), EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			if ((*PropIt)->PropertyFlags & CPF_Net) NetPropCount++;
		}
		if (NetPropCount > 20)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Label);
			Issue->SetStringField(TEXT("issue"), TEXT("HIGH_PROPERTY_COUNT"));
			Issue->SetStringField(TEXT("detail"), FString::Printf(
				TEXT("%d replicated properties — review for grouping via COND_Custom or sub-object replication."),
				NetPropCount));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Issue: bAlwaysRelevant with no replication settings (potential accident)
		if (!Actor->GetIsReplicated() && Actor->bAlwaysRelevant)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Label);
			Issue->SetStringField(TEXT("issue"), TEXT("ALWAYS_RELEVANT_NOT_REPLICATED"));
			Issue->SetStringField(TEXT("detail"),
				TEXT("bAlwaysRelevant=true but bReplicates=false. AlwaysRelevant has no effect without replication enabled."));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("replicated_actors_scanned"), ReplicatedActors);
	Data->SetNumberField(TEXT("issues_found"),              Issues.Num());
	Data->SetArrayField(TEXT("issues"),                    Issues);

	return MakeSuccess(DOMAIN, TEXT("audit_replication"),
		FString::Printf(TEXT("Audited %d replicated actor(s): %d issue(s) found"),
			ReplicatedActors, Issues.Num()), Data);
}

// ---------------------------------------------------------------------------
// set_iris_filter — best-effort Iris filter config
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_SetIrisFilter(TSharedPtr<FJsonObject> Params)
{
	FString FilterName;
	bool bEnabled = false;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("filter_name"), FilterName) || FilterName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_iris_filter"), 1000, TEXT("Missing required param: 'filter_name'"));
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		return MakeError(DOMAIN, TEXT("set_iris_filter"), 1000, TEXT("Missing required param: 'enabled' (bool)"));

	if (!FModuleManager::Get().ModuleExists(TEXT("IrisCore")))
	{
		return MakeError(DOMAIN, TEXT("set_iris_filter"), 3003,
			TEXT("set_iris_filter requires Iris networking (IrisCore module not loaded)"),
			TEXT("Enable the Iris plugin/module in your .uproject or Build.cs."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("filter_name"), FilterName);
	Data->SetBoolField(TEXT("enabled"), bEnabled);
	Data->SetStringField(TEXT("note"),
		TEXT("Full wiring requires IrisFilteringManager — see UE 5.7 Iris docs for FDataStreamDefinitions."));

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("set_iris_filter"),
		FString::Printf(TEXT("set_iris_filter: Iris filter '%s' configuration noted (enabled=%s). Full wiring requires IrisFilteringManager — see UE 5.7 Iris docs for FDataStreamDefinitions."),
			*FilterName, bEnabled ? TEXT("true") : TEXT("false")),
		Data);
	return R;
}

// ---------------------------------------------------------------------------
// get_iris_stats — read Iris CVars via console manager
// ---------------------------------------------------------------------------

FBridgeResult UReplicationHandler::Action_GetIrisStats(TSharedPtr<FJsonObject> Params)
{
	const bool bHasIrisCore = FModuleManager::Get().ModuleExists(TEXT("IrisCore"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("iris_module_loaded"), bHasIrisCore);

	bool bIrisEnabled = false;
	auto ReadCVar = [&](const TCHAR* Name, const TCHAR* JsonKey, bool bIsBoolish) -> bool
	{
		IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!CV) return false;
		if (bIsBoolish)
		{
			const int32 V = CV->GetInt();
			Data->SetBoolField(JsonKey, V != 0);
			return V != 0;
		}
		else
		{
			Data->SetNumberField(JsonKey, CV->GetFloat());
			return true;
		}
	};

	if (ReadCVar(TEXT("r.Iris.Enable"), TEXT("iris_enabled"), /*bool*/ true))  { bIrisEnabled = true; }
	else { Data->SetBoolField(TEXT("iris_enabled"), false); }

	// Additional probes (optional CVars — absent on some builds)
	if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("net.IrisEnabled")))
	{
		Data->SetBoolField(TEXT("net_iris_enabled"), CV->GetInt() != 0);
	}
	ReadCVar(TEXT("net.ReplicateActorThrottle"), TEXT("net_replicate_actor_throttle"), /*bool*/ false);

	Data->SetStringField(TEXT("note"),
		TEXT("Full Iris stats available via Iris profiling API"));

	FBridgeResult R = MakeSuccess(DOMAIN, TEXT("get_iris_stats"),
		FString::Printf(TEXT("get_iris_stats: IrisCore %s, Iris %s"),
			bHasIrisCore ? TEXT("loaded") : TEXT("not loaded"),
			bIrisEnabled ? TEXT("enabled") : TEXT("disabled")),
		Data);
	return R;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UReplicationHandler::GetActionSchemas() const
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

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all CPF_Net replicated properties on a class (reflection)"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("class_path"), P(TEXT("string"), true, TEXT("Full class path e.g. /Script/MyGame.MyActor"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_replicated_properties"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all FUNC_Net RPC functions on a class (reflection)"));
	  auto Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("class_path"), P(TEXT("string"), true, TEXT("Full class path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_rpcs"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set NetUpdateFrequency on an actor in the current level"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true,  TEXT("Actor label")));
	  Pr->SetObjectField(TEXT("frequency"),   P(TEXT("number"), true,  TEXT("Update frequency in Hz")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_net_update_frequency"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set relevancy flags and cull distance on an actor"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("actor_label"),    P(TEXT("string"), true,  TEXT("Actor label")));
	  Pr->SetObjectField(TEXT("always_relevant"),P(TEXT("bool"),   false, TEXT("Set bAlwaysRelevant")));
	  Pr->SetObjectField(TEXT("owner_only"),     P(TEXT("bool"),   false, TEXT("Set bOnlyRelevantToOwner")));
	  Pr->SetObjectField(TEXT("cull_distance"),  P(TEXT("number"), false, TEXT("NetCullDistance in cm")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_relevancy"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a property's DOREPLIFETIME condition (Python dispatch — compile-time C++ macro)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("class_path"),    P(TEXT("string"), true,  TEXT("Class path")));
	  Pr->SetObjectField(TEXT("property_name"), P(TEXT("string"), true,  TEXT("Property name")));
	  Pr->SetObjectField(TEXT("condition"),     P(TEXT("string"), true,  TEXT("COND_OwnerOnly|COND_None|COND_SimulatedOnly|etc.")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_replication_condition"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get net driver info and ReplicationGraph plugin status"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_replication_graph_info"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Audit all replicated actors in current level for anti-patterns"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("audit_replication"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure an Iris replication filter (best-effort; requires IrisCore module)"));
	  auto Pr = MakeShared<FJsonObject>();
	  Pr->SetObjectField(TEXT("filter_name"), P(TEXT("string"), true,  TEXT("Iris filter name")));
	  Pr->SetObjectField(TEXT("enabled"),     P(TEXT("bool"),   true,  TEXT("Enable or disable the filter")));
	  Pr->SetObjectField(TEXT("params"),      P(TEXT("object"), false, TEXT("Optional filter-specific parameters")));
	  A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_iris_filter"), A); }

	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get Iris replication statistics via console CVars (r.Iris.Enable, etc.)"));
	  auto Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_iris_stats"), A); }

	return Root;
}
