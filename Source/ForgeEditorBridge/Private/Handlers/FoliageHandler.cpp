#include "Handlers/FoliageHandler.h"
#include "ForgeAISubsystem.h"

// ---- Foliage types ---------------------------------------------------------
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

// ---- World / editor --------------------------------------------------------
#include "Editor.h"                 // GEditor

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UFoliageHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("foliage"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("add_type"))           return Action_AddType(Params);
	if (Action == TEXT("paint_foliage"))      return Action_PaintFoliage(Params);
	if (Action == TEXT("clear_type"))         return Action_ClearType(Params);
	if (Action == TEXT("list_types"))         return Action_ListTypes(Params);
	if (Action == TEXT("get_instance_count")) return Action_GetInstanceCount(Params);
	if (Action == TEXT("remove_type"))        return Action_RemoveType(Params);

	FBridgeResult R = CreateResult(TEXT("foliage"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown foliage action '%s'. Valid: add_type, paint_foliage, clear_type, list_types, get_instance_count"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// add_type
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::Action_AddType(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("add_type"));

	FString FoliageTypePath;
	if (!Params->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath) || FoliageTypePath.IsEmpty())
	{
		Result.Message = TEXT("add_type: 'foliage_type_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("add_type: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
	if (!FoliageType)
	{
		const FString Suffix = FoliageTypePath + TEXT(".") + FPackageName::GetLongPackageAssetName(FoliageTypePath);
		FoliageType = LoadObject<UFoliageType>(nullptr, *Suffix);
	}
	if (!FoliageType)
	{
		Result.Message = FString::Printf(
			TEXT("add_type: no UFoliageType found at '%s'"), *FoliageTypePath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify foliage_type_path points to a valid UFoliageType asset");
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/true);
	if (!IFA)
	{
		Result.Message = TEXT("add_type: failed to get or create AInstancedFoliageActor for the current level");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Idempotent — only add if not already registered
	FFoliageInfo* ExistingInfo = IFA->FindInfo(FoliageType);
	if (!ExistingInfo)
	{
		IFA->AddMesh(FoliageType);
	}

	Result.bSuccess = true;
	Result.Message  = ExistingInfo
		? FString::Printf(TEXT("add_type: foliage type '%s' already registered (no-op)"), *FoliageTypePath)
		: FString::Printf(TEXT("Foliage type '%s' registered with level IFA"), *FoliageTypePath);
	return Result;
}

// ---------------------------------------------------------------------------
// paint_foliage
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::Action_PaintFoliage(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("paint_foliage"));

	FString FoliageTypePath;
	if (!Params->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath) || FoliageTypePath.IsEmpty())
	{
		Result.Message = TEXT("paint_foliage: 'foliage_type_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	double X = 0.0, Y = 0.0, Z = 0.0, Pitch = 0.0, Yaw = 0.0, Roll = 0.0, Scale = 1.0;
	Params->TryGetNumberField(TEXT("x"),     X);
	Params->TryGetNumberField(TEXT("y"),     Y);
	Params->TryGetNumberField(TEXT("z"),     Z);
	Params->TryGetNumberField(TEXT("pitch"), Pitch);
	Params->TryGetNumberField(TEXT("yaw"),   Yaw);
	Params->TryGetNumberField(TEXT("roll"),  Roll);
	Params->TryGetNumberField(TEXT("scale"), Scale);

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("paint_foliage: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
	if (!FoliageType)
	{
		const FString Suffix = FoliageTypePath + TEXT(".") + FPackageName::GetLongPackageAssetName(FoliageTypePath);
		FoliageType = LoadObject<UFoliageType>(nullptr, *Suffix);
	}
	if (!FoliageType)
	{
		Result.Message = FString::Printf(
			TEXT("paint_foliage: no UFoliageType found at '%s'"), *FoliageTypePath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify foliage_type_path points to a valid UFoliageType asset");
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/true);
	if (!IFA)
	{
		Result.Message = TEXT("paint_foliage: failed to get or create AInstancedFoliageActor");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Auto-register the type if it isn't present yet
	FFoliageInfo* MeshInfo = IFA->FindInfo(FoliageType);
	if (!MeshInfo)
	{
		IFA->AddMesh(FoliageType);
		MeshInfo = IFA->FindInfo(FoliageType);
	}
	if (!MeshInfo)
	{
		Result.Message = FString::Printf(
			TEXT("paint_foliage: failed to get FFoliageInfo for type '%s'"), *FoliageTypePath);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Build a foliage instance and add it
	FFoliageInstance NewInstance;
	NewInstance.Location = FVector(X, Y, Z);
	NewInstance.Rotation = FRotator((float)Pitch, (float)Yaw, (float)Roll);
	NewInstance.DrawScale3D = FVector3f((float)Scale, (float)Scale, (float)Scale);

	MeshInfo->AddInstance(FoliageType, NewInstance);

	IFA->MarkPackageDirty();

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("Painted foliage instance at (%.0f, %.0f, %.0f) scale=%.2f for type '%s'"),
		(float)X, (float)Y, (float)Z, (float)Scale, *FoliageTypePath);
	return Result;
}

// ---------------------------------------------------------------------------
// clear_type
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::Action_ClearType(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("clear_type"));

	FString FoliageTypePath;
	if (!Params->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath) || FoliageTypePath.IsEmpty())
	{
		Result.Message = TEXT("clear_type: 'foliage_type_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("clear_type: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
	if (!FoliageType)
	{
		const FString Suffix = FoliageTypePath + TEXT(".") + FPackageName::GetLongPackageAssetName(FoliageTypePath);
		FoliageType = LoadObject<UFoliageType>(nullptr, *Suffix);
	}
	if (!FoliageType)
	{
		Result.Message = FString::Printf(
			TEXT("clear_type: no UFoliageType found at '%s'"), *FoliageTypePath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify foliage_type_path points to a valid UFoliageType asset");
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/false);
	if (!IFA)
	{
		// No IFA in level — nothing to clear
		Result.bSuccess = true;
		Result.Message  = TEXT("clear_type: no AInstancedFoliageActor in level — nothing to clear");
		return Result;
	}

	FFoliageInfo* MeshInfo = IFA->FindInfo(FoliageType);
	if (!MeshInfo || MeshInfo->Instances.Num() == 0)
	{
		Result.bSuccess = true;
		Result.Message  = FString::Printf(
			TEXT("clear_type: no instances found for '%s' (no-op)"), *FoliageTypePath);
		return Result;
	}

	const int32 NumInstances = MeshInfo->Instances.Num();

	TArray<int32> Indices;
	Indices.Reserve(NumInstances);
	for (int32 i = 0; i < NumInstances; ++i) Indices.Add(i);
	MeshInfo->RemoveInstances(TArrayView<const int32>(Indices), /*bRebuildTree=*/true);

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("Cleared %d instances of foliage type '%s'"), NumInstances, *FoliageTypePath);
	return Result;
}

// ---------------------------------------------------------------------------
// list_types
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::Action_ListTypes(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("list_types"));

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("list_types: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/false);
	if (!IFA)
	{
		Result.bSuccess = true;
		Result.Message  = TEXT("list_types: no AInstancedFoliageActor in current level");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("types"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetNumberField(TEXT("count"), 0);
		FString OutStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
		FJsonSerializer::Serialize(Data.ToSharedRef(), W);
		Result.ExtraData = OutStr;
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> TypesArr;
	// GetAllMeshes() renamed to GetFoliageInfos() in UE 5.7
	const TMap<UFoliageType*, TUniqueObj<FFoliageInfo>>& AllMeshes = IFA->GetFoliageInfos();
	for (auto& Pair : AllMeshes)
	{
		UFoliageType* FT = Pair.Key;
		const FFoliageInfo& Info = *Pair.Value;
		if (!FT) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), FT->GetName());
		Entry->SetStringField(TEXT("path"), FT->GetPathName());
		Entry->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
		TypesArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("types"), TypesArr);
	Data->SetNumberField(TEXT("count"), TypesArr.Num());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess  = true;
	Result.ExtraData = OutStr;
	Result.Message   = FString::Printf(TEXT("list_types: %d foliage type(s) in level"), TypesArr.Num());
	return Result;
}

// ---------------------------------------------------------------------------
// get_instance_count
// ---------------------------------------------------------------------------

FBridgeResult UFoliageHandler::Action_GetInstanceCount(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("get_instance_count"));

	FString FoliageTypePath;
	if (!Params->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath) || FoliageTypePath.IsEmpty())
	{
		Result.Message = TEXT("get_instance_count: 'foliage_type_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("get_instance_count: no editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
	if (!FoliageType)
	{
		const FString Suffix = FoliageTypePath + TEXT(".") + FPackageName::GetLongPackageAssetName(FoliageTypePath);
		FoliageType = LoadObject<UFoliageType>(nullptr, *Suffix);
	}
	if (!FoliageType)
	{
		Result.Message = FString::Printf(
			TEXT("get_instance_count: no UFoliageType found at '%s'"), *FoliageTypePath);
		Result.ErrorCode = 2000;
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/false);

	const int32 Count = (IFA && IFA->FindInfo(FoliageType))
		? IFA->FindInfo(FoliageType)->Instances.Num()
		: 0;

	Result.bSuccess = true;
	Result.Message  = FString::Printf(
		TEXT("get_instance_count: '%s' has %d instance(s)"), *FoliageTypePath, Count);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("foliage_type_path"), FoliageTypePath);
	Data->SetNumberField(TEXT("instance_count"), Count);
	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);
	Result.ExtraData = OutStr;
	return Result;
}

// ---------------------------------------------------------------------------
// remove_type — full deregistration (instances cleared AND type removed from IFA)
// ---------------------------------------------------------------------------
FBridgeResult UFoliageHandler::Action_RemoveType(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("foliage"), TEXT("remove_type"));

	FString FoliageTypePath;
	if (!Params->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath) || FoliageTypePath.IsEmpty())
	{
		Result.Message = TEXT("remove_type: 'foliage_type_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World) { Result.Message = TEXT("No editor world"); Result.ErrorCode = 3000; return Result; }

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
	if (!FoliageType)
	{
		const FString Suffix = FoliageTypePath + TEXT(".") + FPackageName::GetLongPackageAssetName(FoliageTypePath);
		FoliageType = LoadObject<UFoliageType>(nullptr, *Suffix);
	}
	if (!FoliageType)
	{
		Result.Message = FString::Printf(TEXT("UFoliageType not found: %s"), *FoliageTypePath);
		Result.ErrorCode = 2000;
		return Result;
	}

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
		World, /*bCreateIfNone=*/false);
	if (!IFA)
	{
		Result.bSuccess = true;
		Result.Message = TEXT("No IFA in level — nothing to remove");
		return Result;
	}
	if (!IFA->FindInfo(FoliageType))
	{
		Result.bSuccess = true;
		Result.Message = FString::Printf(TEXT("Type '%s' not registered (no-op)"), *FoliageTypePath);
		return Result;
	}

	UFoliageType* TypeArray[] = { FoliageType };
	IFA->RemoveFoliageType(TypeArray, 1);
	IFA->MarkPackageDirty();

	Result.bSuccess = true;
	Result.Message = FString::Printf(TEXT("Removed foliage type '%s' (instances + registration)"),
		*FoliageTypePath);
	return Result;
}
