#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgePackagingCapture.generated.h"

/**
 * v1.0 — Packaging & Cook State Capture
 *
 * Exports the last cook log summary, a content directory size breakdown by
 * asset type, and available target platform configurations.
 *
 * Output: {ProjectRoot}/Forge/ue-context/packaging/last_cook.json
 *
 * Trigger from Python:
 *   sub = unreal.get_editor_subsystem(unreal.ForgeAISubsystem)
 *   sub.packaging_capture.export_packaging_state()
 *
 * JSON structure:
 *   generated,
 *   cook_log{found, path, total_lines, error_count, warning_count,
 *            completed, last_modified, tail_30[]},
 *   content_size{total_files, total_bytes, total_mb, content_dir,
 *                by_extension[]{ext, file_count, bytes, mb}},
 *   platforms[]{name, display_name, is_running}
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgePackagingCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Export cook log summary, content size breakdown, and platform list.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportPackagingState();

private:
	FString OutputDir;

	void UpdateIndexFile();
};
