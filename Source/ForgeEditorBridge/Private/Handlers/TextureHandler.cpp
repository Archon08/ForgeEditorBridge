#include "Handlers/TextureHandler.h"
#include "ForgeAISubsystem.h"
#include "Factories/TextureFactory.h"
#include "Engine/Texture2D.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("texture");

TextureCompressionSettings UTextureHandler::ParseCompressionSetting(const FString& Name)
{
	if (Name == TEXT("TC_Normalmap"))            return TC_Normalmap;
	if (Name == TEXT("TC_Masks"))                return TC_Masks;
	if (Name == TEXT("TC_Grayscale"))            return TC_Grayscale;
	if (Name == TEXT("TC_Displacementmap"))      return TC_Displacementmap;
	if (Name == TEXT("TC_VectorDisplacementmap"))return TC_VectorDisplacementmap;
	if (Name == TEXT("TC_HDR"))                  return TC_HDR;
	if (Name == TEXT("TC_EditorIcon"))           return TC_EditorIcon;
	if (Name == TEXT("TC_Alpha"))                return TC_Alpha;
	if (Name == TEXT("TC_DistanceFieldFont"))    return TC_DistanceFieldFont;
	if (Name == TEXT("TC_HDR_Compressed"))       return TC_HDR_Compressed;
	if (Name == TEXT("TC_BC7"))                  return TC_BC7;
	if (Name == TEXT("TC_HalfFloat"))            return TC_HalfFloat;
	if (Name == TEXT("TC_SingleFloat"))          return TC_SingleFloat;
	return TC_Default;
}

TextureGroup UTextureHandler::ParseLODGroup(const FString& Name)
{
	if (Name == TEXT("TEXTUREGROUP_WorldNormalMap"))     return TEXTUREGROUP_WorldNormalMap;
	if (Name == TEXT("TEXTUREGROUP_WorldSpecular"))      return TEXTUREGROUP_WorldSpecular;
	if (Name == TEXT("TEXTUREGROUP_Character"))          return TEXTUREGROUP_Character;
	if (Name == TEXT("TEXTUREGROUP_CharacterNormalMap")) return TEXTUREGROUP_CharacterNormalMap;
	if (Name == TEXT("TEXTUREGROUP_Vehicle"))            return TEXTUREGROUP_Vehicle;
	if (Name == TEXT("TEXTUREGROUP_Weapon"))             return TEXTUREGROUP_Weapon;
	if (Name == TEXT("TEXTUREGROUP_UI"))                 return TEXTUREGROUP_UI;
	if (Name == TEXT("TEXTUREGROUP_Lightmap"))           return TEXTUREGROUP_Lightmap;
	if (Name == TEXT("TEXTUREGROUP_Shadowmap"))          return TEXTUREGROUP_Shadowmap;
	if (Name == TEXT("TEXTUREGROUP_Skybox"))             return TEXTUREGROUP_Skybox;
	if (Name == TEXT("TEXTUREGROUP_Pixels2D"))           return TEXTUREGROUP_Pixels2D;
	if (Name == TEXT("TEXTUREGROUP_HierarchicalLOD"))    return TEXTUREGROUP_HierarchicalLOD;
	return TEXTUREGROUP_World;
}

FBridgeResult UTextureHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- import_texture ----
	if (Action == TEXT("import_texture"))
	{
		FString SourceFile, DestPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_file"), SourceFile))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'source_file'"));
		if (!Params->TryGetStringField(TEXT("dest_path"), DestPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'dest_path'"));
		if (!FPaths::FileExists(SourceFile))
			return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Source file not found: %s"), *SourceFile));

		FString PackagePath = FPackageName::GetLongPackagePath(DestPath);

		UTextureFactory* Factory = NewObject<UTextureFactory>();
		Factory->SuppressImportOverwriteDialog();

		FString Compression;
		if (Params->TryGetStringField(TEXT("compression"), Compression))
		{
			Factory->CompressionSettings = ParseCompressionSetting(Compression);
			Factory->bUsingExistingSettings = true;
		}

		UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
		ImportData->Filenames.Add(SourceFile);
		ImportData->DestinationPath = PackagePath;
		ImportData->Factory = Factory;
		ImportData->bReplaceExisting = true;

		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<UObject*> Imported = AT.Get().ImportAssetsAutomated(ImportData);

		if (Imported.Num() == 0 || !Imported[0])
			return MakeError(DOMAIN, Action, 3000, FString::Printf(TEXT("Failed to import: %s"), *SourceFile));

		UTexture2D* Tex = Cast<UTexture2D>(Imported[0]);
		if (!Tex)
			return MakeError(DOMAIN, Action, 3001, FString::Printf(
				TEXT("Imported asset '%s' is not a UTexture2D (got %s) — sRGB/compression/LOD settings were not applied"),
				*Imported[0]->GetPathName(), *Imported[0]->GetClass()->GetName()));

		{
			bool bSRGB = true;
			Params->TryGetBoolField(TEXT("srgb"), bSRGB);
			Tex->SRGB = bSRGB;

			FString LODGroup;
			if (Params->TryGetStringField(TEXT("lod_group"), LODGroup))
				Tex->LODGroup = ParseLODGroup(LODGroup);

			Tex->UpdateResource();
			Tex->MarkPackageDirty();
		}

		FBridgeResult R = MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Imported: %s"), *Imported[0]->GetPathName()));
		R.AffectedPath = Imported[0]->GetPathName();
		return R;
	}

	// ---- set_compression ----
	if (Action == TEXT("set_compression"))
	{
		FString AssetPath, Compression;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
		if (!Params->TryGetStringField(TEXT("compression"), Compression))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'compression'"));

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		Tex->CompressionSettings = ParseCompressionSetting(Compression);
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Compression → %s on %s"), *Compression, *AssetPath));
	}

	// ---- set_lod_group ----
	if (Action == TEXT("set_lod_group"))
	{
		FString AssetPath, LODGroup;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
		if (!Params->TryGetStringField(TEXT("lod_group"), LODGroup))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'lod_group'"));

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		Tex->LODGroup = ParseLODGroup(LODGroup);
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("LOD group → %s on %s"), *LODGroup, *AssetPath));
	}

	// ---- set_srgb ----
	if (Action == TEXT("set_srgb"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		bool bSRGB = true;
		Params->TryGetBoolField(TEXT("srgb"), bSRGB);

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		Tex->SRGB = bSRGB;
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("sRGB → %s on %s"), bSRGB ? TEXT("true") : TEXT("false"), *AssetPath));
	}

	// ---- set_virtual_texture ----
	if (Action == TEXT("set_virtual_texture"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		bool bEnabled = true;
		Params->TryGetBoolField(TEXT("enabled"), bEnabled);

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		Tex->VirtualTextureStreaming = bEnabled;
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("VT streaming %s on %s"), bEnabled ? TEXT("on") : TEXT("off"), *AssetPath));
	}

	// ---- resize ----
	if (Action == TEXT("resize"))
	{
		FString AssetPath;
		double MaxSize;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));
		if (!Params->TryGetNumberField(TEXT("max_size"), MaxSize))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'max_size' (number)"));

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		Tex->MaxTextureSize = (int32)MaxSize;
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Max size → %d on %s"), (int32)MaxSize, *AssetPath));
	}

	// ---- get_info ----
	if (Action == TEXT("get_info"))
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return MakeError(DOMAIN, Action, 1000, TEXT("Missing required param: 'asset_path'"));

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
		if (!Tex) return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("width"), (double)Tex->GetSizeX());
		Data->SetNumberField(TEXT("height"), (double)Tex->GetSizeY());
		Data->SetStringField(TEXT("format"), GPixelFormats[Tex->GetPixelFormat()].Name);
		Data->SetBoolField(TEXT("srgb"), Tex->SRGB);
		Data->SetNumberField(TEXT("mip_count"), (double)Tex->GetNumMips());
		Data->SetBoolField(TEXT("virtual_texture"), Tex->VirtualTextureStreaming);
		Data->SetNumberField(TEXT("max_texture_size"), (double)Tex->MaxTextureSize);

		return MakeSuccess(DOMAIN, Action, FString::Printf(TEXT("Info for %s"), *AssetPath), Data);
	}

	return MakeError(DOMAIN, Action, 1001, FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("system/capabilities"));
}

TSharedPtr<FJsonObject> UTextureHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Import a texture from disk")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("source_file"), P(TEXT("string"), true, TEXT("Absolute disk path to source image"))); Ps->SetObjectField(TEXT("dest_path"), P(TEXT("string"), true, TEXT("Content destination path"))); Ps->SetObjectField(TEXT("compression"), P(TEXT("string"), false, TEXT("Compression setting (e.g. TC_Normalmap, TC_Masks)"))); Ps->SetObjectField(TEXT("srgb"), P(TEXT("bool"), false, TEXT("sRGB flag (default true)"))); Ps->SetObjectField(TEXT("lod_group"), P(TEXT("string"), false, TEXT("LOD group (e.g. TEXTUREGROUP_World)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("import_texture"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set compression settings on a texture")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); Ps->SetObjectField(TEXT("compression"), P(TEXT("string"), true, TEXT("Compression setting name"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_compression"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set LOD group on a texture")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); Ps->SetObjectField(TEXT("lod_group"), P(TEXT("string"), true, TEXT("LOD group name"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_lod_group"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set sRGB flag on a texture")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); Ps->SetObjectField(TEXT("srgb"), P(TEXT("bool"), false, TEXT("sRGB enabled (default true)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_srgb"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Enable or disable virtual texture streaming")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); Ps->SetObjectField(TEXT("enabled"), P(TEXT("bool"), false, TEXT("Enable VT streaming (default true)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("set_virtual_texture"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the max texture size")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); Ps->SetObjectField(TEXT("max_size"), P(TEXT("int"), true, TEXT("Maximum texture size in pixels"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("resize"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get texture info (dimensions, format, mips, etc.)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the texture"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_info"), A); }

	return Root;
}
