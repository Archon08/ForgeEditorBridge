#include "Handlers/DebugHandler.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorValidatorSubsystem.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#endif

#include "HAL/PlatformMemory.h"
#include "UObject/UObjectArray.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("debug");

// ---------------------------------------------------------------------------
// GetEditorWorld
// ---------------------------------------------------------------------------

UWorld* UDebugHandler::GetEditorWorld() const
{
#if WITH_EDITOR
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Editor && Ctx.World())
				return Ctx.World();
		}
	}
#endif
	return nullptr;
}

// ---------------------------------------------------------------------------
// ParseColor
// ---------------------------------------------------------------------------

FColor UDebugHandler::ParseColor(const FString& ColorStr, const FColor& Default)
{
	if (ColorStr.IsEmpty()) return Default;

	// Named colors
	if (ColorStr.Equals(TEXT("Red"),     ESearchCase::IgnoreCase)) return FColor::Red;
	if (ColorStr.Equals(TEXT("Green"),   ESearchCase::IgnoreCase)) return FColor::Green;
	if (ColorStr.Equals(TEXT("Blue"),    ESearchCase::IgnoreCase)) return FColor::Blue;
	if (ColorStr.Equals(TEXT("White"),   ESearchCase::IgnoreCase)) return FColor::White;
	if (ColorStr.Equals(TEXT("Black"),   ESearchCase::IgnoreCase)) return FColor::Black;
	if (ColorStr.Equals(TEXT("Yellow"),  ESearchCase::IgnoreCase)) return FColor::Yellow;
	if (ColorStr.Equals(TEXT("Cyan"),    ESearchCase::IgnoreCase)) return FColor::Cyan;
	if (ColorStr.Equals(TEXT("Magenta"), ESearchCase::IgnoreCase)) return FColor::Magenta;
	if (ColorStr.Equals(TEXT("Orange"),  ESearchCase::IgnoreCase)) return FColor::Orange;
	if (ColorStr.Equals(TEXT("Purple"),  ESearchCase::IgnoreCase)) return FColor::Purple;

	// Hex: #RRGGBB or #RRGGBBAA
	FString Hex = ColorStr;
	if (Hex.StartsWith(TEXT("#"))) Hex = Hex.Mid(1);
	if (Hex.Len() == 6) Hex += TEXT("FF");

	if (Hex.Len() == 8)
	{
		const uint8 R = (FParse::HexDigit(Hex[0]) << 4) | FParse::HexDigit(Hex[1]);
		const uint8 G = (FParse::HexDigit(Hex[2]) << 4) | FParse::HexDigit(Hex[3]);
		const uint8 B = (FParse::HexDigit(Hex[4]) << 4) | FParse::HexDigit(Hex[5]);
		const uint8 A = (FParse::HexDigit(Hex[6]) << 4) | FParse::HexDigit(Hex[7]);
		return FColor(R, G, B, A);
	}

	return Default;
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("draw_line"))   return Action_DrawLine(Params);
	if (Action == TEXT("draw_sphere")) return Action_DrawSphere(Params);
	if (Action == TEXT("draw_box"))    return Action_DrawBox(Params);
	if (Action == TEXT("draw_text"))   return Action_DrawText(Params);
	if (Action == TEXT("draw_arrow"))  return Action_DrawArrow(Params);
	if (Action == TEXT("clear"))       return Action_Clear(Params);
	if (Action == TEXT("get_blueprint_errors")) return Action_GetBlueprintErrors(Params);
	if (Action == TEXT("run_asset_validation")) return Action_RunAssetValidation(Params);
	if (Action == TEXT("get_memory_report"))    return Action_GetMemoryReport(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown debug action '%s'. Valid: draw_line, draw_sphere, draw_box, draw_text, draw_arrow, clear, get_blueprint_errors, run_asset_validation, get_memory_report"), *Action));
}

// ---------------------------------------------------------------------------
// draw_line
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_DrawLine(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("draw_line"), 3003, TEXT("No editor world available"));

	double Sx = 0, Sy = 0, Sz = 0, Ex = 0, Ey = 0, Ez = 0;
	if (const TSharedPtr<FJsonObject>* S = nullptr; Params->TryGetObjectField(TEXT("start"), S))
	{
		(*S)->TryGetNumberField(TEXT("x"), Sx);
		(*S)->TryGetNumberField(TEXT("y"), Sy);
		(*S)->TryGetNumberField(TEXT("z"), Sz);
	}
	if (const TSharedPtr<FJsonObject>* E = nullptr; Params->TryGetObjectField(TEXT("end"), E))
	{
		(*E)->TryGetNumberField(TEXT("x"), Ex);
		(*E)->TryGetNumberField(TEXT("y"), Ey);
		(*E)->TryGetNumberField(TEXT("z"), Ez);
	}

	FString ColorStr = TEXT("White");
	Params->TryGetStringField(TEXT("color"), ColorStr);
	double Duration = 0.0, Thickness = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);
	Params->TryGetNumberField(TEXT("thickness"), Thickness);

	DrawDebugLine(World,
		FVector((float)Sx, (float)Sy, (float)Sz),
		FVector((float)Ex, (float)Ey, (float)Ez),
		ParseColor(ColorStr),
		/*bPersistentLines=*/(Duration <= 0.0),
		/*LifeTime=*/(float)(Duration > 0.0 ? Duration : -1.0f),
		/*DepthPriority=*/0,
		/*Thickness=*/(float)Thickness);

	return MakeSuccess(DOMAIN, TEXT("draw_line"),
		FString::Printf(TEXT("Line drawn: (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f)"),
			Sx, Sy, Sz, Ex, Ey, Ez));
}

// ---------------------------------------------------------------------------
// draw_sphere
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_DrawSphere(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("draw_sphere"), 3003, TEXT("No editor world available"));

	double Cx = 0, Cy = 0, Cz = 0;
	if (const TSharedPtr<FJsonObject>* C = nullptr; Params->TryGetObjectField(TEXT("center"), C))
	{
		(*C)->TryGetNumberField(TEXT("x"), Cx);
		(*C)->TryGetNumberField(TEXT("y"), Cy);
		(*C)->TryGetNumberField(TEXT("z"), Cz);
	}

	double Radius = 50.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);
	FString ColorStr = TEXT("White");
	Params->TryGetStringField(TEXT("color"), ColorStr);
	double Duration = 0.0, Thickness = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);
	Params->TryGetNumberField(TEXT("thickness"), Thickness);

	DrawDebugSphere(World,
		FVector((float)Cx, (float)Cy, (float)Cz),
		(float)Radius,
		/*Segments=*/16,
		ParseColor(ColorStr),
		(Duration <= 0.0),
		(float)(Duration > 0.0 ? Duration : -1.0f),
		0,
		(float)Thickness);

	return MakeSuccess(DOMAIN, TEXT("draw_sphere"),
		FString::Printf(TEXT("Sphere drawn at (%.0f,%.0f,%.0f) r=%.0f"), Cx, Cy, Cz, Radius));
}

// ---------------------------------------------------------------------------
// draw_box
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_DrawBox(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("draw_box"), 3003, TEXT("No editor world available"));

	double Cx = 0, Cy = 0, Cz = 0;
	double Ex = 50, Ey = 50, Ez = 50;
	double Pitch = 0, Yaw = 0, Roll = 0;

	if (const TSharedPtr<FJsonObject>* C = nullptr; Params->TryGetObjectField(TEXT("center"), C))
	{
		(*C)->TryGetNumberField(TEXT("x"), Cx);
		(*C)->TryGetNumberField(TEXT("y"), Cy);
		(*C)->TryGetNumberField(TEXT("z"), Cz);
	}
	if (const TSharedPtr<FJsonObject>* E = nullptr; Params->TryGetObjectField(TEXT("extent"), E))
	{
		(*E)->TryGetNumberField(TEXT("x"), Ex);
		(*E)->TryGetNumberField(TEXT("y"), Ey);
		(*E)->TryGetNumberField(TEXT("z"), Ez);
	}
	if (const TSharedPtr<FJsonObject>* Rot = nullptr; Params->TryGetObjectField(TEXT("rotation"), Rot))
	{
		(*Rot)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*Rot)->TryGetNumberField(TEXT("yaw"),   Yaw);
		(*Rot)->TryGetNumberField(TEXT("roll"),  Roll);
	}

	FString ColorStr = TEXT("White");
	Params->TryGetStringField(TEXT("color"), ColorStr);
	double Duration = 0.0, Thickness = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);
	Params->TryGetNumberField(TEXT("thickness"), Thickness);

	DrawDebugBox(World,
		FVector((float)Cx, (float)Cy, (float)Cz),
		FVector((float)Ex, (float)Ey, (float)Ez),
		FQuat(FRotator((float)Pitch, (float)Yaw, (float)Roll)),
		ParseColor(ColorStr),
		(Duration <= 0.0),
		(float)(Duration > 0.0 ? Duration : -1.0f),
		0,
		(float)Thickness);

	return MakeSuccess(DOMAIN, TEXT("draw_box"),
		FString::Printf(TEXT("Box drawn at (%.0f,%.0f,%.0f) extent (%.0f,%.0f,%.0f)"),
			Cx, Cy, Cz, Ex, Ey, Ez));
}

// ---------------------------------------------------------------------------
// draw_text
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_DrawText(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("draw_text"), 3003, TEXT("No editor world available"));

	FString Text;
	if (!Params->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
		return MakeError(DOMAIN, TEXT("draw_text"), 1000, TEXT("draw_text: 'text' is required"));

	double Lx = 0, Ly = 0, Lz = 0;
	if (const TSharedPtr<FJsonObject>* L = nullptr; Params->TryGetObjectField(TEXT("location"), L))
	{
		(*L)->TryGetNumberField(TEXT("x"), Lx);
		(*L)->TryGetNumberField(TEXT("y"), Ly);
		(*L)->TryGetNumberField(TEXT("z"), Lz);
	}

	FString ColorStr = TEXT("White");
	Params->TryGetStringField(TEXT("color"), ColorStr);
	double Duration = -1.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);

	// DrawDebugString: Duration <= 0 treated as persistent (use -1.0f for persistent)
	DrawDebugString(World,
		FVector((float)Lx, (float)Ly, (float)Lz),
		Text,
		/*TestBaseActor=*/nullptr,
		ParseColor(ColorStr),
		/*Duration=*/(float)(Duration <= 0.0 ? -1.0 : Duration));

	return MakeSuccess(DOMAIN, TEXT("draw_text"),
		FString::Printf(TEXT("Text '%s' drawn at (%.0f,%.0f,%.0f)"), *Text, Lx, Ly, Lz));
}

// ---------------------------------------------------------------------------
// draw_arrow
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_DrawArrow(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("draw_arrow"), 3003, TEXT("No editor world available"));

	double Sx = 0, Sy = 0, Sz = 0, Ex = 0, Ey = 0, Ez = 0;
	if (const TSharedPtr<FJsonObject>* S = nullptr; Params->TryGetObjectField(TEXT("start"), S))
	{
		(*S)->TryGetNumberField(TEXT("x"), Sx);
		(*S)->TryGetNumberField(TEXT("y"), Sy);
		(*S)->TryGetNumberField(TEXT("z"), Sz);
	}
	if (const TSharedPtr<FJsonObject>* E = nullptr; Params->TryGetObjectField(TEXT("end"), E))
	{
		(*E)->TryGetNumberField(TEXT("x"), Ex);
		(*E)->TryGetNumberField(TEXT("y"), Ey);
		(*E)->TryGetNumberField(TEXT("z"), Ez);
	}

	double ArrowSize = 40.0;
	Params->TryGetNumberField(TEXT("arrow_size"), ArrowSize);
	FString ColorStr = TEXT("Yellow");
	Params->TryGetStringField(TEXT("color"), ColorStr);
	double Duration = 0.0, Thickness = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);
	Params->TryGetNumberField(TEXT("thickness"), Thickness);

	DrawDebugDirectionalArrow(World,
		FVector((float)Sx, (float)Sy, (float)Sz),
		FVector((float)Ex, (float)Ey, (float)Ez),
		(float)ArrowSize,
		ParseColor(ColorStr),
		(Duration <= 0.0),
		(float)(Duration > 0.0 ? Duration : -1.0f),
		0,
		(float)Thickness);

	return MakeSuccess(DOMAIN, TEXT("draw_arrow"),
		FString::Printf(TEXT("Arrow drawn: (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f)"),
			Sx, Sy, Sz, Ex, Ey, Ez));
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_Clear(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("clear"), 3003, TEXT("No editor world available"));

	FlushPersistentDebugLines(World);
	FlushDebugStrings(World);

	return MakeSuccess(DOMAIN, TEXT("clear"), TEXT("All persistent debug shapes cleared"));
}

// ---------------------------------------------------------------------------
// get_blueprint_errors
// ---------------------------------------------------------------------------

#if WITH_EDITOR
static FString BlueprintStatusToString(EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Unknown:                 return TEXT("BS_Unknown");
	case BS_Dirty:                   return TEXT("BS_Dirty");
	case BS_Error:                   return TEXT("BS_Error");
	case BS_UpToDate:                return TEXT("BS_UpToDate");
	case BS_BeingCreated:            return TEXT("BS_BeingCreated");
	case BS_UpToDateWithWarnings:    return TEXT("BS_UpToDateWithWarnings");
	default:                         return TEXT("BS_Unknown");
	}
}
#endif

FBridgeResult UDebugHandler::Action_GetBlueprintErrors(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(DOMAIN, TEXT("get_blueprint_errors"), 1000, TEXT("'asset_path' is required"));
	}

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!BP)
	{
		return MakeError(DOMAIN, TEXT("get_blueprint_errors"), 2000,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const EBlueprintStatus Status = BP->Status;
	const bool bHasErrors = (Status == BS_Error);
	const bool bHasWarnings = (Status == BS_UpToDateWithWarnings);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("status"), BlueprintStatusToString(Status));
	Data->SetBoolField(TEXT("has_errors"), bHasErrors);
	Data->SetBoolField(TEXT("has_warnings"), bHasWarnings);
	Data->SetStringField(TEXT("note"), TEXT("For full error log, use cpp/get_compile_errors or inspect the Message Log 'BlueprintLog'"));

	return MakeSuccess(DOMAIN, TEXT("get_blueprint_errors"),
		FString::Printf(TEXT("Blueprint %s status: %s"), *AssetPath, *BlueprintStatusToString(Status)),
		Data);
#else
	return MakeError(DOMAIN, TEXT("get_blueprint_errors"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// run_asset_validation
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_RunAssetValidation(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
	if (!GEditor)
	{
		return MakeError(DOMAIN, TEXT("run_asset_validation"), 3003, TEXT("GEditor not available"));
	}

	UEditorValidatorSubsystem* VS = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	if (!VS)
	{
		return MakeError(DOMAIN, TEXT("run_asset_validation"), 3003, TEXT("EditorValidatorSubsystem not available"));
	}

	// Collect asset data from provided paths (if any)
	TArray<FAssetData> AssetsToValidate;
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("asset_paths"), PathsArr) && PathsArr)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		for (const TSharedPtr<FJsonValue>& PV : *PathsArr)
		{
			if (!PV.IsValid()) continue;
			const FString Path = PV->AsString();
			if (Path.IsEmpty()) continue;

			FAssetData AD = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Path));
			if (AD.IsValid())
			{
				AssetsToValidate.Add(AD);
			}
		}
	}

	FValidateAssetsSettings Settings;
	Settings.bSkipExcludedDirectories = true;
	Settings.bShowIfNoFailures = false;

	FValidateAssetsResults Results;
	VS->ValidateAssetsWithSettings(AssetsToValidate, Settings, Results);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("assets_validated"), Results.NumChecked);
	Data->SetNumberField(TEXT("warnings"), Results.NumWarnings);
	Data->SetNumberField(TEXT("errors"), Results.NumInvalid);
	Data->SetNumberField(TEXT("valid"), Results.NumValid);
	Data->SetNumberField(TEXT("skipped"), Results.NumSkipped);
	Data->SetNumberField(TEXT("requested"), Results.NumRequested);

	return MakeSuccess(DOMAIN, TEXT("run_asset_validation"),
		FString::Printf(TEXT("Validated %d asset(s): %d valid, %d warnings, %d errors"),
			Results.NumChecked, Results.NumValid, Results.NumWarnings, Results.NumInvalid),
		Data);
#else
	return MakeError(DOMAIN, TEXT("run_asset_validation"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// get_memory_report
// ---------------------------------------------------------------------------

FBridgeResult UDebugHandler::Action_GetMemoryReport(TSharedPtr<FJsonObject> Params)
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();

	constexpr double BytesPerMB = 1024.0 * 1024.0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("used_physical_mb"),       static_cast<double>(Stats.UsedPhysical)     / BytesPerMB);
	Data->SetNumberField(TEXT("available_physical_mb"),  static_cast<double>(Stats.AvailablePhysical) / BytesPerMB);
	Data->SetNumberField(TEXT("used_virtual_mb"),        static_cast<double>(Stats.UsedVirtual)       / BytesPerMB);
	Data->SetNumberField(TEXT("total_physical_mb"),      static_cast<double>(Stats.TotalPhysical)     / BytesPerMB);
	Data->SetNumberField(TEXT("peak_used_physical_mb"),  static_cast<double>(Stats.PeakUsedPhysical)  / BytesPerMB);
	Data->SetNumberField(TEXT("uobject_count"),          GUObjectArray.GetObjectArrayNum());

	return MakeSuccess(DOMAIN, TEXT("get_memory_report"),
		FString::Printf(TEXT("Memory: %.0f MB used / %.0f MB total; %d UObjects"),
			static_cast<double>(Stats.UsedPhysical) / BytesPerMB,
			static_cast<double>(Stats.TotalPhysical) / BytesPerMB,
			GUObjectArray.GetObjectArrayNum()),
		Data);
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UDebugHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto MakeParam = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetBoolField(TEXT("required"), bRequired);
		P->SetStringField(TEXT("desc"), Desc);
		return P;
	};

	// draw_line
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("start"),     MakeParam(TEXT("object{x,y,z}"), true,  TEXT("Start point in world space (cm)")));
		Params->SetObjectField(TEXT("end"),        MakeParam(TEXT("object{x,y,z}"), true,  TEXT("End point in world space (cm)")));
		Params->SetObjectField(TEXT("color"),      MakeParam(TEXT("string"),        false, TEXT("Hex #RRGGBB or named: Red,Green,Blue,White,Black,Yellow,Cyan,Magenta,Orange,Purple")));
		Params->SetObjectField(TEXT("duration"),   MakeParam(TEXT("float"),         false, TEXT("Seconds (0 = persistent)")));
		Params->SetObjectField(TEXT("thickness"),  MakeParam(TEXT("float"),         false, TEXT("Line thickness in pixels")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Draw a line in the editor viewport"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("draw_line"), Action);
	}

	// draw_sphere
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("center"),    MakeParam(TEXT("object{x,y,z}"), true,  TEXT("Center in world space (cm)")));
		Params->SetObjectField(TEXT("radius"),    MakeParam(TEXT("float"),         false, TEXT("Radius in cm (default 50)")));
		Params->SetObjectField(TEXT("color"),     MakeParam(TEXT("string"),        false, TEXT("Color string")));
		Params->SetObjectField(TEXT("duration"),  MakeParam(TEXT("float"),         false, TEXT("Seconds (0 = persistent)")));
		Params->SetObjectField(TEXT("thickness"), MakeParam(TEXT("float"),         false, TEXT("Wire thickness")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Draw a wire sphere in the editor viewport"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("draw_sphere"), Action);
	}

	// draw_box
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("center"),   MakeParam(TEXT("object{x,y,z}"),               true,  TEXT("Center in world space (cm)")));
		Params->SetObjectField(TEXT("extent"),   MakeParam(TEXT("object{x,y,z}"),               false, TEXT("Half-extents in cm (default 50 each)")));
		Params->SetObjectField(TEXT("rotation"), MakeParam(TEXT("object{pitch,yaw,roll}"),       false, TEXT("Box rotation in degrees")));
		Params->SetObjectField(TEXT("color"),    MakeParam(TEXT("string"),                       false, TEXT("Color string")));
		Params->SetObjectField(TEXT("duration"), MakeParam(TEXT("float"),                        false, TEXT("Seconds (0 = persistent)")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Draw a wire box in the editor viewport"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("draw_box"), Action);
	}

	// draw_text
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("text"),     MakeParam(TEXT("string"),        true,  TEXT("Text to display")));
		Params->SetObjectField(TEXT("location"), MakeParam(TEXT("object{x,y,z}"), false, TEXT("World position (cm)")));
		Params->SetObjectField(TEXT("color"),    MakeParam(TEXT("string"),        false, TEXT("Color string")));
		Params->SetObjectField(TEXT("duration"), MakeParam(TEXT("float"),         false, TEXT("Seconds (-1 = persistent)")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Draw a text string at a world position"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("draw_text"), Action);
	}

	// draw_arrow
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("start"),      MakeParam(TEXT("object{x,y,z}"), true,  TEXT("Arrow base in world space (cm)")));
		Params->SetObjectField(TEXT("end"),         MakeParam(TEXT("object{x,y,z}"), true,  TEXT("Arrow tip in world space (cm)")));
		Params->SetObjectField(TEXT("arrow_size"),  MakeParam(TEXT("float"),         false, TEXT("Arrowhead size in cm (default 40)")));
		Params->SetObjectField(TEXT("color"),       MakeParam(TEXT("string"),        false, TEXT("Color string (default Yellow)")));
		Params->SetObjectField(TEXT("duration"),    MakeParam(TEXT("float"),         false, TEXT("Seconds (0 = persistent)")));
		Params->SetObjectField(TEXT("thickness"),   MakeParam(TEXT("float"),         false, TEXT("Shaft thickness")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Draw a directional arrow in the editor viewport"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("draw_arrow"), Action);
	}

	// clear
	{
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Flush all persistent debug lines and strings from the viewport"));
		Action->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("clear"), Action);
	}

	// get_blueprint_errors
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true, TEXT("Content path of the Blueprint asset")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Get compile status + error/warning flags for a Blueprint asset"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("get_blueprint_errors"), Action);
	}

	// run_asset_validation
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("asset_paths"), MakeParam(TEXT("array<string>"), false, TEXT("Optional list of asset paths to validate (empty = no assets)")));
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Run the EditorValidatorSubsystem on one or more assets"));
		Action->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("run_asset_validation"), Action);
	}

	// get_memory_report
	{
		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("desc"), TEXT("Platform memory usage + UObject count (no PIE required)"));
		Action->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_memory_report"), Action);
	}

	return Root;
}
