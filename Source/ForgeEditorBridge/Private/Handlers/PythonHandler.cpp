#include "Handlers/PythonHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Engine/Engine.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

// SOFT dependency — PythonScriptPlugin is NOT in Build.cs.
// Use FModuleManager::GetModulePtr (never LoadModule) so absence is graceful.
#if __has_include("IPythonScriptPlugin.h")
#include "IPythonScriptPlugin.h"
#define FORGE_HAVE_PYTHON_PLUGIN_HEADER 1
#else
#define FORGE_HAVE_PYTHON_PLUGIN_HEADER 0
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("python");

bool UPythonHandler::IsPythonPluginLoaded() const
{
	static const FName ModuleName("PythonScriptPlugin");
	return FModuleManager::Get().IsModuleLoaded(ModuleName);
}

void UPythonHandler::ExecuteViaPy(const FString& Command)
{
#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
#else
	UWorld* World = nullptr;
#endif
	FString PyCmd = FString::Printf(TEXT("py %s"), *Command);
	GEngine->Exec(World, *PyCmd);
}

FBridgeResult UPythonHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- execute_script ----
	if (Action == TEXT("execute_script"))
	{
		FString Script;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("script"), Script))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'script'"));

		ExecuteViaPy(Script);

		FString Method = IsPythonPluginLoaded() ? TEXT("PythonScriptPlugin") : TEXT("GEngine_Exec");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("method"), Method);

		return MakeSuccess(DOMAIN, Action,
			IsPythonPluginLoaded()
				? TEXT("Script executed via Python plugin")
				: TEXT("Script dispatched via GEngine::Exec (no output capture)"),
			Data);
	}

	// ---- execute_file ----
	if (Action == TEXT("execute_file"))
	{
		FString FilePath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("file_path"), FilePath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'file_path'"));

		if (!FPaths::FileExists(FilePath))
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Python file not found: %s"), *FilePath));

		// py command with quoted path for file execution
#if WITH_EDITOR
		UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
#else
		UWorld* World = nullptr;
#endif
		FString PyCmd = FString::Printf(TEXT("py \"%s\""), *FilePath);
		GEngine->Exec(World, *PyCmd);

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Executed file: %s"), *FilePath));
		R.AffectedPath = FilePath;
		return R;
	}

	// ---- execute_with_args ----
	if (Action == TEXT("execute_with_args"))
	{
		FString Script;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("script"), Script))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'script'"));

		// Template substitution: {key} → value
		const TSharedPtr<FJsonObject>* Variables;
		if (Params->TryGetObjectField(TEXT("variables"), Variables))
		{
			for (const auto& Pair : (*Variables)->Values)
			{
				FString Value;
				if (Pair.Value->TryGetString(Value))
				{
					Script = Script.Replace(*FString::Printf(TEXT("{%s}"), *Pair.Key), *Value);
				}
			}
		}

		ExecuteViaPy(Script);
		return MakeSuccess(DOMAIN, Action, TEXT("Template-resolved script executed"));
	}

	// ---- is_available ----
	if (Action == TEXT("is_available"))
	{
		bool bPlugin = IsPythonPluginLoaded();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("available"), true);  // GEngine::Exec always works
		Data->SetStringField(TEXT("method"), bPlugin ? TEXT("PythonScriptPlugin + GEngine_Exec") : TEXT("GEngine_Exec"));

		if (!bPlugin)
		{
			Data->SetStringField(TEXT("note"),
				TEXT("PythonScriptPlugin module not loaded. Enable 'Python Editor Script Plugin' in Edit > Plugins for full output capture."));
		}

		return MakeSuccess(DOMAIN, Action,
			bPlugin ? TEXT("Python available (full)") : TEXT("Python available (fallback)"), Data);
	}

	// ---- exec_with_capture ----
	if (Action == TEXT("exec_with_capture"))
	{
		return Action_ExecWithCapture(Params);
	}

	return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("system/capabilities"));
}

// ---------------------------------------------------------------------------
// exec_with_capture — IPythonScriptPlugin::ExecPythonCommandEx with stdout capture
// ---------------------------------------------------------------------------

FBridgeResult UPythonHandler::Action_ExecWithCapture(TSharedPtr<FJsonObject> Params)
{
	FString Script;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("script"), Script))
		return MakeError(DOMAIN, TEXT("exec_with_capture"), 1000, TEXT("Missing required param: 'script'"));

#if FORGE_HAVE_PYTHON_PLUGIN_HEADER
	IPythonScriptPlugin* PythonPlugin = FModuleManager::GetModulePtr<IPythonScriptPlugin>("PythonScriptPlugin");
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		// Fallback: dispatch via GEngine->Exec (no capture) and report.
		ExecuteViaPy(Script);
		FBridgeResult R = MakeError(DOMAIN, TEXT("exec_with_capture"), 3003,
			TEXT("exec_with_capture: PythonScriptPlugin not available, output capture unavailable"),
			TEXT("Enable 'Python Editor Script Plugin' in Edit > Plugins for output capture."));
		R.Message = TEXT("exec_with_capture: PythonScriptPlugin not available, output capture unavailable");
		return R;
	}

	FPythonCommandEx CmdEx;
	CmdEx.Command            = Script;
	CmdEx.ExecutionMode      = EPythonCommandExecutionMode::ExecuteStatement;
	CmdEx.FileExecutionScope = EPythonFileExecutionScope::Public;

	const bool bOk = PythonPlugin->ExecPythonCommandEx(CmdEx);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("stdout_output"), CmdEx.CommandResult);
	Data->SetStringField(TEXT("stderr_output"), TEXT(""));  // ExecPythonCommandEx merges into log; no separate stderr capture
	Data->SetBoolField  (TEXT("success"),       bOk);
	Data->SetStringField(TEXT("method"),        TEXT("IPythonScriptPlugin::ExecPythonCommandEx"));

	if (bOk)
	{
		return MakeSuccess(DOMAIN, TEXT("exec_with_capture"),
			TEXT("Python executed with output capture"), Data);
	}
	FBridgeResult R = MakeError(DOMAIN, TEXT("exec_with_capture"), 3000, TEXT("Python execution failed"));
	R.Data = Data;
	return R;
#else
	// Header not reachable in this build — module is a soft dep, degrade to Exec fallback.
	ExecuteViaPy(Script);
	return MakeError(DOMAIN, TEXT("exec_with_capture"), 3003,
		TEXT("exec_with_capture: IPythonScriptPlugin.h not available in this build, output capture unavailable"),
		TEXT("Enable 'Python Editor Script Plugin' and rebuild the ForgeEditorBridge module."));
#endif
}
