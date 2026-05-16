#include "Capture/ForgePackagingCapture.h"
#include "IO/ForgeContextWriter.h"

// ---- File system ------------------------------------------------------------
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

// ---- Target platform --------------------------------------------------------
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"

// ---- JSON + IO --------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgePackagingCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("packaging")));
	UE_LOG(LogTemp, Log, TEXT("ForgePackaging: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportPackagingState
// ---------------------------------------------------------------------------

bool UForgePackagingCapture::ExportPackagingState()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"), FForgeContextWriter::NowISO8601());

	// -------------------------------------------------------------------------
	// Cook log — Saved/Logs/Cook.log
	// -------------------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> CookObj = MakeShared<FJsonObject>();
		const FString CookLogPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("Cook.log"));

		if (!IFileManager::Get().FileExists(*CookLogPath))
		{
			CookObj->SetBoolField  (TEXT("found"), false);
			CookObj->SetStringField(TEXT("path"),  CookLogPath);
			CookObj->SetStringField(TEXT("note"),  TEXT("Cook.log not found. Cook the project first via Platforms menu or UAT."));
		}
		else
		{
			CookObj->SetBoolField  (TEXT("found"), true);
			CookObj->SetStringField(TEXT("path"),  CookLogPath);

			// Last-modified timestamp
			const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*CookLogPath);
			CookObj->SetStringField(TEXT("last_modified"), ModTime.ToString());

			FString Content;
			if (FFileHelper::LoadFileToString(Content, *CookLogPath))
			{
				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines);

				int32 ErrorCount   = 0;
				int32 WarnCount    = 0;
				bool  bCompleted   = false;

				for (const FString& Line : Lines)
				{
					if (Line.Contains(TEXT("Error:")))   ++ErrorCount;
					if (Line.Contains(TEXT("Warning:"))) ++WarnCount;
					if (Line.Contains(TEXT("Cook completed")) || Line.Contains(TEXT("BUILD SUCCESSFUL")))
						bCompleted = true;
				}

				CookObj->SetNumberField(TEXT("total_lines"),   Lines.Num());
				CookObj->SetNumberField(TEXT("error_count"),   ErrorCount);
				CookObj->SetNumberField(TEXT("warning_count"), WarnCount);
				CookObj->SetBoolField  (TEXT("completed"),     bCompleted);

				// Last 30 lines as context
				const int32 TailStart = FMath::Max(0, Lines.Num() - 30);
				TArray<TSharedPtr<FJsonValue>> TailArr;
				for (int32 i = TailStart; i < Lines.Num(); ++i)
					TailArr.Add(MakeShared<FJsonValueString>(Lines[i]));
				CookObj->SetArrayField(TEXT("tail_30"), TailArr);
			}
			else
			{
				CookObj->SetStringField(TEXT("error"), TEXT("Failed to read Cook.log"));
			}
		}

		Root->SetObjectField(TEXT("cook_log"), CookObj);
	}

	// -------------------------------------------------------------------------
	// Content size — scan Content directory, breakdown by extension
	// -------------------------------------------------------------------------
	{
		const FString ContentDir = FPaths::ProjectContentDir();

		TArray<FString> AllFiles;
		IFileManager::Get().FindFilesRecursive(AllFiles, *ContentDir, TEXT("*"), true, false);

		int64 TotalBytes = 0;
		TMap<FString, int64>  BytesByExt;
		TMap<FString, int32>  CountByExt;

		for (const FString& File : AllFiles)
		{
			const int64 Size = IFileManager::Get().FileSize(*File);
			if (Size < 0) continue;

			TotalBytes += Size;
			const FString Ext = FPaths::GetExtension(File).ToLower();
			BytesByExt.FindOrAdd(Ext)  += Size;
			CountByExt.FindOrAdd(Ext)  += 1;
		}

		// Sort extensions by total bytes descending
		TArray<FString> ExtKeys;
		BytesByExt.GetKeys(ExtKeys);
		ExtKeys.Sort([&BytesByExt](const FString& A, const FString& B) {
			return BytesByExt[A] > BytesByExt[B];
		});

		TArray<TSharedPtr<FJsonValue>> BreakdownArr;
		for (const FString& Ext : ExtKeys)
		{
			TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
			EObj->SetStringField(TEXT("ext"),        Ext.IsEmpty() ? TEXT("(no ext)") : Ext);
			EObj->SetNumberField(TEXT("file_count"),  CountByExt[Ext]);
			EObj->SetNumberField(TEXT("bytes"),       (double)BytesByExt[Ext]);
			EObj->SetNumberField(TEXT("mb"),          (double)BytesByExt[Ext] / (1024.0 * 1024.0));
			BreakdownArr.Add(MakeShared<FJsonValueObject>(EObj));
		}

		TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
		SizeObj->SetNumberField(TEXT("total_files"),  AllFiles.Num());
		SizeObj->SetNumberField(TEXT("total_bytes"),  (double)TotalBytes);
		SizeObj->SetNumberField(TEXT("total_mb"),     (double)TotalBytes / (1024.0 * 1024.0));
		SizeObj->SetStringField(TEXT("content_dir"),  ContentDir);
		SizeObj->SetArrayField (TEXT("by_extension"), BreakdownArr);

		Root->SetObjectField(TEXT("content_size"), SizeObj);
	}

	// -------------------------------------------------------------------------
	// Target platforms
	// -------------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> PlatformArr;

		ITargetPlatformManagerModule* TPM =
			FModuleManager::GetModulePtr<ITargetPlatformManagerModule>(TEXT("TargetPlatform"));

		if (TPM)
		{
			const TArray<ITargetPlatform*>& Platforms = TPM->GetTargetPlatforms();
			for (ITargetPlatform* P : Platforms)
			{
				if (!P) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"),         P->PlatformName());
				PObj->SetStringField(TEXT("display_name"), P->DisplayName().ToString());
				PObj->SetBoolField  (TEXT("is_running"),   P->IsRunningPlatform());
				PlatformArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ForgePackaging: TargetPlatform module not available"));
		}

		Root->SetArrayField (TEXT("platforms"),       PlatformArr);
		Root->SetNumberField(TEXT("platform_count"),  PlatformArr.Num());
	}

	bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("packaging"), TEXT("last_cook"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log, TEXT("ForgePackaging: Exported -> packaging/last_cook.json"));
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgePackagingCapture::UpdateIndexFile()
{
	const FString IndexPath = OutputDir / TEXT("index.json");

	TSharedPtr<FJsonObject> Root;
	FString Raw;
	if (FFileHelper::LoadFileToString(Raw, *IndexPath))
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		FJsonSerializer::Deserialize(Reader, Root);
	}
	if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Captures;
	if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
	{
		if (Found->IsValid() && (*Found)->Type == EJson::Object)
			Captures = (*Found)->AsObject();
	}
	if (!Captures.IsValid())
	{
		Captures = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("captures_available"), Captures);
	}

	TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
	Section->SetStringField(TEXT("file"),         TEXT("packaging/last_cook.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("packaging"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
