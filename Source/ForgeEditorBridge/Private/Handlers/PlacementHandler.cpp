#include "Handlers/PlacementHandler.h"
#include "Subsystems/PlacementSubsystem.h"
#include "Factories/AssetFactoryInterface.h"  // 5.7: TScriptInterface<IAssetFactoryInterface>
#include "Elements/Actor/ActorElementData.h"
#include "Elements/Framework/TypedElementHandle.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("placement");

namespace
{
    UPlacementSubsystem* GetPS()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UPlacementSubsystem>() : nullptr;
    }

    bool ResolveAssetData(const FString& AssetPath, FAssetData& OutData)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        OutData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath));
        if (OutData.IsValid()) return true;

        // Try with object suffix variant
        const FString WithSuffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        OutData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(WithSuffix));
        return OutData.IsValid();
    }

    bool ReadVec(const TSharedPtr<FJsonObject>& Params, const FString& Key, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params->TryGetObjectField(Key, Obj) || !Obj || !Obj->IsValid()) return false;
        double X=0, Y=0, Z=0;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        (*Obj)->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X,Y,Z);
        return true;
    }

    bool ReadRot(const TSharedPtr<FJsonObject>& Params, const FString& Key, FRotator& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params->TryGetObjectField(Key, Obj) || !Obj || !Obj->IsValid()) return false;
        double P=0, Yw=0, R=0;
        (*Obj)->TryGetNumberField(TEXT("pitch"), P);
        (*Obj)->TryGetNumberField(TEXT("yaw"),   Yw);
        (*Obj)->TryGetNumberField(TEXT("roll"),  R);
        Out = FRotator(P, Yw, R);
        return true;
    }

    AActor* PlaceOne(UPlacementSubsystem* PS, const FAssetData& AssetData,
                     const FTransform& Xform, FString& OutErr)
    {
        FAssetPlacementInfo Info;
        Info.AssetToPlace      = AssetData;
        Info.FinalizedTransform = Xform;

        FPlacementOptions Options;
        Options.bIsCreatingPreviewElements = false;
        Options.bPreferBatchPlacement      = false;

        TArray<FTypedElementHandle> Elements = PS->PlaceAsset(Info, Options);
        if (Elements.Num() == 0)
        {
            OutErr = TEXT("PlaceAsset returned no elements (no factory matched)");
            return nullptr;
        }
        for (const FTypedElementHandle& H : Elements)
        {
            if (AActor* A = ActorElementDataUtil::GetActorFromHandle(H))
            {
                return A;
            }
        }
        OutErr = TEXT("PlaceAsset succeeded but no actor element found in result");
        return nullptr;
    }
}

FBridgeResult UPlacementHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("place_asset"))            return Action_PlaceAsset(Params);
    if (Action == TEXT("place_assets"))           return Action_PlaceAssets(Params);
    if (Action == TEXT("get_factory_for_asset"))  return Action_GetFactoryForAsset(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("place_asset, place_assets, get_factory_for_asset"));
}

FBridgeResult UPlacementHandler::Action_PlaceAsset(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Placement: place_asset"));

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("place_asset"), 1000, TEXT("'asset_path' is required"));

    FVector Loc = FVector::ZeroVector;
    FRotator Rot = FRotator::ZeroRotator;
    if (!ReadVec(Params, TEXT("location"), Loc))
        return MakeError(DOMAIN, TEXT("place_asset"), 1000,
            TEXT("'location' {x,y,z} is required"));
    ReadRot(Params, TEXT("rotation"), Rot);

    UPlacementSubsystem* PS = GetPS();
    if (!PS) return MakeError(DOMAIN, TEXT("place_asset"), 3000, TEXT("UPlacementSubsystem unavailable"));

    FAssetData AssetData;
    if (!ResolveAssetData(AssetPath, AssetData))
        return MakeError(DOMAIN, TEXT("place_asset"), 2000,
            FString::Printf(TEXT("Asset not found in registry: %s"), *AssetPath));

    FString Err;
    AActor* Placed = PlaceOne(PS, AssetData, FTransform(Rot, Loc), Err);
    if (!Placed)
        return MakeError(DOMAIN, TEXT("place_asset"), 3000, Err);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_label"), Placed->GetActorLabel());
    Data->SetStringField(TEXT("actor_path"),  Placed->GetPathName());
    FBridgeResult R = MakeSuccess(DOMAIN, TEXT("place_asset"),
        FString::Printf(TEXT("Placed '%s' as '%s'"), *AssetPath, *Placed->GetActorLabel()), Data);
    R.AffectedPath = Placed->GetActorLabel();
    return R;
}

FBridgeResult UPlacementHandler::Action_PlaceAssets(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Placement: place_assets"));

    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Params->TryGetArrayField(TEXT("assets"), Arr) || !Arr)
        return MakeError(DOMAIN, TEXT("place_assets"), 1000,
            TEXT("'assets' (array) is required"));

    UPlacementSubsystem* PS = GetPS();
    if (!PS) return MakeError(DOMAIN, TEXT("place_assets"), 3000, TEXT("UPlacementSubsystem unavailable"));

    int32 Placed = 0, Failed = 0;
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const TSharedPtr<FJsonValue>& V : *Arr)
    {
        TSharedPtr<FJsonObject> Item = V->AsObject();
        if (!Item.IsValid()) { ++Failed; continue; }
        FString AssetPath;
        FVector Loc = FVector::ZeroVector;
        FRotator Rot = FRotator::ZeroRotator;
        if (!Item->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()) { ++Failed; continue; }
        ReadVec(Item, TEXT("location"), Loc);
        ReadRot(Item, TEXT("rotation"), Rot);

        FAssetData AssetData;
        if (!ResolveAssetData(AssetPath, AssetData)) { ++Failed; continue; }
        FString Err;
        AActor* A = PlaceOne(PS, AssetData, FTransform(Rot, Loc), Err);
        if (A)
        {
            ++Placed;
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("asset_path"), AssetPath);
            Entry->SetStringField(TEXT("actor_label"), A->GetActorLabel());
            Out.Add(MakeShared<FJsonValueObject>(Entry));
        }
        else
        {
            ++Failed;
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("placed"), Placed);
    Data->SetNumberField(TEXT("failed"), Failed);
    Data->SetArrayField(TEXT("placed_actors"), Out);
    return MakeSuccess(DOMAIN, TEXT("place_assets"),
        FString::Printf(TEXT("Placed %d/%d (failed=%d)"), Placed, Arr->Num(), Failed), Data);
}

FBridgeResult UPlacementHandler::Action_GetFactoryForAsset(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_factory_for_asset"), 1000, TEXT("'asset_path' is required"));

    UPlacementSubsystem* PS = GetPS();
    if (!PS) return MakeError(DOMAIN, TEXT("get_factory_for_asset"), 3000, TEXT("UPlacementSubsystem unavailable"));

    FAssetData AssetData;
    if (!ResolveAssetData(AssetPath, AssetData))
        return MakeError(DOMAIN, TEXT("get_factory_for_asset"), 2000,
            FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

    // 5.7: FindAssetFactoryFromAssetData returns TScriptInterface<IAssetFactoryInterface>.
    TScriptInterface<IAssetFactoryInterface> FactoryInterface =
        PS->FindAssetFactoryFromAssetData(AssetData);
    UObject* FactoryObj = FactoryInterface.GetObject();
    const FString FactoryClassName = FactoryObj ? FactoryObj->GetClass()->GetName() : FString(TEXT("(none)"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("factory_class"), FactoryClassName);
    Data->SetBoolField(TEXT("placeable"), FactoryObj != nullptr);
    return MakeSuccess(DOMAIN, TEXT("get_factory_for_asset"),
        FactoryObj ? FactoryClassName : FString(TEXT("No matching factory")), Data);
}
