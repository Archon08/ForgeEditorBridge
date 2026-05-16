#include "Capture/ForgePCGCapture.h"
#include "IO/ForgeContextWriter.h"

#include "PCGComponent.h"           // UPCGComponent, OnPCGGraphGeneratedDelegate
#include "Data/PCGPointData.h"      // UPCGPointData, FPCGPoint
#include "PCGData.h"                // FPCGDataCollection, FPCGTaggedData

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "EngineUtils.h"            // TActorIterator
#include "Editor.h"                 // GEditor, FEditorDelegates (transitive)
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates::PostLoadMapWithWorld

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UForgePCGCapture::Initialize(const FString& InOutputDir)
{
    UE_LOG(LogTemp, Log, TEXT("ForgePCG: Initialize"));
    OutputDir = InOutputDir;

    BindToAllPCGComponents();

    // Re-bind when a map is explicitly opened via the editor menu.
    MapOpenedHandle = FEditorDelegates::OnMapOpened.AddUObject(
        this, &UForgePCGCapture::OnMapOpened);

    // Re-bind after any map finishes loading, including session restore on startup.
    // This is the primary path when the editor opens with a level already loaded.
    PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
        this, &UForgePCGCapture::OnPostLoadMapWithWorld);
}

void UForgePCGCapture::Deinitialize()
{
    UnbindAllPCGComponents();

    FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
    MapOpenedHandle.Reset();

    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
    PostLoadMapHandle.Reset();
}

// ============================================================
// Binding
// ============================================================

void UForgePCGCapture::BindToAllPCGComponents()
{
    if (!GEngine)
        return;

    // GetEditorWorldContext() calls check(0) if no editor context exists - use safe iteration instead.
    UWorld* World = nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
    {
        if (Ctx.WorldType == EWorldType::Editor)
        {
            World = Ctx.World();
            break;
        }
    }
    if (!World)
        return;

    int32 Bound = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        UPCGComponent* PCGComp = (*It)->FindComponentByClass<UPCGComponent>();
        if (!PCGComp) continue;
        if (BoundComponents.Contains(TWeakObjectPtr<UPCGComponent>(PCGComp))) continue;

        FDelegateHandle Handle = PCGComp->OnPCGGraphGeneratedDelegate.AddUObject(
            this, &UForgePCGCapture::OnPCGGraphGenerated);
        BoundComponents.Add(TWeakObjectPtr<UPCGComponent>(PCGComp), Handle);
        Bound++;
    }
    // Listen for actors spawned into this world after binding (e.g. drag-drop from Content Browser).
    if (UWorld* OldWorld = SpawnListenerWorld.Get())
    {
        OldWorld->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
    }
    SpawnListenerWorld = World;
    ActorSpawnedHandle = World->AddOnActorSpawnedHandler(
        FOnActorSpawned::FDelegate::CreateUObject(this, &UForgePCGCapture::OnActorSpawnedInWorld));

    UE_LOG(LogTemp, Log, TEXT("ForgePCG: BindToAllPCGComponents - bound %d component(s), total %d, spawn listener active"), Bound, BoundComponents.Num());
}

void UForgePCGCapture::UnbindAllPCGComponents()
{
    for (auto& Pair : BoundComponents)
    {
        if (UPCGComponent* Comp = Pair.Key.Get())
        {
            Comp->OnPCGGraphGeneratedDelegate.Remove(Pair.Value);
        }
    }
    BoundComponents.Empty();

    if (UWorld* World = SpawnListenerWorld.Get())
    {
        World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
    }
    ActorSpawnedHandle.Reset();
    SpawnListenerWorld.Reset();
}

void UForgePCGCapture::OnMapOpened(const FString& Filename, bool bAsTemplate)
{
    UnbindAllPCGComponents();
    BindToAllPCGComponents();
}

void UForgePCGCapture::OnPostLoadMapWithWorld(UWorld* World)
{
    UE_LOG(LogTemp, Log, TEXT("ForgePCG: OnPostLoadMapWithWorld fired, world=%s"),
        World ? *World->GetName() : TEXT("null"));
    UnbindAllPCGComponents();
    BindToAllPCGComponents();
}

void UForgePCGCapture::OnActorSpawnedInWorld(AActor* Actor)
{
    if (!Actor) return;
    UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
    if (!PCGComp) return;

    TWeakObjectPtr<UPCGComponent> WeakComp(PCGComp);
    if (BoundComponents.Contains(WeakComp)) return;

    FDelegateHandle Handle = PCGComp->OnPCGGraphGeneratedDelegate.AddUObject(
        this, &UForgePCGCapture::OnPCGGraphGenerated);
    BoundComponents.Add(WeakComp, Handle);
    UE_LOG(LogTemp, Log, TEXT("ForgePCG: OnActorSpawnedInWorld - bound PCGComponent on %s"), *Actor->GetName());
}

// ============================================================
// Delegate handler
// ============================================================

void UForgePCGCapture::OnPCGGraphGenerated(UPCGComponent* PCGComponent)
{
    UE_LOG(LogTemp, Log, TEXT("ForgePCG: OnPCGGraphGenerated fired, actor=%s"),
        PCGComponent && PCGComponent->GetOwner() ? *PCGComponent->GetOwner()->GetName() : TEXT("null"));
    if (!PCGComponent) return;

    TSharedPtr<FJsonObject> Root = BuildPointCloudJSON(PCGComponent);
    if (Root.IsValid())
    {
        FForgeContextWriter::WriteJSON(OutputDir / TEXT("pcg"), TEXT("last-execute.json"),
                                        Root.ToSharedRef());
        UpdateIndexFile();
    }
}

// ============================================================
// JSON builders
// ============================================================

TSharedPtr<FJsonObject> UForgePCGCapture::BuildPointCloudJSON(UPCGComponent* PCGComponent)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FForgeContextWriter::NowISO8601());

    // Actor identity
    Root->SetStringField(TEXT("actor"), PCGComponent->GetOwner()
        ? PCGComponent->GetOwner()->GetName() : TEXT("Unknown"));

    // Collect all points across all tagged output slots.
    // VERIFIED 5.7: GetGeneratedGraphOutput() + GetAllInputs() + FPCGDataPtrWrapper::Get()
    const FPCGDataCollection& Output = PCGComponent->GetGeneratedGraphOutput();

    TArray<FPCGPoint> AllPoints;
    for (const FPCGTaggedData& TD : Output.GetAllInputs())
    {
        // FPCGTaggedData.Data is FPCGDataPtrWrapper in 5.7. .Get() returns const UPCGData*.
        if (const UPCGPointData* PD = Cast<UPCGPointData>(TD.Data.Get()))
        {
            AllPoints.Append(PD->GetPoints());
        }
    }

    const int32 TotalCount  = AllPoints.Num();
    const bool  bTruncated  = TotalCount > MaxExportPoints;
    const int32 ExportCount = FMath::Min(TotalCount, MaxExportPoints);

    Root->SetNumberField(TEXT("point_count_total"),    static_cast<double>(TotalCount));
    Root->SetNumberField(TEXT("point_count_exported"), static_cast<double>(ExportCount));
    Root->SetBoolField  (TEXT("truncated"),            bTruncated);

    TArray<float> Densities;
    Densities.Reserve(ExportCount);

    TArray<TSharedPtr<FJsonValue>> PointValues;
    PointValues.Reserve(ExportCount);

    for (int32 i = 0; i < ExportCount; i++)
    {
        const FPCGPoint& Pt = AllPoints[i];

        TSharedPtr<FJsonObject> PtObj = MakeShared<FJsonObject>();

        TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
        const FVector Loc = Pt.Transform.GetLocation();
        Pos->SetNumberField(TEXT("x"), Loc.X);
        Pos->SetNumberField(TEXT("y"), Loc.Y);
        Pos->SetNumberField(TEXT("z"), Loc.Z);
        PtObj->SetObjectField(TEXT("position"), Pos);

        PtObj->SetNumberField(TEXT("density"), Pt.Density);
        PtObj->SetNumberField(TEXT("seed"),    static_cast<double>(Pt.Seed));

        Densities.Add(Pt.Density);
        PointValues.Add(MakeShared<FJsonValueObject>(PtObj));
    }

    Root->SetArrayField (TEXT("points"),            PointValues);
    Root->SetObjectField(TEXT("density_histogram"), BuildDensityHistogram(Densities));

    return Root;
}

TSharedPtr<FJsonObject> UForgePCGCapture::BuildDensityHistogram(const TArray<float>& Densities)
{
    int32 Buckets[5] = {0, 0, 0, 0, 0};
    for (float D : Densities)
    {
        const int32 Idx = FMath::Clamp(FMath::FloorToInt(D * 5.0f), 0, 4);
        Buckets[Idx]++;
    }

    TSharedPtr<FJsonObject> Hist = MakeShared<FJsonObject>();
    Hist->SetNumberField(TEXT("0.0-0.2"), Buckets[0]);
    Hist->SetNumberField(TEXT("0.2-0.4"), Buckets[1]);
    Hist->SetNumberField(TEXT("0.4-0.6"), Buckets[2]);
    Hist->SetNumberField(TEXT("0.6-0.8"), Buckets[3]);
    Hist->SetNumberField(TEXT("0.8-1.0"), Buckets[4]);
    return Hist;
}

// ============================================================
// Index
// ============================================================

void UForgePCGCapture::UpdateIndexFile()
{
    TSharedPtr<FJsonObject> PCGSection = MakeShared<FJsonObject>();
    PCGSection->SetStringField(TEXT("file"),         TEXT("pcg/last-execute.json"));
    PCGSection->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());

    TSharedPtr<FJsonObject> Captures = MakeShared<FJsonObject>();
    Captures->SetField(TEXT("build_errors"),    MakeShared<FJsonValueNull>());
    Captures->SetObjectField(TEXT("pcg_last_execute"), PCGSection);
    Captures->SetField(TEXT("screenshot_latest"), MakeShared<FJsonValueNull>());
    Captures->SetField(TEXT("runtime_log"),       MakeShared<FJsonValueNull>());
    Captures->SetField(TEXT("heightmap"),         MakeShared<FJsonValueNull>());

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("updated"),        FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("plugin_version"), TEXT("0.2.6"));
    Root->SetObjectField(TEXT("captures_available"), Captures);

    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index.json"), Root.ToSharedRef());
}
