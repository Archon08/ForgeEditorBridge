#include "Handlers/GitHandler.h"
#include "ForgeAISubsystem.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Math/UnrealMathUtility.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UGitHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("git"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("git_status"))      return Action_GitStatus(Params);
	if (Action == TEXT("git_diff"))        return Action_GitDiff(Params);
	if (Action == TEXT("git_stage"))       return Action_GitStage(Params);
	if (Action == TEXT("git_commit"))      return Action_GitCommit(Params);
	if (Action == TEXT("git_log"))         return Action_GitLog(Params);
	if (Action == TEXT("git_branch"))      return Action_GitBranch(Params);
	if (Action == TEXT("git_revert_file")) return Action_GitRevertFile(Params);

	FBridgeResult R = CreateResult(TEXT("git"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown git action '%s'. Valid: git_status, git_diff, git_stage, git_commit, git_log, git_branch, git_revert_file"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// RunGit — synchronous helper
// ---------------------------------------------------------------------------

bool UGitHandler::RunGit(const FString& Args, FString& OutOutput, int32& OutExitCode) const
{
	void* PipeRead  = nullptr;
	void* PipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

	// Working directory — strip trailing separator for CreateProc on Windows
	FString WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	while (WorkDir.EndsWith(TEXT("/")) || WorkDir.EndsWith(TEXT("\\")))
		WorkDir.LeftChopInline(1);

	FProcHandle Proc = FPlatformProcess::CreateProc(
		TEXT("git"),   // must be on PATH
		*Args,
		false,         // bLaunchDetached
		true,          // bLaunchHidden
		true,          // bLaunchReallyHidden
		nullptr,       // OutProcessID
		0,             // PriorityModifier
		*WorkDir,
		PipeWrite,     // child stdout → this pipe
		nullptr        // child stdin
	);

	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutOutput   = TEXT("Failed to launch git process. Ensure git is on the system PATH.");
		OutExitCode = -1;
		return false;
	}

	// Drain stdout while process runs, then do a final drain after exit
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		OutOutput += FPlatformProcess::ReadPipe(PipeRead);
		FPlatformProcess::Sleep(0.05f);
	}
	OutOutput += FPlatformProcess::ReadPipe(PipeRead);

	FPlatformProcess::GetProcReturnCode(Proc, &OutExitCode);
	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
	return true;
}

// ---------------------------------------------------------------------------
// git_status
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitStatus(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_status"));
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(TEXT("status"), Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess  = (ExitCode == 0);
	Result.ExtraData = Output;
	Result.Message   = Result.bSuccess
		? TEXT("git status OK")
		: FString::Printf(TEXT("git status failed (exit %d)"), ExitCode);
	return Result;
}

// ---------------------------------------------------------------------------
// git_diff
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitDiff(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_diff"));
	FString File;
	Params->TryGetStringField(TEXT("file"), File);

	FString Args = TEXT("diff");
	if (!File.IsEmpty())
		Args += TEXT(" -- ") + File;

	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(Args, Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess  = (ExitCode == 0);
	Result.ExtraData = Output;
	Result.Message   = Result.bSuccess
		? TEXT("git diff OK")
		: FString::Printf(TEXT("git diff failed (exit %d)"), ExitCode);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// git_stage
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitStage(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_stage"));
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		Result.Message = TEXT("git_stage: 'path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString Args   = TEXT("add -- ") + Path;
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(Args, Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess     = (ExitCode == 0);
	Result.AffectedPath = Path;
	Result.ExtraData    = Output;
	Result.Message      = Result.bSuccess
		? FString::Printf(TEXT("Staged: %s"), *Path)
		: FString::Printf(TEXT("git add failed (exit %d): %s"), ExitCode, *Output);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// git_commit
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitCommit(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_commit"));
	FString Message;
	if (!Params->TryGetStringField(TEXT("message"), Message) || Message.IsEmpty())
	{
		Result.Message = TEXT("git_commit: 'message' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Sanitise: strip embedded double quotes and newlines to prevent arg injection
	Message.ReplaceInline(TEXT("\""), TEXT("'"));
	Message.ReplaceInline(TEXT("\n"), TEXT(" "));
	Message.ReplaceInline(TEXT("\r"), TEXT(" "));

	FString Args   = FString::Printf(TEXT("commit -m \"%s\""), *Message);
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(Args, Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess  = (ExitCode == 0);
	Result.ExtraData = Output;
	Result.Message   = Result.bSuccess
		? FString::Printf(TEXT("Committed: %s"), *Message)
		: FString::Printf(TEXT("git commit failed (exit %d): %s"), ExitCode, *Output);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// git_log
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitLog(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_log"));
	double CountNum = 20.0;
	Params->TryGetNumberField(TEXT("count"), CountNum);
	const int32 Count = FMath::Clamp((int32)CountNum, 1, 200);

	FString Args   = FString::Printf(TEXT("log --oneline -n %d"), Count);
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(Args, Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess  = (ExitCode == 0);
	Result.ExtraData = Output;
	Result.Message   = Result.bSuccess
		? FString::Printf(TEXT("git log OK (%d entries)"), Count)
		: FString::Printf(TEXT("git log failed (exit %d)"), ExitCode);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// git_branch
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitBranch(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_branch"));
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(TEXT("branch -v"), Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess  = (ExitCode == 0);
	Result.ExtraData = Output;
	Result.Message   = Result.bSuccess
		? TEXT("git branch OK")
		: FString::Printf(TEXT("git branch failed (exit %d)"), ExitCode);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}

// ---------------------------------------------------------------------------
// git_revert_file — SAFETY: single file only, no globs, no flags, no traversal
// ---------------------------------------------------------------------------

FBridgeResult UGitHandler::Action_GitRevertFile(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("git"), TEXT("git_revert_file"));
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("path"), FilePath) || FilePath.IsEmpty())
	{
		Result.Message = TEXT("git_revert_file: 'path' is required (single relative file path)");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Safety: reject glob chars, flag prefixes, and path traversal
	if (FilePath.Contains(TEXT("..")) ||
		FilePath.Contains(TEXT("*"))  ||
		FilePath.Contains(TEXT("?"))  ||
		FilePath.StartsWith(TEXT("-")))
	{
		Result.Message = FString::Printf(
			TEXT("git_revert_file: rejected unsafe path '%s'. "
			     "Provide a single relative file path with no globs or flags."),
			*FilePath);
		Result.ErrorCode = 1000;
		return Result;
	}

	// `git checkout -- <file>` discards working-tree modifications; cannot be used for push/reset
	FString Args   = FString::Printf(TEXT("checkout -- %s"), *FilePath);
	FString Output;
	int32   ExitCode = 0;
	if (!RunGit(Args, Output, ExitCode))
	{
		Result.Message = Output;
		Result.ErrorCode = 3000;
		return Result;
	}
	Result.bSuccess     = (ExitCode == 0);
	Result.AffectedPath = FilePath;
	Result.ExtraData    = Output;
	Result.Message      = Result.bSuccess
		? FString::Printf(TEXT("Reverted: %s"), *FilePath)
		: FString::Printf(TEXT("git checkout failed (exit %d): %s"), ExitCode, *Output);
	if (!Result.bSuccess) Result.ErrorCode = 3000;
	return Result;
}
