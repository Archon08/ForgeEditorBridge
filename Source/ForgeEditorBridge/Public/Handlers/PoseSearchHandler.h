#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PoseSearchHandler.generated.h"

/**
 * PoseSearchHandler — domain "posesearch"  (UE 5.7)
 *
 * Authoring for Motion Matching: UPoseSearchSchema + UPoseSearchDatabase.
 * Database entry CRUD uses FInstancedStruct internals — handler covers
 * lifecycle + index build; entry edits go through asset/set_prop reflection.
 *
 * Actions:
 *   create_schema       → asset_path
 *   create_database     → asset_path
 *   set_database_schema → db_path, schema_path
 *   build_index         → db_path
 *   get_database_info   → db_path → returns {schema_path, num_entries, indexed}
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPoseSearchHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("posesearch"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_schema"), TEXT("create_database"),
            TEXT("set_database_schema"), TEXT("build_index"), TEXT("get_database_info")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateSchema      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateDatabase    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetDatabaseSchema (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_BuildIndex        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetDatabaseInfo   (TSharedPtr<FJsonObject> Params);
};
