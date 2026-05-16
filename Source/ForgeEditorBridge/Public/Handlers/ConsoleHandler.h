#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ConsoleHandler.generated.h"

/**
 * ConsoleHandler — domain "console"  (v0.8.0 / UE 5.7)
 *
 * Exposes the engine's CVar / console command system through the Bridge.
 *
 * Actions:
 *   execute_command  → command (string)
 *                      Run an arbitrary console command via GEngine->Exec.
 *                      Result is fire-and-forget; success means the command was dispatched.
 *
 *   set_cvar         → name (string), value (string)
 *                      Set a console variable by exact name.
 *
 *   get_cvar         → name (string)
 *                      Return the CVar's current value string in ExtraData.
 *
 *   list_cvars       → prefix (string, optional — defaults to "" = all)
 *                      Return up to 200 CVar names matching the prefix as a JSON array in ExtraData.
 *
 *   list_registered_commands → (no params)
 *                      Enumerate all registered console commands (not variables).
 *                      Returns up to 500 command names as a JSON array in Data.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UConsoleHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("console"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("execute_command"), TEXT("set_cvar"), TEXT("get_cvar"), TEXT("list_cvars"), TEXT("list_registered_commands"), TEXT("execute_command_with_capture") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_ExecuteCommand(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetCVar       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetCVar       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListCVars     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListRegisteredCommands(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ExecuteCommandWithCapture(TSharedPtr<FJsonObject> Params);
};
