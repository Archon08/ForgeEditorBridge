#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Handlers/BridgeHandlerBase.h"
#include "ForgeAISubsystem.generated.h"

// ---- Forward declarations: Infrastructure -----------------------------------
class FBridgeHttpServer;
class UBridgeResultWriter;
class UQuarantineHandler;
class UBridgeAttentionManager;

// ---- Forward declarations: Captures (Read side) -----------------------------
class UForgeBuildCapture;
class UForgePCGCapture;
class UForgeScreenshotCapture;
class UForgeRuntimeCapture;
class UForgeHeightmapCapture;
class UForgeDataTableCapture;
class UForgeAssetRegistryCapture;
class UForgeBlueprintCapture;
class UForgeBlueprintBuilder;
class UForgeGASCapture;
class UForgeMaterialCapture;
class UForgeNiagaraCapture;
class UForgeUMGCapture;
class UForgeWeatherCapture;
class UForgeWorldGenCapture;
class UForgeSymbolCapture;
class UForgePerformanceCapture;
class UForgeAnimationCapture;
class UForgeInputCapture;
class UForgeNetworkCapture;
class UForgeCommandChannel;
class UForgeCollisionCapture;
class UForgeLocalizationCapture;
class UForgePackagingCapture;

/**
 * UForgeAISubsystem — Unified AI Bridge Subsystem (v2.0.0)
 *
 * Consolidates the old UBridgeSubsystem (write/command) and
 * UForgeContextSubsystem (read/capture) into a single subsystem.
 *
 * v2.0: Domain handlers now use map-based registration via GetDomainName().
 *       Adding a new handler requires zero changes to this class.
 *
 * Lifecycle:
 *   Initialize() → creates all handler & capture objects
 *   StartBridge() → initializes handlers, starts captures & HTTP server
 *   StopBridge()  → stops HTTP server
 *   Deinitialize() → tears down captures & server
 */
UCLASS()
class FORGEEDITORBRIDGE_API UForgeAISubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Forge Editor Bridge")
	void StartBridge();

	UFUNCTION(BlueprintCallable, Category = "Forge Editor Bridge")
	void StopBridge();

	UFUNCTION(BlueprintCallable, Category = "Forge Editor Bridge")
	bool IsBridgeRunning() const { return bServerRunning; }

	/** Resolved output directory (from settings). */
	FString OutputDirectory;

	// ========================== Handler Registration ==========================

	/** Register a handler under its GetDomainName(). Called during Initialize(). */
	void RegisterHandler(UBridgeHandlerBase* Handler);

	/** Look up a handler by domain string. Returns nullptr if not found. */
	UBridgeHandlerBase* GetHandler(const FString& Domain) const;

	/** Return all registered domain names (sorted). */
	TArray<FString> GetRegisteredDomains() const;

	/** Return the full handler map (for capabilities/batch). */
	const TMap<FString, TObjectPtr<UBridgeHandlerBase>>& GetHandlerMap() const { return HandlerMap; }

private:
	bool bServerRunning = false;

	/** Domain string → Handler instance. Auto-populated during Initialize(). */
	UPROPERTY()
	TMap<FString, TObjectPtr<UBridgeHandlerBase>> HandlerMap;

	/** Helper: create, register, and initialize a handler by class. */
	template<typename T>
	void RegisterHandlerClass()
	{
		T* Handler = NewObject<T>(this);
		RegisterHandler(Handler);
	}

	// ========================== Infrastructure (Write Side) ====================
public:
	TSharedPtr<FBridgeHttpServer> HttpServer;

	UPROPERTY()
	TObjectPtr<UBridgeResultWriter> ResultWriter;

	UPROPERTY()
	TObjectPtr<UQuarantineHandler> Quarantine;

	UPROPERTY()
	TObjectPtr<UBridgeAttentionManager> AttentionManager;

	// ========================== Read Side (Captures) ==========================

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeBuildCapture> BuildCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgePCGCapture> PCGCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeScreenshotCapture> ScreenshotCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeRuntimeCapture> RuntimeCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeHeightmapCapture> HeightmapCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeDataTableCapture> DataTableCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeAssetRegistryCapture> AssetRegistryCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeBlueprintCapture> BlueprintCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeBlueprintBuilder> BlueprintBuilder;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeGASCapture> GASCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeMaterialCapture> MaterialCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeNiagaraCapture> NiagaraCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeUMGCapture> UMGCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeWeatherCapture> WeatherCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeWorldGenCapture> WorldGenCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeSymbolCapture> SymbolCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgePerformanceCapture> PerformanceCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeCommandChannel> CommandChannel;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeAnimationCapture> AnimationCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeInputCapture> InputCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeNetworkCapture> NetworkCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeCollisionCapture> CollisionCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgeLocalizationCapture> LocalizationCapture;

	UPROPERTY(BlueprintReadOnly, Category = "Forge Editor Bridge")
	TObjectPtr<UForgePackagingCapture> PackagingCapture;
};
