#include "Handlers/TransactionHandler.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transactor.h"
#include "Editor/TransBuffer.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("transaction");

namespace
{
    UTransBuffer* GetTransBuffer()
    {
        if (!GEditor) return nullptr;
        return Cast<UTransBuffer>(GEditor->Trans);
    }
}

FBridgeResult UTransactionHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("undo"))                       return Action_Undo(Params);
    if (Action == TEXT("redo"))                       return Action_Redo(Params);
    if (Action == TEXT("get_undo_count"))             return Action_GetUndoCount(Params);
    if (Action == TEXT("get_redo_count"))             return Action_GetRedoCount(Params);
    if (Action == TEXT("get_last_transaction_title")) return Action_GetLastTransactionTitle(Params);
    if (Action == TEXT("list_recent_transactions"))   return Action_ListRecentTransactions(Params);
    if (Action == TEXT("clear_history"))              return Action_ClearHistory(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("undo, redo, get_undo_count, get_redo_count, get_last_transaction_title, list_recent_transactions, clear_history"));
}

FBridgeResult UTransactionHandler::Action_Undo(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor) return MakeError(DOMAIN, TEXT("undo"), 3000, TEXT("GEditor not available"));
    int32 Count = 1;
    Params->TryGetNumberField(TEXT("count"), Count);
    Count = FMath::Max(1, Count);
    int32 Done = 0;
    for (int32 i = 0; i < Count; ++i)
    {
        if (GEditor->UndoTransaction()) ++Done; else break;
    }
    return MakeSuccess(DOMAIN, TEXT("undo"),
        FString::Printf(TEXT("Undid %d transaction(s)"), Done));
}

FBridgeResult UTransactionHandler::Action_Redo(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor) return MakeError(DOMAIN, TEXT("redo"), 3000, TEXT("GEditor not available"));
    int32 Count = 1;
    Params->TryGetNumberField(TEXT("count"), Count);
    Count = FMath::Max(1, Count);
    int32 Done = 0;
    for (int32 i = 0; i < Count; ++i)
    {
        if (GEditor->RedoTransaction()) ++Done; else break;
    }
    return MakeSuccess(DOMAIN, TEXT("redo"),
        FString::Printf(TEXT("Redid %d transaction(s)"), Done));
}

FBridgeResult UTransactionHandler::Action_GetUndoCount(TSharedPtr<FJsonObject> Params)
{
    UTransBuffer* TB = GetTransBuffer();
    if (!TB) return MakeError(DOMAIN, TEXT("get_undo_count"), 3000, TEXT("Transaction buffer unavailable"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("undo_count"), TB->GetQueueLength() - TB->GetUndoCount());
    return MakeSuccess(DOMAIN, TEXT("get_undo_count"), TEXT("OK"), Data);
}

FBridgeResult UTransactionHandler::Action_GetRedoCount(TSharedPtr<FJsonObject> Params)
{
    UTransBuffer* TB = GetTransBuffer();
    if (!TB) return MakeError(DOMAIN, TEXT("get_redo_count"), 3000, TEXT("Transaction buffer unavailable"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("redo_count"), TB->GetUndoCount());
    return MakeSuccess(DOMAIN, TEXT("get_redo_count"), TEXT("OK"), Data);
}

FBridgeResult UTransactionHandler::Action_GetLastTransactionTitle(TSharedPtr<FJsonObject> Params)
{
    UTransBuffer* TB = GetTransBuffer();
    if (!TB || TB->GetQueueLength() == 0)
    {
        return MakeSuccess(DOMAIN, TEXT("get_last_transaction_title"),
            TEXT("No transactions in buffer"));
    }
    const FTransactionContext Ctx = TB->GetUndoContext(false);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("title"), Ctx.Title.ToString());
    Data->SetStringField(TEXT("context"), Ctx.Context);
    return MakeSuccess(DOMAIN, TEXT("get_last_transaction_title"), Ctx.Title.ToString(), Data);
}

FBridgeResult UTransactionHandler::Action_ListRecentTransactions(TSharedPtr<FJsonObject> Params)
{
    UTransBuffer* TB = GetTransBuffer();
    if (!TB) return MakeError(DOMAIN, TEXT("list_recent_transactions"), 3000, TEXT("Transaction buffer unavailable"));

    int32 Limit = 20;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    const int32 N = TB->GetQueueLength();
    const int32 Max = FMath::Min(N, Limit);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (int32 i = 0; i < Max; ++i)
    {
        const int32 QIdx = N - 1 - i;
        const FTransaction* T = TB->GetTransaction(QIdx);
        if (!T) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("queue_index"), QIdx);
        Entry->SetStringField(TEXT("title"), T->GetTitle().ToString());
        Entry->SetStringField(TEXT("context"), T->GetContext().Context);
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("transactions"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    Data->SetNumberField(TEXT("total_in_buffer"), N);
    return MakeSuccess(DOMAIN, TEXT("list_recent_transactions"),
        FString::Printf(TEXT("%d transaction(s) (of %d)"), Arr.Num(), N), Data);
}

FBridgeResult UTransactionHandler::Action_ClearHistory(TSharedPtr<FJsonObject> Params)
{
    if (!GEditor) return MakeError(DOMAIN, TEXT("clear_history"), 3000, TEXT("GEditor not available"));
    GEditor->ResetTransaction(NSLOCTEXT("ForgeEditorBridge", "ClearTrans", "Bridge: clear transaction history"));
    return MakeSuccess(DOMAIN, TEXT("clear_history"), TEXT("Transaction history cleared"));
}