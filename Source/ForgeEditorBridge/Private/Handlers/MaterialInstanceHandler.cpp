#include "Handlers/MaterialInstanceHandler.h"
#include "ForgeAISubsystem.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- Material types --------------------------------------------------------
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "StaticParameterSet.h"              // FStaticParameterSet, FStaticSwitchParameter

// ---- Textures --------------------------------------------------------------
#include "Engine/Texture.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UMaterialInstanceHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("material_instance"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_instance")) return Action_CreateInstance(Params);
	if (Action == TEXT("set_scalar"))      return Action_SetScalar(Params);
	if (Action == TEXT("set_vector"))      return Action_SetVector(Params);
	if (Action == TEXT("set_texture"))     return Action_SetTexture(Params);
	if (Action == TEXT("set_switch"))      return Action_SetSwitch(Params);
	if (Action == TEXT("get_parameters")) return Action_GetParameters(Params);
	if (Action == TEXT("get_parent"))     return Action_GetParent(Params);

	FBridgeResult R = CreateResult(TEXT("material_instance"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown material_instance action '%s'. Valid: create_instance, set_scalar, set_vector, set_texture, set_switch, get_parameters, get_parent"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_instance
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_CreateInstance(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("material_instance"), TEXT("create_instance"));

	FString AssetPath, ParentPath;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("parent_path"), ParentPath) || ParentPath.IsEmpty())
	{
		Result.Message = TEXT("create_instance: 'asset_path' and 'parent_path' are required");
		return Result;
	}

	// Resolve parent material
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
	if (!Parent)
	{
		const FString Suffix = ParentPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ParentPath);
		Parent = LoadObject<UMaterialInterface>(nullptr, *Suffix);
	}
	if (!Parent)
	{
		Result.Message = FString::Printf(
			TEXT("create_instance: parent material not found at '%s'"), *ParentPath);
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
	                                                    UMaterialInstanceConstant::StaticClass(), Factory);
	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_instance: failed to create asset at '%s' (path may already exist or be invalid)"),
			*AssetPath);
		return Result;
	}

	CastChecked<UMaterialInstanceConstant>(CreatedAsset)->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("MaterialInstanceConstant created at '%s' (parent='%s')"), *AssetPath, *ParentPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_scalar
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_SetScalar(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("material_instance"), TEXT("set_scalar"));

	FString AssetPath, ParamName;
	double Value = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("param_name"), ParamName)   || ParamName.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("value"),       Value))
	{
		Result.Message = TEXT("set_scalar: 'asset_path', 'param_name', and 'value' (float) are required");
		return Result;
	}

	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Result);
	if (!MIC) return Result;

	MIC->SetScalarParameterValueEditorOnly(FName(*ParamName), (float)Value);
	MIC->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Scalar '%s' set to %f on '%s'"), *ParamName, (float)Value, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_vector
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_SetVector(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("material_instance"), TEXT("set_vector"));

	FString AssetPath, ParamName;
	double R = 0, G = 0, B = 0, A = 1.0;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("param_name"), ParamName)   || ParamName.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("r"), R) ||
	    !Params->TryGetNumberField(TEXT("g"), G) ||
	    !Params->TryGetNumberField(TEXT("b"), B))
	{
		Result.Message = TEXT("set_vector: 'asset_path', 'param_name', 'r', 'g', 'b' are required; 'a' is optional (default 1.0)");
		return Result;
	}
	Params->TryGetNumberField(TEXT("a"), A);

	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Result);
	if (!MIC) return Result;

	MIC->SetVectorParameterValueEditorOnly(FName(*ParamName),
	    FLinearColor((float)R, (float)G, (float)B, (float)A));
	MIC->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Vector '%s' set to (%.3f, %.3f, %.3f, %.3f) on '%s'"),
		*ParamName, (float)R, (float)G, (float)B, (float)A, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_texture
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_SetTexture(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("material_instance"), TEXT("set_texture"));

	FString AssetPath, ParamName, TexturePath;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("param_name"),   ParamName)   || ParamName.IsEmpty()   ||
	    !Params->TryGetStringField(TEXT("texture_path"), TexturePath) || TexturePath.IsEmpty())
	{
		Result.Message = TEXT("set_texture: 'asset_path', 'param_name', and 'texture_path' are required");
		return Result;
	}

	// Resolve texture
	UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
	if (!Texture)
	{
		const FString Suffix = TexturePath + TEXT(".") + FPackageName::GetLongPackageAssetName(TexturePath);
		Texture = LoadObject<UTexture>(nullptr, *Suffix);
	}
	if (!Texture)
	{
		Result.Message = FString::Printf(
			TEXT("set_texture: UTexture not found at '%s'"), *TexturePath);
		return Result;
	}

	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Result);
	if (!MIC) return Result;

	MIC->SetTextureParameterValueEditorOnly(FName(*ParamName), Texture);
	MIC->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Texture param '%s' set to '%s' on '%s'"), *ParamName, *TexturePath, *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// set_switch
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_SetSwitch(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("material_instance"), TEXT("set_switch"));

	FString AssetPath, ParamName;
	bool bValue = false;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath)  || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("param_name"), ParamName)   || ParamName.IsEmpty() ||
	    !Params->TryGetBoolField  (TEXT("value"),       bValue))
	{
		Result.Message = TEXT("set_switch: 'asset_path', 'param_name', and 'value' (bool) are required");
		return Result;
	}

	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Result);
	if (!MIC) return Result;

	// Read → modify → write static parameter set
	FStaticParameterSet StaticParams;
	MIC->GetStaticParameterValues(StaticParams);

	bool bFound = false;
	for (FStaticSwitchParameter& Switch : StaticParams.StaticSwitchParameters)
	{
		if (Switch.ParameterInfo.Name == FName(*ParamName))
		{
			Switch.Value     = bValue;
			Switch.bOverride = true;
			bFound           = true;
			break;
		}
	}

	if (!bFound)
	{
		FStaticSwitchParameter NewSwitch;
		NewSwitch.ParameterInfo.Name = FName(*ParamName);
		NewSwitch.Value              = bValue;
		NewSwitch.bOverride          = true;
		StaticParams.StaticSwitchParameters.Add(NewSwitch);
	}

	MIC->UpdateStaticPermutation(StaticParams);
	MIC->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("Static switch '%s' set to %s on '%s'"),
		*ParamName, bValue ? TEXT("true") : TEXT("false"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// get_parameters
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_GetParameters(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("material_instance"), TEXT("get_parameters"),
			1000, TEXT("get_parameters: 'asset_path' is required"),
			TEXT("Provide the content path of a UMaterialInstanceConstant asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("material_instance"), TEXT("get_parameters"));
	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, TempResult);
	if (!MIC)
	{
		return MakeError(TEXT("material_instance"), TEXT("get_parameters"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UMaterialInstanceConstant"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Scalar parameters
	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (const FScalarParameterValue& P : MIC->ScalarParameterValues)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), P.ParameterInfo.Name.ToString());
		Entry->SetNumberField(TEXT("value"), P.ParameterValue);
		ScalarArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("scalar_parameters"), ScalarArray);

	// Vector parameters
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (const FVectorParameterValue& P : MIC->VectorParameterValues)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), P.ParameterInfo.Name.ToString());
		Entry->SetStringField(TEXT("value"), FString::Printf(TEXT("(%.4f, %.4f, %.4f, %.4f)"),
			P.ParameterValue.R, P.ParameterValue.G, P.ParameterValue.B, P.ParameterValue.A));
		VectorArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("vector_parameters"), VectorArray);

	// Texture parameters
	TArray<TSharedPtr<FJsonValue>> TextureArray;
	for (const FTextureParameterValue& P : MIC->TextureParameterValues)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), P.ParameterInfo.Name.ToString());
		Entry->SetStringField(TEXT("value"), P.ParameterValue ? P.ParameterValue->GetPathName() : TEXT("None"));
		TextureArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("texture_parameters"), TextureArray);

	const int32 TotalCount = ScalarArray.Num() + VectorArray.Num() + TextureArray.Num();

	return MakeSuccess(TEXT("material_instance"), TEXT("get_parameters"),
		FString::Printf(TEXT("Found %d parameter override(s) on '%s' (scalar=%d, vector=%d, texture=%d)"),
			TotalCount, *AssetPath, ScalarArray.Num(), VectorArray.Num(), TextureArray.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_parent
// ---------------------------------------------------------------------------

FBridgeResult UMaterialInstanceHandler::Action_GetParent(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("material_instance"), TEXT("get_parent"),
			1000, TEXT("get_parent: 'asset_path' is required"),
			TEXT("Provide the content path of a UMaterialInstanceConstant asset"));
	}

	FBridgeResult TempResult = CreateResult(TEXT("material_instance"), TEXT("get_parent"));
	UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, TempResult);
	if (!MIC)
	{
		return MakeError(TEXT("material_instance"), TEXT("get_parent"),
			2000, TempResult.Message,
			TEXT("Verify the asset path points to a valid UMaterialInstanceConstant"));
	}

	FString ParentPath = MIC->Parent ? MIC->Parent->GetPathName() : TEXT("None");

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parent_path"), ParentPath);

	return MakeSuccess(TEXT("material_instance"), TEXT("get_parent"),
		FString::Printf(TEXT("Parent of '%s' is '%s'"), *AssetPath, *ParentPath),
		Data);
}

// ---------------------------------------------------------------------------
// LoadMIC helper
// ---------------------------------------------------------------------------

UMaterialInstanceConstant* UMaterialInstanceHandler::LoadMIC(const FString& AssetPath, FBridgeResult& Result)
{
	UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
	if (!MIC)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *Suffix);
	}
	if (!MIC)
	{
		Result.Message = FString::Printf(
			TEXT("LoadMIC: no UMaterialInstanceConstant found at '%s'"), *AssetPath);
	}
	return MIC;
}

TSharedPtr<FJsonObject> UMaterialInstanceHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a MaterialInstanceConstant from a parent material")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new MIC"))); Ps->SetObjectField(TEXT("parent_path"), P(TEXT("string"), true, TEXT("Content path of the parent material"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_instance"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a scalar parameter override")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Scalar parameter name"))); Ps->SetObjectField(TEXT("value"), P(TEXT("float"), true, TEXT("Scalar value"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_scalar"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a vector/color parameter override")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Vector parameter name"))); Ps->SetObjectField(TEXT("r"), P(TEXT("float"), true, TEXT("Red component"))); Ps->SetObjectField(TEXT("g"), P(TEXT("float"), true, TEXT("Green component"))); Ps->SetObjectField(TEXT("b"), P(TEXT("float"), true, TEXT("Blue component"))); Ps->SetObjectField(TEXT("a"), P(TEXT("float"), false, TEXT("Alpha component (default 1.0)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_vector"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a texture parameter override")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Texture parameter name"))); Ps->SetObjectField(TEXT("texture_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_texture"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a static switch parameter override")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); Ps->SetObjectField(TEXT("param_name"), P(TEXT("string"), true, TEXT("Switch parameter name"))); Ps->SetObjectField(TEXT("value"), P(TEXT("bool"), true, TEXT("Switch value"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_switch"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get all parameter overrides on a MIC")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_parameters"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get the parent material of a MIC")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the MIC"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_parent"), A); }

	return Root;
}
