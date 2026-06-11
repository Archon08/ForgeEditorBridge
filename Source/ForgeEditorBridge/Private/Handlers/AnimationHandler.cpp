#include "Handlers/AnimationHandler.h"
#include "ForgeAISubsystem.h"
#include "Attention/BridgeAttentionManager.h"
#include "Capture/ForgeAnimationCapture.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Sections/MovieSceneColorSection.h"
// #include "Tracks/MovieSceneDoubleVectorTrack.h"   // UE 5.7: MovieSceneDoubleVectorTrack.h removed
// #include "Sections/MovieSceneDoubleVectorSection.h"  // UE 5.7: MovieSceneDoubleVectorSection.h removed
#include "Channels/MovieSceneDoubleChannel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Phase 1d skeletal animation includes
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/SkeletalMesh.h"
#if WITH_EDITOR
// AnimGraphNode_MotionMatching: guarded -- requires PoseSearch plugin
// AnimGraphNode_ControlRig: guarded -- requires ControlRig plugin
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static UWidgetBlueprint* ResolveAnimWBP(UBridgeSubsystem* Subsystem, TSharedPtr<FJsonObject> Params, FString& OutError)
{
    FString AssetPath;
    if (Params.IsValid() && Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
    {
        FString BaseName   = FPackageName::GetShortName(AssetPath);
        FString ObjectPath = AssetPath + TEXT(".") + BaseName;
        UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
        if (!WBP) WBP = Cast<UWidgetBlueprint>(FSoftObjectPath(AssetPath).ResolveObject());
        if (!WBP) { OutError = FString::Printf(TEXT("Could not load WBP: %s"), *AssetPath); return nullptr; }
        return WBP;
    }
    if (Subsystem && Subsystem->AttentionManager)
    {
        UWidgetBlueprint* WBP = Subsystem->AttentionManager->GetCachedWBP();
        if (!WBP) { OutError = TEXT("No asset_path and no WBP cached in AttentionManager"); return nullptr; }
        return WBP;
    }
    OutError = TEXT("No asset_path and AttentionManager unavailable");
    return nullptr;
}

static UWidgetAnimation* FindAnimation(UWidgetBlueprint* BP, const FString& Name)
{
    for (UWidgetAnimation* Anim : BP->Animations)
        if (Anim && Anim->GetName() == Name) return Anim;
    return nullptr;
}

static FString SerializeJson(const TSharedPtr<FJsonObject>& Obj)
{
    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
    return Out;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
FBridgeResult UAnimationHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        FBridgeResult R = CreateResult(TEXT("animation"), Action);
        R.Message = TEXT("Params object is null");
        R.ErrorCode = 1000;
        return R;
    }

    FBridgeResult Result = CreateResult(TEXT("animation"), Action);
    FString Error;
    UWidgetBlueprint* WBP = ResolveAnimWBP(Subsystem, Params, Error);

    // Actions that don't need WBP (scope setters delegate to ContextHandler pattern)
    if (Action == TEXT("set_animation_scope"))
    {
        FString AnimName;
        Params->TryGetStringField(TEXT("animation_name"), AnimName);
        if (Subsystem && Subsystem->AttentionManager)
            Subsystem->AttentionManager->SetAnimationScope(AnimName);
        // Also create the animation if it doesn't exist yet (matches reference behaviour)
        if (WBP && !AnimName.IsEmpty() && !FindAnimation(WBP, AnimName))
        {
            // delegate to create path below
        }
        else
        {
            Result.bSuccess = true;
            Result.Message  = FString::Printf(TEXT("Animation scope set to: %s"), *AnimName);
            return Result;
        }
    }

    if (Action == TEXT("set_widget_scope"))
    {
        FString WidgetName;
        Params->TryGetStringField(TEXT("widget_name"), WidgetName);
        if (Subsystem && Subsystem->AttentionManager)
            Subsystem->AttentionManager->SetWidgetScope(WidgetName);
        Result.bSuccess = true;
        Result.Message  = FString::Printf(TEXT("Widget scope set to: %s"), *WidgetName);
        return Result;
    }

    if (!WBP)
    {
        Result.Message = Error;
        Result.ErrorCode = 2000;
        Result.RecoveryHint = TEXT("Provide a valid 'asset_path' or cache a WBP via AttentionManager");
        return Result;
    }

    // ---- Read actions -------------------------------------------------------
    if (Action == TEXT("get_all_animations"))
    {
        TArray<TSharedPtr<FJsonValue>> AnimArr;
        for (UWidgetAnimation* Anim : WBP->Animations)
        {
            if (!Anim) continue;
            TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
            AnimObj->SetStringField(TEXT("name"),       Anim->GetName());
            AnimObj->SetNumberField (TEXT("start_time"), Anim->GetStartTime());
            AnimObj->SetNumberField (TEXT("end_time"),   Anim->GetEndTime());
            AnimArr.Add(MakeShared<FJsonValueObject>(AnimObj));
        }
        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("animations"), AnimArr);
        Result.bSuccess  = true;
        Result.ExtraData = SerializeJson(DataObj);
        Result.Message   = FString::Printf(TEXT("Found %d animations"), AnimArr.Num());
        return Result;
    }

    if (Action == TEXT("get_animated_widgets"))
    {
        FString AnimName; Params->TryGetStringField(TEXT("animation_name"), AnimName);
        if (AnimName.IsEmpty() && Subsystem && Subsystem->AttentionManager)
            AnimName = Subsystem->AttentionManager->GetAnimationScope();

        UWidgetAnimation* Anim = FindAnimation(WBP, AnimName);
        if (!Anim) { Result.Message = FString::Printf(TEXT("Animation not found: %s"), *AnimName); Result.ErrorCode = 2000; return Result; }

        TArray<TSharedPtr<FJsonValue>> WArr;
        for (const FWidgetAnimationBinding& B : Anim->AnimationBindings)
        {
            TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
            WObj->SetStringField(TEXT("widget_name"), B.WidgetName.ToString());
            WObj->SetStringField(TEXT("guid"),        B.AnimationGuid.ToString());
            WObj->SetBoolField  (TEXT("is_root"),     B.bIsRootWidget);
            WArr.Add(MakeShared<FJsonValueObject>(WObj));
        }
        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("widgets"), WArr);
        Result.bSuccess  = true;
        Result.ExtraData = SerializeJson(DataObj);
        Result.Message   = FString::Printf(TEXT("Found %d animated widgets"), WArr.Num());
        return Result;
    }

    if (Action == TEXT("get_animation_keyframes"))
    {
        FString AnimName; Params->TryGetStringField(TEXT("animation_name"), AnimName);
        if (AnimName.IsEmpty() && Subsystem && Subsystem->AttentionManager)
            AnimName = Subsystem->AttentionManager->GetAnimationScope();

        UWidgetAnimation* Anim = FindAnimation(WBP, AnimName);
        if (!Anim) { Result.Message = FString::Printf(TEXT("Animation not found: %s"), *AnimName); Result.ErrorCode = 2000; return Result; }

        UMovieScene* MS = Anim->GetMovieScene();
        if (!MS) { Result.Message = TEXT("MovieScene is null"); Result.ErrorCode = 3000; return Result; }

        TArray<TSharedPtr<FJsonValue>> TracksArr;
        FFrameRate TickRes = MS->GetTickResolution();

        for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
        {
            FString WidgetName = Binding.WidgetName.ToString();
            for (const UMovieSceneTrack* Track : MS->FindTracks(UMovieSceneFloatTrack::StaticClass(), Binding.AnimationGuid))
            {
                const UMovieSceneFloatTrack* FTrack = Cast<UMovieSceneFloatTrack>(Track);
                if (!FTrack) continue;
                TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
                TObj->SetStringField(TEXT("widget_name"),   WidgetName);
                TObj->SetStringField(TEXT("property_name"), FTrack->GetPropertyName().ToString());
                TArray<TSharedPtr<FJsonValue>> KeysArr;
                if (FTrack->GetAllSections().Num() > 0)
                {
                    const UMovieSceneFloatSection* Sec = Cast<UMovieSceneFloatSection>(FTrack->GetAllSections()[0]);
                    if (Sec)
                    {
                        auto Times  = Sec->GetChannel().GetData().GetTimes();
                        auto Values = Sec->GetChannel().GetData().GetValues();
                        for (int32 i = 0; i < Times.Num(); ++i)
                        {
                            TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
                            double T = (double)Times[i].Value / TickRes.AsDecimal();
                            KObj->SetNumberField(TEXT("time"),  T);
                            KObj->SetNumberField(TEXT("value"), Values[i].Value);
                            KeysArr.Add(MakeShared<FJsonValueObject>(KObj));
                        }
                    }
                }
                TObj->SetArrayField(TEXT("keys"), KeysArr);
                TracksArr.Add(MakeShared<FJsonValueObject>(TObj));
            }
        }

        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("tracks"), TracksArr);
        Result.bSuccess  = true;
        Result.ExtraData = SerializeJson(DataObj);
        Result.Message   = FString::Printf(TEXT("Returned %d tracks for '%s'"), TracksArr.Num(), *AnimName);
        return Result;
    }

    // ---- Write actions -------------------------------------------------------
    if (Action == TEXT("create_animation") || Action == TEXT("set_animation_scope"))
    {
        FString AnimName;
        Params->TryGetStringField(TEXT("animation_name"), AnimName);
        if (AnimName.IsEmpty())
            AnimName = FString::Printf(TEXT("Animation_%d"), WBP->Animations.Num());

        // Find or create
        UWidgetAnimation* ExistingAnim = FindAnimation(WBP, AnimName);
        if (!ExistingAnim)
        {
            UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(
                WBP, FName(*AnimName), RF_Public | RF_Transactional);
            NewAnim->MovieScene = NewObject<UMovieScene>(
                NewAnim, FName(TEXT("MovieScene")), RF_Transactional);

            WBP->Modify();
            WBP->Animations.Add(NewAnim);

            // CRITICAL: Add GUID so the animation is recognized as a widget variable
            FGuid NewGuid = FGuid::NewGuid();
            WBP->WidgetVariableNameToGuidMap.Add(NewAnim->GetFName(), NewGuid);

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
            FKismetEditorUtilities::CompileBlueprint(WBP);

            if (GEditor)
            {
                if (UAssetEditorSubsystem* AESub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
                    AESub->OpenEditorForAsset(WBP);
            }

            Result.AffectedPath = WBP->GetPathName();
            Result.Message      = FString::Printf(TEXT("Created animation '%s'"), *AnimName);
        }
        else
        {
            Result.Message = FString::Printf(TEXT("Animation '%s' already exists"), *AnimName);
        }

        if (Subsystem && Subsystem->AttentionManager)
            Subsystem->AttentionManager->SetAnimationScope(AnimName);

        Result.bSuccess = true;
        return Result;
    }

    if (Action == TEXT("delete_animation"))
    {
        FString AnimName; Params->TryGetStringField(TEXT("animation_name"), AnimName);
        int32 Removed = WBP->Animations.RemoveAll([&](UWidgetAnimation* A) {
            return A && A->GetName() == AnimName;
        });
        if (Removed > 0)
        {
            WBP->WidgetVariableNameToGuidMap.Remove(FName(*AnimName));
            WBP->Modify();
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
            Result.bSuccess = true;
            Result.Message  = FString::Printf(TEXT("Deleted animation '%s'"), *AnimName);
        }
        else
        {
            Result.Message = FString::Printf(TEXT("Animation not found: %s"), *AnimName);
            Result.ErrorCode = 2000;
        }
        return Result;
    }

    if (Action == TEXT("set_property_keys"))
    {
        FString AnimName, WidgetName, PropertyName;
        if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.IsEmpty())
        {
            if (Subsystem && Subsystem->AttentionManager)
                AnimName = Subsystem->AttentionManager->GetAnimationScope();
        }
        if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
        {
            if (Subsystem && Subsystem->AttentionManager)
            {
                WidgetName = Subsystem->AttentionManager->GetActiveWidget();
                if (WidgetName.IsEmpty())
                    WidgetName = Subsystem->AttentionManager->GetWidgetScope();
            }
        }
        if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
        { Result.Message = TEXT("set_property_keys: 'property_name' required"); Result.ErrorCode = 1000; return Result; }

        const TArray<TSharedPtr<FJsonValue>>* KeysPtr;
        if (!Params->TryGetArrayField(TEXT("keys"), KeysPtr) || KeysPtr->Num() == 0)
        { Result.Message = TEXT("set_property_keys: 'keys' array required"); Result.ErrorCode = 1000; return Result; }

        if (AnimName.IsEmpty() || WidgetName.IsEmpty())
        { Result.Message = TEXT("set_property_keys: animation_name and widget_name required"); Result.ErrorCode = 1000; return Result; }

        UWidgetAnimation* Anim = FindAnimation(WBP, AnimName);
        if (!Anim) { Result.Message = FString::Printf(TEXT("Animation not found: %s"), *AnimName); Result.ErrorCode = 2000; return Result; }

        UMovieScene* MS = Anim->GetMovieScene();
        MS->Modify();

        // Detect key type from first key
        bool bIsFloat   = false;
        bool bIsColor   = false;
        bool bIsVector2D = false;
        auto FirstKey = (*KeysPtr)[0]->AsObject();
        if (FirstKey->HasField(TEXT("value")))
        {
            auto Val = FirstKey->GetField<EJson::None>(TEXT("value"));
            if (Val->Type == EJson::Number) bIsFloat = true;
            else if (Val->Type == EJson::Object)
            {
                auto Obj = Val->AsObject();
                if (Obj->HasField(TEXT("r"))) bIsColor = true;
                else if (Obj->HasField(TEXT("x"))) bIsVector2D = true;
            }
        }
        if (!bIsFloat && !bIsColor && !bIsVector2D)
        { Result.Message = TEXT("set_property_keys: Cannot detect key type (need numeric value, or {r,g,b,a}, or {x,y})"); Result.ErrorCode = 1001; return Result; }

        // Find or create widget binding
        FGuid WidgetGuid;
        for (int32 i = 0; i < MS->GetPossessableCount(); ++i)
        {
            if (MS->GetPossessable(i).GetName() == WidgetName)
            { WidgetGuid = MS->GetPossessable(i).GetGuid(); break; }
        }

        if (!WidgetGuid.IsValid() && WBP->WidgetTree)
        {
            UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
            if (!Widget) { Result.Message = FString::Printf(TEXT("Widget not found: %s"), *WidgetName); Result.ErrorCode = 2000; Result.RecoveryHint = TEXT("Check widget name exists in the WidgetTree"); return Result; }

            if (!Widget->bIsVariable)
            {
                Widget->bIsVariable = true;
                WBP->Modify();
            }
            if (!WBP->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            {
                WBP->WidgetVariableNameToGuidMap.Add(Widget->GetFName(), FGuid::NewGuid());
                WBP->Modify();
                FKismetEditorUtilities::CompileBlueprint(WBP);
            }

            WidgetGuid = MS->AddPossessable(WidgetName, Widget->GetClass());
            FWidgetAnimationBinding NewBinding;
            NewBinding.WidgetName    = FName(*WidgetName);
            NewBinding.AnimationGuid = WidgetGuid;
            NewBinding.bIsRootWidget = (Widget == WBP->WidgetTree->RootWidget);
            Anim->AnimationBindings.Add(NewBinding);
        }

        FFrameRate TickRes    = MS->GetTickResolution();
        FFrameNumber RangeStart, RangeEnd;
        bool bRangeInit = false;

        auto UpdateRange = [&](FFrameNumber Frame) {
            if (!bRangeInit) { RangeStart = RangeEnd = Frame; bRangeInit = true; return; }
            if (Frame < RangeStart) RangeStart = Frame;
            if (Frame > RangeEnd)   RangeEnd   = Frame;
        };

        if (bIsFloat)
        {
            UMovieSceneTrack* Track = MS->FindTrack(UMovieSceneFloatTrack::StaticClass(), WidgetGuid, FName(*PropertyName));
            if (!Track)
            {
                Track = MS->AddTrack(UMovieSceneFloatTrack::StaticClass(), WidgetGuid);
                Cast<UMovieSceneFloatTrack>(Track)->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
            }
            Track->Modify();
            bool bSectionAdded = false;
            UMovieSceneSection* Sec = Cast<UMovieSceneFloatTrack>(Track)->FindOrAddSection(0, bSectionAdded);
            Sec->SetRange(TRange<FFrameNumber>::All());
            auto& Channel = Cast<UMovieSceneFloatSection>(Sec)->GetChannel();
            for (const auto& V : *KeysPtr)
            {
                auto KObj = V->AsObject();
                if (!KObj.IsValid()) continue;
                FFrameNumber Frame = (KObj->GetNumberField(TEXT("time")) * TickRes).RoundToFrame();
                Channel.AddCubicKey(Frame, (float)KObj->GetNumberField(TEXT("value")));
                UpdateRange(Frame);
            }
        }
        else if (bIsColor)
        {
            UMovieSceneTrack* Track = MS->FindTrack(UMovieSceneColorTrack::StaticClass(), WidgetGuid, FName(*PropertyName));
            if (!Track)
            {
                Track = MS->AddTrack(UMovieSceneColorTrack::StaticClass(), WidgetGuid);
                Cast<UMovieSceneColorTrack>(Track)->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
            }
            Track->Modify();
            bool bSectionAdded = false;
            UMovieSceneSection* Sec = Cast<UMovieSceneColorTrack>(Track)->FindOrAddSection(0, bSectionAdded);
            Sec->SetRange(TRange<FFrameNumber>::All());
            auto* ColorSec = Cast<UMovieSceneColorSection>(Sec);
            for (const auto& V : *KeysPtr)
            {
                auto KObj = V->AsObject();
                const TSharedPtr<FJsonObject>* VObjField = nullptr;
                if (!KObj.IsValid() || !KObj->TryGetObjectField(TEXT("value"), VObjField) || !VObjField) continue;
                const TSharedPtr<FJsonObject>& VObj = *VObjField;
                FFrameNumber Frame = (KObj->GetNumberField(TEXT("time")) * TickRes).RoundToFrame();
                ColorSec->GetRedChannel()  .AddLinearKey(Frame, (float)VObj->GetNumberField(TEXT("r")));
                ColorSec->GetGreenChannel().AddLinearKey(Frame, (float)VObj->GetNumberField(TEXT("g")));
                ColorSec->GetBlueChannel() .AddLinearKey(Frame, (float)VObj->GetNumberField(TEXT("b")));
                ColorSec->GetAlphaChannel().AddLinearKey(Frame, (float)VObj->GetNumberField(TEXT("a")));
                UpdateRange(Frame);
            }
        }
        // UE 5.7: MovieSceneDoubleVectorTrack removed — use two separate float tracks for X and Y
        else if (bIsVector2D)
        {
            for (const FString& Axis : { FString(TEXT("X")), FString(TEXT("Y")) })
            {
                FString SubPropName = PropertyName + TEXT(".") + Axis;
                UMovieSceneTrack* Track = MS->FindTrack(UMovieSceneFloatTrack::StaticClass(), WidgetGuid, FName(*SubPropName));
                if (!Track)
                {
                    Track = MS->AddTrack(UMovieSceneFloatTrack::StaticClass(), WidgetGuid);
                    Cast<UMovieSceneFloatTrack>(Track)->SetPropertyNameAndPath(FName(*SubPropName), SubPropName);
                }
                Track->Modify();
                bool bSecAdded = false;
                UMovieSceneSection* Sec = Cast<UMovieSceneFloatTrack>(Track)->FindOrAddSection(0, bSecAdded);
                Sec->SetRange(TRange<FFrameNumber>::All());
                auto& Channel = Cast<UMovieSceneFloatSection>(Sec)->GetChannel();
                for (const auto& V : *KeysPtr)
                {
                    auto KObj = V->AsObject();
                    const TSharedPtr<FJsonObject>* VObjField = nullptr;
                    if (!KObj.IsValid() || !KObj->TryGetObjectField(TEXT("value"), VObjField) || !VObjField) continue;
                    const TSharedPtr<FJsonObject>& VObj = *VObjField;
                    FFrameNumber Frame = (KObj->GetNumberField(TEXT("time")) * TickRes).RoundToFrame();
                    float AxisVal = (float)VObj->GetNumberField(*Axis.ToLower());
                    Channel.AddCubicKey(Frame, AxisVal);
                    UpdateRange(Frame);
                }
            }
        }

        if (bRangeInit)
            MS->SetPlaybackRange(TRange<FFrameNumber>(RangeStart, RangeEnd));

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

        if (GEditor)
        {
            if (UAssetEditorSubsystem* AESub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
                AESub->OpenEditorForAsset(WBP);
        }

        Result.bSuccess     = true;
        Result.AffectedPath = WBP->GetPathName();
        Result.Message      = FString::Printf(TEXT("Set %d keyframes on '%s.%s' in '%s'"),
            KeysPtr->Num(), *WidgetName, *PropertyName, *AnimName);
        return Result;
    }

    if (Action == TEXT("remove_property_track"))
    {
        FString AnimName, WidgetName, PropertyName;
        Params->TryGetStringField(TEXT("animation_name"), AnimName);
        Params->TryGetStringField(TEXT("widget_name"),    WidgetName);
        Params->TryGetStringField(TEXT("property_name"),  PropertyName);
        if (AnimName.IsEmpty() && Subsystem && Subsystem->AttentionManager) AnimName   = Subsystem->AttentionManager->GetAnimationScope();
        if (WidgetName.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        {
            WidgetName = Subsystem->AttentionManager->GetActiveWidget();
            if (WidgetName.IsEmpty())
                WidgetName = Subsystem->AttentionManager->GetWidgetScope();
        }

        UWidgetAnimation* Anim = FindAnimation(WBP, AnimName);
        if (!Anim) { Result.Message = FString::Printf(TEXT("Animation not found: %s"), *AnimName); Result.ErrorCode = 2000; return Result; }

        UMovieScene* MS = Anim->GetMovieScene();
        FGuid WidgetGuid;
        for (int32 i = 0; i < MS->GetPossessableCount(); ++i)
            if (MS->GetPossessable(i).GetName() == WidgetName) { WidgetGuid = MS->GetPossessable(i).GetGuid(); break; }

        if (!WidgetGuid.IsValid()) { Result.Message = FString::Printf(TEXT("Widget binding not found: %s"), *WidgetName); Result.ErrorCode = 2000; return Result; }

        MS->Modify();
        bool bFound = false;
        for (auto TrackClass : TArray<TSubclassOf<UMovieSceneTrack>>{
            UMovieSceneFloatTrack::StaticClass(),
            UMovieSceneColorTrack::StaticClass()
            /* UMovieSceneDoubleVectorTrack::StaticClass() */  // UE 5.7: MovieSceneDoubleVectorTrack removed
            })
        {
            UMovieSceneTrack* Track = MS->FindTrack(TrackClass, WidgetGuid, FName(*PropertyName));
            if (Track) { MS->RemoveTrack(*Track); bFound = true; }
        }

        Result.bSuccess = bFound;
        Result.Message  = bFound
            ? FString::Printf(TEXT("Removed track '%s' from '%s'"), *PropertyName, *AnimName)
            : FString::Printf(TEXT("Track '%s' not found"), *PropertyName);
        if (!bFound) Result.ErrorCode = 2000;
        return Result;
    }

    if (Action == TEXT("read_animation_capture"))
    {
        if (Subsystem && Subsystem->AnimationCapture)
            Subsystem->AnimationCapture->ExportAnimationData();
        FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("animation/skeletal_data.json"));
        FString FileContent;
        FBridgeResult Res = MakeSuccess(GetDomainName(), Action, TEXT("Capture complete: ") + FilePath);
        if (FFileHelper::LoadFileToString(FileContent, *FilePath))
        {
            TSharedPtr<FJsonObject> JsonObj;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
            if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
                Res.Data = JsonObj;
        }
        return Res;
    }

    // Phase 1d: skeletal animation asset management (no WBP required)
    if (Action == TEXT("set_motion_matching_db"))  return Action_SetMotionMatchingDB(Params);
    if (Action == TEXT("set_control_rig_link"))    return Action_SetControlRigLink(Params);
    if (Action == TEXT("create_selection_set"))    return Action_CreateSelectionSet(Params);
    if (Action == TEXT("set_retarget_lod"))        return Action_SetRetargetLOD(Params);

    Result.Message = FString::Printf(
        TEXT("Unknown animation action '%s'. Supported: create_animation, delete_animation, "
             "set_animation_scope, set_widget_scope, set_property_keys, remove_property_track, "
             "get_all_animations, get_animation_keyframes, get_animated_widgets, read_animation_capture, "
             "set_motion_matching_db, set_control_rig_link, create_selection_set, set_retarget_lod"),
        *Action);
    Result.ErrorCode = 1001;
    return Result;
}

// ===========================================================================
// Phase 1d: Skeletal Animation Asset Management
// ===========================================================================

// ---------------------------------------------------------------------------
// set_motion_matching_db
// ---------------------------------------------------------------------------
// Loads an AnimBlueprint and attempts to find UAnimGraphNode_MotionMatching
// in the anim graph, then sets its Database property via FProperty reflection.
// PARTIALLY_FEASIBLE if the PoseSearch plugin is not present (error 3003).
// ---------------------------------------------------------------------------

FBridgeResult UAnimationHandler::Action_SetMotionMatchingDB(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_motion_matching_db");

    FString AnimBPPath, DatabasePath;
    if (!Params->TryGetStringField(TEXT("animbp_path"), AnimBPPath) || AnimBPPath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'animbp_path'"));
    if (!Params->TryGetStringField(TEXT("database_path"), DatabasePath) || DatabasePath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'database_path'"));

#if WITH_EDITOR
    // Load the AnimBlueprint with suffix fallback
    UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
    if (!AnimBP)
    {
        const FString Suffix = AnimBPPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AnimBPPath);
        AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
    }
    if (!AnimBP)
        return MakeError(TEXT("animation"), Action, 2000,
            FString::Printf(TEXT("Could not load AnimBlueprint at '%s'"), *AnimBPPath));

    // Resolve the database asset (type-agnostic -- UPoseSearchDatabase, UAnimationDatabase, etc.)
    UObject* DatabaseAsset = LoadObject<UObject>(nullptr, *DatabasePath);
    if (!DatabaseAsset)
    {
        const FString Suffix = DatabasePath + TEXT(".") + FPackageName::GetLongPackageAssetName(DatabasePath);
        DatabaseAsset = LoadObject<UObject>(nullptr, *Suffix);
    }
    if (!DatabaseAsset)
        return MakeError(TEXT("animation"), Action, 2000,
            FString::Printf(TEXT("Could not load database asset at '%s'"), *DatabasePath));

    // Find UAnimGraphNode_MotionMatching in any anim graph via class name (plugin-safe)
    UClass* MMNodeClass = FindFirstObject<UClass>(TEXT("AnimGraphNode_MotionMatching"), EFindFirstObjectOptions::NativeFirst);
    if (!MMNodeClass)
    {
        // PoseSearch plugin not loaded or class not available
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("animbp_path"),   AnimBPPath);
        Data->SetStringField(TEXT("database_path"), DatabasePath);
        FBridgeResult R = CreateResult(TEXT("animation"), Action);
        R.bSuccess  = false;
        R.ErrorCode = 3003;
        R.Message   = TEXT("set_motion_matching_db: PARTIALLY_FEASIBLE -- "
                           "AnimGraphNode_MotionMatching class not found. "
                           "Enable the PoseSearch plugin and recompile.");
        R.Data = Data;
        return R;
    }

    bool bNodeFound = false;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->IsA(MMNodeClass))
            {
                // Set Database property via FProperty reflection
                FObjectProperty* DBProp = FindFProperty<FObjectProperty>(MMNodeClass, TEXT("Database"));
                if (DBProp)
                {
                    Node->Modify();
                    DBProp->SetObjectPropertyValue_InContainer(Node, DatabaseAsset);
                    bNodeFound = true;
                }
                break;
            }
        }
        if (bNodeFound) break;
    }
    // Also search AnimationGraphs (UAnimBlueprint has both FunctionGraphs and AnimationGraphs)
    if (!bNodeFound)
    {
        for (UEdGraph* Graph : AnimBP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->IsA(MMNodeClass))
                {
                    FObjectProperty* DBProp = FindFProperty<FObjectProperty>(MMNodeClass, TEXT("Database"));
                    if (DBProp)
                    {
                        Node->Modify();
                        DBProp->SetObjectPropertyValue_InContainer(Node, DatabaseAsset);
                        bNodeFound = true;
                    }
                    break;
                }
            }
            if (bNodeFound) break;
        }
    }

    if (!bNodeFound)
        return MakeError(TEXT("animation"), Action, 3000,
            FString::Printf(TEXT("AnimGraphNode_MotionMatching node not found in '%s'"), *AnimBPPath),
            TEXT("Add a MotionMatching node to the AnimBlueprint's anim graph first."));

    AnimBP->MarkPackageDirty();
    FKismetEditorUtilities::CompileBlueprint(AnimBP);

    FBridgeResult R = MakeSuccess(TEXT("animation"), Action,
        FString::Printf(TEXT("set_motion_matching_db: Database '%s' set on AnimBP '%s'"),
            *DatabasePath, *AnimBPPath));
    R.AffectedPath = AnimBPPath;
    return R;
#else
    return MakeError(TEXT("animation"), Action, 3003,
        TEXT("set_motion_matching_db requires WITH_EDITOR"));
#endif
}

// ---------------------------------------------------------------------------
// set_control_rig_link
// ---------------------------------------------------------------------------
// Loads an AnimBlueprint and sets the ControlRigClass on UAnimGraphNode_ControlRig
// via FProperty reflection. PARTIALLY_FEASIBLE if ControlRig plugin not present.
// ---------------------------------------------------------------------------

FBridgeResult UAnimationHandler::Action_SetControlRigLink(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_control_rig_link");

    FString AnimBPPath, ControlRigPath;
    if (!Params->TryGetStringField(TEXT("animbp_path"),     AnimBPPath)     || AnimBPPath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'animbp_path'"));
    if (!Params->TryGetStringField(TEXT("control_rig_path"), ControlRigPath) || ControlRigPath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'control_rig_path'"));

#if WITH_EDITOR
    UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
    if (!AnimBP)
    {
        const FString Suffix = AnimBPPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AnimBPPath);
        AnimBP = LoadObject<UAnimBlueprint>(nullptr, *Suffix);
    }
    if (!AnimBP)
        return MakeError(TEXT("animation"), Action, 2000,
            FString::Printf(TEXT("Could not load AnimBlueprint at '%s'"), *AnimBPPath));

    // Load the ControlRig Blueprint -- the class object is what we need
    UBlueprint* CRBlueprint = LoadObject<UBlueprint>(nullptr, *ControlRigPath);
    if (!CRBlueprint)
    {
        const FString Suffix = ControlRigPath + TEXT(".") + FPackageName::GetLongPackageAssetName(ControlRigPath);
        CRBlueprint = LoadObject<UBlueprint>(nullptr, *Suffix);
    }
    if (!CRBlueprint)
        return MakeError(TEXT("animation"), Action, 2000,
            FString::Printf(TEXT("Could not load ControlRig Blueprint at '%s'"), *ControlRigPath));

    UClass* CRNodeClass = FindFirstObject<UClass>(TEXT("AnimGraphNode_ControlRig"), EFindFirstObjectOptions::NativeFirst);
    if (!CRNodeClass)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("animbp_path"),      AnimBPPath);
        Data->SetStringField(TEXT("control_rig_path"), ControlRigPath);
        FBridgeResult R = CreateResult(TEXT("animation"), Action);
        R.bSuccess  = false;
        R.ErrorCode = 3003;
        R.Message   = TEXT("set_control_rig_link: PARTIALLY_FEASIBLE -- "
                           "AnimGraphNode_ControlRig class not found. "
                           "Enable the ControlRig plugin and recompile.");
        R.Data = Data;
        return R;
    }

    // Search FunctionGraphs and UbergraphPages for the ControlRig node
    bool bNodeFound = false;
    TArray<UEdGraph*> AllGraphs;
    AllGraphs.Append(AnimBP->FunctionGraphs);
    AllGraphs.Append(AnimBP->UbergraphPages);

    for (UEdGraph* Graph : AllGraphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->IsA(CRNodeClass))
            {
                // Set ControlRigClass via reflection
                FClassProperty* ClassProp = FindFProperty<FClassProperty>(CRNodeClass, TEXT("ControlRigClass"));
                if (ClassProp)
                {
                    Node->Modify();
                    ClassProp->SetPropertyValue_InContainer(Node, CRBlueprint->GeneratedClass);
                    bNodeFound = true;
                }
                break;
            }
        }
        if (bNodeFound) break;
    }

    if (!bNodeFound)
        return MakeError(TEXT("animation"), Action, 3000,
            FString::Printf(TEXT("AnimGraphNode_ControlRig node not found in '%s'"), *AnimBPPath),
            TEXT("Add a ControlRig node to the AnimBlueprint's anim graph first."));

    AnimBP->MarkPackageDirty();
    FKismetEditorUtilities::CompileBlueprint(AnimBP);

    FBridgeResult R = MakeSuccess(TEXT("animation"), Action,
        FString::Printf(TEXT("set_control_rig_link: ControlRig '%s' set on AnimBP '%s'"),
            *ControlRigPath, *AnimBPPath));
    R.AffectedPath = AnimBPPath;
    return R;
#else
    return MakeError(TEXT("animation"), Action, 3003,
        TEXT("set_control_rig_link requires WITH_EDITOR"));
#endif
}

// ---------------------------------------------------------------------------
// create_selection_set
// ---------------------------------------------------------------------------
// UE 5.7 Selection Sets. Attempts to create a UAnimationSelectionSet asset.
// PARTIALLY_FEASIBLE if the class is not available (pre-5.7 builds).
// ---------------------------------------------------------------------------

FBridgeResult UAnimationHandler::Action_CreateSelectionSet(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("create_selection_set");

    FString AssetPath, SetName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'asset_path'"));
    if (!Params->TryGetStringField(TEXT("name"), SetName) || SetName.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'name'"));

    const TArray<TSharedPtr<FJsonValue>>* BonesPtr = nullptr;
    Params->TryGetArrayField(TEXT("bones"), BonesPtr);

#if WITH_EDITOR
    // Try to find UAnimationSelectionSet class (UE 5.7+)
    // Try multiple possible paths for the class
    UClass* SelectionSetClass = FindFirstObject<UClass>(TEXT("AnimationSelectionSet"), EFindFirstObjectOptions::NativeFirst);
    if (!SelectionSetClass)
        SelectionSetClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimationSelectionSet"));
    if (!SelectionSetClass)
        SelectionSetClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.SkeletalMeshSelectionSet"));

    if (!SelectionSetClass)
    {
        // Class not available -- return partial feasibility descriptor
        TArray<FString> BoneList;
        if (BonesPtr)
            for (const TSharedPtr<FJsonValue>& V : *BonesPtr)
                BoneList.Add(V->AsString());

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_path"), AssetPath);
        Data->SetStringField(TEXT("name"),       SetName);
        TArray<TSharedPtr<FJsonValue>> BoneArr;
        for (const FString& B : BoneList)
            BoneArr.Add(MakeShared<FJsonValueString>(B));
        Data->SetArrayField(TEXT("bones"), BoneArr);
        Data->SetStringField(TEXT("note"),
            TEXT("AnimationSelectionSet / SkeletalMeshSelectionSet class not found. "
                 "Requires UE 5.7+ with the skeletal mesh editor module loaded."));

        FBridgeResult R = CreateResult(TEXT("animation"), Action);
        R.bSuccess  = false;
        R.ErrorCode = 3003;
        R.Message   = TEXT("create_selection_set: PARTIALLY_FEASIBLE -- "
                           "AnimationSelectionSet class not available. "
                           "Requires UE 5.7+ and the skeletal mesh editor.");
        R.Data = Data;
        return R;
    }

    // Derive package path from asset_path
    FString PackageName = AssetPath;
    // Strip existing asset name suffix if present
    int32 DotIdx;
    if (PackageName.FindLastChar(TEXT('.'), DotIdx))
        PackageName = PackageName.Left(DotIdx);

    UPackage* Pkg = CreatePackage(*PackageName);
    if (!Pkg)
        return MakeError(TEXT("animation"), Action, 3000,
            FString::Printf(TEXT("Could not create package at '%s'"), *PackageName));

    UObject* NewAsset = NewObject<UObject>(Pkg, SelectionSetClass,
        *FPackageName::GetLongPackageAssetName(PackageName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!NewAsset)
        return MakeError(TEXT("animation"), Action, 3001,
            TEXT("Failed to create SelectionSet asset object"));

    // Set Name via reflection if property exists
    FNameProperty* NameProp = FindFProperty<FNameProperty>(SelectionSetClass, TEXT("SetName"));
    if (!NameProp)
        NameProp = FindFProperty<FNameProperty>(SelectionSetClass, TEXT("Name"));
    if (NameProp)
        NameProp->SetPropertyValue_InContainer(NewAsset, FName(*SetName));

    // Set Bones array via reflection if property exists
    if (BonesPtr)
    {
        FArrayProperty* BonesProp = FindFProperty<FArrayProperty>(SelectionSetClass, TEXT("Bones"));
        if (!BonesProp)
            BonesProp = FindFProperty<FArrayProperty>(SelectionSetClass, TEXT("BoneNames"));
        if (BonesProp)
        {
            FScriptArrayHelper ArrayHelper(BonesProp, BonesProp->ContainerPtrToValuePtr<void>(NewAsset));
            ArrayHelper.Resize(BonesPtr->Num());
            FNameProperty* ElemProp = CastField<FNameProperty>(BonesProp->Inner);
            if (ElemProp)
            {
                for (int32 i = 0; i < BonesPtr->Num(); ++i)
                    ElemProp->SetPropertyValue(ArrayHelper.GetRawPtr(i),
                        FName(*(*BonesPtr)[i]->AsString()));
            }
        }
    }

    Pkg->MarkPackageDirty();

    FBridgeResult R = MakeSuccess(TEXT("animation"), Action,
        FString::Printf(TEXT("create_selection_set: created '%s' at '%s' with %d bone(s)"),
            *SetName, *AssetPath,
            BonesPtr ? BonesPtr->Num() : 0));
    R.AffectedPath = AssetPath;
    return R;
#else
    return MakeError(TEXT("animation"), Action, 3003,
        TEXT("create_selection_set requires WITH_EDITOR"));
#endif
}

// ---------------------------------------------------------------------------
// set_retarget_lod
// ---------------------------------------------------------------------------
// Sets MinLod on USkeletalMesh, or SourceMeshLOD/TargetMeshLOD on UIKRetargeter
// depending on which asset type the path resolves to.
// ---------------------------------------------------------------------------

FBridgeResult UAnimationHandler::Action_SetRetargetLOD(TSharedPtr<FJsonObject> Params)
{
    const FString Action = TEXT("set_retarget_lod");

    FString AssetPath;
    double LodIndexD = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'asset_path'"));
    if (!Params->TryGetNumberField(TEXT("lod_index"), LodIndexD))
        return MakeError(TEXT("animation"), Action, 1000,
            TEXT("Missing required param: 'lod_index' (int)"));
    const int32 LodIndex = (int32)LodIndexD;

#if WITH_EDITOR
    // Try loading as USkeletalMesh first
    USkeletalMesh* SkelMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
    if (!SkelMesh)
    {
        const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        SkelMesh = LoadObject<USkeletalMesh>(nullptr, *Suffix);
    }

    if (SkelMesh)
    {
        // Set minimum LOD — UE 5.7 replaces direct MinLod field access with accessors.
        FPerPlatformInt NewMinLod = SkelMesh->GetMinLod();
        NewMinLod.Default = LodIndex;
        SkelMesh->SetMinLod(NewMinLod);
        SkelMesh->MarkPackageDirty();

        FBridgeResult R = MakeSuccess(TEXT("animation"), Action,
            FString::Printf(TEXT("set_retarget_lod: SkeletalMesh '%s' MinLod set to %d"),
                *AssetPath, LodIndex));
        R.AffectedPath = AssetPath;
        return R;
    }

    // Try loading as UIKRetargeter via class name lookup (guarded for plugin availability)
    UClass* RetargeterClass = FindFirstObject<UClass>(TEXT("IKRetargeter"), EFindFirstObjectOptions::NativeFirst);
    if (!RetargeterClass)
        RetargeterClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRetargeter"));

    if (RetargeterClass)
    {
        UObject* RetargeterObj = LoadObject<UObject>(nullptr, *AssetPath);
        if (!RetargeterObj)
        {
            const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
            RetargeterObj = LoadObject<UObject>(nullptr, *Suffix);
        }

        if (RetargeterObj && RetargeterObj->IsA(RetargeterClass))
        {
            // Set SourceMeshLOD and TargetMeshLOD via reflection
            FIntProperty* SrcProp = FindFProperty<FIntProperty>(RetargeterClass, TEXT("SourceMeshLOD"));
            FIntProperty* TgtProp = FindFProperty<FIntProperty>(RetargeterClass, TEXT("TargetMeshLOD"));
            if (SrcProp) SrcProp->SetPropertyValue_InContainer(RetargeterObj, LodIndex);
            if (TgtProp) TgtProp->SetPropertyValue_InContainer(RetargeterObj, LodIndex);

            RetargeterObj->MarkPackageDirty();

            FBridgeResult R = MakeSuccess(TEXT("animation"), Action,
                FString::Printf(TEXT("set_retarget_lod: IKRetargeter '%s' SourceMeshLOD/TargetMeshLOD set to %d"),
                    *AssetPath, LodIndex));
            R.AffectedPath = AssetPath;
            return R;
        }
    }

    // Neither type resolved
    return MakeError(TEXT("animation"), Action, 2000,
        FString::Printf(TEXT("set_retarget_lod: could not load asset as USkeletalMesh or UIKRetargeter at '%s'"),
            *AssetPath),
        TEXT("Provide a valid USkeletalMesh or IKRetargeter asset path. "
             "IKRetargeter requires the IKRig plugin."));
#else
    return MakeError(TEXT("animation"), Action, 3003,
        TEXT("set_retarget_lod requires WITH_EDITOR"));
#endif
}
