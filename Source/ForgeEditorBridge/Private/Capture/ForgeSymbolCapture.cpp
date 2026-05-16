#include "Capture/ForgeSymbolCapture.h"
#include "IO/ForgeContextWriter.h"

// --- UObject reflection ---
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Interface.h"

// --- Class role checks ---
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

// --- JSON + IO ---
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Target package prefixes
//
// By default, scan the host project's own /Script/<ProjectName> package. This
// auto-detects via FApp::GetProjectName() so the symbol index works out of the
// box without configuration. If a project ships its gameplay code in multiple
// script modules (e.g. "MyGame" plus "MyGameCore"), extend GetTargetPrefixes()
// to add them; the symbol exporter only walks UClasses in matching packages.
// ---------------------------------------------------------------------------

namespace
{
    TArray<FString> GetTargetPrefixes()
    {
        TArray<FString> Prefixes;
        const FString ProjectName = FApp::GetProjectName();
        if (!ProjectName.IsEmpty())
        {
            Prefixes.Add(FString::Printf(TEXT("/Script/%s"), *ProjectName));
        }
        return Prefixes;
    }

    bool IsTargetPackage(const UClass* Class)
    {
        if (!Class) return false;
        const FString PackageName = Class->GetOutermost()->GetName();
        static const TArray<FString> TargetPackagePrefixes = GetTargetPrefixes();
        for (const FString& Prefix : TargetPackagePrefixes)
        {
            if (PackageName.StartsWith(Prefix, ESearchCase::IgnoreCase))
                return true;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Human-readable type name for any FProperty
    // -----------------------------------------------------------------------
    FString GetPropertyTypeName(FProperty* Prop)
    {
        if (!Prop) return TEXT("Unknown");

        if (CastField<FIntProperty>(Prop))     return TEXT("int32");
        if (CastField<FInt64Property>(Prop))   return TEXT("int64");
        if (CastField<FUInt32Property>(Prop))  return TEXT("uint32");
        if (CastField<FUInt64Property>(Prop))  return TEXT("uint64");
        if (CastField<FInt16Property>(Prop))   return TEXT("int16");
        if (CastField<FInt8Property>(Prop))    return TEXT("int8");
        if (CastField<FFloatProperty>(Prop))   return TEXT("float");
        if (CastField<FDoubleProperty>(Prop))  return TEXT("double");
        if (CastField<FBoolProperty>(Prop))    return TEXT("bool");
        if (CastField<FStrProperty>(Prop))     return TEXT("FString");
        if (CastField<FNameProperty>(Prop))    return TEXT("FName");
        if (CastField<FTextProperty>(Prop))    return TEXT("FText");

        if (const FByteProperty* BP = CastField<FByteProperty>(Prop))
            return BP->Enum
                ? FString::Printf(TEXT("enum:%s"), *BP->Enum->GetName())
                : TEXT("uint8");

        if (const FEnumProperty* EP = CastField<FEnumProperty>(Prop))
            return FString::Printf(TEXT("enum:%s"), *EP->GetEnum()->GetName());

        if (const FObjectProperty* OP = CastField<FObjectProperty>(Prop))
            return OP->PropertyClass
                ? FString::Printf(TEXT("%s*"), *OP->PropertyClass->GetName())
                : TEXT("UObject*");

        if (const FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop))
            return SOP->PropertyClass
                ? FString::Printf(TEXT("TSoftObjectPtr<%s>"), *SOP->PropertyClass->GetName())
                : TEXT("TSoftObjectPtr<UObject>");

        if (const FClassProperty* CP = CastField<FClassProperty>(Prop))
            return CP->MetaClass
                ? FString::Printf(TEXT("TSubclassOf<%s>"), *CP->MetaClass->GetName())
                : TEXT("TSubclassOf<UObject>");

        if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Prop))
            return SCP->MetaClass
                ? FString::Printf(TEXT("TSoftClassPtr<%s>"), *SCP->MetaClass->GetName())
                : TEXT("TSoftClassPtr<UObject>");

        if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
            return SP->Struct
                ? FString::Printf(TEXT("F%s"), *SP->Struct->GetName())
                : TEXT("FStruct");

        if (const FArrayProperty* AP = CastField<FArrayProperty>(Prop))
            return FString::Printf(TEXT("TArray<%s>"), *GetPropertyTypeName(AP->Inner));

        if (const FMapProperty* MP = CastField<FMapProperty>(Prop))
            return FString::Printf(TEXT("TMap<%s,%s>"),
                *GetPropertyTypeName(MP->KeyProp),
                *GetPropertyTypeName(MP->ValueProp));

        if (const FSetProperty* SP = CastField<FSetProperty>(Prop))
            return FString::Printf(TEXT("TSet<%s>"), *GetPropertyTypeName(SP->ElementProp));

        if (const FInterfaceProperty* IP = CastField<FInterfaceProperty>(Prop))
            return IP->InterfaceClass
                ? FString::Printf(TEXT("TScriptInterface<%s>"), *IP->InterfaceClass->GetName())
                : TEXT("TScriptInterface<IInterface>");

        if (CastField<FDelegateProperty>(Prop))
            return TEXT("FDelegate");
        if (CastField<FMulticastInlineDelegateProperty>(Prop))
            return TEXT("FMulticastDelegate");
        if (CastField<FMulticastSparseDelegateProperty>(Prop))
            return TEXT("FMulticastSparseDelegate");

        // Fallback: return the UE field class name for diagnostics
        return FString::Printf(TEXT("<%s>"), *Prop->GetClass()->GetName());
    }

    // -----------------------------------------------------------------------
    // Flag helpers
    // -----------------------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> GetPropertyFlagsJson(FProperty* Prop)
    {
        TArray<TSharedPtr<FJsonValue>> Flags;
        if (Prop->HasAnyPropertyFlags(CPF_Net))              Flags.Add(MakeShared<FJsonValueString>(TEXT("Net")));
        if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintVisible")));
        if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadOnly")));
        if (Prop->HasAnyPropertyFlags(CPF_Edit))              Flags.Add(MakeShared<FJsonValueString>(TEXT("EditAnywhere")));
        if (Prop->HasAnyPropertyFlags(CPF_Config))           Flags.Add(MakeShared<FJsonValueString>(TEXT("Config")));
        if (Prop->HasAnyPropertyFlags(CPF_SaveGame))         Flags.Add(MakeShared<FJsonValueString>(TEXT("SaveGame")));
        if (Prop->HasAnyPropertyFlags(CPF_Transient))        Flags.Add(MakeShared<FJsonValueString>(TEXT("Transient")));
        return Flags;
    }

    TArray<TSharedPtr<FJsonValue>> GetFunctionFlagsJson(UFunction* Func)
    {
        TArray<TSharedPtr<FJsonValue>> Flags;
        if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable)) Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintCallable")));
        if (Func->HasAnyFunctionFlags(FUNC_BlueprintPure))     Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintPure")));
        if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))    Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintEvent")));
        if (Func->HasAnyFunctionFlags(FUNC_Static))            Flags.Add(MakeShared<FJsonValueString>(TEXT("Static")));
        if (Func->HasAnyFunctionFlags(FUNC_Const))             Flags.Add(MakeShared<FJsonValueString>(TEXT("Const")));
        if (Func->HasAnyFunctionFlags(FUNC_NetServer))         Flags.Add(MakeShared<FJsonValueString>(TEXT("NetServer")));
        if (Func->HasAnyFunctionFlags(FUNC_NetClient))         Flags.Add(MakeShared<FJsonValueString>(TEXT("NetClient")));
        if (Func->HasAnyFunctionFlags(FUNC_NetMulticast))      Flags.Add(MakeShared<FJsonValueString>(TEXT("NetMulticast")));
        return Flags;
    }

    // -----------------------------------------------------------------------
    // Serialize a single UClass to a JSON object
    // Only direct (ExcludeSuper) properties and functions are captured;
    // this keeps output concise and avoids massive inherited duplication.
    // -----------------------------------------------------------------------
    TSharedRef<FJsonObject> SerializeClass(UClass* Class)
    {
        TSharedRef<FJsonObject> CObj = MakeShared<FJsonObject>();
        CObj->SetStringField(TEXT("name"),    Class->GetName());
        CObj->SetStringField(TEXT("package"), Class->GetOutermost()->GetName());
        CObj->SetStringField(TEXT("parent"),
            Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT("(none)"));

        // Class role metadata — lets the consumer understand the class's purpose without
        // having to trace the full inheritance chain manually
        CObj->SetBoolField(TEXT("is_abstract"),   Class->HasAnyClassFlags(CLASS_Abstract));
        CObj->SetBoolField(TEXT("is_actor"),      Class->IsChildOf(AActor::StaticClass()));
        CObj->SetBoolField(TEXT("is_component"),  Class->IsChildOf(UActorComponent::StaticClass()));
        CObj->SetBoolField(TEXT("is_interface"),  Class->IsChildOf(UInterface::StaticClass()));

        // Implemented interfaces
        TArray<TSharedPtr<FJsonValue>> IfaceArr;
        for (const FImplementedInterface& Iface : Class->Interfaces)
        {
            if (Iface.Class)
                IfaceArr.Add(MakeShared<FJsonValueString>(Iface.Class->GetName()));
        }
        CObj->SetArrayField(TEXT("interfaces"), IfaceArr);

        // Direct properties (ExcludeSuper — own additions only)
        TArray<TSharedPtr<FJsonValue>> PropArr;
        for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop) continue;

            // Skip function parameters exposed as child properties
            if (Prop->GetOwner<UFunction>()) continue;

            TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
            PObj->SetStringField(TEXT("name"),  Prop->GetName());
            PObj->SetStringField(TEXT("type"),  GetPropertyTypeName(Prop));
            PObj->SetArrayField (TEXT("flags"), GetPropertyFlagsJson(Prop));
            PropArr.Add(MakeShared<FJsonValueObject>(PObj));

            if (PropArr.Num() >= 200) break;    // per-class safety cap
        }
        CObj->SetArrayField(TEXT("properties"), PropArr);

        // Direct functions (ExcludeSuper — own additions only)
        TArray<TSharedPtr<FJsonValue>> FuncArr;
        for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (!Func) continue;

            TSharedRef<FJsonObject> FObj = MakeShared<FJsonObject>();
            FObj->SetStringField(TEXT("name"),  Func->GetName());
            FObj->SetArrayField (TEXT("flags"), GetFunctionFlagsJson(Func));

            // Parameters and return type
            // CPF_ReturnParm marks the return value; all others with CPF_Parm are inputs.
            TArray<TSharedPtr<FJsonValue>> ParamArr;
            FString ReturnType = TEXT("void");
            for (TFieldIterator<FProperty> ParamIt(Func); ParamIt; ++ParamIt)
            {
                FProperty* Param = *ParamIt;
                if (!Param->HasAnyPropertyFlags(CPF_Parm)) continue;

                if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    ReturnType = GetPropertyTypeName(Param);
                    continue;
                }

                TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
                PObj->SetStringField(TEXT("name"), Param->GetName());
                PObj->SetStringField(TEXT("type"), GetPropertyTypeName(Param));
                ParamArr.Add(MakeShared<FJsonValueObject>(PObj));
            }
            FObj->SetStringField(TEXT("return"), ReturnType);
            FObj->SetArrayField (TEXT("params"),  ParamArr);

            FuncArr.Add(MakeShared<FJsonValueObject>(FObj));
            if (FuncArr.Num() >= 200) break;    // per-class safety cap
        }
        CObj->SetArrayField(TEXT("functions"), FuncArr);

        return CObj;
    }

}   // anonymous namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeSymbolCapture::Initialize(const FString& InOutputDir)
{
    OutputDir = InOutputDir;
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*(OutputDir / TEXT("symbols")));
    UE_LOG(LogTemp, Log, TEXT("ForgeSymbol: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportSymbolIndex
// ---------------------------------------------------------------------------

bool UForgeSymbolCapture::ExportSymbolIndex()
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("generated"), FForgeContextWriter::NowISO8601());

    TArray<TSharedPtr<FJsonValue>> PrefixArr;
    for (const FString& Prefix : TargetPackagePrefixes)
        PrefixArr.Add(MakeShared<FJsonValueString>(Prefix));
    Root->SetArrayField(TEXT("project_packages"), PrefixArr);

    TArray<TSharedPtr<FJsonValue>> ClassArr;
    int32 ClassCount = 0;

    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Class = *ClassIt;
        if (!Class)               continue;
        if (!IsTargetPackage(Class)) continue;

        ClassArr.Add(MakeShared<FJsonValueObject>(SerializeClass(Class)));
        ++ClassCount;

        if (ClassCount >= 1000)   // global safety cap
        {
            UE_LOG(LogTemp, Warning,
                TEXT("ForgeSymbol: Reached 1000-class cap — output may be incomplete"));
            break;
        }
    }

    Root->SetNumberField(TEXT("class_count"), ClassCount);
    Root->SetArrayField (TEXT("classes"),     ClassArr);

    bool bOK = FForgeContextWriter::WriteJSON(
        OutputDir / TEXT("symbols"), TEXT("project_symbols"), Root);

    if (bOK)
    {
        UE_LOG(LogTemp, Log,
            TEXT("ForgeSymbol: Exported %d classes -> symbols/project_symbols.json"),
            ClassCount);
        UpdateIndexFile();
    }
    return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeSymbolCapture::UpdateIndexFile()
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
    Section->SetStringField(TEXT("file"),         TEXT("symbols/project_symbols.json"));
    Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
    Captures->SetObjectField(TEXT("symbols"), Section);

    Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
    FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
