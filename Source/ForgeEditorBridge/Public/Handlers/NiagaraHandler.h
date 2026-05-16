#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "NiagaraHandler.generated.h"

/**
 * NiagaraHandler — domain "niagara"  (v0.6.0 / UE 5.7)
 *
 * Creates and authors Niagara System assets programmatically through the Bridge.
 * Uses the UE 5.3+ versioned emitter model (FVersionedNiagaraEmitter).
 *
 * Actions:
 *   create_niagara_system  → asset_path (string)
 *                            Creates a new UNiagaraSystem asset via UNiagaraSystemFactory.
 *
 *   add_emitter            → asset_path (string), emitter_path (string)
 *                            Loads a source UNiagaraEmitter and adds it to the system using
 *                            FNiagaraEditorUtilities::AddEmitterToSystem (versioned API).
 *
 *   set_user_parameter     → asset_path (string), param_name (string), value (string),
 *                            value_type ("float"|"int"|"bool"|"vector"|"color")
 *                            Sets a user-exposed parameter value in the system's
 *                            ExposedParameters store (FNiagaraUserRedirectionParameterStore).
 *
 *   compile_niagara_system → asset_path (string)
 *                            Triggers UNiagaraSystem::RequestCompile(bForce=false).
 */
UCLASS()
class FORGEEDITORBRIDGE_API UNiagaraHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("niagara"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("create_niagara_system"), TEXT("add_emitter"), TEXT("set_user_parameter"), TEXT("compile_niagara_system"), TEXT("get_parameters"), TEXT("set_parameter_default"), TEXT("read_niagara_capture"), TEXT("update_instances"), TEXT("add_module"), TEXT("remove_module"), TEXT("set_module_property"), TEXT("add_renderer"), TEXT("set_renderer_property"), TEXT("create_system"), TEXT("set_parameter"), TEXT("compile"), TEXT("set_gpu_sim"), TEXT("bind_parameter"), TEXT("set_particle_lights"), TEXT("set_collision_response"), TEXT("list_emitters"), TEXT("set_emitter_enabled"), TEXT("remove_emitter"), TEXT("remove_renderer") }; }
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateNiagaraSystem  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddEmitter           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetUserParameter     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CompileNiagaraSystem (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetParameters        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetParameterDefault  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ReadNiagaraCapture   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_UpdateInstances      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddModule            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveModule         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetModuleProperty    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddRenderer          (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetRendererProperty  (TSharedPtr<FJsonObject> Params);
	// Phase 1b additions
	FBridgeResult Action_SetGPUSim           (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_BindParameter       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetParticleLights   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetCollisionResponse(TSharedPtr<FJsonObject> Params);
	// New actions
	FBridgeResult Action_ListEmitters        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetEmitterEnabled   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveEmitter       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveRenderer      (TSharedPtr<FJsonObject> Params);

	/**
	 * Load a UNiagaraSystem asset from a content path.
	 * Populates Result.Message on failure and returns nullptr.
	 */
	class UNiagaraSystem* LoadNiagaraSystem(const FString& AssetPath, FBridgeResult& Result);

	/**
	 * Find an emitter handle by name. Exact match on GetName() first, then
	 * case-insensitive Contains() on GetUniqueInstanceName() as fallback.
	 * Returns -1 if not found, -2 if ambiguous (multiple substring matches).
	 */
	int32 FindEmitterHandleIndex(class UNiagaraSystem* System, const FString& EmitterName) const;
};
