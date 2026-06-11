#include "Handlers/ConcertHandler.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "IConcertSyncClient.h"
#include "IConcertSyncClientModule.h"
#include "IConcertClient.h"
#include "ConcertMessages.h"
#include "ConcertSettings.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("concert");

namespace
{
    bool IsConcertMounted()
    {
        return FModuleManager::Get().IsModuleLoaded(TEXT("ConcertSyncClient")) ||
               FModuleManager::Get().IsModuleLoaded(TEXT("ConcertSyncCore"));
    }
    bool RunCmd(const FString& Cmd)
    {
        if (!GEngine) return false;
        UWorld* W = UBridgeHandlerBase::GetSafeEditorWorld();
        return GEngine->Exec(W, *Cmd);
    }
    TSharedPtr<IConcertClient> GetConcertClient()
    {
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("ConcertSyncClient"))) return nullptr;
        IConcertSyncClientModule& Module = IConcertSyncClientModule::Get();
        TSharedPtr<IConcertSyncClient> SyncClient = Module.GetClient(TEXT("MultiUser"));
        if (!SyncClient.IsValid()) return nullptr;
        return SyncClient->GetConcertClient();
    }
}

FBridgeResult UConcertHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("is_concert_loaded"))    return Action_IsConcertLoaded(Params);
    if (Action == TEXT("list_sessions"))        return Action_ListSessions(Params);
    if (Action == TEXT("get_active_session"))   return Action_GetActiveSession(Params);
    if (Action == TEXT("leave_session"))        return Action_LeaveSession(Params);
    if (Action == TEXT("open_concert_browser")) return Action_OpenConcertBrowser(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("is_concert_loaded, list_sessions, get_active_session, leave_session, open_concert_browser"));
}

FBridgeResult UConcertHandler::Action_IsConcertLoaded(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("loaded"), IsConcertMounted());
    return MakeSuccess(DOMAIN, TEXT("is_concert_loaded"),
        IsConcertMounted() ? TEXT("Concert mounted") : TEXT("Concert not mounted"), Data);
}

FBridgeResult UConcertHandler::Action_ListSessions(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<IConcertClient> Client = GetConcertClient();
    if (!Client.IsValid())
        return MakeError(DOMAIN, TEXT("list_sessions"), 3003,
            TEXT("Concert client unavailable"),
            TEXT("Enable Multi-User Editing plugin and run the editor with -concertEnabled or via the MU browser"));

    // Kick discovery and let it run for a short window before listing.
    Client->StartDiscovery();
    FPlatformProcess::Sleep(1.5f);
    TArray<FConcertServerInfo> Servers = Client->GetKnownServers();

    TArray<TSharedPtr<FJsonValue>> ServerArr;
    TArray<TSharedPtr<FJsonValue>> SessionArr;
    for (const FConcertServerInfo& Server : Servers)
    {
        TSharedPtr<FJsonObject> SO = MakeShared<FJsonObject>();
        SO->SetStringField(TEXT("server_name"), Server.ServerName);
        SO->SetStringField(TEXT("admin_endpoint_id"), Server.AdminEndpointId.ToString());
        SO->SetStringField(TEXT("instance_id"), Server.InstanceInfo.InstanceId.ToString());
        ServerArr.Add(MakeShared<FJsonValueObject>(SO));

        TFuture<FConcertAdmin_GetSessionsResponse> Future = Client->GetLiveSessions(Server.AdminEndpointId);
        const FConcertAdmin_GetSessionsResponse Response = Future.Get();
        for (const FConcertSessionInfo& Sess : Response.Sessions)
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("server_admin_endpoint_id"), Server.AdminEndpointId.ToString());
            O->SetStringField(TEXT("session_name"), Sess.SessionName);
            O->SetStringField(TEXT("session_id"), Sess.SessionId.ToString());
            O->SetStringField(TEXT("owner_user_name"), Sess.OwnerUserName);
            SessionArr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("servers"), ServerArr);
    Data->SetArrayField(TEXT("live_sessions"), SessionArr);
    Data->SetNumberField(TEXT("server_count"), ServerArr.Num());
    Data->SetNumberField(TEXT("session_count"), SessionArr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_sessions"),
        FString::Printf(TEXT("%d server(s), %d live session(s)"), ServerArr.Num(), SessionArr.Num()), Data);
}

FBridgeResult UConcertHandler::Action_GetActiveSession(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<IConcertClient> Client = GetConcertClient();
    if (!Client.IsValid())
        return MakeError(DOMAIN, TEXT("get_active_session"), 3003,
            TEXT("Concert client unavailable"));

    const EConcertConnectionStatus Status = Client->GetSessionConnectionStatus();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("connection_status"), (int32)Status);
    Data->SetBoolField(TEXT("connected"), Status == EConcertConnectionStatus::Connected);

    if (Status == EConcertConnectionStatus::Connected)
    {
        TSharedPtr<IConcertClientSession> Session = Client->GetCurrentSession();
        if (Session.IsValid())
        {
            const FConcertSessionInfo& Info = Session->GetSessionInfo();
            Data->SetStringField(TEXT("session_name"), Info.SessionName);
            Data->SetStringField(TEXT("session_id"), Info.SessionId.ToString());
            Data->SetStringField(TEXT("owner_user_name"), Info.OwnerUserName);
        }
    }
    return MakeSuccess(DOMAIN, TEXT("get_active_session"),
        Status == EConcertConnectionStatus::Connected ? TEXT("connected") : TEXT("not connected"), Data);
}

FBridgeResult UConcertHandler::Action_LeaveSession(TSharedPtr<FJsonObject> Params)
{
    TSharedPtr<IConcertClient> Client = GetConcertClient();
    if (!Client.IsValid())
        return MakeError(DOMAIN, TEXT("leave_session"), 3003, TEXT("Concert client unavailable"));
    Client->DisconnectSession();
    return MakeSuccess(DOMAIN, TEXT("leave_session"), TEXT("DisconnectSession requested"));
}

FBridgeResult UConcertHandler::Action_OpenConcertBrowser(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunCmd(TEXT("Concert.OpenBrowser"));
    return MakeSuccess(DOMAIN, TEXT("open_concert_browser"),
        bOk ? TEXT("Browser open requested") : TEXT("Console exec returned false (try Window > Multi-User Browser)"));
}
