#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "CurveHandler.generated.h"

/**
 * CurveHandler — domain "curve"  (UE 5.7)
 *
 * Authoring for UCurveFloat / UCurveVector / UCurveLinearColor / UCurveTable.
 *
 * Actions:
 *   create_curve_float   → asset_path
 *   create_curve_vector  → asset_path
 *   create_curve_color   → asset_path  (UCurveLinearColor)
 *   create_curve_table   → asset_path
 *   add_key              → asset_path, time, value (number for float; for vector/color use channel)
 *                          channel? ("x"|"y"|"z" for Vector; "r"|"g"|"b"|"a" for LinearColor)
 *                          interp? ("linear"|"constant"|"cubic"|"auto")
 *   remove_key           → asset_path, time, channel?
 *   set_key_tangent      → asset_path, time, arrive_tangent, leave_tangent, tangent_mode? ("auto"|"user"|"break"), channel?
 *   set_key_value        → asset_path, time, value, channel?
 *   eval                 → asset_path, time, channel? — evaluate curve at time
 *   list_keys            → asset_path, channel? — returns array of {time, value, interp_mode}
 *   clear_keys           → asset_path, channel?
 *   set_extrap           → asset_path, pre? ("constant"|"cycle"|"cycle_with_offset"|"linear"|"oscillate"),
 *                          post? (same), channel?
 */
UCLASS()
class FORGEEDITORBRIDGE_API UCurveHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("curve"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_curve_float"), TEXT("create_curve_vector"), TEXT("create_curve_color"),
            TEXT("create_curve_table"),
            TEXT("add_key"), TEXT("remove_key"), TEXT("set_key_tangent"), TEXT("set_key_value"),
            TEXT("eval"), TEXT("list_keys"), TEXT("clear_keys"), TEXT("set_extrap")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateCurveFloat   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateCurveVector  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateCurveColor   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateCurveTable   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddKey             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveKey          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetKeyTangent      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetKeyValue        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Eval               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListKeys           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearKeys          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetExtrap          (TSharedPtr<FJsonObject> Params);
};