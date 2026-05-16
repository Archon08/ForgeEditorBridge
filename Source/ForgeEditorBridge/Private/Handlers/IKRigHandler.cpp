#include "Handlers/IKRigHandler.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("ikrig");

namespace
{
    UIKRigDefinition* LoadRig(const FString& Path)
    {
        if (UIKRigDefinition* R = LoadObject<UIKRigDefinition>(nullptr, *Path)) return R;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<UIKRigDefinition>(nullptr, *Suffix);
    }
    USkeletalMesh* LoadMesh(const FString& Path)
    {
        if (USkeletalMesh* M = LoadObject<USkeletalMesh>(nullptr, *Path)) return M;
        const FString Suffix = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        return LoadObject<USkeletalMesh>(nullptr, *Suffix);
    }
}

FBridgeResult UIKRigHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("create_ik_rig"))      return Action_CreateIKRig(Params);
    if (Action == TEXT("set_skeletal_mesh"))  return Action_SetSkeletalMesh(Params);
    if (Action == TEXT("add_chain"))          return Action_AddChain(Params);
    if (Action == TEXT("list_chains"))        return Action_ListChains(Params);
    if (Action == TEXT("add_goal"))           return Action_AddGoal(Params);
    if (Action == TEXT("list_goals"))         return Action_ListGoals(Params);
    if (Action == TEXT("get_info"))           return Action_GetInfo(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_ik_rig, set_skeletal_mesh, add_chain, list_chains, add_goal, list_goals, get_info"));
}

FBridgeResult UIKRigHandler::Action_CreateIKRig(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath, MeshPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_ik_rig"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath);

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return MakeError(DOMAIN, TEXT("create_ik_rig"), 3000,
        FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
    Package->FullyLoad();

    UIKRigDefinition* Rig = NewObject<UIKRigDefinition>(Package, FName(*AssetName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Rig) return MakeError(DOMAIN, TEXT("create_ik_rig"), 3000, TEXT("NewObject failed"));
    FAssetRegistryModule::AssetCreated(Rig);

    if (!MeshPath.IsEmpty())
    {
        if (USkeletalMesh* Mesh = LoadMesh(MeshPath))
        {
            UIKRigController* Ctrl = UIKRigController::GetController(Rig);
            if (Ctrl) Ctrl->SetSkeletalMesh(Mesh);
        }
    }
    Package->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("create_ik_rig"),
        FString::Printf(TEXT("Created UIKRigDefinition at '%s'"), *AssetPath));
}

FBridgeResult UIKRigHandler::Action_SetSkeletalMesh(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("IKRig: set_skeletal_mesh"));
    FString AssetPath, MeshPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_skeletal_mesh"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath) || MeshPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_skeletal_mesh"), 1000, TEXT("'skeletal_mesh_path' is required"));

    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("set_skeletal_mesh"), 2000, TEXT("Rig not found"));
    USkeletalMesh* Mesh = LoadMesh(MeshPath);
    if (!Mesh) return MakeError(DOMAIN, TEXT("set_skeletal_mesh"), 2000, TEXT("Mesh not found"));

    UIKRigController* Ctrl = UIKRigController::GetController(Rig);
    if (!Ctrl) return MakeError(DOMAIN, TEXT("set_skeletal_mesh"), 3000, TEXT("Could not get IKRigController"));
    Ctrl->SetSkeletalMesh(Mesh);
    Rig->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_skeletal_mesh"),
        FString::Printf(TEXT("Set mesh '%s' on rig"), *MeshPath));
}

FBridgeResult UIKRigHandler::Action_AddChain(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("IKRig: add_chain"));
    FString AssetPath, ChainName, StartBone, EndBone, GoalName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_chain"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("chain_name"), ChainName) || ChainName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_chain"), 1000, TEXT("'chain_name' is required"));
    if (!Params->TryGetStringField(TEXT("start_bone"), StartBone) || StartBone.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_chain"), 1000, TEXT("'start_bone' is required"));
    if (!Params->TryGetStringField(TEXT("end_bone"), EndBone) || EndBone.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_chain"), 1000, TEXT("'end_bone' is required"));
    Params->TryGetStringField(TEXT("goal_name"), GoalName);

    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("add_chain"), 2000, TEXT("Rig not found"));
    UIKRigController* Ctrl = UIKRigController::GetController(Rig);
    if (!Ctrl) return MakeError(DOMAIN, TEXT("add_chain"), 3000, TEXT("Could not get IKRigController"));

    const FName ChainFName(*ChainName);
    const FName StartFName(*StartBone);
    const FName EndFName(*EndBone);
    const FName GoalFName = GoalName.IsEmpty() ? NAME_None : FName(*GoalName);
    const FName Added = Ctrl->AddRetargetChain(ChainFName, StartFName, EndFName, GoalFName);
    if (Added == NAME_None)
        return MakeError(DOMAIN, TEXT("add_chain"), 3000, TEXT("AddRetargetChain returned NAME_None"));

    Rig->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("add_chain"),
        FString::Printf(TEXT("Added chain '%s' (%s -> %s)"), *Added.ToString(), *StartBone, *EndBone));
}

FBridgeResult UIKRigHandler::Action_ListChains(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_chains"), 1000, TEXT("'asset_path' is required"));
    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("list_chains"), 2000, TEXT("Rig not found"));

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FBoneChain& Chain : Rig->GetRetargetChains())
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),       Chain.ChainName.ToString());
        O->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
        O->SetStringField(TEXT("end_bone"),   Chain.EndBone.BoneName.ToString());
        O->SetStringField(TEXT("goal"),       Chain.IKGoalName.ToString());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("chains"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_chains"),
        FString::Printf(TEXT("%d chain(s)"), Arr.Num()), Data);
}

FBridgeResult UIKRigHandler::Action_AddGoal(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("IKRig: add_goal"));
    FString AssetPath, GoalName, BoneName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_goal"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetStringField(TEXT("goal_name"), GoalName) || GoalName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_goal"), 1000, TEXT("'goal_name' is required"));
    if (!Params->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_goal"), 1000, TEXT("'bone_name' is required"));

    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("add_goal"), 2000, TEXT("Rig not found"));
    UIKRigController* Ctrl = UIKRigController::GetController(Rig);
    if (!Ctrl) return MakeError(DOMAIN, TEXT("add_goal"), 3000, TEXT("Could not get IKRigController"));

    const FName Added = Ctrl->AddNewGoal(FName(*GoalName), FName(*BoneName));
    if (Added == NAME_None)
        return MakeError(DOMAIN, TEXT("add_goal"), 3000, TEXT("AddNewGoal returned NAME_None"));
    Rig->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("add_goal"),
        FString::Printf(TEXT("Added goal '%s' on bone '%s'"), *Added.ToString(), *BoneName));
}

FBridgeResult UIKRigHandler::Action_ListGoals(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_goals"), 1000, TEXT("'asset_path' is required"));
    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("list_goals"), 2000, TEXT("Rig not found"));

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const UIKRigEffectorGoal* Goal : Rig->GetGoalArray())
    {
        if (!Goal) continue;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), Goal->GoalName.ToString());
        O->SetStringField(TEXT("bone"), Goal->BoneName.ToString());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("goals"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_goals"),
        FString::Printf(TEXT("%d goal(s)"), Arr.Num()), Data);
}

FBridgeResult UIKRigHandler::Action_GetInfo(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_info"), 1000, TEXT("'asset_path' is required"));
    UIKRigDefinition* Rig = LoadRig(AssetPath);
    if (!Rig) return MakeError(DOMAIN, TEXT("get_info"), 2000, TEXT("Rig not found"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    USkeletalMesh* Mesh = Rig->GetPreviewMesh();
    Data->SetStringField(TEXT("skeletal_mesh"), Mesh ? Mesh->GetPathName() : FString());
    Data->SetNumberField(TEXT("chain_count"), Rig->GetRetargetChains().Num());
    Data->SetNumberField(TEXT("goal_count"),  Rig->GetGoalArray().Num());
    return MakeSuccess(DOMAIN, TEXT("get_info"),
        FString::Printf(TEXT("chains=%d goals=%d"),
            Rig->GetRetargetChains().Num(), Rig->GetGoalArray().Num()), Data);
}
