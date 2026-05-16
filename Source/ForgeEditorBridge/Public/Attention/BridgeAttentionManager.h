#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BridgeAttentionManager.generated.h"

class UForgeAISubsystem;
using UBridgeSubsystem = UForgeAISubsystem;
class UWidgetBlueprint;

/**
 * BridgeAttentionManager — stateful context tracker for the SEB AI session.
 *
 * Tracks: target WBP asset, active widget/animation scope, BP graph cursor
 * (position + last-placed node), and recent asset history.
 *
 * Owned by UBridgeSubsystem (UPROPERTY). Initialized on StartBridge().
 * Adapted from UmgAttentionSubsystem (MIT, winyunq/UnrealMotionGraphicsMCP).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UBridgeAttentionManager : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(UBridgeSubsystem* InSubsystem);

    // ---- Asset focus --------------------------------------------------------

    /** Load/create the WBP at AssetPath and cache it. Returns false if invalid. */
    bool SetTargetAssetPath(const FString& AssetPath);
    FString GetTargetAssetPath() const;
    UWidgetBlueprint* GetCachedWBP() const;

    /** Record that an asset was opened/edited (called by editor open delegate). */
    void RecordRecentAsset(const FString& AssetPath);
    FString GetLastEditedAsset() const;
    TArray<FString> GetRecentAssets(int32 MaxCount = 5) const;

    // ---- BP graph cursor ----------------------------------------------------

    void    SetTargetGraph(const FString& GraphName);
    FString GetTargetGraph() const;

    void    SetCursorNode(const FString& NodeId);
    FString GetCursorNode() const;

    /** Returns current cursor position, then advances X by CursorStepX. */
    FVector2D GetAndAdvanceCursorPosition();
    void    SetCursorPosition(const FVector2D& NewPosition);
    FVector2D GetCursorPosition() const { return CursorPosition; }

    // ---- Widget / animation scope ------------------------------------------

    void    SetActiveWidget(const FString& WidgetName);
    FString GetActiveWidget() const { return ActiveWidgetName; }

    void    SetAnimationScope(const FString& AnimName);
    FString GetAnimationScope() const { return AnimationScopeName; }

    void    SetWidgetScope(const FString& WidgetName);
    FString GetWidgetScope() const { return WidgetScopeName; }

    // ---- Material context --------------------------------------------------

    void    SetMaterialTargetPath(const FString& Path);
    FString GetMaterialTargetPath() const { return MaterialTargetPath; }

private:
    FString TargetAssetPath;
    TWeakObjectPtr<UWidgetBlueprint> CachedTargetWBP;
    TArray<FString> AssetHistory;   // MRU, capped at 10

    FString TargetGraphName   = TEXT("EventGraph");
    FString CursorNodeId;
    FVector2D CursorPosition  = FVector2D(200.f, 200.f);
    static constexpr float CursorStepX = 250.f;

    FString ActiveWidgetName;
    FString AnimationScopeName;
    FString WidgetScopeName;
    FString MaterialTargetPath;

    UPROPERTY()
    TObjectPtr<UForgeAISubsystem> Subsystem;
};
