#include "Handlers/NiagaraHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- Niagara runtime -------------------------------------------------------
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraTypes.h"
// #include "NiagaraVariable.h"  // UE 5.7: FNiagaraVariable now in NiagaraTypes.h

// ---- Niagara editor --------------------------------------------------------
// UNiagaraSystemFactory: verify header path at compile time.
// In UE 5.7 the factory lives in NiagaraEditor module.
// #include "Factories/NiagaraSystemFactory.h"  // UE 5.7: removed — using nullptr factory
#include "NiagaraEditorUtilities.h"              // FNiagaraEditorUtilities
// #include "NiagaraSystemUpdateContext.h"  // UE 5.7: header removed — update context stubbed

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- UObject reflection (FindFProperty, FindFirstObject) -------------------
#include "UObject/UObjectGlobals.h"

// ---- Capture ---------------------------------------------------------------
#include "Capture/ForgeNiagaraCapture.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UNiagaraHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("niagara"), Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_niagara_system"))  return Action_CreateNiagaraSystem(Params);
	if (Action == TEXT("add_emitter"))            return Action_AddEmitter(Params);
	if (Action == TEXT("set_user_parameter"))     return Action_SetUserParameter(Params);
	if (Action == TEXT("compile_niagara_system")) return Action_CompileNiagaraSystem(Params);
	if (Action == TEXT("get_parameters"))        return Action_GetParameters(Params);
	if (Action == TEXT("set_parameter_default")) return Action_SetParameterDefault(Params);
	if (Action == TEXT("read_niagara_capture"))  return Action_ReadNiagaraCapture(Params);
	if (Action == TEXT("update_instances"))      return Action_UpdateInstances(Params);
	if (Action == TEXT("add_module"))            return Action_AddModule(Params);
	if (Action == TEXT("remove_module"))         return Action_RemoveModule(Params);
	if (Action == TEXT("set_module_property"))   return Action_SetModuleProperty(Params);
	if (Action == TEXT("add_renderer"))          return Action_AddRenderer(Params);
	if (Action == TEXT("set_renderer_property")) return Action_SetRendererProperty(Params);
	// Phase 1b aliases (snake_case shortcuts for existing actions)
	if (Action == TEXT("create_system"))  return Action_CreateNiagaraSystem(Params);
	if (Action == TEXT("set_parameter"))  return Action_SetUserParameter(Params);
	if (Action == TEXT("compile"))        return Action_CompileNiagaraSystem(Params);
	// Phase 1b new actions
	if (Action == TEXT("set_gpu_sim"))           return Action_SetGPUSim(Params);
	if (Action == TEXT("bind_parameter"))        return Action_BindParameter(Params);
	if (Action == TEXT("set_particle_lights"))   return Action_SetParticleLights(Params);
	if (Action == TEXT("set_collision_response"))return Action_SetCollisionResponse(Params);
	if (Action == TEXT("list_emitters"))         return Action_ListEmitters(Params);
	if (Action == TEXT("set_emitter_enabled"))   return Action_SetEmitterEnabled(Params);
	if (Action == TEXT("remove_emitter"))        return Action_RemoveEmitter(Params);
	if (Action == TEXT("remove_renderer"))       return Action_RemoveRenderer(Params);

	FBridgeResult R = CreateResult(TEXT("niagara"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown niagara action '%s'. Valid: create_niagara_system, add_emitter, "
		     "set_user_parameter, compile_niagara_system, get_parameters, set_parameter_default, "
		     "read_niagara_capture, update_instances, add_module, remove_module, "
		     "set_module_property, add_renderer, set_renderer_property, "
		     "create_system, set_parameter, compile, set_gpu_sim, bind_parameter, "
		     "set_particle_lights, set_collision_response, list_emitters, set_emitter_enabled"),
		*Action);
	return R;
}

// ---------------------------------------------------------------------------
// create_niagara_system
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_CreateNiagaraSystem(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("create_niagara_system"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_niagara_system: 'asset_path' is required (e.g. '/Game/FX/NS_Fire')");
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	// SAFETY: Creating an empty Niagara system (0 emitters) crashes the editor.
	// Require an emitter_path param so the system is created with at least one emitter.
	FString EmitterPath;
	if (!Params->TryGetStringField(TEXT("emitter_path"), EmitterPath) || EmitterPath.IsEmpty())
	{
		Result.ErrorCode = 1000;
		Result.Message = TEXT("create_niagara_system: 'emitter_path' is required. "
			"An empty Niagara system crashes the editor. Provide an emitter template, e.g. "
			"'/Niagara/DefaultAssets/Templates/FountainEmitter'");
		Result.RecoveryHint = TEXT("Provide emitter_path with a valid UNiagaraEmitter asset path");
		return Result;
	}

	// Verify emitter exists before creating system
	UNiagaraEmitter* TemplateEmitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
	if (!TemplateEmitter)
	{
		const FString Suffix = EmitterPath + TEXT(".") + FPackageName::GetLongPackageAssetName(EmitterPath);
		TemplateEmitter = LoadObject<UNiagaraEmitter>(nullptr, *Suffix);
	}
	if (!TemplateEmitter)
	{
		Result.ErrorCode = 2000;
		Result.Message = FString::Printf(TEXT("Emitter not found: '%s'"), *EmitterPath);
		Result.RecoveryHint = TEXT("Use a valid UNiagaraEmitter asset path");
		return Result;
	}

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UNiagaraSystem::StaticClass(), nullptr);
	if (!CreatedAsset)
	{
		Result.ErrorCode = 3000;
		Result.Message = FString::Printf(
			TEXT("create_niagara_system: failed to create asset at '%s' "
			     "(path may already exist or be invalid)"),
			*AssetPath);
		return Result;
	}

	UNiagaraSystem* NewSystem = CastChecked<UNiagaraSystem>(CreatedAsset);

	// Add the emitter to avoid empty system crash
	// UE 5.7 signature: AddEmitterToSystem(System, Emitter, EmitterVersion, bCreateCopy)
	FNiagaraEditorUtilities::AddEmitterToSystem(*NewSystem, *TemplateEmitter, TemplateEmitter->GetExposedVersion().VersionGuid);

	NewSystem->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("NiagaraSystem created at %s with emitter '%s'"),
		*AssetPath, *TemplateEmitter->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// add_emitter
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_AddEmitter(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("add_emitter"));

	FString AssetPath, EmitterPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("emitter_path"), EmitterPath) || EmitterPath.IsEmpty())
	{
		Result.Message = TEXT("add_emitter: 'asset_path' and 'emitter_path' are required");
		return Result;
	}

	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, Result);
	if (!System) return Result;

	UNiagaraEmitter* SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
	if (!SourceEmitter)
	{
		// Try with asset name suffix
		const FString Suffix = EmitterPath + TEXT(".") + FPackageName::GetLongPackageAssetName(EmitterPath);
		SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *Suffix);
	}
	if (!SourceEmitter)
	{
		Result.Message = FString::Printf(
			TEXT("add_emitter: UNiagaraEmitter not found at '%s'"), *EmitterPath);
		return Result;
	}

	// UE 5.3+ versioned emitter model — pass explicit ExposedVersion GUID to match create_niagara_system.
	FNiagaraEditorUtilities::AddEmitterToSystem(*System, *SourceEmitter, SourceEmitter->GetExposedVersion().VersionGuid);

	System->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Emitter '%s' added to NiagaraSystem '%s'"), *EmitterPath, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_user_parameter
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetUserParameter(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("set_user_parameter"));

	FString AssetPath, ParamName, Value, ValueType;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("param_name"),  ParamName)  || ParamName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("value"),       Value)       || Value.IsEmpty()      ||
	    !Params->TryGetStringField(TEXT("value_type"),  ValueType)   || ValueType.IsEmpty())
	{
		Result.Message = TEXT("set_user_parameter: 'asset_path', 'param_name', 'value', "
		                      "and 'value_type' (float|int|bool|vector|color) are required");
		return Result;
	}

	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, Result);
	if (!System) return Result;

	// Niagara user parameters are scoped under "User." namespace.
	// Accept both "MyParam" and "User.MyParam" forms from callers.
	FString FullParamName = ParamName;
	if (!FullParamName.StartsWith(TEXT("User.")))
	{
		FullParamName = TEXT("User.") + ParamName;
	}
	const FName ParamFName(*FullParamName);

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

	bool bSet = false;

	if (ValueType == TEXT("float"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetFloatDef(), ParamFName);
		float FloatVal = FCString::Atof(*Value);
		Store.SetParameterValue(FloatVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("int"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), ParamFName);
		int32 IntVal = FCString::Atoi(*Value);
		Store.SetParameterValue(IntVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("bool"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetBoolDef(), ParamFName);
		// FNiagaraBool wraps a bool; true if value string is "true"/"1"/"True"
		FNiagaraBool BoolVal;
		BoolVal.SetValue(Value.ToBool());
		Store.SetParameterValue(BoolVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("vector"))
	{
		// Expect "X,Y,Z" format
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetVec3Def(), ParamFName);
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		FVector3f Vec3(0.f, 0.f, 0.f);
		if (Parts.Num() >= 1) Vec3.X = FCString::Atof(*Parts[0]);
		if (Parts.Num() >= 2) Vec3.Y = FCString::Atof(*Parts[1]);
		if (Parts.Num() >= 3) Vec3.Z = FCString::Atof(*Parts[2]);
		Store.SetParameterValue(Vec3, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("color"))
	{
		// Expect "R,G,B,A" format (linear floats)
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetColorDef(), ParamFName);
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		FLinearColor Color(0.f, 0.f, 0.f, 1.f);
		if (Parts.Num() >= 1) Color.R = FCString::Atof(*Parts[0]);
		if (Parts.Num() >= 2) Color.G = FCString::Atof(*Parts[1]);
		if (Parts.Num() >= 3) Color.B = FCString::Atof(*Parts[2]);
		if (Parts.Num() >= 4) Color.A = FCString::Atof(*Parts[3]);
		Store.SetParameterValue(Color, Var, /*bAdd=*/true);
		bSet = true;
	}
	else
	{
		Result.Message = FString::Printf(
			TEXT("set_user_parameter: unsupported value_type '%s'. Use: float, int, bool, vector, color"),
			*ValueType);
		return Result;
	}

	if (bSet)
	{
		System->MarkPackageDirty();
		Result.bSuccess     = true;
		Result.AffectedPath = AssetPath;
		Result.Message      = FString::Printf(
			TEXT("Set user parameter '%s' = '%s' (%s) on '%s'"),
			*FullParamName, *Value, *ValueType, *AssetPath);
	}
	return Result;
}

// ---------------------------------------------------------------------------
// compile_niagara_system
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_CompileNiagaraSystem(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("compile_niagara_system"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("compile_niagara_system: 'asset_path' is required");
		return Result;
	}

	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, Result);
	if (!System) return Result;

	// RequestCompile marks the system dirty for recompilation.
	System->RequestCompile(/*bForce=*/false);
	// UE 5.7: FNiagaraSystemUpdateContext header removed — active instance propagation stubbed
	System->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("NiagaraSystem compile requested: %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_parameters
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_GetParameters(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("get_parameters"),
			1000, TEXT("get_parameters: 'asset_path' is required"),
			TEXT("Provide the content path of a UNiagaraSystem asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("get_parameters"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System)
	{
		return MakeError(TEXT("niagara"), TEXT("get_parameters"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UNiagaraSystem"));
	}

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	TArray<FNiagaraVariable> UserParams;
	Store.GetParameters(UserParams);

	TArray<TSharedPtr<FJsonValue>> ParamArray;
	for (const FNiagaraVariable& Var : UserParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Var.GetName().ToString());
		ParamObj->SetStringField(TEXT("type"), Var.GetType().GetName());
		ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("parameters"), ParamArray);
	Data->SetNumberField(TEXT("count"), ParamArray.Num());

	return MakeSuccess(TEXT("niagara"), TEXT("get_parameters"),
		FString::Printf(TEXT("Found %d user parameter(s) on '%s'"), ParamArray.Num(), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_parameter_default
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetParameterDefault(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ParamName, Value, ValueType;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("param_name"),  ParamName)  || ParamName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("value"),       Value)       || Value.IsEmpty()      ||
	    !Params->TryGetStringField(TEXT("value_type"),  ValueType)   || ValueType.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_parameter_default"),
			1000, TEXT("set_parameter_default: 'asset_path', 'param_name', 'value', and 'value_type' are required"),
			TEXT("value_type must be one of: float, int, bool, vector, color"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("set_parameter_default"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System)
	{
		return MakeError(TEXT("niagara"), TEXT("set_parameter_default"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UNiagaraSystem"));
	}

	// Niagara user parameters are scoped under "User." namespace
	FString FullParamName = ParamName;
	if (!FullParamName.StartsWith(TEXT("User.")))
	{
		FullParamName = TEXT("User.") + ParamName;
	}
	const FName ParamFName(*FullParamName);

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

	bool bSet = false;

	if (ValueType == TEXT("float"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetFloatDef(), ParamFName);
		float FloatVal = FCString::Atof(*Value);
		Store.SetParameterValue(FloatVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("int"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), ParamFName);
		int32 IntVal = FCString::Atoi(*Value);
		Store.SetParameterValue(IntVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("bool"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetBoolDef(), ParamFName);
		FNiagaraBool BoolVal;
		BoolVal.SetValue(Value.ToBool());
		Store.SetParameterValue(BoolVal, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("vector"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetVec3Def(), ParamFName);
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		FVector3f Vec3(0.f, 0.f, 0.f);
		if (Parts.Num() >= 1) Vec3.X = FCString::Atof(*Parts[0]);
		if (Parts.Num() >= 2) Vec3.Y = FCString::Atof(*Parts[1]);
		if (Parts.Num() >= 3) Vec3.Z = FCString::Atof(*Parts[2]);
		Store.SetParameterValue(Vec3, Var, /*bAdd=*/true);
		bSet = true;
	}
	else if (ValueType == TEXT("color"))
	{
		FNiagaraVariable Var(FNiagaraTypeDefinition::GetColorDef(), ParamFName);
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		FLinearColor Color(0.f, 0.f, 0.f, 1.f);
		if (Parts.Num() >= 1) Color.R = FCString::Atof(*Parts[0]);
		if (Parts.Num() >= 2) Color.G = FCString::Atof(*Parts[1]);
		if (Parts.Num() >= 3) Color.B = FCString::Atof(*Parts[2]);
		if (Parts.Num() >= 4) Color.A = FCString::Atof(*Parts[3]);
		Store.SetParameterValue(Color, Var, /*bAdd=*/true);
		bSet = true;
	}
	else
	{
		return MakeError(TEXT("niagara"), TEXT("set_parameter_default"),
			1001, FString::Printf(TEXT("Unsupported value_type '%s'"), *ValueType),
			TEXT("Use one of: float, int, bool, vector, color"));
	}

	if (bSet)
	{
		System->MarkPackageDirty();
		return MakeSuccess(TEXT("niagara"), TEXT("set_parameter_default"),
			FString::Printf(TEXT("Set parameter default '%s' = '%s' (%s) on '%s'"),
				*FullParamName, *Value, *ValueType, *AssetPath));
	}

	return MakeError(TEXT("niagara"), TEXT("set_parameter_default"),
		3000, TEXT("Failed to set parameter default value"),
		TEXT("Check parameter name and value type"));
}

// ---------------------------------------------------------------------------
// read_niagara_capture
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_ReadNiagaraCapture(TSharedPtr<FJsonObject> Params)
{
	if (!Subsystem || !Subsystem->NiagaraCapture)
		return MakeError(TEXT("niagara"), TEXT("read_niagara_capture"),
			2000, TEXT("NiagaraCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

	FString AssetPath;
	const bool bHasAsset = Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();

	if (bHasAsset)
	{
		Subsystem->NiagaraCapture->ExportNiagaraSystem(AssetPath);

		FString Segment = AssetPath;
		int32 SlashIdx;
		if (Segment.FindLastChar(TEXT('/'), SlashIdx))
			Segment = Segment.RightChop(SlashIdx + 1);

		FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("niagara"), Segment + TEXT(".json"));
		FString FileContent;
		FBridgeResult Res = MakeSuccess(TEXT("niagara"), TEXT("read_niagara_capture"),
			TEXT("Capture complete: ") + FilePath);
		if (FFileHelper::LoadFileToString(FileContent, *FilePath))
		{
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
				Res.Data = JsonObj;
		}
		return Res;
	}
	else
	{
		int32 Count = Subsystem->NiagaraCapture->ExportAllNiagaraSystems();
		return MakeSuccess(TEXT("niagara"), TEXT("read_niagara_capture"),
			FString::Printf(TEXT("Exported all %d Niagara systems"), Count));
	}
}

// ---------------------------------------------------------------------------
// update_instances
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_UpdateInstances(TSharedPtr<FJsonObject> Params)
{
	// FNiagaraSystemUpdateContext was removed in UE 5.7.
	// Best available substitute: capture current pool state via NiagaraCapture.
	if (!Subsystem || !Subsystem->NiagaraCapture)
		return MakeError(TEXT("niagara"), TEXT("update_instances"),
			2000, TEXT("NiagaraCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

	Subsystem->NiagaraCapture->CaptureNiagaraPoolState();
	return MakeSuccess(TEXT("niagara"), TEXT("update_instances"),
		TEXT("Niagara pool state captured — use read_niagara_capture to inspect"));
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

UNiagaraSystem* UNiagaraHandler::LoadNiagaraSystem(const FString& AssetPath, FBridgeResult& Result)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		System = LoadObject<UNiagaraSystem>(nullptr, *Suffix);
	}
	if (!System)
	{
		Result.Message = FString::Printf(
			TEXT("LoadNiagaraSystem: no UNiagaraSystem found at '%s'"), *AssetPath);
	}
	return System;
}

int32 UNiagaraHandler::FindEmitterHandleIndex(UNiagaraSystem* System, const FString& EmitterName) const
{
	if (!System) return -1;

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	// Pass 1: exact match on GetName()
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetName().ToString() == EmitterName)
			return i;
	}
	// Pass 1b: case-insensitive equality on GetName()
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
			return i;
	}

	// Pass 2: case-insensitive substring match on GetUniqueInstanceName(). Ambiguous if >1 hit.
	int32 FoundIdx = -1;
	int32 MatchCount = 0;
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetUniqueInstanceName().Contains(EmitterName, ESearchCase::IgnoreCase))
		{
			FoundIdx = i;
			++MatchCount;
		}
	}
	if (MatchCount == 1) return FoundIdx;
	if (MatchCount > 1)  return -2;
	return -1;
}

// ---------------------------------------------------------------------------
// Niagara module / renderer actions
// UE 5.7: Emitter graph internals (stages, module scripts, renderer properties)
// are not directly accessible via public C++ API in the editor module.
// These actions are dispatched through the Python scripting plugin, which exposes
// limited but functional access via unreal.NiagaraEditorSubsystem.
// ---------------------------------------------------------------------------

static void NiagaraExecPython(const FString& Script)
{
#if WITH_EDITOR
	if (GEngine)
	{
		FString PyCmd = FString::Printf(TEXT("py %s"), *Script);
		GEngine->Exec(UBridgeHandlerBase::GetSafeEditorWorld(), *PyCmd);
	}
#endif
}

// ---------------------------------------------------------------------------
// add_module
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_AddModule(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName, Stage, ModulePath;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("stage"),        Stage)       || Stage.IsEmpty()       ||
	    !Params->TryGetStringField(TEXT("module_path"),  ModulePath)  || ModulePath.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("add_module"), 1000,
			TEXT("add_module: 'asset_path', 'emitter_name', 'stage', and 'module_path' are required"),
			TEXT("stage must be Spawn, Update, or Render"));
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "sys = unreal.load_asset('%s'); "
		     "sub = unreal.get_editor_subsystem(unreal.NiagaraSystemEditorSubsystem) if hasattr(unreal, 'NiagaraSystemEditorSubsystem') else None; "
		     "print(f'add_module: system={sys}, subsystem={sub}, emitter=%s, stage=%s, module=%s')"),
		*AssetPath, *EmitterName, *Stage, *ModulePath);

	NiagaraExecPython(PyScript);

	return MakeError(TEXT("niagara"), TEXT("add_module"), 3003,
		TEXT("add_module: NiagaraEditor module API not accessible from bridge context. Use the Niagara editor directly or Script Python console."),
		TEXT("Open the Niagara system, switch to Graph mode, and use the UI to add/remove modules."));
}

// ---------------------------------------------------------------------------
// remove_module
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_RemoveModule(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName, Stage, ModulePath;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("stage"),        Stage)       || Stage.IsEmpty()       ||
	    !Params->TryGetStringField(TEXT("module_path"),  ModulePath)  || ModulePath.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("remove_module"), 1000,
			TEXT("remove_module: 'asset_path', 'emitter_name', 'stage', and 'module_path' are required"));
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "sys = unreal.load_asset('%s'); "
		     "print(f'remove_module: system={sys}, emitter=%s, stage=%s, module=%s — use Niagara editor to remove modules directly')"),
		*AssetPath, *EmitterName, *Stage, *ModulePath);

	NiagaraExecPython(PyScript);

	return MakeError(TEXT("niagara"), TEXT("remove_module"), 3003,
		TEXT("remove_module: NiagaraEditor module API not accessible from bridge context. Use the Niagara editor directly or Script Python console."),
		TEXT("Open the Niagara system, switch to Graph mode, and use the UI to add/remove modules."));
}

// ---------------------------------------------------------------------------
// set_module_property
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetModuleProperty(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName, ModulePath, ParamName, Value;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("module_path"),  ModulePath)  || ModulePath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("param_name"),   ParamName)   || ParamName.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("value"),        Value)       || Value.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_module_property"), 1000,
			TEXT("set_module_property: 'asset_path', 'emitter_name', 'module_path', 'param_name', and 'value' are required"));
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "sys = unreal.load_asset('%s'); "
		     "print(f'set_module_property: system={sys}, emitter=%s, module=%s, param=%s, value=%s')"),
		*AssetPath, *EmitterName, *ModulePath, *ParamName, *Value);

	NiagaraExecPython(PyScript);

	return MakeError(TEXT("niagara"), TEXT("set_module_property"), 3003,
		TEXT("set_module_property: NiagaraEditor module API not accessible from bridge context. Use the Niagara editor directly or Script Python console."),
		TEXT("Open the Niagara system, switch to Graph mode, and use the UI to add/remove modules."));
}

// ---------------------------------------------------------------------------
// add_renderer
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_AddRenderer(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName, RendererType;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("emitter_name"),  EmitterName)  || EmitterName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("renderer_type"), RendererType) || RendererType.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("add_renderer"), 1000,
			TEXT("add_renderer: 'asset_path', 'emitter_name', and 'renderer_type' (Sprite|Mesh|Ribbon|Light|Decal) are required"));
	}

	// Map type string to renderer class name for Python dispatch
	FString RendererClass;
	if      (RendererType == TEXT("Sprite"))  RendererClass = TEXT("NiagaraSpriteRendererProperties");
	else if (RendererType == TEXT("Mesh"))    RendererClass = TEXT("NiagaraMeshRendererProperties");
	else if (RendererType == TEXT("Ribbon"))  RendererClass = TEXT("NiagaraRibbonRendererProperties");
	else if (RendererType == TEXT("Light"))   RendererClass = TEXT("NiagaraLightRendererProperties");
	else if (RendererType == TEXT("Decal"))   RendererClass = TEXT("NiagaraDecalRendererProperties");
	else
	{
		return MakeError(TEXT("niagara"), TEXT("add_renderer"), 1001,
			FString::Printf(TEXT("Unknown renderer_type '%s'"), *RendererType),
			TEXT("Valid: Sprite, Mesh, Ribbon, Light, Decal"));
	}

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "sys_asset = unreal.load_asset('%s'); "
		     "renderer_class = unreal.%s if hasattr(unreal, '%s') else None; "
		     "print(f'add_renderer: system={sys_asset}, emitter=%s, renderer_class={renderer_class}')"),
		*AssetPath, *RendererClass, *RendererClass, *EmitterName);

	NiagaraExecPython(PyScript);

	return MakeError(TEXT("niagara"), TEXT("add_renderer"), 3003,
		TEXT("add_renderer: NiagaraEditor module API not accessible from bridge context. Use the Niagara editor directly or Script Python console."),
		TEXT("Open the Niagara system, switch to Graph mode, and use the UI to add/remove modules."));
}

// ---------------------------------------------------------------------------
// set_renderer_property
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetRendererProperty(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName, PropertyName, Value;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty()    ||
	    !Params->TryGetStringField(TEXT("emitter_name"),  EmitterName)  || EmitterName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("value"),         Value)        || Value.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_renderer_property"), 1000,
			TEXT("set_renderer_property: 'asset_path', 'emitter_name', 'property_name', and 'value' are required"),
			TEXT("Optional: 'renderer_index' (default 0)"));
	}

	double RendererIndex = 0.0;
	Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex);

	FString PyScript = FString::Printf(
		TEXT("import unreal; "
		     "sys_asset = unreal.load_asset('%s'); "
		     "print(f'set_renderer_property: system={sys_asset}, emitter=%s, renderer_index=%d, %s=%s')"),
		*AssetPath, *EmitterName, (int32)RendererIndex, *PropertyName, *Value);

	NiagaraExecPython(PyScript);

	// TODO (DEFERRED, post-0.2.6): unlike add_module / add_renderer, this one is
	// wireable without internal headers. UNiagaraRendererProperties UPROPERTYs
	// are reachable via reflection - mirror the FindFProperty + ImportText pattern
	// from Action_SetParticleLights (NiagaraHandler.cpp ~line 915) and key off
	// EmitterData->GetRenderers()[RendererIndex]. Estimated 4-8 hours of focused
	// work. Until then, the recipe and README list this action as DEFERRED.
	return MakeError(TEXT("niagara"), TEXT("set_renderer_property"), 3003,
		TEXT("set_renderer_property: NiagaraEditor module API not accessible from bridge context. Use the Niagara editor directly or Script Python console."),
		TEXT("Open the Niagara system, switch to Graph mode, and use the UI to add/remove modules."));
}

// ---------------------------------------------------------------------------
// set_gpu_sim  (Phase 1b)
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetGPUSim(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_gpu_sim"), 1000,
			TEXT("set_gpu_sim: 'asset_path' and 'emitter_name' are required"),
			TEXT("Optional: 'enabled' (bool, default true)"));
	}

	bool bGPU = true;
	Params->TryGetBoolField(TEXT("enabled"), bGPU);

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("set_gpu_sim"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System) return TempResult;

	const int32 Idx = FindEmitterHandleIndex(System, EmitterName);
	if (Idx == -2)
	{
		return MakeError(TEXT("niagara"), TEXT("set_gpu_sim"), 2003,
			FString::Printf(TEXT("Emitter name '%s' matched multiple emitters in '%s' (ambiguous substring)"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name and pass it unambiguously."));
	}
	if (Idx < 0)
	{
		return MakeError(TEXT("niagara"), TEXT("set_gpu_sim"), 2002,
			FString::Printf(TEXT("Emitter '%s' not found in '%s'"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name."));
	}

	FNiagaraEmitterHandle& Handle = const_cast<FNiagaraEmitterHandle&>(System->GetEmitterHandles()[Idx]);
	FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
	if (!Data)
	{
		return MakeError(TEXT("niagara"), TEXT("set_gpu_sim"), 3000,
			FString::Printf(TEXT("Emitter '%s' has no versioned emitter data"), *EmitterName));
	}

	Data->SimTarget = bGPU ? ENiagaraSimTarget::GPUComputeSim : ENiagaraSimTarget::CPUSim;
	System->MarkPackageDirty();

	return MakeSuccess(TEXT("niagara"), TEXT("set_gpu_sim"),
		FString::Printf(TEXT("Emitter '%s' sim target set to %s on '%s'"),
		                *EmitterName, bGPU ? TEXT("GPUComputeSim") : TEXT("CPUSim"), *AssetPath));
}

// ---------------------------------------------------------------------------
// bind_parameter  (Phase 1b)
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_BindParameter(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ParamName, ValueType;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("param_name"),  ParamName)  || ParamName.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("value_type"),  ValueType)  || ValueType.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("bind_parameter"), 1000,
			TEXT("bind_parameter: 'asset_path', 'param_name', and 'value_type' are required"),
			TEXT("value_type: float|int|bool|vector|color. Optional: 'value' (default value string)"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("bind_parameter"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System) return TempResult;

	// Normalize param name to User. namespace
	FString FullParamName = ParamName;
	if (!FullParamName.StartsWith(TEXT("User.")))
		FullParamName = TEXT("User.") + ParamName;
	const FName ParamFName(*FullParamName);

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

	// Determine type and add parameter (bAdd=true exposes it to the store / blueprints)
	bool bAdded = false;
	if      (ValueType == TEXT("float"))  { FNiagaraVariable V(FNiagaraTypeDefinition::GetFloatDef(),  ParamFName); Store.AddParameter(V, true); bAdded = true; }
	else if (ValueType == TEXT("int"))    { FNiagaraVariable V(FNiagaraTypeDefinition::GetIntDef(),    ParamFName); Store.AddParameter(V, true); bAdded = true; }
	else if (ValueType == TEXT("bool"))   { FNiagaraVariable V(FNiagaraTypeDefinition::GetBoolDef(),   ParamFName); Store.AddParameter(V, true); bAdded = true; }
	else if (ValueType == TEXT("vector")) { FNiagaraVariable V(FNiagaraTypeDefinition::GetVec3Def(),   ParamFName); Store.AddParameter(V, true); bAdded = true; }
	else if (ValueType == TEXT("color"))  { FNiagaraVariable V(FNiagaraTypeDefinition::GetColorDef(),  ParamFName); Store.AddParameter(V, true); bAdded = true; }
	else
	{
		return MakeError(TEXT("niagara"), TEXT("bind_parameter"), 1001,
			FString::Printf(TEXT("Unsupported value_type '%s'"), *ValueType),
			TEXT("Use: float, int, bool, vector, color"));
	}

	// Optionally set a default value if provided
	FString Value;
	if (bAdded && Params->TryGetStringField(TEXT("value"), Value) && !Value.IsEmpty())
	{
		TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetStringField(TEXT("asset_path"),  AssetPath);
		SetParams->SetStringField(TEXT("param_name"),  ParamName);
		SetParams->SetStringField(TEXT("value"),       Value);
		SetParams->SetStringField(TEXT("value_type"),  ValueType);
		Action_SetUserParameter(SetParams);
	}

	System->MarkPackageDirty();
	return MakeSuccess(TEXT("niagara"), TEXT("bind_parameter"),
		FString::Printf(TEXT("Parameter '%s' (%s) bound/exposed on '%s'"), *FullParamName, *ValueType, *AssetPath));
}

// ---------------------------------------------------------------------------
// set_particle_lights  (Phase 1b)
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetParticleLights(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 1000,
			TEXT("set_particle_lights: 'asset_path' and 'emitter_name' are required"),
			TEXT("Optional: 'radius_scale' (float). 'enabled' and 'mega_lights' are not UPROPERTY fields in UE 5.7."));
	}

	double RadiusScale = 1.0;
	const bool bHasRadiusScale = Params->TryGetNumberField(TEXT("radius_scale"), RadiusScale);

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("set_particle_lights"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System)
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 2000,
			TempResult.Message);

	const int32 Idx = FindEmitterHandleIndex(System, EmitterName);
	if (Idx == -2)
	{
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 2003,
			FString::Printf(TEXT("Emitter name '%s' matched multiple emitters in '%s' (ambiguous substring)"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name and pass it unambiguously."));
	}
	if (Idx < 0)
	{
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 2002,
			FString::Printf(TEXT("Emitter '%s' not found in '%s'"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name."));
	}

	// UNiagaraLightRendererProperties lives in the NiagaraEditor module; find by name to avoid hard include.
	UClass* LightRendClass = FindFirstObject<UClass>(TEXT("NiagaraLightRendererProperties"), EFindFirstObjectOptions::NativeFirst);

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[Idx];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData();
	if (!EmitterData)
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 3000,
			FString::Printf(TEXT("Emitter '%s' has no versioned emitter data"), *EmitterName));

	int32 NumRenderers = 0;
	int32 NumPropertiesPatched = 0;
	for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
	{
		if (!Renderer) continue;
		if (LightRendClass && !Renderer->IsA(LightRendClass)) continue;
		if (!LightRendClass && !Renderer->GetClass()->GetName().Contains(TEXT("Light"))) continue;

		// RadiusScale is the only verified UPROPERTY on UNiagaraLightRendererProperties in UE 5.7.
		// bOverrideRenderingEnabled / bUseMegaLights do not exist on this class.
		if (bHasRadiusScale)
		{
			if (FFloatProperty* RadProp = FindFProperty<FFloatProperty>(Renderer->GetClass(), TEXT("RadiusScale")))
			{
				RadProp->SetPropertyValue_InContainer(Renderer, (float)RadiusScale);
				++NumPropertiesPatched;
				Renderer->MarkPackageDirty();
			}
		}

		++NumRenderers;
	}

	if (NumRenderers == 0)
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 2002,
			FString::Printf(TEXT("No Light renderer found on emitter '%s' in '%s'"), *EmitterName, *AssetPath),
			TEXT("Add a Light renderer to the emitter first."));

	if (NumPropertiesPatched == 0)
		return MakeError(TEXT("niagara"), TEXT("set_particle_lights"), 3003,
			TEXT("No supported properties applied. In UE 5.7 only 'radius_scale' is a UPROPERTY on UNiagaraLightRendererProperties; 'enabled' and 'mega_lights' are not."),
			TEXT("Pass 'radius_scale' (float), or toggle rendering via the LightRenderingEnabledBinding in the Niagara editor."));

	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),          AssetPath);
	Data->SetStringField(TEXT("emitter_name"),        EmitterName);
	Data->SetNumberField(TEXT("renderers_patched"),   NumRenderers);
	Data->SetNumberField(TEXT("properties_patched"),  NumPropertiesPatched);
	if (bHasRadiusScale)
		Data->SetNumberField(TEXT("radius_scale"),    RadiusScale);

	return MakeSuccess(TEXT("niagara"), TEXT("set_particle_lights"),
		FString::Printf(TEXT("set_particle_lights: patched %d property(ies) across %d Light renderer(s) on emitter '%s' in '%s'"),
		                NumPropertiesPatched, NumRenderers, *EmitterName, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_collision_response  (Phase 1b)
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetCollisionResponse(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("set_collision_response"), 1000,
			TEXT("set_collision_response: 'asset_path' and 'emitter_name' are required"),
			TEXT("Optional: 'collision_type' (DepthBuffer|DistanceField|None), 'response' (hint for set_module_property)"));
	}

	// Preserve the response hint for the caller so they can route to set_module_property.
	FString CollisionType = TEXT("DepthBuffer");
	FString Response      = TEXT("Block");
	Params->TryGetStringField(TEXT("collision_type"), CollisionType);
	Params->TryGetStringField(TEXT("response"),       Response);

	return MakeError(TEXT("niagara"), TEXT("set_collision_response"), 3003,
		TEXT("set_collision_response: CollisionMode is a module-level script parameter, not a DI property. Use niagara/set_module_property on the 'Collision' module instead."),
		FString::Printf(TEXT("Suggested: set_module_property(emitter='%s', module='Collision', param='CollisionMode', value='%s'). Response hint: '%s'."),
		                *EmitterName, *CollisionType, *Response));
}

// ---------------------------------------------------------------------------
// list_emitters
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_ListEmitters(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("niagara"), TEXT("list_emitters"), 1000,
			TEXT("list_emitters: 'asset_path' is required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("list_emitters"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System)
		return MakeError(TEXT("niagara"), TEXT("list_emitters"), 2000, TempResult.Message);

	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),                 Handle.GetName().ToString());
		Obj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
		Obj->SetBoolField  (TEXT("enabled"),              Handle.GetIsEnabled());

		FString SimTargetStr = TEXT("Unknown");
		if (FVersionedNiagaraEmitterData* Data = Handle.GetInstance().GetEmitterData())
		{
			SimTargetStr = (Data->SimTarget == ENiagaraSimTarget::GPUComputeSim) ? TEXT("GPU") : TEXT("CPU");
		}
		Obj->SetStringField(TEXT("sim_target"), SimTargetStr);

		EmitterArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField (TEXT("emitters"), EmitterArray);
	Data->SetNumberField(TEXT("count"),    EmitterArray.Num());
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	return MakeSuccess(TEXT("niagara"), TEXT("list_emitters"),
		FString::Printf(TEXT("Found %d emitter(s) on '%s'"), EmitterArray.Num(), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_emitter_enabled
// ---------------------------------------------------------------------------

FBridgeResult UNiagaraHandler::Action_SetEmitterEnabled(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, EmitterName;
	bool bEnabled = true;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty() ||
	    !Params->TryGetBoolField  (TEXT("enabled"),      bEnabled))
	{
		return MakeError(TEXT("niagara"), TEXT("set_emitter_enabled"), 1000,
			TEXT("set_emitter_enabled: 'asset_path', 'emitter_name', and 'enabled' (bool) are required"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("niagara"), TEXT("set_emitter_enabled"));
	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, TempResult);
	if (!System)
		return MakeError(TEXT("niagara"), TEXT("set_emitter_enabled"), 2000, TempResult.Message);

	const int32 Idx = FindEmitterHandleIndex(System, EmitterName);
	if (Idx == -2)
	{
		return MakeError(TEXT("niagara"), TEXT("set_emitter_enabled"), 2003,
			FString::Printf(TEXT("Emitter name '%s' matched multiple emitters in '%s' (ambiguous substring)"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name."));
	}
	if (Idx < 0)
	{
		return MakeError(TEXT("niagara"), TEXT("set_emitter_enabled"), 2002,
			FString::Printf(TEXT("Emitter '%s' not found in '%s'"), *EmitterName, *AssetPath),
			TEXT("Use list_emitters to find the exact name."));
	}

	// GetEmitterHandles returns a const TArray&; need a mutable reference to call SetIsEnabled.
	FNiagaraEmitterHandle& Handle = const_cast<FNiagaraEmitterHandle&>(System->GetEmitterHandles()[Idx]);
	Handle.SetIsEnabled(bEnabled, *System, /*bRecompile=*/true);
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),   AssetPath);
	Data->SetStringField(TEXT("emitter_name"), EmitterName);
	Data->SetBoolField  (TEXT("enabled"),      bEnabled);

	return MakeSuccess(TEXT("niagara"), TEXT("set_emitter_enabled"),
		FString::Printf(TEXT("Emitter '%s' enabled=%s on '%s'"),
		                *EmitterName, bEnabled ? TEXT("true") : TEXT("false"), *AssetPath),
		Data);
}

TSharedPtr<FJsonObject> UNiagaraHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a new Niagara System asset with an initial emitter")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new system"))); Ps->SetObjectField(TEXT("emitter_path"), P(TEXT("string"), true, TEXT("Content path of a UNiagaraEmitter template"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_niagara_system"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add an emitter to an existing Niagara System")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_path"), P(TEXT("string"), true, TEXT("Content path of the emitter to add"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_emitter"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a user-exposed parameter on a Niagara System")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Parameter name (User. prefix added automatically)"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value as string (vectors: X,Y,Z; colors: R,G,B,A)"))); Ps->SetObjectField(TEXT("value_type"), P(TEXT("string"), true, TEXT("Type: float, int, bool, vector, color"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_user_parameter"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Request recompilation of a Niagara System")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("compile_niagara_system"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all user parameters on a Niagara System")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_parameters"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a parameter default value on a Niagara System")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Parameter name"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value as string"))); Ps->SetObjectField(TEXT("value_type"), P(TEXT("string"), true, TEXT("Type: float, int, bool, vector, color"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_parameter_default"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Trigger Niagara system capture export and return the JSON file contents. Without asset_path, exports all systems and returns a count.")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Single Niagara System content path to export (e.g. /Game/FX/NS_Fire)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("read_niagara_capture"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Capture Niagara pool state (replaces FNiagaraSystemUpdateContext removed in UE 5.7). Use read_niagara_capture to inspect the result.")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("update_instances"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a module script to a Niagara emitter stage (dispatched via Python)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Name of the emitter within the system"))); Ps->SetObjectField(TEXT("stage"), P(TEXT("string"), true, TEXT("Stage: Spawn, Update, or Render"))); Ps->SetObjectField(TEXT("module_path"), P(TEXT("string"), true, TEXT("Content path of the module script asset"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_module"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Remove a module script from a Niagara emitter stage (dispatched via Python)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Name of the emitter within the system"))); Ps->SetObjectField(TEXT("stage"), P(TEXT("string"), true, TEXT("Stage: Spawn, Update, or Render"))); Ps->SetObjectField(TEXT("module_path"), P(TEXT("string"), true, TEXT("Content path of the module script to remove"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("remove_module"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a module input parameter on a Niagara emitter (dispatched via Python)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Name of the emitter"))); Ps->SetObjectField(TEXT("module_path"), P(TEXT("string"), true, TEXT("Content path of the module script"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Input parameter name"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Parameter value as string"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_module_property"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a renderer to a Niagara emitter (dispatched via Python)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Name of the emitter"))); Ps->SetObjectField(TEXT("renderer_type"), P(TEXT("string"), true, TEXT("Type: Sprite, Mesh, Ribbon, Light, Decal"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_renderer"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a property on a Niagara renderer (dispatched via Python)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System"))); Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Name of the emitter"))); Ps->SetObjectField(TEXT("renderer_index"), P(TEXT("int"), false, TEXT("Renderer index (default 0)"))); Ps->SetObjectField(TEXT("property_name"), P(TEXT("string"), true, TEXT("Property name to set"))); Ps->SetObjectField(TEXT("value"), P(TEXT("string"), true, TEXT("Value as string"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_renderer_property"), A); }

	// set_gpu_sim
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("Toggle GPU vs CPU sim target on a named emitter."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),   P(TEXT("string"), true,  TEXT("Content path of the Niagara System")));
	  Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true,  TEXT("Emitter name (exact match on GetName, falls back to substring on UniqueInstanceName)")));
	  Ps->SetObjectField(TEXT("enabled"),      P(TEXT("bool"),   false, TEXT("true = GPUComputeSim, false = CPUSim (default true)")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("set_gpu_sim"), A); }

	// bind_parameter
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("Expose a User. parameter on the system's parameter store (NOTE: this is AddParameter, not attribute-binding)."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true,  TEXT("Content path of the Niagara System")));
	  Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true,  TEXT("Parameter name (User. prefix added automatically)")));
	  Ps->SetObjectField(TEXT("value_type"), P(TEXT("string"), true,  TEXT("float | int | bool | vector | color")));
	  Ps->SetObjectField(TEXT("value"),      P(TEXT("string"), false, TEXT("Optional default value")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("bind_parameter"), A); }

	// set_particle_lights
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("Patch properties on Light renderers of a named emitter. In UE 5.7 only 'radius_scale' is a supported UPROPERTY; 'enabled' and 'mega_lights' are not."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),   P(TEXT("string"), true,  TEXT("Content path of the Niagara System")));
	  Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true,  TEXT("Emitter name (exact match preferred; case-insensitive substring fallback)")));
	  Ps->SetObjectField(TEXT("radius_scale"), P(TEXT("number"), false, TEXT("Scale factor on each particle light radius (default 1.0)")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("set_particle_lights"), A); }

	// set_collision_response
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("NOT IMPLEMENTED in UE 5.7 — CollisionMode is a module-pin, not a DI UPROPERTY. Returns 3003. Use set_module_property on the 'Collision' module."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),     P(TEXT("string"), true,  TEXT("Content path of the Niagara System")));
	  Ps->SetObjectField(TEXT("emitter_name"),   P(TEXT("string"), true,  TEXT("Emitter name")));
	  Ps->SetObjectField(TEXT("collision_type"), P(TEXT("string"), false, TEXT("DepthBuffer | DistanceField | None (hint for set_module_property)")));
	  Ps->SetObjectField(TEXT("response"),       P(TEXT("string"), false, TEXT("Block | Overlap | Ignore (not a Niagara concept)")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("set_collision_response"), A); }

	// list_emitters
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("List emitters on a Niagara System: name, unique_instance_name, enabled, sim_target (GPU/CPU)."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the Niagara System")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("list_emitters"), A); }

	// set_emitter_enabled
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
	  A->SetStringField(TEXT("desc"), TEXT("Enable or disable a named emitter on a Niagara System (triggers recompile)."));
	  TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
	  Ps->SetObjectField(TEXT("asset_path"),   P(TEXT("string"), true, TEXT("Content path of the Niagara System")));
	  Ps->SetObjectField(TEXT("emitter_name"), P(TEXT("string"), true, TEXT("Emitter name (exact match preferred; case-insensitive substring fallback)")));
	  Ps->SetObjectField(TEXT("enabled"),      P(TEXT("bool"),   true, TEXT("true to enable the emitter, false to disable")));
	  A->SetObjectField(TEXT("params"), Ps);
	  Root->SetObjectField(TEXT("set_emitter_enabled"), A); }

	return Root;
}

// ---------------------------------------------------------------------------
// remove_emitter — mirrors add_emitter
// ---------------------------------------------------------------------------
FBridgeResult UNiagaraHandler::Action_RemoveEmitter(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("remove_emitter"));
	FString AssetPath, EmitterName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
		return MakeError(TEXT("niagara"), TEXT("remove_emitter"), 1000,
			TEXT("'asset_path' and 'emitter_name' are required"));

	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, Result);
	if (!System) return Result;

	const int32 Idx = FindEmitterHandleIndex(System, EmitterName);
	if (Idx == -1)
		return MakeError(TEXT("niagara"), TEXT("remove_emitter"), 2000,
			FString::Printf(TEXT("Emitter '%s' not found"), *EmitterName));
	if (Idx == -2)
		return MakeError(TEXT("niagara"), TEXT("remove_emitter"), 1002,
			FString::Printf(TEXT("Multiple emitters match '%s' — be more specific"), *EmitterName));

	TSet<FGuid> ToRemove;
	ToRemove.Add(System->GetEmitterHandle(Idx).GetId());
	System->RemoveEmitterHandlesById(ToRemove);
	System->RequestCompile(/*bForce=*/false);
	System->MarkPackageDirty();

	return MakeSuccess(TEXT("niagara"), TEXT("remove_emitter"),
		FString::Printf(TEXT("Removed emitter '%s' from '%s'"), *EmitterName, *AssetPath));
}

// ---------------------------------------------------------------------------
// remove_renderer — by index, on a named emitter
// ---------------------------------------------------------------------------
FBridgeResult UNiagaraHandler::Action_RemoveRenderer(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("niagara"), TEXT("remove_renderer"));
	FString AssetPath, EmitterName;
	int32 RendererIndex = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
		return MakeError(TEXT("niagara"), TEXT("remove_renderer"), 1000,
			TEXT("'asset_path', 'emitter_name', and 'renderer_index' (>=0) are required"));

	UNiagaraSystem* System = LoadNiagaraSystem(AssetPath, Result);
	if (!System) return Result;

	const int32 EIdx = FindEmitterHandleIndex(System, EmitterName);
	if (EIdx < 0)
		return MakeError(TEXT("niagara"), TEXT("remove_renderer"), 2000,
			FString::Printf(TEXT("Emitter '%s' not found"), *EmitterName));

	// 5.7: FVersionedNiagaraEmitterData::RendererProperties is private — no public mutator
	// is exposed. Renderer removal must go through the editor's Niagara emitter API
	// which is not bridged. Return 3003 with a Python-escape recommendation.
	(void)RendererIndex;
	(void)EmitterName;
	return MakeError(TEXT("niagara"), TEXT("remove_renderer"), 3003,
		TEXT("FVersionedNiagaraEmitterData::RendererProperties is private in 5.7"),
		TEXT("Use PythonHandler with unreal.NiagaraEmitter to remove renderers, or edit via the Niagara Editor UI"));
}
