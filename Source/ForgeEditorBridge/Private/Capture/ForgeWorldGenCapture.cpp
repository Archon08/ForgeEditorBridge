#include "Capture/ForgeWorldGenCapture.h"
#include "IO/ForgeContextWriter.h"

// --- PCG ---
#include "PCGVolume.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

// --- World / Actor iteration ---
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// --- World Partition ---
#include "WorldPartition/WorldPartition.h"

// --- UObject reflection ---
#include "UObject/UnrealType.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    // Serialize a single primitive FProperty value into a JSON object with "name", "type", "value".
    // Returns false if the property type is unsupported (caller handles struct/array separately).
    bool SerializePrimitiveProp(FProperty* Prop, const void* ValuePtr, TSharedRef<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("name"), Prop->GetName());

        if (const FIntProperty* IP = CastField<FIntProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("int32"));
            Out->SetNumberField(TEXT("value"), IP->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FFloatProperty* FP = CastField<FFloatProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("float"));
            Out->SetNumberField(TEXT("value"), FP->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("double"));
            Out->SetNumberField(TEXT("value"), DP->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FStrProperty* SP = CastField<FStrProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("FString"));
            Out->SetStringField(TEXT("value"), SP->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FNameProperty* NP = CastField<FNameProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("FName"));
            Out->SetStringField(TEXT("value"), NP->GetPropertyValue(ValuePtr).ToString());
            return true;
        }
        if (const FBoolProperty* BP = CastField<FBoolProperty>(Prop))
        {
            Out->SetStringField(TEXT("type"),  TEXT("bool"));
            Out->SetBoolField  (TEXT("value"), BP->GetPropertyValue(ValuePtr));
            return true;
        }
        return false;
    }

    // Build a JSON array of graph parameters from a UPCGGraph via reflection.
    // Exports any CPF_Edit property not owned by UObject or UPCGGraphInterface base.
    TArray<TSharedPtr<FJsonValue>> BuildGraphParamsArray(UPCGGraphInterface* GraphIface)
    {
        TArray<TSharedPtr<FJsonValue>> ParamArr;
        if (!GraphIface) return ParamArr;

        UPCGGraph* Graph = Cast<UPCGGraph>(GraphIface);
        if (!Graph) return ParamArr;

        // Class names to skip (base engine classes — not user-defined params)
        static const TArray<FString> SkipClasses = {
            TEXT("Object"), TEXT("PCGGraphInterface"), TEXT("PCGGraph")
        };

        for (TFieldIterator<FProperty> PropIt(Graph->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop) continue;

            // Skip if owned by an engine base class
            const FString OwnerName = Prop->GetOwnerClass()
                ? Prop->GetOwnerClass()->GetName() : TEXT("");
            bool bSkip = false;
            for (const FString& Skip : SkipClasses)
            {
                if (OwnerName.Contains(Skip, ESearchCase::IgnoreCase)) { bSkip = true; break; }
            }
            if (bSkip) continue;

            // Only export user-editable (exposed) properties
            if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

            TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Graph);

            if (SerializePrimitiveProp(Prop, ValuePtr, PObj))
            {
                ParamArr.Add(MakeShared<FJsonValueObject>(PObj));
            }
        }
        return ParamArr;
    }

    // Build a JSON object for a single actor + UPCGComponent pair.
    TSharedRef<FJsonObject> SerializePCGActor(AActor* Actor, UPCGComponent* PCGComp)
    {
        TSharedRef<FJsonObject> VObj = MakeShared<FJsonObject>();
        VObj->SetStringField(TEXT("actor_name"),  Actor->GetActorNameOrLabel());
        VObj->SetStringField(TEXT("actor_path"),  Actor->GetPathName());
        VObj->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
        VObj->SetNumberField(TEXT("seed"),        PCGComp->Seed);

        VObj->SetStringField(TEXT("graph_asset"),
            PCGComp->GetGraph() ? PCGComp->GetGraph()->GetPathName() : TEXT("(none)"));

        VObj->SetArrayField(TEXT("graph_parameters"),
            BuildGraphParamsArray(PCGComp->GetGraph()));

        return VObj;
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeWorldGenCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("worldgen")));
    UE_LOG(LogTemp, Log, TEXT("ForgeWorldGen: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportWorldGenState
// ---------------------------------------------------------------------------

bool UForgeWorldGenCapture::ExportWorldGenState()
{
    if (!GEditor)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeWorldGen: GEditor is null"));
        return false;
    }

    // GetEditorWorldContext() check(0)-asserts when no editor context exists —
    // use safe GEngine world iteration (same fix as ForgeHeightmapCapture).
    UWorld* World = nullptr;
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::Editor) { World = Ctx.World(); break; }
        }
    }
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("ForgeWorldGen: No editor world found"));
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
    Root->SetStringField(TEXT("level_name"), World->GetName());

    // -------------------------------------------------------------------------
    // World Partition — enabled flag + depth info via reflection
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> WPObj = MakeShared<FJsonObject>();
        UWorldPartition* WP = World->GetWorldPartition();
        const bool bEnabled = (WP != nullptr);
        WPObj->SetBoolField(TEXT("enabled"), bEnabled);

        if (WP)
        {
            // Cell size — field name varies by UE version; search by name pattern
            for (TFieldIterator<FProperty> PropIt(WP->GetClass()); PropIt; ++PropIt)
            {
                FProperty* Prop = *PropIt;
                const FString Name = Prop->GetName();
                if (!Name.Contains(TEXT("CellSize"), ESearchCase::IgnoreCase)) continue;

                const void* VP = Prop->ContainerPtrToValuePtr<void>(WP);
                if (const FFloatProperty* FP = CastField<FFloatProperty>(Prop))
                {
                    WPObj->SetNumberField(TEXT("cell_size"), FP->GetPropertyValue(VP));
                    break;
                }
                if (const FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
                {
                    WPObj->SetNumberField(TEXT("cell_size"), DP->GetPropertyValue(VP));
                    break;
                }
                if (const FIntProperty* IP = CastField<FIntProperty>(Prop))
                {
                    WPObj->SetNumberField(TEXT("cell_size"), (double)IP->GetPropertyValue(VP));
                    break;
                }
            }

            // HLOD layers — find any TArray property whose name contains "HLOD"
            TArray<TSharedPtr<FJsonValue>> HLODArr;
            for (TFieldIterator<FProperty> PropIt(WP->GetClass()); PropIt; ++PropIt)
            {
                FArrayProperty* ArrProp = CastField<FArrayProperty>(*PropIt);
                if (!ArrProp) continue;
                if (!ArrProp->GetName().Contains(TEXT("HLOD"), ESearchCase::IgnoreCase)) continue;

                FScriptArrayHelper ArrayHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(WP));
                for (int32 i = 0; i < ArrayHelper.Num(); ++i)
                {
                    const FObjectProperty* ObjProp = CastField<FObjectProperty>(ArrProp->Inner);
                    if (ObjProp)
                    {
                        const UObject* Item = ObjProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
                        if (Item)
                            HLODArr.Add(MakeShared<FJsonValueString>(Item->GetName()));
                    }
                    else if (const FNameProperty* NP = CastField<FNameProperty>(ArrProp->Inner))
                    {
                        HLODArr.Add(MakeShared<FJsonValueString>(
                            NP->GetPropertyValue(ArrayHelper.GetRawPtr(i)).ToString()));
                    }
                    else if (const FStrProperty* SP = CastField<FStrProperty>(ArrProp->Inner))
                    {
                        HLODArr.Add(MakeShared<FJsonValueString>(
                            SP->GetPropertyValue(ArrayHelper.GetRawPtr(i))));
                    }
                }
                break;  // first matching HLOD array is sufficient
            }
            WPObj->SetArrayField(TEXT("hlod_layers"), HLODArr);

            // Streaming source count — find any TArray property whose name contains "StreamingSource"
            int32 StreamingSourceCount = 0;
            for (TFieldIterator<FProperty> PropIt(WP->GetClass()); PropIt; ++PropIt)
            {
                FArrayProperty* ArrProp = CastField<FArrayProperty>(*PropIt);
                if (!ArrProp) continue;
                if (!ArrProp->GetName().Contains(TEXT("StreamingSource"), ESearchCase::IgnoreCase)) continue;

                FScriptArrayHelper ArrayHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(WP));
                StreamingSourceCount = ArrayHelper.Num();
                break;
            }
            WPObj->SetNumberField(TEXT("streaming_source_count"), StreamingSourceCount);
        }

        Root->SetObjectField(TEXT("world_partition"), WPObj);
    }

    // -------------------------------------------------------------------------
    // PCG — ALL actors with a UPCGComponent
    // Pass 1: APCGVolume subclasses (canonical case)
    // Pass 2: any other AActor with a UPCGComponent attached
    // -------------------------------------------------------------------------
    {
        TArray<TSharedPtr<FJsonValue>> VolumeArr;

        // Pass 1 — APCGVolume actors
        for (TActorIterator<APCGVolume> It(World); It; ++It)
        {
            APCGVolume* Vol = *It;
            if (!Vol) continue;
            UPCGComponent* PCGComp = Vol->FindComponentByClass<UPCGComponent>();
            if (!PCGComp) continue;
            VolumeArr.Add(MakeShared<FJsonValueObject>(SerializePCGActor(Vol, PCGComp)));
        }

        // Pass 2 — non-APCGVolume actors with a UPCGComponent
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor) continue;
            if (Actor->IsA<APCGVolume>()) continue;  // already captured in pass 1

            UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
            if (!PCGComp) continue;

            VolumeArr.Add(MakeShared<FJsonValueObject>(SerializePCGActor(Actor, PCGComp)));
        }

        TSharedRef<FJsonObject> PCGObj = MakeShared<FJsonObject>();
        PCGObj->SetNumberField(TEXT("volume_count"), VolumeArr.Num());
        PCGObj->SetArrayField (TEXT("volumes"),      VolumeArr);
        Root->SetObjectField(TEXT("pcg"), PCGObj);
    }

    // -------------------------------------------------------------------------
    // Terrain Manager — any AActor whose class name contains "Terrain"
    //
    // Primitive types exported directly.
    // TArray: exported as {type, element_count, first_elements[]}.
    // Struct: recursed one level — sub-fields exported as {type, struct_type, fields{}}.
    // -------------------------------------------------------------------------
    {
        TSharedRef<FJsonObject> TerrainObj = MakeShared<FJsonObject>();

        AActor* TerrainActor = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor) continue;
            if (Actor->GetClass()->GetName().Contains(TEXT("Terrain"), ESearchCase::IgnoreCase))
            {
                TerrainActor = Actor;
                break;
            }
        }

        if (TerrainActor)
        {
            TerrainObj->SetBoolField  (TEXT("found"),       true);
            TerrainObj->SetStringField(TEXT("actor_name"),  TerrainActor->GetActorNameOrLabel());
            TerrainObj->SetStringField(TEXT("actor_class"), TerrainActor->GetClass()->GetName());

            TArray<TSharedPtr<FJsonValue>> PropArr;

            for (TFieldIterator<FProperty> It(TerrainActor->GetClass()); It; ++It)
            {
                FProperty* Prop = *It;
                if (!Prop) continue;

                // Skip properties inherited directly from AActor
                if (Prop->GetOwnerClass() == AActor::StaticClass()) continue;

                TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();

                const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TerrainActor);

                // --- Primitive types ---
                if (SerializePrimitiveProp(Prop, ValuePtr, PropObj))
                {
                    PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
                    continue;
                }

                // --- TArray ---
                if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
                {
                    PropObj->SetStringField(TEXT("name"), Prop->GetName());
                    PropObj->SetStringField(TEXT("type"), TEXT("TArray"));

                    FScriptArrayHelper ArrayHelper(ArrProp, ValuePtr);
                    PropObj->SetNumberField(TEXT("element_count"), ArrayHelper.Num());

                    // Export up to the first 3 elements if the inner type is primitive
                    TArray<TSharedPtr<FJsonValue>> ElemArr;
                    const int32 ExportCount = FMath::Min(ArrayHelper.Num(), 3);
                    for (int32 i = 0; i < ExportCount; ++i)
                    {
                        const void* ElemPtr = ArrayHelper.GetRawPtr(i);
                        if (const FIntProperty* IP = CastField<FIntProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueNumber>(IP->GetPropertyValue(ElemPtr)));
                        else if (const FFloatProperty* FP = CastField<FFloatProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueNumber>(FP->GetPropertyValue(ElemPtr)));
                        else if (const FDoubleProperty* DP = CastField<FDoubleProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueNumber>(DP->GetPropertyValue(ElemPtr)));
                        else if (const FStrProperty* SP = CastField<FStrProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueString>(SP->GetPropertyValue(ElemPtr)));
                        else if (const FNameProperty* NP = CastField<FNameProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueString>(NP->GetPropertyValue(ElemPtr).ToString()));
                        else if (const FBoolProperty* BP = CastField<FBoolProperty>(ArrProp->Inner))
                            ElemArr.Add(MakeShared<FJsonValueBoolean>(BP->GetPropertyValue(ElemPtr)));
                        else
                            break;  // non-primitive inner type — stop sampling
                    }
                    if (ElemArr.Num() > 0)
                        PropObj->SetArrayField(TEXT("first_elements"), ElemArr);

                    PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
                    continue;
                }

                // --- Struct (recurse one level) ---
                if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    PropObj->SetStringField(TEXT("name"),        Prop->GetName());
                    PropObj->SetStringField(TEXT("type"),        TEXT("struct"));
                    PropObj->SetStringField(TEXT("struct_type"), StructProp->Struct->GetName());

                    const void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(TerrainActor);
                    TSharedRef<FJsonObject> SubFieldsObj = MakeShared<FJsonObject>();

                    for (TFieldIterator<FProperty> SubIt(StructProp->Struct); SubIt; ++SubIt)
                    {
                        FProperty* SubProp = *SubIt;
                        if (!SubProp) continue;
                        const void* SubVP = SubProp->ContainerPtrToValuePtr<void>(StructPtr);

                        TSharedRef<FJsonObject> Dummy = MakeShared<FJsonObject>();
                        if (SerializePrimitiveProp(SubProp, SubVP, Dummy))
                        {
                            // Pull "value" from Dummy into SubFieldsObj keyed by field name
                            const TSharedPtr<FJsonValue>* ValField = Dummy->Values.Find(TEXT("value"));
                            if (ValField && ValField->IsValid())
                                SubFieldsObj->SetField(SubProp->GetName(), *ValField);
                        }
                        // Nested struct/array within a struct: skip (one level only)
                    }

                    PropObj->SetObjectField(TEXT("fields"), SubFieldsObj);
                    PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
                    continue;
                }

                // --- Everything else ---
                PropObj->SetStringField(TEXT("name"),  Prop->GetName());
                PropObj->SetStringField(TEXT("type"),  TEXT("unsupported"));
                PropObj->SetStringField(TEXT("value"), Prop->GetClass()->GetName());
                PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
            }

            TerrainObj->SetArrayField(TEXT("properties"), PropArr);
        }
        else
        {
            TerrainObj->SetBoolField(TEXT("found"), false);
        }

        Root->SetObjectField(TEXT("terrain"), TerrainObj);
    }

    bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("worldgen"), TEXT("parameters"), Root);
    if (bOK)
    {
        UE_LOG(LogTemp, Log, TEXT("ForgeWorldGen: Exported -> worldgen/parameters.json"));
        UpdateIndexFile();
    }
    return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeWorldGenCapture::UpdateIndexFile()
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
            Captures = (*Found)->AsObject();
    }
    if (!Captures.IsValid())
    {
        Captures = MakeShared<FJsonObject>();
        Root->SetObjectField(TEXT("captures_available"), Captures);
    }

    TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
    Section->SetStringField(TEXT("file"),         TEXT("worldgen/parameters.json"));
    Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    Captures->SetObjectField(TEXT("worldgen"), Section);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
