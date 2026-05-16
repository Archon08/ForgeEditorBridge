#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Optional.h"
#include "HAL/CriticalSection.h"

/**
 * EBridgeJobStatus — lifecycle states for async bridge jobs.
 * NOTE: plain enum class (not UENUM) — FBridgeSessionStore is not a UObject.
 */
enum class EBridgeJobStatus : uint8
{
    Pending,
    Running,
    Success,
    Failed,
    Cancelled,
};

/** Convert EBridgeJobStatus to a display string. */
inline FString BridgeJobStatusToString(EBridgeJobStatus S)
{
    switch (S)
    {
    case EBridgeJobStatus::Pending:   return TEXT("Pending");
    case EBridgeJobStatus::Running:   return TEXT("Running");
    case EBridgeJobStatus::Success:   return TEXT("Success");
    case EBridgeJobStatus::Failed:    return TEXT("Failed");
    case EBridgeJobStatus::Cancelled: return TEXT("Cancelled");
    default: return TEXT("Unknown");
    }
}

/** A single tracked async job. */
struct FORGEEDITORBRIDGE_API FBridgeJob
{
    FString         JobId;
    FString         ActionName;
    EBridgeJobStatus Status         = EBridgeJobStatus::Pending;
    int32           ProgressPercent = 0;
    TArray<FString> LogTail;                // rolling last-N log lines
    FDateTime       CreatedAt;
    FString         ResultMessage;          // final success/failure message
};

/**
 * FBridgeSessionStore — thread-safe singleton job store.
 *
 * Handlers create a job (getting back a GUID job_id), kick off async work on
 * a background thread, call UpdateJob as progress arrives, then mark Success/Failed.
 * Jobs older than TTLMinutes (30) are purged on each ListJobs / PurgeExpired call.
 *
 * Usage:
 *   FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("package_project"));
 *   FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0);
 *   // ... work ...
 *   FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Success, 100, TEXT("Done"));
 */
class FORGEEDITORBRIDGE_API FBridgeSessionStore
{
public:
    /** Thread-safe singleton accessor. */
    static FBridgeSessionStore& Get();

    /** Create a new job entry. Returns the new GUID job_id string. */
    FString CreateJob(const FString& ActionName = TEXT(""));

    /**
     * Update a job's status and optionally its progress and log.
     * Pass Progress=-1 to leave the current value unchanged.
     * Pass an empty LogLine to skip appending.
     * Keeps the rolling log capped at MaxLogLines.
     */
    void UpdateJob(const FString& JobId, EBridgeJobStatus Status,
                   int32 Progress = -1, const FString& LogLine = TEXT(""));

    /** Returns a copy of the job, or TOptional empty if not found / expired. */
    TOptional<FBridgeJob> GetJob(const FString& JobId);

    /**
     * Cancel a Pending or Running job.
     * Returns false if the job is not found or is already terminal.
     */
    bool CancelJob(const FString& JobId);

    /** Return copies of all non-expired jobs (calls PurgeExpired first). */
    TArray<FBridgeJob> ListJobs();

    /** Remove all jobs older than TTLMinutes. Call periodically. */
    void PurgeExpired();

private:
    FBridgeSessionStore() = default;
    ~FBridgeSessionStore() = default;

    // Non-copyable, non-movable
    FBridgeSessionStore(const FBridgeSessionStore&) = delete;
    FBridgeSessionStore& operator=(const FBridgeSessionStore&) = delete;

    TMap<FString, FBridgeJob> Jobs;
    mutable FCriticalSection  JobsLock;

    static constexpr int32 MaxLogLines  = 50;
    static constexpr float TTLMinutes   = 30.f;
};
