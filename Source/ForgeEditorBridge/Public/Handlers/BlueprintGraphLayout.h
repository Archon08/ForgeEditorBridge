#pragma once

#include "CoreMinimal.h"
#include "Results/BridgeResult.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * One "dev-notes" style comment group — a titled colored box wrapping a set of nodes.
 * Passed by Patch 3's assembler into Layout(), or parsed from JSON in LayoutAction().
 */
struct FORGEEDITORBRIDGE_API FBlueprintGraphGroup
{
    FString                 Title;
    TArray<FString>         NodeIds;   // node GUID strings OR node names (resolver tries both)
    FLinearColor            Color = FLinearColor(0.f, 0.f, 0.f, 0.f); // alpha < 0.05 → preset by title
};

/**
 * Sugiyama-style layered auto-layout for Blueprint graphs.
 *
 * Purpose: AI-generated graphs end up with nodes stacked on top of each other and
 * wires crossing randomly. This pass restructures them into a readable left-to-right
 * exec flow with optional reroute knots and titled comment boxes.
 *
 * Entry points:
 *   Layout(Graph, Groups, bInsertReroutes)
 *     In-process call used by FBlueprintGraphAssembler::Build after node/wire
 *     creation. Does not load/compile the Blueprint itself.
 *
 *   LayoutAction(BP, Graph, Params)
 *     JSON entry for the "layout_graph" action. Params may include:
 *       graph (string, optional)            — target graph name
 *       insert_reroutes (bool, default true)— emit UK2Node_Knot at long/crossing wires
 *       infer_groups (bool, default false)  — auto-wrap Branch true/false paths
 *       groups (array, optional)            — explicit group definitions
 */
class FORGEEDITORBRIDGE_API FBlueprintGraphLayout
{
public:
    static void          Layout      (UEdGraph* Graph,
                                      const TArray<FBlueprintGraphGroup>& Groups,
                                      bool bInsertReroutes = true);

    static FBridgeResult LayoutAction(UBlueprint* Blueprint,
                                      UEdGraph* Graph,
                                      TSharedPtr<FJsonObject> Params);

    /** Return preset color for a group title (case-insensitive substring match). */
    static FLinearColor  GetPresetColorForTitle(const FString& Title);
};