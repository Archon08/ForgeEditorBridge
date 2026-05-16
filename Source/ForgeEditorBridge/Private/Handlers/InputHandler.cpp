#include "Handlers/InputHandler.h"
#include "ForgeAISubsystem.h"

// ---- Enhanced Input types (EnhancedInput) ----------------------------------
#include "InputAction.h"            // UInputAction, EInputActionValueType
#include "InputMappingContext.h"    // UInputMappingContext, FEnhancedActionKeyMapping

// ---- Input core types ------------------------------------------------------
#include "InputCoreTypes.h"         // FKey

// ---- Asset registry --------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

// ---- Misc ------------------------------------------------------------------
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"

// ---- Capture ---------------------------------------------------------------
#include "Capture/ForgeInputCapture.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UInputHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("input"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_action"))  return Action_CreateAction(Params);
	if (Action == TEXT("create_context")) return Action_CreateContext(Params);
	if (Action == TEXT("add_mapping"))    return Action_AddMapping(Params);
	if (Action == TEXT("list_actions"))   return Action_ListActions(Params);
	if (Action == TEXT("list_contexts"))  return Action_ListContexts(Params);
	if (Action == TEXT("get_mappings"))        return Action_GetMappings(Params);
	if (Action == TEXT("read_input_capture"))  return Action_ReadInputCapture(Params);

	FBridgeResult R = CreateResult(TEXT("input"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown input action '%s'. Valid: create_action, create_context, add_mapping, list_actions, list_contexts, get_mappings, read_input_capture"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_action
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_CreateAction(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("input"), TEXT("create_action"));

	FString AssetPath;
	FString ValueTypeStr = TEXT("bool");

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_action: 'asset_path' is required (e.g. \"/Game/Input/IA_Jump\")");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("value_type"), ValueTypeStr);

	// Map value_type string → EInputActionValueType
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if      (ValueTypeStr == TEXT("axis1d")) ValueType = EInputActionValueType::Axis1D;
	else if (ValueTypeStr == TEXT("axis2d")) ValueType = EInputActionValueType::Axis2D;
	else if (ValueTypeStr == TEXT("axis3d")) ValueType = EInputActionValueType::Axis3D;
	else if (ValueTypeStr != TEXT("bool"))
	{
		Result.Message = FString::Printf(
			TEXT("create_action: unknown value_type '%s'. Valid: bool, axis1d, axis2d, axis3d"), *ValueTypeStr);
		Result.ErrorCode = 1001;
		return Result;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	Package->FullyLoad();

	UInputAction* Action = NewObject<UInputAction>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Action->ValueType = ValueType;
	Action->PostEditChange();
	FAssetRegistryModule::AssetCreated(Action);
	Package->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("InputAction '%s' created (value_type=%s) at '%s'"),
		*AssetName, *ValueTypeStr, *FPackageName::GetLongPackagePath(AssetPath));
	return Result;
}

// ---------------------------------------------------------------------------
// create_context
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_CreateContext(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("input"), TEXT("create_context"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_context: 'asset_path' is required (e.g. \"/Game/Input/IMC_Default\")");
		Result.ErrorCode = 1000;
		return Result;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	Package->FullyLoad();

	UInputMappingContext* Context = NewObject<UInputMappingContext>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Context->PostEditChange();
	FAssetRegistryModule::AssetCreated(Context);
	Package->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("InputMappingContext '%s' created at '%s'"),
		*AssetName, *FPackageName::GetLongPackagePath(AssetPath));
	return Result;
}

// ---------------------------------------------------------------------------
// add_mapping
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_AddMapping(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("input"), TEXT("add_mapping"));

	FString ContextPath, ActionPath, KeyName;
	if (!Params->TryGetStringField(TEXT("context_path"), ContextPath) || ContextPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("action_path"),  ActionPath)  || ActionPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("key"),          KeyName)     || KeyName.IsEmpty())
	{
		Result.Message = TEXT("add_mapping: 'context_path', 'action_path', and 'key' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	// Load the mapping context
	UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
	if (!Context)
	{
		const FString Suffix = ContextPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ContextPath);
		Context = LoadObject<UInputMappingContext>(nullptr, *Suffix);
	}
	if (!Context)
	{
		Result.Message = FString::Printf(
			TEXT("add_mapping: no UInputMappingContext found at '%s'"), *ContextPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Load the input action
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
	if (!Action)
	{
		const FString Suffix = ActionPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ActionPath);
		Action = LoadObject<UInputAction>(nullptr, *Suffix);
	}
	if (!Action)
	{
		Result.Message = FString::Printf(
			TEXT("add_mapping: no UInputAction found at '%s'"), *ActionPath);
		Result.ErrorCode = 2000;
		return Result;
	}

	// Validate the key name
	const FKey Key = FKey(FName(*KeyName));
	if (!Key.IsValid())
	{
		Result.Message = FString::Printf(
			TEXT("add_mapping: '%s' is not a valid FKey name (e.g. \"SpaceBar\", \"Gamepad_FaceButton_Bottom\")"),
			*KeyName);
		Result.ErrorCode = 1000;
		return Result;
	}

	// UE 5.7: Context->Mappings is now protected. Use the public MapKey API.
	FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
	(void)Mapping; // suppress unused variable warning
	Context->MarkPackageDirty();

	Result.bSuccess     = true;
	Result.AffectedPath = ContextPath;
	Result.Message      = FString::Printf(
		TEXT("Mapping added: key='%s' → action='%s' in context '%s'"),
		*KeyName, *FPackageName::GetLongPackageAssetName(ActionPath),
		*FPackageName::GetLongPackageAssetName(ContextPath));
	return Result;
}

// ---------------------------------------------------------------------------
// list_actions
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_ListActions(TSharedPtr<FJsonObject> Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	TArray<TSharedPtr<FJsonValue>> ActionArray;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		ActionArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("actions"), ActionArray);
	Data->SetNumberField(TEXT("count"), ActionArray.Num());

	return MakeSuccess(TEXT("input"), TEXT("list_actions"),
		FString::Printf(TEXT("Found %d InputAction asset(s)"), ActionArray.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// list_contexts
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_ListContexts(TSharedPtr<FJsonObject> Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UInputMappingContext::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	TArray<TSharedPtr<FJsonValue>> ContextArray;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		ContextArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("contexts"), ContextArray);
	Data->SetNumberField(TEXT("count"), ContextArray.Num());

	return MakeSuccess(TEXT("input"), TEXT("list_contexts"),
		FString::Printf(TEXT("Found %d InputMappingContext asset(s)"), ContextArray.Num()),
		Data);
}

// ---------------------------------------------------------------------------
// get_mappings
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_GetMappings(TSharedPtr<FJsonObject> Params)
{
	FString ContextPath;
	if (!Params->TryGetStringField(TEXT("context_path"), ContextPath) || ContextPath.IsEmpty())
	{
		return MakeError(TEXT("input"), TEXT("get_mappings"),
			1000, TEXT("get_mappings: 'context_path' is required"),
			TEXT("Provide the content path of a UInputMappingContext asset"));
	}

	UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
	if (!Context)
	{
		const FString Suffix = ContextPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ContextPath);
		Context = LoadObject<UInputMappingContext>(nullptr, *Suffix);
	}
	if (!Context)
	{
		return MakeError(TEXT("input"), TEXT("get_mappings"),
			2000, FString::Printf(TEXT("No UInputMappingContext found at '%s'"), *ContextPath),
			TEXT("Use list_contexts to find valid context paths"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();

	TArray<TSharedPtr<FJsonValue>> MappingArray;
	for (const FEnhancedActionKeyMapping& Mapping : Mappings)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : TEXT("None"));
		Entry->SetStringField(TEXT("key"), Mapping.Key.GetFName().ToString());
		MappingArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("mappings"), MappingArray);
	Data->SetNumberField(TEXT("count"), MappingArray.Num());

	return MakeSuccess(TEXT("input"), TEXT("get_mappings"),
		FString::Printf(TEXT("Found %d mapping(s) in '%s'"), MappingArray.Num(), *ContextPath),
		Data);
}

// ---------------------------------------------------------------------------
// read_input_capture
// ---------------------------------------------------------------------------

FBridgeResult UInputHandler::Action_ReadInputCapture(TSharedPtr<FJsonObject> Params)
{
	if (Subsystem->InputCapture)
		Subsystem->InputCapture->ExportInputAudit();

	FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("input/enhanced_input_audit.json"));
	FString FileContent;
	FBridgeResult Res = MakeSuccess(GetDomainName(), TEXT("read_input_capture"), TEXT("Capture complete"));
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

TSharedPtr<FJsonObject> UInputHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create an Enhanced Input Action asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new InputAction"))); Ps->SetObjectField(TEXT("value_type"), P(TEXT("string"), false, TEXT("bool, axis1d, axis2d, axis3d (default bool)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_action"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create an Input Mapping Context asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new InputMappingContext"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_context"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a key mapping to an InputMappingContext")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("context_path"), P(TEXT("string"), true, TEXT("Content path of the InputMappingContext"))); Ps->SetObjectField(TEXT("action_path"), P(TEXT("string"), true, TEXT("Content path of the InputAction"))); Ps->SetObjectField(TEXT("key"), P(TEXT("string"), true, TEXT("FKey name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_mapping"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all InputAction assets in the project")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("list_actions"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all InputMappingContext assets in the project")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("list_contexts"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get all key mappings in an InputMappingContext")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("context_path"), P(TEXT("string"), true, TEXT("Content path of the InputMappingContext"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_mappings"), A); }

	return Root;
}
