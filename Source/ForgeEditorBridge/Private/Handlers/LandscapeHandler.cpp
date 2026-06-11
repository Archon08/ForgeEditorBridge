#include "Handlers/LandscapeHandler.h"
#include "ForgeAISubsystem.h"

// ---- Landscape types -------------------------------------------------------
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeEdit.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeComponent.h"

// ---- World / editor --------------------------------------------------------
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"

// ---- Material --------------------------------------------------------------
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

// ---- Heightmap I/O ---------------------------------------------------------
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "LandscapeEditorUtils.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---- Capture ---------------------------------------------------------------
#include "Capture/ForgeHeightmapCapture.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void ULandscapeHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("landscape"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_landscape"))  return Action_CreateLandscape(Params);
	if (Action == TEXT("get_landscape_info")) return Action_GetLandscapeInfo(Params);
	if (Action == TEXT("sculpt_height"))     return Action_SculptHeight(Params);
	if (Action == TEXT("paint_layer"))       return Action_PaintLayer(Params);
	if (Action == TEXT("set_material"))      return Action_SetMaterial(Params);
	if (Action == TEXT("add_spline"))              return Action_AddSpline(Params);
	if (Action == TEXT("read_heightmap_capture"))  return Action_ReadHeightmapCapture(Params);
	if (Action == TEXT("apply_noise"))             return Action_ApplyNoise(Params);
	if (Action == TEXT("auto_paint"))              return Action_AutoPaint(Params);
	if (Action == TEXT("set_hole_mask"))           return Action_SetHoleMask(Params);
	if (Action == TEXT("import_weightmap"))        return Action_ImportWeightmap(Params);
	if (Action == TEXT("export_heightmap"))        return Action_ExportHeightmap(Params);
	// Phase 1d additions
	if (Action == TEXT("set_wpo_disable_distance")) return Action_SetWPODisableDistance(Params);
	if (Action == TEXT("build_hlods"))              return Action_BuildHLODs(Params);
	if (Action == TEXT("import_heightmap"))         return Action_ImportHeightmap(Params);
	if (Action == TEXT("get_terrain_data"))         return Action_GetTerrainData(Params);
	if (Action == TEXT("set_layer_info"))           return Action_SetLayerInfo(Params);

	return MakeError(TEXT("landscape"), Action, 1001,
		FString::Printf(TEXT("Unknown landscape action '%s'"), *Action),
		TEXT("Valid: create_landscape, get_landscape_info, sculpt_height, paint_layer, set_material, add_spline, read_heightmap_capture, apply_noise, auto_paint, set_hole_mask, import_weightmap, export_heightmap, set_wpo_disable_distance, build_hlods, import_heightmap, get_terrain_data, set_layer_info"));
}

// ---------------------------------------------------------------------------
// create_landscape
// ---------------------------------------------------------------------------

static constexpr uint16 LANDSCAPE_MID_HEIGHT = 32768;

FBridgeResult ULandscapeHandler::Action_CreateLandscape(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("create_landscape");

	// ---- Parse params -------------------------------------------------------
	FString Name, HeightmapPath;
	double LocationX = 0, LocationY = 0, LocationZ = 0;
	double ScaleX = 100, ScaleY = 100, ScaleZ = 100;
	int32 ComponentCountX = 8, ComponentCountY = 8;
	int32 SectionsPerComponent = 1, QuadsPerSection = 63;

	Params->TryGetStringField(TEXT("name"), Name);
	if (Name.IsEmpty()) Name = TEXT("Landscape");
	Params->TryGetStringField(TEXT("heightmap_path"), HeightmapPath);
	Params->TryGetNumberField(TEXT("location_x"), LocationX);
	Params->TryGetNumberField(TEXT("location_y"), LocationY);
	Params->TryGetNumberField(TEXT("location_z"), LocationZ);
	Params->TryGetNumberField(TEXT("scale_x"), ScaleX);
	Params->TryGetNumberField(TEXT("scale_y"), ScaleY);
	Params->TryGetNumberField(TEXT("scale_z"), ScaleZ);

	double TempD;
	if (Params->TryGetNumberField(TEXT("component_count_x"),       TempD)) ComponentCountX       = (int32)TempD;
	if (Params->TryGetNumberField(TEXT("component_count_y"),       TempD)) ComponentCountY       = (int32)TempD;
	if (Params->TryGetNumberField(TEXT("sections_per_component"),  TempD)) SectionsPerComponent  = (int32)TempD;
	if (Params->TryGetNumberField(TEXT("quads_per_section"),       TempD)) QuadsPerSection       = (int32)TempD;

	// ---- Validate -----------------------------------------------------------
	if (SectionsPerComponent != 1 && SectionsPerComponent != 2)
		return MakeError(TEXT("landscape"), Action, 1001,
			TEXT("sections_per_component must be 1 or 2"));

	const TArray<int32> ValidQuads = {7, 15, 31, 63, 127, 255};
	if (!ValidQuads.Contains(QuadsPerSection))
		return MakeError(TEXT("landscape"), Action, 1002,
			TEXT("quads_per_section must be one of: 7, 15, 31, 63, 127, 255"));

	// ---- Calculate vertex grid size -----------------------------------------
	const int32 SizeX = QuadsPerSection * SectionsPerComponent * ComponentCountX + 1;
	const int32 SizeY = QuadsPerSection * SectionsPerComponent * ComponentCountY + 1;
	const int32 TotalVerts = SizeX * SizeY;

	// ---- Build height data --------------------------------------------------
	TArray<uint16> HeightData;
	bool bFromFile = !HeightmapPath.IsEmpty();

	if (bFromFile)
	{
		const FString Ext = FPaths::GetExtension(HeightmapPath).ToLower();

		if (Ext == TEXT("r16") || Ext == TEXT("raw"))
		{
			TArray<uint8> RawBytes;
			if (!FFileHelper::LoadFileToArray(RawBytes, *HeightmapPath))
				return MakeError(TEXT("landscape"), Action, 2000,
					FString::Printf(TEXT("Cannot read file: %s"), *HeightmapPath));

			const int32 ExpectedBytes = TotalVerts * 2;
			if (RawBytes.Num() != ExpectedBytes)
				return MakeError(TEXT("landscape"), Action, 2001,
					FString::Printf(
						TEXT("File size mismatch: got %d bytes, expected %d. "
						     "Landscape vertex grid is %dx%d — adjust component_count_x/y or quads_per_section."),
						RawBytes.Num(), ExpectedBytes, SizeX, SizeY),
					FString::Printf(TEXT("Required landscape vertex size: %dx%d"), SizeX, SizeY));

			HeightData.SetNumUninitialized(TotalVerts);
			FMemory::Memcpy(HeightData.GetData(), RawBytes.GetData(), ExpectedBytes);
		}
		else if (Ext == TEXT("png"))
		{
			TArray<uint8> PNGBytes;
			if (!FFileHelper::LoadFileToArray(PNGBytes, *HeightmapPath))
				return MakeError(TEXT("landscape"), Action, 2000,
					FString::Printf(TEXT("Cannot read file: %s"), *HeightmapPath));

			IImageWrapperModule& ImageWrapperModule =
				FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			TSharedPtr<IImageWrapper> Wrapper =
				ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

			if (!Wrapper.IsValid() || !Wrapper->SetCompressed(PNGBytes.GetData(), PNGBytes.Num()))
				return MakeError(TEXT("landscape"), Action, 2002,
					FString::Printf(TEXT("Failed to decode PNG: %s"), *HeightmapPath),
					TEXT("Ensure the file is a valid 8-bit or 16-bit grayscale PNG."));

			const int32 ImgW = Wrapper->GetWidth();
			const int32 ImgH = Wrapper->GetHeight();
			if (ImgW != SizeX || ImgH != SizeY)
				return MakeError(TEXT("landscape"), Action, 2003,
					FString::Printf(
						TEXT("PNG dimensions %dx%d do not match landscape vertex grid %dx%d. "
						     "Adjust component_count_x/y or quads_per_section."),
						ImgW, ImgH, SizeX, SizeY),
					FString::Printf(TEXT("Required image size: %dx%d"), SizeX, SizeY));

			HeightData.SetNumUninitialized(TotalVerts);

			// Try 16-bit first, fall back to 8-bit
			TArray64<uint8> RawPixels;
			if (Wrapper->GetRaw(ERGBFormat::Gray, 16, RawPixels) && RawPixels.Num() == (int64)TotalVerts * 2)
			{
				// Native platform byte order (LE on Windows) — copy directly
				FMemory::Memcpy(HeightData.GetData(), RawPixels.GetData(), TotalVerts * 2);
			}
			else if (Wrapper->GetRaw(ERGBFormat::Gray, 8, RawPixels) && RawPixels.Num() == (int64)TotalVerts)
			{
				// Scale 8-bit [0,255] → 16-bit [0,65535]
				for (int32 i = 0; i < TotalVerts; ++i)
					HeightData[i] = (uint16)((uint32)RawPixels[i] * 257u);
			}
			else
			{
				return MakeError(TEXT("landscape"), Action, 1001,
					TEXT("Could not extract pixel data from PNG. Use an 8-bit or 16-bit grayscale PNG."));
			}
		}
		else
		{
			return MakeError(TEXT("landscape"), Action, 1001,
				FString::Printf(TEXT("Unsupported heightmap format '.%s'. Supported: .r16, .raw, .png"), *Ext),
				TEXT(".r16/.raw = raw uint16 little-endian, .png = 8 or 16-bit grayscale"));
		}
	}
	else
	{
		// Flat landscape — midpoint height
		HeightData.Init(LANDSCAPE_MID_HEIGHT, TotalVerts);
	}

	// ---- Spawn landscape actor ----------------------------------------------
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
		return MakeError(TEXT("landscape"), Action, 3000, TEXT("No editor world available"));

	FActorSpawnParameters SpawnParams;
	SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	ALandscape* Landscape = World->SpawnActor<ALandscape>(
		FVector((float)LocationX, (float)LocationY, (float)LocationZ),
		FRotator::ZeroRotator,
		SpawnParams);

	if (!Landscape)
		return MakeError(TEXT("landscape"), Action, 3001, TEXT("SpawnActor<ALandscape> failed — check output log"));

	Landscape->SetActorLabel(Name);
	Landscape->SetActorScale3D(FVector((float)ScaleX, (float)ScaleY, (float)ScaleZ));

	// ---- Import height data (two-step to avoid edit-layer GUID mismatch) ----
	const FGuid LandscapeGuid = FGuid::NewGuid();
	Landscape->SetLandscapeGuid(LandscapeGuid);

	// Import() in UE 5.7 always keys HeightDataMap with FGuid() (the zero GUID),
	// not the landscape GUID passed as InGuid. Using LandscapeGuid as the key
	// causes FindChecked(FGuid()) to assert. Key with FGuid() instead.
	const FGuid ImportKey = FGuid();   // must match what Import() looks up internally

	TMap<FGuid, TArray<uint16>> HeightDataMap;
	HeightDataMap.Add(ImportKey, MoveTemp(HeightData));

	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerInfoMap;
	LayerInfoMap.Add(ImportKey, TArray<FLandscapeImportLayerInfo>());

	TArray<FLandscapeLayer> ImportLayers;

	Landscape->Import(
		LandscapeGuid,
		0, 0, SizeX - 1, SizeY - 1,
		SectionsPerComponent,
		QuadsPerSection,
		HeightDataMap,
		nullptr,
		LayerInfoMap,
		ELandscapeImportAlphamapType::Additive,
		TArrayView<const FLandscapeLayer>(ImportLayers));

	Landscape->MarkPackageDirty();

	if (GEditor)
		GEditor->RedrawLevelEditingViewports();

	// ---- Response -----------------------------------------------------------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"),                  Name);
	Data->SetStringField(TEXT("guid"),                  LandscapeGuid.ToString());
	Data->SetNumberField(TEXT("size_x"),                SizeX);
	Data->SetNumberField(TEXT("size_y"),                SizeY);
	Data->SetNumberField(TEXT("component_count_x"),     ComponentCountX);
	Data->SetNumberField(TEXT("component_count_y"),     ComponentCountY);
	Data->SetNumberField(TEXT("sections_per_component"),SectionsPerComponent);
	Data->SetNumberField(TEXT("quads_per_section"),     QuadsPerSection);
	Data->SetBoolField  (TEXT("from_heightmap"),        bFromFile);

	const FString Msg = bFromFile
		? FString::Printf(TEXT("Created landscape '%s' from '%s' (%dx%d vertices)"),
			*Name, *FPaths::GetCleanFilename(HeightmapPath), SizeX, SizeY)
		: FString::Printf(TEXT("Created flat landscape '%s' (%dx%d vertices, %.0fcm scale)"),
			*Name, SizeX, SizeY, ScaleX);

	FBridgeResult R = MakeSuccess(TEXT("landscape"), Action, Msg, Data);
	R.AffectedPath = Name;
	return R;
}

// ---------------------------------------------------------------------------
// get_landscape_info
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_GetLandscapeInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_landscape_info");

	FString LandscapeName;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'landscape_name'. Pass \"first\" to target the first landscape."));

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	Data->SetStringField(TEXT("name"), Landscape->GetActorLabel());
	Data->SetStringField(TEXT("guid"), Landscape->GetLandscapeGuid().ToString());

	// Location
	const FVector Loc = Landscape->GetActorLocation();
	TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Data->SetObjectField(TEXT("location"), LocObj);

	// Scale
	const FVector Scale = Landscape->GetActorScale3D();
	TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
	Data->SetObjectField(TEXT("scale"), ScaleObj);

	// Geometry
	Data->SetNumberField(TEXT("sections_per_component"), Landscape->NumSubsections);
	Data->SetNumberField(TEXT("quads_per_section"),      Landscape->SubsectionSizeQuads);
	Data->SetNumberField(TEXT("component_size_quads"),   Landscape->ComponentSizeQuads);
	Data->SetNumberField(TEXT("component_count"),        Landscape->LandscapeComponents.Num());

	// Derive vertex grid size from section offset and component count
	// (LandscapeSectionOffset gives the world-space offset; we use component counts)
	const int32 SectionsPerComp = Landscape->NumSubsections;
	const int32 QuadsPerSection = Landscape->SubsectionSizeQuads;
	const int32 CompCount       = Landscape->LandscapeComponents.Num();
	// Approximate square root for NxN grids; exact for non-square grids the user already knows dims
	const int32 SqrtComp        = FMath::RoundToInt(FMath::Sqrt((float)CompCount));
	Data->SetNumberField(TEXT("vertex_grid_approx_x"), QuadsPerSection * SectionsPerComp * SqrtComp + 1);
	Data->SetNumberField(TEXT("vertex_grid_approx_y"), QuadsPerSection * SectionsPerComp * SqrtComp + 1);

	// Material
	Data->SetStringField(TEXT("material"),
		Landscape->LandscapeMaterial
			? Landscape->LandscapeMaterial->GetPathName()
			: TEXT("none"));

	// Bounds
	const FBox Bounds = Landscape->GetComponentsBoundingBox();
	TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
	BoundsObj->SetNumberField(TEXT("min_x"), Bounds.Min.X);
	BoundsObj->SetNumberField(TEXT("min_y"), Bounds.Min.Y);
	BoundsObj->SetNumberField(TEXT("min_z"), Bounds.Min.Z);
	BoundsObj->SetNumberField(TEXT("max_x"), Bounds.Max.X);
	BoundsObj->SetNumberField(TEXT("max_y"), Bounds.Max.Y);
	BoundsObj->SetNumberField(TEXT("max_z"), Bounds.Max.Z);
	Data->SetObjectField(TEXT("bounds"), BoundsObj);

	return MakeSuccess(TEXT("landscape"), Action,
		FString::Printf(TEXT("Landscape '%s': %d component(s), %d sections/comp, %d quads/section"),
			*Landscape->GetActorLabel(),
			Landscape->LandscapeComponents.Num(),
			Landscape->NumSubsections,
			Landscape->SubsectionSizeQuads),
		Data);
}

// ---------------------------------------------------------------------------
// sculpt_height
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_SculptHeight(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("sculpt_height");

	// ---- Parse params -------------------------------------------------------
	FString LandscapeName, Mode;
	double CenterX = 0.0, CenterY = 0.0, Radius = 10.0, Strength = 1.0;
	double Delta = 1000.0, TargetHeight = 0.0;
	bool bHasTargetHeight = false;

	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000, TEXT("Missing required param: 'landscape_name'"));

	Params->TryGetStringField(TEXT("mode"), Mode);
	if (Mode.IsEmpty()) Mode = TEXT("raise");

	Params->TryGetNumberField(TEXT("x"),        CenterX);
	Params->TryGetNumberField(TEXT("y"),        CenterY);
	Params->TryGetNumberField(TEXT("radius"),   Radius);
	Params->TryGetNumberField(TEXT("strength"), Strength);
	Params->TryGetNumberField(TEXT("delta"),    Delta);
	bHasTargetHeight = Params->TryGetNumberField(TEXT("target_height"), TargetHeight);

	Strength = FMath::Clamp(Strength, 0.0, 1.0);
	if (Radius < 1.0) Radius = 1.0;

	static const TArray<FString> ValidModes = { TEXT("raise"), TEXT("lower"), TEXT("flatten"), TEXT("smooth") };
	if (!ValidModes.Contains(Mode))
		return MakeError(TEXT("landscape"), Action, 1001,
			FString::Printf(TEXT("Invalid mode '%s'. Valid: raise, lower, flatten, smooth"), *Mode));

	// ---- Find landscape -----------------------------------------------------
	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message   = FString::Printf(TEXT("sculpt_height: GetLandscapeInfo() returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// ---- Brush rect in section-space vertex coords --------------------------
	const FIntPoint SectionBase = Landscape->GetSectionBase();
	const FIntRect  LandBounds  = Landscape->GetBoundingRect() + SectionBase;

	const int32 MinX = FMath::Max(LandBounds.Min.X, FMath::FloorToInt((float)(CenterX - Radius)));
	const int32 MinY = FMath::Max(LandBounds.Min.Y, FMath::FloorToInt((float)(CenterY - Radius)));
	const int32 MaxX = FMath::Min(LandBounds.Max.X, FMath::CeilToInt ((float)(CenterX + Radius)));
	const int32 MaxY = FMath::Min(LandBounds.Max.Y, FMath::CeilToInt ((float)(CenterY + Radius)));

	if (MinX > MaxX || MinY > MaxY)
	{
		Result.Message   = TEXT("sculpt_height: brush is entirely outside the landscape bounds");
		Result.ErrorCode = 1002;
		return Result;
	}

	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// ---- Read current height data -------------------------------------------
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(Width * Height);

	{
		FHeightmapAccessor<false> Accessor(Info);
		int32 RX1 = MinX, RY1 = MinY, RX2 = MaxX, RY2 = MaxY;
		Accessor.GetData(RX1, RY1, RX2, RY2, HeightData.GetData());
	}

	// ---- Delta conversion: world cm → uint16 units --------------------------
	// world_z = (value - 32768) / 128.0 * ScaleZ  =>  d_value = d_cm * 128.0 / ScaleZ
	const float ScaleZ    = FMath::Max(Landscape->GetActorScale3D().Z, 0.001f);
	const float DeltaU16  = (float)(Delta * 128.0 / ScaleZ);

	// ---- Flatten target -----------------------------------------------------
	uint16 FlattenTarget = LANDSCAPE_MID_HEIGHT;
	if (Mode == TEXT("flatten"))
	{
		if (bHasTargetHeight)
		{
			const int32 Raw = FMath::RoundToInt(32768.0f + (float)TargetHeight * 128.0f / ScaleZ);
			FlattenTarget = (uint16)FMath::Clamp(Raw, 0, 65535);
		}
		else
		{
			// Sample the landscape at the brush center
			const int32 CX = FMath::Clamp(FMath::RoundToInt((float)CenterX) - MinX, 0, Width  - 1);
			const int32 CY = FMath::Clamp(FMath::RoundToInt((float)CenterY) - MinY, 0, Height - 1);
			FlattenTarget = HeightData[CY * Width + CX];
		}
	}

	// ---- Smooth: snapshot original values to read neighbours from -----------
	TArray<uint16> SmoothSrc;
	if (Mode == TEXT("smooth"))
		SmoothSrc = HeightData;

	// ---- Apply brush --------------------------------------------------------
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const float Dist = FVector2D((float)(X - CenterX), (float)(Y - CenterY)).Size();
			if (Dist > (float)Radius) continue;

			// Linear falloff: 1 at centre, 0 at edge
			const float Falloff  = (1.0f - Dist / (float)Radius) * (float)Strength;
			const int32 Idx      = (Y - MinY) * Width + (X - MinX);
			const uint16 Current = HeightData[Idx];
			int32 NewVal         = (int32)Current;

			if (Mode == TEXT("raise"))
			{
				NewVal = (int32)Current + FMath::RoundToInt(DeltaU16 * Falloff);
			}
			else if (Mode == TEXT("lower"))
			{
				NewVal = (int32)Current - FMath::RoundToInt(DeltaU16 * Falloff);
			}
			else if (Mode == TEXT("flatten"))
			{
				NewVal = FMath::RoundToInt(FMath::Lerp((float)Current, (float)FlattenTarget, Falloff));
			}
			else if (Mode == TEXT("smooth"))
			{
				// 3x3 neighbourhood average using the pre-modified snapshot
				int32 Sum = 0, Count = 0;
				for (int32 NY = Y - 1; NY <= Y + 1; ++NY)
				{
					for (int32 NX = X - 1; NX <= X + 1; ++NX)
					{
						const int32 LX = NX - MinX;
						const int32 LY = NY - MinY;
						if (LX >= 0 && LX < Width && LY >= 0 && LY < Height)
						{
							Sum += SmoothSrc[LY * Width + LX];
							++Count;
						}
					}
				}
				const float Avg = (Count > 0) ? (float)Sum / (float)Count : (float)Current;
				NewVal = FMath::RoundToInt(FMath::Lerp((float)Current, Avg, Falloff));
			}

			HeightData[Idx] = (uint16)FMath::Clamp(NewVal, 0, 65535);
		}
	}

	// ---- Write back ---------------------------------------------------------
	{
		FHeightmapAccessor<false> Accessor(Info);
		Accessor.SetData(MinX, MinY, MaxX, MaxY, HeightData.GetData());
	}

	Landscape->MarkPackageDirty();
	if (GEditor)
		GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("Sculpted '%s' mode=%s center=(%.0f,%.0f) radius=%.0f strength=%.2f delta=%.0fcm"),
		*LandscapeName, *Mode, (float)CenterX, (float)CenterY, (float)Radius, (float)Strength, (float)Delta);
	return Result;
}

// ---------------------------------------------------------------------------
// read_heightmap_capture
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_ReadHeightmapCapture(TSharedPtr<FJsonObject> Params)
{
	int32 Resolution = 256;
	FString LandscapeNameStr;
	double TempD;
	if (Params->TryGetNumberField(TEXT("resolution"), TempD))
		Resolution = (int32)TempD;
	Params->TryGetStringField(TEXT("landscape_name"), LandscapeNameStr);

	if (Subsystem->HeightmapCapture)
	{
		if (!LandscapeNameStr.IsEmpty())
		{
			FBridgeResult FindResult = CreateResult(GetDomainName(), TEXT("read_heightmap_capture"));
			ALandscape* LandscapeActor = FindLandscape(LandscapeNameStr, FindResult);
			if (!LandscapeActor)
				return FindResult;
			Subsystem->HeightmapCapture->ExportHeightmapForActor(LandscapeActor, Resolution);
		}
		else
		{
			Subsystem->HeightmapCapture->ExportHeightmap(Resolution);
		}
	}

	FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("landscape/height-slice.json"));
	FString FileContent;
	FBridgeResult Res = MakeSuccess(GetDomainName(), TEXT("read_heightmap_capture"), TEXT("Capture complete"));
	if (FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
			Res.Data = JsonObj;
	}
	else
	{
		Res.Message = FString::Printf(
			TEXT("Capture complete — file not yet available for reading at: %s"), *FilePath);
	}
	return Res;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> ULandscapeHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"),     Type);
		O->SetBoolField  (TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"),     Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// create_landscape
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Spawn a new ALandscape actor. Optionally import a heightmap (.r16/.raw/.png)."));
		Pr->SetObjectField(TEXT("name"),                   P(TEXT("string"), false, TEXT("Actor label (default: Landscape)")));
		Pr->SetObjectField(TEXT("component_count_x"),      P(TEXT("int"),    false, TEXT("Number of components in X (default: 8)")));
		Pr->SetObjectField(TEXT("component_count_y"),      P(TEXT("int"),    false, TEXT("Number of components in Y (default: 8)")));
		Pr->SetObjectField(TEXT("sections_per_component"), P(TEXT("int"),    false, TEXT("1 or 2 (default: 1)")));
		Pr->SetObjectField(TEXT("quads_per_section"),      P(TEXT("int"),    false, TEXT("7|15|31|63|127|255 (default: 63)")));
		Pr->SetObjectField(TEXT("location_x"),             P(TEXT("float"),  false, TEXT("World X (cm, default 0)")));
		Pr->SetObjectField(TEXT("location_y"),             P(TEXT("float"),  false, TEXT("World Y (cm, default 0)")));
		Pr->SetObjectField(TEXT("location_z"),             P(TEXT("float"),  false, TEXT("World Z (cm, default 0)")));
		Pr->SetObjectField(TEXT("scale_x"),                P(TEXT("float"),  false, TEXT("Scale X (cm/vertex, default 100)")));
		Pr->SetObjectField(TEXT("scale_y"),                P(TEXT("float"),  false, TEXT("Scale Y (cm/vertex, default 100)")));
		Pr->SetObjectField(TEXT("scale_z"),                P(TEXT("float"),  false, TEXT("Scale Z (cm/unit, default 100)")));
		Pr->SetObjectField(TEXT("heightmap_path"),         P(TEXT("string"), false,
			TEXT("Absolute path to .r16 (raw uint16 LE), .raw (same), or .png (8/16-bit grayscale). "
			     "Omit for flat terrain. File must be exactly SizeX*SizeY vertices: "
			     "SizeX = quads_per_section * sections_per_component * component_count_x + 1")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("create_landscape"), A);
	}

	// get_landscape_info
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Read metadata from an existing landscape: size, scale, components, material, bounds."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true, TEXT("Actor label, or \"first\" for first landscape in level")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("get_landscape_info"), A);
	}

	// sculpt_height
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Sculpt landscape height with a circular brush. "
			     "mode=raise/lower adds/subtracts delta (world cm) with linear falloff. "
			     "mode=flatten levels to target_height (or the center point's current height). "
			     "mode=smooth averages neighbours with linear falloff. "
			     "x/y are section-space vertex indices (0,0 = top-left of a default landscape)."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("x"),              P(TEXT("float"),  false, TEXT("Brush centre X (section-space vertex index, default 0)")));
		Pr->SetObjectField(TEXT("y"),              P(TEXT("float"),  false, TEXT("Brush centre Y (section-space vertex index, default 0)")));
		Pr->SetObjectField(TEXT("radius"),         P(TEXT("float"),  false, TEXT("Brush radius in vertex units (default 10)")));
		Pr->SetObjectField(TEXT("strength"),       P(TEXT("float"),  false, TEXT("Brush strength 0–1, controls falloff blend (default 1.0)")));
		Pr->SetObjectField(TEXT("mode"),           P(TEXT("string"), false, TEXT("raise | lower | flatten | smooth (default: raise)")));
		Pr->SetObjectField(TEXT("delta"),          P(TEXT("float"),  false, TEXT("Height change in world cm, used by raise/lower (default 1000)")));
		Pr->SetObjectField(TEXT("target_height"),  P(TEXT("float"),  false, TEXT("Target world-cm height for flatten mode; omit to use the center point's current height")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("sculpt_height"), A);
	}

	// paint_layer
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Paint a layer weight on the landscape using a circular brush."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("layer_name"),     P(TEXT("string"), true,  TEXT("Layer name matching a layer in the landscape material")));
		Pr->SetObjectField(TEXT("x"),              P(TEXT("float"),  false, TEXT("Brush centre X in vertex indices")));
		Pr->SetObjectField(TEXT("y"),              P(TEXT("float"),  false, TEXT("Brush centre Y in vertex indices")));
		Pr->SetObjectField(TEXT("radius"),         P(TEXT("float"),  false, TEXT("Brush radius in vertex units (default 10)")));
		Pr->SetObjectField(TEXT("strength"),       P(TEXT("float"),  false, TEXT("Paint strength 0-1 (default 1.0)")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("paint_layer"), A);
	}

	// set_material
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Assign a material to a landscape actor."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true, TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("material_path"),  P(TEXT("string"), true, TEXT("Content path to a UMaterialInterface asset")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("set_material"), A);
	}

	// add_spline
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a landscape spline control point at the given local-space position."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("x"),              P(TEXT("float"),  false, TEXT("Control point X (local space)")));
		Pr->SetObjectField(TEXT("y"),              P(TEXT("float"),  false, TEXT("Control point Y (local space)")));
		Pr->SetObjectField(TEXT("z"),              P(TEXT("float"),  false, TEXT("Control point Z (local space)")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("add_spline"), A);
	}


	// apply_noise
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Procedurally displace landscape height using Perlin noise or FBM within a circular brush. "
			     "mode=add offsets current height; mode=set replaces it relative to mid-height. "
			     "x/y are section-space vertex indices."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("x"),              P(TEXT("float"),  false, TEXT("Brush centre X (vertex index, default 0)")));
		Pr->SetObjectField(TEXT("y"),              P(TEXT("float"),  false, TEXT("Brush centre Y (vertex index, default 0)")));
		Pr->SetObjectField(TEXT("radius"),         P(TEXT("float"),  false, TEXT("Brush radius in vertex units (default 10)")));
		Pr->SetObjectField(TEXT("strength"),       P(TEXT("float"),  false, TEXT("Brush strength 0-1, linear falloff (default 1.0)")));
		Pr->SetObjectField(TEXT("noise_type"),     P(TEXT("string"), false, TEXT("perlin | fbm (default: perlin)")));
		Pr->SetObjectField(TEXT("frequency"),      P(TEXT("float"),  false, TEXT("Noise spatial frequency, higher = finer detail (default 1.0)")));
		Pr->SetObjectField(TEXT("amplitude"),      P(TEXT("float"),  false, TEXT("Max height displacement in world cm (default 500.0)")));
		Pr->SetObjectField(TEXT("mode"),           P(TEXT("string"), false, TEXT("add (offset current height) | set (replace from mid) (default: add)")));
		Pr->SetObjectField(TEXT("octaves"),        P(TEXT("int"),    false, TEXT("FBM octave count 1-8 (default 4, only used with noise_type=fbm)")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("apply_noise"), A);
	}

	// auto_paint
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Iterate all landscape vertices and paint layers based on height and slope rules. "
			     "Rules are evaluated in order; the first matching rule per vertex wins. "
			     "blend_width softens the edges of each height band."));
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("rules"),          P(TEXT("array"),  true,
			TEXT("Array of rule objects: {layer_name(string,req), height_min(float,opt), height_max(float,opt), "
			     "slope_min(float,opt,deg), slope_max(float,opt,deg), blend_width(float,opt,default 10 world-cm)}")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("auto_paint"), A);
	}

	// set_hole_mask
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Punch holes in a landscape within a rectangular region by writing the visibility layer. "
			     "x/y are section-space vertex indices. hole=true removes geometry; hole=false restores it."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("x"),              P(TEXT("int"),    true,  TEXT("Top-left vertex X (section-space)")));
		Pr->SetObjectField(TEXT("y"),              P(TEXT("int"),    true,  TEXT("Top-left vertex Y (section-space)")));
		Pr->SetObjectField(TEXT("width"),          P(TEXT("int"),    true,  TEXT("Region width in vertices")));
		Pr->SetObjectField(TEXT("height"),         P(TEXT("int"),    true,  TEXT("Region height in vertices")));
		Pr->SetObjectField(TEXT("hole"),           P(TEXT("bool"),   false, TEXT("true = punch hole, false = fill (default: true)")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("set_hole_mask"), A);
	}

	// import_weightmap
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Import an 8-bit grayscale PNG or RAW file and write its values as the alpha channel "
			     "for a named landscape layer. The image must match the landscape vertex resolution exactly."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), true,  TEXT("Actor label or 'first'")));
		Pr->SetObjectField(TEXT("layer_name"),     P(TEXT("string"), true,  TEXT("Name of the landscape layer to write")));
		Pr->SetObjectField(TEXT("file_path"),      P(TEXT("string"), true,  TEXT("Absolute path to an 8-bit grayscale .png or .raw file")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("import_weightmap"), A);
	}

	// export_heightmap
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"),
			TEXT("Export the landscape heightmap as a 16-bit grayscale PNG to an absolute file path. "
			     "Each pixel is one landscape vertex; value 32768 = 0 world-Z."));
		Pr->SetObjectField(TEXT("landscape_name"), P(TEXT("string"), false, TEXT("Actor label or 'first' (default: first landscape)")));
		Pr->SetObjectField(TEXT("file_path"),      P(TEXT("string"), true,  TEXT("Absolute path for the output .png file")));
		A->SetObjectField(TEXT("params"), Pr);
		Root->SetObjectField(TEXT("export_heightmap"), A);
	}

	return Root;
}

// ---------------------------------------------------------------------------
// paint_layer
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_PaintLayer(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("paint_layer"));

	FString LandscapeName, LayerName;
	double CenterX = 0.0, CenterY = 0.0, Radius = 10.0, Strength = 1.0;

	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("layer_name"),     LayerName)     || LayerName.IsEmpty())
	{
		Result.Message = TEXT("paint_layer: 'landscape_name' and 'layer_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	Params->TryGetNumberField(TEXT("x"),        CenterX);
	Params->TryGetNumberField(TEXT("y"),        CenterY);
	Params->TryGetNumberField(TEXT("radius"),   Radius);
	Params->TryGetNumberField(TEXT("strength"), Strength);
	Strength = FMath::Clamp(Strength, 0.0, 1.0);

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message = FString::Printf(
			TEXT("paint_layer: GetLandscapeInfo returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Find the layer info object by name
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
	{
		if (Layer.GetLayerName() == FName(*LayerName))
		{
			LayerInfo = Layer.LayerInfoObj;
			break;
		}
	}
	if (!LayerInfo)
	{
		Result.Message = FString::Printf(
			TEXT("paint_layer: layer '%s' not found on landscape '%s'. ")
			TEXT("Ensure the layer exists in the landscape material."),
			*LayerName, *LandscapeName);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Check that the layer name matches a layer in the landscape's material.");
		return Result;
	}

	// Compute index bounds of the circular brush region
	const int32 MinX = FMath::Max(0, FMath::FloorToInt((float)(CenterX - Radius)));
	const int32 MinY = FMath::Max(0, FMath::FloorToInt((float)(CenterY - Radius)));
	const int32 MaxX = FMath::CeilToInt((float)(CenterX + Radius));
	const int32 MaxY = FMath::CeilToInt((float)(CenterY + Radius));

	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// Build alpha data — circular falloff within radius
	TArray<uint8> AlphaData;
	AlphaData.Reserve(Width * Height);
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const float Dist = FVector2D((float)(X - CenterX), (float)(Y - CenterY)).Size();
			const float PixelWeight = (Dist <= (float)Radius) ? (float)Strength : 0.f;
			AlphaData.Add((uint8)FMath::Clamp(FMath::RoundToInt(PixelWeight * 255.f), 0, 255));
		}
	}

	FLandscapeEditDataInterface EditData(Info);
	EditData.SetAlphaData(LayerInfo, MinX, MinY, MaxX, MaxY, AlphaData.GetData(), /*Stride=*/0);

	Landscape->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("Painted layer '%s' at (%.0f, %.0f) radius=%.0f strength=%.2f on '%s'"),
		*LayerName, (float)CenterX, (float)CenterY, (float)Radius, (float)Strength, *LandscapeName);
	return Result;
}

// ---------------------------------------------------------------------------
// set_material
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_SetMaterial(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("set_material"));

	FString LandscapeName, MaterialPath;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("material_path"),  MaterialPath)  || MaterialPath.IsEmpty())
	{
		Result.Message = TEXT("set_material: 'landscape_name' and 'material_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		const FString Suffix = MaterialPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr, *Suffix);
	}
	if (!Material)
	{
		Result.Message = FString::Printf(
			TEXT("set_material: no UMaterialInterface found at '%s'"), *MaterialPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the material asset path exists in the Content Browser.");
		return Result;
	}

	Landscape->LandscapeMaterial = Material;
	Landscape->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("Material '%s' set on landscape '%s'"), *MaterialPath, *LandscapeName);
	return Result;
}

// ---------------------------------------------------------------------------
// add_spline
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_AddSpline(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("add_spline"));

	FString LandscapeName;
	double X = 0.0, Y = 0.0, Z = 0.0;

	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		Result.Message = TEXT("add_spline: 'landscape_name' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetNumberField(TEXT("x"), X);
	Params->TryGetNumberField(TEXT("y"), Y);
	Params->TryGetNumberField(TEXT("z"), Z);

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	// In UE 5.7, splines are accessed via ULandscapeSplinesComponent
	ULandscapeSplinesComponent* SplineComp = Landscape->GetComponentByClass<ULandscapeSplinesComponent>();
	if (!SplineComp)
	{
		// Create the spline component if it doesn't exist
		SplineComp = NewObject<ULandscapeSplinesComponent>(Landscape, NAME_None, RF_Transactional);
		SplineComp->RegisterComponent();
		Landscape->AddInstanceComponent(SplineComp);
	}

	if (!SplineComp)
	{
		Result.Message = TEXT("add_spline: failed to create ULandscapeSplinesComponent");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Add a control point at the specified location
	ULandscapeSplineControlPoint* ControlPoint = NewObject<ULandscapeSplineControlPoint>(SplineComp, NAME_None, RF_Transactional);
	ControlPoint->Location = FVector((float)X, (float)Y, (float)Z);
	SplineComp->GetControlPoints().Add(ControlPoint);

	SplineComp->MarkRenderStateDirty();
	Landscape->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("Added spline control point at (%.0f, %.0f, %.0f) on landscape '%s'"),
		(float)X, (float)Y, (float)Z, *LandscapeName);
	return Result;
}

// ---------------------------------------------------------------------------
// FindLandscape helper
// ---------------------------------------------------------------------------

ALandscape* ULandscapeHandler::FindLandscape(const FString& LandscapeName, FBridgeResult& Result)
{
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("FindLandscape: no editor world available");
		Result.ErrorCode = 3000;
		return nullptr;
	}

	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		ALandscape* Landscape = *It;
		if (LandscapeName == TEXT("first") || Landscape->GetActorLabel() == LandscapeName)
		{
			return Landscape;
		}
	}

	Result.Message = FString::Printf(
		TEXT("FindLandscape: no ALandscape found with label '%s' in the current level. ")
		TEXT("Pass \"first\" to target the first landscape found."),
		*LandscapeName);
	Result.ErrorCode = 2000;
	Result.RecoveryHint = TEXT("Pass \"first\" to target the first landscape, or check the actor label.");
	return nullptr;
}

// ---------------------------------------------------------------------------
// apply_noise
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_ApplyNoise(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("apply_noise");

	FString LandscapeName, NoiseType, Mode;
	double CenterX = 0.0, CenterY = 0.0, Radius = 10.0, Strength = 1.0;
	double Frequency = 1.0, Amplitude = 500.0;
	int32 Octaves = 4;

	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000, TEXT("Missing required param: 'landscape_name'"));

	Params->TryGetStringField(TEXT("noise_type"), NoiseType);
	if (NoiseType.IsEmpty()) NoiseType = TEXT("perlin");

	Params->TryGetStringField(TEXT("mode"), Mode);
	if (Mode.IsEmpty()) Mode = TEXT("add");

	Params->TryGetNumberField(TEXT("x"),         CenterX);
	Params->TryGetNumberField(TEXT("y"),         CenterY);
	Params->TryGetNumberField(TEXT("radius"),    Radius);
	Params->TryGetNumberField(TEXT("strength"),  Strength);
	Params->TryGetNumberField(TEXT("frequency"), Frequency);
	Params->TryGetNumberField(TEXT("amplitude"), Amplitude);

	double TempD;
	if (Params->TryGetNumberField(TEXT("octaves"), TempD)) Octaves = (int32)TempD;

	Strength  = FMath::Clamp(Strength, 0.0, 1.0);
	if (Radius < 1.0) Radius = 1.0;
	Frequency = FMath::Max(Frequency, 0.0001);
	Octaves   = FMath::Clamp(Octaves, 1, 8);

	if (NoiseType != TEXT("perlin") && NoiseType != TEXT("fbm"))
		return MakeError(TEXT("landscape"), Action, 1001,
			FString::Printf(TEXT("Invalid noise_type '%s'. Valid: perlin, fbm"), *NoiseType));

	if (Mode != TEXT("add") && Mode != TEXT("set"))
		return MakeError(TEXT("landscape"), Action, 1002,
			FString::Printf(TEXT("Invalid mode '%s'. Valid: add, set"), *Mode));

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message   = FString::Printf(TEXT("apply_noise: GetLandscapeInfo() returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Brush rect in section-space vertex coords
	const FIntPoint SectionBase = Landscape->GetSectionBase();
	const FIntRect  LandBounds  = Landscape->GetBoundingRect() + SectionBase;

	const int32 MinX = FMath::Max(LandBounds.Min.X, FMath::FloorToInt((float)(CenterX - Radius)));
	const int32 MinY = FMath::Max(LandBounds.Min.Y, FMath::FloorToInt((float)(CenterY - Radius)));
	const int32 MaxX = FMath::Min(LandBounds.Max.X, FMath::CeilToInt ((float)(CenterX + Radius)));
	const int32 MaxY = FMath::Min(LandBounds.Max.Y, FMath::CeilToInt ((float)(CenterY + Radius)));

	if (MinX > MaxX || MinY > MaxY)
	{
		Result.Message   = TEXT("apply_noise: brush is entirely outside the landscape bounds");
		Result.ErrorCode = 1001;
		return Result;
	}

	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// Read current height data
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(Width * Height);
	{
		FHeightmapAccessor<false> Accessor(Info);
		int32 RX1 = MinX, RY1 = MinY, RX2 = MaxX, RY2 = MaxY;
		Accessor.GetData(RX1, RY1, RX2, RY2, HeightData.GetData());
	}

	// Amplitude in uint16 units (same conversion as sculpt_height)
	const float ScaleZ   = FMath::Max(Landscape->GetActorScale3D().Z, 0.001f);
	const float AmpU16   = (float)(Amplitude * 128.0 / ScaleZ);
	const float FreqF    = (float)Frequency;
	const int32 OctI     = Octaves;
	const bool  bFBM     = (NoiseType == TEXT("fbm"));
	const bool  bSetMode = (Mode == TEXT("set"));

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const float Dist = FVector2D((float)(X - CenterX), (float)(Y - CenterY)).Size();
			if (Dist > (float)Radius) continue;

			const float Falloff = (1.0f - Dist / (float)Radius) * (float)Strength;
			const int32 Idx     = (Y - MinY) * Width + (X - MinX);

			// Compute noise value in [-1, 1]
			float NoiseVal;
			if (!bFBM)
			{
				NoiseVal = FMath::PerlinNoise2D(FVector2D((float)X * FreqF, (float)Y * FreqF));
			}
			else
			{
				float Val = 0.f, Freq = FreqF, Amp = 1.0f, TotalAmp = 0.f;
				for (int32 i = 0; i < OctI; ++i)
				{
					Val      += FMath::PerlinNoise2D(FVector2D((float)X * Freq, (float)Y * Freq)) * Amp;
					TotalAmp += Amp;
					Freq     *= 2.f;
					Amp      *= 0.5f;
				}
				NoiseVal = (TotalAmp > 0.f) ? (Val / TotalAmp) : 0.f;
			}

			const uint16 Current = HeightData[Idx];
			int32 NewVal;
			if (bSetMode)
				NewVal = (int32)LANDSCAPE_MID_HEIGHT + FMath::RoundToInt(NoiseVal * AmpU16 * Falloff);
			else
				NewVal = (int32)Current + FMath::RoundToInt(NoiseVal * AmpU16 * Falloff);

			HeightData[Idx] = (uint16)FMath::Clamp(NewVal, 0, 65535);
		}
	}

	{
		FHeightmapAccessor<false> Accessor(Info);
		Accessor.SetData(MinX, MinY, MaxX, MaxY, HeightData.GetData());
	}

	Landscape->MarkPackageDirty();
	if (GEditor) GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("Applied %s noise to '%s' center=(%.0f,%.0f) radius=%.0f amplitude=%.0fcm freq=%.2f"),
		*NoiseType, *LandscapeName, (float)CenterX, (float)CenterY, (float)Radius, (float)Amplitude, (float)Frequency);
	return Result;
}

// ---------------------------------------------------------------------------
// auto_paint
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_AutoPaint(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("auto_paint");

	FString LandscapeName;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000, TEXT("Missing required param: 'landscape_name'"));

	const TArray<TSharedPtr<FJsonValue>>* RulesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("rules"), RulesArray) || !RulesArray || RulesArray->Num() == 0)
		return MakeError(TEXT("landscape"), Action, 1001,
			TEXT("Missing required param: 'rules' (non-empty array of rule objects)"));

	struct FPaintRule
	{
		FString LayerName;
		float   HeightMin  = -1e9f;
		float   HeightMax  =  1e9f;
		float   SlopeMin   = 0.f;
		float   SlopeMax   = 90.f;
		float   BlendWidth = 10.f;
		bool    bHasSlope  = false;
	};

	TArray<FPaintRule> Rules;
	for (const TSharedPtr<FJsonValue>& Val : *RulesArray)
	{
		const TSharedPtr<FJsonObject>* RuleObj = nullptr;
		if (!Val->TryGetObject(RuleObj) || !RuleObj) continue;

		FPaintRule Rule;
		if (!(*RuleObj)->TryGetStringField(TEXT("layer_name"), Rule.LayerName) || Rule.LayerName.IsEmpty())
			return MakeError(TEXT("landscape"), Action, 1002,
				TEXT("Each rule must have a non-empty 'layer_name'"));

		double H;
		if ((*RuleObj)->TryGetNumberField(TEXT("height_min"),  H)) Rule.HeightMin  = (float)H;
		if ((*RuleObj)->TryGetNumberField(TEXT("height_max"),  H)) Rule.HeightMax  = (float)H;
		if ((*RuleObj)->TryGetNumberField(TEXT("blend_width"), H)) Rule.BlendWidth = (float)H;

		double Sm, Sx;
		const bool bHasSlopeMin = (*RuleObj)->TryGetNumberField(TEXT("slope_min"), Sm);
		const bool bHasSlopeMax = (*RuleObj)->TryGetNumberField(TEXT("slope_max"), Sx);
		Rule.bHasSlope = bHasSlopeMin || bHasSlopeMax;
		if (bHasSlopeMin) Rule.SlopeMin = (float)Sm;
		if (bHasSlopeMax) Rule.SlopeMax = (float)Sx;

		Rules.Add(Rule);
	}

	if (Rules.Num() == 0)
		return MakeError(TEXT("landscape"), Action, 1001, TEXT("No valid rules parsed from the 'rules' array"));

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message   = FString::Printf(TEXT("auto_paint: GetLandscapeInfo() returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Full landscape bounds in section-space vertex coords
	const FIntPoint SectionBase = Landscape->GetSectionBase();
	const FIntRect  LandBounds  = Landscape->GetBoundingRect() + SectionBase;
	const int32 MinX   = LandBounds.Min.X;
	const int32 MinY   = LandBounds.Min.Y;
	const int32 MaxX   = LandBounds.Max.X;
	const int32 MaxY   = LandBounds.Max.Y;
	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// Read full heightmap
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(Width * Height);
	{
		FHeightmapAccessor<false> Accessor(Info);
		int32 RX1 = MinX, RY1 = MinY, RX2 = MaxX, RY2 = MaxY;
		Accessor.GetData(RX1, RY1, RX2, RY2, HeightData.GetData());
	}

	const FVector LandLoc   = Landscape->GetActorLocation();
	const FVector LandScale = Landscape->GetActorScale3D();
	const float   ScaleZ    = FMath::Max(LandScale.Z,  0.001f);
	const float   ScaleXY   = FMath::Max(FMath::Max(LandScale.X, LandScale.Y), 0.001f);

	// Find layer info objects and allocate per-rule alpha arrays
	TArray<ULandscapeLayerInfoObject*> LayerInfos;
	TArray<TArray<uint8>>              AlphaArrays;
	TArray<bool>                       bLayerUsed;

	for (const FPaintRule& Rule : Rules)
	{
		ULandscapeLayerInfoObject* LayerInfo = nullptr;
		for (const FLandscapeInfoLayerSettings& LS : Info->Layers)
		{
			if (LS.GetLayerName() == FName(*Rule.LayerName))
			{
				LayerInfo = LS.LayerInfoObj;
				break;
			}
		}
		LayerInfos.Add(LayerInfo);
		AlphaArrays.AddDefaulted();
		AlphaArrays.Last().Init(0, Width * Height);
		bLayerUsed.Add(false);
	}

	// Evaluate each vertex - first-matching-rule wins
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const int32  Idx    = (Y - MinY) * Width + (X - MinX);
			const float  WorldZ = ((float)HeightData[Idx] - 32768.f) / 128.f * ScaleZ + LandLoc.Z;

			// Slope computed lazily (only when a rule needs it)
			float SlopeDeg       = 0.f;
			bool  bSlopeComputed = false;

			for (int32 RuleIdx = 0; RuleIdx < Rules.Num(); ++RuleIdx)
			{
				const FPaintRule& Rule = Rules[RuleIdx];

				if (WorldZ < Rule.HeightMin || WorldZ > Rule.HeightMax)
					continue;

				if (Rule.bHasSlope)
				{
					if (!bSlopeComputed)
					{
						bSlopeComputed = true;
						const int32 LX  = X - MinX;
						const int32 LY  = Y - MinY;
						const int32 LX0 = FMath::Max(LX - 1, 0);
						const int32 LX1 = FMath::Min(LX + 1, Width  - 1);
						const int32 LY0 = FMath::Max(LY - 1, 0);
						const int32 LY1 = FMath::Min(LY + 1, Height - 1);
						// Height differences in world-space units per vertex
						const float DX = ((float)HeightData[LY  * Width + LX1] - (float)HeightData[LY  * Width + LX0])
						               * ScaleZ / (128.f * (float)(LX1 - LX0) * ScaleXY);
						const float DY = ((float)HeightData[LY1 * Width + LX]  - (float)HeightData[LY0 * Width + LX])
						               * ScaleZ / (128.f * (float)(LY1 - LY0) * ScaleXY);
						SlopeDeg = FMath::RadiansToDegrees(FMath::Atan2(FMath::Sqrt(DX * DX + DY * DY), 1.0f));
					}
					if (SlopeDeg < Rule.SlopeMin || SlopeDeg > Rule.SlopeMax)
						continue;
				}

				// Compute blend weight at height band edges
				float Weight = 1.0f;
				if (Rule.BlendWidth > 0.f)
				{
					if (WorldZ < Rule.HeightMin + Rule.BlendWidth)
						Weight *= FMath::Clamp((WorldZ - Rule.HeightMin) / Rule.BlendWidth, 0.f, 1.f);
					if (WorldZ > Rule.HeightMax - Rule.BlendWidth)
						Weight *= FMath::Clamp((Rule.HeightMax - WorldZ) / Rule.BlendWidth, 0.f, 1.f);
				}

				AlphaArrays[RuleIdx][Idx] = (uint8)FMath::Clamp(FMath::RoundToInt(Weight * 255.f), 0, 255);
				bLayerUsed[RuleIdx] = true;
				break;  // first-match-wins
			}
		}
	}

	// Write each used layer
	int32 LayersPainted = 0;
	FLandscapeEditDataInterface EditData(Info);
	for (int32 RuleIdx = 0; RuleIdx < Rules.Num(); ++RuleIdx)
	{
		if (!bLayerUsed[RuleIdx]) continue;
		if (!LayerInfos[RuleIdx])
		{
			UE_LOG(LogTemp, Warning,
				TEXT("auto_paint: layer '%s' not found on landscape '%s', skipping"),
				*Rules[RuleIdx].LayerName, *LandscapeName);
			continue;
		}
		EditData.SetAlphaData(LayerInfos[RuleIdx], MinX, MinY, MaxX, MaxY,
			AlphaArrays[RuleIdx].GetData(), /*Stride=*/0);
		++LayersPainted;
	}

	Landscape->MarkPackageDirty();
	if (GEditor) GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("auto_paint on '%s': %d rules evaluated, %d layer(s) painted across %dx%d vertices"),
		*LandscapeName, Rules.Num(), LayersPainted, Width, Height);
	return Result;
}

// ---------------------------------------------------------------------------
// set_hole_mask
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_SetHoleMask(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("set_hole_mask"));

	FString LandscapeName;
	double  RgnX = 0.0, RgnY = 0.0, RgnW = 0.0, RgnH = 0.0;
	bool    bHole = true;

	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		Result.Message  = TEXT("set_hole_mask: 'landscape_name' is required");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetNumberField(TEXT("x"),      RgnX) ||
	    !Params->TryGetNumberField(TEXT("y"),      RgnY) ||
	    !Params->TryGetNumberField(TEXT("width"),  RgnW) ||
	    !Params->TryGetNumberField(TEXT("height"), RgnH))
	{
		Result.Message  = TEXT("set_hole_mask: 'x', 'y', 'width', and 'height' are required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetBoolField(TEXT("hole"), bHole);

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message  = FString::Printf(TEXT("set_hole_mask: GetLandscapeInfo returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	const int32 MinX = (int32)RgnX;
	const int32 MinY = (int32)RgnY;
	const int32 MaxX = MinX + (int32)RgnW - 1;
	const int32 MaxY = MinY + (int32)RgnH - 1;
	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// Visibility layer value: 0 = hole, 255 = solid
	const uint8 FillValue = bHole ? 0 : 255;
	TArray<uint8> VisData;
	VisData.Init(FillValue, Width * Height);

	ULandscapeLayerInfoObject* VisLayerInfo = ALandscapeProxy::VisibilityLayer;
	if (!VisLayerInfo)
	{
		Result.Message  = TEXT("set_hole_mask: ALandscapeProxy::VisibilityLayer is null — ensure LandscapeHole material is set up");
		Result.ErrorCode = 3001;
		return Result;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Landscape Set Hole Mask")));
	Landscape->Modify();

	FLandscapeEditDataInterface EditData(Info);
	EditData.SetAlphaData(VisLayerInfo, MinX, MinY, MaxX, MaxY, VisData.GetData(), /*Stride=*/0);

	Landscape->MarkPackageDirty();
	if (GEditor) GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("set_hole_mask on '%s': %s at region (%d,%d)–(%d,%d) (%dx%d vertices)"),
		*LandscapeName, bHole ? TEXT("punched hole") : TEXT("filled hole"),
		MinX, MinY, MaxX, MaxY, Width, Height);
	return Result;
}

// ---------------------------------------------------------------------------
// import_weightmap
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_ImportWeightmap(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("import_weightmap"));

	FString LandscapeName, LayerName, FilePath;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("layer_name"),     LayerName)     || LayerName.IsEmpty()     ||
	    !Params->TryGetStringField(TEXT("file_path"),      FilePath)      || FilePath.IsEmpty())
	{
		Result.Message  = TEXT("import_weightmap: 'landscape_name', 'layer_name', and 'file_path' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message  = FString::Printf(TEXT("import_weightmap: GetLandscapeInfo returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Find layer info object by name
	// ForAllLandscapeProxies + EditorLayerSettings removed in UE 5.7.
	// Use ULandscapeInfo::Layers to find the layer info object by name.
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	for (const FLandscapeInfoLayerSettings& LayerSettings : Info->Layers)
	{
		if (LayerSettings.LayerInfoObj && LayerSettings.LayerInfoObj->GetLayerName() == FName(*LayerName))
		{
			LayerInfo = LayerSettings.LayerInfoObj;
			break;
		}
	}

	if (!LayerInfo)
	{
		Result.Message  = FString::Printf(TEXT("import_weightmap: layer '%s' not found on landscape '%s'"), *LayerName, *LandscapeName);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Read raw bytes from file
	TArray<uint8> RawBytes;
	if (!FFileHelper::LoadFileToArray(RawBytes, *FilePath))
	{
		Result.Message  = FString::Printf(TEXT("import_weightmap: failed to read file '%s'"), *FilePath);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Determine landscape vertex bounds
	int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
	Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// For PNG files, decode through IImageWrapper; for raw, use bytes directly
	TArray<uint8> AlphaData;
	const FString Ext = FPaths::GetExtension(FilePath).ToLower();
	if (Ext == TEXT("png"))
	{
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper  = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(RawBytes.GetData(), RawBytes.Num()))
		{
			Result.Message  = FString::Printf(TEXT("import_weightmap: failed to decode PNG '%s'"), *FilePath);
			Result.ErrorCode = 3000;
			return Result;
		}
		TArray<uint8> Decoded;
		if (!ImageWrapper->GetRaw(ERGBFormat::Gray, 8, Decoded))
		{
			Result.Message  = TEXT("import_weightmap: PNG must be 8-bit grayscale");
			Result.ErrorCode = 1001;
			return Result;
		}
		AlphaData = MoveTemp(Decoded);
	}
	else
	{
		// Assume raw 8-bit alpha
		AlphaData = MoveTemp(RawBytes);
	}

	if (AlphaData.Num() != Width * Height)
	{
		Result.Message = FString::Printf(
			TEXT("import_weightmap: file pixel count (%d) does not match landscape vertex count (%dx%d=%d)"),
			AlphaData.Num(), Width, Height, Width * Height);
		Result.ErrorCode = 1001;
		return Result;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Landscape Import Weightmap")));
	Landscape->Modify();

	FLandscapeEditDataInterface EditData(Info);
	EditData.SetAlphaData(LayerInfo, MinX, MinY, MaxX, MaxY, AlphaData.GetData(), /*Stride=*/0);

	Landscape->MarkPackageDirty();
	if (GEditor) GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = LandscapeName;
	Result.Message      = FString::Printf(
		TEXT("import_weightmap: wrote layer '%s' on '%s' from '%s' (%dx%d vertices)"),
		*LayerName, *LandscapeName, *FilePath, Width, Height);
	return Result;
}

// ---------------------------------------------------------------------------
// export_heightmap
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_ExportHeightmap(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("landscape"), TEXT("export_heightmap"));

	FString LandscapeName, FilePath;
	Params->TryGetStringField(TEXT("landscape_name"), LandscapeName);
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		Result.Message  = TEXT("export_heightmap: 'file_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	ALandscape* Landscape = FindLandscape(LandscapeName, Result);
	if (!Landscape) return Result;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		Result.Message  = FString::Printf(TEXT("export_heightmap: GetLandscapeInfo returned null for '%s'"), *LandscapeName);
		Result.ErrorCode = 3000;
		return Result;
	}

	// Get landscape vertex bounds
	int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
	Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
	const int32 Width  = MaxX - MinX + 1;
	const int32 Height = MaxY - MinY + 1;

	// Read raw height data (uint16 per vertex)
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(Width * Height);
	{
		FLandscapeEditDataInterface EditData(Info);
		EditData.GetHeightData(MinX, MinY, MaxX, MaxY, HeightData.GetData(), /*Stride=*/0);
	}

	// Encode as 16-bit grayscale PNG
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper  = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid())
	{
		Result.Message  = TEXT("export_heightmap: failed to create PNG image wrapper");
		Result.ErrorCode = 3000;
		return Result;
	}

	// Reinterpret uint16 array as byte array for the image wrapper (big-endian 16-bit gray)
	const int32 ByteCount = Width * Height * sizeof(uint16);
	ImageWrapper->SetRaw(HeightData.GetData(), ByteCount, Width, Height, ERGBFormat::Gray, 16);
	const TArray64<uint8>& Compressed = ImageWrapper->GetCompressed();

	if (!FFileHelper::SaveArrayToFile(Compressed, *FilePath))
	{
		Result.Message  = FString::Printf(TEXT("export_heightmap: failed to write file '%s'"), *FilePath);
		Result.ErrorCode = 3000;
		return Result;
	}

	Result.bSuccess     = true;
	Result.AffectedPath = FilePath;
	Result.Message      = FString::Printf(
		TEXT("export_heightmap: wrote '%s' (%dx%d, 16-bit grayscale PNG) from '%s'"),
		*FilePath, Width, Height, *LandscapeName);
	return Result;
}

// ---------------------------------------------------------------------------
// set_wpo_disable_distance  (Phase 1d)
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_SetWPODisableDistance(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_wpo_disable_distance");

	FString ActorLabel;
	double Distance = 0.0;

	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'actor_label'. Pass the landscape actor label or \"first\"."));
	if (!Params->TryGetNumberField(TEXT("distance"), Distance))
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'distance' (float, world units)."));

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(ActorLabel, Result);
	if (!Landscape) return Result;

	// UE 5.7: ALandscape::WPODisableDistance was removed. This field is no longer
	// a member of ALandscape. No direct C++ replacement is available on the actor;
	// WPO disable distance is now driven by material-side parameters and console
	// variables (e.g. r.Nanite.WPODistanceDisable, r.Nanite.ProgrammableRaster).
	(void)Distance;
	Result.bSuccess  = false;
	Result.ErrorCode = 3003;
	Result.Message   = FString::Printf(
		TEXT("set_wpo_disable_distance: NOT_SUPPORTED in UE 5.7 -- "
		     "ALandscape::WPODisableDistance was removed. Use material-side WPO "
		     "parameters or CVars (e.g. r.Nanite.WPODistanceDisable) instead. "
		     "Actor: '%s'"), *ActorLabel);
	return Result;
}

// ---------------------------------------------------------------------------
// build_hlods  (Phase 1d -- PARTIALLY_FEASIBLE)
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_BuildHLODs(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("build_hlods");

	// No in-process C++ API to invoke HLOD building in UE 5.7 at runtime.
	// HLODs must be built via the WorldPartitionHLODsBuilder commandlet.

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("commandlet"),  TEXT("WorldPartitionBuilderCommandlet"));
	Data->SetStringField(TEXT("builder"),     TEXT("WorldPartitionHLODsBuilder"));
	Data->SetStringField(TEXT("example_cmd"),
		TEXT("UnrealEditor.exe {ProjectPath} {MapPath} -run=WorldPartitionBuilderCommandlet"
		     " -SCCProvider=None -AllowCommandletRendering -Builder=WorldPartitionHLODsBuilder"));
	Data->SetStringField(TEXT("note"),
		TEXT("HLOD generation requires a command-line cook pass; it cannot be triggered "
		     "from within an active editor session via C++ API. Use the job descriptor above "
		     "to launch the commandlet as a separate process."));

	FBridgeResult R = CreateResult(TEXT("landscape"), Action);
	R.bSuccess  = false;
	R.ErrorCode = 3003;
	R.Message   = TEXT("build_hlods: PARTIALLY_FEASIBLE -- returns commandlet job descriptor. "
	                   "WorldPartitionHLODsBuilder must be run as a command-line commandlet.");
	R.Data      = Data;
	return R;
}

// ---------------------------------------------------------------------------
// import_heightmap  (Phase 1d -- PARTIALLY_FEASIBLE)
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_ImportHeightmap(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("import_heightmap");

	FString ActorLabel, FilePath;
	Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
	Params->TryGetStringField(TEXT("file_path"),   FilePath);
	if (ActorLabel.IsEmpty()) ActorLabel = TEXT("first");

	// UE 5.7 does not expose a public C++ API to re-import the heightmap of an
	// existing ALandscape at runtime. Recommend Python via execute_python, or
	// use create_landscape with heightmap_path to rebuild from scratch.

	const FString PythonHint = FString::Printf(
		TEXT("import unreal\n"
		     "landscape = None\n"
		     "for actor in unreal.EditorLevelLibrary.get_all_level_actors():\n"
		     "    if actor.get_class().get_name() == 'Landscape':\n"
		     "        if '%s' == 'first' or actor.get_actor_label() == '%s':\n"
		     "            landscape = actor\n"
		     "            break\n"
		     "if landscape:\n"
		     "    # unreal.LandscapeEditorUtilities.import_heightmap(landscape, '%s')\n"
		     "    print('Found:', landscape.get_actor_label())\n"
		     "else:\n"
		     "    print('Landscape not found')"),
		*ActorLabel, *ActorLabel, *FilePath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"),  ActorLabel);
	Data->SetStringField(TEXT("file_path"),    FilePath);
	Data->SetStringField(TEXT("python_hint"),  PythonHint);
	Data->SetStringField(TEXT("note"),
		TEXT("UE 5.7 has no public C++ API for in-place heightmap re-import on an existing "
		     "ALandscape. Use the Python script hint above via execute_python, or recreate "
		     "the landscape with create_landscape + heightmap_path."));

	FBridgeResult R = CreateResult(TEXT("landscape"), Action);
	R.bSuccess  = false;
	R.ErrorCode = 3003;
	R.Message   = TEXT("import_heightmap: PARTIALLY_FEASIBLE -- no public C++ API in UE 5.7. "
	                   "See python_hint in data for a Python script workaround.");
	R.Data      = Data;
	return R;
}

// ---------------------------------------------------------------------------
// get_terrain_data  (Phase 1d)
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_GetTerrainData(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_terrain_data");

	FString ActorLabel;
	Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (ActorLabel.IsEmpty()) ActorLabel = TEXT("first");

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(ActorLabel, Result);
	if (!Landscape) return Result;

	UWorld* World = Landscape->GetWorld();
	const bool bHasWorldPartition = (World && World->GetWorldPartition() != nullptr);

	const FVector Loc = Landscape->GetActorLocation();
	TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);

	const int32 SectionsPC  = Landscape->NumSubsections;
	const int32 QuadsPerSec = Landscape->SubsectionSizeQuads;
	const int32 CompCount   = Landscape->LandscapeComponents.Num();
	const int32 SqrtComp    = FMath::RoundToInt(FMath::Sqrt((float)CompCount));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"),            Landscape->GetActorLabel());
	Data->SetObjectField(TEXT("location"),               LocObj);
	Data->SetNumberField(TEXT("section_size"),           QuadsPerSec);
	Data->SetNumberField(TEXT("sections_per_component"), SectionsPC);
	Data->SetNumberField(TEXT("component_count"),        CompCount);
	Data->SetBoolField  (TEXT("has_world_partition"),    bHasWorldPartition);
	Data->SetNumberField(TEXT("vertex_grid_approx_x"),   QuadsPerSec * SectionsPC * SqrtComp + 1);
	Data->SetNumberField(TEXT("vertex_grid_approx_y"),   QuadsPerSec * SectionsPC * SqrtComp + 1);
	Data->SetStringField(TEXT("material"),
		Landscape->LandscapeMaterial
			? Landscape->LandscapeMaterial->GetPathName()
			: TEXT("none"));

	Result.bSuccess     = true;
	Result.AffectedPath = ActorLabel;
	Result.Data         = Data;
	Result.Message      = FString::Printf(
		TEXT("get_terrain_data: '%s' -- %d component(s), %d sections/comp, %d quads/section, wp=%s"),
		*Landscape->GetActorLabel(), CompCount, SectionsPC, QuadsPerSec,
		bHasWorldPartition ? TEXT("true") : TEXT("false"));
	return Result;
}

// ---------------------------------------------------------------------------
// set_layer_info  (Phase 1d)
// ---------------------------------------------------------------------------

FBridgeResult ULandscapeHandler::Action_SetLayerInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("set_layer_info");

	FString ActorLabel, LayerName, InfoAssetPath;
	if (!Params->TryGetStringField(TEXT("actor_label"),     ActorLabel)    || ActorLabel.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'actor_label'"));
	if (!Params->TryGetStringField(TEXT("layer_name"),      LayerName)     || LayerName.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'layer_name'"));
	if (!Params->TryGetStringField(TEXT("info_asset_path"), InfoAssetPath) || InfoAssetPath.IsEmpty())
		return MakeError(TEXT("landscape"), Action, 1000,
			TEXT("Missing required param: 'info_asset_path'"));

	FBridgeResult Result = CreateResult(TEXT("landscape"), Action);
	ALandscape* Landscape = FindLandscape(ActorLabel, Result);
	if (!Landscape) return Result;

	// Load the ULandscapeLayerInfoObject with suffix fallback
	ULandscapeLayerInfoObject* LayerInfoObj = LoadObject<ULandscapeLayerInfoObject>(nullptr, *InfoAssetPath);
	if (!LayerInfoObj)
	{
		const FString Suffix = InfoAssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(InfoAssetPath);
		LayerInfoObj = LoadObject<ULandscapeLayerInfoObject>(nullptr, *Suffix);
	}
	if (!LayerInfoObj)
		return MakeError(TEXT("landscape"), Action, 2000,
			FString::Printf(TEXT("Could not load ULandscapeLayerInfoObject at '%s'"), *InfoAssetPath),
			TEXT("Verify the asset path exists in the Content Browser."));

	// UE 5.7: ALandscape::EditorLayerSettings and FLandscapeEditorLayerSettings
	// were removed. Use ULandscapeInfo::Layers (TArray<FLandscapeInfoLayerSettings>)
	// to associate a ULandscapeLayerInfoObject with a named layer.
	// Note: assigning layer info here makes it available for painting but does not
	// paint any weights. Use paint_layer or import_weightmap to paint.
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
		return MakeError(TEXT("landscape"), Action, 3000,
			FString::Printf(TEXT("set_layer_info: GetLandscapeInfo returned null for '%s'"), *ActorLabel));

	bool bFound = false;
	const FName TargetName(*LayerName);
	for (FLandscapeInfoLayerSettings& LayerSettings : Info->Layers)
	{
		if (LayerSettings.GetLayerName() == TargetName)
		{
			LayerSettings.LayerInfoObj = LayerInfoObj;
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		// Layer not registered yet -- add a new entry on ULandscapeInfo::Layers.
		// FLandscapeInfoLayerSettings construction in UE 5.7 takes a
		// ULandscapeLayerInfoObject* plus an owning proxy.
		FLandscapeInfoLayerSettings NewEntry(LayerInfoObj, Landscape);
		Info->Layers.Add(NewEntry);
	}

	Landscape->MarkPackageDirty();
	if (GEditor)
		GEditor->RedrawLevelEditingViewports();

	Result.bSuccess     = true;
	Result.AffectedPath = ActorLabel;
	Result.Message      = FString::Printf(
		TEXT("set_layer_info: assigned '%s' to layer '%s' on '%s'. "
		     "Note: this does not paint weights -- use paint_layer or import_weightmap."),
		*InfoAssetPath, *LayerName, *ActorLabel);
	return Result;
}
