#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "AssetValidatorHandler.generated.h"

/**
 * AssetValidatorHandler — domain "validator"  (UE 5.7)
 *
 * Wraps UEditorValidatorSubsystem + UDataValidationSubsystem.
 * Existing asset/validate_asset is high-level pass/fail; this domain returns
 * rule-level results from registered UEditorValidatorBase subclasses.
 *
 * Actions:
 *   list_validators           → array of registered validator class names
 *   validate_asset_detailed   → asset_path → returns per-validator outcomes
 *   validate_path             → content_path (e.g. "/Game/Audio") → bulk validation
 *   get_last_validation_results → returns the cached results from the most
 *                                 recent validate_path or validate_asset_detailed
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAssetValidatorHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("validator"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("list_validators"), TEXT("validate_asset_detailed"),
            TEXT("validate_path"), TEXT("get_last_validation_results")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    TSharedPtr<FJsonObject> LastResults;

    FBridgeResult Action_ListValidators        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ValidateAssetDetailed (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ValidatePath          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetLastResults        (TSharedPtr<FJsonObject> Params);
};
