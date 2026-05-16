#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeNetworkCapture.generated.h"

/**
 * v1.13 — Network & Replication Audit
 *
 * Static analysis of all replicated Blueprint classes in the project plus,
 * when PIE is active, a live snapshot of the NetDriver connection state.
 *
 * Output: {ProjectRoot}/Forge/ue-context/network/audit.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.network_capture.export_network_audit()
 *
 * Exported JSON fields:
 *   generated, level_name, pie_active,
 *   replicated_actors[]{         (editor world — placed instances)
 *     actor_name, class_name,
 *     replicates, always_relevant,
 *     net_update_frequency, min_net_update_frequency, net_cull_distance_sq,
 *     has_movement_component, static_mesh_only
 *   },
 *   rpc_catalog[]{              (static analysis — all project Blueprint classes)
 *     class_path, function_name,
 *     is_server, is_client, is_multicast, is_reliable, param_count
 *   },
 *   replicated_properties[]{   (static analysis — all project Blueprint classes)
 *     class_path, property_name, property_type,
 *     has_rep_notify, rep_notify_func,
 *     repnotify_always, lifetime_condition
 *   },
 *   net_driver{                (PIE only — null object if not in PIE)
 *     role, connection_count, max_client_rate, net_driver_name
 *   },
 *   audit{ total_issues, issues[]{issue_type, severity, detail} }
 *
 * Audit rules (3):
 *   CHATTY_RPC        — Warning: Reliable NetMulticast RPC detected. Reliable
 *                       broadcasts fill the reliable channel; if called from Tick
 *                       or a hot path they cause connection drops at scale.
 *                       Signal: FUNC_NetMulticast + FUNC_NetReliable.
 *
 *   BROAD_RELEVANCY   — Info: Actor placed in the level has bAlwaysRelevant=true
 *                       but no movement component and only static mesh geometry.
 *                       Static decorative actors do not need always-on relevancy;
 *                       they add to every connected client's replication load.
 *
 *   STALE_REPLICATION — Warning: A replicated property is configured with
 *                       REPNOTIFY_Always. Its OnRep fires every replication tick
 *                       even when the value has not changed, wasting client CPU.
 *                       Switch to REPNOTIFY_OnChanged unless always-fire is needed.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeNetworkCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Run the full network audit and write network/audit.json.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportNetworkAudit();

private:
	FString OutputDir;

	// Sub-sections — each appends issues to OutIssues
	TArray<TSharedPtr<FJsonValue>> BuildActorRelevancyArray(
		UWorld* World, TArray<TSharedPtr<FJsonValue>>& OutIssues);

	void BuildRPCAndPropertyArrays(
		TArray<TSharedPtr<FJsonValue>>& OutRPCs,
		TArray<TSharedPtr<FJsonValue>>& OutProps,
		TArray<TSharedPtr<FJsonValue>>& OutIssues);

	TSharedPtr<FJsonObject> BuildNetDriverObject(UWorld* PlayWorld);

	static TSharedPtr<FJsonObject> MakeIssue(
		const FString& IssueType,
		const FString& Severity,
		const FString& Detail);

	// READ-MERGE-WRITE index.json to add/update the "network" section
	void UpdateIndexFile();
};
