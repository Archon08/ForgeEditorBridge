#include "Handlers/ActorHandler.h"
#include "ForgeAISubsystem.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "CollisionShape.h"
#include "CollisionQueryParams.h"
#include "Math/UnrealMathUtility.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"
#include "Selection.h"

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace
{

/** Try to resolve a UClass from a simple name ("StaticMeshActor") or full asset path ("/Game/BP_Foo"). */
static UClass* ResolveClass(const FString& ClassName)
{
    if (ClassName.IsEmpty()) return nullptr;

    // Full path — assume Blueprint-generated class, append _C if needed
    if (ClassName.StartsWith(TEXT("/")))
    {
        FString FullPath = ClassName;
        if (!FullPath.EndsWith(TEXT("_C")))
            FullPath += TEXT("_C");
        if (UClass* Cls = LoadClass<AActor>(nullptr, *FullPath))
            return Cls;
    }

    // Engine scripts (most common native actors live here)
    static const TCHAR* NativeScripts[] = {
        TEXT("/Script/Engine."),
        TEXT("/Script/GameFramework."),
        TEXT("/Script/UMG."),
        TEXT("/Script/AIModule."),
    };
    for (const TCHAR* Prefix : NativeScripts)
    {
        FString EngPath = FString(Prefix) + ClassName;
        if (UClass* Cls = FindObject<UClass>(nullptr, *EngPath))
            return Cls;
    }

    // Brute-force: scan loaded classes (expensive but guaranteed)
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
            return *It;
    }
    return nullptr;
}

/** Get the current PIE-safe editor world. */
static UWorld* GetEditorWorld()
{
    if (!GEditor) return nullptr;
    return GEditor->GetEditorWorldContext().World();
}

/** Serialize a single actor to a lightweight JSON object. */
static TSharedPtr<FJsonObject> ActorToJson(AActor* Actor)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Actor) return Obj;

    Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

    auto MakeVecObj = [](const FVector& V) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("x"), V.X);
        O->SetNumberField(TEXT("y"), V.Y);
        O->SetNumberField(TEXT("z"), V.Z);
        return O;
    };
    auto MakeRotObj = [](const FRotator& R) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("pitch"), R.Pitch);
        O->SetNumberField(TEXT("yaw"),   R.Yaw);
        O->SetNumberField(TEXT("roll"),  R.Roll);
        return O;
    };

    Obj->SetObjectField(TEXT("location"), MakeVecObj(Actor->GetActorLocation()));
    Obj->SetObjectField(TEXT("rotation"), MakeRotObj(Actor->GetActorRotation()));
    Obj->SetObjectField(TEXT("scale"),    MakeVecObj(Actor->GetActorScale3D()));

    return Obj;
}

/** Read a {x,y,z} sub-object from Params[Key]. Returns false if key absent. */
static bool ReadVec3(TSharedPtr<FJsonObject> Params, const FString& Key, FVector& OutVec)
{
    const TSharedPtr<FJsonObject>* SubObj = nullptr;
    if (!Params->TryGetObjectField(Key, SubObj)) return false;
    double X = 0, Y = 0, Z = 0;
    (*SubObj)->TryGetNumberField(TEXT("x"), X);
    (*SubObj)->TryGetNumberField(TEXT("y"), Y);
    (*SubObj)->TryGetNumberField(TEXT("z"), Z);
    OutVec = FVector(X, Y, Z);
    return true;
}

/** Read a {pitch,yaw,roll} sub-object from Params[Key]. Returns false if key absent. */
static bool ReadRotator(TSharedPtr<FJsonObject> Params, const FString& Key, FRotator& OutRot)
{
    const TSharedPtr<FJsonObject>* SubObj = nullptr;
    if (!Params->TryGetObjectField(Key, SubObj)) return false;
    double Pitch = 0, Yaw = 0, Roll = 0;
    (*SubObj)->TryGetNumberField(TEXT("pitch"), Pitch);
    (*SubObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
    (*SubObj)->TryGetNumberField(TEXT("roll"),  Roll);
    OutRot = FRotator(Pitch, Yaw, Roll);
    return true;
}

/** Load a UBlueprint from a package path like "/Game/AI/BP_Foo". */
static UBlueprint* LoadBlueprint(const FString& PackagePath)
{
    FString ShortName = FPackageName::GetShortName(PackagePath);
    FString ObjectPath = PackagePath + TEXT(".") + ShortName;
    return LoadObject<UBlueprint>(nullptr, *ObjectPath);
}

/**
 * Find an actor by path (GetPathName()) or by label (GetActorLabel()).
 * Tries exact path match first, then falls back to label search.
 */
static AActor* FindActor(UWorld* World, const FString& PathOrLabel)
{
    if (!World || PathOrLabel.IsEmpty()) return nullptr;

    // Try exact object path first
    if (AActor* ByPath = FindObject<AActor>(nullptr, *PathOrLabel))
        return ByPath;

    // Fallback: label search (case-insensitive)
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if ((*It)->GetActorLabel().Equals(PathOrLabel, ESearchCase::IgnoreCase))
            return *It;
    }
    return nullptr;
}

} // anonymous namespace

// Windows <errno.h> defines DOMAIN as 1 — undefine to avoid macro conflict.
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("actor");

// ---------------------------------------------------------------------------
// HandleCommand
// ---------------------------------------------------------------------------

FBridgeResult UActorHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("actor"), Action);

    if (!Params.IsValid())
    {
        Result.Message = TEXT("Params object is null");
        return Result;
    }

    // -----------------------------------------------------------------------
    // spawn_actor
    // -----------------------------------------------------------------------
    if (Action == TEXT("spawn_actor"))
    {
        FString ClassName;
        if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
        {
            Result.Message = TEXT("spawn_actor: 'class_name' is required");
            return Result;
        }

        UClass* ActorClass = ResolveClass(ClassName);
        if (!ActorClass)
        {
            Result.Message = FString::Printf(TEXT("spawn_actor: could not resolve class '%s'"), *ClassName);
            return Result;
        }
        if (!ActorClass->IsChildOf(AActor::StaticClass()))
        {
            Result.Message = FString::Printf(TEXT("spawn_actor: '%s' is not an AActor subclass"), *ClassName);
            return Result;
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
            Result.Message = TEXT("spawn_actor: no editor world available");
            return Result;
        }

        FVector   Location = FVector::ZeroVector;
        FRotator  Rotation = FRotator::ZeroRotator;
        ReadVec3(Params,    TEXT("location"), Location);
        ReadRotator(Params, TEXT("rotation"), Rotation);

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        AActor* Spawned = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location), SpawnParams);
        if (!Spawned)
        {
            Result.Message = TEXT("spawn_actor: World::SpawnActor returned null");
            return Result;
        }

        FString Label;
        if (Params->TryGetStringField(TEXT("label"), Label))
            Spawned->SetActorLabel(Label);

        // Optional: apply a static mesh to the spawned actor.
        FString StaticMeshPath;
        FString MeshWarning;
        FString MeshAppliedPath;
        if (Params->TryGetStringField(TEXT("static_mesh"), StaticMeshPath) && !StaticMeshPath.IsEmpty())
        {
            UStaticMeshComponent* SMC = nullptr;

            // Prefer GetStaticMeshComponent() when the spawned actor is a StaticMeshActor
            if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Spawned))
                SMC = SMA->GetStaticMeshComponent();
            if (!SMC)
                SMC = Spawned->FindComponentByClass<UStaticMeshComponent>();

            if (!SMC)
            {
                MeshWarning = TEXT("Actor class has no UStaticMeshComponent; mesh was ignored");
            }
            else
            {
                UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
                if (!Mesh)
                {
                    MeshWarning = FString::Printf(TEXT("Failed to load UStaticMesh '%s'; mesh was ignored"),
                        *StaticMeshPath);
                }
                else
                {
                    SMC->Modify();
                    Spawned->Modify();
                    SMC->SetStaticMesh(Mesh);
                    Spawned->MarkPackageDirty();
                    MeshAppliedPath = StaticMeshPath;
                }
            }
        }

        Result.bSuccess    = true;
        Result.AffectedPath = Spawned->GetActorLabel();
        if (!MeshAppliedPath.IsEmpty())
        {
            Result.Message = FString::Printf(
                TEXT("Spawned '%s' (%s) at (%.1f, %.1f, %.1f); static_mesh='%s'"),
                *Spawned->GetActorLabel(), *ClassName, Location.X, Location.Y, Location.Z,
                *MeshAppliedPath);
        }
        else if (!MeshWarning.IsEmpty())
        {
            Result.Message = FString::Printf(
                TEXT("Spawned '%s' (%s) at (%.1f, %.1f, %.1f); warning: %s"),
                *Spawned->GetActorLabel(), *ClassName, Location.X, Location.Y, Location.Z,
                *MeshWarning);
        }
        else
        {
            Result.Message = FString::Printf(TEXT("Spawned '%s' (%s) at (%.1f, %.1f, %.1f)"),
                *Spawned->GetActorLabel(), *ClassName, Location.X, Location.Y, Location.Z);
        }
    }

    // -----------------------------------------------------------------------
    // set_actor_transform
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_actor_transform"))
    {
        FString ActorLabel;
        if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
        {
            Result.Message = TEXT("set_actor_transform: 'actor_label' is required");
            return Result;
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
            Result.Message = TEXT("set_actor_transform: no editor world");
            return Result;
        }

        AActor* Target = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if ((*It)->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase))
            {
                Target = *It;
                break;
            }
        }
        if (!Target)
        {
            Result.Message = FString::Printf(TEXT("set_actor_transform: actor '%s' not found in level"), *ActorLabel);
            return Result;
        }

        // Start from current transform so unspecified fields keep their values
        FVector  Location = Target->GetActorLocation();
        FRotator Rotation = Target->GetActorRotation();
        FVector  Scale    = Target->GetActorScale3D();

        ReadVec3(Params,    TEXT("location"), Location);
        const bool bHasRotation = ReadRotator(Params, TEXT("rotation"), Rotation);
        ReadVec3(Params,    TEXT("scale"),    Scale);

        // Optional: derive rotation from a world-space normal vector.
        FVector Normal = FVector::ZeroVector;
        const bool bHasNormal = ReadVec3(Params, TEXT("rotation_from_normal"), Normal);
        bool bPreserveYaw = false;
        Params->TryGetBoolField(TEXT("rotation_from_normal_preserve_yaw"), bPreserveYaw);

        if (bHasNormal)
        {
            if (Normal.IsNearlyZero())
            {
                return MakeError(DOMAIN, Action, 1001,
                    TEXT("'rotation_from_normal' must be a non-zero vector"),
                    TEXT("Pass the surface normal as {x,y,z} with magnitude > 0"));
            }
            Normal.Normalize();

            FRotator FromNormal = FRotationMatrix::MakeFromZ(Normal).Rotator();
            if (bPreserveYaw)
            {
                // Preserve yaw from the provided 'rotation' input (or current actor yaw if absent)
                FromNormal.Yaw = Rotation.Yaw;
            }
            Rotation = FromNormal;
        }

        Target->Modify();
        Target->SetActorTransform(FTransform(Rotation, Location, Scale));

        Result.bSuccess    = true;
        Result.AffectedPath = ActorLabel;
        Result.Message      = bHasNormal
            ? FString::Printf(TEXT("Transform updated for '%s' (rotation_from_normal%s)"),
                *ActorLabel, bPreserveYaw ? TEXT(", preserve_yaw=true") : TEXT(""))
            : FString::Printf(TEXT("Transform updated for '%s'"), *ActorLabel);
        (void)bHasRotation; // silence unused-warning when no normal path
    }

    // -----------------------------------------------------------------------
    // find_actors_by_name
    // -----------------------------------------------------------------------
    else if (Action == TEXT("find_actors_by_name"))
    {
        FString NamePattern;
        Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

        UWorld* World = GetEditorWorld();
        if (!World)
        {
            Result.Message = TEXT("find_actors_by_name: no editor world");
            return Result;
        }

        TArray<TSharedPtr<FJsonValue>> ActorArr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (NamePattern.IsEmpty() ||
                Actor->GetActorLabel().Contains(NamePattern, ESearchCase::IgnoreCase))
            {
                ActorArr.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor)));
            }
        }

        FString JsonStr;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
        FJsonSerializer::Serialize(ActorArr, Writer);

        Result.bSuccess  = true;
        Result.Message   = FString::Printf(TEXT("Found %d actor(s) matching '%s'"), ActorArr.Num(), *NamePattern);
        Result.ExtraData = JsonStr;
    }

    // -----------------------------------------------------------------------
    // get_actors_in_level
    // -----------------------------------------------------------------------
    else if (Action == TEXT("get_actors_in_level"))
    {
        UWorld* World = GetEditorWorld();
        if (!World)
        {
            Result.Message = TEXT("get_actors_in_level: no editor world");
            return Result;
        }

        TArray<TSharedPtr<FJsonValue>> ActorArr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            ActorArr.Add(MakeShared<FJsonValueObject>(ActorToJson(*It)));
        }

        FString JsonStr;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
        FJsonSerializer::Serialize(ActorArr, Writer);

        Result.bSuccess  = true;
        Result.Message   = FString::Printf(TEXT("Level contains %d actor(s)"), ActorArr.Num());
        Result.ExtraData = JsonStr;
    }

    // -----------------------------------------------------------------------
    // add_component_to_blueprint
    // -----------------------------------------------------------------------
    else if (Action == TEXT("add_component_to_blueprint"))
    {
        FString BlueprintPath, ComponentClass, ComponentName;
        if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
        {
            Result.Message = TEXT("add_component_to_blueprint: 'blueprint_path' is required");
            return Result;
        }
        if (!Params->TryGetStringField(TEXT("component_class"), ComponentClass))
        {
            Result.Message = TEXT("add_component_to_blueprint: 'component_class' is required");
            return Result;
        }
        Params->TryGetStringField(TEXT("component_name"), ComponentName);
        if (ComponentName.IsEmpty())
            ComponentName = ComponentClass;

        UBlueprint* BP = LoadBlueprint(BlueprintPath);
        if (!BP || !BP->SimpleConstructionScript)
        {
            Result.Message = FString::Printf(
                TEXT("add_component_to_blueprint: could not load Blueprint '%s' (or SCS is null)"), *BlueprintPath);
            return Result;
        }

        UClass* CompClass = ResolveClass(ComponentClass);
        if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
        {
            Result.Message = FString::Printf(
                TEXT("add_component_to_blueprint: '%s' is not a valid UActorComponent class"), *ComponentClass);
            return Result;
        }

        USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
        USCS_Node* NewNode = SCS->CreateNode(CompClass, *ComponentName);
        if (!NewNode)
        {
            Result.Message = TEXT("add_component_to_blueprint: SCS->CreateNode returned null");
            return Result;
        }
        SCS->AddNode(NewNode);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

        Result.bSuccess    = true;
        Result.AffectedPath = BlueprintPath;
        Result.Message      = FString::Printf(TEXT("Added component '%s' (%s) to '%s'"),
            *ComponentName, *ComponentClass, *BlueprintPath);
    }

    // -----------------------------------------------------------------------
    // spawn_blueprint_actor
    // -----------------------------------------------------------------------
    else if (Action == TEXT("spawn_blueprint_actor"))
    {
        FString BlueprintPath;
        if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
        {
            Result.Message = TEXT("spawn_blueprint_actor: 'blueprint_path' is required");
            return Result;
        }

        UBlueprint* BP = LoadBlueprint(BlueprintPath);
        if (!BP || !BP->GeneratedClass)
        {
            Result.Message = FString::Printf(
                TEXT("spawn_blueprint_actor: could not load Blueprint '%s'"), *BlueprintPath);
            return Result;
        }

        UClass* ActorClass = BP->GeneratedClass;
        if (!ActorClass->IsChildOf(AActor::StaticClass()))
        {
            Result.Message = TEXT("spawn_blueprint_actor: Blueprint GeneratedClass is not an AActor");
            return Result;
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
            Result.Message = TEXT("spawn_blueprint_actor: no editor world");
            return Result;
        }

        FVector  Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        ReadVec3(Params,    TEXT("location"), Location);
        ReadRotator(Params, TEXT("rotation"), Rotation);

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        AActor* Spawned = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location), SpawnParams);
        if (!Spawned)
        {
            Result.Message = TEXT("spawn_blueprint_actor: World::SpawnActor returned null");
            return Result;
        }

        FString Label;
        if (Params->TryGetStringField(TEXT("label"), Label))
            Spawned->SetActorLabel(Label);

        Result.bSuccess    = true;
        Result.AffectedPath = Spawned->GetActorLabel();
        Result.Message      = FString::Printf(TEXT("Spawned Blueprint actor '%s' from '%s' at (%.1f, %.1f, %.1f)"),
            *Spawned->GetActorLabel(), *BlueprintPath, Location.X, Location.Y, Location.Z);
    }

    // -----------------------------------------------------------------------
    // get_actor_transform  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("get_actor_transform"))
    {
        FString ActorId;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorId))
            Params->TryGetStringField(TEXT("actor_label"), ActorId);
        if (ActorId.IsEmpty())
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' or 'actor_label' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorId);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorId),
                TEXT("Use find_actors_by_name to list available actors"));

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        auto MakeVecObj = [](const FVector& V) -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("x"), V.X);
            O->SetNumberField(TEXT("y"), V.Y);
            O->SetNumberField(TEXT("z"), V.Z);
            return O;
        };
        auto MakeRotObj = [](const FRotator& R) -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("pitch"), R.Pitch);
            O->SetNumberField(TEXT("yaw"),   R.Yaw);
            O->SetNumberField(TEXT("roll"),  R.Roll);
            return O;
        };

        Data->SetObjectField(TEXT("location"), MakeVecObj(Target->GetActorLocation()));
        Data->SetObjectField(TEXT("rotation"), MakeRotObj(Target->GetActorRotation()));
        Data->SetObjectField(TEXT("scale"),    MakeVecObj(Target->GetActorScale3D()));
        Data->SetStringField(TEXT("label"),    Target->GetActorLabel());

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Transform for '%s'"), *Target->GetActorLabel()), Data);
    }

    // -----------------------------------------------------------------------
    // get_actor_properties  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("get_actor_properties"))
    {
        FString ActorPath;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

        const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
        if (!Params->TryGetArrayField(TEXT("properties"), PropsArray) || PropsArray->Num() == 0)
            return MakeError(DOMAIN, Action, 1000, TEXT("'properties' array is required and must not be empty"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorPath),
                TEXT("Use find_actors_by_name to list available actors"));

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        UClass* ActorClass = Target->GetClass();
        int32 Found = 0;

        for (const TSharedPtr<FJsonValue>& Val : *PropsArray)
        {
            FString PropName = Val->AsString();
            if (PropName.IsEmpty()) continue;

            FProperty* Prop = ActorClass->FindPropertyByName(*PropName);
            if (!Prop)
            {
                Data->SetStringField(PropName, TEXT("<not found>"));
                continue;
            }

            FString ValueStr;
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);
            Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, Target, PPF_None);
            Data->SetStringField(PropName, ValueStr);
            ++Found;
        }

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Exported %d properties from '%s'"), Found, *Target->GetActorLabel()), Data);
    }

    // -----------------------------------------------------------------------
    // set_actor_property  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_actor_property"))
    {
        FString ActorPath, PropName, ValueStr;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));
        if (!Params->TryGetStringField(TEXT("property"), PropName))
            return MakeError(DOMAIN, Action, 1000, TEXT("'property' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorPath),
                TEXT("Use find_actors_by_name to list available actors"));

        FProperty* Prop = Target->GetClass()->FindPropertyByName(*PropName);
        if (!Prop)
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Target->GetClass()->GetName()),
                TEXT("Check spelling or use get_actor_properties to inspect available properties"));

        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);

        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("AI Bridge: Set %s.%s"), *Target->GetActorLabel(), *PropName)));
        Target->Modify();

        // Type dispatch
        if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
        {
            double NumVal = 0;
            if (!Params->TryGetNumberField(TEXT("value"), NumVal))
                return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a number for this property"));

            if (NumProp->IsInteger())
                NumProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumVal));
            else
                NumProp->SetFloatingPointPropertyValue(ValuePtr, NumVal);
        }
        else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
        {
            bool BoolVal = false;
            if (!Params->TryGetBoolField(TEXT("value"), BoolVal))
                return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a boolean for this property"));
            BoolProp->SetPropertyValue(ValuePtr, BoolVal);
        }
        else if (CastField<FStrProperty>(Prop) || CastField<FNameProperty>(Prop) || CastField<FTextProperty>(Prop))
        {
            if (!Params->TryGetStringField(TEXT("value"), ValueStr))
                return MakeError(DOMAIN, Action, 1001, TEXT("'value' must be a string for this property"));

            if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
                StrProp->SetPropertyValue(ValuePtr, ValueStr);
            else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
                NameProp->SetPropertyValue(ValuePtr, FName(*ValueStr));
            else if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
                TextProp->SetPropertyValue(ValuePtr, FText::FromString(ValueStr));
        }
        else
        {
            // Generic fallback: try ImportText
            if (!Params->TryGetStringField(TEXT("value"), ValueStr))
                return MakeError(DOMAIN, Action, 1001,
                    TEXT("'value' (as string) is required for this property type — will use ImportText"));
            Prop->ImportText_Direct(*ValueStr, ValuePtr, Target, PPF_None);
        }

        Target->PostEditChange();

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Set %s.%s"), *Target->GetActorLabel(), *PropName));
    }

    // -----------------------------------------------------------------------
    // attach_to  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("attach_to"))
    {
        FString ChildPath, ParentPath;
        if (!Params->TryGetStringField(TEXT("child_path"), ChildPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'child_path' is required"));
        if (!Params->TryGetStringField(TEXT("parent_path"), ParentPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'parent_path' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Child  = FindActor(World, ChildPath);
        AActor* Parent = FindActor(World, ParentPath);
        if (!Child)
            return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Child actor '%s' not found"), *ChildPath));
        if (!Parent)
            return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Parent actor '%s' not found"), *ParentPath));

        FString SocketName;
        Params->TryGetStringField(TEXT("socket"), SocketName);

        FString RuleStr;
        Params->TryGetStringField(TEXT("rule"), RuleStr);

        FAttachmentTransformRules Rules = FAttachmentTransformRules::KeepWorldTransform;
        if (RuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
            Rules = FAttachmentTransformRules::KeepRelativeTransform;
        else if (RuleStr.Equals(TEXT("SnapToTarget"), ESearchCase::IgnoreCase))
            Rules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;

        Child->AttachToActor(Parent, Rules, SocketName.IsEmpty() ? NAME_None : FName(*SocketName));

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Attached '%s' to '%s'%s"),
                *Child->GetActorLabel(), *Parent->GetActorLabel(),
                SocketName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (socket: %s)"), *SocketName)));
    }

    // -----------------------------------------------------------------------
    // detach  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("detach"))
    {
        FString ActorPath;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorPath));

        FString RuleStr;
        Params->TryGetStringField(TEXT("rule"), RuleStr);

        FDetachmentTransformRules Rules = FDetachmentTransformRules::KeepWorldTransform;
        if (RuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
            Rules = FDetachmentTransformRules::KeepRelativeTransform;

        Target->DetachFromActor(Rules);

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Detached '%s'"), *Target->GetActorLabel()));
    }

    // -----------------------------------------------------------------------
    // set_mobility  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_mobility"))
    {
        FString ActorPath, MobilityStr;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));
        if (!Params->TryGetStringField(TEXT("mobility"), MobilityStr))
            return MakeError(DOMAIN, Action, 1000, TEXT("'mobility' is required (Static|Stationary|Movable)"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Actor '%s' not found"), *ActorPath));

        USceneComponent* Root = Target->GetRootComponent();
        if (!Root)
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("Actor '%s' has no root component"), *ActorPath));

        EComponentMobility::Type Mobility;
        if (MobilityStr.Equals(TEXT("Static"), ESearchCase::IgnoreCase))
            Mobility = EComponentMobility::Static;
        else if (MobilityStr.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))
            Mobility = EComponentMobility::Stationary;
        else if (MobilityStr.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))
            Mobility = EComponentMobility::Movable;
        else
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("Invalid mobility '%s'. Use: Static, Stationary, or Movable"), *MobilityStr));

        Root->SetMobility(Mobility);

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Set mobility of '%s' to %s"), *Target->GetActorLabel(), *MobilityStr));
    }

    // -----------------------------------------------------------------------
    // set_tag  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_tag"))
    {
        FString ActorPath, Tag;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));
        if (!Params->TryGetStringField(TEXT("tag"), Tag))
            return MakeError(DOMAIN, Action, 1000, TEXT("'tag' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000, FString::Printf(TEXT("Actor '%s' not found"), *ActorPath));

        bool bAdd = true;
        Params->TryGetBoolField(TEXT("add"), bAdd);

        FName TagName(*Tag);
        if (bAdd)
            Target->Tags.AddUnique(TagName);
        else
            Target->Tags.Remove(TagName);

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("%s tag '%s' on '%s'"),
                bAdd ? TEXT("Added") : TEXT("Removed"), *Tag, *Target->GetActorLabel()));
    }

    // -----------------------------------------------------------------------
    // get_selected_actors  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("get_selected_actors"))
    {
#if WITH_EDITOR
        if (!GEditor)
            return MakeError(DOMAIN, Action, 3000, TEXT("GEditor not available"));

        USelection* Selection = GEditor->GetSelectedActors();
        if (!Selection)
            return MakeError(DOMAIN, Action, 3000, TEXT("No selection object available"));

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> ActorArr;

        for (FSelectionIterator It(*Selection); It; ++It)
        {
            AActor* Actor = Cast<AActor>(*It);
            if (!Actor) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"),  Actor->GetName());
            Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
            Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
            Entry->SetStringField(TEXT("path"),  Actor->GetPathName());
            ActorArr.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("actors"), ActorArr);
        Data->SetNumberField(TEXT("count"), ActorArr.Num());

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("%d actor(s) selected"), ActorArr.Num()), Data);
#else
        return MakeError(DOMAIN, Action, 3000, TEXT("get_selected_actors requires editor build"));
#endif
    }

    // -----------------------------------------------------------------------
    // select_actors  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("select_actors"))
    {
#if WITH_EDITOR
        if (!GEditor)
            return MakeError(DOMAIN, Action, 3000, TEXT("GEditor not available"));

        const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
        if (!Params->TryGetArrayField(TEXT("actor_paths"), PathsArray) || PathsArray->Num() == 0)
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_paths' array is required and must not be empty"));

        bool bDeselectOthers = true;
        Params->TryGetBoolField(TEXT("deselect_others"), bDeselectOthers);

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        if (bDeselectOthers)
            GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);

        int32 Selected = 0;
        for (const TSharedPtr<FJsonValue>& Val : *PathsArray)
        {
            FString Path = Val->AsString();
            if (Path.IsEmpty()) continue;

            AActor* Actor = FindActor(World, Path);
            if (Actor)
            {
                GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/false);
                ++Selected;
            }
        }

        GEditor->NoteSelectionChange();

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Selected %d actor(s)"), Selected));
#else
        return MakeError(DOMAIN, Action, 3000, TEXT("select_actors requires editor build"));
#endif
    }

    // -----------------------------------------------------------------------
    // focus_actor  (Phase 3)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("focus_actor"))
    {
#if WITH_EDITOR
        if (!GEditor)
            return MakeError(DOMAIN, Action, 3000, TEXT("GEditor not available"));

        FString ActorPath;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorPath);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorPath),
                TEXT("Use find_actors_by_name to list available actors"));

        TArray<AActor*> Actors;
        Actors.Add(Target);
        GEditor->MoveViewportCamerasToActor(Actors, /*bActiveViewportOnly=*/false);

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Focused viewport on '%s'"), *Target->GetActorLabel()));
#else
        return MakeError(DOMAIN, Action, 3000, TEXT("focus_actor requires editor build"));
#endif
    }

    // -----------------------------------------------------------------------
    // set_lod_settings  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_lod_settings"))
    {
        FString ActorLabel;
        if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_label' is required"));

        double LodIndexD = 0, ScreenSizeD = 0;
        if (!Params->TryGetNumberField(TEXT("lod_index"), LodIndexD))
            return MakeError(DOMAIN, Action, 1000, TEXT("'lod_index' is required"));
        if (!Params->TryGetNumberField(TEXT("screen_size"), ScreenSizeD))
            return MakeError(DOMAIN, Action, 1000, TEXT("'screen_size' is required"));

        int32 LodIndex    = static_cast<int32>(LodIndexD);
        float ScreenSize  = static_cast<float>(ScreenSizeD);

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorLabel);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorLabel));

        UStaticMeshComponent* SMComp = Target->FindComponentByClass<UStaticMeshComponent>();
        if (!SMComp)
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("Actor '%s' has no StaticMeshComponent"), *ActorLabel));

        UStaticMesh* SM = SMComp->GetStaticMesh();
        if (!SM)
            return MakeError(DOMAIN, Action, 2000, TEXT("StaticMeshComponent has no StaticMesh assigned"));

        if (LodIndex < 0 || LodIndex >= SM->GetNumSourceModels())
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("lod_index %d out of range (mesh has %d LODs)"),
                    LodIndex, SM->GetNumSourceModels()));

        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("AI Bridge: Set LOD %d ScreenSize on '%s'"), LodIndex, *ActorLabel)));
        SM->Modify();

        FStaticMeshSourceModel& SourceModel = SM->GetSourceModel(LodIndex);
        SourceModel.ScreenSize.Default = ScreenSize;
        SM->PostEditChange();

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("LOD %d screen size set to %.4f on '%s'"),
                LodIndex, ScreenSize, *ActorLabel));
    }

    // -----------------------------------------------------------------------
    // find_actors_of_class  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("find_actors_of_class"))
    {
        FString ClassName;
        if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
            return MakeError(DOMAIN, Action, 1000, TEXT("'class_name' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        UClass* TargetClass = ResolveClass(ClassName);
        if (!TargetClass)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Could not resolve class '%s'"), *ClassName));

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> ActorArr;

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor->IsA(TargetClass)) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
            Entry->SetStringField(TEXT("path"),  Actor->GetPathName());

            TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
            FVector Loc = Actor->GetActorLocation();
            LocObj->SetNumberField(TEXT("x"), Loc.X);
            LocObj->SetNumberField(TEXT("y"), Loc.Y);
            LocObj->SetNumberField(TEXT("z"), Loc.Z);
            Entry->SetObjectField(TEXT("location"), LocObj);

            ActorArr.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("actors"), ActorArr);
        Data->SetNumberField(TEXT("count"), ActorArr.Num());

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Found %d actor(s) of class '%s'"), ActorArr.Num(), *ClassName), Data);
    }

    // -----------------------------------------------------------------------
    // set_actor_label  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_actor_label"))
    {
        FString CurrentLabel, NewLabel;
        if (!Params->TryGetStringField(TEXT("actor_label"), CurrentLabel))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_label' (current label) is required"));
        if (!Params->TryGetStringField(TEXT("new_label"), NewLabel))
            return MakeError(DOMAIN, Action, 1000, TEXT("'new_label' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, CurrentLabel);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *CurrentLabel));

        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("AI Bridge: Rename '%s' to '%s'"), *CurrentLabel, *NewLabel)));
        Target->Modify();
        Target->SetActorLabel(NewLabel);

        FBridgeResult R = MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Renamed '%s' to '%s'"), *CurrentLabel, *NewLabel));
        R.AffectedPath = NewLabel;
        return R;
    }

    // -----------------------------------------------------------------------
    // duplicate  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("duplicate"))
    {
#if WITH_EDITOR
        FString ActorLabel;
        if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_label' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorLabel);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorLabel));

        // Parse optional offset ("X,Y,Z"), default (100,0,0)
        FVector Offset(100.f, 0.f, 0.f);
        FString OffsetStr;
        if (Params->TryGetStringField(TEXT("offset"), OffsetStr))
        {
            TArray<FString> Parts;
            OffsetStr.ParseIntoArray(Parts, TEXT(","));
            if (Parts.Num() == 3)
                Offset = FVector(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
        }

        UEditorActorSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
        if (!EAS)
            return MakeError(DOMAIN, Action, 3000, TEXT("EditorActorSubsystem not available"));

        // Select only this actor, then duplicate selection
        GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
        GEditor->SelectActor(Target, /*bInSelected=*/true, /*bNotify=*/false);
        GEditor->NoteSelectionChange();

        // UE 5.7: UEditorActorSubsystem::DuplicateSelectedActors returns void — capture
        // the new actors by snapshotting the selection before/after the call.
        TSet<AActor*> BeforeSelection;
        for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
            if (AActor* A = Cast<AActor>(*It)) BeforeSelection.Add(A);

        EAS->DuplicateSelectedActors(World);

        TArray<AActor*> Duplicated;
        for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
            if (AActor* A = Cast<AActor>(*It))
                if (!BeforeSelection.Contains(A))
                    Duplicated.Add(A);

        if (Duplicated.Num() == 0)
            return MakeError(DOMAIN, Action, 3000, TEXT("DuplicateSelectedActors returned empty — duplication failed"));

        AActor* NewActor = Duplicated[0];
        NewActor->SetActorLocation(Target->GetActorLocation() + Offset);
        NewActor->MarkPackageDirty();

        FBridgeResult R = MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Duplicated '%s' as '%s' at offset (%.1f, %.1f, %.1f)"),
                *ActorLabel, *NewActor->GetActorLabel(), Offset.X, Offset.Y, Offset.Z));
        R.AffectedPath = NewActor->GetActorLabel();
        return R;
#else
        return MakeError(DOMAIN, Action, 3000, TEXT("duplicate requires editor build"));
#endif
    }

    // -----------------------------------------------------------------------
    // replace_class  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("replace_class"))
    {
        // FReplaceActorHelper was removed in UE 5.4.
        // UE 5.7 has no drop-in public replacement. Returning a descriptive not-feasible error.
        return MakeError(DOMAIN, Action, 3003,
            TEXT("replace_class is not feasible in UE 5.7: FReplaceActorHelper was removed in UE 5.4 and "
                 "no equivalent public API exists. Use the editor UI: right-click actor > Replace Selected Actors With, "
                 "or manually spawn the new class and transfer properties via get_actor_properties / set_actor_property."),
            TEXT("Use editor UI or manual property transfer workflow"));
    }

    // -----------------------------------------------------------------------
    // set_replication_settings  (Phase 1d)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("set_replication_settings"))
    {
        FString ActorLabel;
        if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_label' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorLabel);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorLabel));

        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("AI Bridge: Set replication settings on '%s'"), *ActorLabel)));
        Target->Modify();

        bool bReplicates = false;
        if (Params->TryGetBoolField(TEXT("replicates"), bReplicates))
            Target->SetReplicates(bReplicates);

        double NetUpdateFreq = 0.0;
        if (Params->TryGetNumberField(TEXT("net_update_frequency"), NetUpdateFreq))
            Target->SetNetUpdateFrequency(static_cast<float>(NetUpdateFreq));

        bool bAlwaysRelevant = false;
        if (Params->TryGetBoolField(TEXT("always_relevant"), bAlwaysRelevant))
            Target->bAlwaysRelevant = bAlwaysRelevant;

        double NetPriority = 0.0;
        if (Params->TryGetNumberField(TEXT("net_priority"), NetPriority))
            Target->NetPriority = static_cast<float>(NetPriority);

        Target->MarkPackageDirty();

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("replicates"),          Target->GetIsReplicated());
        Data->SetNumberField(TEXT("net_update_frequency"), Target->GetNetUpdateFrequency());
        Data->SetBoolField(TEXT("always_relevant"),     Target->bAlwaysRelevant);
        Data->SetNumberField(TEXT("net_priority"),      Target->NetPriority);

        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Replication settings updated on '%s'"), *ActorLabel), Data);
    }

    // -----------------------------------------------------------------------
    // spawn_with_retry  (Phase 4)
    // -----------------------------------------------------------------------
    else if (Action == TEXT("spawn_with_retry"))
    {
        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
            return MakeError(DOMAIN, Action, 1000, TEXT("'asset_path' is required"));

        FVector BaseLocation = FVector::ZeroVector;
        if (!ReadVec3(Params, TEXT("location"), BaseLocation))
            return MakeError(DOMAIN, Action, 1000, TEXT("'location' {x,y,z} is required"));

        FRotator Rotation = FRotator::ZeroRotator;
        ReadRotator(Params, TEXT("rotation"), Rotation);

        // Optional parameters
        double RetryOffsetRange = 200.0;
        Params->TryGetNumberField(TEXT("retry_offset_range"), RetryOffsetRange);

        double MaxRetriesD = 5;
        Params->TryGetNumberField(TEXT("max_retries"), MaxRetriesD);
        const int32 MaxRetries = FMath::Max(0, static_cast<int32>(MaxRetriesD));

        FVector BoxExtent(50.0, 50.0, 50.0);
        ReadVec3(Params, TEXT("collision_box_extent"), BoxExtent);
        // Guard against zero/negative extents
        BoxExtent.X = FMath::Max(1.0, BoxExtent.X);
        BoxExtent.Y = FMath::Max(1.0, BoxExtent.Y);
        BoxExtent.Z = FMath::Max(1.0, BoxExtent.Z);

        // Resolve class (supports native class names and /Game Blueprint asset paths)
        UClass* ActorClass = ResolveClass(AssetPath);
        if (!ActorClass)
        {
            // Try Blueprint asset path → GeneratedClass path
            if (UBlueprint* BP = LoadBlueprint(AssetPath))
                ActorClass = BP->GeneratedClass;
        }
        if (!ActorClass)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("spawn_with_retry: could not resolve class or blueprint '%s'"), *AssetPath),
                TEXT("Pass a class name, /Script/Engine.<Class>, or a /Game/... Blueprint asset path"));
        if (!ActorClass->IsChildOf(AActor::StaticClass()))
            return MakeError(DOMAIN, Action, 1001,
                FString::Printf(TEXT("spawn_with_retry: '%s' is not an AActor subclass"), *AssetPath));

        UWorld* World = GetEditorWorld();
        if (!World)
            return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        // Attempt loop: first iteration uses provided location, subsequent iterations jitter XY.
        const int32 TotalAttempts = MaxRetries + 1;
        FVector CandidateLocation = BaseLocation;
        bool bClear = false;
        int32 AttemptsUsed = 0;

        // FCollisionShape::MakeBox takes half-extents.
        FCollisionShape Box = FCollisionShape::MakeBox(BoxExtent);
        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ForgeAI_SpawnWithRetry), /*bTraceComplex=*/false);

        // Use multi-channel overlap by testing each channel; stop early on block.
        auto ProbeBlocked = [&](const FVector& TestLoc) -> bool
        {
            const FQuat QuatRot = Rotation.Quaternion();
            if (World->OverlapBlockingTestByChannel(TestLoc, QuatRot, ECC_WorldStatic, Box, QueryParams))
                return true;
            if (World->OverlapBlockingTestByChannel(TestLoc, QuatRot, ECC_WorldDynamic, Box, QueryParams))
                return true;
            return false;
        };

        for (int32 i = 0; i < TotalAttempts; ++i)
        {
            AttemptsUsed = i + 1;
            if (i == 0)
            {
                CandidateLocation = BaseLocation;
            }
            else
            {
                const float Range = static_cast<float>(FMath::Abs(RetryOffsetRange));
                const float Jx = FMath::RandRange(-Range, Range);
                const float Jy = FMath::RandRange(-Range, Range);
                CandidateLocation = BaseLocation + FVector(Jx, Jy, 0.0f);
            }

            if (!ProbeBlocked(CandidateLocation))
            {
                bClear = true;
                break;
            }
        }

        if (!bClear)
        {
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("spawn_with_retry: all %d attempts blocked by overlap"), TotalAttempts),
                TEXT("Increase retry_offset_range, shrink collision_box_extent, or clear the area"));
        }

        // Spawn at the cleared location (same path as Action_SpawnActor)
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        AActor* Spawned = World->SpawnActor<AActor>(ActorClass,
            FTransform(Rotation, CandidateLocation), SpawnParams);
        if (!Spawned)
            return MakeError(DOMAIN, Action, 3000,
                TEXT("spawn_with_retry: World::SpawnActor returned null"));

        FString Label;
        if (Params->TryGetStringField(TEXT("label"), Label))
            Spawned->SetActorLabel(Label);

        // Build success payload
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("spawned_actor_label"), Spawned->GetActorLabel());
        Data->SetNumberField(TEXT("attempts_used"), AttemptsUsed);

        TSharedPtr<FJsonObject> XformObj = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
        LocObj->SetNumberField(TEXT("x"), CandidateLocation.X);
        LocObj->SetNumberField(TEXT("y"), CandidateLocation.Y);
        LocObj->SetNumberField(TEXT("z"), CandidateLocation.Z);
        TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
        RotObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
        RotObj->SetNumberField(TEXT("yaw"),   Rotation.Yaw);
        RotObj->SetNumberField(TEXT("roll"),  Rotation.Roll);
        XformObj->SetObjectField(TEXT("location"), LocObj);
        XformObj->SetObjectField(TEXT("rotation"), RotObj);
        Data->SetObjectField(TEXT("final_transform"), XformObj);

        FBridgeResult R = MakeSuccess(DOMAIN, Action,
            FString::Printf(
                TEXT("Spawned '%s' (%s) at (%.1f, %.1f, %.1f) after %d attempt(s)"),
                *Spawned->GetActorLabel(), *AssetPath,
                CandidateLocation.X, CandidateLocation.Y, CandidateLocation.Z,
                AttemptsUsed),
            Data);
        R.AffectedPath = Spawned->GetActorLabel();
        return R;
    }

    // -----------------------------------------------------------------------
    // destroy_actor — single actor by label or path
    // -----------------------------------------------------------------------
    else if (Action == TEXT("destroy_actor"))
    {
        auto Tx = BeginTransaction(TEXT("Destroy Actor"));
        FString ActorId;
        if (!Params->TryGetStringField(TEXT("actor_path"), ActorId))
            Params->TryGetStringField(TEXT("actor_label"), ActorId);
        if (ActorId.IsEmpty())
            return MakeError(DOMAIN, Action, 1000, TEXT("'actor_path' or 'actor_label' is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        AActor* Target = FindActor(World, ActorId);
        if (!Target)
            return MakeError(DOMAIN, Action, 2000,
                FString::Printf(TEXT("Actor '%s' not found"), *ActorId),
                TEXT("Use find_actors_by_name to list available actors"));

        const FString DestroyedLabel = Target->GetActorLabel();
        const bool bDestroyed = World->DestroyActor(Target);
        if (!bDestroyed)
            return MakeError(DOMAIN, Action, 3000,
                FString::Printf(TEXT("World::DestroyActor returned false for '%s'"), *DestroyedLabel),
                TEXT("Actor may be a level-bound default actor or PIE-only"));

        FBridgeResult R = MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Destroyed actor '%s'"), *DestroyedLabel));
        R.AffectedPath = DestroyedLabel;
        return R;
    }

    // -----------------------------------------------------------------------
    // destroy_actors — bulk
    // -----------------------------------------------------------------------
    else if (Action == TEXT("destroy_actors"))
    {
        auto Tx = BeginTransaction(TEXT("Destroy Actors (Bulk)"));
        const TArray<TSharedPtr<FJsonValue>>* IdsArr = nullptr;
        if (!Params->TryGetArrayField(TEXT("actor_paths"), IdsArr))
            Params->TryGetArrayField(TEXT("actor_labels"), IdsArr);
        if (!IdsArr)
            return MakeError(DOMAIN, Action, 1000,
                TEXT("'actor_paths' or 'actor_labels' (array) is required"));

        UWorld* World = GetEditorWorld();
        if (!World) return MakeError(DOMAIN, Action, 3000, TEXT("No editor world available"));

        int32 Destroyed = 0, NotFound = 0, Failed = 0;
        TArray<TSharedPtr<FJsonValue>> Failures;
        for (const TSharedPtr<FJsonValue>& V : *IdsArr)
        {
            const FString Id = V->AsString();
            AActor* A = FindActor(World, Id);
            if (!A) { ++NotFound; Failures.Add(MakeShared<FJsonValueString>(Id + TEXT(": not found"))); continue; }
            if (World->DestroyActor(A)) ++Destroyed;
            else { ++Failed; Failures.Add(MakeShared<FJsonValueString>(Id + TEXT(": destroy returned false"))); }
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetNumberField(TEXT("destroyed"), Destroyed);
        Data->SetNumberField(TEXT("not_found"), NotFound);
        Data->SetNumberField(TEXT("failed"), Failed);
        Data->SetArrayField(TEXT("failures"), Failures);
        return MakeSuccess(DOMAIN, Action,
            FString::Printf(TEXT("Destroyed %d/%d actor(s) (not_found=%d failed=%d)"),
                Destroyed, IdsArr->Num(), NotFound, Failed),
            Data);
    }

    // -----------------------------------------------------------------------
    // Unknown
    // -----------------------------------------------------------------------
    else
    {
        return MakeError(DOMAIN, Action, 1001,
            FString::Printf(TEXT("Unknown action '%s'"), *Action),
            TEXT("Use system/capabilities to list supported actions"));
    }

    return Result;
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UActorHandler::GetActionSchemas() const
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

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn an actor by class name in the editor world. Optionally assigns a UStaticMesh on the spawned actor's StaticMeshComponent."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("class_name"), P(TEXT("string"), true, TEXT("Actor class name or Blueprint path"))); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), false, TEXT("Spawn location"))); Pr->SetObjectField(TEXT("rotation"), P(TEXT("object {pitch,yaw,roll}"), false, TEXT("Spawn rotation"))); Pr->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Actor label in the World Outliner"))); Pr->SetObjectField(TEXT("static_mesh"), P(TEXT("string"), false, TEXT("Optional UStaticMesh asset path. Applied to AStaticMeshActor's mesh component, or the first UStaticMeshComponent found. If no component exists, a warning is returned."))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("spawn_actor"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set location, rotation, and/or scale on an actor. Supports rotation_from_normal to align +Z to a world-space normal."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label to find"))); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), false, TEXT("New location"))); Pr->SetObjectField(TEXT("rotation"), P(TEXT("object {pitch,yaw,roll}"), false, TEXT("New rotation"))); Pr->SetObjectField(TEXT("scale"), P(TEXT("object {x,y,z}"), false, TEXT("New scale"))); Pr->SetObjectField(TEXT("rotation_from_normal"), P(TEXT("object {x,y,z}"), false, TEXT("World-space normal vector. Aligns actor +Z to this normal via FRotationMatrix::MakeFromZ. Wins over 'rotation' unless preserve_yaw is true."))); Pr->SetObjectField(TEXT("rotation_from_normal_preserve_yaw"), P(TEXT("bool"), false, TEXT("If true, preserve yaw from 'rotation' input while aligning Z to the normal. Default false."))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_actor_transform"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Find actors by label substring match"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("name_pattern"), P(TEXT("string"), false, TEXT("Substring to match (empty = all actors)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("find_actors_by_name"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return all actors in the current level"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_actors_in_level"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn a Blueprint actor in the editor world"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("location"), P(TEXT("object {x,y,z}"), false, TEXT("Spawn location"))); Pr->SetObjectField(TEXT("rotation"), P(TEXT("object {pitch,yaw,roll}"), false, TEXT("Spawn rotation"))); Pr->SetObjectField(TEXT("label"), P(TEXT("string"), false, TEXT("Actor label"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("spawn_blueprint_actor"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get the transform (location, rotation, scale) of an actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), false, TEXT("Actor path or label (alias: actor_label)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_actor_transform"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Read reflected property values from an actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label"))); Pr->SetObjectField(TEXT("properties"), P(TEXT("array<string>"), true, TEXT("Property names to read"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_actor_properties"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set a reflected property on an actor (transacted)"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label"))); Pr->SetObjectField(TEXT("property"), P(TEXT("string"), true, TEXT("Property name"))); Pr->SetObjectField(TEXT("value"), P(TEXT("any"), true, TEXT("Value to set (type matches property)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_actor_property"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Attach a child actor to a parent actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("child_path"), P(TEXT("string"), true, TEXT("Child actor path or label"))); Pr->SetObjectField(TEXT("parent_path"), P(TEXT("string"), true, TEXT("Parent actor path or label"))); Pr->SetObjectField(TEXT("socket"), P(TEXT("string"), false, TEXT("Socket name to attach to"))); Pr->SetObjectField(TEXT("rule"), P(TEXT("string"), false, TEXT("KeepWorld, KeepRelative, or SnapToTarget (default KeepWorld)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("attach_to"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Detach an actor from its parent"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label"))); Pr->SetObjectField(TEXT("rule"), P(TEXT("string"), false, TEXT("KeepWorld or KeepRelative (default KeepWorld)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("detach"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the mobility of an actor's root component"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label"))); Pr->SetObjectField(TEXT("mobility"), P(TEXT("string"), true, TEXT("Static, Stationary, or Movable"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_mobility"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add or remove a tag on an actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label"))); Pr->SetObjectField(TEXT("tag"), P(TEXT("string"), true, TEXT("Tag name"))); Pr->SetObjectField(TEXT("add"), P(TEXT("bool"), false, TEXT("Add tag (true) or remove (false), default true"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_tag"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Return the currently selected actors in the editor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("get_selected_actors"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Select actors in the editor by path or label"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_paths"), P(TEXT("array<string>"), true, TEXT("Array of actor paths or labels to select"))); Pr->SetObjectField(TEXT("deselect_others"), P(TEXT("bool"), false, TEXT("Deselect all other actors first (default true)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("select_actors"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Move viewport cameras to focus on an actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_path"), P(TEXT("string"), true, TEXT("Actor path or label to focus on"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("focus_actor"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a component to a Blueprint's SimpleConstructionScript"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("blueprint_path"), P(TEXT("string"), true, TEXT("Blueprint asset path"))); Pr->SetObjectField(TEXT("component_class"), P(TEXT("string"), true, TEXT("Component class name"))); Pr->SetObjectField(TEXT("component_name"), P(TEXT("string"), false, TEXT("Component variable name (defaults to class name)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("add_component_to_blueprint"), A); }

    // Phase 1d schemas
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set the LOD screen size threshold on a StaticMesh actor"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("lod_index"), P(TEXT("int"), true, TEXT("LOD index (0 = highest detail)"))); Pr->SetObjectField(TEXT("screen_size"), P(TEXT("float"), true, TEXT("Screen size threshold (0.0-1.0, lower = further)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_lod_settings"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Find all actors of a given class (or subclass) in the level"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("class_name"), P(TEXT("string"), true, TEXT("Class name (e.g. PointLight) or full script path (/Script/Engine.PointLight)"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("find_actors_of_class"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Rename an actor in the World Outliner"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Current actor label"))); Pr->SetObjectField(TEXT("new_label"), P(TEXT("string"), true, TEXT("New label to assign"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_actor_label"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Duplicate an actor and offset the copy"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor to duplicate"))); Pr->SetObjectField(TEXT("offset"), P(TEXT("string"), false, TEXT("Offset as \"X,Y,Z\" (default \"100,0,0\")"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("duplicate"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Replace actor class — NOT FEASIBLE in UE 5.7 (FReplaceActorHelper removed in 5.4). Always returns error_code 3003."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor to replace"))); Pr->SetObjectField(TEXT("new_class"), P(TEXT("string"), true, TEXT("Target class path"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("replace_class"), A); }

    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Set replication settings on an actor instance"));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>(); Pr->SetObjectField(TEXT("actor_label"), P(TEXT("string"), true, TEXT("Actor label"))); Pr->SetObjectField(TEXT("replicates"), P(TEXT("bool"), false, TEXT("Enable/disable replication"))); Pr->SetObjectField(TEXT("net_update_frequency"), P(TEXT("float"), false, TEXT("Net update frequency (Hz)"))); Pr->SetObjectField(TEXT("always_relevant"), P(TEXT("bool"), false, TEXT("bAlwaysRelevant flag"))); Pr->SetObjectField(TEXT("net_priority"), P(TEXT("float"), false, TEXT("Net priority weight"))); A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("set_replication_settings"), A); }

    // Phase 4 schema
    { TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Spawn an actor with overlap-avoidance retry. Probes an OverlapBlockingTest against WorldStatic+WorldDynamic at the candidate location; if blocked, jitters XY within +/- retry_offset_range and retries up to max_retries times. Returns error 3000 if all attempts blocked."));
      TSharedPtr<FJsonObject> Pr = MakeShared<FJsonObject>();
      Pr->SetObjectField(TEXT("asset_path"),           P(TEXT("string"),                 true,  TEXT("UClass path, native class name, or /Game/... Blueprint asset path")));
      Pr->SetObjectField(TEXT("location"),             P(TEXT("object {x,y,z}"),         true,  TEXT("Base spawn location; first attempt uses this verbatim")));
      Pr->SetObjectField(TEXT("rotation"),             P(TEXT("object {pitch,yaw,roll}"),false, TEXT("Spawn rotation (default 0,0,0)")));
      Pr->SetObjectField(TEXT("retry_offset_range"),   P(TEXT("float"),                  false, TEXT("Max +/- XY jitter per retry (UU). Default 200.0")));
      Pr->SetObjectField(TEXT("max_retries"),          P(TEXT("int"),                    false, TEXT("Retry count after the first attempt. Default 5")));
      Pr->SetObjectField(TEXT("collision_box_extent"), P(TEXT("object {x,y,z}"),         false, TEXT("Half-extents for the overlap test (default 50,50,50)")));
      Pr->SetObjectField(TEXT("label"),                P(TEXT("string"),                 false, TEXT("Actor label in the World Outliner")));
      A->SetObjectField(TEXT("params"), Pr); Root->SetObjectField(TEXT("spawn_with_retry"), A); }

    return Root;
}
