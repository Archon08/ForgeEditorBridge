#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "StringTableHandler.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API UStringTableHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("string_table"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_string_table"), TEXT("add_entry"), TEXT("get_entry"), TEXT("list_entries"), TEXT("remove_entry"), TEXT("update_entry"), TEXT("import_csv"), TEXT("export_csv") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
