#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "MaterialHandler.generated.h"

/**
 * MaterialHandler — domain "material"
 *
 * Ported from UmgMcpMaterialSubsystem.cpp (MIT).
 * Uses UMaterialEditingLibrary for all graph operations.
 *
 * Actions:
 *   material_set_target      → asset_path → sets active material asset
 *   material_add_node        → node_type, x?, y? → adds expression node
 *   material_connect_pins    → from_node, from_pin, to_node, to_pin
 *   material_connect_nodes   → from_node, to_node, to_input
 *   material_get_graph       → returns JSON list of all nodes + pins
 *   material_set_hlsl_node_io → node_id, inputs[], outputs[]
 *   material_set_node_properties → node_id, properties{}
 *   material_get_pins        → node_id → returns input/output pin list
 *   material_compile_asset   → compiles and saves current material
 */
UCLASS()
class FORGEEDITORBRIDGE_API UMaterialHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("material"); }
    virtual TArray<FString> GetSupportedActions() const override { return { TEXT("material_set_target"), TEXT("material_add_node"), TEXT("material_connect_pins"), TEXT("material_connect_nodes"), TEXT("material_get_graph"), TEXT("material_get_pins"), TEXT("material_set_node_properties"), TEXT("material_set_hlsl_node_io"), TEXT("material_compile_asset"), TEXT("material_remove_node"), TEXT("material_disconnect_pins"), TEXT("material_set_blend_mode"), TEXT("material_set_shading_model"), TEXT("material_set_two_sided"), TEXT("material_get_node_properties"), TEXT("material_list_expressions"), TEXT("read_material_capture"), TEXT("create_material_function"), TEXT("add_parameter"), TEXT("bake_textures") }; }
    virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_ReadMaterialCapture(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CreateMaterialFunction(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddParameter(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_BakeTextures(TSharedPtr<FJsonObject> Params);
};
