#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "StructHandler.generated.h"

struct FEdGraphPinType;
class UUserDefinedStruct;

/**
 * StructHandler — domain "struct"  (v0.3.0-dev / UE 5.8)
 *
 * Creates and modifies UUserDefinedStruct assets (Blueprintable data structures).
 * NOTE: In UE 5.7, float fields must use PC_Real + PC_Double subcategory —
 *       PC_Float was removed. This handler handles that mapping automatically.
 *
 * GUID-diff pattern for add_field: captures variable GUIDs before and after
 * FStructureEditorUtils::AddVariable() to reliably find the new variable for
 * renaming, rather than assuming insertion order (which is fragile).
 *
 * Actions:
 *   create         → asset_path (full package path e.g. "/Game/Data/S_MyStruct"),
 *                    fields[] (optional: [{name, type}]) — initial field batch
 *   add_field      → asset_path, field_name, field_type
 *                    Supported types: bool|int|int64|float|string|name|text|
 *                                     vector|rotator|transform|color|object
 *   remove_field   → asset_path, field_name
 *   rename_field   → asset_path, old_name, new_name — propagates to DataTables
 *   set_field_default → asset_path, field_name, default_value (string)
 *   list_fields    → asset_path — returns array of {name, type, guid, default_value}
 */
UCLASS()
class FORGEEDITORBRIDGE_API UStructHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()
public:
	virtual FString GetDomainName() const override { return TEXT("struct"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return { TEXT("create"), TEXT("add_field"), TEXT("remove_field"),
		         TEXT("rename_field"), TEXT("set_field_default"), TEXT("list_fields") };
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_Create          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddField        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveField     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RenameField     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetFieldDefault (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListFields      (TSharedPtr<FJsonObject> Params);

	/** Load a UUserDefinedStruct from a full package path. Populates Result.Message on failure. */
	UUserDefinedStruct* LoadStruct(const FString& AssetPath, FBridgeResult& Result) const;

	/**
	 * Map a field_type string to FEdGraphPinType.
	 * "float" → PC_Real + PC_Double subcategory (UE 5.7, PC_Float was removed).
	 * Returns false if the type is unrecognised.
	 */
	static bool ParseFieldType(const FString& TypeStr, FEdGraphPinType& OutPinType);

	/**
	 * After calling FStructureEditorUtils::AddVariable(), compare current GUIDs against
	 * the Before set to identify the newly added variable's GUID.
	 */
	static FGuid FindNewVarGuid(UUserDefinedStruct* Struct, const TSet<FGuid>& Before);
};
