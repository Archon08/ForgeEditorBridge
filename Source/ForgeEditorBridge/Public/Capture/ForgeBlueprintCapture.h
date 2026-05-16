#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeBlueprintCapture.generated.h"

class UBlueprint;

UCLASS()
class FORGEEDITORBRIDGE_API UForgeBlueprintCapture : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(const FString& InOutputDir);

    // Export a single Blueprint's structure + audit to blueprints/{Name}.json
    // AssetPath e.g. "/Game/Blueprints/BP_MyActor"
    UFUNCTION(BlueprintCallable, Category = "Forge")
    bool ExportBlueprint(const FString& AssetPath);

    // Export all UBlueprint assets found under /Game/
    // Returns the number of blueprints successfully exported
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportAllBlueprints();

    // Export blueprints whose package path starts with Prefix (e.g. "/Game/YourProject/")
    // Returns the number of blueprints successfully exported
    UFUNCTION(BlueprintCallable, Category = "Forge")
    int32 ExportBlueprintsByPrefix(const FString& Prefix);

private:
    FString OutputDir;

    bool SerializeBlueprintToJSON(UBlueprint* BP, const FString& AssetPath,
                                  TSharedRef<FJsonObject> OutRoot);
    void SerializeVariables(UBlueprint* BP, TSharedRef<FJsonObject> OutRoot);
    void SerializeComponents(UBlueprint* BP, TSharedRef<FJsonObject> OutRoot);
    void SerializeGraphs(UBlueprint* BP, TSharedRef<FJsonObject> OutRoot);

    // Runs all 8 audit rules; returns array of issue JSON objects
    TArray<TSharedPtr<FJsonValue>> RunAudit(UBlueprint* BP);

    void UpdateIndexFile(int32 BlueprintCount);
};
