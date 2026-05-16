#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PythonHandler.generated.h"

/**
 * PythonHandler — domain "python"
 *
 * Execute Python scripts via soft dependency on PythonScriptPlugin.
 * Falls back to GEngine->Exec("py ...") if the module isn't loaded.
 * Do NOT add PythonScriptPlugin to Build.cs.
 *
 * Actions:
 *   execute_script    → script (inline Python code)
 *   execute_file      → file_path (absolute path to .py file)
 *   execute_with_args → script, variables { key: value } (template substitution)
 *   is_available      → returns availability and method (plugin vs fallback)
 *   exec_with_capture → script (executes via IPythonScriptPlugin::ExecPythonCommandEx, captures stdout)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPythonHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("python"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("execute_script"), TEXT("execute_file"), TEXT("execute_with_args"), TEXT("is_available"), TEXT("exec_with_capture") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	bool IsPythonPluginLoaded() const;
	void ExecuteViaPy(const FString& Command);
	FBridgeResult Action_ExecWithCapture(TSharedPtr<FJsonObject> Params);
};
