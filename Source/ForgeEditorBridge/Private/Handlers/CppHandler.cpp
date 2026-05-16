#include "Handlers/CppHandler.h"
#include "ForgeAISettings.h"
#include "ForgeAISubsystem.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

// Live Coding
#include "ILiveCodingModule.h"  // module: LiveCoding

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UCppHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
    Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        FBridgeResult R = CreateResult(TEXT("cpp"), Action);
        R.Message = TEXT("Params object is null");
        R.ErrorCode = 1000;
        return R;
    }

    if (Action == TEXT("write_source_file"))      return Action_WriteSourceFile(Params);
    if (Action == TEXT("write_header_file"))      return Action_WriteHeaderFile(Params);
    if (Action == TEXT("trigger_live_coding"))    return Action_TriggerLiveCoding(Params);
    if (Action == TEXT("get_live_coding_status")) return Action_GetLiveCodingStatus(Params);
    if (Action == TEXT("read_source_file"))       return Action_ReadSourceFile(Params);
    if (Action == TEXT("get_last_build_result"))  return Action_GetLastBuildResult(Params);
    if (Action == TEXT("generate_file"))          return Action_GenerateFile(Params);
    if (Action == TEXT("get_compile_errors"))     return Action_GetCompileErrors(Params);
    if (Action == TEXT("regenerate_project_files")) return Action_RegenerateProjectFiles(Params);
    if (Action == TEXT("add_module_dependency"))    return Action_AddModuleDependency(Params);
    if (Action == TEXT("read_build_cs"))            return Action_ReadBuildCs(Params);

    FBridgeResult R = CreateResult(TEXT("cpp"), Action);
    R.Message = FString::Printf(TEXT("Unknown cpp action '%s'. Valid: write_source_file, "
        "write_header_file, trigger_live_coding, get_live_coding_status, read_source_file, "
        "get_last_build_result, generate_file, get_compile_errors, regenerate_project_files, "
        "add_module_dependency, read_build_cs"), *Action);
    R.ErrorCode = 1001;
    return R;
}

// ---------------------------------------------------------------------------
// write_source_file
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_WriteSourceFile(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("write_source_file"));
    FString RelPath, Content;
    if (!Params->TryGetStringField(TEXT("rel_path"), RelPath) ||
        !Params->TryGetStringField(TEXT("content"),  Content))
    {
        Result.Message = TEXT("write_source_file requires: rel_path, content");
        Result.ErrorCode = 1000;
        return Result;
    }
    FString AbsPath;
    if (!ValidateSourcePath(RelPath, AbsPath, Result)) return Result;
    WriteFile(AbsPath, Content, Result);
    return Result;
}

// ---------------------------------------------------------------------------
// write_header_file
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_WriteHeaderFile(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("write_header_file"));
    FString RelPath, Content;
    if (!Params->TryGetStringField(TEXT("rel_path"), RelPath) ||
        !Params->TryGetStringField(TEXT("content"),  Content))
    {
        Result.Message = TEXT("write_header_file requires: rel_path, content");
        Result.ErrorCode = 1000;
        return Result;
    }
    FString AbsPath;
    if (!ValidateSourcePath(RelPath, AbsPath, Result)) return Result;
    WriteFile(AbsPath, Content, Result);
    return Result;
}

// ---------------------------------------------------------------------------
// trigger_live_coding
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_TriggerLiveCoding(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("trigger_live_coding"));

    ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
    if (!LC)
    {
        Result.Message = TEXT("LiveCoding module not available — is the LiveCoding plugin enabled?");
        Result.ErrorCode = 3000;
        return Result;
    }

    if (!LC->IsEnabledForSession())
    {
        LC->EnableForSession(true);
    }

    // Compile() triggers an iterative Live Coding compile (fire-and-forget).
    // StartLiveCoding() is a lifecycle method — do NOT use it for iterative compiles.
    LC->Compile(ELiveCodingCompileFlags::None, nullptr);

    Result.bSuccess = true;
    Result.Message  = TEXT("Live Coding compile triggered. Poll CCP build/errors.json for result.");
    return Result;
}

// ---------------------------------------------------------------------------
// get_live_coding_status
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_GetLiveCodingStatus(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("get_live_coding_status"));

    ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
    if (!LC)
    {
        Result.bSuccess = true;
        Result.Message  = TEXT("{\"enabled\":false,\"status\":\"unavailable\"}");
        return Result;
    }

    bool bEnabled = LC->IsEnabledForSession();

    TSharedPtr<FJsonObject> StatusJson = MakeShared<FJsonObject>();
    StatusJson->SetBoolField(TEXT("enabled"), bEnabled);
    // Note: live compiling state not exposed on ILiveCodingModule — poll CCP build/errors.json

    FString StatusStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&StatusStr);
    FJsonSerializer::Serialize(StatusJson.ToSharedRef(), Writer);

    Result.bSuccess  = true;
    Result.ExtraData = StatusStr;
    Result.Message   = TEXT("Live Coding status queried. Check ExtraData for details.");
    return Result;
}

// ---------------------------------------------------------------------------
// read_source_file
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_ReadSourceFile(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("read_source_file"));
    FString RelPath;
    if (!Params->TryGetStringField(TEXT("rel_path"), RelPath))
    {
        Result.Message = TEXT("read_source_file requires: rel_path");
        Result.ErrorCode = 1000;
        return Result;
    }
    FString AbsPath;
    if (!ValidateSourcePath(RelPath, AbsPath, Result)) return Result;

    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *AbsPath))
    {
        Result.Message = FString::Printf(TEXT("File not found: %s"), *AbsPath);
        Result.ErrorCode = 2000;
        Result.RecoveryHint = TEXT("Verify the rel_path is correct relative to {ProjectDir}/Source/");
        return Result;
    }
    Result.bSuccess     = true;
    Result.AffectedPath = AbsPath;
    Result.ExtraData    = FileContent;
    Result.Message      = FString::Printf(TEXT("Read %d chars from %s"), FileContent.Len(), *AbsPath);
    return Result;
}

// ---------------------------------------------------------------------------
// get_last_build_result — read CCP's build/errors.json
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_GetLastBuildResult(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("get_last_build_result"));

    const FString BuildResultPath = FPaths::ConvertRelativePathToFull(
		GetMutableDefault<UForgeAISettings>()->GetAbsoluteContextDirectory() / TEXT("build/errors.json"));

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *BuildResultPath))
    {
        Result.Message = FString::Printf(
            TEXT("get_last_build_result: no build result file found at '%s'. "
                 "Trigger a compile via 'cpp/trigger_live_coding' first, or verify CCP is active."),
            *BuildResultPath);
        Result.ErrorCode = 2000;
        return Result;
    }

    Result.bSuccess  = true;
    Result.ExtraData = Content;
    Result.Message   = FString::Printf(TEXT("Build result read from %s"), *BuildResultPath);
    return Result;
}

// ---------------------------------------------------------------------------
// generate_file — build a .h/.cpp template pair and write them to Source/
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_GenerateFile(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("generate_file"));

    FString Template;
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("template"),   Template) ||
        !Params->TryGetStringField(TEXT("class_name"), ClassName))
    {
        Result.Message   = TEXT("generate_file requires: template, class_name "
                                "(template is one of: actor, actor_component, interface, "
                                "gameplay_ability, attribute_set, struct, enum)");
        Result.ErrorCode = 1000;
        return Result;
    }

    FString ModuleName;
    if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
    {
        ModuleName = FApp::GetProjectName();
    }

    // Build the *_API macro string (e.g. "MYMODULE_API").
    const FString ModuleApi = FString::Printf(TEXT("%s_API"), *ModuleName.ToUpper());

    // Header + source content -------------------------------------------------
    FString HeaderContent;
    FString SourceContent;

    const FString T = Template.ToLower();

    if (T == TEXT("actor"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"GameFramework/Actor.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("UCLASS()\r\n")
            TEXT("class %s A%s : public AActor\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n")
            TEXT("public:\r\n")
            TEXT("    A%s();\r\n")
            TEXT("protected:\r\n")
            TEXT("    virtual void BeginPlay() override;\r\n")
            TEXT("public:\r\n")
            TEXT("    virtual void Tick(float DeltaTime) override;\r\n")
            TEXT("};\r\n"),
            *ClassName, *ModuleApi, *ClassName, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n\r\n")
            TEXT("A%s::A%s() { PrimaryActorTick.bCanEverTick = true; }\r\n")
            TEXT("void A%s::BeginPlay() { Super::BeginPlay(); }\r\n")
            TEXT("void A%s::Tick(float DeltaTime) { Super::Tick(DeltaTime); }\r\n"),
            *ClassName, *ClassName, *ClassName, *ClassName, *ClassName);
    }
    else if (T == TEXT("actor_component"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"Components/ActorComponent.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))\r\n")
            TEXT("class %s U%s : public UActorComponent\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n")
            TEXT("public:\r\n")
            TEXT("    U%s();\r\n")
            TEXT("protected:\r\n")
            TEXT("    virtual void BeginPlay() override;\r\n")
            TEXT("};\r\n"),
            *ClassName, *ModuleApi, *ClassName, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n\r\n")
            TEXT("U%s::U%s() { PrimaryComponentTick.bCanEverTick = false; }\r\n")
            TEXT("void U%s::BeginPlay() { Super::BeginPlay(); }\r\n"),
            *ClassName, *ClassName, *ClassName, *ClassName);
    }
    else if (T == TEXT("interface"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"UObject/Interface.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("UINTERFACE(MinimalAPI)\r\n")
            TEXT("class U%s : public UInterface { GENERATED_BODY() };\r\n\r\n")
            TEXT("class %s I%s\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n")
            TEXT("};\r\n"),
            *ClassName, *ClassName, *ModuleApi, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n"),
            *ClassName);
    }
    else if (T == TEXT("gameplay_ability"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"Abilities/GameplayAbility.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("UCLASS()\r\n")
            TEXT("class %s U%s : public UGameplayAbility\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n")
            TEXT("public:\r\n")
            TEXT("    U%s();\r\n")
            TEXT("    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;\r\n")
            TEXT("};\r\n"),
            *ClassName, *ModuleApi, *ClassName, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n\r\n")
            TEXT("U%s::U%s() {}\r\n")
            TEXT("void U%s::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)\r\n")
            TEXT("{\r\n")
            TEXT("    Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);\r\n")
            TEXT("    EndAbility(Handle, ActorInfo, ActivationInfo, true, false);\r\n")
            TEXT("}\r\n"),
            *ClassName, *ClassName, *ClassName, *ClassName);
    }
    else if (T == TEXT("attribute_set"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"AttributeSet.h\"\r\n")
            TEXT("#include \"AbilitySystemComponent.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \\\r\n")
            TEXT("    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \\\r\n")
            TEXT("    GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \\\r\n")
            TEXT("    GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \\\r\n")
            TEXT("    GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)\r\n\r\n")
            TEXT("UCLASS()\r\n")
            TEXT("class %s U%s : public UAttributeSet\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n")
            TEXT("public:\r\n")
            TEXT("    UPROPERTY(BlueprintReadOnly, Category = \"Attributes\", ReplicatedUsing = OnRep_Health)\r\n")
            TEXT("    FGameplayAttributeData Health;\r\n")
            TEXT("    ATTRIBUTE_ACCESSORS(U%s, Health)\r\n\r\n")
            TEXT("    UFUNCTION()\r\n")
            TEXT("    void OnRep_Health(const FGameplayAttributeData& OldHealth);\r\n")
            TEXT("    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\r\n")
            TEXT("};\r\n"),
            *ClassName, *ModuleApi, *ClassName, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n")
            TEXT("#include \"Net/UnrealNetwork.h\"\r\n")
            TEXT("#include \"GameplayEffectExtension.h\"\r\n\r\n")
            TEXT("void U%s::OnRep_Health(const FGameplayAttributeData& OldHealth) { GAMEPLAYATTRIBUTE_REPNOTIFY(U%s, Health, OldHealth); }\r\n")
            TEXT("void U%s::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const\r\n")
            TEXT("{\r\n")
            TEXT("    Super::GetLifetimeReplicatedProps(OutLifetimeProps);\r\n")
            TEXT("    DOREPLIFETIME_CONDITION_NOTIFY(U%s, Health, COND_None, REPNOTIFY_Always);\r\n")
            TEXT("}\r\n"),
            *ClassName, *ClassName, *ClassName, *ClassName, *ClassName);
    }
    else if (T == TEXT("struct"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("USTRUCT(BlueprintType)\r\n")
            TEXT("struct %s F%s\r\n")
            TEXT("{\r\n")
            TEXT("    GENERATED_BODY()\r\n\r\n")
            TEXT("    UPROPERTY(EditAnywhere, BlueprintReadWrite)\r\n")
            TEXT("    float Value = 0.f;\r\n")
            TEXT("};\r\n"),
            *ClassName, *ModuleApi, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n"),
            *ClassName);
    }
    else if (T == TEXT("enum"))
    {
        HeaderContent = FString::Printf(
            TEXT("#pragma once\r\n")
            TEXT("#include \"CoreMinimal.h\"\r\n")
            TEXT("#include \"%s.generated.h\"\r\n\r\n")
            TEXT("UENUM(BlueprintType)\r\n")
            TEXT("enum class E%s : uint8\r\n")
            TEXT("{\r\n")
            TEXT("    None UMETA(DisplayName = \"None\"),\r\n")
            TEXT("    Option1 UMETA(DisplayName = \"Option 1\"),\r\n")
            TEXT("    Option2 UMETA(DisplayName = \"Option 2\"),\r\n")
            TEXT("};\r\n"),
            *ClassName, *ClassName);

        SourceContent = FString::Printf(
            TEXT("#include \"%s.h\"\r\n"),
            *ClassName);
    }
    else
    {
        Result.Message = FString::Printf(
            TEXT("Unknown template '%s'. Valid: actor, actor_component, interface, "
                 "gameplay_ability, attribute_set, struct, enum"),
            *Template);
        Result.ErrorCode = 1001;
        return Result;
    }

    // Resolve rel_path (folder or explicit path).
    // Default: {ModuleName}/{ClassName}.h and .cpp
    FString RelPathBase;
    Params->TryGetStringField(TEXT("rel_path"), RelPathBase);
    if (RelPathBase.IsEmpty())
    {
        RelPathBase = ModuleName;
    }

    // Normalize: treat RelPathBase as a subfolder under Source/. Append {ClassName}.{ext}
    // Strip trailing slashes so "MyModule/" and "MyModule" behave the same.
    RelPathBase.RemoveFromEnd(TEXT("/"));
    RelPathBase.RemoveFromEnd(TEXT("\\"));

    const FString HeaderRel = FString::Printf(TEXT("%s/%s.h"),   *RelPathBase, *ClassName);
    const FString SourceRel = FString::Printf(TEXT("%s/%s.cpp"), *RelPathBase, *ClassName);

    FString HeaderAbs, SourceAbs;
    if (!ValidateSourcePath(HeaderRel, HeaderAbs, Result)) return Result;
    if (!ValidateSourcePath(SourceRel, SourceAbs, Result)) return Result;

    FBridgeResult HeaderResult = CreateResult(TEXT("cpp"), TEXT("generate_file"));
    WriteFile(HeaderAbs, HeaderContent, HeaderResult);
    if (!HeaderResult.bSuccess)
    {
        Result.ErrorCode = HeaderResult.ErrorCode != 0 ? HeaderResult.ErrorCode : 3000;
        Result.Message   = FString::Printf(TEXT("Header write failed: %s"), *HeaderResult.Message);
        return Result;
    }

    FBridgeResult SourceResult = CreateResult(TEXT("cpp"), TEXT("generate_file"));
    WriteFile(SourceAbs, SourceContent, SourceResult);
    if (!SourceResult.bSuccess)
    {
        Result.ErrorCode = SourceResult.ErrorCode != 0 ? SourceResult.ErrorCode : 3000;
        Result.Message   = FString::Printf(
            TEXT("Source write failed after header succeeded. Header at '%s'. Source error: %s"),
            *HeaderAbs, *SourceResult.Message);
        return Result;
    }

    // Build ExtraData JSON
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("header_path"), HeaderAbs);
    Out->SetStringField(TEXT("source_path"), SourceAbs);

    FString OutStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
    FJsonSerializer::Serialize(Out.ToSharedRef(), Writer);

    Result.bSuccess     = true;
    Result.AffectedPath = HeaderAbs;
    Result.ExtraData    = OutStr;
    Result.Message      = FString::Printf(
        TEXT("Generated '%s' template pair: %s + %s. Remember to regenerate project files "
             "(or rely on Live Coding) before compiling."),
        *Template, *HeaderAbs, *SourceAbs);
    return Result;
}

// ---------------------------------------------------------------------------
// get_compile_errors — tail the most recent UnrealEditor log in Saved/Logs
// ---------------------------------------------------------------------------

FBridgeResult UCppHandler::Action_GetCompileErrors(TSharedPtr<FJsonObject> Params)
{
    FBridgeResult Result = CreateResult(TEXT("cpp"), TEXT("get_compile_errors"));

    const FString LogsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Logs"));

    IFileManager& FM = IFileManager::Get();

    // Enumerate logs. Prefer UnrealEditor-*.log; fall back to *.log.
    TArray<FString> LogFiles;
    FM.FindFiles(LogFiles, *(LogsDir / TEXT("UnrealEditor-*.log")), true, false);
    if (LogFiles.Num() == 0)
    {
        FM.FindFiles(LogFiles, *(LogsDir / TEXT("*.log")), true, false);
    }

    if (LogFiles.Num() == 0)
    {
        Result.Message = FString::Printf(
            TEXT("get_compile_errors: no .log files found in '%s'. Has the editor ever run?"),
            *LogsDir);
        Result.ErrorCode = 2000;
        return Result;
    }

    // Pick the most recently modified.
    FString BestFile;
    FDateTime BestTime = FDateTime::MinValue();
    for (const FString& Name : LogFiles)
    {
        const FString AbsPath = LogsDir / Name;
        const FDateTime T = FM.GetTimeStamp(*AbsPath);
        if (T > BestTime)
        {
            BestTime = T;
            BestFile = AbsPath;
        }
    }

    if (BestFile.IsEmpty())
    {
        Result.Message = FString::Printf(
            TEXT("get_compile_errors: could not resolve timestamps for any logs in '%s'."),
            *LogsDir);
        Result.ErrorCode = 2000;
        return Result;
    }

    FString FullContent;
    if (!FFileHelper::LoadFileToString(FullContent, *BestFile))
    {
        Result.Message = FString::Printf(TEXT("Failed to read log: %s"), *BestFile);
        Result.ErrorCode = 2000;
        return Result;
    }

    // Tail: last 200 lines OR last 8 KB, whichever leaves more context.
    const int32 CharLimit = 8 * 1024;
    FString Tail = FullContent;
    if (Tail.Len() > CharLimit)
    {
        Tail = Tail.RightChop(Tail.Len() - CharLimit);
    }

    // Then further trim to at most 200 lines.
    TArray<FString> Lines;
    Tail.ParseIntoArrayLines(Lines, /*CullEmpty*/ false);
    const int32 MaxLines = 200;
    if (Lines.Num() > MaxLines)
    {
        Lines.RemoveAt(0, Lines.Num() - MaxLines, EAllowShrinking::Yes);
    }
    const FString TrimmedTail = FString::Join(Lines, TEXT("\n"));

    // Package result as JSON so the caller can discover which log this was.
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("log_path"),  BestFile);
    Out->SetNumberField(TEXT("tail_bytes"), TrimmedTail.Len());
    Out->SetNumberField(TEXT("tail_lines"), Lines.Num());
    Out->SetStringField(TEXT("tail"),       TrimmedTail);

    FString OutStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
    FJsonSerializer::Serialize(Out.ToSharedRef(), Writer);

    Result.bSuccess     = true;
    Result.AffectedPath = BestFile;
    Result.ExtraData    = OutStr;
    Result.Message      = FString::Printf(
        TEXT("Tail of most recent log (%d lines, %d chars) from %s"),
        Lines.Num(), TrimmedTail.Len(), *BestFile);
    return Result;
}

// ---------------------------------------------------------------------------
// ValidateSourcePath
// ---------------------------------------------------------------------------

bool UCppHandler::ValidateSourcePath(const FString& RelPath,
                                      FString&       OutAbsPath,
                                      FBridgeResult& OutResult)
{
    // Reject any path containing ".." to prevent directory traversal
    if (RelPath.Contains(TEXT("..")) || !FPaths::IsRelative(RelPath))
    {
        OutResult.Message = FString::Printf(
            TEXT("Invalid rel_path '%s': must be relative and must not contain '..'"), *RelPath);
        OutResult.ErrorCode = 1001;
        return false;
    }

    // Root: {ProjectDir}/Source/
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
    OutAbsPath = FPaths::ConvertRelativePathToFull(SourceRoot / RelPath);

    // Double-check the resolved path still lives under SourceRoot
    if (!OutAbsPath.StartsWith(SourceRoot))
    {
        OutResult.Message = FString::Printf(
            TEXT("Path escapes Source directory: %s"), *OutAbsPath);
        OutResult.ErrorCode = 1001;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// WriteFile
// ---------------------------------------------------------------------------

void UCppHandler::WriteFile(const FString& AbsPath, const FString& Content,
                             FBridgeResult& Result)
{
    // Ensure parent directory exists
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    const FString Dir = FPaths::GetPath(AbsPath);
    if (!PF.DirectoryExists(*Dir))
        PF.CreateDirectoryTree(*Dir);

    if (FFileHelper::SaveStringToFile(Content, *AbsPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Result.bSuccess     = true;
        Result.AffectedPath = AbsPath;
        Result.Message      = FString::Printf(TEXT("Written %d chars to %s"),
                                               Content.Len(), *AbsPath);
    }
    else
    {
        Result.ErrorCode = 3000;
        Result.Message = FString::Printf(TEXT("Failed to write file: %s"), *AbsPath);
    }
}

// ===========================================================================
// Phase 3: build/project-file management
// ===========================================================================

#include "Misc/MonitoredProcess.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

FBridgeResult UCppHandler::Action_RegenerateProjectFiles(TSharedPtr<FJsonObject> Params)
{
	const FString ProjectFile = FPaths::GetProjectFilePath();
	if (ProjectFile.IsEmpty())
		return MakeError(TEXT("cpp"), TEXT("regenerate_project_files"), 3000, TEXT("No project file path"));

	const FString EngineRoot = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
#if PLATFORM_WINDOWS
	const FString UBT = FPaths::Combine(EngineRoot, TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));
#else
	const FString UBT = FPaths::Combine(EngineRoot, TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool"));
#endif
	const FString Args = FString::Printf(TEXT("-projectfiles -project=\"%s\" -game -engine -progress"), *ProjectFile);

	int32 ReturnCode = -1;
	FString StdOut, StdErr;
	const bool bRan = FPlatformProcess::ExecProcess(*UBT, *Args, &ReturnCode, &StdOut, &StdErr);
	if (!bRan)
		return MakeError(TEXT("cpp"), TEXT("regenerate_project_files"), 3000,
			FString::Printf(TEXT("Failed to launch UBT at: %s"), *UBT));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("return_code"), ReturnCode);
	Data->SetStringField(TEXT("ubt_path"), UBT);
	Data->SetStringField(TEXT("stdout_tail"), StdOut.Right(2048));
	if (ReturnCode != 0)
	{
		Data->SetStringField(TEXT("stderr_tail"), StdErr.Right(2048));
		return MakeError(TEXT("cpp"), TEXT("regenerate_project_files"), 3000,
			FString::Printf(TEXT("UBT exited with code %d"), ReturnCode), TEXT("Inspect stdout_tail/stderr_tail"));
	}
	return MakeSuccess(TEXT("cpp"), TEXT("regenerate_project_files"),
		TEXT("Project files regenerated"), Data);
}

FBridgeResult UCppHandler::Action_AddModuleDependency(TSharedPtr<FJsonObject> Params)
{
	FString BuildCsPath, ModuleName;
	bool bPrivate = true;
	if (!Params->TryGetStringField(TEXT("build_cs_path"), BuildCsPath) || BuildCsPath.IsEmpty())
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 1000,
			TEXT("'build_cs_path' is required (absolute path to a .Build.cs file)"));
	if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 1000, TEXT("'module_name' is required"));
	Params->TryGetBoolField(TEXT("private"), bPrivate);

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *BuildCsPath))
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 2000,
			FString::Printf(TEXT("Could not read: %s"), *BuildCsPath));

	const FString ListMarker = bPrivate ? TEXT("PrivateDependencyModuleNames") : TEXT("PublicDependencyModuleNames");
	const FString Quoted = FString::Printf(TEXT("\"%s\""), *ModuleName);
	if (Content.Contains(Quoted))
	{
		return MakeSuccess(TEXT("cpp"), TEXT("add_module_dependency"),
			FString::Printf(TEXT("Module '%s' already present (no-op)"), *ModuleName));
	}

	int32 MarkerIdx = Content.Find(ListMarker);
	if (MarkerIdx == INDEX_NONE)
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 2001,
			FString::Printf(TEXT("Could not locate %s in build.cs"), *ListMarker));

	int32 OpenBrace = Content.Find(TEXT("{"), ESearchCase::IgnoreCase, ESearchDir::FromStart, MarkerIdx);
	int32 CloseBrace = Content.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromStart, OpenBrace);
	if (OpenBrace == INDEX_NONE || CloseBrace == INDEX_NONE)
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 2001,
			TEXT("Malformed build.cs — could not find module list braces"));

	const FString Insertion = FString::Printf(TEXT("\n            %s,"), *Quoted);
	Content.InsertAt(OpenBrace + 1, Insertion);
	if (!FFileHelper::SaveStringToFile(Content, *BuildCsPath))
		return MakeError(TEXT("cpp"), TEXT("add_module_dependency"), 3000, TEXT("Failed to save build.cs"));

	return MakeSuccess(TEXT("cpp"), TEXT("add_module_dependency"),
		FString::Printf(TEXT("Added '%s' to %s"), *ModuleName, *ListMarker));
}

FBridgeResult UCppHandler::Action_ReadBuildCs(TSharedPtr<FJsonObject> Params)
{
	FString BuildCsPath;
	if (!Params->TryGetStringField(TEXT("build_cs_path"), BuildCsPath) || BuildCsPath.IsEmpty())
		return MakeError(TEXT("cpp"), TEXT("read_build_cs"), 1000, TEXT("'build_cs_path' is required"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *BuildCsPath))
		return MakeError(TEXT("cpp"), TEXT("read_build_cs"), 2000,
			FString::Printf(TEXT("Could not read: %s"), *BuildCsPath));

	FBridgeResult R = MakeSuccess(TEXT("cpp"), TEXT("read_build_cs"), BuildCsPath);
	R.ExtraData = Content;
	return R;
}
