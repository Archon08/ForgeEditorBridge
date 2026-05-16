#include "Handlers/PoseSearchHandler.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("posesearch");

namespace
{
    template <typename T>
    T* LoadAt(const FString& Path)
    {
        if (T* O = LoadObject<T>(nullptr, *Path)) return O;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<T>(nullptr, *Suffix);
    }

    template <typename T>
    T* CreateAt(const FString& AssetPath, FString& OutErr)
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package) { OutErr = FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName); return nullptr; }
        Package->FullyLoad();
        T* New = NewObject<T>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!New) { OutErr = TEXT("NewObject failed"); return nullptr; }
        FAssetRegistryModule::AssetCreated(New);
        Package->MarkPackageDirty();
        return New;
    }
}

FBridgeResult UPoseSearchHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_schema"))       return Action_CreateSchema(Params);
    if (Action == TEXT("create_database"))     return Action_CreateDatabase(Params);
    if (Action == TEXT("set_database_schema")) return Action_SetDatabaseSchema(Params);
    if (Action == TEXT("build_index"))         return Action_BuildIndex(Params);
    if (Action == TEXT("get_database_info"))   return Action_GetDatabaseInfo(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_schema, create_database, set_database_schema, build_index, get_database_info"));
}

FBridgeResult UPoseSearchHandler::Action_CreateSchema(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_schema"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UPoseSearchSchema* S = CreateAt<UPoseSearchSchema>(AssetPath, Err);
    if (!S) return MakeError(DOMAIN, TEXT("create_schema"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_schema"),
        FString::Printf(TEXT("Created UPoseSearchSchema at '%s'"), *AssetPath));
}

FBridgeResult UPoseSearchHandler::Action_CreateDatabase(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_database"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UPoseSearchDatabase* DB = CreateAt<UPoseSearchDatabase>(AssetPath, Err);
    if (!DB) return MakeError(DOMAIN, TEXT("create_database"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_database"),
        FString::Printf(TEXT("Created UPoseSearchDatabase at '%s'"), *AssetPath));
}

FBridgeResult UPoseSearchHandler::Action_SetDatabaseSchema(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("PoseSearch Set DB Schema"));
    FString DBPath, SchemaPath;
    if (!Params->TryGetStringField(TEXT("db_path"), DBPath) || DBPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_database_schema"), 1000, TEXT("'db_path' is required"));
    if (!Params->TryGetStringField(TEXT("schema_path"), SchemaPath) || SchemaPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_database_schema"), 1000, TEXT("'schema_path' is required"));

    UPoseSearchDatabase* DB = LoadAt<UPoseSearchDatabase>(DBPath);
    if (!DB) return MakeError(DOMAIN, TEXT("set_database_schema"), 2000, TEXT("Database not found"));
    UPoseSearchSchema* Schema = LoadAt<UPoseSearchSchema>(SchemaPath);
    if (!Schema) return MakeError(DOMAIN, TEXT("set_database_schema"), 2000, TEXT("Schema not found"));

    DB->Schema = Schema;
    DB->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_database_schema"),
        FString::Printf(TEXT("DB '%s' schema -> '%s'"), *DBPath, *SchemaPath));
}

FBridgeResult UPoseSearchHandler::Action_BuildIndex(TSharedPtr<FJsonObject> Params)
{
    FString DBPath;
    if (!Params->TryGetStringField(TEXT("db_path"), DBPath) || DBPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("build_index"), 1000, TEXT("'db_path' is required"));
    UPoseSearchDatabase* DB = LoadAt<UPoseSearchDatabase>(DBPath);
    if (!DB) return MakeError(DOMAIN, TEXT("build_index"), 2000, TEXT("Database not found"));

    // Trigger reindex via post-edit pipeline (reliable across 5.5-5.7 minor changes).
    FProperty* AnyProp = DB->GetClass()->PropertyLink;
    FPropertyChangedEvent Evt(AnyProp);
    DB->PostEditChangeProperty(Evt);
    DB->MarkPackageDirty();

    return MakeSuccess(DOMAIN, TEXT("build_index"),
        FString::Printf(TEXT("Reindex requested for '%s'"), *DBPath));
}

FBridgeResult UPoseSearchHandler::Action_GetDatabaseInfo(TSharedPtr<FJsonObject> Params)
{
    FString DBPath;
    if (!Params->TryGetStringField(TEXT("db_path"), DBPath) || DBPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_database_info"), 1000, TEXT("'db_path' is required"));
    UPoseSearchDatabase* DB = LoadAt<UPoseSearchDatabase>(DBPath);
    if (!DB) return MakeError(DOMAIN, TEXT("get_database_info"), 2000, TEXT("Database not found"));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("db_path"), DBPath);
    Data->SetStringField(TEXT("schema_path"), DB->Schema ? DB->Schema->GetPathName() : FString());
    // 5.7: DatabaseAnimationAssets is private — count not directly accessible. Reflection workaround:
    int32 NumEntries = -1;
    if (FProperty* P = DB->GetClass()->FindPropertyByName(TEXT("DatabaseAnimationAssets")))
    {
        if (FArrayProperty* AP = CastField<FArrayProperty>(P))
        {
            FScriptArrayHelper Helper(AP, AP->ContainerPtrToValuePtr<void>(DB));
            NumEntries = Helper.Num();
        }
    }
    Data->SetNumberField(TEXT("num_entries"), NumEntries);
    return MakeSuccess(DOMAIN, TEXT("get_database_info"), TEXT("DB info"), Data);
}
