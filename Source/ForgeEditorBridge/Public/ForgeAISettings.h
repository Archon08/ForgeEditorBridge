#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ForgeAISettings.generated.h"

/**
 * UForgeAISettings — Project Settings > Plugins > Forge Editor Bridge
 *
 * Centralised configuration for the ForgeEditorBridge plugin.
 * Accessible at runtime via GetDefault<UForgeAISettings>() or
 * from the editor under Project Settings > Plugins > Forge Editor Bridge.
 */
UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Forge Editor Bridge"))
class FORGEEDITORBRIDGE_API UForgeAISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UForgeAISettings();

	// ---- Settings Category (Project Settings section path) ----
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override  { return FName(TEXT("Forge Editor Bridge")); }

	// ---- HTTP Server ----

	/** Port the bridge HTTP server listens on. Default: 8765. */
	UPROPERTY(config, EditAnywhere, Category = "Server",
		meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 HttpPort = 8765;

	/** If non-empty, this fixed token is used instead of a random per-session GUID.
	 *  Leave blank (default) for automatic random tokens. */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	FString AuthToken;

	// ---- Context Output ----

	/** Root directory where context capture files and bridge results are written.
	 *  Relative paths are resolved against the project root.
	 *  Default: "Forge/ue-context" (i.e. {ProjectDir}/Forge/ue-context/). */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	FString ContextDirectory = TEXT("Forge/ue-context");

	// ---- Behaviour ----

	/** Automatically start the bridge when the editor launches.
	 *  If false, call UForgeAISubsystem::StartBridge() manually. */
	UPROPERTY(config, EditAnywhere, Category = "Behaviour")
	bool bAutoStart = true;

	// ---- Helpers ----

	/** Returns the absolute context directory path, resolving relative paths against ProjectDir. */
	FString GetAbsoluteContextDirectory() const;
};
