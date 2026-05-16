#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FORGEEDITORBRIDGE_API FForgeContextWriter
{
public:
    // Write/overwrite a JSON object to Directory/Filename.json
    static bool WriteJSON(const FString& Directory, const FString& Filename,
                          const TSharedRef<FJsonObject>& JsonObject);

    // Append a single line to Directory/Filename (creates file if not exists)
    static bool AppendLine(const FString& Directory, const FString& Filename,
                           const FString& Line);

    // Write/overwrite a CSV file from an array of row strings
    static bool WriteCSV(const FString& Directory, const FString& Filename,
                         const TArray<FString>& Rows);

    // Write raw bytes - used for PNG output
    static bool WriteBytes(const FString& Directory, const FString& Filename,
                           const TArray<uint8>& Bytes);

    // ISO 8601 UTC timestamp: "2026-03-07T20:50:00Z"
    static FString NowISO8601();

private:
    FForgeContextWriter() = delete;
    static bool EnsureDirectory(const FString& Directory);
};
