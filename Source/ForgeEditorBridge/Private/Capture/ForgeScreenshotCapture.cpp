#include "Capture/ForgeScreenshotCapture.h"
#include "IO/ForgeContextWriter.h"

#include "UnrealClient.h"           // FScreenshotRequest, FOnScreenshotCaptured
#include "ImageUtils.h"             // FImageUtils::CompressImage (includes ImageCore.h -> FImageView)
#include "Misc/FileHelper.h"        // FFileHelper::SaveArrayToFile
#include "HAL/PlatformFileManager.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates::PostLoadMapWithWorld
#include "Editor.h"                 // GEditor, RedrawLevelEditingViewports (transitive)

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgeScreenshotCapture::Initialize(const FString& InOutputDir)
{
    UE_LOG(LogTemp, Log, TEXT("ForgeScreenshot: Initialize"));
    OutputDir = InOutputDir;

    // FScreenshotRequest::OnScreenshotCaptured() is a standard (non-dynamic) multicast delegate.
    // Signature: void(int32 Width, int32 Height, const TArray<FColor>& Colors)
    // Verified: UnrealClient.h - DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnScreenshotCaptured, ...)
    ScreenshotCapturedHandle = FScreenshotRequest::OnScreenshotCaptured().AddUObject(
        this, &UForgeScreenshotCapture::OnScreenshotCaptured);

    // Re-request a capture whenever a new map finishes loading.
    PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
        this, &UForgeScreenshotCapture::OnPostLoadMapWithWorld);

    // Initial capture attempt. May not fire until the viewport actually renders a frame,
    // but the request will be pending and fulfilled on the next redraw.
    RequestCapture();
}

void UForgeScreenshotCapture::Deinitialize()
{
    FScreenshotRequest::OnScreenshotCaptured().Remove(ScreenshotCapturedHandle);
    ScreenshotCapturedHandle.Reset();

    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
    PostLoadMapHandle.Reset();
}

// ============================================================
// Trigger
// ============================================================

void UForgeScreenshotCapture::OnPostLoadMapWithWorld(UWorld* World)
{
    UE_LOG(LogTemp, Log, TEXT("ForgeScreenshot: OnPostLoadMapWithWorld fired, world=%s"),
        World ? *World->GetName() : TEXT("null"));
    RequestCapture();
}

void UForgeScreenshotCapture::RequestCapture()
{
    UE_LOG(LogTemp, Log, TEXT("ForgeScreenshot: RequestCapture"));

    // Queue the screenshot - will be serviced on the next rendered frame.
    // bInShowUI = false: capture viewport only, no Slate UI overlay.
    // Verified: FScreenshotRequest::RequestScreenshot(bool) in UnrealClient.h
    FScreenshotRequest::RequestScreenshot(false);

    // Force all level editing viewports to redraw so the request is serviced immediately
    // rather than waiting for user interaction.
    // Verified: UEditorEngine::RedrawLevelEditingViewports in EditorEngine.h (virtual, overridden
    // in UUnrealEdEngine which GEditor points to at runtime).
    if (GEditor)
    {
        GEditor->RedrawLevelEditingViewports(true);
    }
}

// ============================================================
// Screenshot received
// ============================================================

void UForgeScreenshotCapture::OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Colors)
{
    UE_LOG(LogTemp, Log, TEXT("ForgeScreenshot: OnScreenshotCaptured fired %dx%d (%d pixels)"),
        Width, Height, Colors.Num());

    if (Width <= 0 || Height <= 0 || Colors.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeScreenshot: Empty screenshot data, skipping"));
        return;
    }

    // Ensure screenshot subdirectory exists.
    const FString ScreenshotDir = OutputDir / TEXT("screenshot");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*ScreenshotDir);

    // Encode raw FColor array to PNG bytes.
    // FImageView(const FColor*, int32 W, int32 H, EGammaSpace) - verified in ImageCore.h
    // FImageUtils::CompressImage(TArray64<uint8>&, const TCHAR* ext, const FImageView&) - verified in ImageUtils.h
    TArray64<uint8> PNGBytes;
    const FImageView View(Colors.GetData(), Width, Height, EGammaSpace::sRGB);
    if (!FImageUtils::CompressImage(PNGBytes, TEXT(".png"), View))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeScreenshot: PNG compression failed"));
        return;
    }

    // Write to screenshot/latest.png.
    // FFileHelper::SaveArrayToFile(const TArray64<uint8>&, ...) - verified in FileHelper.h
    const FString OutputPath = ScreenshotDir / TEXT("latest.png");
    if (!FFileHelper::SaveArrayToFile(PNGBytes, *OutputPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeScreenshot: Failed to write %s"), *OutputPath);
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("ForgeScreenshot: Written %s (%lld bytes)"),
        *OutputPath, static_cast<int64>(PNGBytes.Num()));

    UpdateIndexFile(FForgeContextWriter::NowISO8601());
}

// ============================================================
// Index - read-merge-write so we don't clobber PCG data
// ============================================================

void UForgeScreenshotCapture::UpdateIndexFile(const FString& Timestamp)
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    // Load and parse existing index.json if it exists.
    TSharedPtr<FJsonObject> Root;
    FString ExistingContent;
    if (FFileHelper::LoadFileToString(ExistingContent, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingContent);
        FJsonSerializer::Deserialize(Reader, Root);
    }

    if (!Root.IsValid())
    {
        Root = MakeShared<FJsonObject>();
    }

    // Get or create the captures_available section.
    TSharedPtr<FJsonObject> Captures;
    const TSharedPtr<FJsonObject>* ExistingCaptures;
    if (Root->TryGetObjectField(TEXT("captures_available"), ExistingCaptures))
    {
        Captures = *ExistingCaptures;
    }
    else
    {
        Captures = MakeShared<FJsonObject>();
    }

    // Set only the screenshot_latest section - leave other captures untouched.
    TSharedPtr<FJsonObject> ScreenshotSection = MakeShared<FJsonObject>();
    ScreenshotSection->SetStringField(TEXT("file"),         TEXT("screenshot/latest.png"));
    ScreenshotSection->SetStringField(TEXT("last_updated"), Timestamp);
    Captures->SetObjectField(TEXT("screenshot_latest"), ScreenshotSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), TEXT("0.2.6"));

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
