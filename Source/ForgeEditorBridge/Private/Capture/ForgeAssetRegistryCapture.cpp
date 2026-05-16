#include "Capture/ForgeAssetRegistryCapture.h"
#include "IO/ForgeContextWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgeAssetRegistryCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("assets")));
}

void UForgeAssetRegistryCapture::Deinitialize()
{
    // No delegates or tickers to unregister
}

// ============================================================
// ExportAssetRegistry — full /Game/ scan
// ============================================================

int32 UForgeAssetRegistryCapture::ExportAssetRegistry()
{
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = ARModule.Get();

    // Block until the registry has finished its background scan.
    // Acceptable at Initialize time; avoids partial results.
    AssetRegistry.SearchAllAssets(true);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName("/Game"));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    if (Assets.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeAssetRegistry: No assets found under /Game/"));
        return 0;
    }

    // --- Aggregates ---
    int32 TotalAssets = Assets.Num();
    TMap<FString, int32> ByType;
    for (const FAssetData& Asset : Assets)
    {
        FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
        ByType.FindOrAdd(ClassName)++;
    }

    // --- Build JSON ---
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"),    FForgeContextWriter::NowISO8601());
    Root->SetNumberField(TEXT("total_assets"), TotalAssets);

    // by_type map
    TSharedPtr<FJsonObject> ByTypeJson = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : ByType)
    {
        ByTypeJson->SetNumberField(Pair.Key, Pair.Value);
    }
    Root->SetObjectField(TEXT("by_type"), ByTypeJson);

    // per-asset array
    TArray<TSharedPtr<FJsonValue>> AssetsArray;
    AssetsArray.Reserve(TotalAssets);

    for (const FAssetData& Asset : Assets)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"),    Asset.GetObjectPathString());
        Entry->SetStringField(TEXT("type"),    Asset.AssetClassPath.GetAssetName().ToString());
        Entry->SetStringField(TEXT("package"), Asset.PackageName.ToString());

        // Hard package dependencies — filtered to /Game/ only to keep output focused
        TArray<FName> RawDeps;
        AssetRegistry.GetDependencies(Asset.PackageName, RawDeps,
            UE::AssetRegistry::EDependencyCategory::Package);

        TArray<TSharedPtr<FJsonValue>> DepsArray;
        for (const FName& Dep : RawDeps)
        {
            FString DepStr = Dep.ToString();
            // Skip engine, plugin, and script packages — only /Game/ deps are actionable
            if (DepStr.StartsWith(TEXT("/Game/")))
            {
                DepsArray.Add(MakeShared<FJsonValueString>(DepStr));
            }
        }
        Entry->SetArrayField(TEXT("dependencies"), DepsArray);

        AssetsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("assets"), AssetsArray);

    bool bSuccess = FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("assets"), TEXT("registry.json"), Root);

    if (bSuccess)
    {
        UE_LOG(LogTemp, Log,
            TEXT("ForgeAssetRegistry: Exported %d assets (%d types) to assets/registry.json"),
            TotalAssets, ByType.Num());
        UpdateIndexFile(TotalAssets);
    }

    return bSuccess ? TotalAssets : -1;
}

// ============================================================
// ExportAssetsByFilter — single class filter
// ============================================================

int32 UForgeAssetRegistryCapture::ExportAssetsByFilter(const FString& ClassName)
{
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = ARModule.Get();

    AssetRegistry.SearchAllAssets(true);

    // Resolve the class name to a full path (e.g. "StaticMesh" -> "/Script/Engine.StaticMesh").
    // FTopLevelAssetPath requires the /Script/Module.ClassName form — we can't build it from
    // a bare name, so we look up the UClass first and use GetClassPathName().
    UClass* FoundClass = UClass::TryFindTypeSlow<UClass>(ClassName);
    if (!FoundClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeAssetRegistry: Class '%s' not found — cannot build filter"), *ClassName);
        return 0;
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(FName("/Game"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FoundClass->GetClassPathName());

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    if (Assets.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeAssetRegistry: No assets of type '%s' found under /Game/"), *ClassName);
        return 0;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"),    FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("filter_class"), ClassName);
    Root->SetNumberField(TEXT("total_assets"), Assets.Num());

    TArray<TSharedPtr<FJsonValue>> AssetsArray;
    AssetsArray.Reserve(Assets.Num());
    for (const FAssetData& Asset : Assets)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"),    Asset.GetObjectPathString());
        Entry->SetStringField(TEXT("package"), Asset.PackageName.ToString());
        AssetsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("assets"), AssetsArray);

    const FString Filename = TEXT("filter_") + ClassName.ToLower() + TEXT(".json");
    bool bSuccess = FForgeContextWriter::WriteJSON(OutputDir / TEXT("assets"), Filename, Root);

    if (bSuccess)
    {
        UE_LOG(LogTemp, Log,
            TEXT("ForgeAssetRegistry: Exported %d '%s' assets to assets/%s"),
            Assets.Num(), *ClassName, *Filename);
    }

    return bSuccess ? Assets.Num() : -1;
}

// ============================================================
// Index — READ-MERGE-WRITE preserving all other capture sections
// ============================================================

void UForgeAssetRegistryCapture::UpdateIndexFile(int32 TotalAssets)
{
    const FString IndexPath = OutputDir / TEXT("index.json");
    const FString Timestamp = FForgeContextWriter::NowISO8601();

    TSharedPtr<FJsonObject> Root;
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *IndexPath))
    {
        TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExistingContent);
        FJsonSerializer::Deserialize(R, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
        Captures = *ExistingCaptures;
    else
        Captures = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
    Section->SetStringField(TEXT("output_file"),  TEXT("assets/registry.json"));
    Section->SetNumberField(TEXT("total_assets"), TotalAssets);
    Section->SetStringField(TEXT("last_updated"), Timestamp);
    Captures->SetObjectField(TEXT("asset_registry"), Section);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), TEXT("0.2.6"));

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
