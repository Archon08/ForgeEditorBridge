#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Results/BridgeResult.h"
#include "QuarantineHandler.generated.h"

UCLASS()
class FORGEEDITORBRIDGE_API UQuarantineHandler : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InOutputDir);
	FBridgeResult QuarantineAsset(const FString& AssetPath, const FString& Reason);

private:
	FString OutputDir;
	void AppendToLog(const FString& AssetPath, const FString& DestPath, const FString& Reason);
};
