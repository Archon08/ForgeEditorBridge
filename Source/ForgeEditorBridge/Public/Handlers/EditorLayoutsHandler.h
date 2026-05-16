#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "EditorLayoutsHandler.generated.h"

/**
 * EditorLayoutsHandler — domain "layout"  (UE 5.7)
 *
 * Editor window layout save/restore. Limited surface — UE's layout system is
 * mostly Slate-internal (FLayoutSaveRestore) and the user-saved-layouts list
 * isn't enumerable from C++ without scanning EditorLayouts.ini directly.
 * The bridge exposes the documented save/load + console-driven defaults.
 *
 * Actions:
 *   save_current_layout   → uses FGlobalTabmanager::SaveAllVisualState()
 *                           writes to the active editor layout config slot
 *   reset_layout          → executes "LayoutMenu.LoadDefaultLayout" console command
 *   load_layout_config    → ini_path (string, abs path) — reads layout JSON from a file
 *                           and applies via console command
 *   list_layout_slots     → enumerates EditorLayouts.ini slot names from Saved/Config
 *   get_active_layout     → returns the currently-applied layout's name (best-effort)
 */
UCLASS()
class FORGEEDITORBRIDGE_API UEditorLayoutsHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("layout"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("save_current_layout"), TEXT("reset_layout"),
            TEXT("load_layout_config"), TEXT("list_layout_slots"),
            TEXT("get_active_layout")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_SaveCurrent     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ResetLayout     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_LoadLayoutConfig(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListSlots       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetActive       (TSharedPtr<FJsonObject> Params);
};
