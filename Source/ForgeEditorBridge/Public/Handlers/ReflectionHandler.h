#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ReflectionHandler.generated.h"

/**
 * ReflectionHandler — domain "reflection"  (v0.12.0 / Wave L)
 *
 * The "Escape Hatch" — direct string-based access to any UPROPERTY or
 * Editor Utility function in the engine via UE reflection.
 *
 * Actions:
 *   get_prop         → asset_path (string), property_name (string)
 *                      Reads any UPROPERTY value from a loaded UObject by string name.
 *                      Returns the value as a string via ExportText.
 *
 *   set_prop         → asset_path (string), property_name (string), value (string)
 *                      Sets any UPROPERTY value on a loaded UObject via ImportText.
 *
 *   call_utility_fn  → asset_path (string), function_name (string),
 *                      args (JSON object, optional)
 *                      Invokes a UFUNCTION (CallInEditor / BlueprintCallable)
 *                      on a loaded UObject.
 *
 *   run_euw          → widget_path (string)
 *                      Launches an Editor Utility Widget (Blutility) by asset path.
 *
 *   read_symbol_capture → (no params) → reads symbols/index.json from capture output
 */
UCLASS()
class FORGEEDITORBRIDGE_API UReflectionHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("reflection"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("get_prop"), TEXT("set_prop"), TEXT("call_utility_fn"), TEXT("run_euw"), TEXT("list_functions"), TEXT("read_symbol_capture") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetProp        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetProp        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CallUtilityFn  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunEUW         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListFunctions  (TSharedPtr<FJsonObject> Params);

	UObject* LoadObjectByPath(const FString& AssetPath, FBridgeResult& Result);
};
