#include "IO/BridgeResultWriter.h"
#include "IO/ForgeContextWriter.h"
#include "Dom/JsonObject.h"

void UBridgeResultWriter::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
}

void UBridgeResultWriter::WriteResult(const FBridgeResult& Result)
{
	int32 Slot = Counter % RING_SIZE;
	Counter++;

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("ok"), Result.bSuccess);
	Json->SetStringField(TEXT("message"), Result.Message);
	Json->SetStringField(TEXT("domain"), Result.Domain);
	Json->SetStringField(TEXT("action"), Result.Action);
	Json->SetStringField(TEXT("affected_path"), Result.AffectedPath);
	Json->SetStringField(TEXT("timestamp"), Result.Timestamp.IsEmpty() ? FDateTime::UtcNow().ToIso8601() : Result.Timestamp);
	Json->SetNumberField(TEXT("sequence"), (double)(Counter - 1));

	FString SlotFile = FString::Printf(TEXT("bridge/results/result-%03d.json"), Slot);
	FForgeContextWriter::WriteJSON(OutputDir, SlotFile, Json.ToSharedRef());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("bridge/results/latest.json"), Json.ToSharedRef());
}
