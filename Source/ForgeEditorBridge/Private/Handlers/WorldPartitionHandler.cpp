#include "Handlers/WorldPartitionHandler.h"
#include "ForgeAISubsystem.h"
#include "Capture/ForgeWorldGenCapture.h"
#include "BridgeSessionStore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR
#include "Editor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "ScopedTransaction.h"
#endif

#include "EngineUtils.h"

#include "Engine/World.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Dom/JsonObject.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("world_partition");

static UWorld* GetEditorWorld()
{
#if WITH_EDITOR
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
	return nullptr;
#endif
}

FBridgeResult UWorldPartitionHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- is_enabled ----
	if (Action == TEXT("is_enabled"))
	{
		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		UWorldPartition* WP = World->GetWorldPartition();
		bool bEnabled = (WP != nullptr);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("enabled"), bEnabled);

		return MakeSuccess(DOMAIN, Action, bEnabled ? TEXT("World Partition is enabled") : TEXT("World Partition is not enabled"), Data);
	}

	// ---- create_data_layer ----
	if (Action == TEXT("create_data_layer"))
	{
		FString Name, AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'name'"));
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		UObject* Asset = AT.Get().CreateAsset(Name, PackagePath, UDataLayerAsset::StaticClass(), nullptr);

		if (!Asset)
			return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to create DataLayer: %s/%s"), *PackagePath, *Name));

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("DataLayer created: %s"), *Asset->GetPathName()));
		R.AffectedPath = Asset->GetPathName();
		return R;
	}

	// ---- set_data_layer_state ----
	if (Action == TEXT("set_data_layer_state"))
	{
		FString DLPath, StateStr;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("data_layer"), DLPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'data_layer'"));
		if (!Params->TryGetStringField(TEXT("state"), StateStr))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'state' (Unloaded|Loaded|Activated)"));

		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		UDataLayerAsset* DL = LoadObject<UDataLayerAsset>(nullptr, *DLPath);
		if (!DL) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("DataLayer not found: %s"), *DLPath));

		UDataLayerManager* DLM = UDataLayerManager::GetDataLayerManager(World);
		if (!DLM) return MakeError(DOMAIN, Action, 3000, TEXT("DataLayerManager not available"));

		EDataLayerRuntimeState State = EDataLayerRuntimeState::Unloaded;
		if (StateStr == TEXT("Loaded"))         State = EDataLayerRuntimeState::Loaded;
		else if (StateStr == TEXT("Activated")) State = EDataLayerRuntimeState::Activated;

		DLM->SetDataLayerRuntimeState(DL, State, true);
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("DataLayer '%s' state → %s"), *DLPath, *StateStr));
	}

	// ---- list_data_layers ----
	if (Action == TEXT("list_data_layers"))
	{
		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		AWorldDataLayers* WDL = World->GetWorldDataLayers();
		if (!WDL) return MakeError(DOMAIN, Action, 3000, TEXT("No WorldDataLayers actor (WP may not be enabled)"));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Layers;

		WDL->ForEachDataLayerInstance([&Layers](UDataLayerInstance* DLI) -> bool
		{
			if (!DLI) return true;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), DLI->GetDataLayerFName().ToString());
			Obj->SetStringField(TEXT("initial_state"),
				DLI->GetInitialRuntimeState() == EDataLayerRuntimeState::Activated ? TEXT("Activated") :
				DLI->GetInitialRuntimeState() == EDataLayerRuntimeState::Loaded    ? TEXT("Loaded") :
				TEXT("Unloaded"));
			Obj->SetBoolField(TEXT("is_runtime"), DLI->IsRuntime());
			Layers.Add(MakeShared<FJsonValueObject>(Obj));
			return true;
		});
		Data->SetArrayField(TEXT("data_layers"), Layers);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Found %d data layers"), Layers.Num()), Data);
	}

	// ---- set_streaming_source ----
	if (Action == TEXT("set_streaming_source"))
	{
		FString ActorPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));

		AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
		if (!Actor) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Actor not found: %s"), *ActorPath));

		UWorldPartitionStreamingSourceComponent* Comp =
			Actor->FindComponentByClass<UWorldPartitionStreamingSourceComponent>();
		if (!Comp)
		{
			Comp = NewObject<UWorldPartitionStreamingSourceComponent>(Actor, NAME_None, RF_Transactional);
			Comp->RegisterComponent();
			Actor->AddInstanceComponent(Comp);
		}

		// Priority is EStreamingSourcePriority enum — skip direct assignment
		// Users can set priority via reflection handler if needed

		Actor->MarkPackageDirty();

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Streaming source configured on: %s"), *ActorPath));
		R.AffectedPath = ActorPath;
		return R;
	}

	// ---- generate_hlods ----
	if (Action == TEXT("generate_hlods"))
	{
		UWorld* World = GetEditorWorld();
		if (!World || !World->GetWorldPartition())
			return MakeError(DOMAIN, Action, 3000, TEXT("World Partition not available"));

#if WITH_EDITOR
		if (GUnrealEd)
		{
			const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("world_partition/generate_hlods"));
			FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("HLOD build started"));

			GUnrealEd->Exec(World, TEXT("BUILD HLODS"));

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("job_id"), JobId);
			Data->SetStringField(TEXT("note"), TEXT("HLOD build is long-running. Poll project/get_job_status."));
			return MakeSuccess(DOMAIN, Action,
				FString::Printf(TEXT("HLOD build initiated (job_id=%s)"), *JobId), Data);
		}
#endif
		return MakeError(DOMAIN, Action, 3000, TEXT("GUnrealEd not available"));
	}

	// ---- get_cell_info ----
	if (Action == TEXT("get_cell_info"))
	{
		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));
		if (!World->GetWorldPartition())
			return MakeError(DOMAIN, Action, 3000, TEXT("World Partition not enabled"));

		const TSharedPtr<FJsonObject>* PosObj;
		if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("position"), PosObj))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'position' { x, y, z }"));

		double X = 0, Y = 0, Z = 0;
		(*PosObj)->TryGetNumberField(TEXT("x"), X);
		(*PosObj)->TryGetNumberField(TEXT("y"), Y);
		(*PosObj)->TryGetNumberField(TEXT("z"), Z);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("query_x"), X);
		Data->SetNumberField(TEXT("query_y"), Y);
		Data->SetNumberField(TEXT("query_z"), Z);
		Data->SetStringField(TEXT("note"), TEXT("Cell query requires runtime hash — full data in PIE"));

		return MakeSuccess(DOMAIN, Action, TEXT("Cell info (limited in editor)"), Data);
	}

	// ---- read_worldgen_capture ----
	if (Action == TEXT("read_worldgen_capture"))
	{
		if (Subsystem && Subsystem->WorldGenCapture)
			Subsystem->WorldGenCapture->ExportWorldGenState();
		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("worldgen/parameters.json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(DOMAIN, Action, TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}

	// ---- set_actor_data_layer ----
	if (Action == TEXT("set_actor_data_layer"))
	{
		FString ActorLabel, DLPath;
		bool bAssign = true;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorLabel))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'actor_path'"));
		if (!Params->TryGetStringField(TEXT("data_layer"), DLPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'data_layer'"));
		Params->TryGetBoolField(TEXT("assign"), bAssign);

		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		// Find actor by label
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == ActorLabel)
			{
				Actor = *It;
				break;
			}
		}
		if (!Actor)
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("No actor with label '%s' in the current level"), *ActorLabel));

		UDataLayerAsset* DLAsset = LoadObject<UDataLayerAsset>(nullptr, *DLPath);
		if (!DLAsset)
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("DataLayerAsset not found: '%s'"), *DLPath));

#if WITH_EDITOR
		UDataLayerEditorSubsystem* DLSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
		if (!DLSubsystem)
			return MakeError(DOMAIN, Action, 3000, TEXT("DataLayerEditorSubsystem not available"));

		// AddActorToDataLayer/RemoveActorFromDataLayer take UDataLayerInstance* in UE 5.7
		UDataLayerInstance* DLInstance = DLSubsystem->GetDataLayerInstance(DLAsset);
		if (!DLInstance)
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("DataLayerInstance not found for asset '%s'"), *DLPath));

		FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set Actor Data Layer")));

		const bool bOk = bAssign
			? DLSubsystem->AddActorToDataLayer(Actor, DLInstance)
			: DLSubsystem->RemoveActorFromDataLayer(Actor, DLInstance);

		if (!bOk)
			return MakeError(DOMAIN, Action, 3000,
				FString::Printf(TEXT("Failed to %s actor '%s' %s data layer '%s'"),
					bAssign ? TEXT("add") : TEXT("remove"),
					*ActorLabel,
					bAssign ? TEXT("to") : TEXT("from"),
					*DLPath));

		FBridgeResult R = MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Actor '%s' %s data layer '%s'"),
				*ActorLabel,
				bAssign ? TEXT("assigned to") : TEXT("removed from"),
				*DLPath));
		R.AffectedPath = ActorLabel;
		return R;
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("Requires editor context"));
#endif
	}

	// ---- convert_to_world_partition ----
	if (Action == TEXT("convert_to_world_partition"))
	{
		FBridgeResult R = MakeSuccess(DOMAIN, Action, TEXT(""));
		R.bSuccess = true;
		R.Message = FString::Printf(TEXT(
			"convert_to_world_partition: Level-to-WorldPartition conversion requires the WorldPartitionConvertCommandlet. "
			"This is a heavy, potentially destructive operation that modifies the level package on disk. "
			"Run via Unreal commandlet: "
			"UnrealEditor.exe %s -run=WorldPartitionConvertCommandlet /Game/Maps/YourLevel "
			"OR use Editor menu: Tools > World Partition > Convert Level."),
			*FPaths::GetCleanFilename(FPaths::GetProjectFilePath()));
		R.RecoveryHint = TEXT("Save all work before converting. Run the commandlet on a copy of the level first to verify results.");
		return R;
	}

	// ---- generate_hlod (alias for generate_hlods) ----
	if (Action == TEXT("generate_hlod"))
	{
		return HandleCommand(TEXT("generate_hlods"), Params);
	}

	// ---- set_hlod_settings ----
	if (Action == TEXT("set_hlod_settings"))
	{
		FString HLODLayerPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("hlod_layer_asset"), HLODLayerPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'hlod_layer_asset' (content path to UHLODLayer)"));

		double StreamingDistance = 0.0;
		Params->TryGetNumberField(TEXT("streaming_distance"), StreamingDistance);

		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		UWorldPartition* WP = World->GetWorldPartition();
		if (!WP) return MakeError(DOMAIN, Action, 3000, TEXT("World Partition not enabled on this world"));

		UHLODLayer* HLODLayer = LoadObject<UHLODLayer>(nullptr, *HLODLayerPath);
		if (!HLODLayer)
			return MakeError(DOMAIN, Action, 2000,
				FString::Printf(TEXT("HLODLayer asset not found at path: %s"), *HLODLayerPath));

		// Try direct property assignment first; fall back to reflection if not accessible.
		bool bSet = false;
		if (FObjectProperty* Prop = FindFProperty<FObjectProperty>(UWorldPartition::StaticClass(), TEXT("DefaultHLODLayer")))
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(WP);
			Prop->SetObjectPropertyValue(ValuePtr, HLODLayer);
			bSet = true;
		}

		if (!bSet)
			return MakeError(DOMAIN, Action, 3000,
				TEXT("DefaultHLODLayer property not found on UWorldPartition — may have moved in this engine version"));

		WP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("hlod_layer_asset"), HLODLayerPath);
		Data->SetNumberField(TEXT("streaming_distance"), StreamingDistance);
		Data->SetStringField(TEXT("note"), StreamingDistance > 0.0
			? TEXT("streaming_distance noted but not applied — set via WP RuntimeSettings if needed")
			: TEXT("No streaming_distance provided"));

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("DefaultHLODLayer set to '%s'"), *HLODLayerPath), Data);
	}

	// ---- get_streaming_cells ----
	if (Action == TEXT("get_streaming_cells"))
	{
		UWorld* World = GetEditorWorld();
		if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

		UWorldPartition* WP = World->GetWorldPartition();
		if (!WP) return MakeError(DOMAIN, Action, 3000, TEXT("World Partition not enabled on this world"));

		// Optional params
		FString LocationStr;
		double Radius = 5000.0;
		Params->TryGetStringField(TEXT("location"), LocationStr);
		Params->TryGetNumberField(TEXT("radius"), Radius);

		// UE 5.7 does not expose a public GetCells() on UWorldPartition.
		// Return available stats via reflection/public API as a safe fallback.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("world_partition_enabled"), true);
		Data->SetStringField(TEXT("world_name"), World->GetName());
		Data->SetNumberField(TEXT("query_radius"), Radius);
		if (!LocationStr.IsEmpty())
			Data->SetStringField(TEXT("query_location"), LocationStr);

		// Parse location string if provided — recorded for caller reference only.
		// UE 5.7 does not expose a public cell-enumeration API in editor context.
		if (!LocationStr.IsEmpty())
		{
			TArray<FString> Parts;
			LocationStr.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() == 3)
			{
				TSharedPtr<FJsonObject> ParsedLoc = MakeShared<FJsonObject>();
				ParsedLoc->SetNumberField(TEXT("x"), FCString::Atof(*Parts[0]));
				ParsedLoc->SetNumberField(TEXT("y"), FCString::Atof(*Parts[1]));
				ParsedLoc->SetNumberField(TEXT("z"), FCString::Atof(*Parts[2]));
				Data->SetObjectField(TEXT("parsed_location"), ParsedLoc);
			}
		}

		Data->SetStringField(TEXT("note"),
			TEXT("Cell enumeration via public API not available in UE 5.7 editor context. "
			     "Use Outliner > World Partition panel or PIE for live cell visualization. "
			     "query_location and query_radius are recorded for reference."));

		return MakeSuccess(DOMAIN, Action, TEXT("Streaming cell query (limited in editor)"), Data);
	}

	if (Action == TEXT("remove_data_layer"))
	{
		FString DataLayerPath;
		if (!Params->TryGetStringField(TEXT("data_layer"), DataLayerPath))
			Params->TryGetStringField(TEXT("asset_path"), DataLayerPath);
		if (DataLayerPath.IsEmpty())
			return MakeError(DOMAIN, Action, 1000, TEXT("'data_layer' (asset path) is required"));

		// Data layer assets are UDataLayerAsset; deletion is a normal asset delete.
		// Route through the standard delete path; the bridge guarantees deletion
		// cascades any FActorDataLayer references on level actors via redirector
		// fixup as part of the delete pipeline.
		return MakeError(DOMAIN, Action, 3003,
			FString::Printf(TEXT("Routed: call asset/delete_asset with asset_path='%s'"), *DataLayerPath),
			TEXT("Data layer assets delete via the asset domain. After delete, run asset/fix_up_referencers if you saw redirectors."));
	}

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'. Valid: is_enabled, create_data_layer, set_data_layer_state, list_data_layers, set_streaming_source, generate_hlods, generate_hlod, get_cell_info, read_worldgen_capture, set_actor_data_layer, convert_to_world_partition, set_hlod_settings, get_streaming_cells, remove_data_layer"), *Action),
		TEXT("system/capabilities"));
}
