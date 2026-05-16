#include "Handlers/ControlRigHandler.h"
#include "ForgeAISubsystem.h"

// ---- ControlRig / RigVM includes (UE 5.7) ----------------------------------
#include "ControlRig.h"
// UE 5.7: UControlRigBlueprint moved into the ControlRigDeveloper module
// and the header was renamed ControlRigBlueprintLegacy.h.
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigBlueprintFactory.h"    // ControlRigEditor — creates the asset
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMClient.h"
// URigVMBlueprint is transitively included via ControlRigBlueprintLegacy.h above.

// ---- Blueprint compilation -------------------------------------------------
#include "Kismet2/KismetEditorUtilities.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

// ---- Reflection ------------------------------------------------------------
#include "UObject/UnrealType.h"

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UControlRigHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("control_rig"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("create_rig"))             return Action_CreateRig(Params);
	if (Action == TEXT("add_control"))            return Action_AddControl(Params);
	if (Action == TEXT("add_node"))               return Action_AddNode(Params);
	if (Action == TEXT("connect_pins"))           return Action_ConnectPins(Params);
	if (Action == TEXT("add_bone_chain"))         return Action_AddBoneChain(Params);
	if (Action == TEXT("set_ik_goal"))            return Action_SetIKGoal(Params);
	if (Action == TEXT("add_constraint"))         return Action_AddConstraint(Params);
	if (Action == TEXT("get_rig_topology"))       return Action_GetRigTopology(Params);
	if (Action == TEXT("add_forward_solve_node")) return Action_AddForwardSolveNode(Params);
	if (Action == TEXT("compile"))                return Action_Compile(Params);

	FBridgeResult R = CreateResult(TEXT("control_rig"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown control_rig action '%s'. Valid: create_rig, add_control, add_node, connect_pins, add_bone_chain, set_ik_goal, add_constraint, get_rig_topology, add_forward_solve_node, compile"),
		*Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// create_rig — uses UControlRigBlueprintFactory
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_CreateRig(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("create_rig"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("create_rig: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();

	FAssetToolsModule& ATModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* CreatedAsset = ATModule.Get().CreateAsset(AssetName, PackagePath,
		nullptr, Factory);

	if (!CreatedAsset)
	{
		Result.Message = FString::Printf(
			TEXT("create_rig: failed to create ControlRig at '%s'"), *AssetPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	CreatedAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("ControlRig created at %s"), *AssetPath);
	return Result;
}

// ---------------------------------------------------------------------------
// add_control — uses RigHierarchyController
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_AddControl(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("add_control"));

	FString AssetPath, ControlName, ControlType, ParentName;
	if (!Params->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("control_name"),  ControlName)  || ControlName.IsEmpty())
	{
		Result.Message = TEXT("add_control: 'asset_path' and 'control_name' required");
		Result.ErrorCode = 1000;
		return Result;
	}
	if (!Params->TryGetStringField(TEXT("control_type"), ControlType)) ControlType = TEXT("Transform");
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	// Access hierarchy — try the Hierarchy property via reflection
	FProperty* HierProp = CRAsset->GetClass()->FindPropertyByName(TEXT("Hierarchy"));
	URigHierarchy* Hierarchy = nullptr;
	if (HierProp)
	{
		FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(HierProp);
		if (ObjProp)
			Hierarchy = Cast<URigHierarchy>(ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CRAsset)));
	}

	if (!Hierarchy)
	{
		Result.Message = TEXT("add_control: could not find Hierarchy on asset");
		Result.ErrorCode = 3000;
		return Result;
	}

	URigHierarchyController* Controller = Hierarchy->GetController(true);
	if (!Controller)
	{
		Result.Message = TEXT("add_control: failed to get RigHierarchyController");
		Result.ErrorCode = 3000;
		return Result;
	}

	ERigControlType RigControlType = ERigControlType::EulerTransform;
	if      (ControlType == TEXT("Float"))     RigControlType = ERigControlType::Float;
	else if (ControlType == TEXT("Integer"))   RigControlType = ERigControlType::Integer;
	else if (ControlType == TEXT("Bool"))      RigControlType = ERigControlType::Bool;
	else if (ControlType == TEXT("Vector2D"))  RigControlType = ERigControlType::Vector2D;
	else if (ControlType == TEXT("Position"))  RigControlType = ERigControlType::Position;
	else if (ControlType == TEXT("Scale"))     RigControlType = ERigControlType::Scale;
	else if (ControlType == TEXT("Rotator"))   RigControlType = ERigControlType::Rotator;

	FRigControlSettings Settings;
	Settings.ControlType = RigControlType;

	FRigElementKey ParentKey;
	if (!ParentName.IsEmpty())
		ParentKey = FRigElementKey(FName(*ParentName), ERigElementType::Control);

	FRigElementKey NewKey = Controller->AddControl(
		FName(*ControlName), ParentKey, Settings, FRigControlValue(), FTransform::Identity, FTransform::Identity);

	if (!NewKey.IsValid())
	{
		Result.Message = FString::Printf(TEXT("add_control: failed to add control '%s'"), *ControlName);
		Result.ErrorCode = 3000;
		return Result;
	}

	CRAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Control '%s' (%s) added"), *ControlName, *ControlType);
	return Result;
}

// ---------------------------------------------------------------------------
// add_node — adds a RigVM unit node via RigVMClient
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_AddNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("add_node"));

	FString AssetPath, FunctionPath;
	double PosX = 0.0, PosY = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("function"),   FunctionPath) || FunctionPath.IsEmpty())
	{
		Result.Message = TEXT("add_node: 'asset_path' and 'function' (UScriptStruct path) required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetNumberField(TEXT("x"), PosX);
	Params->TryGetNumberField(TEXT("y"), PosY);

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	// Get RigVMClient via reflection — it's a member on the ControlRig asset
	FRigVMClient* Client = nullptr;
	FProperty* ClientProp = CRAsset->GetClass()->FindPropertyByName(TEXT("RigVMClient"));
	if (ClientProp)
	{
		FStructProperty* StructProp = CastField<FStructProperty>(ClientProp);
		if (StructProp)
			Client = StructProp->ContainerPtrToValuePtr<FRigVMClient>(CRAsset);
	}

	if (!Client || Client->Num() == 0)
	{
		Result.Message = TEXT("add_node: no RigVMClient found on asset");
		Result.ErrorCode = 3000;
		return Result;
	}

	URigVMGraph* Graph = Client->GetDefaultModel();
	URigVMController* Controller = Graph ? Client->GetOrCreateController(Graph) : nullptr;
	if (!Controller)
	{
		Result.Message = TEXT("add_node: no RigVMController available");
		Result.ErrorCode = 3000;
		return Result;
	}

	UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *FunctionPath);
	if (!Struct) Struct = LoadObject<UScriptStruct>(nullptr, *FunctionPath);
	if (!Struct)
	{
		Result.Message = FString::Printf(TEXT("add_node: UScriptStruct not found: '%s'"), *FunctionPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the 'function' path is a valid UScriptStruct (e.g. '/Script/ControlRig.RigUnit_SetTransform')");
		return Result;
	}

	URigVMNode* NewNode = Controller->AddUnitNode(Struct, TEXT("Execute"),
		FVector2D(PosX, PosY), FString(), false);

	if (!NewNode)
	{
		Result.Message = FString::Printf(TEXT("add_node: AddUnitNode failed for '%s'"), *FunctionPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	CRAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Node '%s' added at (%.0f, %.0f)"),
		*NewNode->GetName(), PosX, PosY);
	return Result;
}

// ---------------------------------------------------------------------------
// connect_pins
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_ConnectPins(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("connect_pins"));

	FString AssetPath, OutputPin, InputPin;
	if (!Params->TryGetStringField(TEXT("asset_path"),  AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("output_pin"),  OutputPin) || OutputPin.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("input_pin"),   InputPin)  || InputPin.IsEmpty())
	{
		Result.Message = TEXT("connect_pins: 'asset_path', 'output_pin', and 'input_pin' required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	FRigVMClient* Client = nullptr;
	FProperty* ClientProp = CRAsset->GetClass()->FindPropertyByName(TEXT("RigVMClient"));
	if (ClientProp)
	{
		FStructProperty* StructProp = CastField<FStructProperty>(ClientProp);
		if (StructProp)
			Client = StructProp->ContainerPtrToValuePtr<FRigVMClient>(CRAsset);
	}

	if (!Client || Client->Num() == 0)
	{
		Result.Message = TEXT("connect_pins: no RigVMClient found");
		Result.ErrorCode = 3000;
		return Result;
	}

	URigVMGraph* Graph = Client->GetDefaultModel();
	URigVMController* Controller = Graph ? Client->GetOrCreateController(Graph) : nullptr;
	if (!Controller)
	{
		Result.Message = TEXT("connect_pins: no RigVMController available");
		Result.ErrorCode = 3000;
		return Result;
	}

	bool bLinked = Controller->AddLink(OutputPin, InputPin, false);
	if (!bLinked)
	{
		Result.Message = FString::Printf(TEXT("connect_pins: AddLink failed for '%s' -> '%s'"), *OutputPin, *InputPin);
		Result.ErrorCode = 3000;
		return Result;
	}

	CRAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Connected '%s' -> '%s'"), *OutputPin, *InputPin);
	return Result;
}

// ---------------------------------------------------------------------------
// LoadCRAsset helper
// ---------------------------------------------------------------------------

UObject* UControlRigHandler::LoadCRAsset(const FString& AssetPath, FBridgeResult& Result)
{
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Asset = LoadObject<UObject>(nullptr, *Suffix);
	}
	if (!Asset)
	{
		Result.Message = FString::Printf(TEXT("LoadCRAsset: no asset at '%s'"), *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify asset_path points to a valid ControlRig asset");
	}
	return Asset;
}

// ---------------------------------------------------------------------------
// Shared helper: get URigHierarchy from a CR asset via reflection
// ---------------------------------------------------------------------------

static URigHierarchy* GetRigHierarchy(UObject* CRAsset)
{
	if (!CRAsset) return nullptr;
	FProperty* Prop = CRAsset->GetClass()->FindPropertyByName(TEXT("Hierarchy"));
	if (!Prop) return nullptr;
	FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
	return ObjProp ? Cast<URigHierarchy>(ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CRAsset))) : nullptr;
}

// ---------------------------------------------------------------------------
// add_bone_chain — append a chain of bones to the rig hierarchy
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_AddBoneChain(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("add_bone_chain"));

	FString AssetPath, ChainName, ParentName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("chain_name"), ChainName) || ChainName.IsEmpty())
	{
		Result.Message = TEXT("add_bone_chain: 'asset_path' and 'chain_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	double BoneCount = 4.0, BoneLength = 10.0;
	Params->TryGetNumberField(TEXT("count"),  BoneCount);
	Params->TryGetNumberField(TEXT("length"), BoneLength);
	const int32 Count = FMath::Max(1, (int32)BoneCount);

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	URigHierarchy* Hierarchy = GetRigHierarchy(CRAsset);
	if (!Hierarchy)
	{
		Result.Message = TEXT("add_bone_chain: could not find Hierarchy on asset");
		Result.ErrorCode = 3000;
		return Result;
	}

	URigHierarchyController* Controller = Hierarchy->GetController(true);
	if (!Controller)
	{
		Result.Message = TEXT("add_bone_chain: failed to get RigHierarchyController");
		Result.ErrorCode = 3000;
		return Result;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Add Bone Chain")));

	FRigElementKey ParentKey;
	if (!ParentName.IsEmpty())
		ParentKey = FRigElementKey(FName(*ParentName), ERigElementType::Bone);

	int32 Added = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		const FString BoneName = FString::Printf(TEXT("%s_%02d"), *ChainName, i);
		FTransform BoneTransform = FTransform::Identity;
		BoneTransform.SetLocation(FVector((float)(i * BoneLength), 0.0f, 0.0f));

		// AddBone 5th param changed from bool to ERigBoneType in UE 5.7
		const FRigElementKey NewKey = Controller->AddBone(FName(*BoneName), ParentKey, BoneTransform, false, ERigBoneType::Imported);
		if (NewKey.IsValid())
		{
			ParentKey = NewKey;
			++Added;
		}
	}

	if (Added == 0)
	{
		Result.Message = FString::Printf(TEXT("add_bone_chain: failed to add any bones for chain '%s'"), *ChainName);
		Result.ErrorCode = 3000;
		return Result;
	}

	CRAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("Chain '%s': %d bones added (%.1f unit spacing)"),
		*ChainName, Added, (float)BoneLength);
	return Result;
}

// ---------------------------------------------------------------------------
// set_ik_goal — set the initial transform of an IK goal control
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_SetIKGoal(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("set_ik_goal"));

	FString AssetPath, ControlName;
	if (!Params->TryGetStringField(TEXT("asset_path"),   AssetPath)   || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("control_name"), ControlName) || ControlName.IsEmpty())
	{
		Result.Message = TEXT("set_ik_goal: 'asset_path' and 'control_name' are required");
		Result.ErrorCode = 1000;
		return Result;
	}

	double X = 0.0, Y = 0.0, Z = 0.0;
	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
	{
		(*PosObj)->TryGetNumberField(TEXT("x"), X);
		(*PosObj)->TryGetNumberField(TEXT("y"), Y);
		(*PosObj)->TryGetNumberField(TEXT("z"), Z);
	}

	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj).IsValid())
	{
		(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
		(*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
	}

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	URigHierarchy* Hierarchy = GetRigHierarchy(CRAsset);
	if (!Hierarchy)
	{
		Result.Message = TEXT("set_ik_goal: could not find Hierarchy on asset");
		Result.ErrorCode = 3000;
		return Result;
	}

	const FRigElementKey ControlKey(FName(*ControlName), ERigElementType::Control);
	if (!Hierarchy->Contains(ControlKey))
	{
		Result.Message = FString::Printf(TEXT("set_ik_goal: no control named '%s' in rig"), *ControlName);
		Result.ErrorCode = 2000;
		return Result;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeAI: Set IK Goal")));

	const FTransform GoalTransform(
		FRotator((float)Pitch, (float)Yaw, (float)Roll),
		FVector((float)X, (float)Y, (float)Z));

	// bInitial=true → sets rest-pose; bAffectChildren=false; bSetupUndo=false (FScopedTransaction handles undo)
	Hierarchy->SetLocalTransform(ControlKey, GoalTransform, true, false, false);

	CRAsset->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(
		TEXT("IK goal '%s' initial transform: pos=(%.1f,%.1f,%.1f) rot=(P=%.1f Y=%.1f R=%.1f)"),
		*ControlName, (float)X, (float)Y, (float)Z, (float)Pitch, (float)Yaw, (float)Roll);
	return Result;
}

// ---------------------------------------------------------------------------
// add_constraint — Python dispatch (RigVM graph authoring required)
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_AddConstraint(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("add_constraint"));
	Result.bSuccess = true;
	Result.Message = TEXT(
		"add_constraint: Control Rig constraints (parent/aim/twist) must be authored as RigVM graph nodes "
		"in the Forwards Solve graph. No direct C++ constraint API exists outside the node system in UE 5.7. "
		"Use add_node with function='/Script/ControlRig.RigUnit_ParentConstraint' (or RigUnit_AimConstraint), "
		"then connect_pins to wire the constraint into the graph.");
	Result.RecoveryHint = TEXT("add_node: function='/Script/ControlRig.RigUnit_ParentConstraint', then connect_pins to wire child/parent transforms.");
	return Result;
}

// ---------------------------------------------------------------------------
// get_rig_topology — serialize full hierarchy to JSON
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_GetRigTopology(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("get_rig_topology"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("get_rig_topology: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UObject* CRAsset = LoadCRAsset(AssetPath, Result);
	if (!CRAsset) return Result;

	URigHierarchy* Hierarchy = GetRigHierarchy(CRAsset);
	if (!Hierarchy)
	{
		Result.Message = TEXT("get_rig_topology: could not find Hierarchy on asset");
		Result.ErrorCode = 3000;
		return Result;
	}

	const TArray<FRigElementKey> AllKeys = Hierarchy->GetAllKeys(true);
	TArray<TSharedPtr<FJsonValue>> Elements;
	Elements.Reserve(AllKeys.Num());

	const UEnum* ElementTypeEnum = StaticEnum<ERigElementType>();

	for (const FRigElementKey& Key : AllKeys)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Key.Name.ToString());

		const FString TypeStr = ElementTypeEnum
			? ElementTypeEnum->GetNameByValue((int64)Key.Type).ToString()
			: FString::FromInt((int32)Key.Type);
		Obj->SetStringField(TEXT("type"), TypeStr);

		const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
		if (ParentKey.IsValid())
			Obj->SetStringField(TEXT("parent"), ParentKey.Name.ToString());

		const FTransform LocalT = Hierarchy->GetLocalTransform(Key, true);
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), LocalT.GetLocation().X);
		LocObj->SetNumberField(TEXT("y"), LocalT.GetLocation().Y);
		LocObj->SetNumberField(TEXT("z"), LocalT.GetLocation().Z);
		Obj->SetObjectField(TEXT("local_position"), LocObj);

		Elements.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),    AssetPath);
	Data->SetNumberField(TEXT("element_count"), (double)Elements.Num());
	Data->SetArrayField(TEXT("elements"),       Elements);

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("get_rig_topology: %d elements in '%s'"), Elements.Num(), *AssetPath);
	Result.ExtraData    = OutStr;
	return Result;
}

// ---------------------------------------------------------------------------
// add_forward_solve_node — adds a RigVM unit node to the ForwardsSolve graph
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_AddForwardSolveNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("add_forward_solve_node"));

	FString AssetPath, FunctionPath;
	double PosX = 0.0, PosY = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("function"),   FunctionPath) || FunctionPath.IsEmpty())
	{
		Result.Message = TEXT("add_forward_solve_node: 'asset_path' and 'function' (UScriptStruct path) required");
		Result.ErrorCode = 1000;
		return Result;
	}
	Params->TryGetNumberField(TEXT("x"), PosX);
	Params->TryGetNumberField(TEXT("y"), PosY);

	UObject* Asset = LoadCRAsset(AssetPath, Result);
	if (!Asset) return Result;

	UControlRigBlueprint* CRBlueprint = Cast<UControlRigBlueprint>(Asset);
	if (!CRBlueprint)
	{
		Result.Message = FString::Printf(TEXT("add_forward_solve_node: asset '%s' is not a UControlRigBlueprint"), *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify asset_path points to a Control Rig Blueprint asset.");
		return Result;
	}

	// UE 5.7: UControlRigBlueprint inherits GetRigVMClient from multiple bases (ambiguous C2385).
	// Qualify through URigVMBlueprint to pick a single overload.
	FRigVMClient* Client = static_cast<URigVMBlueprint*>(CRBlueprint)->GetRigVMClient();
	if (!Client || Client->Num() == 0)
	{
		Result.Message = TEXT("add_forward_solve_node: no FRigVMClient on ControlRig blueprint");
		Result.ErrorCode = 3000;
		return Result;
	}

	URigVMGraph* Graph = Client->GetDefaultModel();
	if (!Graph)
	{
		Result.Message = TEXT("ForwardsSolve graph not found in ControlRig asset");
		Result.ErrorCode = 2000;
		return Result;
	}

	URigVMController* Controller = Client->GetOrCreateController(Graph);
	if (!Controller)
	{
		Result.Message = TEXT("add_forward_solve_node: no RigVMController available for ForwardsSolve graph");
		Result.ErrorCode = 3000;
		return Result;
	}

	// UE 5.7: FindObject<T>(nullptr, Name, bExactClass) is deprecated. Use FindFirstObject.
	UScriptStruct* NodeStruct = FindFirstObject<UScriptStruct>(*FunctionPath, EFindFirstObjectOptions::NativeFirst);
	if (!NodeStruct) NodeStruct = LoadObject<UScriptStruct>(nullptr, *FunctionPath);
	if (!NodeStruct)
	{
		Result.Message = FString::Printf(TEXT("add_forward_solve_node: UScriptStruct not found: '%s'"), *FunctionPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the 'function' path is a valid UScriptStruct (e.g. '/Script/ControlRig.RigUnit_GetTransform').");
		return Result;
	}

	URigVMNode* NewNode = Controller->AddUnitNode(NodeStruct, TEXT("Execute"),
		FVector2D((float)PosX, (float)PosY), FString(), true);

	if (!NewNode)
	{
		Result.Message = FString::Printf(TEXT("add_forward_solve_node: AddUnitNode failed for '%s'"), *FunctionPath);
		Result.ErrorCode = 3000;
		return Result;
	}

	CRBlueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetStringField(TEXT("node_name"), NewNode->GetName());
	FString ExtraStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ExtraStr);
	FJsonSerializer::Serialize(Extra.ToSharedRef(), Writer);

	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("ForwardsSolve node '%s' added at (%.0f, %.0f)"),
		*NewNode->GetName(), PosX, PosY);
	Result.ExtraData    = ExtraStr;
	return Result;
}

// ---------------------------------------------------------------------------
// compile — compile a ControlRig blueprint
// ---------------------------------------------------------------------------

FBridgeResult UControlRigHandler::Action_Compile(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("control_rig"), TEXT("compile"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.Message = TEXT("compile: 'asset_path' is required");
		Result.ErrorCode = 1000;
		return Result;
	}

	UObject* Asset = LoadCRAsset(AssetPath, Result);
	if (!Asset) return Result;

	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
	{
		Result.Message = FString::Printf(TEXT("compile: asset '%s' is not a UBlueprint"), *AssetPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify asset_path points to a Blueprint (e.g. a Control Rig Blueprint).");
		return Result;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	BP->MarkPackageDirty();
	Result.bSuccess     = true;
	Result.AffectedPath = AssetPath;
	Result.Message      = FString::Printf(TEXT("ControlRig blueprint compiled: %s"), *AssetPath);
	return Result;
}
