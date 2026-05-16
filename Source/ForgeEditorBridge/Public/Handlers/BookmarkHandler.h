#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "BookmarkHandler.generated.h"

/**
 * BookmarkHandler — domain "bookmark"  (UE 5.7)
 *
 * Editor viewport bookmarks (Ctrl+1..9 in the level editor).
 *
 * Actions:
 *   set_bookmark        → index (0-9), location? {x,y,z} (default: current viewport),
 *                         rotation? {pitch,yaw,roll}
 *   jump_to_bookmark    → index
 *   clear_bookmark      → index
 *   clear_all_bookmarks → (no params)
 *   list_bookmarks      → returns array of {index, location, rotation} for set bookmarks
 */
UCLASS()
class FORGEEDITORBRIDGE_API UBookmarkBridgeHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("bookmark"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("set_bookmark"), TEXT("jump_to_bookmark"),
            TEXT("clear_bookmark"), TEXT("clear_all_bookmarks"), TEXT("list_bookmarks")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_SetBookmark        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_JumpToBookmark     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearBookmark      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ClearAllBookmarks  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListBookmarks      (TSharedPtr<FJsonObject> Params);
};