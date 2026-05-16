#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "TextureHandler.generated.h"

/**
 * TextureHandler — domain "texture"
 *
 * Import and configure texture assets.
 *
 * Actions:
 *   import_texture     → source_file, dest_path, compression?, srgb?, lod_group?
 *   set_compression    → asset_path, compression (TC_Default, TC_Normalmap, etc.)
 *   set_lod_group      → asset_path, lod_group (TEXTUREGROUP_World, etc.)
 *   set_srgb           → asset_path, srgb (bool)
 *   set_virtual_texture → asset_path, enabled (bool)
 *   resize             → asset_path, max_size (int)
 *   get_info           → asset_path → width, height, format, compression, mip_count, etc.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UTextureHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("texture"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("import_texture"), TEXT("set_compression"), TEXT("set_lod_group"), TEXT("set_srgb"), TEXT("set_virtual_texture"), TEXT("resize"), TEXT("get_info") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	static TextureCompressionSettings ParseCompressionSetting(const FString& Name);
	static TextureGroup ParseLODGroup(const FString& Name);
};
