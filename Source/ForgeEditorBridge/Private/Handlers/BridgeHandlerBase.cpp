#include "Handlers/BridgeHandlerBase.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"

void UBridgeHandlerBase::Initialize(UForgeAISubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

FBridgeResult UBridgeHandlerBase::CreateResult(const FString& Domain, const FString& Action)
{
	FBridgeResult Result;
	Result.Domain = Domain;
	Result.Action = Action;
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();
	Result.bSuccess = false; // Default to false until explicitly set
	return Result;
}

FBridgeResult UBridgeHandlerBase::MakeError(const FString& Domain, const FString& Action,
	int32 ErrorCode, const FString& Message, const FString& RecoveryHint)
{
	FBridgeResult Result;
	Result.Domain = Domain;
	Result.Action = Action;
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();
	Result.bSuccess = false;
	Result.ErrorCode = ErrorCode;
	Result.Message = Message;
	Result.RecoveryHint = RecoveryHint;
	return Result;
}

FBridgeResult UBridgeHandlerBase::MakeSuccess(const FString& Domain, const FString& Action,
	const FString& Message, TSharedPtr<FJsonObject> Data)
{
	FBridgeResult Result;
	Result.Domain = Domain;
	Result.Action = Action;
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();
	Result.bSuccess = true;
	Result.Message = Message;
	Result.Data = Data;
	return Result;
}

TUniquePtr<FScopedTransaction> UBridgeHandlerBase::BeginTransaction(const FString& Description)
{
#if WITH_EDITOR
	return MakeUnique<FScopedTransaction>(FText::FromString(Description));
#else
	return nullptr;
#endif
}

FBridgeResult UBridgeHandlerBase::MakeUnknownAction(const FString& Domain, const FString& Action,
	const FString& ValidActionsCsv)
{
	return MakeError(Domain, Action, 1001,
		FString::Printf(TEXT("Unknown %s action '%s'"), *Domain, *Action),
		FString::Printf(TEXT("Valid: %s"), *ValidActionsCsv));
}

UWorld* UBridgeHandlerBase::GuardPIE(const FString& Domain, const FString& Action,
	FBridgeResult& OutResult)
{
#if WITH_EDITOR
	if (GEditor)
	{
		for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
				return Ctx.World();
		}
	}
#endif
	OutResult = MakeError(Domain, Action, 3004,
		FString::Printf(TEXT("%s requires Play In Editor — no PIE world is running"), *Action),
		TEXT("Press Play in the editor, then retry the action"));
	return nullptr;
}
