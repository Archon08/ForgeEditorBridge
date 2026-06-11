#include "ForgeEditorBridge.h"
#include "ForgeBridgeVersion.h"

#define LOCTEXT_NAMESPACE "FForgeEditorBridgeModule"

void FForgeEditorBridgeModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge: module started (v%s)"), FORGE_BRIDGE_VERSION);
}

void FForgeEditorBridgeModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FForgeEditorBridgeModule, ForgeEditorBridge)
