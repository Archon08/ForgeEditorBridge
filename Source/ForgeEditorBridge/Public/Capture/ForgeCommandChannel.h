#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Containers/Ticker.h"
#include "ForgeCommandChannel.generated.h"

class UForgeAISubsystem;

/**
 * v1.7 — Command Channel
 *
 * Polls {ProjectRoot}/Forge/ue-context/commands/inbox/ every 1 second for
 * JSON command files written by an AI consumer. Each file is parsed, dispatched to
 * the appropriate handler, then moved to commands/history/ alongside a
 * companion _result.json recording the outcome.
 *
 * Safety: polling is suspended during PIE. At most 5 commands are processed
 * per tick to avoid stalling the editor.
 *
 * Supported command types (JSON field "command"):
 *   trigger_pcg_regenerate   — Calls Generate(true) on the UPCGComponent of a named actor
 *   spawn_test_actor         — Spawns a UClass at a given world location + rotation
 *   set_actor_property       — Reflection-based property write on an actor or its components;
 *                              optional "component_name" field disambiguates multi-component actors
 *   apply_weather            — Batch property writes targeting weather actors (batch set_actor_property)
 *   capture_screenshot       — Manually triggers UForgeScreenshotCapture (v0.3)
 *   export_asset_registry    — Manually triggers UForgeAssetRegistryCapture (v0.7)
 *   create_material_instance — Creates a UMaterialInstanceConstant from a parent material asset
 *   import_datatable_rows    — Adds/updates rows in a UDataTable asset (v0.6 logic, unified here)
 *   set_actor_transform      — Sets location, rotation, and/or scale on a named actor in one shot
 *   delete_actor             — Destroys a named actor; creates an undo entry
 *   save_packages            — Flushes dirty content packages to disk; optional "paths" filter
 *   draw_debug_line          — Draws a persistent debug line in the Editor viewport
 *                              { start{x,y,z}, end{x,y,z}, color, duration, thickness }
 *   draw_debug_sphere        — Draws a persistent debug sphere in the Editor viewport
 *                              { center{x,y,z}, radius, color, duration, thickness }
 *   draw_debug_box           — Draws a persistent debug box in the Editor viewport
 *                              { center{x,y,z}, extent{x,y,z}, rotation{pitch,yaw,roll}, color, duration, thickness }
 *   draw_debug_text          — Draws a debug string at a world location in the Editor viewport
 *                              { text, location{x,y,z}, color, duration }
 *   clear_debug_shapes       — Flushes all persistent debug lines and strings from the Editor viewport
 *   capture_perf_snapshot    — Triggers UForgePerformanceCapture::CaptureSnapshot() (v1.15)
 *
 * Color format (all draw commands): hex string "#RRGGBB" / "#RRGGBBAA",
 *   or named color: Red, Green, Blue, White, Black, Yellow, Cyan, Magenta, Orange, Purple
 * Duration (seconds): default 0 = persistent until clear_debug_shapes is called;
 *   positive value = auto-expires after that many seconds (cannot be flushed early)
 *
 * Directory layout (relative to ue-context/):
 *   commands/inbox/                     — AI consumer writes command files here
 *   commands/history/                   — Processed command files archived here
 *   commands/history/{name}_result.json — Per-command success/failure report
 *   commands/failed/                    — Quarantined files (unreadable / invalid JSON)
 *
 * Command file format (minimal examples):
 *   { "command": "capture_screenshot" }
 *   { "command": "trigger_pcg_regenerate", "actor_label": "PCGVolume_0" }
 *   { "command": "set_actor_property", "actor_label": "SunLight",
 *     "property": "Intensity", "value": "8.0" }
 *   { "command": "set_actor_property", "actor_label": "SM_Rock_12",
 *     "component_name": "StaticMeshComponent0",
 *     "property": "bCastShadow", "value": "false" }
 *   { "command": "set_actor_transform", "actor_label": "SM_Rock_0",
 *     "location": {"x": 100, "y": 200, "z": 50},
 *     "rotation": {"pitch": 0, "yaw": 45, "roll": 0},
 *     "scale":    {"x": 2.0, "y": 2.0, "z": 2.0} }
 *   { "command": "delete_actor", "actor_label": "OldTestActor" }
 *   { "command": "save_packages" }
 *   { "command": "save_packages", "paths": ["/Game/Materials/MI_Blue_Base"] }
 *   { "command": "apply_weather",
 *     "changes": [
 *       { "actor_label": "SunLight", "property": "Intensity", "value": "8.0" },
 *       { "actor_label": "ExponentialHeightFog_0", "property": "FogDensity", "value": "0.02" }
 *     ] }
 *   { "command": "spawn_test_actor", "class_path": "/Script/Engine.PointLight",
 *     "location": {"x": 0, "y": 0, "z": 200},
 *     "rotation": {"pitch": 0, "yaw": 90, "roll": 0},
 *     "label": "TestLight" }
 *   { "command": "create_material_instance",
 *     "parent_path": "/Game/Materials/M_Base",
 *     "output_path": "/Game/Materials/",
 *     "instance_name": "MI_Blue_Base" }
 *   { "command": "import_datatable_rows",
 *     "asset_path": "/Game/Data/DT_Items",
 *     "rows": { "Row_Sword": { "Name": "Iron Sword", "Damage": 10 } } }
 *
 * FRotator ImportText format (for set_actor_property / apply_weather):
 *   "(Pitch=-45.000000,Yaw=30.000000,Roll=0.000000)"  — field names must be capitalized
 *
 * index.json: adds a "commands" section after the first successful execution.
 *
 * Python trigger (after editor restart):
 *   sub = unreal.get_editor_subsystem(unreal.ForgeContextSubsystem)
 *   sub.command_channel.poll_inbox()
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeCommandChannel : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir, UForgeAISubsystem* InSubsystem);
    void Deinitialize();

    /**
     * Scan the inbox directory and execute up to MaxPerPoll pending commands.
     * Called automatically every second by the internal ticker.
     * Also callable directly from Python for on-demand processing.
     * Returns the number of command files processed this call.
     */
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 PollInbox();

private:
    FString OutputDir;

    /** Weak reference back to the subsystem — used to reach other captures. */
    TObjectPtr<UForgeAISubsystem> Subsystem;

    FTSTicker::FDelegateHandle TickHandle;
    FDelegateHandle BeginPIEHandle;
    FDelegateHandle EndPIEHandle;

    bool bPIEActive = false;

    /** Maximum commands executed in a single tick. */
    static constexpr int32 MaxPerTick = 5;

    // ---- Ticker / PIE delegates -----------------------------------------

    bool OnTick(float DeltaTime);
    void OnBeginPIE(bool bIsSimulating);
    void OnEndPIE(bool bIsSimulating);

    // ---- Core dispatch --------------------------------------------------

    /**
     * Read, parse, dispatch, archive, and write result for a single command file.
     * Returns true if the file was consumed (success or handled failure).
     */
    bool ProcessCommandFile(const FString& FilePath);

    // ---- Command handlers -----------------------------------------------
    // Each fills OutResult with at minimum a "status" field ("ok" or "error")
    // plus any command-specific output. Returns true on success.

    bool Cmd_TriggerPCGRegenerate  (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_SpawnTestActor        (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_SetActorProperty      (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_ApplyWeather          (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_CaptureScreenshot     (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_ExportAssetRegistry   (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_CreateMaterialInstance(const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_ImportDataTableRows   (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_SetActorTransform     (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_DeleteActor           (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_SavePackages          (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_DrawDebugLine         (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_DrawDebugSphere       (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_DrawDebugBox          (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_DrawDebugText         (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_ClearDebugShapes      (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);
    bool Cmd_CapturePerfSnapshot   (const TSharedPtr<FJsonObject>& Cmd, TSharedRef<FJsonObject> OutResult);

    // ---- Shared helpers -------------------------------------------------

    /**
     * Safely retrieve the editor world without triggering check(0) in UE5.7.
     * GEditor->GetEditorWorldContext() asserts if no editor context exists.
     * This iterates GEngine->GetWorldContexts() for EWorldType::Editor instead.
     */
    UWorld* GetEditorWorld() const;

    /** Find the first actor in World whose label matches ActorLabel (case-insensitive). */
    AActor* FindActorByLabel(UWorld* World, const FString& ActorLabel) const;

    /**
     * Set a named property on Obj (actor or one of its components) from a string value.
     * Uses UE5 reflection: FindFProperty + ImportText_Direct.
     * ComponentNameFilter: if non-empty, only components whose GetName() matches are searched.
     * Returns true if a property was found and the import succeeded.
     */
    bool SetObjectPropertyFromString(UObject* Obj, const FString& PropertyName,
                                     const FString& Value,
                                     const FString& ComponentNameFilter = TEXT("")) const;

    /**
     * Shared implementation for a single actor-label / property / value change.
     * Used by both Cmd_SetActorProperty and Cmd_ApplyWeather.
     * Returns a short status string ("ok" or an error description).
     */
    FString ApplySinglePropertyChange(UWorld* World, const FString& ActorLabel,
                                      const FString& Property, const FString& Value,
                                      const FString& ComponentName = TEXT("")) const;

    // READ-MERGE-WRITE index.json to add/update the "commands" section
    void UpdateIndexFile();
};
