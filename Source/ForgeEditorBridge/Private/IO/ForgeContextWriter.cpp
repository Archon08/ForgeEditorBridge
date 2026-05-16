#include "IO/ForgeContextWriter.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

bool FForgeContextWriter::EnsureDirectory(const FString& Directory)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*Directory))
    {
        return PlatformFile.CreateDirectoryTree(*Directory);
    }
    return true;
}

bool FForgeContextWriter::WriteJSON(const FString& Directory, const FString& Filename,
                                      const TSharedRef<FJsonObject>& JsonObject)
{
    if (!EnsureDirectory(Directory)) return false;

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    if (!FJsonSerializer::Serialize(JsonObject, Writer)) return false;

    const FString FilePath = Directory / Filename;
    return FFileHelper::SaveStringToFile(JsonString, *FilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FForgeContextWriter::AppendLine(const FString& Directory, const FString& Filename,
                                       const FString& Line)
{
    if (!EnsureDirectory(Directory)) return false;

    const FString FilePath = Directory / Filename;
    const FString Content = Line + LINE_TERMINATOR;
    return FFileHelper::SaveStringToFile(Content, *FilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
        &IFileManager::Get(), FILEWRITE_Append);
}

bool FForgeContextWriter::WriteCSV(const FString& Directory, const FString& Filename,
                                     const TArray<FString>& Rows)
{
    if (!EnsureDirectory(Directory)) return false;

    const FString Content = FString::Join(Rows, LINE_TERMINATOR);
    const FString FilePath = Directory / Filename;
    return FFileHelper::SaveStringToFile(Content, *FilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FForgeContextWriter::WriteBytes(const FString& Directory, const FString& Filename,
                                       const TArray<uint8>& Bytes)
{
    if (!EnsureDirectory(Directory)) return false;

    const FString FilePath = Directory / Filename;
    return FFileHelper::SaveArrayToFile(Bytes, *FilePath);
}

FString FForgeContextWriter::NowISO8601()
{
    const FDateTime Now = FDateTime::UtcNow();
    return FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02dZ"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(),
        Now.GetHour(), Now.GetMinute(), Now.GetSecond());
}
