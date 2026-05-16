#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeLocalizationCapture.generated.h"

/**
 * v1.0 — Localization Coverage Capture
 *
 * Scans Content/Localization/ to export target configurations and per-culture
 * translation coverage statistics.
 *
 * Output: {ProjectRoot}/Forge/ue-context/localization/coverage.json
 *
 * Trigger from Python:
 *   sub = unreal.get_editor_subsystem(unreal.ForgeAISubsystem)
 *   sub.localization_capture.export_localization_coverage()
 *
 * JSON structure:
 *   generated, source_culture, current_culture,
 *   targets[]{name, path, cultures[]{culture, po_file, key_count,
 *             translated_count, coverage_pct, status}}
 *
 * Coverage is computed from PO files (msgid/msgstr counts) when available.
 * If only .locres (binary) or .manifest files exist, coverage is listed as
 * "no_po_file" with file presence noted.
 *
 * Note: To generate fresh PO files, use the Localization Dashboard
 * (Window > Localization Dashboard) or run the InternationalizationExport
 * commandlet (see LocalizationHandler::export_po for the command).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeLocalizationCapture : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);

	/**
	 * Scan localization directory and export coverage report.
	 * Returns true on successful write.
	 */
	UFUNCTION(BlueprintCallable, Category = "Forge")
	bool ExportLocalizationCoverage();

private:
	FString OutputDir;

	/**
	 * Parse a PO file and return total msgid count and translated msgstr count.
	 * A msgstr is considered translated if it is non-empty.
	 * Skips the file header entry (msgid "").
	 */
	static void ParsePOFile(const FString& FilePath, int32& OutTotal, int32& OutTranslated);

	void UpdateIndexFile();
};
