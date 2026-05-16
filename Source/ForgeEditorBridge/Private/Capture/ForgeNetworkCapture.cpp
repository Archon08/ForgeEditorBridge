#include "Capture/ForgeNetworkCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Actor / World ---
#include "GameFramework/Actor.h"
#include "GameFramework/MovementComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"

// --- Networking ---
#include "Engine/NetDriver.h"
#include "Net/UnrealNetwork.h"     // FLifetimeProperty, ELifetimeRepNotifyCondition

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/TopLevelAssetPath.h"

// --- Blueprint ---
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"

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
	FString LifetimeCondToString(ELifetimeCondition Cond)
	{
		switch (Cond)
		{
		case COND_None:                  return TEXT("COND_None");
		case COND_InitialOnly:           return TEXT("COND_InitialOnly");
		case COND_OwnerOnly:             return TEXT("COND_OwnerOnly");
		case COND_SkipOwner:             return TEXT("COND_SkipOwner");
		case COND_SimulatedOnly:         return TEXT("COND_SimulatedOnly");
		case COND_AutonomousOnly:        return TEXT("COND_AutonomousOnly");
		case COND_SimulatedOrPhysics:    return TEXT("COND_SimulatedOrPhysics");
		case COND_InitialOrOwner:        return TEXT("COND_InitialOrOwner");
		case COND_Custom:                return TEXT("COND_Custom");
		case COND_ReplayOrOwner:         return TEXT("COND_ReplayOrOwner");
		case COND_ReplayOnly:            return TEXT("COND_ReplayOnly");
		case COND_SimulatedOnlyNoReplay: return TEXT("COND_SimulatedOnlyNoReplay");
		case COND_SimulatedOrPhysicsNoReplay: return TEXT("COND_SimulatedOrPhysicsNoReplay");
		case COND_SkipReplay:            return TEXT("COND_SkipReplay");
		case COND_Never:                 return TEXT("COND_Never");
		default:                         return FString::Printf(TEXT("COND_%d"), (int32)Cond);
		}
	}
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeNetworkCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("network")));
	UE_LOG(LogTemp, Log, TEXT("ForgeNetwork: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportNetworkAudit
// ---------------------------------------------------------------------------

bool UForgeNetworkCapture::ExportNetworkAudit()
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeNetwork: GEditor is null"));
		return false;
	}

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
	Root->SetStringField(TEXT("level_name"),
		EditorWorld ? EditorWorld->GetName() : TEXT("(none)"));

	const bool bPIEActive = (GEditor->PlayWorld != nullptr);
	Root->SetBoolField(TEXT("pie_active"), bPIEActive);

	TArray<TSharedPtr<FJsonValue>> Issues;

	// -------------------------------------------------------------------------
	// Section 1: Actor relevancy — placed instances in the editor world
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ActorsArr =
		BuildActorRelevancyArray(EditorWorld, Issues);
	Root->SetArrayField(TEXT("replicated_actors"), ActorsArr);

	// -------------------------------------------------------------------------
	// Section 2 + 3: RPC catalog + replicated property list
	// Single pass over project blueprints to avoid loading assets twice
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> RPCArr;
	TArray<TSharedPtr<FJsonValue>> PropsArr;
	BuildRPCAndPropertyArrays(RPCArr, PropsArr, Issues);
	Root->SetArrayField(TEXT("rpc_catalog"),            RPCArr);
	Root->SetArrayField(TEXT("replicated_properties"),  PropsArr);

	// -------------------------------------------------------------------------
	// Section 4: NetDriver snapshot (PIE only)
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> NetDriverObj = BuildNetDriverObject(GEditor->PlayWorld);
	if (NetDriverObj.IsValid())
		Root->SetObjectField(TEXT("net_driver"), NetDriverObj);
	else
		Root->SetBoolField(TEXT("net_driver"), false); // explicit null sentinel

	// -------------------------------------------------------------------------
	// Audit summary
	// -------------------------------------------------------------------------
	{
		TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
		AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
		AuditObj->SetArrayField (TEXT("issues"),       Issues);
		Root->SetObjectField(TEXT("audit"), AuditObj);
	}

	bool bOK = FForgeContextWriter::WriteJSON(
		OutputDir / TEXT("network"), TEXT("audit"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log,
			TEXT("ForgeNetwork: Exported -> network/audit.json "
			     "(%d actor(s), %d RPC(s), %d prop(s), %d issue(s))"),
			ActorsArr.Num(), RPCArr.Num(), PropsArr.Num(), Issues.Num());
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// BuildActorRelevancyArray
// Iterates all placed actors in the editor world and captures their network
// configuration.  Emits BROAD_RELEVANCY issues for always-relevant static actors.
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UForgeNetworkCapture::BuildActorRelevancyArray(
	UWorld* World, TArray<TSharedPtr<FJsonValue>>& OutIssues)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	if (!World) return Result;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		// Skip non-replicated actors — they have no network footprint
		// bReplicates is protected in UE5.7 — use accessor
		if (!Actor->GetIsReplicated()) continue;

		const bool  bAlwaysRelevant   = Actor->bAlwaysRelevant;
		const float NetUpdateFreq     = Actor->GetNetUpdateFrequency();
		const float MinNetUpdateFreq  = Actor->GetMinNetUpdateFrequency();
		const float NetCullDistSq     = Actor->GetNetCullDistanceSquared();

		bool bHasMovement = (Actor->FindComponentByClass<UMovementComponent>() != nullptr);

		TArray<UStaticMeshComponent*> StaticMeshComps;
		Actor->GetComponents<UStaticMeshComponent>(StaticMeshComps);
		const bool bStaticMeshOnly = !StaticMeshComps.IsEmpty() && !bHasMovement;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_name"),           Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("class_name"),           Actor->GetClass()->GetName());
		Obj->SetBoolField  (TEXT("replicates"),           true);
		Obj->SetBoolField  (TEXT("always_relevant"),      bAlwaysRelevant);
		Obj->SetNumberField(TEXT("net_update_frequency"), NetUpdateFreq);
		Obj->SetNumberField(TEXT("min_net_update_frequency"), MinNetUpdateFreq);
		Obj->SetNumberField(TEXT("net_cull_distance_sq"), NetCullDistSq);
		Obj->SetBoolField  (TEXT("has_movement_component"), bHasMovement);
		Obj->SetBoolField  (TEXT("static_mesh_only"),     bStaticMeshOnly);
		Result.Add(MakeShared<FJsonValueObject>(Obj));

		// AUDIT: BROAD_RELEVANCY
		// Always-relevant actors that are purely decorative waste replication budget
		// by broadcasting their existence to every connected client unconditionally.
		if (bAlwaysRelevant && bStaticMeshOnly)
		{
			OutIssues.Add(MakeShared<FJsonValueObject>(MakeIssue(
				TEXT("BROAD_RELEVANCY"),
				TEXT("info"),
				FString::Printf(
					TEXT("Actor '%s' (%s) has bAlwaysRelevant=true but has only static "
					     "mesh geometry and no movement component. Static decorative actors "
					     "do not need always-on network relevancy — they push replication "
					     "load to every client for no gameplay benefit. "
					     "Remove bAlwaysRelevant or use a non-replicated actor."),
					*Actor->GetActorNameOrLabel(),
					*Actor->GetClass()->GetName()))));
		}
	}
	return Result;
}

// ---------------------------------------------------------------------------
// BuildRPCAndPropertyArrays
// Single pass over all /Game Blueprint assets.  Populates the RPC catalog and
// replicated property list, and emits CHATTY_RPC and STALE_REPLICATION issues.
// ---------------------------------------------------------------------------

void UForgeNetworkCapture::BuildRPCAndPropertyArrays(
	TArray<TSharedPtr<FJsonValue>>& OutRPCs,
	TArray<TSharedPtr<FJsonValue>>& OutProps,
	TArray<TSharedPtr<FJsonValue>>& OutIssues)
{
	IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")));

	TArray<FAssetData> BPAssets;
	AR.GetAssets(Filter, BPAssets);

	for (const FAssetData& AD : BPAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
		if (!BP || !BP->GeneratedClass) continue;

		UClass* Class = BP->GeneratedClass;

		// Cheap early-out: does this class have ANY replicated content?
		bool bHasAnyNet = false;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			if ((*FuncIt)->HasAnyFunctionFlags(FUNC_Net)) { bHasAnyNet = true; break; }
		}
		if (!bHasAnyNet)
		{
			for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
			{
				if ((*PropIt)->HasAnyPropertyFlags(CPF_Net)) { bHasAnyNet = true; break; }
			}
		}
		if (!bHasAnyNet) continue;

		const FString ClassPath = AD.PackageName.ToString();

		// Actor CDO — used for NetUpdateFrequency in CHATTY_RPC heuristic
		AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject(false));

		// ---------------------------------------------------------------------
		// RPC catalog + CHATTY_RPC audit
		// ---------------------------------------------------------------------
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			if (!Func->HasAnyFunctionFlags(FUNC_Net)) continue;

			const bool bIsServer    = Func->HasAllFunctionFlags(FUNC_NetServer);
			const bool bIsClient    = Func->HasAllFunctionFlags(FUNC_NetClient);
			const bool bIsMulticast = Func->HasAllFunctionFlags(FUNC_NetMulticast);
			const bool bIsReliable  = Func->HasAllFunctionFlags(FUNC_NetReliable);

			// Count non-return parameters
			int32 ParamCount = 0;
			for (TFieldIterator<FProperty> PIt(Func); PIt && PIt->HasAnyPropertyFlags(CPF_Parm); ++PIt)
				if (!PIt->HasAnyPropertyFlags(CPF_ReturnParm)) ++ParamCount;

			TSharedRef<FJsonObject> RPCObj = MakeShared<FJsonObject>();
			RPCObj->SetStringField(TEXT("class_path"),    ClassPath);
			RPCObj->SetStringField(TEXT("function_name"), Func->GetName());
			RPCObj->SetBoolField  (TEXT("is_server"),     bIsServer);
			RPCObj->SetBoolField  (TEXT("is_client"),     bIsClient);
			RPCObj->SetBoolField  (TEXT("is_multicast"),  bIsMulticast);
			RPCObj->SetBoolField  (TEXT("is_reliable"),   bIsReliable);
			RPCObj->SetNumberField(TEXT("param_count"),   ParamCount);
			OutRPCs.Add(MakeShared<FJsonValueObject>(RPCObj));

			// AUDIT: CHATTY_RPC
			// Reliable NetMulticast fills the reliable delivery channel.  If the
			// RPC is called every frame or from a hot path the reliable buffer
			// saturates and the engine drops the connection.  Flag all reliable
			// multicasts so the developer can audit call sites.
			if (bIsMulticast && bIsReliable)
			{
				const float NetUpdateFreq = ActorCDO ? ActorCDO->GetNetUpdateFrequency() : 0.0f;
				OutIssues.Add(MakeShared<FJsonValueObject>(MakeIssue(
					TEXT("CHATTY_RPC"),
					TEXT("warning"),
					FString::Printf(
						TEXT("'%s::%s' is a Reliable NetMulticast RPC "
						     "(class NetUpdateFrequency=%.0f Hz). Reliable multicasts are "
						     "delivered to every connected client with guaranteed ordering. "
						     "If called more than ~10 times/second they exhaust the reliable "
						     "channel buffer and cause connection drops. "
						     "Use Unreliable for cosmetic/cosmetic-only events, or gate "
						     "calls behind a server-side rate limiter."),
						*Class->GetName(), *Func->GetName(), NetUpdateFreq))));
			}
		}

		// ---------------------------------------------------------------------
		// Replicated property list + STALE_REPLICATION audit
		// Build RepIndex -> FLifetimeProperty map by calling GetLifetimeReplicatedProps
		// on the CDO — this is the only way to detect REPNOTIFY_Always statically.
		// ---------------------------------------------------------------------
		TMap<uint16, FLifetimeProperty> RepIndexMap;
		if (UObject* CDO = Class->GetDefaultObject(false))
		{
			TArray<FLifetimeProperty> LifetimeProps;
			CDO->GetLifetimeReplicatedProps(LifetimeProps);
			for (const FLifetimeProperty& LP : LifetimeProps)
				RepIndexMap.Add(LP.RepIndex, LP);
		}

		for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Net)) continue;

			const bool  bHasRepNotify = Prop->HasAnyPropertyFlags(CPF_RepNotify);
			const FName RepNotifyFunc = Prop->RepNotifyFunc;

			bool               bRepNotifyAlways = false;
			ELifetimeCondition LifeCond         = COND_None;

			if (const FLifetimeProperty* LP = RepIndexMap.Find(Prop->RepIndex))
			{
				bRepNotifyAlways = (LP->RepNotifyCondition == REPNOTIFY_Always);
				LifeCond         = LP->Condition;
			}

			TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("class_path"),        ClassPath);
			PropObj->SetStringField(TEXT("property_name"),     Prop->GetName());
			PropObj->SetStringField(TEXT("property_type"),     Prop->GetCPPType());
			PropObj->SetBoolField  (TEXT("has_rep_notify"),    bHasRepNotify);
			PropObj->SetStringField(TEXT("rep_notify_func"),
				bHasRepNotify ? RepNotifyFunc.ToString() : TEXT("(none)"));
			PropObj->SetBoolField  (TEXT("repnotify_always"),  bRepNotifyAlways);
			PropObj->SetStringField(TEXT("lifetime_condition"), LifetimeCondToString(LifeCond));
			OutProps.Add(MakeShared<FJsonValueObject>(PropObj));

			// AUDIT: STALE_REPLICATION
			// REPNOTIFY_Always causes the OnRep to fire every replication tick
			// even when the server-side value has not changed.  This wastes CPU
			// on both sides and can cause cascading side effects in OnRep logic.
			if (bRepNotifyAlways)
			{
				OutIssues.Add(MakeShared<FJsonValueObject>(MakeIssue(
					TEXT("STALE_REPLICATION"),
					TEXT("warning"),
					FString::Printf(
						TEXT("'%s::%s' uses REPNOTIFY_Always — its OnRep function '%s' is "
						     "called every replication tick regardless of whether the value "
						     "changed. This fires redundant callbacks on all clients and can "
						     "cause unintended animation/UI flicker. "
						     "Use REPNOTIFY_OnChanged unless you specifically need to react "
						     "to replication of an identical value (e.g., re-triggering a "
						     "cosmetic effect on reconnect)."),
						*Class->GetName(), *Prop->GetName(), *RepNotifyFunc.ToString()))));
			}
		}
	}
}

// ---------------------------------------------------------------------------
// BuildNetDriverObject
// Captures live NetDriver state when PIE is active.
// Returns nullptr if no PIE world or no NetDriver is available.
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeNetworkCapture::BuildNetDriverObject(UWorld* PlayWorld)
{
	if (!PlayWorld) return nullptr;

	UNetDriver* NetDriver = PlayWorld->GetNetDriver();
	if (!NetDriver) return nullptr;

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

	// Determine role: if ServerConnection is valid this process is a client
	const bool bIsClient = (NetDriver->ServerConnection != nullptr);
	Obj->SetStringField(TEXT("role"), bIsClient ? TEXT("client") : TEXT("server"));

	// Connection count (from client side this is always 1 via ServerConnection)
	const int32 ConnCount = bIsClient
		? (NetDriver->ServerConnection ? 1 : 0)
		: NetDriver->ClientConnections.Num();
	Obj->SetNumberField(TEXT("connection_count"), ConnCount);

	// Bandwidth config
	Obj->SetNumberField(TEXT("max_client_rate"),          NetDriver->MaxClientRate);
	Obj->SetNumberField(TEXT("max_internet_client_rate"), NetDriver->MaxInternetClientRate);
	Obj->SetStringField(TEXT("net_driver_name"),          NetDriver->NetDriverName.ToString());

	// Client connections summary (server side only)
	if (!bIsClient)
	{
		TArray<TSharedPtr<FJsonValue>> ConnsArr;
		for (UNetConnection* Conn : NetDriver->ClientConnections)
		{
			if (!Conn) continue;
			TSharedRef<FJsonObject> ConnObj = MakeShared<FJsonObject>();
			ConnObj->SetStringField(TEXT("remote_address"),
				Conn->LowLevelGetRemoteAddress(/*bAppendPort=*/true));
			ConnObj->SetNumberField(TEXT("average_lag_ms"), Conn->AvgLag * 1000.0f);
			ConnsArr.Add(MakeShared<FJsonValueObject>(ConnObj));
		}
		Obj->SetArrayField(TEXT("client_connections"), ConnsArr);
	}
	else
	{
		// Client side: capture lag to server
		if (NetDriver->ServerConnection)
		{
			Obj->SetNumberField(TEXT("average_lag_ms"),
				NetDriver->ServerConnection->AvgLag * 1000.0f);
		}
	}

	return Obj;
}

// ---------------------------------------------------------------------------
// MakeIssue
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UForgeNetworkCapture::MakeIssue(
	const FString& IssueType, const FString& Severity, const FString& Detail)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("issue_type"), IssueType);
	Obj->SetStringField(TEXT("severity"),   Severity);
	Obj->SetStringField(TEXT("detail"),     Detail);
	return Obj;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeNetworkCapture::UpdateIndexFile()
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
	Section->SetStringField(TEXT("file"),         TEXT("network/audit.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("network"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
