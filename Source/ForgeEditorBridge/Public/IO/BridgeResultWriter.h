#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Results/BridgeResult.h"
#include "BridgeResultWriter.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API UBridgeResultWriter : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 RING_SIZE = 10;

	void Initialize(const FString& InOutputDir);
	void WriteResult(const FBridgeResult& Result);

private:
	FString OutputDir;
	int32 Counter = 0;
};
