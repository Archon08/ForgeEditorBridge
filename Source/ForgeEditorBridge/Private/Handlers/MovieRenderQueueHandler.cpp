#include "Handlers/MovieRenderQueueHandler.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelinePIEExecutor.h"
#include "MovieRenderPipelineDataTypes.h"
#include "Editor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("mrq");

namespace
{
    UMoviePipelineQueueSubsystem* GetQS()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    }
}

FBridgeResult UMovieRenderQueueHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_queue_asset"))    return Action_CreateQueueAsset(Params);
    if (Action == TEXT("load_queue_from_asset")) return Action_LoadQueueFromAsset(Params);
    if (Action == TEXT("add_job"))               return Action_AddJob(Params);
    if (Action == TEXT("set_job_sequence"))      return Action_SetJobSequence(Params);
    if (Action == TEXT("set_job_map"))           return Action_SetJobMap(Params);
    if (Action == TEXT("list_jobs"))             return Action_ListJobs(Params);
    if (Action == TEXT("remove_job"))            return Action_RemoveJob(Params);
    if (Action == TEXT("render_queue"))          return Action_RenderQueue(Params);
    if (Action == TEXT("is_rendering"))          return Action_IsRendering(Params);
    if (Action == TEXT("cancel_render"))         return Action_CancelRender(Params);
    if (Action == TEXT("get_render_progress"))   return Action_GetRenderProgress(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_queue_asset, load_queue_from_asset, add_job, set_job_sequence, set_job_map, list_jobs, remove_job, render_queue, is_rendering, cancel_render, get_render_progress"));
}

FBridgeResult UMovieRenderQueueHandler::Action_CreateQueueAsset(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_queue_asset"), 1000, TEXT("'asset_path' is required"));

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return MakeError(DOMAIN, TEXT("create_queue_asset"), 3000,
        FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
    Package->FullyLoad();

    UMoviePipelineQueue* Queue = NewObject<UMoviePipelineQueue>(Package, FName(*AssetName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Queue) return MakeError(DOMAIN, TEXT("create_queue_asset"), 3000, TEXT("NewObject failed"));
    FAssetRegistryModule::AssetCreated(Queue);
    Package->MarkPackageDirty();

    return MakeSuccess(DOMAIN, TEXT("create_queue_asset"),
        FString::Printf(TEXT("Created MRQ queue asset at '%s'"), *AssetPath));
}

FBridgeResult UMovieRenderQueueHandler::Action_LoadQueueFromAsset(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("load_queue_from_asset"), 1000, TEXT("'asset_path' is required"));

    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("load_queue_from_asset"), 3000, TEXT("MRQ subsystem unavailable"));

    UMoviePipelineQueue* Source = LoadObject<UMoviePipelineQueue>(nullptr, *AssetPath);
    if (!Source)
    {
        const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        Source = LoadObject<UMoviePipelineQueue>(nullptr, *Suffix);
    }
    if (!Source) return MakeError(DOMAIN, TEXT("load_queue_from_asset"), 2000,
        FString::Printf(TEXT("Queue asset not found: %s"), *AssetPath));

    if (UMoviePipelineQueue* Active = QS->GetQueue())
    {
        Active->CopyFrom(Source);
    }

    return MakeSuccess(DOMAIN, TEXT("load_queue_from_asset"),
        FString::Printf(TEXT("Loaded queue from '%s'"), *AssetPath));
}

FBridgeResult UMovieRenderQueueHandler::Action_AddJob(TSharedPtr<FJsonObject> Params)
{
    FString SequencePath, MapPath, JobName;
    if (!Params->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_job"), 1000, TEXT("'sequence_path' is required"));
    Params->TryGetStringField(TEXT("map_path"), MapPath);
    Params->TryGetStringField(TEXT("job_name"), JobName);

    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("add_job"), 3000, TEXT("MRQ subsystem unavailable"));
    UMoviePipelineQueue* Queue = QS->GetQueue();
    if (!Queue) return MakeError(DOMAIN, TEXT("add_job"), 3000, TEXT("Active queue unavailable"));

    UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Job) return MakeError(DOMAIN, TEXT("add_job"), 3000, TEXT("AllocateNewJob returned null"));

    Job->Sequence = FSoftObjectPath(SequencePath);
    if (!MapPath.IsEmpty()) Job->Map = FSoftObjectPath(MapPath);
    if (!JobName.IsEmpty()) Job->JobName = JobName;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("job_index"), Queue->GetJobs().Num() - 1);
    Data->SetStringField(TEXT("sequence_path"), SequencePath);
    return MakeSuccess(DOMAIN, TEXT("add_job"),
        FString::Printf(TEXT("Job added (index=%d)"), Queue->GetJobs().Num() - 1), Data);
}

FBridgeResult UMovieRenderQueueHandler::Action_SetJobSequence(TSharedPtr<FJsonObject> Params)
{
    int32 Index = -1; FString Path;
    Params->TryGetNumberField(TEXT("job_index"), Index);
    if (!Params->TryGetStringField(TEXT("sequence_path"), Path) || Path.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_job_sequence"), 1000, TEXT("'sequence_path' is required"));
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("set_job_sequence"), 3000, TEXT("MRQ subsystem unavailable"));
    UMoviePipelineQueue* Queue = QS->GetQueue();
    if (!Queue || Index < 0 || Index >= Queue->GetJobs().Num())
        return MakeError(DOMAIN, TEXT("set_job_sequence"), 1001, TEXT("Invalid 'job_index'"));
    Queue->GetJobs()[Index]->Sequence = FSoftObjectPath(Path);
    return MakeSuccess(DOMAIN, TEXT("set_job_sequence"),
        FString::Printf(TEXT("Job %d sequence -> '%s'"), Index, *Path));
}

FBridgeResult UMovieRenderQueueHandler::Action_SetJobMap(TSharedPtr<FJsonObject> Params)
{
    int32 Index = -1; FString Path;
    Params->TryGetNumberField(TEXT("job_index"), Index);
    if (!Params->TryGetStringField(TEXT("map_path"), Path) || Path.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_job_map"), 1000, TEXT("'map_path' is required"));
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("set_job_map"), 3000, TEXT("MRQ subsystem unavailable"));
    UMoviePipelineQueue* Queue = QS->GetQueue();
    if (!Queue || Index < 0 || Index >= Queue->GetJobs().Num())
        return MakeError(DOMAIN, TEXT("set_job_map"), 1001, TEXT("Invalid 'job_index'"));
    Queue->GetJobs()[Index]->Map = FSoftObjectPath(Path);
    return MakeSuccess(DOMAIN, TEXT("set_job_map"),
        FString::Printf(TEXT("Job %d map -> '%s'"), Index, *Path));
}

FBridgeResult UMovieRenderQueueHandler::Action_ListJobs(TSharedPtr<FJsonObject> Params)
{
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("list_jobs"), 3000, TEXT("MRQ subsystem unavailable"));
    UMoviePipelineQueue* Queue = QS->GetQueue();
    if (!Queue) return MakeError(DOMAIN, TEXT("list_jobs"), 3000, TEXT("Active queue unavailable"));

    TArray<TSharedPtr<FJsonValue>> Arr;
    int32 Idx = 0;
    for (UMoviePipelineExecutorJob* Job : Queue->GetJobs())
    {
        if (!Job) { ++Idx; continue; }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Idx);
        Entry->SetStringField(TEXT("job_name"), Job->JobName);
        Entry->SetStringField(TEXT("sequence_path"), Job->Sequence.ToString());
        Entry->SetStringField(TEXT("map_path"), Job->Map.ToString());
        Entry->SetBoolField(TEXT("is_consumed"), Job->IsConsumed());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        ++Idx;
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("jobs"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_jobs"),
        FString::Printf(TEXT("%d job(s)"), Arr.Num()), Data);
}

FBridgeResult UMovieRenderQueueHandler::Action_RemoveJob(TSharedPtr<FJsonObject> Params)
{
    int32 Index = -1;
    Params->TryGetNumberField(TEXT("job_index"), Index);
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("remove_job"), 3000, TEXT("MRQ subsystem unavailable"));
    UMoviePipelineQueue* Queue = QS->GetQueue();
    if (!Queue || Index < 0 || Index >= Queue->GetJobs().Num())
        return MakeError(DOMAIN, TEXT("remove_job"), 1001, TEXT("Invalid 'job_index'"));
    UMoviePipelineExecutorJob* Job = Queue->GetJobs()[Index];
    Queue->DeleteJob(Job);
    return MakeSuccess(DOMAIN, TEXT("remove_job"),
        FString::Printf(TEXT("Job %d removed"), Index));
}

FBridgeResult UMovieRenderQueueHandler::Action_RenderQueue(TSharedPtr<FJsonObject> Params)
{
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("render_queue"), 3000, TEXT("MRQ subsystem unavailable"));

    if (QS->IsRendering())
        return MakeError(DOMAIN, TEXT("render_queue"), 3000, TEXT("Already rendering"));

    FString ExecutorClassName;
    Params->TryGetStringField(TEXT("executor_class"), ExecutorClassName);

    TSubclassOf<UMoviePipelineExecutorBase> ExecClass = UMoviePipelinePIEExecutor::StaticClass();
    if (!ExecutorClassName.IsEmpty() && ExecutorClassName != TEXT("PIE"))
    {
        UClass* Found = LoadClass<UMoviePipelineExecutorBase>(nullptr, *ExecutorClassName);
        if (Found) ExecClass = Found;
    }

    UMoviePipelineExecutorBase* Exec = QS->RenderQueueWithExecutor(ExecClass);
    if (!Exec)
        return MakeError(DOMAIN, TEXT("render_queue"), 3000,
            TEXT("RenderQueueWithExecutor returned null"));

    return MakeSuccess(DOMAIN, TEXT("render_queue"),
        FString::Printf(TEXT("Render started (executor=%s)"), *ExecClass->GetName()));
}

FBridgeResult UMovieRenderQueueHandler::Action_IsRendering(TSharedPtr<FJsonObject> Params)
{
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("is_rendering"), 3000, TEXT("MRQ subsystem unavailable"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("rendering"), QS->IsRendering());
    return MakeSuccess(DOMAIN, TEXT("is_rendering"),
        QS->IsRendering() ? TEXT("rendering") : TEXT("idle"), Data);
}

FBridgeResult UMovieRenderQueueHandler::Action_CancelRender(TSharedPtr<FJsonObject> Params)
{
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("cancel_render"), 3000, TEXT("MRQ subsystem unavailable"));
    if (UMoviePipelineExecutorBase* Exec = QS->GetActiveExecutor())
    {
        Exec->CancelAllJobs();
        return MakeSuccess(DOMAIN, TEXT("cancel_render"), TEXT("Cancel requested"));
    }
    return MakeSuccess(DOMAIN, TEXT("cancel_render"), TEXT("No active render"));
}

FBridgeResult UMovieRenderQueueHandler::Action_GetRenderProgress(TSharedPtr<FJsonObject> Params)
{
    UMoviePipelineQueueSubsystem* QS = GetQS();
    if (!QS) return MakeError(DOMAIN, TEXT("get_render_progress"), 3000, TEXT("MRQ subsystem unavailable"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("rendering"), QS->IsRendering());
    if (UMoviePipelineQueue* Queue = QS->GetQueue())
    {
        int32 Total = 0, Consumed = 0;
        for (UMoviePipelineExecutorJob* Job : Queue->GetJobs())
        {
            if (!Job) continue;
            ++Total;
            if (Job->IsConsumed()) ++Consumed;
        }
        Data->SetNumberField(TEXT("total_jobs"), Total);
        Data->SetNumberField(TEXT("consumed_jobs"), Consumed);
        Data->SetNumberField(TEXT("progress"), Total > 0 ? (double)Consumed / (double)Total : 0.0);
    }
    return MakeSuccess(DOMAIN, TEXT("get_render_progress"), TEXT("Progress"), Data);
}
