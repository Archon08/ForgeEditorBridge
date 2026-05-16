#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "InputHandler.generated.h"

/**
 * InputHandler — domain "input"  (v0.11.0 / UE 5.7)
 *
 * Actions:
 *   create_action   → asset_path (string, e.g. "/Game/Input/IA_Jump"),
 *                     value_type ("bool"|"axis1d"|"axis2d"|"axis3d"; default "bool")
 *
 *   create_context  → asset_path (string, e.g. "/Game/Input/IMC_Default")
 *
 *   add_mapping     → context_path (string — asset path of UInputMappingContext),
 *                     action_path  (string — asset path of UInputAction),
 *                     key          (string — FKey name, e.g. "SpaceBar", "Gamepad_FaceButton_Bottom",
 *                                   "Gamepad_LeftX")
 *
 *   read_input_capture → triggers InputCapture export and returns input/enhanced_input_audit.json
 */
UCLASS()
class FORGEEDITORBRIDGE_API UInputHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("input"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_action"), TEXT("create_context"), TEXT("add_mapping"), TEXT("list_actions"), TEXT("list_contexts"), TEXT("get_mappings"), TEXT("read_input_capture") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateAction  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CreateContext (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddMapping    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListActions   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListContexts  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMappings       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadInputCapture  (TSharedPtr<FJsonObject> Params);
};
