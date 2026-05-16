#include "Handlers/ChooserHandler.h"
// 5.7: UChooserTable lives in Chooser/Internal/Chooser.h. The PrivateIncludePathModuleNames
// entry for "Chooser" exposes the Internal/ folder for compile.
#include "Chooser.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("chooser");

namespace
{
    UChooserTable* LoadChooser(const FString& Path)
    {
        if (UChooserTable* C = LoadObject<UChooserTable>(nullptr, *Path)) return C;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UChooserTable>(nullptr, *Suffix);
    }
}

FBridgeResult UChooserHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_chooser"))    return Action_CreateChooser(Params);
    if (Action == TEXT("get_chooser_info"))  return Action_GetChooserInfo(Params);
    if (Action == TEXT("set_output_object")) return Action_SetOutputObject(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_chooser, get_chooser_info, set_output_object"));
}

FBridgeResult UChooserHandler::Action_CreateChooser(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_chooser"), 1000, TEXT("'asset_path' is required"));

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return MakeError(DOMAIN, TEXT("create_chooser"), 3000,
        FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
    Package->FullyLoad();

    UChooserTable* Chooser = NewObject<UChooserTable>(Package, FName(*AssetName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Chooser) return MakeError(DOMAIN, TEXT("create_chooser"), 3000, TEXT("NewObject failed"));
    FAssetRegistryModule::AssetCreated(Chooser);
    Package->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("create_chooser"),
        FString::Printf(TEXT("Created UChooserTable at '%s'"), *AssetPath));
}

FBridgeResult UChooserHandler::Action_GetChooserInfo(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_chooser_info"), 1000, TEXT("'asset_path' is required"));
    UChooserTable* Chooser = LoadChooser(AssetPath);
    if (!Chooser) return MakeError(DOMAIN, TEXT("get_chooser_info"), 2000,
        FString::Printf(TEXT("Chooser not found: %s"), *AssetPath));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetNumberField(TEXT("column_count"), Chooser->ColumnsStructs.Num());
    Data->SetNumberField(TEXT("result_count"), Chooser->ResultsStructs.Num());
    if (UClass* OutputCls = Chooser->OutputObjectType)
    {
        Data->SetStringField(TEXT("output_class"), OutputCls->GetPathName());
    }
    return MakeSuccess(DOMAIN, TEXT("get_chooser_info"), TEXT("Chooser info"), Data);
}

FBridgeResult UChooserHandler::Action_SetOutputObject(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Chooser Set Output"));
    FString AssetPath, ClassPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_output_object"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_output_object"), 1000, TEXT("'class_path' is required"));

    UChooserTable* Chooser = LoadChooser(AssetPath);
    if (!Chooser) return MakeError(DOMAIN, TEXT("set_output_object"), 2000, TEXT("Chooser not found"));

    UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath);
    if (!Cls) return MakeError(DOMAIN, TEXT("set_output_object"), 2000,
        FString::Printf(TEXT("Class not found: %s"), *ClassPath));

    Chooser->OutputObjectType = Cls;
    Chooser->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_output_object"),
        FString::Printf(TEXT("Output class -> '%s'"), *Cls->GetName()));
}
