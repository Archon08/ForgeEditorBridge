#include "Attention/BridgeAttentionManager.h"
#include "ForgeAISubsystem.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

void UBridgeAttentionManager::Initialize(UBridgeSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    CachedTargetWBP = nullptr;
    CursorPosition  = FVector2D(200.f, 200.f);

    // Auto-detect any currently open WBP editor as initial target
    if (GEditor)
    {
        UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AES)
        {
            AES->OnAssetOpenedInEditor().AddWeakLambda(this,
                [this](UObject* Asset, IAssetEditorInstance*)
                {
                    if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
                        RecordRecentAsset(WBP->GetPathName());
                });
        }
    }
}

bool UBridgeAttentionManager::SetTargetAssetPath(const FString& AssetPath)
{
    if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
    {
        TargetAssetPath.Empty();
        CachedTargetWBP = nullptr;
        return false;
    }

    UWidgetBlueprint* TargetBP = nullptr;

    // 1. Look for open editor first (live instance)
    if (GEditor)
    {
        UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AES)
        {
            FSoftObjectPath ObjPath(AssetPath);
            if (UObject* Obj = ObjPath.ResolveObject())
            {
                if (AES->FindEditorForAsset(Obj, false))
                    TargetBP = Cast<UWidgetBlueprint>(Obj);
            }
        }
    }

    // 2. LoadObject fallback
    if (!TargetBP)
        TargetBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath, nullptr, LOAD_NoWarn);

    // 3. Create if path is valid but asset is missing
    if (!TargetBP && FPackageName::IsValidPath(AssetPath))
    {
        FString AssetName = FPaths::GetBaseFilename(AssetPath);
        FString PackagePath = FPaths::GetPath(AssetPath);
        IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        TargetBP = Cast<UWidgetBlueprint>(AT.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory));
    }

    if (TargetBP)
    {
        TargetAssetPath = AssetPath;
        CachedTargetWBP = TargetBP;
        RecordRecentAsset(AssetPath);
        return true;
    }

    TargetAssetPath.Empty();
    CachedTargetWBP = nullptr;
    return false;
}

FString UBridgeAttentionManager::GetTargetAssetPath() const
{
    // Lazy-reload if cached pointer is stale
    if (!TargetAssetPath.IsEmpty() && !CachedTargetWBP.IsValid())
    {
        UWidgetBlueprint* Reloaded = LoadObject<UWidgetBlueprint>(nullptr, *TargetAssetPath);
        if (Reloaded)
            const_cast<UBridgeAttentionManager*>(this)->CachedTargetWBP = Reloaded;
    }

    if (!TargetAssetPath.IsEmpty())
        return TargetAssetPath;

    // Fallback: first open WBP editor
    if (GEditor)
    {
        UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AES)
        {
            for (UObject* Asset : AES->GetAllEditedAssets())
            {
                if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
                {
                    const_cast<UBridgeAttentionManager*>(this)->CachedTargetWBP = WBP;
                    return WBP->GetPathName();
                }
            }
        }
    }

    return GetLastEditedAsset();
}

UWidgetBlueprint* UBridgeAttentionManager::GetCachedWBP() const
{
    GetTargetAssetPath(); // triggers lazy-load
    if (CachedTargetWBP.IsValid())
        return CachedTargetWBP.Get();

    // Fallback to last edited
    FString Last = GetLastEditedAsset();
    if (!Last.IsEmpty())
    {
        UWidgetBlueprint* Loaded = LoadObject<UWidgetBlueprint>(nullptr, *Last);
        if (Loaded)
        {
            const_cast<UBridgeAttentionManager*>(this)->CachedTargetWBP = Loaded;
            return Loaded;
        }
    }
    return nullptr;
}

void UBridgeAttentionManager::RecordRecentAsset(const FString& AssetPath)
{
    AssetHistory.Remove(AssetPath);
    AssetHistory.Insert(AssetPath, 0);
    if (AssetHistory.Num() > 10)
        AssetHistory.SetNum(10);
}

FString UBridgeAttentionManager::GetLastEditedAsset() const
{
    return AssetHistory.Num() > 0 ? AssetHistory[0] : FString();
}

TArray<FString> UBridgeAttentionManager::GetRecentAssets(int32 MaxCount) const
{
    TArray<FString> Out;
    int32 N = FMath::Min(AssetHistory.Num(), MaxCount);
    for (int32 i = 0; i < N; ++i)
        Out.Add(AssetHistory[i]);
    return Out;
}

void UBridgeAttentionManager::SetTargetGraph(const FString& GraphName)
{
    TargetGraphName = GraphName;
    CursorNodeId.Empty();
    CursorPosition = FVector2D(200.f, 200.f);
}

FString UBridgeAttentionManager::GetTargetGraph() const
{
    return TargetGraphName.IsEmpty() ? TEXT("EventGraph") : TargetGraphName;
}

void UBridgeAttentionManager::SetCursorNode(const FString& NodeId)
{
    CursorNodeId = NodeId;
}

FString UBridgeAttentionManager::GetCursorNode() const
{
    return CursorNodeId;
}

FVector2D UBridgeAttentionManager::GetAndAdvanceCursorPosition()
{
    FVector2D Out = CursorPosition;
    CursorPosition.X += CursorStepX;
    return Out;
}

void UBridgeAttentionManager::SetCursorPosition(const FVector2D& NewPosition)
{
    CursorPosition = NewPosition;
}

void UBridgeAttentionManager::SetActiveWidget(const FString& WidgetName)
{
    ActiveWidgetName = WidgetName;
}

void UBridgeAttentionManager::SetAnimationScope(const FString& AnimName)
{
    AnimationScopeName = AnimName;
}

void UBridgeAttentionManager::SetWidgetScope(const FString& WidgetName)
{
    WidgetScopeName = WidgetName;
}

void UBridgeAttentionManager::SetMaterialTargetPath(const FString& Path)
{
    MaterialTargetPath = Path;
}
