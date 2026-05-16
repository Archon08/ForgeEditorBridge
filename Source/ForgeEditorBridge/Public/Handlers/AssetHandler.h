#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "AssetHandler.generated.h"

/**
 * AssetHandler — domain "asset"  (v0.9.0 / UE 5.7)
 *
 * Actions:
 *   create_data_asset  → asset_name, package_path, class_name (optional)
 *   create_material    → material_name, package_path
 *   duplicate_asset    → source_path, dest_path
 *   set_metadata       → asset_path (string), key (string), value (string)
 *                        Persist a key-value metadata pair for the asset in
 *                        {ProjectDir}/Forge/ue-context/asset-metadata.json.
 *   get_metadata       → asset_path (string), key (string, optional — omit to get all keys)
 *                        Returns stored metadata as a JSON object in ExtraData.
 *   rename_asset       → old_path, new_name
 *   move_asset         → source_path, dest_path
 *   search_assets      → class? path? recursive? name_filter?
 *   import_asset       → source_file, dest_path, replace?
 *   import_folder      → source_folder, target_package_path,
 *                        file_extensions? (array; default ["*"]),
 *                        recursive? (bool, default false),
 *                        replace_existing? (bool, default false)
 *   save_asset         → asset_path
 *   validate_asset     → asset_path
 *   get_asset_info     → asset_path
 */
UCLASS()
class FORGEEDITORBRIDGE_API UAssetHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("asset"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("create_data_asset"), TEXT("create_material"), TEXT("duplicate_asset"),
			TEXT("set_metadata"), TEXT("get_metadata"),
			TEXT("rename_asset"), TEXT("move_asset"), TEXT("search_assets"),
			TEXT("import_asset"), TEXT("import_folder"),
			TEXT("save_asset"), TEXT("validate_asset"),
			TEXT("get_asset_info"),
			TEXT("delete_asset"), TEXT("fix_up_referencers")
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult CreateDataAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult CreateMaterial(TSharedPtr<FJsonObject> Params);
	FBridgeResult DuplicateAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetMetadata(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMetadata(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RenameAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_MoveAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SearchAssets(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ImportFolder(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SaveAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ValidateAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetAssetInfo(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DeleteAsset(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_FixUpReferencers(TSharedPtr<FJsonObject> Params);

	/** Read the metadata store JSON from disk. Returns empty object if file doesn't exist. */
	TSharedPtr<FJsonObject> LoadMetadataStore() const;
	/** Write the metadata store JSON back to disk. Returns false on write failure. */
	bool SaveMetadataStore(TSharedPtr<FJsonObject> Store, FString& OutError) const;
	/** Returns the canonical path to the metadata store file. */
	FString MetadataStorePath() const;
};
