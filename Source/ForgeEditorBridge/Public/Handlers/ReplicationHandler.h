#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ReplicationHandler.generated.h"

/**
 * ReplicationHandler — domain "replication" (v2.0.0 / UE 5.7)
 *
 * Inspect and configure network replication settings.
 *
 * Actions:
 *   get_replicated_properties → class_path (string) — list CPF_Net properties + type info.
 *
 *   get_rpcs                  → class_path (string) — list FUNC_Net functions with server/client/multicast/reliable flags.
 *
 *   set_net_update_frequency  → actor_label (string), frequency (number, Hz)
 *
 *   set_relevancy             → actor_label (string), always_relevant (bool, opt),
 *                               owner_only (bool, opt), cull_distance (number, opt, cm)
 *
 *   set_replication_condition → Python dispatch — DOREPLIFETIME conditions are compile-time.
 *
 *   get_replication_graph_info → (no params) — returns net driver info + replication graph state.
 *
 *   audit_replication          → (no params) — scans world actors for replication anti-patterns.
 *
 *   set_iris_filter            → filter_name (string), enabled (bool), params? (object)
 *                                — best-effort Iris filter configuration; requires IrisCore module.
 *
 *   get_iris_stats             → (no params) — reads Iris CVars (r.Iris.Enable, etc.).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UReplicationHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("replication"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("get_replicated_properties"),
			TEXT("get_rpcs"),
			TEXT("set_net_update_frequency"),
			TEXT("set_relevancy"),
			TEXT("set_replication_condition"),
			TEXT("get_replication_graph_info"),
			TEXT("audit_replication"),
			TEXT("set_iris_filter"),
			TEXT("get_iris_stats"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetReplicatedProperties (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetRPCs                 (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetNetUpdateFrequency   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetRelevancy            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetReplicationCondition (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetReplicationGraphInfo (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AuditReplication        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetIrisFilter           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetIrisStats            (TSharedPtr<FJsonObject> Params);

	static AActor* FindActorByLabel(UWorld* World, const FString& Label);
};
