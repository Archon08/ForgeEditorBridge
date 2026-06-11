#include "Capture/ForgeHeightmapCapture.h"
#include "ForgeBridgeVersion.h"
#include "IO/ForgeContextWriter.h"

// Landscape headers - all in Runtime/Landscape module (no LandscapeEditor needed)
// VERIFIED against UE 5.7 engine source:
//   Landscape.h       -> Runtime/Landscape/Classes/Landscape.h
//   LandscapeInfo.h   -> Runtime/Landscape/Classes/LandscapeInfo.h
//   LandscapeEdit.h   -> Runtime/Landscape/Public/LandscapeEdit.h  (FLandscapeEditDataInterface lives here)
//   LandscapeDataAccess.h is a DIFFERENT header - do NOT use it for FLandscapeEditDataInterface
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"

#include "Engine/World.h"
#include "EngineUtils.h"            // TActorIterator
#include "Editor.h"                 // GEditor (transitive)

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgeHeightmapCapture::Setup(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    // No delegates - purely imperative / on-demand via Python MCP call
}

void UForgeHeightmapCapture::Deinitialize()
{
    // Nothing to unregister
}

// ============================================================
// Public API
// ============================================================

bool UForgeHeightmapCapture::ExportHeightmap(int32 Resolution)
{
    ALandscape* Landscape = FindFirstLandscape();
    if (!Landscape)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeHeightmap: No landscape found in current level"));
        return false;
    }
    return ExportHeightmapForActor(Landscape, Resolution);
}

bool UForgeHeightmapCapture::ExportHeightmapForActor(ALandscape* Landscape, int32 Resolution)
{
    if (!Landscape) return false;

    TArray<TArray<float>> Heights;
    FVector2D WorldMin, WorldMax;
    float MinHeightCm, MaxHeightCm;

    if (!SampleHeightGrid(Landscape, Resolution, Heights, WorldMin, WorldMax,
                          MinHeightCm, MaxHeightCm))
    {
        return false;
    }

    bool bSuccess = WriteHeightJSON(Landscape, Heights, WorldMin, WorldMax,
                                    MinHeightCm, MaxHeightCm, Resolution);
    bSuccess &= WriteHeightThumbnail(Heights, Resolution);

    if (bSuccess) { UpdateIndexFile(); }
    return bSuccess;
}

// ============================================================
// Height sampling
// ============================================================

bool UForgeHeightmapCapture::SampleHeightGrid(ALandscape* Landscape, int32 Resolution,
    TArray<TArray<float>>& OutHeights,
    FVector2D& OutWorldMin, FVector2D& OutWorldMax,
    float& OutMinHeightCm, float& OutMaxHeightCm)
{
    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (!LandscapeInfo)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeHeightmap: GetLandscapeInfo() returned null"));
        return false;
    }

    int32 MinX, MinY, MaxX, MaxY;
    if (!LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeHeightmap: GetLandscapeExtent() failed"));
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("ForgeHeightmap: Landscape extent [%d,%d] to [%d,%d]"),
        MinX, MinY, MaxX, MaxY);

    // Read entire native heightmap in one bulk call.
    // VERIFIED: FLandscapeEditDataInterface(ULandscapeInfo*, bool bUploadToGPU = true)
    // VERIFIED: GetHeightData(int32& X1, int32& Y1, int32& X2, int32& Y2, uint16* Data, int32 Stride)
    // Passing false for bUploadToGPU - we are reading only, no GPU upload needed.
    FLandscapeEditDataInterface DataInterface(LandscapeInfo, false);
    int32 NativeWidth  = MaxX - MinX + 1;
    int32 NativeHeight = MaxY - MinY + 1;
    TArray<uint16> RawData;
    RawData.SetNumZeroed(NativeWidth * NativeHeight);

    // GetHeightData modifies X1/Y1/X2/Y2 to clamp to valid range - use local copies
    int32 X1 = MinX, Y1 = MinY, X2 = MaxX, Y2 = MaxY;
    DataInterface.GetHeightData(X1, Y1, X2, Y2, RawData.GetData(), 0);

    // Downsample to target resolution using point sampling
    OutHeights.SetNum(Resolution);
    for (int32 Row = 0; Row < Resolution; Row++)
    {
        OutHeights[Row].SetNum(Resolution);
        for (int32 Col = 0; Col < Resolution; Col++)
        {
            int32 SrcX = FMath::RoundToInt((float)Col / (Resolution - 1) * (NativeWidth  - 1));
            int32 SrcY = FMath::RoundToInt((float)Row / (Resolution - 1) * (NativeHeight - 1));
            uint16 Raw = RawData[SrcY * NativeWidth + SrcX];
            // VERIFIED normalization: 0-65535 -> 0.0-1.0
            // Midpoint 32768 = world Z 0
            OutHeights[Row][Col] = Raw / 65535.0f;
        }
    }

    // World bounds from actor transform + component-space extent
    FVector Scale  = Landscape->GetActorScale3D();
    FVector Origin = Landscape->GetActorLocation();
    OutWorldMin = FVector2D(Origin.X + MinX * Scale.X, Origin.Y + MinY * Scale.Y);
    OutWorldMax = FVector2D(Origin.X + MaxX * Scale.X, Origin.Y + MaxY * Scale.Y);

    // Height range in cm: VERIFIED formula
    // (RawValue - 32768) * 0.001953125 * Scale.Z  (0.001953125 = 1/512)
    OutMinHeightCm = (0.0f     - 32768.0f) * 0.001953125f * Scale.Z;
    OutMaxHeightCm = (65535.0f - 32768.0f) * 0.001953125f * Scale.Z;

    UE_LOG(LogTemp, Log, TEXT("ForgeHeightmap: Sampled %dx%d from %dx%d native, height range %.0f-%.0f cm"),
        Resolution, Resolution, NativeWidth, NativeHeight, OutMinHeightCm, OutMaxHeightCm);

    return true;
}

// ============================================================
// FindFirstLandscape - SAFE world iteration
// ============================================================

ALandscape* UForgeHeightmapCapture::FindFirstLandscape() const
{
    if (!GEngine) return nullptr;

    // VERIFIED FIX: GEditor->GetEditorWorldContext().World() calls check(0) if no editor context
    // exists (same crash as v0.2 PCGCapture). Use safe GEngine world iteration instead.
    UWorld* World = nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
    {
        if (Ctx.WorldType == EWorldType::Editor)
        {
            World = Ctx.World();
            break;
        }
    }

    if (!World) return nullptr;

    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        return *It;  // Return first landscape found
    }

    return nullptr;
}

// ============================================================
// Write height-slice.json
// ============================================================

bool UForgeHeightmapCapture::WriteHeightJSON(ALandscape* Landscape,
    const TArray<TArray<float>>& Heights,
    const FVector2D& WorldMin, const FVector2D& WorldMax,
    float MinHeightCm, float MaxHeightCm, int32 Resolution)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"),       FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("landscape_actor"), Landscape->GetName());
    Root->SetStringField(TEXT("data_encoding"),   TEXT("normalized_float_0_to_1"));

    // World bounds
    TSharedPtr<FJsonObject> Bounds    = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> BoundsMin = MakeShared<FJsonObject>();
    BoundsMin->SetNumberField(TEXT("x"), WorldMin.X);
    BoundsMin->SetNumberField(TEXT("y"), WorldMin.Y);
    TSharedPtr<FJsonObject> BoundsMax = MakeShared<FJsonObject>();
    BoundsMax->SetNumberField(TEXT("x"), WorldMax.X);
    BoundsMax->SetNumberField(TEXT("y"), WorldMax.Y);
    Bounds->SetObjectField(TEXT("min"), BoundsMin);
    Bounds->SetObjectField(TEXT("max"), BoundsMax);
    Root->SetObjectField(TEXT("world_bounds"), Bounds);

    // Resolution
    TSharedPtr<FJsonObject> Res = MakeShared<FJsonObject>();
    Res->SetNumberField(TEXT("width"),  Resolution);
    Res->SetNumberField(TEXT("height"), Resolution);
    Root->SetObjectField(TEXT("resolution"), Res);

    // Height range
    TSharedPtr<FJsonObject> HeightRange = MakeShared<FJsonObject>();
    HeightRange->SetNumberField(TEXT("min_cm"), MinHeightCm);
    HeightRange->SetNumberField(TEXT("max_cm"), MaxHeightCm);
    Root->SetObjectField(TEXT("height_range"), HeightRange);

    // 2D float array - row 0 = lowest Y (south in UE left-handed coords)
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const TArray<float>& Row : Heights)
    {
        TArray<TSharedPtr<FJsonValue>> Cols;
        for (float H : Row)
        {
            Cols.Add(MakeShared<FJsonValueNumber>(H));
        }
        Rows.Add(MakeShared<FJsonValueArray>(Cols));
    }
    Root->SetArrayField(TEXT("data"), Rows);

    return FForgeContextWriter::WriteJSON(OutputDir / TEXT("landscape"),
                                           TEXT("height-slice.json"), Root.ToSharedRef());
}

// ============================================================
// Write grayscale PNG thumbnail
// ============================================================

bool UForgeHeightmapCapture::WriteHeightThumbnail(const TArray<TArray<float>>& Heights,
                                                    int32 Resolution)
{
    // Build 16-bit grayscale pixel array.
    // Engine source (LandscapeFileFormatPng.cpp) confirms uint16 G16 is the landscape ceiling -
    // 8-bit triggers an explicit "lower quality" warning on import.
    // Normalized float 0-1 maps to uint16 0-65535 (same scale as UE's internal height storage).
    TArray<uint16> Pixels;
    Pixels.SetNum(Resolution * Resolution);
    for (int32 Row = 0; Row < Resolution; Row++)
    {
        for (int32 Col = 0; Col < Resolution; Col++)
        {
            float H = FMath::Clamp(Heights[Row][Col], 0.0f, 1.0f);
            Pixels[Row * Resolution + Col] = static_cast<uint16>(FMath::RoundToInt(H * 65535.0f));
        }
    }

    // Encode as 16-bit grayscale PNG via IImageWrapperModule
    // VERIFIED: IImageWrapper::GetCompressed() returns TArray64<uint8> in UE 5.7
    // VERIFIED: SetRaw(const void*, int64 size, int32 w, int32 h, ERGBFormat, int32 bitDepth)
    IImageWrapperModule& IWM =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> Wrapper = IWM.CreateImageWrapper(EImageFormat::PNG);
    if (!Wrapper.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeHeightmap: Failed to create PNG ImageWrapper"));
        return false;
    }

    // ERGBFormat::Gray + bitDepth 16 = 16-bit grayscale PNG (G16)
    // ByteSize = Resolution * Resolution * 2 (2 bytes per uint16 pixel)
    Wrapper->SetRaw(Pixels.GetData(), static_cast<int64>(Pixels.Num() * sizeof(uint16)),
                    Resolution, Resolution, ERGBFormat::Gray, 16);

    // GetCompressed returns TArray64<uint8> - convert to TArray<uint8> for WriteBytes
    TArray64<uint8> CompressedData = Wrapper->GetCompressed(100);
    TArray<uint8> PNGBytes;
    PNGBytes.Append(CompressedData.GetData(), CompressedData.Num());

    return FForgeContextWriter::WriteBytes(OutputDir / TEXT("landscape"),
                                             TEXT("height-thumb.png"), PNGBytes);
}

// ============================================================
// Index - READ-MERGE-WRITE preserving all other capture sections
// ============================================================

void UForgeHeightmapCapture::UpdateIndexFile()
{
    const FString IndexPath = OutputDir / TEXT("index.json");
    const FString Timestamp = FForgeContextWriter::NowISO8601();

    // Load and parse existing index.json if present
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

    // Get or create captures_available section
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

    // Set only the landscape section - leave pcg/screenshot/runtime untouched
    TSharedPtr<FJsonObject> LandscapeSection = MakeShared<FJsonObject>();
    LandscapeSection->SetStringField(TEXT("height_json"),  TEXT("landscape/height-slice.json"));
    LandscapeSection->SetStringField(TEXT("height_thumb"), TEXT("landscape/height-thumb.png"));
    LandscapeSection->SetStringField(TEXT("last_updated"), Timestamp);
    Captures->SetObjectField(TEXT("landscape_heightmap"), LandscapeSection);

    Root->SetObjectField(TEXT("captures_available"), Captures);
    Root->SetStringField(TEXT("updated"),        Timestamp);
    Root->SetStringField(TEXT("plugin_version"), FORGE_BRIDGE_VERSION);

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
