#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "MovieRenderQueueHandler.generated.h"

/**
 * MovieRenderQueueHandler — domain "mrq"  (UE 5.7)
 *
 * Wraps UMoviePipelineQueueSubsystem — programmatic Movie Render Queue.
 * Settings on individual jobs are set via reflection (asset/set_prop) since
 * MRQ uses UMoviePipelineSetting subclasses.
 *
 * Actions:
 *   create_queue_asset   → asset_path
 *   load_queue_from_asset → asset_path (loads queue asset into the active subsystem)
 *   add_job              → sequence_path, map_path?, job_name?
 *   set_job_sequence     → job_index, sequence_path
 *   set_job_map          → job_index, map_path
 *   list_jobs            → (no params)
 *   remove_job           → job_index
 *   render_queue         → executor_class? ("PIE" default | full path)
 *   is_rendering         → returns bool
 *   cancel_render        → (no params)
 *   get_render_progress  → returns approximate progress
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMovieRenderQueueHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("mrq"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_queue_asset"), TEXT("load_queue_from_asset"),
            TEXT("add_job"), TEXT("set_job_sequence"), TEXT("set_job_map"),
            TEXT("list_jobs"), TEXT("remove_job"),
            TEXT("render_queue"), TEXT("is_rendering"), TEXT("cancel_render"),
            TEXT("get_render_progress")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_CreateQueueAsset    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_LoadQueueFromAsset  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddJob              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetJobSequence      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetJobMap           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListJobs            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveJob           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RenderQueue         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_IsRendering         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CancelRender        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetRenderProgress   (TSharedPtr<FJsonObject> Params);
};
