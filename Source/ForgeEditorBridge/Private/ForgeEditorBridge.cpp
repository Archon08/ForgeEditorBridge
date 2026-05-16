#include "ForgeEditorBridge.h"

#define LOCTEXT_NAMESPACE "FForgeEditorBridgeModule"

void FForgeEditorBridgeModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge: module started (v0.2.6)"));
}

void FForgeEditorBridgeModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FForgeEditorBridgeModule, ForgeEditorBridge)
