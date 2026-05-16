#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "ControlRigHandler.generated.h"

/**
 * ControlRigHandler — domain "control_rig"  (v0.9.0 / UE 5.7)
 *
 * Creates and authors UControlRigBlueprint assets through the Bridge via RigVM / RigHierarchy APIs.
 *
 * Actions:
 *   create_rig    → asset_path (string)
 *                   Creates a new UControlRigBlueprint asset.
 *
 *   add_control   → asset_path (string), control_name (string),
 *                   control_type ("Transform"|"Float"|"Integer"|"Bool"|"Vector2D"|
 *                                 "Position"|"Scale"|"Rotator", default "Transform"),
 *                   parent_name (string, optional — parent control; omit for root)
 *                   Adds a control to the rig hierarchy.
 *
 *   add_node      → asset_path (string),
 *                   function (string — UScriptStruct path, e.g. "/Script/ControlRig.RigUnit_GetTransform"),
 *                   x (float, default 0), y (float, default 0)
 *                   Adds a RigVM unit node to the default graph at the given canvas position.
 *
 *   connect_pins  → asset_path (string),
 *                   output_pin (string — "NodeName.PinName"),
 *                   input_pin  (string — "NodeName.PinName")
 *                   Links an output pin to an input pin in the default graph.
 *
 *   add_bone_chain → asset_path (string), chain_name (string),
 *                    parent_name (string, optional), count (int, default 4),
 *                    length (float, default 10.0 — spacing per bone along X)
 *                    Appends a chain of bones to the rig hierarchy.
 *
 *   set_ik_goal    → asset_path (string), control_name (string),
 *                    position {x,y,z} (optional), rotation {pitch,yaw,roll} (optional)
 *                    Sets the initial transform of an IK goal control.
 *
 *   add_constraint → (Python-dispatched) — RigVM node documentation for
 *                    RigUnit_ParentConstraint / RigUnit_AimConstraint wiring.
 *
 *   get_rig_topology → asset_path (string)
 *                      Serializes all elements (bones, controls, nulls) to JSON.
 *
 *   add_forward_solve_node → asset_path (string),
 *                            function (string — UScriptStruct path, e.g. "/Script/ControlRig.RigUnit_GetTransform"),
 *                            x (float, default 0), y (float, default 0)
 *                            Adds a RigVM unit node to the ForwardsSolve graph.
 *
 *   compile        → asset_path (string)
 *                    Compiles a ControlRig blueprint via FKismetEditorUtilities.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UControlRigHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("control_rig"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_rig"), TEXT("add_control"), TEXT("add_node"), TEXT("connect_pins"), TEXT("add_bone_chain"), TEXT("set_ik_goal"), TEXT("add_constraint"), TEXT("get_rig_topology"), TEXT("add_forward_solve_node"), TEXT("compile") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateRig      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddControl     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddNode        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ConnectPins    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddBoneChain   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetIKGoal      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddConstraint  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetRigTopology (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddForwardSolveNode (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Compile        (TSharedPtr<FJsonObject> Params);

	/** Load a ControlRig asset. UE 5.7: UControlRigBlueprint removed, uses IControlRigAssetInterface. */
	UObject* LoadCRAsset(const FString& AssetPath, FBridgeResult& Result);
};
