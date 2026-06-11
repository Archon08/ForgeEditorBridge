#include "ForgeAISubsystem.h"
#include "ForgeBridgeVersion.h"
#include "ForgeAISettings.h"

// ---- Infrastructure includes ----
#include "Server/BridgeHttpServer.h"
#include "IO/BridgeResultWriter.h"
#include "Handlers/QuarantineHandler.h"
#include "Attention/BridgeAttentionManager.h"
#include "Handlers/BridgeHandlerBase.h"
#include "UObject/UObjectIterator.h"

// ---- Read-side includes ----
#include "Capture/ForgeBuildCapture.h"
#include "Capture/ForgePCGCapture.h"
#include "Capture/ForgeScreenshotCapture.h"
#include "Capture/ForgeRuntimeCapture.h"
#include "Capture/ForgeHeightmapCapture.h"
#include "Capture/ForgeDataTableCapture.h"
#include "Capture/ForgeAssetRegistryCapture.h"
#include "Capture/ForgeBlueprintCapture.h"
#include "Blueprint/ForgeBlueprintBuilder.h"
#include "Capture/ForgeGASCapture.h"
#include "Capture/ForgeMaterialCapture.h"
#include "Capture/ForgeNiagaraCapture.h"
#include "Capture/ForgeUMGCapture.h"
#include "Capture/ForgeWeatherCapture.h"
#include "Capture/ForgeWorldGenCapture.h"
#include "Capture/ForgeSymbolCapture.h"
#include "Capture/ForgePerformanceCapture.h"
#include "Capture/ForgeCommandChannel.h"
#include "Capture/ForgeAnimationCapture.h"
#include "Capture/ForgeInputCapture.h"
#include "Capture/ForgeNetworkCapture.h"
#include "Capture/ForgeCollisionCapture.h"
#include "Capture/ForgeLocalizationCapture.h"
#include "Capture/ForgePackagingCapture.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Handler Registration
// ---------------------------------------------------------------------------

void UForgeAISubsystem::RegisterHandler(UBridgeHandlerBase* Handler)
{
	if (!Handler) return;

	const FString Domain = Handler->GetDomainName();
	if (Domain.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("ForgeEditorBridge: Handler %s returned empty domain name, skipping."),
			*Handler->GetClass()->GetName());
		return;
	}

	if (HandlerMap.Contains(Domain))
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeEditorBridge: Duplicate domain '%s' — replacing %s with %s."),
			*Domain, *HandlerMap[Domain]->GetClass()->GetName(), *Handler->GetClass()->GetName());
	}

	HandlerMap.Add(Domain, Handler);
}

UBridgeHandlerBase* UForgeAISubsystem::GetHandler(const FString& Domain) const
{
	const TObjectPtr<UBridgeHandlerBase>* Found = HandlerMap.Find(Domain);
	return Found ? Found->Get() : nullptr;
}

TArray<FString> UForgeAISubsystem::GetRegisteredDomains() const
{
	TArray<FString> Domains;
	HandlerMap.GetKeys(Domains);
	Domains.Sort();
	return Domains;
}

// ---------------------------------------------------------------------------
// Initialize — create all objects (write + read sides)
// ---------------------------------------------------------------------------

void UForgeAISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Resolve output directory from settings
	const UForgeAISettings* Settings = GetDefault<UForgeAISettings>();
	OutputDirectory = Settings->GetAbsoluteContextDirectory();

	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*OutputDirectory);

	// ---- Infrastructure ----
	ResultWriter     = NewObject<UBridgeResultWriter>(this);
	Quarantine       = NewObject<UQuarantineHandler>(this);
	AttentionManager = NewObject<UBridgeAttentionManager>(this);
	HttpServer       = MakeShared<FBridgeHttpServer>(this);

	// ---- Auto-discover and register all UBridgeHandlerBase subclasses ----
	{
		TArray<UClass*> HandlerClasses;
		GetDerivedClasses(UBridgeHandlerBase::StaticClass(), HandlerClasses, true);

		for (UClass* HandlerClass : HandlerClasses)
		{
			if (!HandlerClass || HandlerClass->HasAnyClassFlags(CLASS_Abstract)) continue;

			UBridgeHandlerBase* Handler = NewObject<UBridgeHandlerBase>(this, HandlerClass);
			if (Handler)
			{
				RegisterHandler(Handler);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge: Auto-registered %d domain handlers."), HandlerMap.Num());

	// ---- Read side: create capture objects ----
	BuildCapture         = NewObject<UForgeBuildCapture>(this);
	PCGCapture           = NewObject<UForgePCGCapture>(this);
	ScreenshotCapture    = NewObject<UForgeScreenshotCapture>(this);
	RuntimeCapture       = NewObject<UForgeRuntimeCapture>(this);
	HeightmapCapture     = NewObject<UForgeHeightmapCapture>(this);
	DataTableCapture     = NewObject<UForgeDataTableCapture>(this);
	AssetRegistryCapture = NewObject<UForgeAssetRegistryCapture>(this);
	BlueprintCapture     = NewObject<UForgeBlueprintCapture>(this);
	BlueprintBuilder     = NewObject<UForgeBlueprintBuilder>(this);
	GASCapture           = NewObject<UForgeGASCapture>(this);
	MaterialCapture      = NewObject<UForgeMaterialCapture>(this);
	NiagaraCapture       = NewObject<UForgeNiagaraCapture>(this);
	UMGCapture           = NewObject<UForgeUMGCapture>(this);
	WeatherCapture       = NewObject<UForgeWeatherCapture>(this);
	WorldGenCapture      = NewObject<UForgeWorldGenCapture>(this);
	SymbolCapture        = NewObject<UForgeSymbolCapture>(this);
	PerformanceCapture   = NewObject<UForgePerformanceCapture>(this);
	CommandChannel       = NewObject<UForgeCommandChannel>(this);
	AnimationCapture     = NewObject<UForgeAnimationCapture>(this);
	InputCapture         = NewObject<UForgeInputCapture>(this);
	NetworkCapture       = NewObject<UForgeNetworkCapture>(this);
	CollisionCapture     = NewObject<UForgeCollisionCapture>(this);
	LocalizationCapture  = NewObject<UForgeLocalizationCapture>(this);
	PackagingCapture     = NewObject<UForgePackagingCapture>(this);

	// Auto-start if enabled in settings
	if (Settings->bAutoStart)
	{
		StartBridge();
	}
}

// ---------------------------------------------------------------------------
// Deinitialize — tear down everything
// ---------------------------------------------------------------------------

void UForgeAISubsystem::Deinitialize()
{
	// Stop server
	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
		HttpServer.Reset();
	}

	// Clear handler map (GC handles UObject lifetime)
	HandlerMap.Empty();

	// Tear down captures (reverse order, those with Deinitialize())
	if (PackagingCapture)     { PackagingCapture = nullptr; }
	if (LocalizationCapture)  { LocalizationCapture = nullptr; }
	if (CollisionCapture)     { CollisionCapture = nullptr; }
	if (NetworkCapture)       { NetworkCapture = nullptr; }
	if (InputCapture)         { InputCapture = nullptr; }
	if (AnimationCapture)     { AnimationCapture = nullptr; }
	if (CommandChannel)       { CommandChannel->Deinitialize(); CommandChannel = nullptr; }
	if (PerformanceCapture)   { PerformanceCapture->Deinitialize(); PerformanceCapture = nullptr; }
	if (SymbolCapture)        { SymbolCapture = nullptr; }
	if (WorldGenCapture)      { WorldGenCapture = nullptr; }
	if (WeatherCapture)       { WeatherCapture = nullptr; }
	if (UMGCapture)           { UMGCapture = nullptr; }
	if (NiagaraCapture)       { NiagaraCapture = nullptr; }
	if (MaterialCapture)      { MaterialCapture = nullptr; }
	if (GASCapture)           { GASCapture = nullptr; }
	if (BlueprintBuilder)     { BlueprintBuilder = nullptr; }
	if (BlueprintCapture)     { BlueprintCapture = nullptr; }
	if (AssetRegistryCapture) { AssetRegistryCapture->Deinitialize(); AssetRegistryCapture = nullptr; }
	if (DataTableCapture)     { DataTableCapture->Deinitialize(); DataTableCapture = nullptr; }
	if (HeightmapCapture)     { HeightmapCapture->Deinitialize(); HeightmapCapture = nullptr; }
	if (RuntimeCapture)       { RuntimeCapture->Deinitialize(); RuntimeCapture = nullptr; }
	if (ScreenshotCapture)    { ScreenshotCapture->Deinitialize(); ScreenshotCapture = nullptr; }
	if (PCGCapture)           { PCGCapture->Deinitialize(); PCGCapture = nullptr; }
	if (BuildCapture)         { BuildCapture->Deinitialize(); BuildCapture = nullptr; }

	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// StartBridge — initialize all handlers & captures, start HTTP server
// ---------------------------------------------------------------------------

void UForgeAISubsystem::StartBridge()
{
	if (bServerRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeEditorBridge: StartBridge called but already running."));
		return;
	}

	if (!HttpServer.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("ForgeEditorBridge: HttpServer is null."));
		return;
	}

	// ---- Initialize infrastructure ----
	ResultWriter->Initialize(OutputDirectory);
	Quarantine->Initialize(OutputDirectory);
	AttentionManager->Initialize(this);

	// ---- Initialize all domain handlers from map ----
	for (auto& Pair : HandlerMap)
	{
		Pair.Value->Initialize(this);
	}

	// ---- Initialize read-side captures ----
	BuildCapture->Initialize(OutputDirectory);
	PCGCapture->Initialize(OutputDirectory);
	ScreenshotCapture->Initialize(OutputDirectory);
	RuntimeCapture->Initialize(OutputDirectory);
	HeightmapCapture->Setup(OutputDirectory);
	DataTableCapture->Initialize(OutputDirectory);
	AssetRegistryCapture->Initialize(OutputDirectory);
	AssetRegistryCapture->ExportAssetRegistry();
	BlueprintCapture->Initialize(OutputDirectory);
	GASCapture->Initialize(OutputDirectory);
	MaterialCapture->Initialize(OutputDirectory);
	NiagaraCapture->Initialize(OutputDirectory);
	UMGCapture->Initialize(OutputDirectory);
	WeatherCapture->Initialize(OutputDirectory);
	WorldGenCapture->Initialize(OutputDirectory);
	SymbolCapture->Initialize(OutputDirectory);
	PerformanceCapture->Initialize(this);
	CommandChannel->Initialize(OutputDirectory, this);
	AnimationCapture->Initialize(OutputDirectory);
	InputCapture->Initialize(OutputDirectory);
	NetworkCapture->Initialize(OutputDirectory);
	CollisionCapture->Initialize(OutputDirectory);
	LocalizationCapture->Initialize(OutputDirectory);
	PackagingCapture->Initialize(OutputDirectory);

	// ---- Start HTTP server ----
	const UForgeAISettings* Settings = GetDefault<UForgeAISettings>();
	HttpServer->Start(OutputDirectory, Settings->HttpPort, Settings->AuthToken);

	bServerRunning = true;
	UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge: Bridge started on port %d (v%s) — %d domains."),
		Settings->HttpPort, FORGE_BRIDGE_VERSION, HandlerMap.Num());
}

// ---------------------------------------------------------------------------
// StopBridge
// ---------------------------------------------------------------------------

void UForgeAISubsystem::StopBridge()
{
	if (!bServerRunning) return;

	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
	}

	bServerRunning = false;
	UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge: Bridge server stopped."));
}
