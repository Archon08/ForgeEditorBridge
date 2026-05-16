#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "GitHandler.generated.h"

/**
 * GitHandler — domain "git"  (v0.4.0)
 *
 * Runs git commands in {ProjectDir} via FPlatformProcess::CreateProc and captures stdout.
 *
 * Safety constraints:
 *   - git_revert_file only accepts a single relative file path
 *     (no globs, no flags beginning with '-', no '..' traversal)
 *   - git push and git reset are explicitly blocked — not exposed as actions
 *
 * Actions:
 *   git_status       — (no params)                   → ExtraData: stdout from `git status`
 *   git_diff         — file? (optional rel path)     → ExtraData: diff output
 *   git_stage        — path                          → stages one file or directory
 *   git_commit       — message                       → commits staged changes
 *   git_log          — count? (default 20, max 200)  → ExtraData: `git log --oneline -n N`
 *   git_branch       — (no params)                   → ExtraData: `git branch -v`
 *   git_revert_file  — path (single file only)       → `git checkout -- <file>`
 */
UCLASS()
class FORGEEDITORBRIDGE_API UGitHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()
public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("git"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("git_status"), TEXT("git_diff"), TEXT("git_stage"), TEXT("git_commit"), TEXT("git_log"), TEXT("git_branch"), TEXT("git_revert_file") }; }
	virtual FBridgeResult HandleCommand(const FString& Action,
	                                    TSharedPtr<FJsonObject> Params) override;
private:
	FBridgeResult Action_GitStatus     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitDiff       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitStage      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitCommit     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitLog        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitBranch     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GitRevertFile (TSharedPtr<FJsonObject> Params);

	/**
	 * Runs `git <Args>` synchronously in {ProjectDir}.
	 * Returns true if the process launched successfully.
	 * OutOutput receives combined stdout. OutExitCode is the process exit code.
	 */
	bool RunGit(const FString& Args, FString& OutOutput, int32& OutExitCode) const;
};
