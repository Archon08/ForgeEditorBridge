#include "Handlers/RenderingHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ReflectionCapture.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/MeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("rendering");

// ---------------------------------------------------------------------------
// GetEditorWorld
// ---------------------------------------------------------------------------

UWorld* URenderingHandler::GetEditorWorld() const
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
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("set_nanite"))            return Action_SetNanite(Params);
	if (Action == TEXT("set_lumen"))             return Action_SetLumen(Params);
	if (Action == TEXT("set_shadow_method"))     return Action_SetShadowMethod(Params);
	if (Action == TEXT("capture_reflection"))    return Action_CaptureReflection(Params);
	if (Action == TEXT("set_post_process_blend"))return Action_SetPostProcessBlend(Params);
	if (Action == TEXT("set_console_var"))       return Action_SetConsoleVar(Params);
	if (Action == TEXT("get_render_stats"))      return Action_GetRenderStats(Params);
	if (Action == TEXT("set_lod_screen_size"))   return Action_SetLODScreenSize(Params);
	if (Action == TEXT("set_ray_tracing"))            return Action_SetRayTracing(Params);
	if (Action == TEXT("set_anti_aliasing"))          return Action_SetAntiAliasing(Params);
	if (Action == TEXT("set_nanite_settings"))        return Action_SetNaniteSettings(Params);
	if (Action == TEXT("set_megalights"))             return Action_SetMegaLights(Params);
	if (Action == TEXT("set_smaa"))                   return Action_SetSMAA(Params);
	if (Action == TEXT("set_affect_dynamic_lighting"))return Action_SetAffectDynamicLighting(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown rendering action '%s'"), *Action));
}

// ---------------------------------------------------------------------------
// set_nanite
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetNanite(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_nanite"), 1000, TEXT("'asset_path' is required"));

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Mesh)
		return MakeError(DOMAIN, TEXT("set_nanite"), 2001,
			FString::Printf(TEXT("StaticMesh not found: '%s'"), *AssetPath));

	auto Tx = BeginTransaction(TEXT("Bridge: set_nanite"));
	Mesh->Modify();
	{
		FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
		NaniteSettings.bEnabled = bEnabled;
		Mesh->SetNaniteSettings(NaniteSettings);
	}
	Mesh->MarkPackageDirty();
	Mesh->PostEditChange();

	return MakeSuccess(DOMAIN, TEXT("set_nanite"),
		FString::Printf(TEXT("Nanite %s on '%s'"), bEnabled ? TEXT("enabled") : TEXT("disabled"), *AssetPath));
}

// ---------------------------------------------------------------------------
// set_lumen  (CVars — project settings require editor restart; CVars take effect immediately)
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetLumen(TSharedPtr<FJsonObject> Params)
{
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	// DynamicGlobalIllumination.Method: 0=None, 1=Lumen, 2=SSGI, 3=RTGI
	if (IConsoleVariable* GI = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIllumination.Method")))
		GI->Set(bEnabled ? 1 : 0, ECVF_SetByCode);

	// ReflectionMethod: 0=None, 1=Lumen, 2=SSR, 3=RT
	if (IConsoleVariable* Refl = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ReflectionMethod")))
		Refl->Set(bEnabled ? 1 : 0, ECVF_SetByCode);

	return MakeSuccess(DOMAIN, TEXT("set_lumen"),
		FString::Printf(TEXT("Lumen %s (r.DynamicGlobalIllumination.Method + r.ReflectionMethod)"),
			bEnabled ? TEXT("enabled") : TEXT("disabled")));
}

// ---------------------------------------------------------------------------
// set_shadow_method
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetShadowMethod(TSharedPtr<FJsonObject> Params)
{
	FString Method;
	if (!Params->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_shadow_method"), 1000,
			TEXT("'method' is required: VSM|Cascaded|RayTraced"));

	if (Method.Equals(TEXT("VSM"), ESearchCase::IgnoreCase))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable")))
			V->Set(1, ECVF_SetByCode);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadows")))
			V->Set(0, ECVF_SetByCode);
	}
	else if (Method.Equals(TEXT("Cascaded"), ESearchCase::IgnoreCase))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable")))
			V->Set(0, ECVF_SetByCode);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadows")))
			V->Set(0, ECVF_SetByCode);
	}
	else if (Method.Equals(TEXT("RayTraced"), ESearchCase::IgnoreCase))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable")))
			V->Set(0, ECVF_SetByCode);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadows")))
			V->Set(1, ECVF_SetByCode);
	}
	else
	{
		return MakeError(DOMAIN, TEXT("set_shadow_method"), 1000,
			FString::Printf(TEXT("Unknown method '%s'. Valid: VSM|Cascaded|RayTraced"), *Method));
	}

	return MakeSuccess(DOMAIN, TEXT("set_shadow_method"),
		FString::Printf(TEXT("Shadow method set to '%s'"), *Method));
}

// ---------------------------------------------------------------------------
// capture_reflection
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_CaptureReflection(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("capture_reflection"), 3003, TEXT("No editor world available"));

	int32 Count = 0;
	for (TActorIterator<AReflectionCapture> It(World); It; ++It)
	{
		if (UReflectionCaptureComponent* Comp = (*It)->GetCaptureComponent())
		{
			Comp->MarkDirtyForRecapture();
			++Count;
		}
	}

	UReflectionCaptureComponent::UpdateReflectionCaptureContents(World);

	return MakeSuccess(DOMAIN, TEXT("capture_reflection"),
		FString::Printf(TEXT("Triggered rebuild of %d reflection capture(s)"), Count));
}

// ---------------------------------------------------------------------------
// set_post_process_blend
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetPostProcessBlend(TSharedPtr<FJsonObject> Params)
{
	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_post_process_blend"), 3003, TEXT("No editor world available"));

	FString ActorName;
	Params->TryGetStringField(TEXT("actor_name"), ActorName);

	APostProcessVolume* PPVol = nullptr;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		if (ActorName.IsEmpty() || (*It)->GetName().Equals(ActorName, ESearchCase::IgnoreCase))
		{
			PPVol = *It;
			break;
		}
	}

	if (!PPVol)
	{
		return MakeError(DOMAIN, TEXT("set_post_process_blend"), 2001,
			ActorName.IsEmpty()
				? TEXT("No APostProcessVolume found in level")
				: FString::Printf(TEXT("APostProcessVolume '%s' not found"), *ActorName));
	}

	auto Tx = BeginTransaction(TEXT("Bridge: set_post_process_blend"));
	PPVol->Modify();

	double BlendWeight = 1.0;
	if (Params->TryGetNumberField(TEXT("blend_weight"), BlendWeight))
		PPVol->BlendWeight = (float)FMath::Clamp(BlendWeight, 0.0, 1.0);

	bool bUnbound = PPVol->bUnbound;
	if (Params->TryGetBoolField(TEXT("unbound"), bUnbound))
		PPVol->bUnbound = bUnbound;

	double Priority = (double)PPVol->Priority;
	if (Params->TryGetNumberField(TEXT("priority"), Priority))
		PPVol->Priority = (float)Priority;

	PPVol->MarkComponentsRenderStateDirty();

	return MakeSuccess(DOMAIN, TEXT("set_post_process_blend"),
		FString::Printf(TEXT("PostProcessVolume '%s': blend=%.2f unbound=%s priority=%.0f"),
			*PPVol->GetName(), PPVol->BlendWeight,
			PPVol->bUnbound ? TEXT("true") : TEXT("false"),
			(double)PPVol->Priority));
}

// ---------------------------------------------------------------------------
// set_console_var
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetConsoleVar(TSharedPtr<FJsonObject> Params)
{
	FString VarName, Value;
	if (!Params->TryGetStringField(TEXT("var_name"), VarName) || VarName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_console_var"), 1000, TEXT("'var_name' is required"));
	if (!Params->TryGetStringField(TEXT("value"), Value))
		return MakeError(DOMAIN, TEXT("set_console_var"), 1000, TEXT("'value' is required"));

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*VarName);
	if (!CVar)
		return MakeError(DOMAIN, TEXT("set_console_var"), 2001,
			FString::Printf(TEXT("Console variable '%s' not found"), *VarName));

	CVar->Set(*Value, ECVF_SetByCode);

	bool bPersist = false;
	if (Params->TryGetBoolField(TEXT("persist"), bPersist) && bPersist)
	{
		// Write to DefaultEngine.ini [/Script/Engine.RendererSettings]
		GConfig->SetString(TEXT("SystemSettings"), *VarName, *Value, GEngineIni);
		GConfig->Flush(false, GEngineIni);
	}

	return MakeSuccess(DOMAIN, TEXT("set_console_var"),
		FString::Printf(TEXT("Set '%s' = '%s'%s"), *VarName, *Value,
			bPersist ? TEXT(" (persisted to DefaultEngine.ini)") : TEXT("")));
}

// ---------------------------------------------------------------------------
// get_render_stats
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_GetRenderStats(TSharedPtr<FJsonObject> Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	auto GetCVarInt = [](const TCHAR* Name) -> int32
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name))
			return V->GetInt();
		return -1;
	};
	auto GetCVarFloat = [](const TCHAR* Name) -> float
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name))
			return V->GetFloat();
		return -1.f;
	};

	// Lumen
	int32 GIMethod = GetCVarInt(TEXT("r.DynamicGlobalIllumination.Method"));
	Data->SetNumberField(TEXT("lumen_gi_method"), GIMethod);
	Data->SetBoolField(TEXT("lumen_gi_enabled"), GIMethod == 1);

	int32 ReflMethod = GetCVarInt(TEXT("r.ReflectionMethod"));
	Data->SetNumberField(TEXT("lumen_reflection_method"), ReflMethod);
	Data->SetBoolField(TEXT("lumen_reflections_enabled"), ReflMethod == 1);

	// Virtual Shadow Maps
	int32 VSM = GetCVarInt(TEXT("r.Shadow.Virtual.Enable"));
	Data->SetBoolField(TEXT("vsm_enabled"), VSM == 1);

	// Ray Tracing
	int32 RT = GetCVarInt(TEXT("r.RayTracing.Enable"));
	Data->SetBoolField(TEXT("ray_tracing_enabled"), RT == 1);

	int32 RTShadows = GetCVarInt(TEXT("r.RayTracing.Shadows"));
	Data->SetBoolField(TEXT("ray_traced_shadows_enabled"), RTShadows == 1);

	// Anti-Aliasing
	int32 AA = GetCVarInt(TEXT("r.AntiAliasingMethod"));
	const TArray<FString> AANames = { TEXT("None"), TEXT("FXAA"), TEXT("TAA"), TEXT("MSAA"), TEXT("TSR") };
	Data->SetNumberField(TEXT("anti_aliasing_method"), AA);
	Data->SetStringField(TEXT("anti_aliasing_name"), AANames.IsValidIndex(AA) ? AANames[AA] : TEXT("Unknown"));

	// PostProcess volumes in level
	int32 PPCount = 0;
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			++PPCount;

		// Reflection capture count
		int32 RCCount = 0;
		for (TActorIterator<AReflectionCapture> It(World); It; ++It)
			++RCCount;
		Data->SetNumberField(TEXT("reflection_capture_count"), RCCount);
	}
	Data->SetNumberField(TEXT("post_process_volume_count"), PPCount);

	// Nanite (global enable, not per-mesh)
	int32 NaniteGlobal = GetCVarInt(TEXT("r.Nanite"));
	Data->SetBoolField(TEXT("nanite_globally_enabled"), NaniteGlobal != 0);

	return MakeSuccess(DOMAIN, TEXT("get_render_stats"), TEXT("Render stats queried"), Data);
}

// ---------------------------------------------------------------------------
// set_lod_screen_size
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetLODScreenSize(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_lod_screen_size"), 1000, TEXT("'asset_path' is required"));

	double LODIndexD = 0.0;
	Params->TryGetNumberField(TEXT("lod_index"), LODIndexD);
	int32 LODIndex = (int32)LODIndexD;

	double ScreenSize = 0.3;
	Params->TryGetNumberField(TEXT("screen_size"), ScreenSize);

	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Mesh)
		return MakeError(DOMAIN, TEXT("set_lod_screen_size"), 2001,
			FString::Printf(TEXT("StaticMesh not found: '%s'"), *AssetPath));

	if (LODIndex < 0 || LODIndex >= Mesh->GetNumLODs())
		return MakeError(DOMAIN, TEXT("set_lod_screen_size"), 1000,
			FString::Printf(TEXT("LOD index %d out of range (mesh has %d LODs)"),
				LODIndex, Mesh->GetNumLODs()));

	auto Tx = BeginTransaction(TEXT("Bridge: set_lod_screen_size"));
	Mesh->Modify();
	Mesh->GetSourceModel(LODIndex).ScreenSize.Default = (float)FMath::Clamp(ScreenSize, 0.0, 1.0);
	Mesh->MarkPackageDirty();
	Mesh->PostEditChange();

	return MakeSuccess(DOMAIN, TEXT("set_lod_screen_size"),
		FString::Printf(TEXT("LOD %d screen size set to %.4f on '%s'"),
			LODIndex, ScreenSize, *AssetPath));
}

// ---------------------------------------------------------------------------
// set_ray_tracing
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetRayTracing(TSharedPtr<FJsonObject> Params)
{
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Enable")))
		V->Set(bEnabled ? 1 : 0, ECVF_SetByCode);

	return MakeSuccess(DOMAIN, TEXT("set_ray_tracing"),
		FString::Printf(TEXT("Hardware ray tracing %s"), bEnabled ? TEXT("enabled") : TEXT("disabled")));
}

// ---------------------------------------------------------------------------
// set_anti_aliasing
// ---------------------------------------------------------------------------

FBridgeResult URenderingHandler::Action_SetAntiAliasing(TSharedPtr<FJsonObject> Params)
{
	FString Method;
	if (!Params->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_anti_aliasing"), 1000,
			TEXT("'method' is required: None|FXAA|TAA|MSAA|TSR"));

	// r.AntiAliasingMethod: 0=None, 1=FXAA, 2=TAA, 3=MSAA, 4=TSR
	int32 MethodValue = -1;
	if      (Method.Equals(TEXT("None"), ESearchCase::IgnoreCase)) MethodValue = 0;
	else if (Method.Equals(TEXT("FXAA"), ESearchCase::IgnoreCase)) MethodValue = 1;
	else if (Method.Equals(TEXT("TAA"),  ESearchCase::IgnoreCase)) MethodValue = 2;
	else if (Method.Equals(TEXT("MSAA"), ESearchCase::IgnoreCase)) MethodValue = 3;
	else if (Method.Equals(TEXT("TSR"),  ESearchCase::IgnoreCase)) MethodValue = 4;

	if (MethodValue < 0)
		return MakeError(DOMAIN, TEXT("set_anti_aliasing"), 1000,
			FString::Printf(TEXT("Unknown method '%s'. Valid: None|FXAA|TAA|MSAA|TSR"), *Method));

	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod")))
		V->Set(MethodValue, ECVF_SetByCode);

	return MakeSuccess(DOMAIN, TEXT("set_anti_aliasing"),
		FString::Printf(TEXT("Anti-aliasing set to '%s' (%d)"), *Method, MethodValue));
}

// ---------------------------------------------------------------------------
// set_nanite_settings  (Phase 1 gap)
// Params: asset_path (StaticMesh), enabled (bool),
//         max_wpo_displacement (float, optional), allow_masked_materials (bool, optional),
//         max_pixels_per_edge (float, optional)
// Supplements set_nanite with fine-grained per-mesh Nanite controls.
// ---------------------------------------------------------------------------
FBridgeResult URenderingHandler::Action_SetNaniteSettings(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_nanite_settings"), 1000, TEXT("'asset_path' is required (StaticMesh content path)"));

	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Mesh)
		return MakeError(DOMAIN, TEXT("set_nanite_settings"), 2001,
			FString::Printf(TEXT("StaticMesh not found: '%s'"), *AssetPath));

	auto Tx = BeginTransaction(TEXT("Bridge: set_nanite_settings"));
	Mesh->Modify();

	FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();

	bool bEnabled = NaniteSettings.bEnabled;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);
	NaniteSettings.bEnabled = bEnabled;

	// Note: MaxWPODisplacement and MaxPixelsPerEdge are not members of
	// FMeshNaniteSettings in UE 5.7. Incoming params for these fields are
	// accepted for backward compatibility but ignored here.
	double MaxWPO = 0.0;
	Params->TryGetNumberField(TEXT("max_wpo_displacement"), MaxWPO);

	double MaxPPE = 0.0;
	Params->TryGetNumberField(TEXT("max_pixels_per_edge"), MaxPPE);

	Mesh->SetNaniteSettings(NaniteSettings);

	// allow_masked_materials — r.Nanite.AllowMaskedMaterials CVar (project-wide)
	bool bAllowMasked = false;
	if (Params->TryGetBoolField(TEXT("allow_masked_materials"), bAllowMasked))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.AllowMaskedMaterials")))
			V->Set(bAllowMasked ? 1 : 0, ECVF_SetByCode);
	}

	Mesh->MarkPackageDirty();
	Mesh->PostEditChange();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("nanite_enabled"), bEnabled);
	Data->SetStringField(TEXT("max_wpo_displacement"),
		TEXT("unsupported_in_ue5.7_FMeshNaniteSettings"));
	Data->SetStringField(TEXT("max_pixels_per_edge"),
		TEXT("unsupported_in_ue5.7_FMeshNaniteSettings"));

	return MakeSuccess(DOMAIN, TEXT("set_nanite_settings"),
		FString::Printf(TEXT("Nanite settings updated on '%s'"), *AssetPath), Data);
}

// ---------------------------------------------------------------------------
// set_megalights  (Phase 1e gap — UE 5.7 Beta)
// Params: enabled (bool)
// Controls r.MegaLights.Enable CVar — Lumen MegaLights feature.
// ---------------------------------------------------------------------------
FBridgeResult URenderingHandler::Action_SetMegaLights(TSharedPtr<FJsonObject> Params)
{
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.Enable")))
	{
		V->Set(bEnabled ? 1 : 0, ECVF_SetByCode);
		return MakeSuccess(DOMAIN, TEXT("set_megalights"),
			FString::Printf(TEXT("MegaLights %s (r.MegaLights.Enable=%d)"),
				bEnabled ? TEXT("enabled") : TEXT("disabled"), bEnabled ? 1 : 0));
	}

	// CVar not found — UE version without MegaLights or feature not compiled in
	return MakeError(DOMAIN, TEXT("set_megalights"), 3003,
		TEXT("r.MegaLights.Enable CVar not found — MegaLights requires UE 5.7 with the feature enabled in the project"),
		TEXT("Ensure r.MegaLights is available: check DefaultEngine.ini or upgrade to UE 5.7"));
}

// ---------------------------------------------------------------------------
// set_smaa  (Phase 1 gap — r.AntiAliasingMethod=5 + quality/edge CVars)
// Params: quality (int 0-3, optional), edge_mode (Luma|Color|Depth, optional)
// Note: set_anti_aliasing only covers methods 0-4; SMAA is method 5.
// ---------------------------------------------------------------------------
FBridgeResult URenderingHandler::Action_SetSMAA(TSharedPtr<FJsonObject> Params)
{
	// Force AA method to 5 (SMAA)
	if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod")))
		V->Set(5, ECVF_SetByCode);

	double Quality = -1.0;
	if (Params->TryGetNumberField(TEXT("quality"), Quality))
	{
		int32 Q = FMath::Clamp((int32)Quality, 0, 3);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SMAA.Quality")))
			V->Set(Q, ECVF_SetByCode);
	}

	FString EdgeMode;
	if (Params->TryGetStringField(TEXT("edge_mode"), EdgeMode) && !EdgeMode.IsEmpty())
	{
		int32 ModeVal = 0;
		if      (EdgeMode == TEXT("Luma"))  ModeVal = 0;
		else if (EdgeMode == TEXT("Color")) ModeVal = 1;
		else if (EdgeMode == TEXT("Depth")) ModeVal = 2;
		else return MakeError(DOMAIN, TEXT("set_smaa"), 1001,
			FString::Printf(TEXT("Invalid edge_mode '%s'. Valid: Luma|Color|Depth"), *EdgeMode));

		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SMAA.EdgeMode")))
			V->Set(ModeVal, ECVF_SetByCode);
	}

	return MakeSuccess(DOMAIN, TEXT("set_smaa"),
		FString::Printf(TEXT("SMAA enabled (r.AntiAliasingMethod=5)%s%s"),
			Quality >= 0.0 ? *FString::Printf(TEXT(", quality=%d"), (int32)Quality) : TEXT(""),
			!EdgeMode.IsEmpty() ? *FString::Printf(TEXT(", edge_mode=%s"), *EdgeMode) : TEXT("")));
}

// ---------------------------------------------------------------------------
// set_affect_dynamic_lighting  (Phase 1e gap)
// Params: actor_name (string), enabled (bool), component_name (string, optional)
// Sets UMeshComponent::bAffectDynamicIndirectLighting on matching mesh components.
// Useful per-mesh Lumen performance toggle.
// ---------------------------------------------------------------------------
FBridgeResult URenderingHandler::Action_SetAffectDynamicLighting(TSharedPtr<FJsonObject> Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_affect_dynamic_lighting"), 1000,
			TEXT("'actor_name' is required (actor label in the current level)"));

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	FString ComponentName;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);

	UWorld* World = GetEditorWorld();
	if (!World)
		return MakeError(DOMAIN, TEXT("set_affect_dynamic_lighting"), 2000,
			TEXT("No editor world available"));

	AActor* TargetActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
		{
			TargetActor = *It;
			break;
		}
	}
	if (!TargetActor)
		return MakeError(DOMAIN, TEXT("set_affect_dynamic_lighting"), 2001,
			FString::Printf(TEXT("Actor '%s' not found in current level"), *ActorName));

	int32 ModifiedCount = 0;
	TargetActor->Modify();
	for (UActorComponent* Comp : TargetActor->GetComponents())
	{
		UMeshComponent* Mesh = Cast<UMeshComponent>(Comp);
		if (!Mesh) continue;
		if (!ComponentName.IsEmpty() && !Comp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) continue;

		Mesh->bAffectDynamicIndirectLighting = bEnabled;
		Mesh->MarkRenderStateDirty();
		++ModifiedCount;
	}

	if (ModifiedCount == 0)
		return MakeError(DOMAIN, TEXT("set_affect_dynamic_lighting"), 2002,
			ComponentName.IsEmpty()
				? FString::Printf(TEXT("No UMeshComponent found on actor '%s'"), *ActorName)
				: FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *ActorName));

	return MakeSuccess(DOMAIN, TEXT("set_affect_dynamic_lighting"),
		FString::Printf(TEXT("Set bAffectDynamicIndirectLighting=%s on %d component(s) of actor '%s'"),
			bEnabled ? TEXT("true") : TEXT("false"), ModifiedCount, *ActorName));
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> URenderingHandler::GetActionSchemas() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	auto MakeParam = [](const FString& Type, bool bReq, const FString& Desc) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetBoolField(TEXT("required"), bReq);
		P->SetStringField(TEXT("desc"), Desc);
		return P;
	};

	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"), MakeParam(TEXT("string"), true,  TEXT("Full package path to the StaticMesh")));
		Pm->SetObjectField(TEXT("enabled"),    MakeParam(TEXT("bool"),   false, TEXT("Enable or disable Nanite (default true)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Enable or disable Nanite on a StaticMesh asset"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_nanite"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("enabled"), MakeParam(TEXT("bool"), false, TEXT("Enable or disable Lumen GI + reflections (default true)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Toggle Lumen via r.DynamicGlobalIllumination.Method + r.ReflectionMethod"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_lumen"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("method"), MakeParam(TEXT("string"), true, TEXT("VSM|Cascaded|RayTraced")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the shadow rendering method (Virtual Shadow Maps, cascaded, or ray-traced)"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_shadow_method"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Mark all AReflectionCapture actors dirty and trigger UpdateReflectionCaptureContents"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("capture_reflection"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("actor_name"),   MakeParam(TEXT("string"), false, TEXT("Optional: name of specific APostProcessVolume")));
		Pm->SetObjectField(TEXT("blend_weight"), MakeParam(TEXT("float"),  false, TEXT("0.0-1.0")));
		Pm->SetObjectField(TEXT("unbound"),      MakeParam(TEXT("bool"),   false, TEXT("Apply globally, not inside volume")));
		Pm->SetObjectField(TEXT("priority"),     MakeParam(TEXT("float"),  false, TEXT("Blend priority (higher wins)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set blend_weight, unbound, and priority on a PostProcessVolume"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_post_process_blend"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("var_name"), MakeParam(TEXT("string"), true,  TEXT("Console variable name e.g. r.ScreenPercentage")));
		Pm->SetObjectField(TEXT("value"),    MakeParam(TEXT("string"), true,  TEXT("Value as string")));
		Pm->SetObjectField(TEXT("persist"),  MakeParam(TEXT("bool"),   false, TEXT("Write to DefaultEngine.ini [SystemSettings]")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set any IConsoleVariable by name"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_console_var"), A);
	}
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Return current Nanite/Lumen/VSM/AA/RT/PP state as structured JSON"));
		A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("get_render_stats"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("asset_path"),  MakeParam(TEXT("string"), true,  TEXT("Full package path to the StaticMesh")));
		Pm->SetObjectField(TEXT("lod_index"),   MakeParam(TEXT("int"),    false, TEXT("LOD index (default 0)")));
		Pm->SetObjectField(TEXT("screen_size"), MakeParam(TEXT("float"),  true,  TEXT("Screen size threshold 0.0-1.0")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set the screen size threshold at which a LOD becomes active"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_lod_screen_size"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("enabled"), MakeParam(TEXT("bool"), false, TEXT("Enable or disable hardware ray tracing (default true)")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Toggle hardware ray tracing via r.RayTracing.Enable"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_ray_tracing"), A);
	}
	{
		TSharedPtr<FJsonObject> Pm = MakeShared<FJsonObject>();
		Pm->SetObjectField(TEXT("method"), MakeParam(TEXT("string"), true, TEXT("None|FXAA|TAA|MSAA|TSR")));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set anti-aliasing method via r.AntiAliasingMethod"));
		A->SetObjectField(TEXT("params"), Pm);
		Root->SetObjectField(TEXT("set_anti_aliasing"), A);
	}

	return Root;
}
