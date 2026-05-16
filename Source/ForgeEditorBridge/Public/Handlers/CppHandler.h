#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "CppHandler.generated.h"

/**
 * CppHandler — domain "cpp"
 *
 * Writes C++ source/header files to {ProjectDir}/Source/ and triggers
 * Live Coding iterative compiles. Part of the AI-driven development loop.
 *
 * Safety: ValidateSourcePath rejects any rel_path containing ".." or
 * resolving outside the Source directory.
 *
 * Actions:
 *   write_source_file      → rel_path, content   → writes .cpp to Source/
 *   write_header_file      → rel_path, content   → writes .h to Source/
 *   trigger_live_coding    → (none)               → triggers LC compile
 *   get_live_coding_status → (none)               → ExtraData JSON: enabled
 *   read_source_file       → rel_path             → ExtraData = file content
 *   get_last_build_result  → (none)               → ExtraData = contents of CCP's build/errors.json
 *   generate_file          → template, class_name, [module_name], [rel_path] → writes .h/.cpp pair
 *   get_compile_errors     → (none)               → ExtraData = tail of most-recent UnrealEditor log
 */
UCLASS()
class FORGEEDITORBRIDGE_API UCppHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()
public:
    virtual FString GetDomainName() const override { return TEXT("cpp"); }
    virtual TArray<FString> GetSupportedActions() const override { return { TEXT("write_source_file"), TEXT("write_header_file"), TEXT("trigger_live_coding"), TEXT("get_live_coding_status"), TEXT("read_source_file"), TEXT("get_last_build_result"), TEXT("generate_file"), TEXT("get_compile_errors"), TEXT("regenerate_project_files"), TEXT("add_module_dependency"), TEXT("read_build_cs") }; }
    virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
    virtual FBridgeResult HandleCommand(const FString& Action,
                                        TSharedPtr<FJsonObject> Params) override;
private:
    FBridgeResult Action_WriteSourceFile(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_WriteHeaderFile(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_TriggerLiveCoding(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetLiveCodingStatus(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ReadSourceFile(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetLastBuildResult(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GenerateFile(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetCompileErrors(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_RegenerateProjectFiles(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AddModuleDependency(TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ReadBuildCs(TSharedPtr<FJsonObject> Params);

    // Returns false and populates OutResult.Message if path is invalid/unsafe
    bool ValidateSourcePath(const FString& RelPath, FString& OutAbsPath, FBridgeResult& OutResult);
    void WriteFile(const FString& AbsPath, const FString& Content, FBridgeResult& Result);
};
