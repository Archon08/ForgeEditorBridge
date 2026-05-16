#include "Handlers/SourceControlHandler.h"
#include "ForgeAISubsystem.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "SourceControlOperations.h"
#include "ISourceControlState.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TArray<FString> USourceControlHandler::AssetPathsToDiskPaths(const TArray<FString>& AssetPaths)
{
	TArray<FString> DiskPaths;
	DiskPaths.Reserve(AssetPaths.Num());

	for (const FString& AssetPath : AssetPaths)
	{
		FString DiskPath;
		if (FPackageName::TryConvertLongPackageNameToFilename(AssetPath, DiskPath, FPackageName::GetAssetPackageExtension()))
		{
			DiskPaths.Add(MoveTemp(DiskPath));
		}
		else
		{
			// Fall back to treating it as a literal disk path
			DiskPaths.Add(AssetPath);
		}
	}

	return DiskPaths;
}

static TArray<FString> ParseFilesArray(TSharedPtr<FJsonObject> Params)
{
	TArray<FString> Files;
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("files"), FilesArray) && FilesArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *FilesArray)
		{
			FString Str;
			if (Val.IsValid() && Val->TryGetString(Str) && !Str.IsEmpty())
			{
				Files.Add(MoveTemp(Str));
			}
		}
	}
	return Files;
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return MakeError(TEXT("source_control"), Action, 1000, TEXT("Params object is null"));
	}

	if (Action == TEXT("checkout"))      return Action_Checkout(Params);
	if (Action == TEXT("checkin"))        return Action_Checkin(Params);
	if (Action == TEXT("revert"))         return Action_Revert(Params);
	if (Action == TEXT("get_status"))     return Action_GetStatus(Params);
	if (Action == TEXT("mark_for_add"))   return Action_MarkForAdd(Params);

	return MakeError(TEXT("source_control"), Action, 1000,
		FString::Printf(TEXT("Unknown source_control action '%s'. Valid: checkout, checkin, revert, get_status, mark_for_add"), *Action));
}

// ---------------------------------------------------------------------------
// checkout
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::Action_Checkout(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("source_control");
	const FString Action = TEXT("checkout");

	TArray<FString> AssetPaths = ParseFilesArray(Params);
	if (AssetPaths.Num() == 0)
	{
		return MakeError(Domain, Action, 1000, TEXT("'files' array is required and must not be empty"),
			TEXT("Provide an array of asset paths, e.g. [\"/Game/Maps/MyLevel\"]"));
	}

	TArray<FString> DiskPaths = AssetPathsToDiskPaths(AssetPaths);

	ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
	TSharedRef<FCheckOut, ESPMode::ThreadSafe> CheckOutOp = ISourceControlOperation::Create<FCheckOut>();

	ECommandResult::Type Result = SCC.Execute(CheckOutOp, DiskPaths);

	if (Result == ECommandResult::Succeeded)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("files_checked_out"), DiskPaths.Num());
		return MakeSuccess(Domain, Action,
			FString::Printf(TEXT("Checked out %d file(s)"), DiskPaths.Num()), Data);
	}

	return MakeError(Domain, Action, 3000,
		FString::Printf(TEXT("Checkout failed for %d file(s)"), DiskPaths.Num()),
		TEXT("Verify source control is connected and files are not checked out by another user"));
}

// ---------------------------------------------------------------------------
// checkin
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::Action_Checkin(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("source_control");
	const FString Action = TEXT("checkin");

	TArray<FString> AssetPaths = ParseFilesArray(Params);
	if (AssetPaths.Num() == 0)
	{
		return MakeError(Domain, Action, 1000, TEXT("'files' array is required and must not be empty"));
	}

	FString Description;
	if (!Params->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
	{
		return MakeError(Domain, Action, 1000, TEXT("'description' is required for checkin"));
	}

	TArray<FString> DiskPaths = AssetPathsToDiskPaths(AssetPaths);

	ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOp = ISourceControlOperation::Create<FCheckIn>();
	CheckInOp->SetDescription(FText::FromString(Description));

	ECommandResult::Type Result = SCC.Execute(CheckInOp, DiskPaths);

	if (Result == ECommandResult::Succeeded)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("files_checked_in"), DiskPaths.Num());
		Data->SetStringField(TEXT("description"), Description);
		return MakeSuccess(Domain, Action,
			FString::Printf(TEXT("Checked in %d file(s)"), DiskPaths.Num()), Data);
	}

	return MakeError(Domain, Action, 3000,
		FString::Printf(TEXT("Checkin failed for %d file(s)"), DiskPaths.Num()),
		TEXT("Ensure all files are checked out and up to date before checkin"));
}

// ---------------------------------------------------------------------------
// revert
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::Action_Revert(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("source_control");
	const FString Action = TEXT("revert");

	TArray<FString> AssetPaths = ParseFilesArray(Params);
	if (AssetPaths.Num() == 0)
	{
		return MakeError(Domain, Action, 1000, TEXT("'files' array is required and must not be empty"));
	}

	TArray<FString> DiskPaths = AssetPathsToDiskPaths(AssetPaths);

	ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
	TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOp = ISourceControlOperation::Create<FRevert>();

	ECommandResult::Type Result = SCC.Execute(RevertOp, DiskPaths);

	if (Result == ECommandResult::Succeeded)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("files_reverted"), DiskPaths.Num());
		return MakeSuccess(Domain, Action,
			FString::Printf(TEXT("Reverted %d file(s)"), DiskPaths.Num()), Data);
	}

	return MakeError(Domain, Action, 3000,
		FString::Printf(TEXT("Revert failed for %d file(s)"), DiskPaths.Num()),
		TEXT("Verify files are checked out before reverting"));
}

// ---------------------------------------------------------------------------
// get_status
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::Action_GetStatus(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("source_control");
	const FString Action = TEXT("get_status");

	TArray<FString> AssetPaths = ParseFilesArray(Params);
	if (AssetPaths.Num() == 0)
	{
		return MakeError(Domain, Action, 1000, TEXT("'files' array is required and must not be empty"));
	}

	TArray<FString> DiskPaths = AssetPathsToDiskPaths(AssetPaths);

	ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();

	ECommandResult::Type Result = SCC.Execute(UpdateOp, DiskPaths);
	if (Result != ECommandResult::Succeeded)
	{
		return MakeError(Domain, Action, 3000, TEXT("Failed to update source control status"),
			TEXT("Ensure source control provider is connected"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> StatusArray;

	for (int32 i = 0; i < DiskPaths.Num(); ++i)
	{
		FSourceControlStatePtr State = SCC.GetState(DiskPaths[i], EStateCacheUsage::Use);

		TSharedPtr<FJsonObject> FileStatus = MakeShared<FJsonObject>();
		FileStatus->SetStringField(TEXT("asset_path"), AssetPaths[i]);
		FileStatus->SetStringField(TEXT("disk_path"), DiskPaths[i]);

		if (State.IsValid())
		{
			FileStatus->SetBoolField(TEXT("is_checked_out"), State->IsCheckedOut());
			FileStatus->SetBoolField(TEXT("is_checked_out_other"), State->IsCheckedOutOther());
			FileStatus->SetBoolField(TEXT("is_current"), State->IsCurrent());
			FileStatus->SetBoolField(TEXT("can_checkout"), State->CanCheckout());
			FileStatus->SetBoolField(TEXT("is_added"), State->IsAdded());
			FileStatus->SetBoolField(TEXT("is_deleted"), State->IsDeleted());
		}
		else
		{
			FileStatus->SetStringField(TEXT("status"), TEXT("unknown"));
		}

		StatusArray.Add(MakeShared<FJsonValueObject>(FileStatus));
	}

	Data->SetArrayField(TEXT("files"), StatusArray);
	return MakeSuccess(Domain, Action,
		FString::Printf(TEXT("Status retrieved for %d file(s)"), DiskPaths.Num()), Data);
}

// ---------------------------------------------------------------------------
// mark_for_add
// ---------------------------------------------------------------------------

FBridgeResult USourceControlHandler::Action_MarkForAdd(TSharedPtr<FJsonObject> Params)
{
	const FString Domain = TEXT("source_control");
	const FString Action = TEXT("mark_for_add");

	TArray<FString> AssetPaths = ParseFilesArray(Params);
	if (AssetPaths.Num() == 0)
	{
		return MakeError(Domain, Action, 1000, TEXT("'files' array is required and must not be empty"));
	}

	TArray<FString> DiskPaths = AssetPathsToDiskPaths(AssetPaths);

	ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
	TSharedRef<FMarkForAdd, ESPMode::ThreadSafe> MarkAddOp = ISourceControlOperation::Create<FMarkForAdd>();

	ECommandResult::Type Result = SCC.Execute(MarkAddOp, DiskPaths);

	if (Result == ECommandResult::Succeeded)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("files_marked"), DiskPaths.Num());
		return MakeSuccess(Domain, Action,
			FString::Printf(TEXT("Marked %d file(s) for add"), DiskPaths.Num()), Data);
	}

	return MakeError(Domain, Action, 3000,
		FString::Printf(TEXT("Mark for add failed for %d file(s)"), DiskPaths.Num()),
		TEXT("Verify files exist on disk and are not already tracked"));
}

TSharedPtr<FJsonObject> USourceControlHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Check out files from source control")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("files"), P(TEXT("array"), true, TEXT("Array of asset paths to check out"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("checkout"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Check in files to source control")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("files"), P(TEXT("array"), true, TEXT("Array of asset paths to check in"))); Ps->SetObjectField(TEXT("description"), P(TEXT("string"), true, TEXT("Checkin description/comment"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("checkin"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Revert files to depot version")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("files"), P(TEXT("array"), true, TEXT("Array of asset paths to revert"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("revert"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get source control status for files")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("files"), P(TEXT("array"), true, TEXT("Array of asset paths to query status"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_status"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Mark new files for add to source control")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("files"), P(TEXT("array"), true, TEXT("Array of asset paths to mark for add"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("mark_for_add"), A); }

	return Root;
}
