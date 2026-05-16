#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ToolMenusHandler.generated.h"

/**
 * ToolMenusHandler — domain "menus"  (UE 5.7)
 *
 * Editor menu/toolbar extensions via UToolMenus. Lets the bridge author its own
 * editor UI. Menu entries trigger console commands when clicked (the entry
 * "verb" is just a console string the bridge wires up).
 *
 * Common menu paths:
 *   MainFrame.MainMenu.File
 *   MainFrame.MainMenu.Window
 *   MainFrame.MainMenu.Tools
 *   LevelEditor.MainMenu.Build
 *   LevelEditor.LevelEditorToolBar.PlayToolBar
 *
 * Actions:
 *   add_menu_entry      → menu_name, section_name, entry_name, label,
 *                         tooltip?, console_command?
 *   add_section         → menu_name, section_name, label?
 *   remove_menu_entry   → menu_name, section_name, entry_name
 *   remove_section      → menu_name, section_name
 *   list_menu_sections  → menu_name → returns sections + entry counts
 *   list_menus          → returns top-level registered menus
 *   refresh_all_menus   → triggers UToolMenus::RefreshAllWidgets
 */
UCLASS()
class FORGEEDITORBRIDGE_API UToolMenusHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("menus"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("add_menu_entry"), TEXT("add_section"),
            TEXT("remove_menu_entry"), TEXT("remove_section"),
            TEXT("list_menu_sections"), TEXT("list_menus"),
            TEXT("refresh_all_menus")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_AddMenuEntry      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddSection        (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveMenuEntry   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RemoveSection     (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListMenuSections  (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListMenus         (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RefreshAllMenus   (TSharedPtr<FJsonObject> Params);
};
