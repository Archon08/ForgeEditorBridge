#include "Handlers/DecalHandler.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("decal");

namespace
{
    UWorld* GetEditorWorld()
    {
        return UBridgeHandlerBase::GetSafeEditorWorld();
    }

    ADecalActor* FindDecalByLabel(UWorld* World, const FString& Label)
    {
        if (!World || Label.IsEmpty()) return nullptr;
        for (TActorIterator<ADecalActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Label) return *It;
        }
        return nullptr;
    }

    UMaterialInterface* LoadMaterial(const FString& Path)
    {
        if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path)) return M;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UMaterialInterface>(nullptr, *Suffix);
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
}

FBridgeResult UDecalHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("spawn_decal"))           return Action_SpawnDecal(Params);
    if (Action == TEXT("set_decal_size"))        return Action_SetDecalSize(Params);
    if (Action == TEXT("set_decal_material"))    return Action_SetDecalMaterial(Params);
    if (Action == TEXT("set_sort_order"))        return Action_SetSortOrder(Params);
    if (Action == TEXT("set_fade_in"))           return Action_SetFadeIn(Params);
    if (Action == TEXT("set_fade_out"))          return Action_SetFadeOut(Params);
    if (Action == TEXT("set_fade_screen_size"))  return Action_SetFadeScreenSize(Params);
    if (Action == TEXT("list_decals"))           return Action_ListDecals(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("spawn_decal, set_decal_size, set_decal_material, set_sort_order, set_fade_in, set_fade_out, set_fade_screen_size, list_decals"));
}

FBridgeResult UDecalHandler::Action_SpawnDecal(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: spawn"));
    FString MaterialPath, Label;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("spawn_decal"), 1000, TEXT("'material_path' is required"));

    FVector Loc = FVector::ZeroVector;
    if (!ReadVec(Params, TEXT("location"), Loc))
        return MakeError(DOMAIN, TEXT("spawn_decal"), 1000, TEXT("'location' {x,y,z} is required"));

    FRotator Rot = FRotator::ZeroRotator;
    ReadRot(Params, TEXT("rotation"), Rot);

    double SizeX=256, SizeY=256, SizeZ=256;
    Params->TryGetNumberField(TEXT("size_x"), SizeX);
    Params->TryGetNumberField(TEXT("size_y"), SizeY);
    Params->TryGetNumberField(TEXT("size_z"), SizeZ);
    int32 SortOrder = 0;
    Params->TryGetNumberField(TEXT("sort_order"), SortOrder);
    double FadeIn = 0.0;
    Params->TryGetNumberField(TEXT("fade_in_duration"), FadeIn);
    Params->TryGetStringField(TEXT("label"), Label);

    UWorld* World = GetEditorWorld();
    if (!World) return MakeError(DOMAIN, TEXT("spawn_decal"), 3000, TEXT("No editor world"));

    UMaterialInterface* Mat = LoadMaterial(MaterialPath);
    if (!Mat) return MakeError(DOMAIN, TEXT("spawn_decal"), 2000,
        FString::Printf(TEXT("Decal material not found: %s"), *MaterialPath));

    FActorSpawnParameters Sp;
    Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADecalActor* Decal = World->SpawnActor<ADecalActor>(Loc, Rot, Sp);
    if (!Decal) return MakeError(DOMAIN, TEXT("spawn_decal"), 3000, TEXT("World::SpawnActor returned null"));

    Decal->SetDecalMaterial(Mat);
    if (UDecalComponent* C = Decal->GetDecal())
    {
        C->DecalSize = FVector(SizeX, SizeY, SizeZ);
        C->SortOrder = SortOrder;
        if (FadeIn > 0.0) C->SetFadeIn(0.0f, (float)FadeIn);
    }
    if (!Label.IsEmpty()) Decal->SetActorLabel(Label);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_label"), Decal->GetActorLabel());
    Data->SetStringField(TEXT("actor_path"),  Decal->GetPathName());
    FBridgeResult R = MakeSuccess(DOMAIN, TEXT("spawn_decal"),
        FString::Printf(TEXT("Spawned decal '%s' at (%.1f,%.1f,%.1f)"),
            *Decal->GetActorLabel(), Loc.X, Loc.Y, Loc.Z), Data);
    R.AffectedPath = Decal->GetActorLabel();
    return R;
}

FBridgeResult UDecalHandler::Action_SetDecalSize(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_size"));
    FString Label;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_decal_size"), 1000, TEXT("'actor_label' is required"));
    double X=0, Y=0, Z=0;
    if (!Params->TryGetNumberField(TEXT("size_x"), X) ||
        !Params->TryGetNumberField(TEXT("size_y"), Y) ||
        !Params->TryGetNumberField(TEXT("size_z"), Z))
        return MakeError(DOMAIN, TEXT("set_decal_size"), 1000, TEXT("'size_x', 'size_y', 'size_z' are required"));

    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D) return MakeError(DOMAIN, TEXT("set_decal_size"), 2000, TEXT("Decal not found"));
    if (UDecalComponent* C = D->GetDecal()) C->DecalSize = FVector(X,Y,Z);
    return MakeSuccess(DOMAIN, TEXT("set_decal_size"),
        FString::Printf(TEXT("Resized '%s' to (%.0f,%.0f,%.0f)"), *Label, X, Y, Z));
}

FBridgeResult UDecalHandler::Action_SetDecalMaterial(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_material"));
    FString Label, MatPath;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_decal_material"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetStringField(TEXT("material_path"), MatPath) || MatPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_decal_material"), 1000, TEXT("'material_path' is required"));
    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D) return MakeError(DOMAIN, TEXT("set_decal_material"), 2000, TEXT("Decal not found"));
    UMaterialInterface* Mat = LoadMaterial(MatPath);
    if (!Mat) return MakeError(DOMAIN, TEXT("set_decal_material"), 2000, TEXT("Material not found"));
    D->SetDecalMaterial(Mat);
    return MakeSuccess(DOMAIN, TEXT("set_decal_material"),
        FString::Printf(TEXT("'%s' material -> '%s'"), *Label, *MatPath));
}

FBridgeResult UDecalHandler::Action_SetSortOrder(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_sort"));
    FString Label;
    int32 Order = 0;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_sort_order"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetNumberField(TEXT("sort_order"), Order))
        return MakeError(DOMAIN, TEXT("set_sort_order"), 1000, TEXT("'sort_order' is required"));
    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D || !D->GetDecal()) return MakeError(DOMAIN, TEXT("set_sort_order"), 2000, TEXT("Decal not found"));
    D->GetDecal()->SortOrder = Order;
    return MakeSuccess(DOMAIN, TEXT("set_sort_order"),
        FString::Printf(TEXT("'%s' sort_order=%d"), *Label, Order));
}

FBridgeResult UDecalHandler::Action_SetFadeIn(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_fade_in"));
    FString Label;
    double Duration = 0.0, StartDelay = 0.0;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_fade_in"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetNumberField(TEXT("duration"), Duration))
        return MakeError(DOMAIN, TEXT("set_fade_in"), 1000, TEXT("'duration' is required"));
    Params->TryGetNumberField(TEXT("start_delay"), StartDelay);
    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D || !D->GetDecal()) return MakeError(DOMAIN, TEXT("set_fade_in"), 2000, TEXT("Decal not found"));
    D->GetDecal()->SetFadeIn((float)StartDelay, (float)Duration);
    return MakeSuccess(DOMAIN, TEXT("set_fade_in"),
        FString::Printf(TEXT("'%s' fade_in=%.2fs (delay=%.2fs)"), *Label, Duration, StartDelay));
}

FBridgeResult UDecalHandler::Action_SetFadeOut(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_fade_out"));
    FString Label;
    double Duration = 0.0, StartDelay = 0.0;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_fade_out"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetNumberField(TEXT("duration"), Duration))
        return MakeError(DOMAIN, TEXT("set_fade_out"), 1000, TEXT("'duration' is required"));
    Params->TryGetNumberField(TEXT("start_delay"), StartDelay);
    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D || !D->GetDecal()) return MakeError(DOMAIN, TEXT("set_fade_out"), 2000, TEXT("Decal not found"));
    D->GetDecal()->SetFadeOut((float)StartDelay, (float)Duration);
    return MakeSuccess(DOMAIN, TEXT("set_fade_out"),
        FString::Printf(TEXT("'%s' fade_out=%.2fs (delay=%.2fs)"), *Label, Duration, StartDelay));
}

FBridgeResult UDecalHandler::Action_SetFadeScreenSize(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Decal: set_fade_screen_size"));
    FString Label;
    double ScreenSize = 0.01;
    if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_fade_screen_size"), 1000, TEXT("'actor_label' is required"));
    if (!Params->TryGetNumberField(TEXT("fade_screen_size"), ScreenSize))
        return MakeError(DOMAIN, TEXT("set_fade_screen_size"), 1000, TEXT("'fade_screen_size' is required"));
    ADecalActor* D = FindDecalByLabel(GetEditorWorld(), Label);
    if (!D || !D->GetDecal()) return MakeError(DOMAIN, TEXT("set_fade_screen_size"), 2000, TEXT("Decal not found"));
    D->GetDecal()->FadeScreenSize = (float)ScreenSize;
    return MakeSuccess(DOMAIN, TEXT("set_fade_screen_size"),
        FString::Printf(TEXT("'%s' fade_screen_size=%.4f"), *Label, ScreenSize));
}

FBridgeResult UDecalHandler::Action_ListDecals(TSharedPtr<FJsonObject> Params)
{
    UWorld* World = GetEditorWorld();
    if (!World) return MakeError(DOMAIN, TEXT("list_decals"), 3000, TEXT("No editor world"));
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (TActorIterator<ADecalActor> It(World); It; ++It)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("label"), It->GetActorLabel());
        Entry->SetStringField(TEXT("path"),  It->GetPathName());
        UDecalComponent* C = It->GetDecal();
        if (C)
        {
            Entry->SetNumberField(TEXT("sort_order"), C->SortOrder);
            UMaterialInterface* M = C->GetDecalMaterial();
            Entry->SetStringField(TEXT("material"), M ? M->GetPathName() : FString());
        }
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("decals"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_decals"),
        FString::Printf(TEXT("%d decal(s)"), Arr.Num()), Data);
}
