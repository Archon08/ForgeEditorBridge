#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "SourceControlHandler.generated.h"

/**
 * SourceControlHandler — domain "source_control"  (v0.4.0)
 *
 * Wraps the engine's ISourceControlProvider to expose checkout, checkin,
 * revert, status, and mark-for-add operations over the bridge.
 *
 * Actions:
 *   checkout      — files[]                       → checks out files from SCC
 *   checkin       — files[], description           → checks in files with description
 *   revert        — files[]                       → reverts files to depot version
 *   get_status    — files[]                       → per-file SCC state flags
 *   mark_for_add  — files[]                       → marks new files for add
 */
UCLASS()
class FORGEEDITORBRIDGE_API USourceControlHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("source_control"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("checkout"), TEXT("checkin"), TEXT("revert"), TEXT("get_status"), TEXT("mark_for_add") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_Checkout(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Checkin(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Revert(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetStatus(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_MarkForAdd(TSharedPtr<FJsonObject> Params);

	/** Convert asset paths (e.g. /Game/Maps/MyLevel) to on-disk filenames. */
	TArray<FString> AssetPathsToDiskPaths(const TArray<FString>& AssetPaths);
};
