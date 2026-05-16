#pragma once
#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "LiveLinkHandler.generated.h"

/**
 * LiveLinkHandler — domain "livelink"  (UE 5.7)
 *
 * Wraps ILiveLinkClient (via IModularFeatures) for subject discovery and
 * frame evaluation. Source-provider authoring (Editor / OSC / FreeD / etc.)
 * is C++/plugin-side; this handler covers consumer-side queries.
 *
 * Actions:
 *   list_subjects        → returns array of subject names + role class
 *   get_subject_info     → subject_name → returns {is_enabled, source_name, role}
 *   evaluate_subject     → subject_name → returns last-known frame data summary
 *   list_sources         → enumerate active LiveLink sources
 *   set_subject_enabled  → subject_name, enabled (bool)
 */
UCLASS()
class FORGEEDITORBRIDGE_API ULiveLinkHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("livelink"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("list_subjects"), TEXT("get_subject_info"), TEXT("evaluate_subject"),
            TEXT("list_sources"), TEXT("set_subject_enabled")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    FBridgeResult Action_ListSubjects      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetSubjectInfo    (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_EvaluateSubject   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListSources       (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SetSubjectEnabled (TSharedPtr<FJsonObject> Params);
};
