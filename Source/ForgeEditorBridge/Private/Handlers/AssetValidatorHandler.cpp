#include "Handlers/AssetValidatorHandler.h"
#include "EditorValidatorSubsystem.h"
#include "EditorValidatorBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("validator");

namespace
{
    UEditorValidatorSubsystem* GetVS()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>() : nullptr;
    }
}

FBridgeResult UAssetValidatorHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("list_validators"))            return Action_ListValidators(Params);
    if (Action == TEXT("validate_asset_detailed"))    return Action_ValidateAssetDetailed(Params);
    if (Action == TEXT("validate_path"))              return Action_ValidatePath(Params);
    if (Action == TEXT("get_last_validation_results")) return Action_GetLastResults(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("list_validators, validate_asset_detailed, validate_path, get_last_validation_results"));
}

FBridgeResult UAssetValidatorHandler::Action_ListValidators(TSharedPtr<FJsonObject> Params)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* C = *It;
        if (!C) continue;
        if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
        if (!C->IsChildOf(UEditorValidatorBase::StaticClass())) continue;
        if (C == UEditorValidatorBase::StaticClass()) continue;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("class"), C->GetName());
        O->SetStringField(TEXT("path"),  C->GetPathName());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("validators"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_validators"),
        FString::Printf(TEXT("%d validator class(es)"), Arr.Num()), Data);
}

FBridgeResult UAssetValidatorHandler::Action_ValidateAssetDetailed(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("validate_asset_detailed"), 1000, TEXT("'asset_path' is required"));

    UEditorValidatorSubsystem* VS = GetVS();
    if (!VS) return MakeError(DOMAIN, TEXT("validate_asset_detailed"), 3000, TEXT("UEditorValidatorSubsystem unavailable"));

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        Asset = LoadObject<UObject>(nullptr, *Suffix);
    }
    if (!Asset) return MakeError(DOMAIN, TEXT("validate_asset_detailed"), 2000,
        FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    FAssetData AD = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(Asset->GetPathName()));

    TArray<FText> Warnings, Errors;
    FValidateAssetsSettings Settings;
    Settings.bSkipExcludedDirectories = false;
    Settings.bShowIfNoFailures = true;
    FValidateAssetsResults Results;

    TArray<FAssetData> Assets;
    if (AD.IsValid()) Assets.Add(AD);
    VS->ValidateAssetsWithSettings(Assets, Settings, Results);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetNumberField(TEXT("num_checked"),  Results.NumChecked);
    Data->SetNumberField(TEXT("num_valid"),    Results.NumValid);
    Data->SetNumberField(TEXT("num_invalid"),  Results.NumInvalid);
    Data->SetNumberField(TEXT("num_skipped"),  Results.NumSkipped);
    Data->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);

    LastResults = Data;
    return MakeSuccess(DOMAIN, TEXT("validate_asset_detailed"),
        FString::Printf(TEXT("%d/%d valid (%d warnings)"),
            Results.NumValid, Results.NumChecked, Results.NumWarnings), Data);
}

FBridgeResult UAssetValidatorHandler::Action_ValidatePath(TSharedPtr<FJsonObject> Params)
{
    FString ContentPath;
    if (!Params->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("validate_path"), 1000, TEXT("'content_path' is required"));

    UEditorValidatorSubsystem* VS = GetVS();
    if (!VS) return MakeError(DOMAIN, TEXT("validate_path"), 3000, TEXT("UEditorValidatorSubsystem unavailable"));

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> Assets;
    ARM.Get().GetAssetsByPath(FName(*ContentPath), Assets, /*bRecursive=*/true);

    FValidateAssetsSettings Settings;
    Settings.bSkipExcludedDirectories = true;
    Settings.bShowIfNoFailures = false;
    FValidateAssetsResults Results;
    VS->ValidateAssetsWithSettings(Assets, Settings, Results);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("content_path"), ContentPath);
    Data->SetNumberField(TEXT("num_checked"),  Results.NumChecked);
    Data->SetNumberField(TEXT("num_valid"),    Results.NumValid);
    Data->SetNumberField(TEXT("num_invalid"),  Results.NumInvalid);
    Data->SetNumberField(TEXT("num_skipped"),  Results.NumSkipped);
    Data->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);

    LastResults = Data;
    return MakeSuccess(DOMAIN, TEXT("validate_path"),
        FString::Printf(TEXT("Validated %d asset(s) under '%s' — %d valid"),
            Results.NumChecked, *ContentPath, Results.NumValid), Data);
}

FBridgeResult UAssetValidatorHandler::Action_GetLastResults(TSharedPtr<FJsonObject> Params)
{
    if (!LastResults.IsValid())
        return MakeSuccess(DOMAIN, TEXT("get_last_validation_results"), TEXT("No prior results"));
    return MakeSuccess(DOMAIN, TEXT("get_last_validation_results"), TEXT("Cached"), LastResults);
}
