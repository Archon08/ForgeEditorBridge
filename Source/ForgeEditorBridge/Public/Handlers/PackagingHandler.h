#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "PackagingHandler.generated.h"

/**
 * PackagingHandler — domain "packaging" (v2.0.0 / UE 5.7)
 *
 * Cook, package, and inspect project build outputs.
 *
 * Actions:
 *   cook_content     → platform (string), maps[] (string[], opt), iterate (bool, opt)
 *                      Python dispatch — invokes UAT BuildCookRun commandlet.
 *
 *   package_project  → platform (string), config (Development|Shipping), output_dir (string)
 *                      Python dispatch — invokes UAT BuildCookRun -package.
 *
 *   get_cook_status  → (no params)
 *                      Read last Cook.log from Saved/Logs — returns tail + error count.
 *
 *   validate_content → (no params)
 *                      Run UEditorValidatorSubsystem on all checked-out assets.
 *
 *   get_package_size → (no params)
 *                      Scan project Content dir; returns total + breakdown by extension.
 *
 *   list_platforms   → (no params)
 *                      Returns available target platforms via ITargetPlatformManagerModule.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPackagingHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("packaging"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("cook_content"),
			TEXT("package_project"),
			TEXT("get_cook_status"),
			TEXT("validate_content"),
			TEXT("get_package_size"),
			TEXT("list_platforms"),
			TEXT("cook_incremental"),
			TEXT("enable_zen_streaming"),
			TEXT("cook_dlc"),
			TEXT("set_build_target"),
			TEXT("set_build_config"),
			TEXT("get_build_targets"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CookContent       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_PackageProject    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetCookStatus     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ValidateContent   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPackageSize    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListPlatforms     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CookIncremental   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_EnableZenStreaming(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CookDLC           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetBuildTarget    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetBuildConfig    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetBuildTargets   (TSharedPtr<FJsonObject> Params);
};
