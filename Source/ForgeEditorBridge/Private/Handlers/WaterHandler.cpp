#include "Handlers/WaterHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// ---- Water types -----------------------------------------------------------
#include "WaterBodyActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyComponent.h"
#include "WaterWaves.h"

// ---- World / actors --------------------------------------------------------
#include "EngineUtils.h"
#include "Engine/World.h"

// ---- Transactions ----------------------------------------------------------
#include "ScopedTransaction.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("water");

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
	return nullptr;
#endif
}

/** Find an actor in the editor world by label. */
static AActor* FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Label)
		{
			return *It;
		}
	}
	return nullptr;
}

/** Find the WaterBodyComponent on an actor (works for all water body types). */
static UWaterBodyComponent* GetWaterBodyComponent(AActor* Actor)
{
	if (!Actor) return nullptr;
	return Actor->FindComponentByClass<UWaterBodyComponent>();
}

FBridgeResult UWaterHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- create_water_body ----
	if (Action == TEXT("create_water_body"))
	{
		FString Type;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'type' (lake|river|ocean)"));

		Type = Type.ToLower();
		if (Type != TEXT("lake") && Type != TEXT("river") && Type != TEXT("ocean"))
			return this->MakeError(DOMAIN, Action, 1001,
				FString::Printf(TEXT("Invalid water body type '%s'"), *Type),
				TEXT("Valid types: lake, river, ocean"));

		// Parse location
		double LocX = 0.0, LocY = 0.0, LocZ = 0.0;
		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj && (*LocObj).IsValid())
		{
			(*LocObj)->TryGetNumberField(TEXT("x"), LocX);
			(*LocObj)->TryGetNumberField(TEXT("y"), LocY);
			(*LocObj)->TryGetNumberField(TEXT("z"), LocZ);
		}

		// Parse optional size
		double SizeX = 5000.0, SizeY = 5000.0;
		const TSharedPtr<FJsonObject>* SizeObj = nullptr;
		if (Params->TryGetObjectField(TEXT("size"), SizeObj) && SizeObj && (*SizeObj).IsValid())
		{
			(*SizeObj)->TryGetNumberField(TEXT("x"), SizeX);
			(*SizeObj)->TryGetNumberField(TEXT("y"), SizeY);
		}

		UWorld* World = GetEditorWorld();
		if (!World)
			return this->MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Create Water Body")));

		// Determine the correct class to spawn
		UClass* WaterClass = nullptr;
		if (Type == TEXT("lake"))
		{
			WaterClass = AWaterBodyLake::StaticClass();
		}
		else if (Type == TEXT("river"))
		{
			WaterClass = AWaterBodyRiver::StaticClass();
		}
		else if (Type == TEXT("ocean"))
		{
			WaterClass = AWaterBodyOcean::StaticClass();
		}

		FTransform SpawnTransform;
		SpawnTransform.SetLocation(FVector((float)LocX, (float)LocY, (float)LocZ));

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* NewActor = World->SpawnActor(WaterClass, &SpawnTransform, SpawnParams);
		if (!NewActor)
			return this->MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to spawn %s water body at (%.0f, %.0f, %.0f). Ensure the Water plugin is enabled and a WaterZone exists in the level."),
					*Type, (float)LocX, (float)LocY, (float)LocZ),
				TEXT("Add a WaterZone actor to the level first if one does not exist"));

		// Apply size for lake type via scale (rivers use spline points, ocean tiles the world)
		if (Type == TEXT("lake"))
		{
			UWaterBodyComponent* WBC = GetWaterBodyComponent(NewActor);
			if (WBC)
			{
				// Lakes use spline-based shapes; scale is approximate via actor scale
				float ScaleX = (float)(SizeX / 5000.0);
				float ScaleY = (float)(SizeY / 5000.0);
				NewActor->SetActorScale3D(FVector(ScaleX, ScaleY, 1.0f));
			}
		}

		FString ActorLabel = FString::Printf(TEXT("WaterBody_%s"), *Type);
		NewActor->SetActorLabel(ActorLabel);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("type"), Type);
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetNumberField(TEXT("location_x"), LocX);
		Data->SetNumberField(TEXT("location_y"), LocY);
		Data->SetNumberField(TEXT("location_z"), LocZ);
		Data->SetNumberField(TEXT("size_x"), SizeX);
		Data->SetNumberField(TEXT("size_y"), SizeY);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Created %s water body '%s' at (%.0f, %.0f, %.0f) size=(%.0f, %.0f)"),
				*Type, *ActorLabel, (float)LocX, (float)LocY, (float)LocZ, (float)SizeX, (float)SizeY),
			Data);
	}

	// ---- set_water_material ----
	if (Action == TEXT("set_water_material"))
	{
		FString ActorLabel;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorLabel) || ActorLabel.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path' (actor label)"));

		FString MaterialPath;
		if (!Params->TryGetStringField(TEXT("material"), MaterialPath) || MaterialPath.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'material' (asset path)"));

		UWorld* World = GetEditorWorld();
		if (!World)
			return this->MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		AActor* Actor = FindActorByLabel(World, ActorLabel);
		if (!Actor)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No actor found with label '%s'"), *ActorLabel),
				TEXT("Use the actor label, not the asset path"));

		UWaterBodyComponent* WBC = GetWaterBodyComponent(Actor);
		if (!WBC)
			return this->MakeError(DOMAIN, Action, 2001,
				FString::Printf(TEXT("Actor '%s' has no WaterBodyComponent"), *ActorLabel),
				TEXT("Ensure the target actor is a Water Body"));

		// Load the material
		FString FullMatPath = MaterialPath + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPath);
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material)
		{
			Material = LoadObject<UMaterialInterface>(nullptr, *FullMatPath);
		}
		if (!Material)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No material found at '%s'"), *MaterialPath),
				TEXT("Provide a valid material or material instance path"));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Water Material")));

		WBC->SetWaterMaterial(Material);
		WBC->MarkPackageDirty();
		Actor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetStringField(TEXT("material"), MaterialPath);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Set water material on '%s' to '%s'"), *ActorLabel, *MaterialPath),
			Data);
	}

	// ---- configure_waves ----
	if (Action == TEXT("configure_waves"))
	{
		FString ActorLabel;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorLabel) || ActorLabel.IsEmpty())
			return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path' (actor label)"));

		double WaveAmplitude = -1.0, WaveLength = -1.0, WaveSpeed = -1.0;
		Params->TryGetNumberField(TEXT("wave_amplitude"), WaveAmplitude);
		Params->TryGetNumberField(TEXT("wave_length"), WaveLength);
		Params->TryGetNumberField(TEXT("wave_speed"), WaveSpeed);

		if (WaveAmplitude < 0.0 && WaveLength < 0.0 && WaveSpeed < 0.0)
			return this->MakeError(DOMAIN, Action, 1000,
				TEXT("At least one wave param required: 'wave_amplitude', 'wave_length', 'wave_speed'"));

		UWorld* World = GetEditorWorld();
		if (!World)
			return this->MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		AActor* Actor = FindActorByLabel(World, ActorLabel);
		if (!Actor)
			return this->MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No actor found with label '%s'"), *ActorLabel),
				TEXT("Use the actor label, not the asset path"));

		UWaterBodyComponent* WBC = GetWaterBodyComponent(Actor);
		if (!WBC)
			return this->MakeError(DOMAIN, Action, 2001,
				FString::Printf(TEXT("Actor '%s' has no WaterBodyComponent"), *ActorLabel),
				TEXT("Ensure the target actor is a Water Body"));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Configure Water Waves")));

		// Access wave settings — UE Water plugin uses FWaterWaves on the component
		int32 Applied = 0;

		// Wave properties are configured via the WaterWaves asset or directly on FWaterBodyData
		// Access through the component's wave model
		if (WaveAmplitude >= 0.0)
		{
			// UE 5.7: SetMaxWaveHeightOffset removed — use MaxWaveHeightOffset property directly
			WBC->MaxWaveHeightOffset = (float)WaveAmplitude;
			++Applied;
		}

		// For wave length and speed, we modify the water waves source if available
		UWaterWavesBase* WaterWaves = WBC->GetWaterWaves();
		if (WaterWaves)
		{
			UWaterWaves* DirectWaves = Cast<UWaterWaves>(WaterWaves);

			if (DirectWaves)
			{
				// UE 5.7: Gerstner wave params are accessed via the WaterWaves properties directly
				// GetGerstnerWaves() removed in 5.7 — set wave params via exposed properties
				if (WaveLength >= 0.0 || WaveSpeed >= 0.0)
				{
					// Access wave parameters through reflection or direct property access
					if (WaveLength >= 0.0)
					{
						// Set via the FProperty system since direct accessors were removed
						FProperty* WaveLenProp = DirectWaves->GetClass()->FindPropertyByName(TEXT("WaveLength"));
						if (WaveLenProp)
						{
							float WaveLenFloat = (float)WaveLength;
							WaveLenProp->CopyCompleteValue(WaveLenProp->ContainerPtrToValuePtr<void>(DirectWaves), &WaveLenFloat);
						}
						++Applied;
					}
					if (WaveSpeed >= 0.0)
					{
						FProperty* WaveSpdProp = DirectWaves->GetClass()->FindPropertyByName(TEXT("WaveSpeed"));
						if (WaveSpdProp)
						{
							float WaveSpdFloat = (float)WaveSpeed;
							WaveSpdProp->CopyCompleteValue(WaveSpdProp->ContainerPtrToValuePtr<void>(DirectWaves), &WaveSpdFloat);
						}
						++Applied;
					}
				}
			}
			else if (!DirectWaves && (WaveLength >= 0.0 || WaveSpeed >= 0.0))
			{
				// Waves are in an asset reference; can't modify inline
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("actor_label"), ActorLabel);
				Data->SetNumberField(TEXT("applied_count"), Applied);
				Data->SetStringField(TEXT("warning"), TEXT("WaterWaves is an asset reference; wave_length and wave_speed require a UWaterWaves (not asset). Only amplitude was applied."));

				return this->MakeSuccess(DOMAIN, Action,
					FString::Printf(TEXT("Partially configured waves on '%s': %d params applied (wave_length/wave_speed skipped — waves are asset-referenced)"),
						*ActorLabel, Applied),
					Data);
			}
		}
		else if (WaveLength >= 0.0 || WaveSpeed >= 0.0)
		{
			// No wave model assigned
			if (Applied == 0)
				return this->MakeError(DOMAIN, Action, 3000,
					FString::Printf(TEXT("No WaterWaves assigned to '%s'; cannot set wave_length/wave_speed"), *ActorLabel),
					TEXT("Assign a WaterWaves asset to the water body first, or only set wave_amplitude"));
		}

		WBC->MarkRenderStateDirty();
		Actor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetNumberField(TEXT("applied_count"), Applied);
		if (WaveAmplitude >= 0.0)  Data->SetNumberField(TEXT("wave_amplitude"), WaveAmplitude);
		if (WaveLength >= 0.0)     Data->SetNumberField(TEXT("wave_length"), WaveLength);
		if (WaveSpeed >= 0.0)      Data->SetNumberField(TEXT("wave_speed"), WaveSpeed);

		return this->MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Configured %d wave params on '%s'"), Applied, *ActorLabel),
			Data);
	}

	if (Action == TEXT("get_water_info")) return Action_GetWaterInfo(Params);

	return this->MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'"), *Action),
		TEXT("water capabilities: create_water_body, set_water_material, configure_waves"));
}

TSharedPtr<FJsonObject> UWaterHandler::GetActionSchemas() const
{
	auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("type"), Type);
		O->SetBoolField(TEXT("required"), bRequired);
		O->SetStringField(TEXT("desc"), Desc);
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a water body actor (lake, river, or ocean)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("type"), P(TEXT("string"), true, TEXT("Water body type: lake, river, ocean"))); Ps->SetObjectField(TEXT("location"), P(TEXT("object"), false, TEXT("Spawn location {x,y,z}"))); Ps->SetObjectField(TEXT("size"), P(TEXT("object"), false, TEXT("Size {x,y} for lake (default 5000)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_water_body"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the water material on a water body actor")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the water body"))); Ps->SetObjectField(TEXT("material"), P(TEXT("string"), true, TEXT("Content path of the material"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_water_material"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Configure wave parameters on a water body")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor label of the water body"))); Ps->SetObjectField(TEXT("wave_amplitude"), P(TEXT("float"), false, TEXT("Wave amplitude (max height offset)"))); Ps->SetObjectField(TEXT("wave_length"), P(TEXT("float"), false, TEXT("Wave length"))); Ps->SetObjectField(TEXT("wave_speed"), P(TEXT("float"), false, TEXT("Wave speed"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("configure_waves"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// get_water_info
// ---------------------------------------------------------------------------

FBridgeResult UWaterHandler::Action_GetWaterInfo(TSharedPtr<FJsonObject> Params)
{
	const FString Action = TEXT("get_water_info");

	FString ActorPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		return this->MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));

	UWorld* World = GetEditorWorld();
	if (!World)
		return this->MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

	AActor* Actor = FindActorByLabel(World, ActorPath);
	if (!Actor)
		return this->MakeError(DOMAIN, Action, 2000,
			FString::Printf(TEXT("No actor found with label '%s'"), *ActorPath));

	UWaterBodyComponent* WBC = GetWaterBodyComponent(Actor);
	if (!WBC)
		return this->MakeError(DOMAIN, Action, 2001,
			FString::Printf(TEXT("Actor '%s' has no UWaterBodyComponent"), *ActorPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorPath);
	Data->SetStringField(TEXT("water_body_type"), UEnum::GetDisplayValueAsText(WBC->GetWaterBodyType()).ToString());

	// Location
	FVector Loc = Actor->GetActorLocation();
	TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Data->SetObjectField(TEXT("location"), LocObj);

	// Material
	if (UMaterialInterface* Mat = WBC->GetWaterMaterial())
	{
		Data->SetStringField(TEXT("material"), Mat->GetPathName());
	}

	// Wave info
	if (const UWaterWavesBase* Waves = WBC->GetWaterWaves())
	{
		Data->SetStringField(TEXT("wave_asset"), Waves->GetName());
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	FBridgeResult R = MakeSuccess(DOMAIN, Action,
		FString::Printf(TEXT("get_water_info: '%s' type=%s"),
			*ActorPath, *UEnum::GetDisplayValueAsText(WBC->GetWaterBodyType()).ToString()));
	R.ExtraData = OutStr;
	return R;
}
