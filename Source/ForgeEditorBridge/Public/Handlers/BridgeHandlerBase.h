#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Results/BridgeResult.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "BridgeHandlerBase.generated.h"

class UForgeAISubsystem;

// Backward-compatible type alias so existing handler code compiles unchanged.
using UBridgeSubsystem = UForgeAISubsystem;

UCLASS(Abstract)
class FORGEEDITORBRIDGE_API UBridgeHandlerBase : public UObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(UForgeAISubsystem* InSubsystem);
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) PURE_VIRTUAL(UBridgeHandlerBase::HandleCommand, return FBridgeResult(););

	/** Return the domain string this handler registers under (e.g. "blueprint", "actor"). */
	virtual FString GetDomainName() const PURE_VIRTUAL(UBridgeHandlerBase::GetDomainName, return FString(););

	/** Return the list of actions this handler supports (for system/capabilities). */
	virtual TArray<FString> GetSupportedActions() const { return {}; }

	/**
	 * Return a JSON object describing each action's parameters.
	 * Format: { "action_name": { "params": { "param_name": { "type": "string", "required": true, "desc": "..." } }, "desc": "..." }, ... }
	 * Override in handlers to provide self-documenting param schemas.
	 */
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const { return nullptr; }

protected:
	UPROPERTY()
	TObjectPtr<UForgeAISubsystem> Subsystem;

	FBridgeResult CreateResult(const FString& Domain, const FString& Action);

	/**
	 * Begin an editor transaction scoped to this handler operation.
	 * Returns nullptr outside of WITH_EDITOR builds.
	 * Usage: auto Tx = BeginTransaction(TEXT("Set material slot"));
	 */
	static TUniquePtr<FScopedTransaction> BeginTransaction(const FString& Description);

	/** Create an error result with code and recovery hint. */
	static FBridgeResult MakeError(const FString& Domain, const FString& Action,
		int32 ErrorCode, const FString& Message, const FString& RecoveryHint = FString());

	/** Create a success result with optional structured data. */
	static FBridgeResult MakeSuccess(const FString& Domain, const FString& Action,
		const FString& Message, TSharedPtr<FJsonObject> Data = nullptr);

	/**
	 * Standardized error for unknown action names within a domain.
	 * Emits ErrorCode 1001 (InvalidParameterValue) with a consistent message
	 * and a recovery hint listing the valid actions.
	 */
	static FBridgeResult MakeUnknownAction(const FString& Domain, const FString& Action,
		const FString& ValidActionsCsv);

	/**
	 * Return a valid PIE UWorld* if PIE is active, or nullptr if not.
	 * On nullptr, OutResult is populated with a 3004 BlockedAPI error
	 * (ready to return directly from the action body).
	 */
	static class UWorld* GuardPIE(const FString& Domain, const FString& Action,
		FBridgeResult& OutResult);
};
