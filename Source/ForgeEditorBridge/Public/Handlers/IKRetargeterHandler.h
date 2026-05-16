#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "IKRetargeterHandler.generated.h"

/**
 * IKRetargeterHandler - domain "ik_retargeter"  (v1.0.0 / UE 5.7)
 *
 * Creates and configures UIKRetargeter assets through the Bridge.
 *
 * Actions:
 *   create_ik_retargeter -> asset_path (string),
 *                           source_ik_rig_path (string),
 *                           target_ik_rig_path (string)
 *                           Creates a new UIKRetargeter asset and assigns both IK rigs.
 *
 *   add_chain_mapping    -> asset_path (string),
 *                           source_chain (string),
 *                           target_chain (string)
 *                           Maps a source retarget chain to a target retarget chain.
 *
 *   retarget             -> source_ik_rig_path (string),
 *                           target_ik_rig_path (string),
 *                           output_path (string - content path for new retargeter, used if retargeter_path not provided),
 *                           retargeter_path (string - optional, load existing retargeter instead of creating),
 *                           auto_map (bool - default true, runs AutoMapChains(Fuzzy))
 *                           Creates (or loads) a UIKRetargeter, assigns both rigs, and optionally auto-maps chains.
 *
 *   batch_retarget       -> retargeter_path (string),
 *                           animation_paths (array of strings - content paths to AnimSequences),
 *                           output_directory (string - content path to output folder)
 *                           Retargets a batch of animation sequences using the given retargeter.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UIKRetargeterHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("ik_retargeter"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("create_ik_retargeter"),
			TEXT("add_chain_mapping"),
			TEXT("retarget"),
			TEXT("batch_retarget")
		};
	}
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateIKRetargeter(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddChainMapping   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Retarget          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_BatchRetarget     (TSharedPtr<FJsonObject> Params);
};
