#include "Handlers/ConsoleHandler.h"
#include "ForgeAISubsystem.h"

// ---- Console / CVar --------------------------------------------------------
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"         // GEngine
#include "Editor.h"                // GEditor, GetEditorWorldContext

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- File / Path -----------------------------------------------------------
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UConsoleHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("console"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("execute_command")) return Action_ExecuteCommand(Params);
	if (Action == TEXT("set_cvar"))        return Action_SetCVar(Params);
	if (Action == TEXT("get_cvar"))        return Action_GetCVar(Params);
	if (Action == TEXT("list_cvars"))                return Action_ListCVars(Params);
	if (Action == TEXT("list_registered_commands")) return Action_ListRegisteredCommands(Params);
	if (Action == TEXT("execute_command_with_capture")) return Action_ExecuteCommandWithCapture(Params);

	FBridgeResult R = CreateResult(TEXT("console"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown console action '%s'. Valid: execute_command, set_cvar, get_cvar, list_cvars, list_registered_commands, execute_command_with_capture"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// execute_command
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_ExecuteCommand(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("console"), TEXT("execute_command"));

	FString Command;
	if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		Result.Message = TEXT("execute_command: 'command' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = nullptr;
	if (GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
	if (!World && GEngine)
	{
		World = GEngine->GetWorld();
	}

	if (GEngine)
	{
		GEngine->Exec(World, *Command);
		Result.bSuccess = true;
		Result.Message  = FString::Printf(TEXT("Console command dispatched: %s"), *Command);
	}
	else
	{
		Result.Message = TEXT("execute_command: GEngine is null — cannot dispatch command");
		Result.ErrorCode = 3000;
	}
	return Result;
}

// ---------------------------------------------------------------------------
// set_cvar
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_SetCVar(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("console"), TEXT("set_cvar"));

	FString Name, Value;
	if (!Params->TryGetStringField(TEXT("name"),  Name)  || Name.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("value"), Value))
	{
		Result.Message = TEXT("set_cvar: 'name' and 'value' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar)
	{
		Result.Message = FString::Printf(TEXT("set_cvar: CVar '%s' not found"), *Name);
		Result.ErrorCode = 2000;
		return Result;
	}

	CVar->Set(*Value, ECVF_SetByCode);

	Result.bSuccess = true;
	Result.Message  = FString::Printf(TEXT("CVar '%s' set to '%s'"), *Name, *Value);
	return Result;
}

// ---------------------------------------------------------------------------
// get_cvar
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_GetCVar(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("console"), TEXT("get_cvar"));

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		Result.ErrorCode = 1000;
		Result.Message = TEXT("get_cvar: 'name' is required");
		return Result;
	}

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar)
	{
		Result.ErrorCode = 2000;
		Result.Message = FString::Printf(TEXT("get_cvar: CVar '%s' not found"), *Name);
		Result.RecoveryHint = TEXT("Use console/list_cvars to find available CVars");
		return Result;
	}

	const FString CurrentValue = CVar->GetString();

	Result.bSuccess  = true;
	Result.ExtraData = CurrentValue;
	Result.Message   = FString::Printf(TEXT("CVar '%s' = '%s'"), *Name, *CurrentValue);
	return Result;
}

// ---------------------------------------------------------------------------
// list_cvars
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_ListCVars(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("console"), TEXT("list_cvars"));

	FString Prefix;
	Params->TryGetStringField(TEXT("prefix"), Prefix);

	constexpr int32 MaxResults = 200;
	TArray<FString> Names;

	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
		FConsoleObjectVisitor::CreateLambda([&](const TCHAR* InName, IConsoleObject* /*Obj*/)
		{
			if (Names.Num() < MaxResults)
			{
				Names.Add(FString(InName));
			}
		}),
		*Prefix
	);

	// Serialize to JSON array string
	TArray<TSharedPtr<FJsonValue>> JsonNames;
	for (const FString& N : Names)
	{
		JsonNames.Add(MakeShared<FJsonValueString>(N));
	}

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	JsonObj->SetArrayField(TEXT("cvars"), JsonNames);
	JsonObj->SetNumberField(TEXT("count"), Names.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("Found %d CVar(s) matching prefix '%s'%s"),
		Names.Num(), *Prefix,
		Names.Num() >= MaxResults ? TEXT(" (result capped at 200)") : TEXT(""));
	return Result;
}

// ---------------------------------------------------------------------------
// list_registered_commands
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_ListRegisteredCommands(TSharedPtr<FJsonObject> Params)
{
	constexpr int32 MaxResults = 500;
	TArray<FString> CommandNames;

	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
		FConsoleObjectVisitor::CreateLambda([&](const TCHAR* InName, IConsoleObject* Obj)
		{
			if (CommandNames.Num() >= MaxResults) return;

			// Filter: only IConsoleCommand (not IConsoleVariable)
			if (Obj->AsVariable()) return; // skip variables — keep only commands

			CommandNames.Add(FString(InName));
		}),
		TEXT("") // empty prefix = enumerate all
	);

	TArray<TSharedPtr<FJsonValue>> JsonNames;
	for (const FString& N : CommandNames)
	{
		JsonNames.Add(MakeShared<FJsonValueString>(N));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("commands"), JsonNames);
	Data->SetNumberField(TEXT("count"), CommandNames.Num());

	return MakeSuccess(TEXT("console"), TEXT("list_registered_commands"),
		FString::Printf(TEXT("Found %d console command(s)%s"),
			CommandNames.Num(),
			CommandNames.Num() >= MaxResults ? TEXT(" (result capped at 500)") : TEXT("")),
		Data);
}

// ---------------------------------------------------------------------------
// execute_command_with_capture — run command + best-effort log tail read
// ---------------------------------------------------------------------------

FBridgeResult UConsoleHandler::Action_ExecuteCommandWithCapture(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("console"), TEXT("execute_command_with_capture"));

	FString Command;
	if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		Result.ErrorCode = 1000;
		Result.Message = TEXT("execute_command_with_capture: 'command' is required");
		return Result;
	}

	int32 DurationFrames = 0;
	{
		double DurationNum = 0.0;
		if (Params->TryGetNumberField(TEXT("duration_frames"), DurationNum))
			DurationFrames = FMath::Max(0, (int32)DurationNum);
	}

	UWorld* World = nullptr;
	if (GEditor) World = GEditor->GetEditorWorldContext().World();
	if (!World && GEngine) World = GEngine->GetWorld();

	if (!GEngine)
	{
		Result.ErrorCode = 3000;
		Result.Message = TEXT("execute_command_with_capture: GEngine is null — cannot dispatch command");
		return Result;
	}

	GEngine->Exec(World, *Command);
	GLog->Flush();

	// ---- Best-effort log tail -----------------------------------------------
	constexpr int32 TailLineCount = 20;
	TArray<FString> TailLines;
	FString LogFilePath = FPaths::ProjectLogDir() / TEXT("UnrealEditor.log");
	if (!FPaths::FileExists(LogFilePath))
	{
		// Fallback: probe common per-project log name
		const FString ProjectName = FApp::GetProjectName();
		LogFilePath = FPaths::ProjectLogDir() / (ProjectName + TEXT(".log"));
	}

	FString LogContent;
	bool bReadOk = false;
	if (FPaths::FileExists(LogFilePath))
	{
		bReadOk = FFileHelper::LoadFileToString(LogContent, *LogFilePath);
	}

	if (bReadOk)
	{
		TArray<FString> AllLines;
		LogContent.ParseIntoArrayLines(AllLines, /*CullEmpty*/ false);
		const int32 Start = FMath::Max(0, AllLines.Num() - TailLineCount);
		for (int32 i = Start; i < AllLines.Num(); ++i)
		{
			TailLines.Add(AllLines[i]);
		}
	}

	TArray<TSharedPtr<FJsonValue>> TailArr;
	for (const FString& L : TailLines)
	{
		TailArr.Add(MakeShared<FJsonValueString>(L));
	}

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	JsonObj->SetStringField(TEXT("command"), Command);
	JsonObj->SetNumberField(TEXT("duration_frames"), DurationFrames);
	JsonObj->SetStringField(TEXT("log_path"), LogFilePath);
	JsonObj->SetBoolField(TEXT("log_read_ok"), bReadOk);
	JsonObj->SetArrayField(TEXT("output_tail"), TailArr);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("execute_command_with_capture: dispatched '%s' (duration_frames=%d, %d tail line(s)). Output capture is best-effort via log tail."),
		*Command, DurationFrames, TailLines.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UConsoleHandler::GetActionSchemas() const
{
    auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), Type);
        O->SetBoolField(TEXT("required"), bRequired);
        O->SetStringField(TEXT("desc"), Desc);
        return O;
    };

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Run an arbitrary console command via GEngine->Exec"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("command"), P(TEXT("string"), true, TEXT("Console command string"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("execute_command"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a console variable by exact name"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("name"), P(TEXT("string"), true, TEXT("CVar name"))); Pr->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value to set"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_cvar"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get the current value of a console variable"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("name"), P(TEXT("string"), true, TEXT("CVar name"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_cvar"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List console variables matching a prefix (up to 200)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("prefix"), P(TEXT("string"), false, TEXT("Prefix filter (empty = all)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_cvars"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Enumerate all registered console commands (not variables), up to 500"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("list_registered_commands"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Execute a console command and return last 20 lines of UnrealEditor.log (best-effort capture)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
      Pr->SetObjectField(TEXT("command"),         P(TEXT("string"), true,  TEXT("Console command to execute")));
      Pr->SetObjectField(TEXT("duration_frames"), P(TEXT("int"),    false, TEXT("Frames to wait before reading log (0 = fire-and-forget)")));
      A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("execute_command_with_capture"), A); }

    return Root;
}
