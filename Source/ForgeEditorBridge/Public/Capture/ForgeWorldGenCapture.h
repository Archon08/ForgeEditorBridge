#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeWorldGenCapture.generated.h"

/**
 * v1.4 — World Generation Parameters Capture
 *
 * Exports procedural generation seeds and parameters: PCG volumes in the level,
 * custom terrain manager state (SWG-style), and World Partition status.
 * Allows an AI consumer to understand and reproduce world generation configurations.
 *
 * Output: {ProjectRoot}/Forge/ue-context/worldgen/parameters.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.world_gen_capture.export_world_gen_state()
 *
 * Exported JSON fields:
 *   generated, level_name,
 *   world_partition{enabled, cell_size, hlod_layers[], streaming_source_count},
 *   pcg{volume_count, volumes[]{actor_name, actor_path, actor_class, graph_asset, seed,
 *                               graph_parameters[]{name, type, value}}},
 *   terrain{found, actor_name, actor_class,
 *           properties[]{name, type, value}}
 *
 * PCG capture:
 *   Captures ALL actors with a UPCGComponent, not just APCGVolume subclasses.
 *   Graph parameters are extracted via TFieldIterator on UPCGGraph — any CPF_Edit
 *   property not inherited from the engine base class is treated as an overrideable param.
 *
 * Terrain capture:
 *   Reflection-based extraction from any actor whose class name contains "Terrain".
 *   Primitive types (int32, float, double, FString, FName, bool) are exported directly.
 *   TArray properties: exported as {type, element_count, first_elements[]}.
 *   Struct properties: recursed one level, sub-fields exported as {type, struct_type, fields{}}.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeWorldGenCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Capture PCG volumes, terrain manager state, and world partition config.
     * Writes worldgen/parameters.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportWorldGenState();

private:
    FString OutputDir;

    // READ-MERGE-WRITE index.json to add/update the "worldgen" section
    void UpdateIndexFile();
};
