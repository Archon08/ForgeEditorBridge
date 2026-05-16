#include "Handlers/ToolMenusHandler.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("menus");

namespace
{
    UToolMenu* GetOrExtendMenu(const FString& MenuName)
    {
        UToolMenus* TM = UToolMenus::Get();
        if (!TM) return nullptr;
        return TM->ExtendMenu(FName(*MenuName));
    }

    void ExecuteConsoleCommand(const FString& Cmd)
    {
        if (Cmd.IsEmpty() || !GEngine) return;
        if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : GEngine->GetWorldContexts().Num() ? GEngine->GetWorldContexts()[0].World() : nullptr)
        {
            GEngine->Exec(World, *Cmd);
        }
    }
}

FBridgeResult UToolMenusHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("add_menu_entry"))      return Action_AddMenuEntry(Params);
    if (Action == TEXT("add_section"))         return Action_AddSection(Params);
    if (Action == TEXT("remove_menu_entry"))   return Action_RemoveMenuEntry(Params);
    if (Action == TEXT("remove_section"))      return Action_RemoveSection(Params);
    if (Action == TEXT("list_menu_sections"))  return Action_ListMenuSections(Params);
    if (Action == TEXT("list_menus"))          return Action_ListMenus(Params);
    if (Action == TEXT("refresh_all_menus"))   return Action_RefreshAllMenus(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("add_menu_entry, add_section, remove_menu_entry, remove_section, list_menu_sections, list_menus, refresh_all_menus"));
}

FBridgeResult UToolMenusHandler::Action_AddMenuEntry(TSharedPtr<FJsonObject> Params)
{
    FString MenuName, SectionName, EntryName, Label, Tooltip, Cmd;
    if (!Params->TryGetStringField(TEXT("menu_name"), MenuName) || MenuName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_menu_entry"), 1000, TEXT("'menu_name' is required"));
    if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_menu_entry"), 1000, TEXT("'section_name' is required"));
    if (!Params->TryGetStringField(TEXT("entry_name"), EntryName) || EntryName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_menu_entry"), 1000, TEXT("'entry_name' is required"));
    if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_menu_entry"), 1000, TEXT("'label' is required"));
    Params->TryGetStringField(TEXT("tooltip"), Tooltip);
    Params->TryGetStringField(TEXT("console_command"), Cmd);

    UToolMenu* Menu = GetOrExtendMenu(MenuName);
    if (!Menu) return MakeError(DOMAIN, TEXT("add_menu_entry"), 3000,
        FString::Printf(TEXT("Could not extend menu '%s'"), *MenuName));

    FToolMenuSection& Section = Menu->FindOrAddSection(FName(*SectionName));
    if (!Section.Label.IsSet())
    {
        Section.Label = FText::FromString(SectionName);
    }

    const FString CmdCopy = Cmd; // captured by lambda
    FToolMenuEntry Entry = FToolMenuEntry::InitMenuEntry(
        FName(*EntryName),
        FText::FromString(Label),
        FText::FromString(Tooltip),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([CmdCopy]()
        {
            ExecuteConsoleCommand(CmdCopy);
        }))
    );
    Section.AddEntry(Entry);

    UToolMenus::Get()->RefreshAllWidgets();
    return MakeSuccess(DOMAIN, TEXT("add_menu_entry"),
        FString::Printf(TEXT("Added entry '%s' under '%s' / '%s'"), *EntryName, *MenuName, *SectionName));
}

FBridgeResult UToolMenusHandler::Action_AddSection(TSharedPtr<FJsonObject> Params)
{
    FString MenuName, SectionName, Label;
    if (!Params->TryGetStringField(TEXT("menu_name"), MenuName) || MenuName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_section"), 1000, TEXT("'menu_name' is required"));
    if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_section"), 1000, TEXT("'section_name' is required"));
    Params->TryGetStringField(TEXT("label"), Label);

    UToolMenu* Menu = GetOrExtendMenu(MenuName);
    if (!Menu) return MakeError(DOMAIN, TEXT("add_section"), 3000,
        FString::Printf(TEXT("Could not extend menu '%s'"), *MenuName));

    FToolMenuSection& Section = Menu->FindOrAddSection(
        FName(*SectionName), FText::FromString(Label.IsEmpty() ? SectionName : Label));
    UToolMenus::Get()->RefreshAllWidgets();
    return MakeSuccess(DOMAIN, TEXT("add_section"),
        FString::Printf(TEXT("Section '%s' under '%s'"), *SectionName, *MenuName));
}

FBridgeResult UToolMenusHandler::Action_RemoveMenuEntry(TSharedPtr<FJsonObject> Params)
{
    FString MenuName, SectionName, EntryName;
    if (!Params->TryGetStringField(TEXT("menu_name"), MenuName) || MenuName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_menu_entry"), 1000, TEXT("'menu_name' is required"));
    if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_menu_entry"), 1000, TEXT("'section_name' is required"));
    if (!Params->TryGetStringField(TEXT("entry_name"), EntryName) || EntryName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_menu_entry"), 1000, TEXT("'entry_name' is required"));

    UToolMenus* TM = UToolMenus::Get();
    if (!TM) return MakeError(DOMAIN, TEXT("remove_menu_entry"), 3000, TEXT("UToolMenus unavailable"));

    TM->RemoveEntry(FName(*MenuName), FName(*SectionName), FName(*EntryName));
    TM->RefreshAllWidgets();
    return MakeSuccess(DOMAIN, TEXT("remove_menu_entry"),
        FString::Printf(TEXT("Removed entry '%s'"), *EntryName));
}

FBridgeResult UToolMenusHandler::Action_RemoveSection(TSharedPtr<FJsonObject> Params)
{
    FString MenuName, SectionName;
    if (!Params->TryGetStringField(TEXT("menu_name"), MenuName) || MenuName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_section"), 1000, TEXT("'menu_name' is required"));
    if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_section"), 1000, TEXT("'section_name' is required"));

    UToolMenus* TM = UToolMenus::Get();
    if (!TM) return MakeError(DOMAIN, TEXT("remove_section"), 3000, TEXT("UToolMenus unavailable"));

    TM->RemoveSection(FName(*MenuName), FName(*SectionName));
    TM->RefreshAllWidgets();
    return MakeSuccess(DOMAIN, TEXT("remove_section"),
        FString::Printf(TEXT("Removed section '%s'"), *SectionName));
}

FBridgeResult UToolMenusHandler::Action_ListMenuSections(TSharedPtr<FJsonObject> Params)
{
    FString MenuName;
    if (!Params->TryGetStringField(TEXT("menu_name"), MenuName) || MenuName.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_menu_sections"), 1000, TEXT("'menu_name' is required"));

    UToolMenus* TM = UToolMenus::Get();
    if (!TM) return MakeError(DOMAIN, TEXT("list_menu_sections"), 3000, TEXT("UToolMenus unavailable"));

    UToolMenu* Menu = TM->FindMenu(FName(*MenuName));
    if (!Menu)
    {
        return MakeError(DOMAIN, TEXT("list_menu_sections"), 2000,
            FString::Printf(TEXT("Menu '%s' not found"), *MenuName));
    }

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FToolMenuSection& Section : Menu->Sections)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Section.Name.ToString());
        Entry->SetStringField(TEXT("label"), Section.Label.IsSet() ? Section.Label.Get().ToString() : FString());
        Entry->SetNumberField(TEXT("entry_count"), Section.Blocks.Num());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("sections"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_menu_sections"),
        FString::Printf(TEXT("%d section(s)"), Arr.Num()), Data);
}

FBridgeResult UToolMenusHandler::Action_ListMenus(TSharedPtr<FJsonObject> Params)
{
    UToolMenus* TM = UToolMenus::Get();
    if (!TM) return MakeError(DOMAIN, TEXT("list_menus"), 3000, TEXT("UToolMenus unavailable"));

    // Common known menu paths — full enumeration is internal-only; return a curated list.
    static const TArray<FString> KnownMenus = {
        TEXT("MainFrame.MainMenu.File"),
        TEXT("MainFrame.MainMenu.Edit"),
        TEXT("MainFrame.MainMenu.Window"),
        TEXT("MainFrame.MainMenu.Tools"),
        TEXT("MainFrame.MainMenu.Help"),
        TEXT("LevelEditor.MainMenu.File"),
        TEXT("LevelEditor.MainMenu.Build"),
        TEXT("LevelEditor.MainMenu.Select"),
        TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"),
        TEXT("LevelEditor.LevelViewportToolBar.View"),
        TEXT("LevelEditor.ActorContextMenu"),
        TEXT("ContentBrowser.AssetContextMenu")
    };
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FString& M : KnownMenus)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), M);
        Entry->SetBoolField(TEXT("registered"), TM->FindMenu(FName(*M)) != nullptr);
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("menus"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_menus"),
        FString::Printf(TEXT("%d known menu path(s)"), Arr.Num()), Data);
}

FBridgeResult UToolMenusHandler::Action_RefreshAllMenus(TSharedPtr<FJsonObject> Params)
{
    UToolMenus* TM = UToolMenus::Get();
    if (!TM) return MakeError(DOMAIN, TEXT("refresh_all_menus"), 3000, TEXT("UToolMenus unavailable"));
    TM->RefreshAllWidgets();
    return MakeSuccess(DOMAIN, TEXT("refresh_all_menus"), TEXT("Menus refreshed"));
}
