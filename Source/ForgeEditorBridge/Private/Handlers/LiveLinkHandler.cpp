#include "Handlers/LiveLinkHandler.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkBasicRole.h"
#include "Roles/LiveLinkCameraRole.h"
#include "Roles/LiveLinkLightRole.h"
#include "Roles/LiveLinkTransformRole.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("livelink");

namespace
{
    ILiveLinkClient* GetClient()
    {
        IModularFeatures& MF = IModularFeatures::Get();
        if (!MF.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName)) return nullptr;
        return &MF.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    }
}

FBridgeResult ULiveLinkHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid()) return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    if (Action == TEXT("list_subjects"))      return Action_ListSubjects(Params);
    if (Action == TEXT("get_subject_info"))   return Action_GetSubjectInfo(Params);
    if (Action == TEXT("evaluate_subject"))   return Action_EvaluateSubject(Params);
    if (Action == TEXT("list_sources"))       return Action_ListSources(Params);
    if (Action == TEXT("set_subject_enabled"))return Action_SetSubjectEnabled(Params);
    return MakeUnknownAction(DOMAIN, Action,
        TEXT("list_subjects, get_subject_info, evaluate_subject, list_sources, set_subject_enabled"));
}

FBridgeResult ULiveLinkHandler::Action_ListSubjects(TSharedPtr<FJsonObject> Params)
{
    ILiveLinkClient* Client = GetClient();
    if (!Client) return MakeError(DOMAIN, TEXT("list_subjects"), 3003,
        TEXT("LiveLink modular feature not available"),
        TEXT("Enable the LiveLink plugin in Plugins > Animation > Live Link"));

    TArray<FLiveLinkSubjectKey> Keys = Client->GetSubjects(/*bIncludeDisabledSubjects=*/true, /*bIncludeVirtualSubjects=*/true);
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FLiveLinkSubjectKey& K : Keys)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("subject"), K.SubjectName.ToString());
        O->SetStringField(TEXT("source_guid"), K.Source.ToString());
        TSubclassOf<ULiveLinkRole> Role = Client->GetSubjectRole_AnyThread(K);
        O->SetStringField(TEXT("role"), Role ? Role->GetName() : FString());
        O->SetBoolField(TEXT("enabled"), Client->IsSubjectEnabled(K, /*bForThisFrame=*/false));
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("subjects"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_subjects"),
        FString::Printf(TEXT("%d subject(s)"), Arr.Num()), Data);
}

FBridgeResult ULiveLinkHandler::Action_GetSubjectInfo(TSharedPtr<FJsonObject> Params)
{
    FString SubjectName;
    if (!Params->TryGetStringField(TEXT("subject_name"), SubjectName) || SubjectName.IsEmpty())
        return MakeError(DOMAIN, TEXT("get_subject_info"), 1000, TEXT("'subject_name' is required"));
    ILiveLinkClient* Client = GetClient();
    if (!Client) return MakeError(DOMAIN, TEXT("get_subject_info"), 3003, TEXT("LiveLink unavailable"));

    const FName Subject(*SubjectName);
    TArray<FLiveLinkSubjectKey> Keys = Client->GetSubjects(true, true);
    for (const FLiveLinkSubjectKey& K : Keys)
    {
        if (K.SubjectName == Subject)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            TSubclassOf<ULiveLinkRole> Role = Client->GetSubjectRole_AnyThread(K);
            Data->SetStringField(TEXT("role"), Role ? Role->GetName() : FString());
            Data->SetBoolField(TEXT("enabled"), Client->IsSubjectEnabled(K, false));
            Data->SetStringField(TEXT("source_guid"), K.Source.ToString());
            return MakeSuccess(DOMAIN, TEXT("get_subject_info"),
                FString::Printf(TEXT("Subject '%s'"), *SubjectName), Data);
        }
    }
    return MakeError(DOMAIN, TEXT("get_subject_info"), 2000,
        FString::Printf(TEXT("Subject '%s' not found"), *SubjectName));
}

FBridgeResult ULiveLinkHandler::Action_EvaluateSubject(TSharedPtr<FJsonObject> Params)
{
    FString SubjectName;
    if (!Params->TryGetStringField(TEXT("subject_name"), SubjectName) || SubjectName.IsEmpty())
        return MakeError(DOMAIN, TEXT("evaluate_subject"), 1000, TEXT("'subject_name' is required"));
    ILiveLinkClient* Client = GetClient();
    if (!Client) return MakeError(DOMAIN, TEXT("evaluate_subject"), 3003, TEXT("LiveLink unavailable"));

    FLiveLinkSubjectFrameData Frame;
    const FName Subject(*SubjectName);
    if (!Client->EvaluateFrame_AnyThread(Subject, ULiveLinkBasicRole::StaticClass(), Frame))
        return MakeError(DOMAIN, TEXT("evaluate_subject"), 3000,
            FString::Printf(TEXT("EvaluateFrame returned false for '%s'"), *SubjectName));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("subject"), SubjectName);
    Data->SetBoolField(TEXT("has_static_data"), Frame.StaticData.IsValid());
    Data->SetBoolField(TEXT("has_frame_data"),  Frame.FrameData.IsValid());
    return MakeSuccess(DOMAIN, TEXT("evaluate_subject"),
        FString::Printf(TEXT("Evaluated '%s'"), *SubjectName), Data);
}

FBridgeResult ULiveLinkHandler::Action_ListSources(TSharedPtr<FJsonObject> Params)
{
    ILiveLinkClient* Client = GetClient();
    if (!Client) return MakeError(DOMAIN, TEXT("list_sources"), 3003, TEXT("LiveLink unavailable"));

    TArray<FGuid> SourceIds = Client->GetSources();
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FGuid& Id : SourceIds)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("guid"), Id.ToString());
        O->SetStringField(TEXT("type"), Client->GetSourceType(Id).ToString());
        O->SetStringField(TEXT("machine"), Client->GetSourceMachineName(Id).ToString());
        O->SetStringField(TEXT("status"), Client->GetSourceStatus(Id).ToString());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("sources"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_sources"),
        FString::Printf(TEXT("%d source(s)"), Arr.Num()), Data);
}

FBridgeResult ULiveLinkHandler::Action_SetSubjectEnabled(TSharedPtr<FJsonObject> Params)
{
    FString SubjectName;
    bool bEnabled = true;
    if (!Params->TryGetStringField(TEXT("subject_name"), SubjectName) || SubjectName.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_subject_enabled"), 1000, TEXT("'subject_name' is required"));
    if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
        return MakeError(DOMAIN, TEXT("set_subject_enabled"), 1000, TEXT("'enabled' (bool) is required"));

    ILiveLinkClient* Client = GetClient();
    if (!Client) return MakeError(DOMAIN, TEXT("set_subject_enabled"), 3003, TEXT("LiveLink unavailable"));
    const FName Subject(*SubjectName);
    TArray<FLiveLinkSubjectKey> Keys = Client->GetSubjects(true, true);
    for (const FLiveLinkSubjectKey& K : Keys)
    {
        if (K.SubjectName == Subject)
        {
            Client->SetSubjectEnabled(K, bEnabled);
            return MakeSuccess(DOMAIN, TEXT("set_subject_enabled"),
                FString::Printf(TEXT("'%s' enabled=%s"), *SubjectName, bEnabled ? TEXT("true") : TEXT("false")));
        }
    }
    return MakeError(DOMAIN, TEXT("set_subject_enabled"), 2000,
        FString::Printf(TEXT("Subject '%s' not found"), *SubjectName));
}
