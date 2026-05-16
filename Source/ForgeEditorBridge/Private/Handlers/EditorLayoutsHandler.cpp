#include "Handlers/EditorLayoutsHandler.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Docking/LayoutService.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("layout");

namespace
{
    UWorld* GetEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }
    bool RunConsole(const FString& Cmd)
    {
        if (!GEngine) return false;
        return GEngine->Exec(GetEditorWorld(), *Cmd);
    }
}

FBridgeResult UEditorLayoutsHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("save_current_layout"))  return Action_SaveCurrent(Params);
    if (Action == TEXT("reset_layout"))         return Action_ResetLayout(Params);
    if (Action == TEXT("load_layout_config"))   return Action_LoadLayoutConfig(Params);
    if (Action == TEXT("list_layout_slots"))    return Action_ListSlots(Params);
    if (Action == TEXT("get_active_layout"))    return Action_GetActive(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("save_current_layout, reset_layout, load_layout_config, list_layout_slots, get_active_layout"));
}

FBridgeResult UEditorLayoutsHandler::Action_SaveCurrent(TSharedPtr<FJsonObject> Params)
{
    FGlobalTabmanager::Get()->SaveAllVisualState();
    return MakeSuccess(DOMAIN, TEXT("save_current_layout"),
        TEXT("Layout saved to active editor layout config"));
}

FBridgeResult UEditorLayoutsHandler::Action_ResetLayout(TSharedPtr<FJsonObject> Params)
{
    const bool bOk = RunConsole(TEXT("LayoutMenu.LoadDefaultLayout"));
    return MakeSuccess(DOMAIN, TEXT("reset_layout"),
        bOk ? TEXT("Default layout requested") : TEXT("Console exec returned false (try menu Window > Reset Layout)"));
}

FBridgeResult UEditorLayoutsHandler::Action_LoadLayoutConfig(TSharedPtr<FJsonObject> Params)
{
    // 5.7: Global editor layout swap requires restart. The editor's own
    // Layouts > Load menu invokes FUnrealEdMisc::RestartEditor(false) after
    // copying the chosen config to GEditorLayoutIni. We expose the pre-restart
    // half here: copy the user-supplied ini onto GEditorLayoutIni so the next
    // editor launch picks it up.
    FString IniPath;
    if (!Params->TryGetStringField(TEXT("ini_path"), IniPath) || IniPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("load_layout_config"), 1000, TEXT("'ini_path' is required"));

    if (!IFileManager::Get().FileExists(*IniPath))
        return MakeError(DOMAIN, TEXT("load_layout_config"), 2000,
            FString::Printf(TEXT("Layout ini not found: %s"), *IniPath));

    extern FString GEditorLayoutIni;
    if (!IFileManager::Get().Copy(*GEditorLayoutIni, *IniPath, /*bReplace=*/true))
        return MakeError(DOMAIN, TEXT("load_layout_config"), 3000,
            FString::Printf(TEXT("Failed to copy layout to %s"), *GEditorLayoutIni));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("staged_to"), GEditorLayoutIni);
    Data->SetBoolField(TEXT("restart_required"), true);
    return MakeSuccess(DOMAIN, TEXT("load_layout_config"),
        TEXT("Layout staged to GEditorLayoutIni — restart the editor to apply (UE 5.7 has no restart-free global swap)"),
        Data);
}

FBridgeResult UEditorLayoutsHandler::Action_ListSlots(TSharedPtr<FJsonObject> Params)
{
    // Layouts live under {Saved}/Config/{Platform}/EditorLayouts.ini
    const FString ConfigDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"));
    TArray<FString> SectionNames;
    TArray<TSharedPtr<FJsonValue>> Arr;

    // Try every per-platform variant. EditorLayouts.ini is loaded at editor start.
    // The simplest reliable enumeration is via GConfig section names.
    TArray<FString> AllConfigFiles;
    GConfig->GetConfigFilenames(AllConfigFiles);
    for (const FString& File : AllConfigFiles)
    {
        if (!File.Contains(TEXT("EditorLayouts"), ESearchCase::IgnoreCase)) continue;
        TArray<FString> Sections;
        if (FConfigFile* CF = GConfig->FindConfigFile(File))
        {
            for (const auto& Pair : *CF)
            {
                TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
                O->SetStringField(TEXT("section"), Pair.Key);
                O->SetStringField(TEXT("file"), File);
                Arr.Add(MakeShared<FJsonValueObject>(O));
            }
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("layouts"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_layout_slots"),
        FString::Printf(TEXT("%d layout section(s) found in EditorLayouts*.ini"), Arr.Num()), Data);
}

FBridgeResult UEditorLayoutsHandler::Action_GetActive(TSharedPtr<FJsonObject> Params)
{
    // No public API for "currently active layout name" — UE applies a single
    // FTabManager::FLayout instance per editor and doesn't expose its slot name.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("note"),
        TEXT("UE 5.7 does not expose the active layout name programmatically — only the most recent SaveAllVisualState slot is implicit"));
    return MakeSuccess(DOMAIN, TEXT("get_active_layout"), TEXT("Active layout query is best-effort"), Data);
}
