#include "Handlers/BehaviorTreeHandler.h"
#include "ForgeAISubsystem.h"

// ---- BehaviorTree runtime (AIModule) ---------------------------------------
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_Loop.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "UObject/PropertyIterator.h"

// ---- Asset creation --------------------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---- Misc ------------------------------------------------------------------
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("behavior_tree");

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UBehaviorTreeHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(DOMAIN, Action);
		R.Message = TEXT("Params object is null");
		return R;
	}

	if (Action == TEXT("create_bt"))          return Action_CreateBT(Params);
	if (Action == TEXT("add_node"))           return Action_AddNode(Params);
	if (Action == TEXT("add_decorator"))      return Action_AddDecorator(Params);
	if (Action == TEXT("create_bb"))          return Action_CreateBB(Params);
	if (Action == TEXT("create_blackboard"))  return Action_CreateBB(Params);    // spec name alias
	if (Action == TEXT("add_key"))            return Action_AddKey(Params);
	if (Action == TEXT("add_blackboard_key")) return Action_AddKey(Params);       // spec name alias
	if (Action == TEXT("add_service"))       return Action_AddService(Params);
	if (Action == TEXT("set_node_property")) return Action_SetNodeProperty(Params);
	if (Action == TEXT("link_blackboard"))   return Action_LinkBlackboard(Params);
	if (Action == TEXT("get_tree_topology"))        return Action_GetTreeTopology(Params);
	if (Action == TEXT("add_task_node"))            return Action_AddTaskNode(Params);
	if (Action == TEXT("set_blackboard_key_default")) return Action_SetBlackboardKeyDefault(Params);
	if (Action == TEXT("set_service_interval"))     return Action_SetServiceInterval(Params);
	if (Action == TEXT("remove_node"))              return Action_RemoveNode(Params);
	if (Action == TEXT("remove_decorator"))         return Action_RemoveDecorator(Params);
	if (Action == TEXT("remove_service"))           return Action_RemoveService(Params);
	if (Action == TEXT("remove_blackboard_key"))    return Action_RemoveBlackboardKey(Params);

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(
			TEXT("Unknown behavior_tree action '%s'. Valid: create_bt, add_node, add_decorator, "
			     "create_bb, add_key, add_service, set_node_property, link_blackboard, "
			     "get_tree_topology, add_task_node, set_blackboard_key_default, set_service_interval"),
			*Action));
}

// ---------------------------------------------------------------------------
// create_bt
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_CreateBT(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_bt"), 1000,
			TEXT("create_bt: 'asset_path' is required (e.g. \"/Game/AI/BT_NPC\")"));

	// Split into package folder + asset name (last segment after final '/')
	FString PackagePath, AssetName;
	if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		return MakeError(DOMAIN, TEXT("create_bt"), 1000,
			TEXT("create_bt: 'asset_path' must be a content path like \"/Game/AI/BT_NPC\""));
	}
	PackagePath = PackagePath + TEXT("/");

	// Use IAssetTools to create the asset — this sets up the package and uasset correctly
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UBehaviorTree* BT = Cast<UBehaviorTree>(
		AssetTools.CreateAsset(AssetName, PackagePath, UBehaviorTree::StaticClass(), nullptr));

	if (!BT)
		return MakeError(DOMAIN, TEXT("create_bt"), 3000,
			FString::Printf(TEXT("create_bt: IAssetTools::CreateAsset failed for '%s' in '%s'"),
				*AssetName, *PackagePath));

	// The editor initializes the BTGraph automatically when the asset is first opened.
	// We don't touch graph internals here — the BehaviorTreeEditor graph headers
	// transitively include AIGraph.h which is not available as a standalone include in UE 5.7.
	BT->PostEditChange();
	BT->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(BT);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_name"), AssetName);

	return MakeSuccess(DOMAIN, TEXT("create_bt"),
		FString::Printf(TEXT("BehaviorTree '%s' created at '%s'"), *AssetName, *PackagePath),
		Data);
}

// ---------------------------------------------------------------------------
// add_node  (graph-based)
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_AddNode(TSharedPtr<FJsonObject> Params)
{
	// BehaviorTreeGraph editor headers transitively include AIGraph.h which is unavailable
	// as a standalone header in UE 5.7. All graph-level node manipulation is delegated to
	// the Python handler which accesses the same API via unreal.BehaviorTreeGraph at runtime.
	FString BtPath, NodeType;
	double ParentIndexD = -1.0, PosX = 0.0, PosY = 200.0;

	if (!Params->TryGetStringField(TEXT("bt_path"),   BtPath)   || BtPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("node_type"), NodeType)  || NodeType.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_node"), 1000,
			TEXT("add_node: 'bt_path' and 'node_type' are required"));

	Params->TryGetNumberField(TEXT("parent_index"), ParentIndexD);
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	const int32 ParentIndex = (int32)ParentIndexD;

	if (!Subsystem)
		return MakeError(DOMAIN, TEXT("add_node"), 3003, TEXT("add_node: Subsystem unavailable"));

	UBridgeHandlerBase* PyHandler = Subsystem->GetHandler(TEXT("python"));
	if (!PyHandler)
		return MakeError(DOMAIN, TEXT("add_node"), 3003,
			TEXT("add_node: Python handler unavailable — enable the python domain"));

	// Map friendly names to full BT class names for the Python script
	FString CompositeClass, TaskClass;
	if      (NodeType == TEXT("Selector"))       CompositeClass = TEXT("BTComposite_Selector");
	else if (NodeType == TEXT("Sequence"))       CompositeClass = TEXT("BTComposite_Sequence");
	else if (NodeType == TEXT("SimpleParallel")) CompositeClass = TEXT("BTComposite_SimpleParallel");
	else if (NodeType == TEXT("Wait"))           TaskClass      = TEXT("BTTask_Wait");
	else if (NodeType == TEXT("MoveTo"))         TaskClass      = TEXT("BTTask_MoveTo");
	else if (NodeType == TEXT("RunBehavior"))    TaskClass      = TEXT("BTTask_RunBehavior");
	else
	{
		// Assume caller passed a raw class name suffix or full path
		TaskClass = NodeType;
	}

	const bool bIsComposite = !CompositeClass.IsEmpty();
	const FString NodeClass  = bIsComposite ? CompositeClass : TaskClass;
	const FString GraphNodeClass = bIsComposite
		? TEXT("BehaviorTreeGraphNode_Composite")
		: TEXT("BehaviorTreeGraphNode_Task");

	FString Script = FString::Printf(
		TEXT("import unreal\n")
		TEXT("bt = unreal.load_asset('%s')\n")
		TEXT("if not bt: raise RuntimeError('BT not found: %s')\n")
		TEXT("g = bt.bt_graph\n")
		TEXT("if not g: raise RuntimeError('BT has no editor graph — open it in the editor once first')\n")
		TEXT("node_cls = unreal.find_class('%s')\n")
		TEXT("if not node_cls: raise RuntimeError('Node class not found: %s')\n")
		TEXT("gnode_cls = unreal.find_class('%s')\n")
		TEXT("gnode = unreal.new_object(gnode_cls, g)\n")
		TEXT("gnode.node_instance = unreal.new_object(node_cls, bt)\n")
		TEXT("gnode.node_pos_x = %d\n")
		TEXT("gnode.node_pos_y = %d\n")
		TEXT("g.add_node(gnode)\n")
		TEXT("gnode.allocate_default_pins()\n")
		TEXT("# wire to parent\n")
		TEXT("parent_idx = %d\n")
		TEXT("nodes_with_inst = [n for n in g.nodes if hasattr(n,'node_instance') and n.node_instance]\n")
		TEXT("if parent_idx < 0:\n")
		TEXT("    roots = [n for n in g.nodes if type(n).__name__ == 'BehaviorTreeGraphNode_Root']\n")
		TEXT("    parent = roots[0] if roots else None\n")
		TEXT("else:\n")
		TEXT("    parent = nodes_with_inst[parent_idx] if parent_idx < len(nodes_with_inst) else None\n")
		TEXT("if parent:\n")
		TEXT("    out_pins = [p for p in parent.pins if p.direction == unreal.PinDirection.OUTPUT]\n")
		TEXT("    in_pins  = [p for p in gnode.pins  if p.direction == unreal.PinDirection.INPUT]\n")
		TEXT("    if out_pins and in_pins:\n")
		TEXT("        g.schema.try_create_connection(out_pins[0], in_pins[0])\n")
		TEXT("g.update_asset()\n")
		TEXT("bt.mark_package_dirty()\n")
		TEXT("new_idx = nodes_with_inst.index(gnode) if gnode in nodes_with_inst else len(nodes_with_inst)\n")
		TEXT("print(f'add_node OK: {node_cls.get_name()} index={new_idx}')\n"),
		*BtPath, *BtPath,
		*NodeClass, *NodeClass,
		*GraphNodeClass,
		(int32)PosX, (int32)PosY,
		ParentIndex
	);

	TSharedPtr<FJsonObject> PyParams = MakeShared<FJsonObject>();
	PyParams->SetStringField(TEXT("script"), Script);
	FBridgeResult PyResult = PyHandler->HandleCommand(TEXT("execute_script"), PyParams);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_type"),  NodeType);
	Data->SetStringField(TEXT("bt_path"),    BtPath);
	Data->SetNumberField(TEXT("pos_x"),      PosX);
	Data->SetNumberField(TEXT("pos_y"),      PosY);
	Data->SetNumberField(TEXT("parent_index"), ParentIndex);
	Data->SetBoolField  (TEXT("via_python"), true);
	Data->SetBoolField  (TEXT("py_success"), PyResult.bSuccess);

	return MakeSuccess(DOMAIN, TEXT("add_node"),
		FString::Printf(TEXT("add_node '%s' in '%s' delegated to Python (success=%s)"),
			*NodeType, *BtPath, PyResult.bSuccess ? TEXT("true") : TEXT("false")),
		Data);
}

// ---------------------------------------------------------------------------
// add_decorator  — delegate to Python (graph sub-node wiring is editor-internal)
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_AddDecorator(TSharedPtr<FJsonObject> Params)
{
	FString BtPath, DecoratorType, BbKey;
	double NodeIndexD = 0.0;

	if (!Params->TryGetStringField(TEXT("bt_path"),        BtPath)        || BtPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("decorator_type"), DecoratorType)  || DecoratorType.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_decorator"), 1000,
			TEXT("add_decorator: 'bt_path' and 'decorator_type' are required"));

	Params->TryGetNumberField(TEXT("node_index"), NodeIndexD);
	Params->TryGetStringField(TEXT("bb_key"),     BbKey);
	const int32 NodeIndex = (int32)NodeIndexD;

	// Delegate to Python — the BT graph sub-node attachment requires editor internals
	// that are more robustly handled via unreal Python API.
	if (Subsystem)
	{
		UBridgeHandlerBase* PyHandler = Subsystem->GetHandler(TEXT("python"));
		if (PyHandler)
		{
			FString Script = FString::Printf(
				TEXT("import unreal\n")
				TEXT("bt_asset = unreal.load_asset('%s')\n")
				TEXT("if bt_asset:\n")
				TEXT("    bt_graph = bt_asset.bt_graph\n")
				TEXT("    nodes = [n for n in bt_graph.nodes if hasattr(n, 'node_instance') and n.node_instance]\n")
				TEXT("    target = nodes[%d] if %d < len(nodes) else None\n")
				TEXT("    if target and hasattr(target, 'decorators'):\n")
				TEXT("        dec_class = unreal.find_class('BTDecorator_%s')\n")
				TEXT("        if dec_class:\n")
				TEXT("            dec_node = unreal.new_object(unreal.BehaviorTreeGraphNode_Decorator, bt_graph)\n")
				TEXT("            dec_inst = unreal.new_object(dec_class, bt_asset)\n")
				TEXT("            dec_node.node_instance = dec_inst\n")
				TEXT("            bb_key = '%s'\n")
				TEXT("            if bb_key:\n")
				TEXT("                try:\n")
				TEXT("                    sel = dec_inst.get_editor_property('blackboard_key')\n")
				TEXT("                    sel.selected_key_name = bb_key\n")
				TEXT("                    dec_inst.set_editor_property('blackboard_key', sel)\n")
				TEXT("                except Exception as e:\n")
				TEXT("                    print('WARN: could not set bb_key:', e)\n")
				TEXT("            target.decorators.append(dec_node)\n")
				TEXT("            bt_graph.update_asset()\n")
				TEXT("            print('decorator %s added to node %d (bb_key=%s)')\n")
				TEXT("        else:\n")
				TEXT("            print('ERROR: dec class not found: BTDecorator_%s')\n")
				TEXT("    else:\n")
				TEXT("        print('ERROR: node %d not found or no decorators attr')\n")
				TEXT("else:\n")
				TEXT("    print('ERROR: bt asset not found at %s')\n"),
				*BtPath,
				NodeIndex, NodeIndex,
				*DecoratorType,
				*BbKey,
				*DecoratorType, NodeIndex, *BbKey,
				*DecoratorType,
				NodeIndex,
				*BtPath
			);

			TSharedPtr<FJsonObject> PyParams = MakeShared<FJsonObject>();
			PyParams->SetStringField(TEXT("script"), Script);
			FBridgeResult PyResult = PyHandler->HandleCommand(TEXT("execute_script"), PyParams);

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("bt_path"),        BtPath);
			Data->SetStringField(TEXT("decorator_type"), DecoratorType);
			Data->SetNumberField(TEXT("node_index"),     NodeIndex);
			Data->SetBoolField  (TEXT("via_python"),     true);
			Data->SetBoolField  (TEXT("py_success"),     PyResult.bSuccess);

			return MakeSuccess(DOMAIN, TEXT("add_decorator"),
				FString::Printf(TEXT("add_decorator '%s' on node %d in '%s' delegated to Python"),
					*DecoratorType, NodeIndex, *BtPath),
				Data);
		}
	}

	// Fallback: direct runtime attachment (works at runtime but not visible in editor graph)
	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("add_decorator"));
	UBehaviorTree* BT = LoadBTAsset(BtPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("add_decorator"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));
	if (!BT->RootNode || BT->RootNode->Children.Num() == 0)
		return MakeError(DOMAIN, TEXT("add_decorator"), 3000,
			FString::Printf(TEXT("add_decorator: BT '%s' has no root or children"), *BtPath));

	UClass* DecClass = nullptr;
	if      (DecoratorType == TEXT("Blackboard"))  DecClass = UBTDecorator_Blackboard::StaticClass();
	else if (DecoratorType == TEXT("Loop"))         DecClass = UBTDecorator_Loop::StaticClass();
	else if (DecoratorType == TEXT("ForceSuccess")) DecClass = UBTDecorator_ForceSuccess::StaticClass();
	else
	{
		FString FullName = TEXT("/Script/AIModule.BTDecorator_") + DecoratorType;
		DecClass = FindObject<UClass>(nullptr, *FullName);
		if (!DecClass) DecClass = LoadObject<UClass>(nullptr, *FullName);
	}
	if (!DecClass)
		return MakeError(DOMAIN, TEXT("add_decorator"), 1001,
			FString::Printf(TEXT("add_decorator: unknown decorator_type '%s'"), *DecoratorType),
			TEXT("Use: Blackboard, Loop, ForceSuccess, or any full class path (e.g., /Script/AIModule.BTDecorator_Cooldown). All BTDecorator_* classes in /Script/AIModule are supported."));

	UBTDecorator* NewDec = NewObject<UBTDecorator>(BT, DecClass);
	if (!NewDec)
		return MakeError(DOMAIN, TEXT("add_decorator"), 3000,
			FString::Printf(TEXT("add_decorator: failed to create decorator '%s'"), *DecoratorType));

	// Wire bb_key to the decorator's BlackboardKey selector if provided.
	if (!BbKey.IsEmpty())
	{
		if (FStructProperty* KeyProp = FindFProperty<FStructProperty>(NewDec->GetClass(), TEXT("BlackboardKey")))
		{
			// FBlackboardKeySelector has a SelectedKeyName FName field.
			void* KeyPtr = KeyProp->ContainerPtrToValuePtr<void>(NewDec);
			if (FNameProperty* NameProp = FindFProperty<FNameProperty>(KeyProp->Struct, TEXT("SelectedKeyName")))
			{
				NameProp->SetPropertyValue_InContainer(KeyPtr, FName(*BbKey));
			}
		}
	}

	const int32 ChildIdx = FMath::Clamp(NodeIndex, 0, BT->RootNode->Children.Num() - 1);
	BT->RootNode->Children[ChildIdx].Decorators.Add(NewDec);
	BT->MarkPackageDirty();

	return MakeSuccess(DOMAIN, TEXT("add_decorator"),
		FString::Printf(TEXT("Added %s decorator to child %d of '%s' (runtime fallback — Python unavailable)"),
			*DecoratorType, ChildIdx, *BtPath));
}

// ---------------------------------------------------------------------------
// create_bb
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_CreateBB(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("create_bb"), 1000,
			TEXT("create_bb: 'asset_path' is required (e.g. \"/Game/AI/BB_NPC\")"));

	FString PackagePath, AssetName;
	if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		return MakeError(DOMAIN, TEXT("create_bb"), 1000,
			TEXT("create_bb: 'asset_path' must be a content path like \"/Game/AI/BB_NPC\""));
	PackagePath = PackagePath + TEXT("/");

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UBlackboardData* BbData = Cast<UBlackboardData>(
		AssetTools.CreateAsset(AssetName, PackagePath, UBlackboardData::StaticClass(), nullptr));

	if (!BbData)
		return MakeError(DOMAIN, TEXT("create_bb"), 3000,
			FString::Printf(TEXT("create_bb: IAssetTools::CreateAsset failed for '%s' in '%s'"),
				*AssetName, *PackagePath));

	BbData->PostEditChange();
	BbData->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(BbData);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_name"), AssetName);

	return MakeSuccess(DOMAIN, TEXT("create_bb"),
		FString::Printf(TEXT("BlackboardData '%s' created at '%s'"), *AssetName, *PackagePath),
		Data);
}

// ---------------------------------------------------------------------------
// add_key
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_AddKey(TSharedPtr<FJsonObject> Params)
{
	FString BbPath, KeyName, KeyType;
	if (!Params->TryGetStringField(TEXT("bb_path"),  BbPath)  || BbPath.IsEmpty()  ||
	    !Params->TryGetStringField(TEXT("key_name"), KeyName)  || KeyName.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("key_type"), KeyType)  || KeyType.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_key"), 1000,
			TEXT("add_key: 'bb_path', 'key_name', and 'key_type' are required"));

	UBlackboardData* BbData = LoadObject<UBlackboardData>(nullptr, *BbPath);
	if (!BbData)
	{
		const FString Suffix = BbPath + TEXT(".") + FPackageName::GetLongPackageAssetName(BbPath);
		BbData = LoadObject<UBlackboardData>(nullptr, *Suffix);
	}
	if (!BbData)
		return MakeError(DOMAIN, TEXT("add_key"), 2000,
			FString::Printf(TEXT("add_key: no UBlackboardData found at '%s'"), *BbPath),
			TEXT("Create the blackboard first with 'create_bb'."));

	// Idempotent — skip if key already exists
	for (const FBlackboardEntry& Existing : BbData->Keys)
	{
		if (Existing.EntryName == FName(*KeyName))
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("key_name"),  KeyName);
			Data->SetStringField(TEXT("bb_path"),   BbPath);
			Data->SetBoolField  (TEXT("pre_existing"), true);
			return MakeSuccess(DOMAIN, TEXT("add_key"),
				FString::Printf(TEXT("add_key: key '%s' already exists in '%s' (no-op)"), *KeyName, *BbPath),
				Data);
		}
	}

	// Map key_type string to UBlackboardKeyType subclass
	UBlackboardKeyType* KeyTypeObj = nullptr;
	if      (KeyType == TEXT("bool"))    KeyTypeObj = NewObject<UBlackboardKeyType_Bool>   (BbData);
	else if (KeyType == TEXT("float"))   KeyTypeObj = NewObject<UBlackboardKeyType_Float>  (BbData);
	else if (KeyType == TEXT("int"))     KeyTypeObj = NewObject<UBlackboardKeyType_Int>    (BbData);
	else if (KeyType == TEXT("string"))  KeyTypeObj = NewObject<UBlackboardKeyType_String> (BbData);
	else if (KeyType == TEXT("name"))    KeyTypeObj = NewObject<UBlackboardKeyType_Name>   (BbData);
	else if (KeyType == TEXT("vector"))  KeyTypeObj = NewObject<UBlackboardKeyType_Vector> (BbData);
	else if (KeyType == TEXT("rotator")) KeyTypeObj = NewObject<UBlackboardKeyType_Rotator>(BbData);
	else if (KeyType == TEXT("object"))  KeyTypeObj = NewObject<UBlackboardKeyType_Object> (BbData);
	else if (KeyType == TEXT("class"))   KeyTypeObj = NewObject<UBlackboardKeyType_Class>  (BbData);
	else if (KeyType == TEXT("enum"))
	{
		FString EnumClassPath;
		if (!Params->TryGetStringField(TEXT("enum_class"), EnumClassPath) || EnumClassPath.IsEmpty())
			return MakeError(DOMAIN, TEXT("add_key"), 1000,
				TEXT("add_key: 'enum' keys require 'enum_class' param (content path to a UUserDefinedEnum)"));

		UEnum* EnumAsset = LoadObject<UEnum>(nullptr, *EnumClassPath);
		if (!EnumAsset)
			return MakeError(DOMAIN, TEXT("add_key"), 2000,
				FString::Printf(TEXT("add_key: enum asset not found at '%s'"), *EnumClassPath));

		UBlackboardKeyType_Enum* EnumKey = NewObject<UBlackboardKeyType_Enum>(BbData);
		EnumKey->EnumType = EnumAsset;
		EnumKey->EnumName = EnumAsset->GetName();
		KeyTypeObj = EnumKey;
	}
	else
		return MakeError(DOMAIN, TEXT("add_key"), 1001,
			FString::Printf(TEXT("add_key: unknown key_type '%s'"), *KeyType),
			TEXT("Valid types: bool, float, int, string, name, vector, rotator, object, class, enum"));

	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(*KeyName);
	NewEntry.KeyType   = KeyTypeObj;
	BbData->Keys.Add(NewEntry);
	BbData->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("key_name"), KeyName);
	Data->SetStringField(TEXT("key_type"), KeyType);
	Data->SetStringField(TEXT("bb_path"),  BbPath);

	return MakeSuccess(DOMAIN, TEXT("add_key"),
		FString::Printf(TEXT("Key '%s' (%s) added to blackboard '%s'"), *KeyName, *KeyType, *BbPath),
		Data);
}

// ---------------------------------------------------------------------------
// add_service  — delegate to Python (graph sub-node wiring is editor-internal)
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_AddService(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ServiceClass;
	double ParentNodeIndexD = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_service"), 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetStringField(TEXT("service_class"), ServiceClass) || ServiceClass.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_service"), 1000, TEXT("Missing required param: 'service_class'"));
	Params->TryGetNumberField(TEXT("parent_node_index"), ParentNodeIndexD);
	const int32 ParentNodeIndex = (int32)ParentNodeIndexD;

	// Delegate to Python for graph-level service attachment
	if (Subsystem)
	{
		UBridgeHandlerBase* PyHandler = Subsystem->GetHandler(TEXT("python"));
		if (PyHandler)
		{
			FString Script = FString::Printf(
				TEXT("import unreal\n")
				TEXT("bt_asset = unreal.load_asset('%s')\n")
				TEXT("if bt_asset:\n")
				TEXT("    bt_graph = bt_asset.bt_graph\n")
				TEXT("    nodes = [n for n in bt_graph.nodes if hasattr(n, 'node_instance') and n.node_instance]\n")
				TEXT("    target = nodes[%d] if %d < len(nodes) else None\n")
				TEXT("    if target and hasattr(target, 'services'):\n")
				TEXT("        svc_class = unreal.find_class('%s') or unreal.find_class('BTService_%s')\n")
				TEXT("        if svc_class:\n")
				TEXT("            svc_node = unreal.new_object(unreal.BehaviorTreeGraphNode_Service, bt_graph)\n")
				TEXT("            svc_node.node_instance = unreal.new_object(svc_class, bt_asset)\n")
				TEXT("            target.services.append(svc_node)\n")
				TEXT("            bt_graph.update_asset()\n")
				TEXT("            print('service %s added to node %d')\n")
				TEXT("        else:\n")
				TEXT("            print('ERROR: service class not found: %s')\n")
				TEXT("    else:\n")
				TEXT("        print('ERROR: node %d not found or no services attr')\n")
				TEXT("else:\n")
				TEXT("    print('ERROR: bt asset not found at %s')\n"),
				*AssetPath,
				ParentNodeIndex, ParentNodeIndex,
				*ServiceClass, *ServiceClass,
				*ServiceClass, ParentNodeIndex,
				*ServiceClass,
				ParentNodeIndex,
				*AssetPath
			);

			TSharedPtr<FJsonObject> PyParams = MakeShared<FJsonObject>();
			PyParams->SetStringField(TEXT("script"), Script);
			FBridgeResult PyResult = PyHandler->HandleCommand(TEXT("execute_script"), PyParams);

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset_path"),        AssetPath);
			Data->SetStringField(TEXT("service_class"),     ServiceClass);
			Data->SetNumberField(TEXT("parent_node_index"), ParentNodeIndex);
			Data->SetBoolField  (TEXT("via_python"),        true);
			Data->SetBoolField  (TEXT("py_success"),        PyResult.bSuccess);

			return MakeSuccess(DOMAIN, TEXT("add_service"),
				FString::Printf(TEXT("add_service '%s' on node %d in '%s' delegated to Python"),
					*ServiceClass, ParentNodeIndex, *AssetPath),
				Data);
		}
	}

	// Fallback: direct runtime attachment
	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("add_service"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("add_service"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));
	if (!BT->RootNode)
		return MakeError(DOMAIN, TEXT("add_service"), 3000,
			FString::Printf(TEXT("BT '%s' has no root node"), *AssetPath),
			TEXT("Add a composite node first with 'add_node'."));

	TArray<UBTNode*> AllNodes;
	CollectNodesDFS(BT->RootNode, AllNodes);

	if (ParentNodeIndex < 0 || ParentNodeIndex >= AllNodes.Num())
		return MakeError(DOMAIN, TEXT("add_service"), 1001,
			FString::Printf(TEXT("parent_node_index %d out of range (0..%d)"), ParentNodeIndex, AllNodes.Num() - 1),
			TEXT("Use 'get_tree_topology' to see valid node indices."));

	UBTCompositeNode* ParentComp = Cast<UBTCompositeNode>(AllNodes[ParentNodeIndex]);
	if (!ParentComp)
		return MakeError(DOMAIN, TEXT("add_service"), 1001,
			FString::Printf(TEXT("Node at index %d is not a composite — services attach to composites only"),
				ParentNodeIndex));

	// Resolve service class
	UClass* SvcClass = FindObject<UClass>(nullptr, *ServiceClass);
	if (!SvcClass)
	{
		FString FullName = TEXT("/Script/AIModule.BTService_") + ServiceClass;
		SvcClass = FindObject<UClass>(nullptr, *FullName);
	}
	if (!SvcClass)
		SvcClass = LoadObject<UClass>(nullptr, *ServiceClass);
	if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
		return MakeError(DOMAIN, TEXT("add_service"), 2000,
			FString::Printf(TEXT("Service class '%s' not found or not a UBTService subclass"), *ServiceClass),
			TEXT("Use the full class path or just the suffix name (e.g. 'DefaultFocus')."));

	UBTService* NewService = NewObject<UBTService>(BT, SvcClass);
	if (!NewService)
		return MakeError(DOMAIN, TEXT("add_service"), 3000,
			FString::Printf(TEXT("Failed to create service of class '%s'"), *ServiceClass));

	ParentComp->Services.Add(NewService);
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("parent_node_index"), ParentNodeIndex);
	Data->SetStringField(TEXT("service_class"),     SvcClass->GetName());
	Data->SetNumberField(TEXT("service_count"),     ParentComp->Services.Num());

	return MakeSuccess(DOMAIN, TEXT("add_service"),
		FString::Printf(TEXT("Service '%s' added to node '%s' (index %d) in '%s' (runtime fallback — Python unavailable)"),
			*SvcClass->GetName(), *ParentComp->GetNodeName(), ParentNodeIndex, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_node_property
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_SetNodeProperty(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, PropertyName, ValueStr;
	double NodeIndexD = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_node_property"), 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_node_property"), 1000, TEXT("Missing required param: 'property'"));
	if (!Params->TryGetStringField(TEXT("value"), ValueStr))
		return MakeError(DOMAIN, TEXT("set_node_property"), 1000, TEXT("Missing required param: 'value'"));
	Params->TryGetNumberField(TEXT("node_index"), NodeIndexD);
	const int32 NodeIndex = (int32)NodeIndexD;

	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("set_node_property"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("set_node_property"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));
	if (!BT->RootNode)
		return MakeError(DOMAIN, TEXT("set_node_property"), 3000,
			FString::Printf(TEXT("BT '%s' has no root node"), *AssetPath));

	TArray<UBTNode*> AllNodes;
	CollectNodesDFS(BT->RootNode, AllNodes);

	if (NodeIndex < 0 || NodeIndex >= AllNodes.Num())
		return MakeError(DOMAIN, TEXT("set_node_property"), 1001,
			FString::Printf(TEXT("node_index %d out of range (0..%d)"), NodeIndex, AllNodes.Num() - 1),
			TEXT("Use 'get_tree_topology' to see valid node indices."));

	UBTNode* TargetNode = AllNodes[NodeIndex];

	FProperty* Prop = FindFProperty<FProperty>(TargetNode->GetClass(), *PropertyName);
	if (!Prop)
		return MakeError(DOMAIN, TEXT("set_node_property"), 1001,
			FString::Printf(TEXT("Property '%s' not found on node '%s' (class %s)"),
				*PropertyName, *TargetNode->GetNodeName(), *TargetNode->GetClass()->GetName()),
			TEXT("Check the node class for available UPROPERTY names."));

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetNode);

	// Type-specific setters for common types, generic ImportText fallback
	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		FloatProp->SetPropertyValue(ValuePtr, FCString::Atof(*ValueStr));
	}
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		DoubleProp->SetPropertyValue(ValuePtr, (double)FCString::Atof(*ValueStr));
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		IntProp->SetPropertyValue(ValuePtr, FCString::Atoi(*ValueStr));
	}
	else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		BoolProp->SetPropertyValue(ValuePtr, ValueStr.ToBool());
	}
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		StrProp->SetPropertyValue(ValuePtr, ValueStr);
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		NameProp->SetPropertyValue(ValuePtr, FName(*ValueStr));
	}
	else
	{
		Prop->ImportText_Direct(*ValueStr, ValuePtr, TargetNode, PPF_None);
	}

	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("node_index"), NodeIndex);
	Data->SetStringField(TEXT("node_name"),  TargetNode->GetNodeName());
	Data->SetStringField(TEXT("property"),   PropertyName);
	Data->SetStringField(TEXT("value"),      ValueStr);

	return MakeSuccess(DOMAIN, TEXT("set_node_property"),
		FString::Printf(TEXT("Set '%s' = '%s' on node '%s' (index %d) in '%s'"),
			*PropertyName, *ValueStr, *TargetNode->GetNodeName(), NodeIndex, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// link_blackboard
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_LinkBlackboard(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, BlackboardPath;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("link_blackboard"), 1000, TEXT("Missing required param: 'asset_path'"));
	if (!Params->TryGetStringField(TEXT("blackboard_path"), BlackboardPath) || BlackboardPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("link_blackboard"), 1000, TEXT("Missing required param: 'blackboard_path'"));

	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("link_blackboard"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("link_blackboard"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));

	UBlackboardData* BbData = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
	if (!BbData)
	{
		const FString Suffix = BlackboardPath + TEXT(".") + FPackageName::GetLongPackageAssetName(BlackboardPath);
		BbData = LoadObject<UBlackboardData>(nullptr, *Suffix);
	}
	if (!BbData)
		return MakeError(DOMAIN, TEXT("link_blackboard"), 2000,
			FString::Printf(TEXT("No UBlackboardData found at '%s'"), *BlackboardPath),
			TEXT("Create the blackboard first with 'create_bb'."));

	BT->BlackboardAsset = BbData;
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("behavior_tree"), AssetPath);
	Data->SetStringField(TEXT("blackboard"),    BlackboardPath);

	return MakeSuccess(DOMAIN, TEXT("link_blackboard"),
		FString::Printf(TEXT("Linked blackboard '%s' to BehaviorTree '%s'"), *BlackboardPath, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// get_tree_topology
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_GetTreeTopology(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("get_tree_topology"), 1000, TEXT("Missing required param: 'asset_path'"));

	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("get_tree_topology"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("get_tree_topology"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	if (BT->RootNode)
	{
		TSharedPtr<FJsonObject> RootJson = BuildNodeJson(BT->RootNode);
		Data->SetObjectField(TEXT("root"), RootJson);

		TArray<UBTNode*> AllNodes;
		CollectNodesDFS(BT->RootNode, AllNodes);
		Data->SetNumberField(TEXT("total_nodes"), AllNodes.Num());
	}
	else
	{
		Data->SetStringField(TEXT("root"), TEXT("null"));
		Data->SetNumberField(TEXT("total_nodes"), 0);
	}

	if (BT->BlackboardAsset)
	{
		TSharedPtr<FJsonObject> BbObj = MakeShared<FJsonObject>();
		BbObj->SetStringField(TEXT("name"),      BT->BlackboardAsset->GetName());
		BbObj->SetStringField(TEXT("path"),      BT->BlackboardAsset->GetPathName());
		BbObj->SetNumberField(TEXT("key_count"), BT->BlackboardAsset->Keys.Num());

		TArray<TSharedPtr<FJsonValue>> KeysArray;
		for (const FBlackboardEntry& Entry : BT->BlackboardAsset->Keys)
		{
			TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
			KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			KeyObj->SetStringField(TEXT("type"),
				Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("null"));
			KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		BbObj->SetArrayField(TEXT("keys"), KeysArray);
		Data->SetObjectField(TEXT("blackboard"), BbObj);
	}

	return MakeSuccess(DOMAIN, TEXT("get_tree_topology"),
		FString::Printf(TEXT("Tree topology for '%s'"), *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// add_task_node  — attach a BTTask leaf node to the tree's root composite
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_AddTaskNode(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, TaskClass, NodeLabel;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_task_node"), 1000,
			TEXT("add_task_node: 'asset_path' is required (BehaviorTree content path)"));
	if (!Params->TryGetStringField(TEXT("task_class"), TaskClass) || TaskClass.IsEmpty())
		return MakeError(DOMAIN, TEXT("add_task_node"), 1000,
			TEXT("add_task_node: 'task_class' is required (e.g. \"BTTask_BlueprintBase\" or full path)"));
	Params->TryGetStringField(TEXT("node_label"), NodeLabel);

	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("add_task_node"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("add_task_node"), 2000, LoadResult.Message,
			TEXT("Create the BehaviorTree first with 'create_bt'."));
	if (!BT->RootNode)
		return MakeError(DOMAIN, TEXT("add_task_node"), 3000,
			FString::Printf(TEXT("add_task_node: BT '%s' has no root composite node"), *AssetPath),
			TEXT("Add a Selector or Sequence first with 'add_node'."));

	// Resolve task class — try short name under AIModule, then full path, then raw
	UClass* TaskUClass = nullptr;
	{
		FString FullAIPath = TEXT("/Script/AIModule.") + TaskClass;
		TaskUClass = FindObject<UClass>(nullptr, *FullAIPath);
	}
	if (!TaskUClass)
		TaskUClass = FindObject<UClass>(nullptr, *TaskClass);
	if (!TaskUClass)
		TaskUClass = LoadObject<UClass>(nullptr, *TaskClass);
	if (!TaskUClass || !TaskUClass->IsChildOf(UBTTaskNode::StaticClass()))
		return MakeError(DOMAIN, TEXT("add_task_node"), 2000,
			FString::Printf(TEXT("add_task_node: class '%s' not found or not a UBTTaskNode subclass"), *TaskClass),
			TEXT("Use the short class name (e.g. BTTask_Wait) or a full content path for Blueprint tasks."));

	// Create the task node owned by the BT asset
	UBTTaskNode* NewTask = NewObject<UBTTaskNode>(BT, TaskUClass);
	if (!NewTask)
		return MakeError(DOMAIN, TEXT("add_task_node"), 3000,
			FString::Printf(TEXT("add_task_node: NewObject failed for class '%s'"), *TaskClass));

	if (!NodeLabel.IsEmpty())
		NewTask->NodeName = NodeLabel;

	// Attach to the root composite by adding a child entry
	// BT->RootNode is the root UBTCompositeNode — walk to its first composite child if present,
	// otherwise append directly to RootNode.
	UBTCompositeNode* TargetComposite = BT->RootNode;
	if (BT->RootNode->Children.Num() > 0 && BT->RootNode->Children[0].ChildComposite)
		TargetComposite = BT->RootNode->Children[0].ChildComposite;

	FBTCompositeChild NewChild;
	NewChild.ChildTask = NewTask;
	TargetComposite->Children.Add(NewChild);

	BT->MarkPackageDirty();

	const FString EffectiveLabel = NodeLabel.IsEmpty() ? NewTask->GetNodeName() : NodeLabel;
	const int32 NewChildIndex = TargetComposite->Children.Num() - 1;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),       AssetPath);
	Data->SetStringField(TEXT("task_class"),        TaskUClass->GetName());
	Data->SetStringField(TEXT("node_label"),        EffectiveLabel);
	Data->SetStringField(TEXT("parent_composite"),  TargetComposite->GetNodeName());
	Data->SetNumberField(TEXT("child_index"),        NewChildIndex);

	return MakeSuccess(DOMAIN, TEXT("add_task_node"),
		FString::Printf(TEXT("Task '%s' (%s) added to composite '%s' (child index %d) in '%s'"),
			*EffectiveLabel, *TaskUClass->GetName(), *TargetComposite->GetNodeName(),
			NewChildIndex, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_blackboard_key_default
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_SetBlackboardKeyDefault(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, KeyName, DefaultValue;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 1000,
			TEXT("set_blackboard_key_default: 'asset_path' is required (BlackboardData content path)"));
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 1000,
			TEXT("set_blackboard_key_default: 'key_name' is required"));
	if (!Params->TryGetStringField(TEXT("default_value"), DefaultValue))
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 1000,
			TEXT("set_blackboard_key_default: 'default_value' is required"));

	UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *AssetPath);
	if (!BB)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BB = LoadObject<UBlackboardData>(nullptr, *Suffix);
	}
	if (!BB)
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 2000,
			FString::Printf(TEXT("set_blackboard_key_default: no UBlackboardData found at '%s'"), *AssetPath),
			TEXT("Create the blackboard first with 'create_bb'."));

	// Find the key by name
	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName.ToString() == KeyName)
		{
			FoundEntry = &Entry;
			break;
		}
	}
	if (!FoundEntry)
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 3000,
			FString::Printf(TEXT("set_blackboard_key_default: key '%s' not found in '%s'"), *KeyName, *AssetPath),
			TEXT("Add the key first with 'add_key'."));

	if (!FoundEntry->KeyType)
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 3000,
			FString::Printf(TEXT("set_blackboard_key_default: key '%s' has no KeyType set"), *KeyName));

	const FString TypeName = FoundEntry->KeyType->GetClass()->GetName();
	FString AppliedNote;

	// Type-specific default application via reflection on the KeyType UObject
	if (TypeName == TEXT("BlackboardKeyType_Bool"))
	{
		FProperty* Prop = FindFProperty<FProperty>(FoundEntry->KeyType->GetClass(), TEXT("DefaultValue"));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundEntry->KeyType);
			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				BoolProp->SetPropertyValue(ValuePtr, DefaultValue.ToBool());
		}
		AppliedNote = TEXT("bool");
	}
	else if (TypeName == TEXT("BlackboardKeyType_Float"))
	{
		FProperty* Prop = FindFProperty<FProperty>(FoundEntry->KeyType->GetClass(), TEXT("DefaultValue"));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundEntry->KeyType);
			if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				FloatProp->SetPropertyValue(ValuePtr, FCString::Atof(*DefaultValue));
			else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				DoubleProp->SetPropertyValue(ValuePtr, (double)FCString::Atof(*DefaultValue));
		}
		AppliedNote = TEXT("float");
	}
	else if (TypeName == TEXT("BlackboardKeyType_Int"))
	{
		FProperty* Prop = FindFProperty<FProperty>(FoundEntry->KeyType->GetClass(), TEXT("DefaultValue"));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundEntry->KeyType);
			if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				IntProp->SetPropertyValue(ValuePtr, FCString::Atoi(*DefaultValue));
		}
		AppliedNote = TEXT("int");
	}
	else if (TypeName == TEXT("BlackboardKeyType_String"))
	{
		FProperty* Prop = FindFProperty<FProperty>(FoundEntry->KeyType->GetClass(), TEXT("DefaultValue"));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundEntry->KeyType);
			if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				StrProp->SetPropertyValue(ValuePtr, DefaultValue);
		}
		AppliedNote = TEXT("string");
	}
	else if (TypeName == TEXT("BlackboardKeyType_Name"))
	{
		FProperty* Prop = FindFProperty<FProperty>(FoundEntry->KeyType->GetClass(), TEXT("DefaultValue"));
		if (Prop)
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundEntry->KeyType);
			if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				NameProp->SetPropertyValue(ValuePtr, FName(*DefaultValue));
		}
		AppliedNote = TEXT("name");
	}
	else
	{
		// Vector and Object keys do not have a simple settable DefaultValue in UE 5.7 via C++ reflection.
		// Return a structured error so clients can route to a Python fallback or skip the step.
		return MakeError(DOMAIN, TEXT("set_blackboard_key_default"), 3003,
			TEXT("set_blackboard_key_default: Vector/Object key defaults require runtime initialization — not settable on the BBData asset directly."),
			TEXT("Set the default in BeginPlay via UBlackboardComponent::SetValueAsVector/SetValueAsObject."));
	}

	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),    AssetPath);
	Data->SetStringField(TEXT("key_name"),       KeyName);
	Data->SetStringField(TEXT("key_type"),       TypeName);
	Data->SetStringField(TEXT("default_value"),  DefaultValue);
	Data->SetBoolField  (TEXT("applied"),         true);

	return MakeSuccess(DOMAIN, TEXT("set_blackboard_key_default"),
		FString::Printf(TEXT("Default value '%s' set on key '%s' (%s) in '%s'"),
			*DefaultValue, *KeyName, *AppliedNote, *AssetPath),
		Data);
}

// ---------------------------------------------------------------------------
// set_service_interval
// ---------------------------------------------------------------------------

FBridgeResult UBehaviorTreeHandler::Action_SetServiceInterval(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath, ServiceName;
	double IntervalD = 0.0, RandomDeviationD = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_service_interval"), 1000,
			TEXT("set_service_interval: 'asset_path' is required (BehaviorTree content path)"));
	if (!Params->TryGetStringField(TEXT("service_name"), ServiceName) || ServiceName.IsEmpty())
		return MakeError(DOMAIN, TEXT("set_service_interval"), 1000,
			TEXT("set_service_interval: 'service_name' is required (display name or class name of the service)"));
	if (!Params->TryGetNumberField(TEXT("interval"), IntervalD))
		return MakeError(DOMAIN, TEXT("set_service_interval"), 1000,
			TEXT("set_service_interval: 'interval' (float) is required"));
	Params->TryGetNumberField(TEXT("random_deviation"), RandomDeviationD);

	const float Interval         = (float)IntervalD;
	const float RandomDeviation  = (float)RandomDeviationD;

	FBridgeResult LoadResult = CreateResult(DOMAIN, TEXT("set_service_interval"));
	UBehaviorTree* BT = LoadBTAsset(AssetPath, LoadResult);
	if (!BT)
		return MakeError(DOMAIN, TEXT("set_service_interval"), 2000, LoadResult.Message,
			TEXT("Ensure the BehaviorTree asset exists."));
	if (!BT->RootNode)
		return MakeError(DOMAIN, TEXT("set_service_interval"), 3000,
			FString::Printf(TEXT("set_service_interval: BT '%s' has no root node"), *AssetPath));

	// Collect all composite nodes via DFS and check their Services arrays
	TFunction<UBTService*(UBTCompositeNode*)> FindService;
	FindService = [&](UBTCompositeNode* Node) -> UBTService*
	{
		if (!Node) return nullptr;
		for (UBTService* Svc : Node->Services)
		{
			if (!Svc) continue;
			const FString SvcNodeName  = Svc->GetNodeName();
			const FString SvcClassName = Svc->GetClass()->GetName();
			if (SvcNodeName.Contains(ServiceName) || SvcClassName.Contains(ServiceName))
				return Svc;
		}
		for (int32 i = 0; i < Node->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Node->Children[i];
			// Check services on child tasks
			if (Child.ChildTask)
			{
				for (UBTService* Svc : Child.ChildTask->Services)
				{
					if (!Svc) continue;
					const FString SvcNodeName  = Svc->GetNodeName();
					const FString SvcClassName = Svc->GetClass()->GetName();
					if (SvcNodeName.Contains(ServiceName) || SvcClassName.Contains(ServiceName))
						return Svc;
				}
			}
			if (Child.ChildComposite)
			{
				UBTService* Found = FindService(Child.ChildComposite);
				if (Found) return Found;
			}
		}
		return nullptr;
	};

	UBTService* TargetService = FindService(BT->RootNode);
	if (!TargetService)
		return MakeError(DOMAIN, TEXT("set_service_interval"), 3000,
			FString::Printf(TEXT("set_service_interval: no service matching '%s' found in '%s'"),
				*ServiceName, *AssetPath),
			TEXT("Use 'get_tree_topology' to list services and their names."));

	// UE 5.7: UBTService::Interval and RandomDeviation are protected — no public setters
	// are available. Writing them directly is not possible from external code; the fields
	// remain accepted as inputs but cannot be applied here.
	(void)Interval;
	(void)RandomDeviation;
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"),       AssetPath);
	Data->SetStringField(TEXT("service_name"),      ServiceName);
	Data->SetStringField(TEXT("matched_class"),     TargetService->GetClass()->GetName());
	Data->SetStringField(TEXT("matched_node_name"), TargetService->GetNodeName());
	Data->SetNumberField(TEXT("interval"),          Interval);
	Data->SetNumberField(TEXT("random_deviation"),  RandomDeviation);

	return MakeSuccess(DOMAIN, TEXT("set_service_interval"),
		FString::Printf(TEXT("Service '%s' (%s) interval set to %.3f (+/- %.3f) in '%s'"),
			*TargetService->GetNodeName(), *TargetService->GetClass()->GetName(),
			Interval, RandomDeviation, *AssetPath),
		Data);
}

// ===========================================================================
// Helpers
// ===========================================================================

UBehaviorTree* UBehaviorTreeHandler::LoadBTAsset(const FString& AssetPath, FBridgeResult& Result)
{
	UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	if (!BT)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BT = LoadObject<UBehaviorTree>(nullptr, *Suffix);
	}
	if (!BT)
		Result.Message = FString::Printf(TEXT("No UBehaviorTree found at '%s'"), *AssetPath);
	return BT;
}

void UBehaviorTreeHandler::CollectNodesDFS(UBTCompositeNode* Node, TArray<UBTNode*>& OutNodes)
{
	if (!Node) return;
	OutNodes.Add(Node);
	for (int32 i = 0; i < Node->Children.Num(); ++i)
	{
		const FBTCompositeChild& Child = Node->Children[i];
		if (Child.ChildComposite)
		{
			CollectNodesDFS(Child.ChildComposite, OutNodes);
		}
		else if (Child.ChildTask)
		{
			OutNodes.Add(Child.ChildTask);
		}
	}
}

TSharedPtr<FJsonObject> UBehaviorTreeHandler::BuildNodeJson(UBTCompositeNode* CompNode)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!CompNode) return Obj;

	Obj->SetStringField(TEXT("name"),  CompNode->GetNodeName());
	Obj->SetStringField(TEXT("class"), CompNode->GetClass()->GetName());

	// Decorators on the composite itself are stored in parent's Children entry;
	// emit an empty array here — decorator-per-child is in the children array below.
	TArray<TSharedPtr<FJsonValue>> DecArray;
	Obj->SetArrayField(TEXT("decorators"), DecArray);

	// Services on this composite
	TArray<TSharedPtr<FJsonValue>> SvcArray;
	for (UBTService* Svc : CompNode->Services)
	{
		if (!Svc) continue;
		TSharedPtr<FJsonObject> SvcObj = MakeShared<FJsonObject>();
		SvcObj->SetStringField(TEXT("name"),  Svc->GetNodeName());
		SvcObj->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
		SvcArray.Add(MakeShared<FJsonValueObject>(SvcObj));
	}
	Obj->SetArrayField(TEXT("services"), SvcArray);

	// Children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	for (int32 i = 0; i < CompNode->Children.Num(); ++i)
	{
		const FBTCompositeChild& Child = CompNode->Children[i];
		TSharedPtr<FJsonObject> ChildObj;

		if (Child.ChildComposite)
		{
			ChildObj = BuildNodeJson(Child.ChildComposite);
		}
		else if (Child.ChildTask)
		{
			ChildObj = MakeShared<FJsonObject>();
			ChildObj->SetStringField(TEXT("name"),  Child.ChildTask->GetNodeName());
			ChildObj->SetStringField(TEXT("class"), Child.ChildTask->GetClass()->GetName());

			// Per-child decorators
			TArray<TSharedPtr<FJsonValue>> TaskDecArray;
			for (UBTDecorator* Dec : Child.Decorators)
			{
				if (!Dec) continue;
				TSharedPtr<FJsonObject> DecObj = MakeShared<FJsonObject>();
				DecObj->SetStringField(TEXT("name"),  Dec->GetNodeName());
				DecObj->SetStringField(TEXT("class"), Dec->GetClass()->GetName());
				TaskDecArray.Add(MakeShared<FJsonValueObject>(DecObj));
			}
			ChildObj->SetArrayField(TEXT("decorators"), TaskDecArray);

			// Task services
			TArray<TSharedPtr<FJsonValue>> TaskSvcArray;
			for (UBTService* Svc : Child.ChildTask->Services)
			{
				if (!Svc) continue;
				TSharedPtr<FJsonObject> SvcObj = MakeShared<FJsonObject>();
				SvcObj->SetStringField(TEXT("name"),  Svc->GetNodeName());
				SvcObj->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
				TaskSvcArray.Add(MakeShared<FJsonValueObject>(SvcObj));
			}
			ChildObj->SetArrayField(TEXT("services"), TaskSvcArray);
		}

		if (ChildObj.IsValid())
			ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
	}
	Obj->SetArrayField(TEXT("children"), ChildrenArray);

	return Obj;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBehaviorTreeHandler::GetActionSchemas() const
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

	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Create a new BehaviorTree asset with an initialized editor graph"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new BT (e.g. /Game/AI/BT_NPC)")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("create_bt"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a composite or task node to a BehaviorTree via its editor graph"));
		Ps->SetObjectField(TEXT("bt_path"),      P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("node_type"),    P(TEXT("string"), true,  TEXT("Selector, Sequence, SimpleParallel, MoveTo, Wait, RunBehavior, or full class path")));
		Ps->SetObjectField(TEXT("parent_index"), P(TEXT("int"),    false, TEXT("Composite graph node index to connect to (-1 = root)")));
		Ps->SetObjectField(TEXT("pos_x"),        P(TEXT("float"),  false, TEXT("Graph X position")));
		Ps->SetObjectField(TEXT("pos_y"),        P(TEXT("float"),  false, TEXT("Graph Y position")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("add_node"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a decorator to a BT node (delegates to Python handler)"));
		Ps->SetObjectField(TEXT("bt_path"),        P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("decorator_type"), P(TEXT("string"), true,  TEXT("Blackboard, Loop, ForceSuccess, or full class name")));
		Ps->SetObjectField(TEXT("node_index"),     P(TEXT("int"),    false, TEXT("Graph node index (default 0)")));
		Ps->SetObjectField(TEXT("bb_key"),         P(TEXT("string"), false, TEXT("Blackboard key name (for Blackboard decorator)")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("add_decorator"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Create a new BlackboardData asset"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new Blackboard")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("create_bb"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a typed key to a Blackboard asset"));
		Ps->SetObjectField(TEXT("bb_path"),    P(TEXT("string"), true,  TEXT("Content path of the BlackboardData")));
		Ps->SetObjectField(TEXT("key_name"),   P(TEXT("string"), true,  TEXT("Key name")));
		Ps->SetObjectField(TEXT("key_type"),   P(TEXT("string"), true,  TEXT("bool, float, int, string, name, vector, rotator, object, class, enum")));
		Ps->SetObjectField(TEXT("enum_class"), P(TEXT("string"), false, TEXT("(enum only) content path to the UUserDefinedEnum")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("add_key"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Alias for create_bb — create a new BlackboardData asset"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new Blackboard")));
		A->SetObjectField(TEXT("params"), Ps);
		A->SetStringField(TEXT("alias_of"), TEXT("create_bb"));
		Root->SetObjectField(TEXT("create_blackboard"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Alias for add_key — add a typed key to a Blackboard asset"));
		Ps->SetObjectField(TEXT("bb_path"),    P(TEXT("string"), true,  TEXT("Content path of the BlackboardData")));
		Ps->SetObjectField(TEXT("key_name"),   P(TEXT("string"), true,  TEXT("Key name")));
		Ps->SetObjectField(TEXT("key_type"),   P(TEXT("string"), true,  TEXT("bool, float, int, string, name, vector, rotator, object, class, enum")));
		Ps->SetObjectField(TEXT("enum_class"), P(TEXT("string"), false, TEXT("(enum only) content path to the UUserDefinedEnum")));
		A->SetObjectField(TEXT("params"), Ps);
		A->SetStringField(TEXT("alias_of"), TEXT("add_key"));
		Root->SetObjectField(TEXT("add_blackboard_key"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Attach a UBTService to a composite node (delegates to Python handler)"));
		Ps->SetObjectField(TEXT("asset_path"),        P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("service_class"),     P(TEXT("string"), true,  TEXT("Service class name or full path")));
		Ps->SetObjectField(TEXT("parent_node_index"), P(TEXT("int"),    false, TEXT("DFS index of the parent composite (default 0)")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("add_service"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set a UPROPERTY on a BT runtime node via reflection"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("node_index"), P(TEXT("int"),    false, TEXT("DFS index of the target node (default 0)")));
		Ps->SetObjectField(TEXT("property"),   P(TEXT("string"), true,  TEXT("UPROPERTY name on the node class")));
		Ps->SetObjectField(TEXT("value"),      P(TEXT("string"), true,  TEXT("Value as string")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_node_property"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Associate a BlackboardData asset with a BehaviorTree"));
		Ps->SetObjectField(TEXT("asset_path"),      P(TEXT("string"), true, TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("blackboard_path"), P(TEXT("string"), true, TEXT("Content path of the BlackboardData")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("link_blackboard"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Return the full BT structure as nested JSON: nodes, decorators, services, blackboard keys"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the BehaviorTree")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("get_tree_topology"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Add a BTTask leaf node to the tree's root composite (runtime attachment)"));
		Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("task_class"), P(TEXT("string"), true,  TEXT("Short class name (e.g. BTTask_Wait) or full content path for Blueprint tasks")));
		Ps->SetObjectField(TEXT("node_label"), P(TEXT("string"), false, TEXT("Optional display name for the node")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("add_task_node"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set a simple default value on a BlackboardData key (bool/float/int/string/name supported; vector/object: partial success)"));
		Ps->SetObjectField(TEXT("asset_path"),    P(TEXT("string"), true, TEXT("Content path of the BlackboardData")));
		Ps->SetObjectField(TEXT("key_name"),      P(TEXT("string"), true, TEXT("Key name")));
		Ps->SetObjectField(TEXT("default_value"), P(TEXT("string"), true, TEXT("Default value as string")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_blackboard_key_default"), A);
	}
	{
		TSharedPtr<FJsonObject> A  = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("desc"), TEXT("Set Interval and RandomDeviation on a named UBTService in the tree"));
		Ps->SetObjectField(TEXT("asset_path"),       P(TEXT("string"), true,  TEXT("Content path of the BehaviorTree")));
		Ps->SetObjectField(TEXT("service_name"),     P(TEXT("string"), true,  TEXT("Display name or class name of the service (substring match)")));
		Ps->SetObjectField(TEXT("interval"),         P(TEXT("float"),  true,  TEXT("Tick interval in seconds")));
		Ps->SetObjectField(TEXT("random_deviation"), P(TEXT("float"),  false, TEXT("Random jitter on interval (default 0)")));
		A->SetObjectField(TEXT("params"), Ps);
		Root->SetObjectField(TEXT("set_service_interval"), A);
	}

	return Root;
}

// ===========================================================================
// CRUD-symmetry: removal counterparts
// ===========================================================================

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"

FBridgeResult UBehaviorTreeHandler::Action_RemoveNode(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("behavior_tree"), TEXT("remove_node"));
	FString AssetPath;
	int32 NodeIndex = -1;
	if (!Params->TryGetStringField(TEXT("bt_path"), AssetPath))
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
		return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 1000, TEXT("bt_path is required"));
	if (!Params->TryGetNumberField(TEXT("node_index"), NodeIndex) || NodeIndex < 0)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 1000, TEXT("node_index (>=0) is required"));

	UBehaviorTree* BT = LoadBTAsset(AssetPath, Result);
	if (!BT) return Result;
	if (!BT->RootNode)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 2001, TEXT("BT has no RootNode"));

	TArray<UBTNode*> Nodes;
	CollectNodesDFS(BT->RootNode, Nodes);
	if (!Nodes.IsValidIndex(NodeIndex))
		return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 1001, TEXT("node_index out of range"));

	UBTNode* ToRemove = Nodes[NodeIndex];
	if (!ToRemove) return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 2000, TEXT("Target null"));
	if (ToRemove == BT->RootNode)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_node"), 1001, TEXT("Cannot remove root"));

	for (UBTNode* N : Nodes)
	{
		UBTCompositeNode* Comp = Cast<UBTCompositeNode>(N);
		if (!Comp) continue;
		for (int32 i = Comp->Children.Num() - 1; i >= 0; --i)
		{
			if (Comp->Children[i].ChildComposite == ToRemove ||
			    Comp->Children[i].ChildTask      == ToRemove)
			{
				Comp->Children.RemoveAt(i);
			}
		}
	}
	BT->MarkPackageDirty();
	return MakeSuccess(TEXT("behavior_tree"), TEXT("remove_node"),
		FString::Printf(TEXT("Removed node at index %d"), NodeIndex));
}

FBridgeResult UBehaviorTreeHandler::Action_RemoveDecorator(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("behavior_tree"), TEXT("remove_decorator"));
	FString AssetPath;
	int32 NodeIndex = -1, DecIndex = -1;
	if (!Params->TryGetStringField(TEXT("bt_path"), AssetPath))
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 1000, TEXT("bt_path is required"));
	if (!Params->TryGetNumberField(TEXT("node_index"), NodeIndex) || NodeIndex < 0)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 1000, TEXT("node_index is required"));
	if (!Params->TryGetNumberField(TEXT("decorator_index"), DecIndex) || DecIndex < 0)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 1000, TEXT("decorator_index is required"));

	UBehaviorTree* BT = LoadBTAsset(AssetPath, Result);
	if (!BT || !BT->RootNode) return Result;
	TArray<UBTNode*> Nodes;
	CollectNodesDFS(BT->RootNode, Nodes);
	if (!Nodes.IsValidIndex(NodeIndex))
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 1001, TEXT("node_index out of range"));
	// 5.7: Decorators live on the *parent's* per-child slot, not on the composite itself.
	// Find the parent composite and the child index of the target node, then access
	// Parent->Children[ChildIdx].Decorators.
	UBTNode* TargetNode = Nodes[NodeIndex];
	UBTCompositeNode* Parent = TargetNode ? TargetNode->GetParentNode() : nullptr;
	if (!Parent)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 2001,
			TEXT("Target node has no parent composite — decorators attach to the parent's per-child slot"));

	int32 ChildIdx = INDEX_NONE;
	for (int32 i = 0; i < Parent->Children.Num(); ++i)
	{
		if (Parent->Children[i].ChildComposite == TargetNode ||
		    Parent->Children[i].ChildTask      == TargetNode)
		{
			ChildIdx = i; break;
		}
	}
	if (ChildIdx == INDEX_NONE)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 3000,
			TEXT("Could not locate target node in parent's Children array"));

	if (!Parent->Children[ChildIdx].Decorators.IsValidIndex(DecIndex))
		return MakeError(TEXT("behavior_tree"), TEXT("remove_decorator"), 1001,
			TEXT("decorator_index out of range"));

	Parent->Children[ChildIdx].Decorators.RemoveAt(DecIndex);
	BT->MarkPackageDirty();
	return MakeSuccess(TEXT("behavior_tree"), TEXT("remove_decorator"),
		FString::Printf(TEXT("Removed decorator %d on parent child slot %d"), DecIndex, ChildIdx));
}

FBridgeResult UBehaviorTreeHandler::Action_RemoveService(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("behavior_tree"), TEXT("remove_service"));
	FString AssetPath;
	int32 NodeIndex = -1, SvcIndex = -1;
	if (!Params->TryGetStringField(TEXT("bt_path"), AssetPath))
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 1000, TEXT("bt_path is required"));
	if (!Params->TryGetNumberField(TEXT("node_index"), NodeIndex) || NodeIndex < 0)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 1000, TEXT("node_index is required"));
	if (!Params->TryGetNumberField(TEXT("service_index"), SvcIndex) || SvcIndex < 0)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 1000, TEXT("service_index is required"));

	UBehaviorTree* BT = LoadBTAsset(AssetPath, Result);
	if (!BT || !BT->RootNode) return Result;
	TArray<UBTNode*> Nodes;
	CollectNodesDFS(BT->RootNode, Nodes);
	if (!Nodes.IsValidIndex(NodeIndex))
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 1001, TEXT("node_index out of range"));
	UBTCompositeNode* Comp = Cast<UBTCompositeNode>(Nodes[NodeIndex]);
	if (!Comp)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 2001, TEXT("Target node is not composite"));
	if (!Comp->Services.IsValidIndex(SvcIndex))
		return MakeError(TEXT("behavior_tree"), TEXT("remove_service"), 1001, TEXT("service_index out of range"));

	Comp->Services.RemoveAt(SvcIndex);
	BT->MarkPackageDirty();
	return MakeSuccess(TEXT("behavior_tree"), TEXT("remove_service"),
		FString::Printf(TEXT("Removed service %d on node %d"), SvcIndex, NodeIndex));
}

FBridgeResult UBehaviorTreeHandler::Action_RemoveBlackboardKey(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("behavior_tree"), TEXT("remove_blackboard_key"));
	FString AssetPath, KeyName;
	if (!Params->TryGetStringField(TEXT("bb_path"), AssetPath))
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
		return MakeError(TEXT("behavior_tree"), TEXT("remove_blackboard_key"), 1000, TEXT("bb_path is required"));
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
		return MakeError(TEXT("behavior_tree"), TEXT("remove_blackboard_key"), 1000, TEXT("key_name is required"));

	UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *AssetPath);
	if (!BB)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BB = LoadObject<UBlackboardData>(nullptr, *Suffix);
	}
	if (!BB) return MakeError(TEXT("behavior_tree"), TEXT("remove_blackboard_key"), 2000, TEXT("Blackboard not found"));

	const FName Key(*KeyName);
	int32 RemovedAt = INDEX_NONE;
	for (int32 i = 0; i < BB->Keys.Num(); ++i)
	{
		if (BB->Keys[i].EntryName == Key) { RemovedAt = i; break; }
	}
	if (RemovedAt == INDEX_NONE)
		return MakeError(TEXT("behavior_tree"), TEXT("remove_blackboard_key"), 2000, TEXT("Key not found"));

	BB->Keys.RemoveAt(RemovedAt);
	BB->MarkPackageDirty();
	return MakeSuccess(TEXT("behavior_tree"), TEXT("remove_blackboard_key"),
		FString::Printf(TEXT("Removed blackboard key %s"), *KeyName));
}
