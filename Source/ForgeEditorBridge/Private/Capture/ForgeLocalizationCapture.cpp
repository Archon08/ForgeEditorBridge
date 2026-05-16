#include "Capture/ForgeLocalizationCapture.h"
#include "IO/ForgeContextWriter.h"

// ---- Internationalization ---------------------------------------------------
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

// ---- File system ------------------------------------------------------------
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

// ---- JSON + IO --------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeLocalizationCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("localization")));
	UE_LOG(LogTemp, Log, TEXT("ForgeLocalization: Initialized"));
}

// ---------------------------------------------------------------------------
// ParsePOFile — count msgid/msgstr pairs
// ---------------------------------------------------------------------------

void UForgeLocalizationCapture::ParsePOFile(const FString& FilePath, int32& OutTotal, int32& OutTranslated)
{
	OutTotal      = 0;
	OutTranslated = 0;

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath)) return;

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);

	// State machine: track last msgid, then check msgstr
	bool bLastWasMsgid = false;
	FString LastMsgid;

	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("msgid ")))
		{
			// Extract the quoted value
			const int32 QuoteStart = Trimmed.Find(TEXT("\""));
			const int32 QuoteEnd   = Trimmed.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (QuoteStart != INDEX_NONE && QuoteEnd > QuoteStart)
				LastMsgid = Trimmed.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			else
				LastMsgid.Empty();

			// Skip the PO header entry (msgid "")
			bLastWasMsgid = !LastMsgid.IsEmpty();
			if (bLastWasMsgid) ++OutTotal;
		}
		else if (Trimmed.StartsWith(TEXT("msgstr ")) && bLastWasMsgid)
		{
			// Check if the translation is non-empty
			const int32 QuoteStart = Trimmed.Find(TEXT("\""));
			const int32 QuoteEnd   = Trimmed.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (QuoteStart != INDEX_NONE && QuoteEnd > QuoteStart)
			{
				const FString MsgStr = Trimmed.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
				if (!MsgStr.IsEmpty()) ++OutTranslated;
			}
			bLastWasMsgid = false;
		}
		else if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("#")))
		{
			// Continuation lines or other content — reset state
			bLastWasMsgid = false;
		}
	}
}

// ---------------------------------------------------------------------------
// ExportLocalizationCoverage
// ---------------------------------------------------------------------------

bool UForgeLocalizationCapture::ExportLocalizationCoverage()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"), FForgeContextWriter::NowISO8601());

	// -------------------------------------------------------------------------
	// Culture info from FInternationalization
	// -------------------------------------------------------------------------
	{
		FInternationalization& I18N = FInternationalization::Get();
		const TSharedPtr<FCulture> CurrentCulture = I18N.GetCurrentCulture();
		const TSharedPtr<FCulture> DefaultCulture  = I18N.GetDefaultCulture();

		Root->SetStringField(TEXT("current_culture"),
			CurrentCulture.IsValid() ? CurrentCulture->GetName() : TEXT("(unknown)"));
		Root->SetStringField(TEXT("source_culture"),
			DefaultCulture.IsValid() ? DefaultCulture->GetName() : TEXT("en"));
	}

	// -------------------------------------------------------------------------
	// Scan Content/Localization/ directory
	// -------------------------------------------------------------------------
	const FString LocalizationDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Localization"));
	Root->SetStringField(TEXT("localization_dir"), LocalizationDir);

	TArray<TSharedPtr<FJsonValue>> TargetArr;

	if (!IFileManager::Get().DirectoryExists(*LocalizationDir))
	{
		Root->SetStringField(TEXT("status"),
			TEXT("Content/Localization/ directory not found. No localization targets configured."));
		Root->SetArrayField(TEXT("targets"), TargetArr);

		bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("localization"), TEXT("coverage"), Root);
		if (bOK) UpdateIndexFile();
		return bOK;
	}

	// Each immediate subdirectory is a localization target
	TArray<FString> TargetDirs;
	IFileManager::Get().FindFiles(TargetDirs, *(LocalizationDir / TEXT("*")), false, true);

	for (const FString& TargetDirName : TargetDirs)
	{
		const FString TargetPath = LocalizationDir / TargetDirName;

		TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
		TObj->SetStringField(TEXT("name"), TargetDirName);
		TObj->SetStringField(TEXT("path"), TargetPath);

		// Each subdirectory within the target is a culture code
		TArray<FString> CultureDirs;
		IFileManager::Get().FindFiles(CultureDirs, *(TargetPath / TEXT("*")), false, true);

		TArray<TSharedPtr<FJsonValue>> CultureArr;
		for (const FString& CultureCode : CultureDirs)
		{
			const FString CulturePath = TargetPath / CultureCode;

			TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
			CObj->SetStringField(TEXT("culture"), CultureCode);
			CObj->SetStringField(TEXT("path"),    CulturePath);

			// Check for PO file (preferred for coverage stats)
			const FString POPath = CulturePath / (TargetDirName + TEXT(".po"));
			if (IFileManager::Get().FileExists(*POPath))
			{
				CObj->SetStringField(TEXT("po_file"), POPath);

				int32 TotalKeys    = 0;
				int32 Translated   = 0;
				ParsePOFile(POPath, TotalKeys, Translated);

				CObj->SetNumberField(TEXT("key_count"),        TotalKeys);
				CObj->SetNumberField(TEXT("translated_count"), Translated);
				CObj->SetNumberField(TEXT("missing_count"),    TotalKeys - Translated);

				const double CoveragePct = (TotalKeys > 0)
					? (double(Translated) / double(TotalKeys)) * 100.0 : 100.0;
				CObj->SetNumberField(TEXT("coverage_pct"), FMath::RoundToFloat(CoveragePct * 10.0) / 10.0);
				CObj->SetStringField(TEXT("status"),
					(TotalKeys == 0)         ? TEXT("empty")
					: (Translated == TotalKeys) ? TEXT("complete")
					: (Translated == 0)         ? TEXT("untranslated")
					:                             TEXT("partial"));
			}
			else
			{
				// No PO file — check for other localization artifacts
				TArray<FString> LocFiles;
				IFileManager::Get().FindFiles(LocFiles, *(CulturePath / TEXT("*")), true, false);

				TArray<TSharedPtr<FJsonValue>> FileArr;
				for (const FString& F : LocFiles)
					FileArr.Add(MakeShared<FJsonValueString>(F));

				CObj->SetArrayField(TEXT("files"),  FileArr);
				CObj->SetStringField(TEXT("status"), TEXT("no_po_file"));
				CObj->SetStringField(TEXT("note"),
					TEXT("Run LocalizationHandler::export_po to generate PO files for coverage analysis."));
			}

			CultureArr.Add(MakeShared<FJsonValueObject>(CObj));
		}

		TObj->SetArrayField(TEXT("cultures"),     CultureArr);
		TObj->SetNumberField(TEXT("culture_count"), CultureArr.Num());
		TargetArr.Add(MakeShared<FJsonValueObject>(TObj));
	}

	Root->SetArrayField(TEXT("targets"),      TargetArr);
	Root->SetNumberField(TEXT("target_count"), TargetArr.Num());

	bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("localization"), TEXT("coverage"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log, TEXT("ForgeLocalization: Exported -> localization/coverage.json (%d target(s))"),
			TargetArr.Num());
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeLocalizationCapture::UpdateIndexFile()
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
	Section->SetStringField(TEXT("file"),         TEXT("localization/coverage.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("localization"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
