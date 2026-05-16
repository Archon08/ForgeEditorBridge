#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "BridgeResult.generated.h"

/**
 * Error code taxonomy:
 *   0      = Success
 *   1000   = Missing required parameter
 *   1001   = Invalid parameter value
 *   1002   = Ambiguous selector (multiple assets/actors matched)
 *   2000   = Asset not found
 *   2001   = Wrong asset type
 *   2002   = Asset already exists
 *   2003   = Asset not loaded (exists in registry but not in memory)
 *   3000   = Engine API failure
 *   3001   = Compilation failure
 *   3002   = Compile required (blueprint/module has unresolved errors)
 *   3003   = Module not available (required plugin/processor not loaded)
 *   3004   = Blocked API (requires PIE or runtime context)
 *   4000   = Quarantine intercept (destructive op blocked)
 *   5000   = Internal bridge error
 */
USTRUCT()
struct FBridgeResult
{
	GENERATED_BODY()

	UPROPERTY()
	bool bSuccess = false;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	FString Domain;

	UPROPERTY()
	FString Action;

	UPROPERTY()
	FString AffectedPath;

	UPROPERTY()
	FString Timestamp;

	UPROPERTY()
	FString ExtraData;  // JSON string for complex responses (backward compat)

	/** Structured JSON data for rich query responses. */
	TSharedPtr<FJsonObject> Data;

	/** Machine-readable error code (0 = success). */
	UPROPERTY()
	int32 ErrorCode = 0;

	/** Machine-readable recovery hint for AI self-correction. */
	UPROPERTY()
	FString RecoveryHint;
};
