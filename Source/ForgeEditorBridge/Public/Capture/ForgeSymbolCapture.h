#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeSymbolCapture.generated.h"

/**
 * v1.5 — C++ Symbol Index
 *
 * Iterates all loaded UClass objects belonging to the host project's own
 * /Script/&lt;ProjectName&gt; package (auto-detected via FApp::GetProjectName()) and
 * exports a structured symbol table: class hierarchy, implemented interfaces,
 * direct properties (with Net/BlueprintVisible/Config flags), and direct
 * functions (with parameter types and net/blueprint flags). Engine UClasses
 * are skipped by default; extend GetTargetPrefixes() in ForgeSymbolCapture.cpp
 * to widen the scan.
 *
 * Output: {ProjectRoot}/Forge/ue-context/symbols/project_symbols.json
 *
 * Trigger from Python (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.symbol_capture.export_symbol_index()
 *
 * Exported JSON fields:
 *   generated, project_packages[], class_count,
 *   classes[]{
 *     name, package, parent,
 *     is_abstract, is_actor, is_component, is_interface,
 *     interfaces[],
 *     properties[]{name, type, flags[]},
 *     functions[]{name, return, flags[], params[]{name, type}}
 *   }
 *
 * Property flags: Net, BlueprintVisible, BlueprintReadOnly, EditAnywhere,
 *                 Config, SaveGame, Transient
 * Function flags: BlueprintCallable, BlueprintPure, BlueprintEvent,
 *                 Static, Const, NetServer, NetClient, NetMulticast
 *
 * Notes:
 *   - Only direct (non-inherited) properties and functions per class.
 *   - Property type names are human-readable (e.g. "TArray<float>", "UMyClass*").
 *   - Capped at 1000 classes and 200 properties/functions per class.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeSymbolCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    /**
     * Iterate all UClass objects in target project packages and write
     * symbols/project_symbols.json.
     * Returns true on successful write.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportSymbolIndex();

private:
    FString OutputDir;

    // READ-MERGE-WRITE index.json to add/update the "symbols" section
    void UpdateIndexFile();
};
