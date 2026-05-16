#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "TransactionHandler.generated.h"

/**
 * TransactionHandler — domain "transaction"  (UE 5.7)
 *
 * Editor undo/redo control. Lets the AI roll back its own mistakes within
 * a session — the missing reversibility primitive.
 *
 * Actions:
 *   undo                          → count? (int, default 1) — pop N transactions
 *   redo                          → count? (int, default 1)
 *   get_undo_count                → number of undoable transactions
 *   get_redo_count                → number of redoable transactions
 *   get_last_transaction_title    → title of the topmost undo entry
 *   list_recent_transactions      → limit? (default 20) — recent undo titles
 *   clear_history                 → clear undo/redo stacks
 */
UCLASS()
class FORGEEDITORBRIDGE_API UTransactionHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("transaction"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("undo"), TEXT("redo"),
            TEXT("get_undo_count"), TEXT("get_redo_count"),
            TEXT("get_last_transaction_title"), TEXT("list_recent_transactions"),
            TEXT("clear_history")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_Undo                    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_Redo                    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetUndoCount            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetRedoCount            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetLastTransactionTitle (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListRecentTransactions  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearHistory            (TSharedPtr<FJsonObject> Params);
};