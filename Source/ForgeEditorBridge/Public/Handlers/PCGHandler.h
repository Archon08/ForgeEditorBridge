#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PCGHandler.generated.h"

class AActor;
class UWorld;

/**
 * PCGHandler — Phase 5
 * Domain: "pcg"
 * Actions: execute_graph, set_pcg_parameter
 *
 * actor_path can be an actor label, actor name, or full path name.
 * component_name is optional for execute_graph (uses first PCGComponent if omitted).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPCGHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("pcg"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("execute_graph"), TEXT("set_pcg_parameter") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult ExecuteGraph(TSharedPtr<FJsonObject> Params);
	FBridgeResult SetPCGParameter(TSharedPtr<FJsonObject> Params);

	// Finds an actor by label, name, or path in the editor world
	AActor* FindActorByPath(UWorld* World, const FString& ActorPath) const;

	// Sets a typed property on an object via reflection; returns error message or empty on success
	FString SetReflectedProperty(UObject* Target, const FString& PropName, const FString& Value) const;
};
