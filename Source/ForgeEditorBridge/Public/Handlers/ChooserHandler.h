#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ChooserHandler.generated.h"

/**
 * ChooserHandler — domain "chooser"  (UE 5.7)
 *
 * UChooserTable creation and basic introspection. The Chooser column/row
 * structures are FInstancedStruct-based and best authored via Python or the
 * editor; this handler covers asset lifecycle + reflection-friendly queries.
 *
 * Actions:
 *   create_chooser     → asset_path
 *   get_chooser_info   → asset_path → returns {row_count, column_count, output_type}
 *   set_output_object  → asset_path, class_path (sets the chooser's output to UObject)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UChooserHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("chooser"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return { TEXT("create_chooser"), TEXT("get_chooser_info"), TEXT("set_output_object") };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateChooser   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetChooserInfo  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetOutputObject (TSharedPtr<FJsonObject> Params);
};
