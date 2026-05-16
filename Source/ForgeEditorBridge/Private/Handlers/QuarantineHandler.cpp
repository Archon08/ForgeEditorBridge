#include "Handlers/QuarantineHandler.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "IO/ForgeContextWriter.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

void UQuarantineHandler::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
}

FBridgeResult UQuarantineHandler::QuarantineAsset(const FString& AssetPath, const FString& Reason)
{
	FBridgeResult Result;
	Result.Domain = TEXT("quarantine");
	Result.Action = TEXT("quarantine_asset");
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath));

	if (!AssetData.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = 2000;
		Result.Message = FString::Printf(TEXT("Asset not found at path: %s"), *AssetPath);
		return Result;
	}

	FString QuarantineFolder = TEXT("/Game/Forge_Quarantine");
	FString NewName = FString::Printf(TEXT("%s_%s"), *FDateTime::UtcNow().ToIso8601().Replace(TEXT(":"), TEXT("-")), *AssetData.AssetName.ToString());
	FString NewPath = QuarantineFolder / NewName;

	TArray<FAssetRenameData> AssetsToRename;
	AssetsToRename.Add(FAssetRenameData(AssetData.GetAsset(), QuarantineFolder, NewName));

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	if (AssetToolsModule.Get().RenameAssets(AssetsToRename))
	{
		Result.bSuccess = true;
		Result.Message = FString::Printf(TEXT("Asset quarantined to %s"), *NewPath);
		Result.AffectedPath = NewPath;
		AppendToLog(AssetPath, NewPath, Reason);
	}
	else
	{
		Result.bSuccess = false;
		Result.ErrorCode = 3000;
		Result.Message = FString::Printf(TEXT("Failed to rename asset for quarantine: %s"), *AssetPath);
	}

	return Result;
}

void UQuarantineHandler::AppendToLog(const FString& AssetPath, const FString& DestPath, const FString& Reason)
{
	TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
	LogEntry->SetStringField(TEXT("original_path"), AssetPath);
	LogEntry->SetStringField(TEXT("quarantine_path"), DestPath);
	LogEntry->SetStringField(TEXT("reason"), Reason);
	LogEntry->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());

	// Write latest quarantine event log
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("bridge/quarantine/latest_event.json"), LogEntry.ToSharedRef());
}
