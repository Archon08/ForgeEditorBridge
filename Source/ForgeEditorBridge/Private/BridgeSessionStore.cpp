#include "BridgeSessionStore.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Misc/TimeSpan.h"

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

FBridgeSessionStore& FBridgeSessionStore::Get()
{
    static FBridgeSessionStore Instance;
    return Instance;
}

// ---------------------------------------------------------------------------
// CreateJob
// ---------------------------------------------------------------------------

FString FBridgeSessionStore::CreateJob(const FString& ActionName)
{
    FBridgeJob Job;
    Job.JobId      = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Job.ActionName = ActionName;
    Job.Status     = EBridgeJobStatus::Pending;
    Job.CreatedAt  = FDateTime::UtcNow();

    FScopeLock Lock(&JobsLock);
    Jobs.Add(Job.JobId, Job);
    return Job.JobId;
}

// ---------------------------------------------------------------------------
// UpdateJob
// ---------------------------------------------------------------------------

void FBridgeSessionStore::UpdateJob(const FString& JobId, EBridgeJobStatus Status,
                                     int32 Progress, const FString& LogLine)
{
    FScopeLock Lock(&JobsLock);
    FBridgeJob* Job = Jobs.Find(JobId);
    if (!Job) return;

    Job->Status = Status;
    if (Progress >= 0)
        Job->ProgressPercent = FMath::Clamp(Progress, 0, 100);
    if (!LogLine.IsEmpty())
    {
        Job->LogTail.Add(LogLine);
        if (Job->LogTail.Num() > MaxLogLines)
            Job->LogTail.RemoveAt(0, Job->LogTail.Num() - MaxLogLines, EAllowShrinking::No);
    }
}

// ---------------------------------------------------------------------------
// GetJob
// ---------------------------------------------------------------------------

TOptional<FBridgeJob> FBridgeSessionStore::GetJob(const FString& JobId)
{
    FScopeLock Lock(&JobsLock);
    FBridgeJob* Job = Jobs.Find(JobId);
    if (!Job) return TOptional<FBridgeJob>();
    return TOptional<FBridgeJob>(*Job);
}

// ---------------------------------------------------------------------------
// CancelJob
// ---------------------------------------------------------------------------

bool FBridgeSessionStore::CancelJob(const FString& JobId)
{
    FScopeLock Lock(&JobsLock);
    FBridgeJob* Job = Jobs.Find(JobId);
    if (!Job) return false;
    if (Job->Status != EBridgeJobStatus::Pending && Job->Status != EBridgeJobStatus::Running)
        return false;
    Job->Status = EBridgeJobStatus::Cancelled;
    return true;
}

// ---------------------------------------------------------------------------
// ListJobs
// ---------------------------------------------------------------------------

TArray<FBridgeJob> FBridgeSessionStore::ListJobs()
{
    PurgeExpired();

    FScopeLock Lock(&JobsLock);
    TArray<FBridgeJob> Out;
    Out.Reserve(Jobs.Num());
    for (const auto& Pair : Jobs)
        Out.Add(Pair.Value);
    return Out;
}

// ---------------------------------------------------------------------------
// PurgeExpired
// ---------------------------------------------------------------------------

void FBridgeSessionStore::PurgeExpired()
{
    const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromMinutes(TTLMinutes);
    FScopeLock Lock(&JobsLock);
    for (auto It = Jobs.CreateIterator(); It; ++It)
    {
        if (It->Value.CreatedAt < Cutoff)
            It.RemoveCurrent();
    }
}
