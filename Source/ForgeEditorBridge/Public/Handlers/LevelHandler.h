#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "LevelHandler.generated.h"

/**
 * LevelHandler — domain "level"
 *
 * Level and sublevel management.
 *
 * Actions:
 *   open_level           → path
 *   save_level           → (optional path, saves current if omitted)
 *   save_all             → prompt_user?
 *   get_level_info       → returns current level info + sublevel list
 *   add_sublevel         → level_path, streaming_class?
 *   remove_sublevel      → level_path
 *   list_sublevels       → returns all streaming levels
 *   set_sublevel_visible → level_path, visible
 *   run_build            → build_type (lighting|navigation|geometry|all)
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULevelHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("level"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("open_level"), TEXT("save_level"), TEXT("save_all"), TEXT("get_level_info"), TEXT("add_sublevel"), TEXT("remove_sublevel"), TEXT("list_sublevels"), TEXT("set_sublevel_visible"), TEXT("run_build"), TEXT("build_hlods"), TEXT("import_scene"), TEXT("set_current_level"), TEXT("set_sky_light_source"), TEXT("build_all_lighting"), TEXT("export_asset_list") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;
};
