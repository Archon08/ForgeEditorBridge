#include "Handlers/WorldSettingsHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Dom/JsonObject.h"
#include "NavigationSystem.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("world_settings");

static AWorldSettings* GetWS()
{
#if WITH_EDITOR
	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	return World ? World->GetWorldSettings() : nullptr;
#else
	return nullptr;
#endif
}

FBridgeResult UWorldSettingsHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- get_world_settings ----
	if (Action == TEXT("get_world_settings"))
	{
		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("game_mode"), WS->DefaultGameMode ? WS->DefaultGameMode->GetPathName() : TEXT("None"));
		Data->SetNumberField(TEXT("kill_z"), WS->KillZ);
		Data->SetNumberField(TEXT("global_gravity_z"), WS->GlobalGravityZ);
		Data->SetBoolField(TEXT("global_gravity_set"), WS->bGlobalGravitySet);
		Data->SetBoolField(TEXT("force_no_precomputed_lighting"), WS->bForceNoPrecomputedLighting);
		Data->SetBoolField(TEXT("enable_navigation_system"), WS->IsNavigationSystemEnabled());
		Data->SetNumberField(TEXT("world_to_meters"), WS->WorldToMeters);

		TSharedPtr<FJsonObject> LM = MakeShared<FJsonObject>();
		LM->SetNumberField(TEXT("static_lighting_level_scale"), WS->LightmassSettings.StaticLightingLevelScale);
		LM->SetNumberField(TEXT("num_indirect_lighting_bounces"), (double)WS->LightmassSettings.NumIndirectLightingBounces);
		LM->SetNumberField(TEXT("indirect_lighting_quality"), WS->LightmassSettings.IndirectLightingQuality);
		LM->SetNumberField(TEXT("indirect_lighting_smoothness"), WS->LightmassSettings.IndirectLightingSmoothness);
		Data->SetObjectField(TEXT("lightmass"), LM);

		return MakeSuccess(DOMAIN, Action, TEXT("World settings retrieved"), Data);
	}

	// ---- set_game_mode ----
	if (Action == TEXT("set_game_mode"))
	{
		FString GMClass;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("game_mode_class"), GMClass))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'game_mode_class'"));

		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		UClass* LoadedClass = LoadClass<AGameModeBase>(nullptr, *GMClass);
		if (!LoadedClass)
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Failed to load game mode class: %s"), *GMClass));

		WS->DefaultGameMode = LoadedClass;
		WS->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Game mode set to: %s"), *GMClass));
	}

	// ---- set_kill_z ----
	if (Action == TEXT("set_kill_z"))
	{
		double KillZ;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("kill_z"), KillZ))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'kill_z' (number)"));

		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		WS->KillZ = (float)KillZ;
		WS->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("KillZ set to %.1f"), KillZ));
	}

	// ---- set_global_gravity ----
	if (Action == TEXT("set_global_gravity"))
	{
		double GravityZ;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("gravity_z"), GravityZ))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'gravity_z' (number)"));

		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		WS->bGlobalGravitySet = true;
		WS->GlobalGravityZ = (float)GravityZ;
		WS->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Global gravity set to %.1f"), GravityZ));
	}

	// ---- set_lightmass ----
	if (Action == TEXT("set_lightmass"))
	{
		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		FLightmassWorldInfoSettings& LM = WS->LightmassSettings;
		bool bChanged = false;
		double V;

		if (Params.IsValid())
		{
			if (Params->TryGetNumberField(TEXT("num_bounces"), V))       { LM.NumIndirectLightingBounces = (int32)V; bChanged = true; }
			if (Params->TryGetNumberField(TEXT("quality"), V))           { LM.IndirectLightingQuality = (float)V;   bChanged = true; }
			if (Params->TryGetNumberField(TEXT("smoothness"), V))        { LM.IndirectLightingSmoothness = (float)V; bChanged = true; }
			if (Params->TryGetNumberField(TEXT("static_level_scale"), V)){ LM.StaticLightingLevelScale = (float)V;  bChanged = true; }

			const TSharedPtr<FJsonObject>* EnvColor;
			if (Params->TryGetObjectField(TEXT("environment_color"), EnvColor))
			{
				double R = 0, G = 0, B = 0;
				(*EnvColor)->TryGetNumberField(TEXT("r"), R);
				(*EnvColor)->TryGetNumberField(TEXT("g"), G);
				(*EnvColor)->TryGetNumberField(TEXT("b"), B);
				LM.EnvironmentColor = FColor((uint8)R, (uint8)G, (uint8)B);
				bChanged = true;
			}
		}

		if (!bChanged)
			return MakeError(DOMAIN, Action, 1000, TEXT("No lightmass parameters provided"));

		WS->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, TEXT("Lightmass settings updated"));
	}

	// ---- set_world_property ----
	if (Action == TEXT("set_world_property"))
	{
		FString PropName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("property"), PropName))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'property'"));

		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		FProperty* Prop = AWorldSettings::StaticClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
			return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Property '%s' not found on AWorldSettings"), *PropName));

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(WS);

		if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		{
			bool bVal = false;
			if (!Params->TryGetBoolField(TEXT("value"), bVal))
				return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be boolean for this property"));
			BP->SetPropertyValue(ValuePtr, bVal);
		}
		else if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
		{
			double NumVal;
			if (!Params->TryGetNumberField(TEXT("value"), NumVal))
				return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a number for this property"));
			FP->SetPropertyValue(ValuePtr, (float)NumVal);
		}
		else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
		{
			double NumVal;
			if (!Params->TryGetNumberField(TEXT("value"), NumVal))
				return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a number for this property"));
			DP->SetPropertyValue(ValuePtr, NumVal);
		}
		else if (FIntProperty* IP = CastField<FIntProperty>(Prop))
		{
			double NumVal;
			if (!Params->TryGetNumberField(TEXT("value"), NumVal))
				return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a number for this property"));
			IP->SetPropertyValue(ValuePtr, (int32)NumVal);
		}
		else
		{
			return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unsupported property type for generic setter: %s"), *PropName));
		}

		WS->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Set %s"), *PropName));
	}

	// ---- set_default_map ----
	if (Action == TEXT("set_default_map"))
	{
		FString MapPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("map_path"), MapPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'map_path'"));

		GConfig->SetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("EditorStartupMap"), *MapPath, GEditorIni);
		GConfig->Flush(false, GEditorIni);

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Default editor startup map set to: %s"), *MapPath));
		R.AffectedPath = MapPath;
		return R;
	}

	// ---- set_world_settings ----
	if (Action == TEXT("set_world_settings"))
	{
		AWorldSettings* WS = GetWS();
		if (!WS) return MakeError(DOMAIN, Action, 3000, TEXT("No world settings available"));

		bool bChanged = false;
		TArray<FString> Changed;
		double V;

		if (Params.IsValid())
		{
			if (Params->TryGetNumberField(TEXT("kill_z"), V))
			{
				WS->KillZ = (float)V;
				Changed.Add(FString::Printf(TEXT("kill_z=%.1f"), V));
				bChanged = true;
			}
			if (Params->TryGetNumberField(TEXT("gravity_z"), V))
			{
				WS->bGlobalGravitySet = true;
				WS->GlobalGravityZ = (float)V;
				Changed.Add(FString::Printf(TEXT("gravity_z=%.1f"), V));
				bChanged = true;
			}
			if (Params->TryGetNumberField(TEXT("world_to_meters"), V))
			{
				WS->WorldToMeters = (float)V;
				Changed.Add(FString::Printf(TEXT("world_to_meters=%.1f"), V));
				bChanged = true;
			}
			if (Params->TryGetNumberField(TEXT("world_bounds_scale"), V))
			{
				// UE 5.7: AWorldSettings::WorldBoundsScale was removed — silently accept
				// the parameter but note it as unsupported in the change list.
				Changed.Add(FString::Printf(TEXT("world_bounds_scale=<unsupported in UE 5.7>")));
				bChanged = true;
			}
			FString NavClass;
			if (Params->TryGetStringField(TEXT("navigation_system_class"), NavClass))
			{
				UClass* NavSysClass = FindObject<UClass>(nullptr, *NavClass);
				if (!NavSysClass)
					NavSysClass = LoadClass<UObject>(nullptr, *NavClass);
				if (NavSysClass)
				{
					// UE 5.7: AWorldSettings::NavigationSystemConfig is protected. Use the
					// public SetNavigationSystemConfigOverride entry point which wins over
					// the base NavigationSystemConfig in GetNavigationSystemConfig().
					UNavigationSystemConfig* NewConfig = NewObject<UNavigationSystemConfig>(WS, NavSysClass);
					WS->SetNavigationSystemConfigOverride(NewConfig);
					Changed.Add(FString::Printf(TEXT("navigation_system_class=%s"), *NavClass));
					bChanged = true;
				}
				else
				{
					return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Could not find navigation system class: %s"), *NavClass));
				}
			}
		}

		if (!bChanged)
			return MakeError(DOMAIN, Action, 1000, TEXT("No world settings parameters provided"));

		WS->PostEditChange();
		WS->MarkPackageDirty();

		FString Summary = FString::Join(Changed, TEXT(", "));
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("changed"), Summary);
		Data->SetNumberField(TEXT("fields_changed"), (double)Changed.Num());
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("World settings updated: %s"), *Summary), Data);
	}

	return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("system/capabilities"));
}

TSharedPtr<FJsonObject> UWorldSettingsHandler::GetActionSchemas() const
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

	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Retrieve current world settings (game mode, gravity, lightmass, etc.)"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_world_settings"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the default game mode class"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("game_mode_class"), P(TEXT("string"), true, TEXT("Full class path of the game mode")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_game_mode"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the KillZ height"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("kill_z"), P(TEXT("float"), true, TEXT("Z height below which actors are destroyed")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_kill_z"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set global gravity override"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("gravity_z"), P(TEXT("float"), true, TEXT("Gravity Z value (negative = downward)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_global_gravity"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Configure Lightmass world settings"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("num_bounces"), P(TEXT("int"), false, TEXT("Number of indirect lighting bounces")));
		Params->SetObjectField(TEXT("quality"), P(TEXT("float"), false, TEXT("Indirect lighting quality multiplier")));
		Params->SetObjectField(TEXT("smoothness"), P(TEXT("float"), false, TEXT("Indirect lighting smoothness")));
		Params->SetObjectField(TEXT("static_level_scale"), P(TEXT("float"), false, TEXT("Static lighting level scale")));
		Params->SetObjectField(TEXT("environment_color"), P(TEXT("object"), false, TEXT("Environment color {r,g,b} as 0-255 ints")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_lightmass"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set an arbitrary AWorldSettings property via reflection"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("property"), P(TEXT("string"), true, TEXT("UPROPERTY name on AWorldSettings")));
		Params->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value (bool, float, int supported)")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_world_property"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the default editor startup map in config"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("map_path"), P(TEXT("string"), true, TEXT("Content path of the map")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_default_map"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Comprehensive world settings setter — set multiple properties in one call"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("kill_z"),                  P(TEXT("float"),  false, TEXT("KillZ height")));
		Params->SetObjectField(TEXT("gravity_z"),               P(TEXT("float"),  false, TEXT("Global gravity Z (enables bGlobalGravitySet)")));
		Params->SetObjectField(TEXT("world_to_meters"),         P(TEXT("float"),  false, TEXT("World scale: units per meter")));
		Params->SetObjectField(TEXT("world_bounds_scale"),      P(TEXT("float"),  false, TEXT("World bounds scale multiplier")));
		Params->SetObjectField(TEXT("navigation_system_class"), P(TEXT("string"), false, TEXT("Full UClass path for the navigation system config")));
		A->SetObjectField(TEXT("params"), Params);
		Root->SetObjectField(TEXT("set_world_settings"), A);
	}

	return Root;
}
