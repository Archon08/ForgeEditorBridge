#pragma once

#include "CoreMinimal.h"
#include "Results/BridgeResult.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

/**
 * High-level Blueprint graph DSL. One JSON call materializes many K2 nodes + wires
 * and optionally runs the layout pass — replaces dozens of individual add_node /
 * connect_pins / set_pin_default primitives.
 *
 * Input JSON:
 *   {
 *     "asset_path":  "/Game/BP_Item",
 *     "graph":       "EventGraph",
 *     "nodes": [
 *       { "id": "begin", "type": "event",       "name": "ReceiveBeginPlay" },
 *       { "id": "cast",  "type": "cast",        "target_class": "/Game/BP_Player.BP_Player_C" },
 *       { "id": "branch","type": "branch" },
 *       { "id": "rpc",   "type": "call_event",  "event": "Server_Pickup" },
 *       { "id": "loop",  "type": "for_each" },
 *       { "id": "getv",  "type": "get_var",     "var_name": "Health" }
 *     ],
 *     "wires":    [ { "from": "begin.then", "to": "cast.exec" }, ... ],
 *     "defaults": [ { "node": "branch", "pin": "Condition", "value": "true" } ],
 *     "groups":   [ { "title": "Authority check", "nodes": ["cast","branch"], "color": [0.2,0.8,0.4,0.3] } ],
 *     "layout":   "auto"      // "auto" | "none"
 *   }
 *
 * Supported node types:
 *   event, custom_event, function_call, call_event, cast, branch, sequence,
 *   get_var, set_var, for_each, reroute, comment
 *
 * The caller's UBlueprintHandler::Action_BuildBlueprintGraph is the thin entry
 * point — this class does the heavy lifting.
 */
class FORGEEDITORBRIDGE_API FBlueprintGraphAssembler
{
public:
    static FBridgeResult Build(UBlueprint* Blueprint,
                               UEdGraph* Graph,
                               TSharedPtr<FJsonObject> Params);
};