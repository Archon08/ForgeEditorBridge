#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "RuntimeCaptureHandler.generated.h"

/**
 * RuntimeCaptureHandler — domain "runtime_capture"  (v0.11.0 / UE 5.7)
 *
 * Actions:
 *   capture_pie_state → max_actors   (int, default 200)
 *                       include_tags (bool, default true — capture GAS gameplay tags per actor)
 *
 *   Returns: Result.Message contains the full JSON snapshot string.
 *   JSON shape:
 *   {
 *     "captured_at": "<ISO8601>",
 *     "actor_count": <int>,
 *     "actors": [
 *       {
 *         "name":     "<actor label>",
 *         "class":    "<class name>",
 *         "location": { "x": f, "y": f, "z": f },
 *         "rotation": { "pitch": f, "yaw": f, "roll": f },
 *         "gameplay_tags": ["Tag.A", "Tag.B"]   // only present when actor has ASC
 *       }
 *     ]
 *   }
 *
 *   Fails gracefully (bSuccess=false) when PIE is not running.
 *
 *   capture_runtime_variables → (no params) → triggers variable snapshot, reads runtime/variables.json
 */
UCLASS()
class FORGEEDITORBRIDGE_API URuntimeCaptureHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("runtime_capture"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("capture_pie_state"), TEXT("capture_runtime_variables") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CapturePIEState(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CaptureRuntimeVariables(TSharedPtr<FJsonObject> Params);
};
