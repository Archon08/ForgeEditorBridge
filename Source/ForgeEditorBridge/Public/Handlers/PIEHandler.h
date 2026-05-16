#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PIEHandler.generated.h"

/**
 * PIEHandler — domain "pie"  (UE 5.7)
 *
 * Drives Play-In-Editor lifecycle so handlers that require a runtime context
 * (EQS::run_debug_query, Chaos::set_physics_field, Camera::preview_shake) can
 * be reached without manual intervention.
 *
 * Actions:
 *   start_pie       → mode? ("play"|"simulate", default "play"), spawn_player_at_camera? (bool)
 *   stop_pie        → (no params) — requests end of the active PIE/Simulate session
 *   pause_pie       → (no params)
 *   resume_pie      → (no params)
 *   is_pie_active   → returns { active, mode, world_count }
 *   get_pie_info    → returns { active, mode, num_clients, num_pie_worlds }
 *   pilot_actor     → actor_label or actor_path — pilot the current viewport from this actor
 *   eject_pilot     → (no params)
 *   set_simulating  → simulating (bool) — toggle Simulate vs Play
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPIEHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("pie"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("start_pie"), TEXT("stop_pie"), TEXT("pause_pie"), TEXT("resume_pie"),
            TEXT("is_pie_active"), TEXT("get_pie_info"),
            TEXT("pilot_actor"), TEXT("eject_pilot"), TEXT("set_simulating")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_StartPIE      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_StopPIE       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_PausePIE      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ResumePIE     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_IsPIEActive   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetPIEInfo    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_PilotActor    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_EjectPilot    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetSimulating (TSharedPtr<FJsonObject> Params);
};