#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "EditorUtilityHandler.generated.h"

/**
 * EditorUtilityHandler — domain "editor_utility"
 *
 * Spawn EUWs, run EUBs, call UFUNCTION on utility objects.
 *
 * Actions:
 *   run_euw        → asset_path (spawns Editor Utility Widget tab)
 *   run_eub        → asset_path (runs Editor Utility Blueprint)
 *   call_function  → asset_path, function_name, args?
 *   list_utilities → prefix? (scans asset registry for EUW/EUB assets)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEditorUtilityHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("editor_utility"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("run_euw"), TEXT("run_eub"), TEXT("call_function"), TEXT("list_utilities") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
