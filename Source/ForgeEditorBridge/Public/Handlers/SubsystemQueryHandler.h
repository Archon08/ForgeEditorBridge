#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "SubsystemQueryHandler.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API USubsystemQueryHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("subsystem_query"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("list_editor_subsystems"), TEXT("list_world_subsystems") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
