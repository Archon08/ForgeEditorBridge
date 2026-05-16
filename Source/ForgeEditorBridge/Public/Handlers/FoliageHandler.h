#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "FoliageHandler.generated.h"

/**
 * FoliageHandler — domain "foliage"  (v0.10.0 / UE 5.7)
 *
 * Actions:
 *   add_type      → foliage_type_path (string)
 *                   Registers the foliage type with the level's AInstancedFoliageActor
 *                   without placing any instances. Idempotent.
 *
 *   paint_foliage → foliage_type_path (string),
 *                   x (float), y (float), z (float)   ← world-space location,
 *                   pitch (float, degrees), yaw (float), roll (float),
 *                   scale (float, uniform, default 1.0)
 *                   Auto-adds the type if not already registered.
 *
 *   clear_type    → foliage_type_path (string)
 *                   Removes all placed instances; keeps the type registration.
 *
 *   list_types    → (no params — lists all foliage type asset paths registered in the level IFA)
 *                   Returns types[] array with foliage_type_path and instance_count per type.
 *
 *   get_instance_count → foliage_type_path (string)
 *                        Returns instance_count for the specified foliage type in the current level.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UFoliageHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("foliage"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("add_type"), TEXT("paint_foliage"), TEXT("clear_type"), TEXT("list_types"), TEXT("get_instance_count"), TEXT("remove_type") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_AddType          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_PaintFoliage     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ClearType        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListTypes        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetInstanceCount (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveType       (TSharedPtr<FJsonObject> Params);
};
