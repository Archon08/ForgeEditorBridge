#include "Handlers/BookmarkHandler.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Bookmarks/IBookmarkTypeTools.h"
#include "Engine/BookmarkBase.h"
#include "Engine/BookMark.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("bookmark");

namespace
{
    FEditorViewportClient* GetActiveViewportClient()
    {
        if (!GEditor) return nullptr;
        if (GCurrentLevelEditingViewportClient) return GCurrentLevelEditingViewportClient;
        if (FViewport* Vp = GEditor->GetActiveViewport())
        {
            return static_cast<FEditorViewportClient*>(Vp->GetClient());
        }
        return nullptr;
    }

    UWorld* GetEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }
}

FBridgeResult UBookmarkBridgeHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("set_bookmark"))         return Action_SetBookmark(Params);
    if (Action == TEXT("jump_to_bookmark"))     return Action_JumpToBookmark(Params);
    if (Action == TEXT("clear_bookmark"))       return Action_ClearBookmark(Params);
    if (Action == TEXT("clear_all_bookmarks"))  return Action_ClearAllBookmarks(Params);
    if (Action == TEXT("list_bookmarks"))       return Action_ListBookmarks(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("set_bookmark, jump_to_bookmark, clear_bookmark, clear_all_bookmarks, list_bookmarks"));
}

FBridgeResult UBookmarkBridgeHandler::Action_SetBookmark(TSharedPtr<FJsonObject> Params)
{
    int32 Index = 0;
    if (!Params->TryGetNumberField(TEXT("index"), Index))
        return MakeError(DOMAIN, TEXT("set_bookmark"), 1000, TEXT("'index' is required"));

    FEditorViewportClient* VC = GetActiveViewportClient();
    if (!VC) return MakeError(DOMAIN, TEXT("set_bookmark"), 3000, TEXT("No active viewport client"));

    // If an explicit location/rotation was passed, position the viewport first
    const TSharedPtr<FJsonObject>* LocObj = nullptr;
    if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj && LocObj->IsValid())
    {
        double X=0, Y=0, Z=0;
        (*LocObj)->TryGetNumberField(TEXT("x"), X);
        (*LocObj)->TryGetNumberField(TEXT("y"), Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), Z);
        VC->SetViewLocation(FVector(X,Y,Z));
    }
    const TSharedPtr<FJsonObject>* RotObj = nullptr;
    if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
    {
        double P=0, Yw=0, R=0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), P);
        (*RotObj)->TryGetNumberField(TEXT("yaw"),   Yw);
        (*RotObj)->TryGetNumberField(TEXT("roll"),  R);
        VC->SetViewRotation(FRotator(P, Yw, R));
    }

    IBookmarkTypeTools::Get().CreateOrSetBookmark((uint32)Index, VC);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("index"), Index);
    return MakeSuccess(DOMAIN, TEXT("set_bookmark"),
        FString::Printf(TEXT("Bookmark %d set"), Index), Data);
}

FBridgeResult UBookmarkBridgeHandler::Action_JumpToBookmark(TSharedPtr<FJsonObject> Params)
{
    int32 Index = 0;
    if (!Params->TryGetNumberField(TEXT("index"), Index))
        return MakeError(DOMAIN, TEXT("jump_to_bookmark"), 1000, TEXT("'index' is required"));

    FEditorViewportClient* VC = GetActiveViewportClient();
    if (!VC) return MakeError(DOMAIN, TEXT("jump_to_bookmark"), 3000, TEXT("No active viewport client"));

    // 5.7: JumpToBookmark returns void; success is implicit (caller verifies via list_bookmarks).
    IBookmarkTypeTools::Get().JumpToBookmark(
        (uint32)Index, TSharedPtr<FBookmarkBaseJumpToSettings>(), VC);
    return MakeSuccess(DOMAIN, TEXT("jump_to_bookmark"),
        FString::Printf(TEXT("Jumped to bookmark %d (no-op if unset)"), Index));
}

FBridgeResult UBookmarkBridgeHandler::Action_ClearBookmark(TSharedPtr<FJsonObject> Params)
{
    int32 Index = 0;
    if (!Params->TryGetNumberField(TEXT("index"), Index))
        return MakeError(DOMAIN, TEXT("clear_bookmark"), 1000, TEXT("'index' is required"));

    // 5.7: ClearBookmark takes FEditorViewportClient*, not UWorld*.
    FEditorViewportClient* VC = GetActiveViewportClient();
    if (!VC) return MakeError(DOMAIN, TEXT("clear_bookmark"), 3000, TEXT("No active viewport client"));

    IBookmarkTypeTools::Get().ClearBookmark((uint32)Index, VC);
    return MakeSuccess(DOMAIN, TEXT("clear_bookmark"),
        FString::Printf(TEXT("Bookmark %d cleared"), Index));
}

FBridgeResult UBookmarkBridgeHandler::Action_ClearAllBookmarks(TSharedPtr<FJsonObject> Params)
{
    FEditorViewportClient* VC = GetActiveViewportClient();
    if (!VC) return MakeError(DOMAIN, TEXT("clear_all_bookmarks"), 3000, TEXT("No active viewport client"));
    IBookmarkTypeTools::Get().ClearAllBookmarks(VC);
    return MakeSuccess(DOMAIN, TEXT("clear_all_bookmarks"), TEXT("All bookmarks cleared"));
}

FBridgeResult UBookmarkBridgeHandler::Action_ListBookmarks(TSharedPtr<FJsonObject> Params)
{
    UWorld* World = GetEditorWorld();
    if (!World) return MakeError(DOMAIN, TEXT("list_bookmarks"), 3000, TEXT("No editor world"));

    AWorldSettings* WS = World->GetWorldSettings();
    TArray<TSharedPtr<FJsonValue>> Arr;
    if (WS)
    {
        // 5.7: BookmarkArray is private — use the public GetBookmarks() accessor.
        const TArray<UBookmarkBase*>& Bookmarks = WS->GetBookmarks();
        const int32 N = Bookmarks.Num();
        for (int32 i = 0; i < N; ++i)
        {
            UBookmarkBase* B = Bookmarks[i];
            if (!B) continue;
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("index"), i);
            Entry->SetStringField(TEXT("class"), B->GetClass()->GetName());
            if (UBookMark* B2 = Cast<UBookMark>(B))
            {
                TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
                Loc->SetNumberField(TEXT("x"), B2->Location.X);
                Loc->SetNumberField(TEXT("y"), B2->Location.Y);
                Loc->SetNumberField(TEXT("z"), B2->Location.Z);
                Entry->SetObjectField(TEXT("location"), Loc);
                TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
                Rot->SetNumberField(TEXT("pitch"), B2->Rotation.Pitch);
                Rot->SetNumberField(TEXT("yaw"),   B2->Rotation.Yaw);
                Rot->SetNumberField(TEXT("roll"),  B2->Rotation.Roll);
                Entry->SetObjectField(TEXT("rotation"), Rot);
            }
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("bookmarks"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_bookmarks"),
        FString::Printf(TEXT("%d bookmark(s)"), Arr.Num()), Data);
}