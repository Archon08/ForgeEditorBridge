#include "Capture/ForgeNiagaraCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Niagara ---
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "NiagaraWorldManager.h"
#include "NiagaraComponentPool.h"
#include "NiagaraComponent.h"

// --- Asset Registry ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// --- UObject property reflection (protected property access) ---
#include "UObject/UnrealType.h"

// --- Editor world access (for PIE pool capture) ---
#include "Editor.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeNiagaraCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("niagara")));
}

// ---------------------------------------------------------------------------
// ExportNiagaraSystem
// ---------------------------------------------------------------------------

bool UForgeNiagaraCapture::ExportNiagaraSystem(const FString& AssetPath)
{
    UNiagaraSystem* System = Cast<UNiagaraSystem>(
        StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, *AssetPath));
    if (!System)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ForgeNiagaraCapture: Failed to load NiagaraSystem '%s'"), *AssetPath);
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("asset_path"), AssetPath);

    // System-level properties.
    // WarmupTime and WarmupTickCount are protected in UE5.5+ — access via property reflection.
    // CanBePooled() and bAutoDeactivate were removed from the public API; skip them.
    float WarmupTime = 0.f;
    int32 WarmupTickCount = 0;
    if (const FFloatProperty* Prop = FindFProperty<FFloatProperty>(System->GetClass(), TEXT("WarmupTime")))
    {
        WarmupTime = Prop->GetPropertyValue_InContainer(System);
    }
    if (const FIntProperty* Prop = FindFProperty<FIntProperty>(System->GetClass(), TEXT("WarmupTickCount")))
    {
        WarmupTickCount = Prop->GetPropertyValue_InContainer(System);
    }
    Root->SetNumberField(TEXT("warmup_time"),      WarmupTime);
    Root->SetNumberField(TEXT("warmup_tick_count"), WarmupTickCount);

    // -----------------------------------------------------------------------
    // Emitters
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> EmitterArr;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("id"),      Handle.GetId().ToString());
        E->SetStringField(TEXT("name"),    Handle.GetName().ToString());
        E->SetBoolField  (TEXT("enabled"), Handle.GetIsEnabled());

        // Per-emitter data: particle estimate, sim target, local space
        int32   MaxParticles = 0;
        FString SimTarget    = TEXT("unknown");
        bool    bLocalSpace  = false;
        if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
        {
            MaxParticles = EmitterData->GetMaxParticleCountEstimate();
            SimTarget    = (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
                           ? TEXT("GPU") : TEXT("CPU");
            bLocalSpace  = EmitterData->bLocalSpace != 0;
        }
        E->SetNumberField(TEXT("max_particles_estimate"), MaxParticles);
        E->SetStringField(TEXT("sim_target"),             SimTarget);
        E->SetBoolField  (TEXT("local_space"),            bLocalSpace);

        EmitterArr.Add(MakeShared<FJsonValueObject>(E));
    }
    Root->SetArrayField(TEXT("emitters"), EmitterArr);

    // -----------------------------------------------------------------------
    // User Parameters (exposed knobs for Blueprint / sequencer control)
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> ParamArr;
    const FNiagaraUserRedirectionParameterStore& ParamStore = System->GetExposedParameters();
    for (const FNiagaraVariableWithOffset& VarWithOffset : ParamStore.ReadParameterVariables())
    {
        TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), VarWithOffset.GetName().ToString());
        P->SetStringField(TEXT("type"), VarWithOffset.GetType().GetName());
        ParamArr.Add(MakeShared<FJsonValueObject>(P));
    }
    Root->SetArrayField(TEXT("user_parameters"), ParamArr);

    // -----------------------------------------------------------------------
    // Scalability overrides — per-platform cull settings baked into the system
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> ScalabilityArr;
    const FNiagaraSystemScalabilityOverrides& ScalOverrides = System->GetSystemScalabilityOverrides();
    for (const FNiagaraSystemScalabilityOverride& Override : ScalOverrides.Overrides)
    {
        TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
        S->SetBoolField  (TEXT("cull_by_distance"),          Override.bCullByDistance != 0);
        S->SetNumberField(TEXT("max_distance"),               Override.MaxDistance);
        S->SetBoolField  (TEXT("cull_by_system_count"),      Override.bCullPerSystemMaxInstanceCount != 0);
        S->SetNumberField(TEXT("max_system_instances"),       Override.MaxSystemInstances);
        S->SetBoolField  (TEXT("cull_by_effect_type_count"), Override.bCullMaxInstanceCount != 0);
        S->SetNumberField(TEXT("max_effect_type_instances"), Override.MaxInstances);
        ScalabilityArr.Add(MakeShared<FJsonValueObject>(S));
    }
    Root->SetArrayField(TEXT("scalability"), ScalabilityArr);

    // -----------------------------------------------------------------------
    // Audit pass
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> Issues;
    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

    // HIGH_EMITTER_COUNT — each emitter is an independent tick cost
    const int32 EmitterCountThreshold = 8;
    if (Handles.Num() > EmitterCountThreshold)
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("HIGH_EMITTER_COUNT"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("detail"),
            FString::Printf(
                TEXT("System has %d emitters (threshold: %d). Each emitter is an "
                     "independent tick and contributes to per-frame CPU cost. "
                     "Consider merging emitters or splitting into multiple smaller systems."),
                Handles.Num(), EmitterCountThreshold));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // UNBOUND_FIXED_BOUNDS — dynamic bounds computed every frame for non-fixed systems
    if (!System->bFixedBounds)
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("UNBOUND_FIXED_BOUNDS"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("detail"),
            TEXT("bFixedBounds is false. The engine computes bounds every frame for culling "
                 "and shadow casting, adding CPU cost proportional to particle count. "
                 "Enable Fixed Bounds and set a conservative box unless the effect "
                 "must dynamically update its extents."));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // Per-emitter checks: DISABLED_EMITTER and ZERO_MAX_PARTICLES
    for (const FNiagaraEmitterHandle& Handle : Handles)
    {
        // DISABLED_EMITTER — asset carries emitter data but it never runs
        if (!Handle.GetIsEnabled())
        {
            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("issue_type"), TEXT("DISABLED_EMITTER"));
            Issue->SetStringField(TEXT("severity"),   TEXT("info"));
            Issue->SetStringField(TEXT("detail"),
                FString::Printf(
                    TEXT("Emitter '%s' is disabled. It still occupies memory in the "
                         "compiled system. Remove it if no longer needed."),
                    *Handle.GetName().ToString()));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
            continue; // skip ZERO_MAX_PARTICLES check for disabled emitters
        }

        // ZERO_MAX_PARTICLES — enabled emitter with 0 estimate is likely uncompiled
        if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
        {
            if (EmitterData->GetMaxParticleCountEstimate() == 0)
            {
                TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
                Issue->SetStringField(TEXT("issue_type"), TEXT("ZERO_MAX_PARTICLES"));
                Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
                Issue->SetStringField(TEXT("detail"),
                    FString::Printf(
                        TEXT("Enabled emitter '%s' reports a max particle estimate of 0. "
                             "The emitter may be uncompiled, have a zero spawn rate, or use "
                             "a burst-only module. Verify compilation and spawn settings."),
                        *Handle.GetName().ToString()));
                Issues.Add(MakeShared<FJsonValueObject>(Issue));
            }
        }
    }

    // NO_SCALABILITY — no overrides defined; system runs at full cost on all platforms
    if (ScalOverrides.Overrides.IsEmpty())
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("NO_SCALABILITY"));
        Issue->SetStringField(TEXT("severity"),   TEXT("info"));
        Issue->SetStringField(TEXT("detail"),
            TEXT("System has no scalability overrides. It spawns at full quality and cost "
                 "on all platforms. Add per-platform overrides with distance culling and "
                 "instance caps if this system is used in open-world contexts."));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // GPU_NO_SCALABILITY — GPU emitter with no culling is a budget risk
    bool bHasGPUEmitter = false;
    for (const FNiagaraEmitterHandle& EmHandle : Handles)
    {
        if (FVersionedNiagaraEmitterData* ED = EmHandle.GetEmitterData())
        {
            if (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim)
            {
                bHasGPUEmitter = true;
                break;
            }
        }
    }
    if (bHasGPUEmitter && ScalOverrides.Overrides.IsEmpty())
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("GPU_NO_SCALABILITY"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("detail"),
            TEXT("System contains a GPU-simulated emitter but has no scalability overrides. "
                 "GPU Niagara simulations are expensive and uncapped instances can cause "
                 "frametime spikes. Add per-platform instance caps and distance culling."));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    // WARMUP_COST — synchronous warmup ticks add hidden spawn cost
    if (WarmupTime > 0.f && WarmupTickCount > 0)
    {
        const float MsPerTick = WarmupTime / WarmupTickCount * 1000.f;
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("issue_type"), TEXT("WARMUP_COST"));
        Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
        Issue->SetStringField(TEXT("detail"),
            FString::Printf(
                TEXT("WarmupTime=%.2fs with %d warmup ticks (~%.1fms/tick). Each warmup tick "
                     "is simulated synchronously at spawn, adding hidden cost proportional to "
                     "particle density. Use sparingly and only where pre-warmed effects are required."),
                WarmupTime, WarmupTickCount, MsPerTick));
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
    AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
    AuditObj->SetArrayField (TEXT("issues"),       Issues);
    Root->SetObjectField(TEXT("audit"), AuditObj);

    // Derive a safe filename from the last path component
    FString SystemName = FPaths::GetBaseFilename(AssetPath);
    bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("niagara"), SystemName, Root);

    return bOK;
}

// ---------------------------------------------------------------------------
// ExportAllNiagaraSystems
// ---------------------------------------------------------------------------

int32 UForgeNiagaraCapture::ExportAllNiagaraSystems()
{
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(TEXT("/Game"));
    Filter.bRecursivePaths      = true;
    Filter.bRecursiveClasses    = false;

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    int32 Exported = 0;
    for (const FAssetData& Asset : Assets)
    {
        if (ExportNiagaraSystem(Asset.GetObjectPathString()))
        {
            ++Exported;
        }
    }

    UpdateIndexFile(Exported, false);

    UE_LOG(LogTemp, Log,
        TEXT("ForgeNiagaraCapture: Exported %d NiagaraSystem assets"), Exported);
    return Exported;
}

// ---------------------------------------------------------------------------
// CaptureNiagaraPoolState
// ---------------------------------------------------------------------------

bool UForgeNiagaraCapture::CaptureNiagaraPoolState()
{
    // Prefer the PIE world; fall back to the editor world for the manager lookup.
    UWorld* World = nullptr;
    if (GEditor)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE)
            {
                World = Ctx.World();
                break;
            }
        }
        if (!World)
        {
            World = GEditor->GetEditorWorldContext().World();
        }
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"), FForgeContextWriter::NowISO8601());

    const bool bPoolEnabled = UNiagaraComponentPool::Enabled();
    Root->SetBoolField(TEXT("pool_enabled"), bPoolEnabled);

    if (!World)
    {
        Root->SetStringField(TEXT("note"), TEXT("No valid world found. Pool data unavailable."));
        bool bOK = FForgeContextWriter::WriteJSON(
            OutputDir / TEXT("niagara"), TEXT("pool_state"), Root);
        UpdateIndexFile(-1, bOK);
        return bOK;
    }

    FNiagaraWorldManager* Manager = FNiagaraWorldManager::Get(World);
    if (!Manager)
    {
        Root->SetStringField(TEXT("note"), TEXT("FNiagaraWorldManager not available for this world."));
        bool bOK = FForgeContextWriter::WriteJSON(
            OutputDir / TEXT("niagara"), TEXT("pool_state"), Root);
        UpdateIndexFile(-1, bOK);
        return bOK;
    }

    UNiagaraComponentPool* Pool = Manager->GetComponentPool();

    // -----------------------------------------------------------------------
    // Active component tracking — only compiled in non-shipping (editor) builds
    // -----------------------------------------------------------------------
    int32 TotalActive = 0;
    int32 PeakTotal   = 0;
    TArray<TSharedPtr<FJsonValue>> SystemsInUse;

#if ENABLE_NC_POOL_DEBUGGING
    if (Pool)
    {
        TotalActive = Pool->InUseComponents_Auto.Num() + Pool->InUseComponents_Manual.Num();
        PeakTotal   = Pool->MaxUsed;

        // Group in-use components by their system asset to get per-system active counts.
        TMap<FString, int32> ActivePerSystem;
        auto CountComponents = [&](const TArray<TWeakObjectPtr<UNiagaraComponent>>& Components)
        {
            for (const TWeakObjectPtr<UNiagaraComponent>& WeakNC : Components)
            {
                if (!WeakNC.IsValid()) continue;
                UNiagaraSystem* Asset = WeakNC->GetAsset();
                if (!Asset) continue;
                FString Path = Asset->GetPathName();
                ActivePerSystem.FindOrAdd(Path)++;
            }
        };
        CountComponents(Pool->InUseComponents_Auto);
        CountComponents(Pool->InUseComponents_Manual);

        // Threshold: flag any system with more than 10 active components simultaneously.
        // This is a heuristic — tune per-project via the audit_threshold field.
        const int32 RiskThreshold = 10;
        for (const TTuple<FString, int32>& Pair : ActivePerSystem)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("system_path"),  Pair.Key);
            Entry->SetNumberField(TEXT("active_count"), Pair.Value);
            const bool bRisk = Pair.Value >= RiskThreshold;
            Entry->SetBoolField  (TEXT("risk_flag"),    bRisk);
            if (bRisk)
            {
                Entry->SetStringField(TEXT("risk_detail"),
                    FString::Printf(
                        TEXT("System has %d active pooled components (threshold: %d). "
                             "Pool may be exhausting its budget — consider priming with a higher cap "
                             "or reducing simultaneous activations."),
                        Pair.Value, RiskThreshold));
            }
            SystemsInUse.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
#else
    Root->SetStringField(TEXT("pool_debug_note"),
        TEXT("ENABLE_NC_POOL_DEBUGGING is false in this build config. "
             "Per-component tracking is unavailable. Rerun in Development or DebugGame."));
#endif

    Root->SetNumberField(TEXT("total_active"), TotalActive);
    Root->SetNumberField(TEXT("peak_total"),   PeakTotal);
    Root->SetArrayField (TEXT("systems_in_use"), SystemsInUse);

    bool bOK = FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("niagara"), TEXT("pool_state"), Root);
    UpdateIndexFile(-1, bOK);
    return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeNiagaraCapture::UpdateIndexFile(int32 SystemCount, bool bPoolCaptured)
{
    const FString IndexPath = OutputDir / TEXT("index.json");

    TSharedPtr<FJsonObject> Root;
    FString Raw;
    if (FFileHelper::LoadFileToString(Raw, *IndexPath))
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        FJsonSerializer::Deserialize(Reader, Root);
    }
    if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> Captures;
    if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
    {
        if (Found->IsValid() && (*Found)->Type == EJson::Object)
        {
            Captures = (*Found)->AsObject();
        }
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> NiagaraSection = MakeShared<FJsonObject>();
    NiagaraSection->SetStringField(TEXT("directory"),    TEXT("niagara/"));
    NiagaraSection->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    if (SystemCount >= 0)
    {
        NiagaraSection->SetNumberField(TEXT("system_count"), SystemCount);
    }
    NiagaraSection->SetBoolField(TEXT("pool_state_captured"), bPoolCaptured);
    Captures->SetObjectField(TEXT("niagara"), NiagaraSection);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
