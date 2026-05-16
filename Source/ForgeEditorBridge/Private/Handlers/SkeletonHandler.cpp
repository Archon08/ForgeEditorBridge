#include "Handlers/SkeletonHandler.h"
#include "ForgeAISubsystem.h"

// ---- Skeleton / socket types -----------------------------------------------
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Misc/PackageName.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void USkeletonHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("skeleton"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("add_socket"))         return Action_AddSocket(Params);
	if (Action == TEXT("move_socket"))        return Action_MoveSocket(Params);
	if (Action == TEXT("add_virtual_bone"))   return Action_AddVirtualBone(Params);
	if (Action == TEXT("get_sockets"))        return Action_GetSockets(Params);
	if (Action == TEXT("list_virtual_bones")) return Action_ListVirtualBones(Params);
	if (Action == TEXT("get_skeleton_info"))  return Action_GetSkeletonInfo(Params);
	if (Action == TEXT("remove_socket"))      return Action_RemoveSocket(Params);
	if (Action == TEXT("remove_virtual_bone"))return Action_RemoveVirtualBone(Params);

	FBridgeResult R = CreateResult(TEXT("skeleton"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown skeleton action '%s'. Valid: add_socket, move_socket, add_virtual_bone, get_sockets, list_virtual_bones, get_skeleton_info, remove_socket, remove_virtual_bone"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// add_socket
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_AddSocket(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("add_socket"));

	FString AssetPath, SocketName, BoneName;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("bone_name"),   BoneName)   || BoneName.IsEmpty())
	{
		Result.Message = TEXT("add_socket: 'asset_path', 'socket_name', and 'bone_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	// Idempotent — skip if socket already exists
	if (Skeleton->FindSocket(FName(*SocketName)))
	{
		Result.bSuccess = true;
		Result.Message  = FString::Printf(
			TEXT("add_socket: socket '%s' already exists on '%s' (no-op)"), *SocketName, *AssetPath);
		return Result;
	}

	double X = 0.0, Y = 0.0, Z = 0.0;
	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	double Scale = 1.0;
	Params->TryGetNumberField(TEXT("x"),     X);
	Params->TryGetNumberField(TEXT("y"),     Y);
	Params->TryGetNumberField(TEXT("z"),     Z);
	Params->TryGetNumberField(TEXT("pitch"), Pitch);
	Params->TryGetNumberField(TEXT("yaw"),   Yaw);
	Params->TryGetNumberField(TEXT("roll"),  Roll);
	Params->TryGetNumberField(TEXT("scale"), Scale);

	USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(Skeleton);
	NewSocket->SocketName        = FName(*SocketName);
	NewSocket->BoneName          = FName(*BoneName);
	NewSocket->RelativeLocation  = FVector((float)X, (float)Y, (float)Z);
	NewSocket->RelativeRotation  = FRotator((float)Pitch, (float)Yaw, (float)Roll);
	NewSocket->RelativeScale     = FVector((float)Scale);

	Skeleton->Sockets.Add(NewSocket);
	Skeleton->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Socket '%s' added to bone '%s' on '%s'"), *SocketName, *BoneName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// move_socket
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_MoveSocket(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("move_socket"));

	FString AssetPath, SocketName;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		Result.Message = TEXT("move_socket: 'asset_path' and 'socket_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	USkeletalMeshSocket* Socket = Skeleton->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		Result.Message = FString::Printf(
			TEXT("move_socket: socket '%s' not found on '%s'"), *SocketName, *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Use add_socket to create the socket first.");
		return Result;
	}

	// Only update components that are fully specified (all 3 fields in a group)
	double X, Y, Z;
	if (Params->TryGetNumberField(TEXT("x"), X) &&
	    Params->TryGetNumberField(TEXT("y"), Y) &&
	    Params->TryGetNumberField(TEXT("z"), Z))
	{
		Socket->RelativeLocation = FVector((float)X, (float)Y, (float)Z);
	}

	double Pitch, Yaw, Roll;
	if (Params->TryGetNumberField(TEXT("pitch"), Pitch) &&
	    Params->TryGetNumberField(TEXT("yaw"),   Yaw)   &&
	    Params->TryGetNumberField(TEXT("roll"),  Roll))
	{
		Socket->RelativeRotation = FRotator((float)Pitch, (float)Yaw, (float)Roll);
	}

	double Scale;
	if (Params->TryGetNumberField(TEXT("scale"), Scale))
	{
		Socket->RelativeScale = FVector((float)Scale);
	}

	Skeleton->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Socket '%s' transform updated on '%s'"), *SocketName, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_virtual_bone
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_AddVirtualBone(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("add_virtual_bone"));

	FString AssetPath, SourceBone, TargetBone;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("source_bone"), SourceBone) || SourceBone.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("target_bone"), TargetBone) || TargetBone.IsEmpty())
	{
		Result.Message = TEXT("add_virtual_bone: 'asset_path', 'source_bone', and 'target_bone' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	FName VirtualBoneName;
	const bool bAdded = Skeleton->AddNewVirtualBone(FName(*SourceBone), FName(*TargetBone), VirtualBoneName);
	if (!bAdded)
	{
		Result.Message = FString::Printf(
			TEXT("add_virtual_bone: AddVirtualBone returned false for '%s'→'%s' on '%s'. "
			     "Both bones must exist in the skeleton and the virtual bone must not already exist."),
			*SourceBone, *TargetBone, *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	Skeleton->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Virtual bone '%s' added (source='%s', target='%s') on '%s'"),
		*VirtualBoneName.ToString(), *SourceBone, *TargetBone, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_sockets
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_GetSockets(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("get_sockets"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_sockets: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	TArray<TSharedPtr<FJsonValue>> SocketsArr;
	for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (!Socket) continue;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		Entry->SetStringField(TEXT("bone"), Socket->BoneName.ToString());

		TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), Socket->RelativeLocation.X);
		Loc->SetNumberField(TEXT("y"), Socket->RelativeLocation.Y);
		Loc->SetNumberField(TEXT("z"), Socket->RelativeLocation.Z);
		Entry->SetObjectField(TEXT("location"), Loc);

		TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"),   Socket->RelativeRotation.Yaw);
		Rot->SetNumberField(TEXT("roll"),  Socket->RelativeRotation.Roll);
		Entry->SetObjectField(TEXT("rotation"), Rot);

		Entry->SetNumberField(TEXT("scale"), Socket->RelativeScale.X);
		SocketsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("sockets"), SocketsArr);
	Data->SetNumberField(TEXT("count"), SocketsArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("get_sockets: %d socket(s) on '%s'"), SocketsArr.Num(), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// list_virtual_bones
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_ListVirtualBones(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("list_virtual_bones"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("list_virtual_bones: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	TArray<TSharedPtr<FJsonValue>> BonesArr;
	for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),        VB.VirtualBoneName.ToString());
		Entry->SetStringField(TEXT("source_bone"),  VB.SourceBoneName.ToString());
		Entry->SetStringField(TEXT("target_bone"),  VB.TargetBoneName.ToString());
		BonesArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("virtual_bones"), BonesArr);
	Data->SetNumberField(TEXT("count"), BonesArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("list_virtual_bones: %d virtual bone(s) on '%s'"), BonesArr.Num(), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_skeleton_info
// ---------------------------------------------------------------------------

FBridgeResult USkeletonHandler::Action_GetSkeletonInfo(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("get_skeleton_info"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_skeleton_info: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	const int32 BoneCount = RefSkeleton.GetNum();

	// Collect root bones (bones with no parent, i.e. parent index == INDEX_NONE)
	TArray<TSharedPtr<FJsonValue>> RootBonesArr;
	for (int32 i = 0; i < BoneCount; ++i)
	{
		if (RefSkeleton.GetParentIndex(i) == INDEX_NONE)
		{
			RootBonesArr.Add(MakeShared<FJsonValueString>(RefSkeleton.GetBoneName(i).ToString()));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),        AssetPath);
	Data->SetNumberField(TEXT("bone_count"),         BoneCount);
	Data->SetNumberField(TEXT("socket_count"),       Skeleton->Sockets.Num());
	Data->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
	Data->SetArrayField(TEXT("root_bones"),          RootBonesArr);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(
		TEXT("get_skeleton_info: '%s' — %d bones, %d sockets, %d virtual bones"),
		*AssetPath, BoneCount, Skeleton->Sockets.Num(), Skeleton->GetVirtualBones().Num());
	return Result;
}

// ---------------------------------------------------------------------------
// LoadSkeleton helper
// ---------------------------------------------------------------------------

USkeleton* USkeletonHandler::LoadSkeleton(const FString& AssetPath, FBridgeResult& Result)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *AssetPath);
	if (!Skeleton)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Skeleton = LoadObject<USkeleton>(nullptr, *Suffix);
	}
	if (!Skeleton)
	{
		Result.Message = FString::Printf(
			TEXT("LoadSkeleton: no USkeleton found at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the skeleton asset path exists in the Content Browser.");
	}
	return Skeleton;
}

// ---------------------------------------------------------------------------
// remove_socket
// ---------------------------------------------------------------------------
FBridgeResult USkeletonHandler::Action_RemoveSocket(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("remove_socket"));
	FString AssetPath, SocketName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("skeleton"), TEXT("remove_socket"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
		return MakeError(TEXT("skeleton"), TEXT("remove_socket"), 1000, TEXT("'socket_name' is required"));

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	const FName SockName(*SocketName);
	int32 RemovedAt = INDEX_NONE;
	for (int32 i = 0; i < Skeleton->Sockets.Num(); ++i)
	{
		USkeletalMeshSocket* S = Skeleton->Sockets[i];
		if (S && S->SocketName == SockName)
		{
			Skeleton->Sockets.RemoveAt(i);
			RemovedAt = i;
			break;
		}
	}
	if (RemovedAt == INDEX_NONE)
		return MakeError(TEXT("skeleton"), TEXT("remove_socket"), 2000,
			FString::Printf(TEXT("Socket '%s' not found"), *SocketName),
			TEXT("Use get_sockets to list available sockets"));

	Skeleton->MarkPackageDirty();
	return MakeSuccess(TEXT("skeleton"), TEXT("remove_socket"),
		FString::Printf(TEXT("Removed socket '%s' from skeleton"), *SocketName));
}

// ---------------------------------------------------------------------------
// remove_virtual_bone
// ---------------------------------------------------------------------------
FBridgeResult USkeletonHandler::Action_RemoveVirtualBone(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("skeleton"), TEXT("remove_virtual_bone"));
	FString AssetPath, BoneName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("skeleton"), TEXT("remove_virtual_bone"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
		return MakeError(TEXT("skeleton"), TEXT("remove_virtual_bone"), 1000, TEXT("'bone_name' is required"));

	USkeleton* Skeleton = LoadSkeleton(AssetPath, Result);
	if (!Skeleton) return Result;

	TArray<FName> Names; Names.Add(FName(*BoneName));
	Skeleton->RemoveVirtualBones(Names);
	Skeleton->MarkPackageDirty();
	return MakeSuccess(TEXT("skeleton"), TEXT("remove_virtual_bone"),
		FString::Printf(TEXT("Requested removal of virtual bone '%s'"), *BoneName));
}
