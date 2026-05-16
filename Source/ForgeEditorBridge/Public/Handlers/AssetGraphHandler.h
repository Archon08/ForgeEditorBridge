#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "AssetGraphHandler.generated.h"

/**
 * AssetGraphHandler — domain "asset_graph"  (Phase 4 / UE 5.7)
 *
 * Actions:
 *   get_references   → asset_path — returns referencers
 *   get_dependencies → asset_path, recursive(bool) — returns dependencies
 *   validate_references → asset_path — checks all deps exist
 *   get_package_size → asset_path — returns disk size
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAssetGraphHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("asset_graph"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("get_references"), TEXT("get_dependencies"),
			TEXT("validate_references"), TEXT("get_package_size")
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetReferences     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetDependencies   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ValidateReferences(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPackageSize    (TSharedPtr<FJsonObject> Params);

	/** Human-readable file size. */
	static FString HumanReadableSize(int64 Bytes);
};
