#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "PlacementHandler.generated.h"

/**
 * PlacementHandler — domain "placement"  (UE 5.7)
 *
 * UPlacementSubsystem wrapper. Lets the bridge use Placement Mode's
 * factory-aware "smart placement" path (handles foliage, BPs, classes, assets)
 * instead of raw World->SpawnActor.
 *
 * Actions:
 *   place_asset           → asset_path, location {x,y,z}, rotation? {pitch,yaw,roll}
 *   place_assets          → assets[] (array of {asset_path, location, rotation?})
 *   get_factory_for_asset → asset_path → returns factory class name
 */
UCLASS()
class FORGEEDITORBRIDGE_API UPlacementHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("placement"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return { TEXT("place_asset"), TEXT("place_assets"), TEXT("get_factory_for_asset") };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_PlaceAsset           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_PlaceAssets          (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetFactoryForAsset   (TSharedPtr<FJsonObject> Params);
};
