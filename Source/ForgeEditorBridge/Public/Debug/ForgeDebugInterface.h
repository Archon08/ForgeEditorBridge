#pragma once

#include "UObject/Interface.h"
#include "ForgeDebugInterface.generated.h"

// ---------------------------------------------------------------------------
// IForgeDebugInterface
// Optional interface for Blueprint actors that want structured variable export
// in runtime/variables.json. Actors opt-in by implementing this interface.
// Only actors also tagged "ForgeDebug" are included in snapshots.
//
// Usage in Blueprint:
//   1. Add interface "ForgeDebugInterface" to your Actor BP
//   2. Implement "GetDebugData" - return a TMap<FString,FString> of name->value pairs
//   3. Tag the actor with "ForgeDebug" (Actor Tags in the Details panel)
//   4. Call UForgeRuntimeCapture::CaptureVariableSnapshot() during PIE
// ---------------------------------------------------------------------------

UINTERFACE(MinimalAPI, Blueprintable)
class UForgeDebugInterface : public UInterface
{
    GENERATED_BODY()
};

class FORGEEDITORBRIDGE_API IForgeDebugInterface
{
    GENERATED_BODY()
public:
    // Implement in Blueprint to return key-value debug data for this actor.
    // Example keys: "Health", "CurrentBiome", "ActiveAbility", "GoldCount"
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Forge")
    void GetDebugData(TMap<FString, FString>& OutData);
};
