#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "WorldPartitionHandler.generated.h"

/**
 * WorldPartitionHandler — domain "world_partition"
 *
 * World Partition, Data Layers, HLOD, and streaming management.
 *
 * Actions:
 *   is_enabled           → returns WP status
 *   create_data_layer    → name, asset_path
 *   set_data_layer_state → data_layer, state (Unloaded|Loaded|Activated)
 *   list_data_layers     → returns all data layer instances
 *   set_streaming_source → actor_path, radius?, priority?
 *   generate_hlods       → layer?
 *   generate_hlod        → alias for generate_hlods
 *   get_cell_info          → position { x, y, z }
 *   read_worldgen_capture  → (no params) → reads worldgen/parameters.json from capture output
 *   set_actor_data_layer   → actor_path (string — actor label), data_layer (string — asset path),
 *                            assign (bool, default true — false removes the assignment)
 *   convert_to_world_partition → (Python-dispatched) — runs WorldPartitionConvertCommandlet
 *   set_hlod_settings      → hlod_layer_asset (string, content path), streaming_distance (float)
 *   get_streaming_cells    → location? ("X,Y,Z"), radius? (float, default 5000)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UWorldPartitionHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("world_partition"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("is_enabled"), TEXT("create_data_layer"), TEXT("set_data_layer_state"),
			TEXT("list_data_layers"), TEXT("set_streaming_source"), TEXT("generate_hlods"),
			TEXT("generate_hlod"), TEXT("get_cell_info"), TEXT("read_worldgen_capture"),
			TEXT("set_actor_data_layer"), TEXT("convert_to_world_partition"),
			TEXT("set_hlod_settings"), TEXT("get_streaming_cells"),
			TEXT("remove_data_layer")
		};
	}
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
