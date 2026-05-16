#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "SmartObjectHandler.generated.h"

/**
 * SmartObjectHandler — domain "smart_object"
 *
 * Smart Object definition and placement management.
 *
 * Actions:
 *   create_smart_object    → asset_path
 *   add_slot               → asset_path, offset {x,y,z}, rotation {pitch,yaw,roll}
 *   set_slot_tag           → asset_path, slot_index, tag
 *   place_smart_object     → definition, location {x,y,z}, rotation {pitch,yaw,roll}
 *   get_smart_object_info  → asset_path — returns slot count, tags per slot, definition name
 *   list_slots             → asset_path — returns all slots with offset, rotation, activity tags
 */
UCLASS()
class FORGEEDITORBRIDGE_API USmartObjectHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("smart_object"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_smart_object"), TEXT("add_slot"), TEXT("set_slot_tag"), TEXT("place_smart_object"), TEXT("get_smart_object_info"), TEXT("list_slots") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_GetSmartObjectInfo(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListSlots(TSharedPtr<FJsonObject> Params);
};
