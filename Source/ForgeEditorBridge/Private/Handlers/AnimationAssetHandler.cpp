#include "Handlers/AnimationAssetHandler.h"
#include "ForgeAISubsystem.h"

// ---- Animation types -------------------------------------------------------
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Misc/PackageName.h"

// ---- Asset creation (editor-only) ------------------------------------------
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/AnimMontageFactory.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
		return MakeError(TEXT("animation_asset"), Action, 1000, TEXT("Params object is null"), TEXT("Provide a valid JSON params object."));

	if (Action == TEXT("create_montage"))        return Action_CreateMontage(Params);
	if (Action == TEXT("add_notify"))            return Action_AddNotify(Params);
	if (Action == TEXT("create_blendspace"))     return Action_CreateBlendSpace(Params);
	if (Action == TEXT("add_blendspace_sample")) return Action_AddBlendSpaceSample(Params);
	if (Action == TEXT("get_sequence_info"))     return Action_GetSequenceInfo(Params);
	if (Action == TEXT("get_montage_sections"))  return Action_GetMontageSections(Params);
	if (Action == TEXT("remove_notify"))         return Action_RemoveNotify(Params);
	if (Action == TEXT("remove_blendspace_sample")) return Action_RemoveBlendSpaceSample(Params);

	return MakeError(TEXT("animation_asset"), Action, 1001,
		FString::Printf(TEXT("Unknown animation_asset action '%s'. Valid: create_montage, add_notify, create_blendspace, add_blendspace_sample, get_sequence_info, get_montage_sections, remove_notify, remove_blendspace_sample"), *Action),
		TEXT("Check action spelling against GetSupportedActions()."));
}

// ---------------------------------------------------------------------------
// create_montage
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_CreateMontage(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("create_montage");

#if WITH_EDITOR
	FString AssetPath, SkeletonPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("create_montage: 'asset_path' and 'skeleton' are required"),
			TEXT("Provide asset_path (destination content path) and skeleton (content path to USkeleton)."));
	}

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		const FString Suffix = SkeletonPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SkeletonPath);
		Skeleton = LoadObject<USkeleton>(nullptr, *Suffix);
	}
	if (!Skeleton)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("Skeleton not found: '%s'"), *SkeletonPath),
			TEXT("Verify the skeleton content path."));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->TargetSkeleton = Skeleton;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* CreatedAsset = AssetTools.CreateAsset(AssetName, PackagePath, UAnimMontage::StaticClass(), Factory);

	if (!CreatedAsset)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 3000,
			FString::Printf(TEXT("Failed to create montage at '%s'"), *AssetPath),
			TEXT("Check UE log for factory errors. Verify the destination path is valid."));
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(CreatedAsset);
	if (!Montage)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2001,
			TEXT("Created asset is not a UAnimMontage"),
			TEXT("Internal error — factory produced unexpected type."));
	}

	// If source_sequence provided, add it as a segment
	FString SourceSequencePath;
	if (Params->TryGetStringField(TEXT("source_sequence"), SourceSequencePath) && !SourceSequencePath.IsEmpty())
	{
		UAnimSequence* SourceSeq = LoadObject<UAnimSequence>(nullptr, *SourceSequencePath);
		if (!SourceSeq)
		{
			const FString Suffix = SourceSequencePath + TEXT(".") + FPackageName::GetLongPackageAssetName(SourceSequencePath);
			SourceSeq = LoadObject<UAnimSequence>(nullptr, *Suffix);
		}
		if (SourceSeq)
		{
			// Ensure at least one slot track exists
			if (Montage->SlotAnimTracks.Num() == 0)
			{
				FSlotAnimationTrack& NewTrack = Montage->SlotAnimTracks.AddDefaulted_GetRef();
				NewTrack.SlotName = FName(TEXT("DefaultSlot"));
			}

			FAnimSegment Segment;
			Segment.SetAnimReference(SourceSeq);
			Segment.AnimStartTime = 0.0f;
			Segment.AnimEndTime = SourceSeq->GetPlayLength();
			Segment.AnimPlayRate = 1.0f;
			Segment.StartPos = 0.0f;
			Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);

			Montage->PostEditChange();
		}
	}

	Montage->MarkPackageDirty();

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("Montage created at '%s'"), *CreatedAsset->GetPathName()));
#else
	return MakeError(TEXT("animation_asset"), ActionName, 3000,
		TEXT("create_montage requires WITH_EDITOR"),
		TEXT("This action is only available in editor builds."));
#endif
}

// ---------------------------------------------------------------------------
// add_notify
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_AddNotify(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("add_notify");

	FString AssetPath, NotifyClassName, TrackName;
	double Time = 0.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("notify_class"), NotifyClassName) || NotifyClassName.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("time"), Time) ||
	    !Params->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("add_notify: 'asset_path', 'notify_class', 'time', and 'track_name' are required"),
			TEXT("Provide all four parameters."));
	}

	UAnimSequenceBase* AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AssetPath);
	if (!AnimAsset)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *Suffix);
	}
	if (!AnimAsset)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("No UAnimSequenceBase found at '%s'"), *AssetPath),
			TEXT("Verify the content path points to an AnimSequence or AnimMontage."));
	}

	// Find the notify class
	FString FullClassName = NotifyClassName;
	if (!FullClassName.StartsWith(TEXT("AnimNotify_")))
	{
		FullClassName = FString::Printf(TEXT("AnimNotify_%s"), *NotifyClassName);
	}

	UClass* NotifyClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::ExactClass);
	if (!NotifyClass)
	{
		NotifyClass = FindFirstObject<UClass>(*NotifyClassName, EFindFirstObjectOptions::ExactClass);
	}
	if (!NotifyClass)
	{
		// Try StaticFindObject as fallback
		NotifyClass = StaticLoadClass(UAnimNotify::StaticClass(), nullptr, *FullClassName);
	}
	if (!NotifyClass)
	{
		NotifyClass = StaticLoadClass(UAnimNotify::StaticClass(), nullptr, *NotifyClassName);
	}
	if (!NotifyClass || !NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("Notify class '%s' not found or not a UAnimNotify subclass"), *NotifyClassName),
			TEXT("Use the full class name (e.g., 'AnimNotify_PlaySound') or a valid notify class path."));
	}

	UAnimNotify* NewNotify = NewObject<UAnimNotify>(AnimAsset, NotifyClass);

	FAnimNotifyEvent NewEvent;
	NewEvent.Notify = NewNotify;
	NewEvent.NotifyName = FName(*FullClassName);
	NewEvent.SetTime((float)Time);
	NewEvent.TrackIndex = 0; // Will be resolved below

	// Find or create the track
	int32 TrackIdx = INDEX_NONE;
	for (int32 i = 0; i < AnimAsset->AnimNotifyTracks.Num(); ++i)
	{
		if (AnimAsset->AnimNotifyTracks[i].TrackName == FName(*TrackName))
		{
			TrackIdx = i;
			break;
		}
	}
	if (TrackIdx == INDEX_NONE)
	{
		FAnimNotifyTrack NewTrack;
		NewTrack.TrackName = FName(*TrackName);
		TrackIdx = AnimAsset->AnimNotifyTracks.Add(NewTrack);
	}
	NewEvent.TrackIndex = TrackIdx;

	AnimAsset->Notifies.Add(NewEvent);
	AnimAsset->PostEditChange();
	AnimAsset->MarkPackageDirty();

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("Notify '%s' added at time %.3f on track '%s' in '%s'"),
			*FullClassName, (float)Time, *TrackName, *AssetPath));
}

// ---------------------------------------------------------------------------
// create_blendspace
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_CreateBlendSpace(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("create_blendspace");

#if WITH_EDITOR
	FString AssetPath, SkeletonPath, AxisX, AxisY;
	double MinX = -100.0, MaxX = 100.0, MinY = -100.0, MaxY = 100.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("axis_x"), AxisX) || AxisX.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("axis_y"), AxisY) || AxisY.IsEmpty())
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("create_blendspace: 'asset_path', 'skeleton', 'axis_x', and 'axis_y' are required"),
			TEXT("Provide all required parameters. min_x, max_x, min_y, max_y are optional (default +/-100)."));
	}

	Params->TryGetNumberField(TEXT("min_x"), MinX);
	Params->TryGetNumberField(TEXT("max_x"), MaxX);
	Params->TryGetNumberField(TEXT("min_y"), MinY);
	Params->TryGetNumberField(TEXT("max_y"), MaxY);

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		const FString Suffix = SkeletonPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SkeletonPath);
		Skeleton = LoadObject<USkeleton>(nullptr, *Suffix);
	}
	if (!Skeleton)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("Skeleton not found: '%s'"), *SkeletonPath),
			TEXT("Verify the skeleton content path."));
	}

	const FString AssetName   = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* CreatedAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlendSpace::StaticClass(), nullptr);

	if (!CreatedAsset)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 3000,
			FString::Printf(TEXT("Failed to create blendspace at '%s'"), *AssetPath),
			TEXT("Check UE log for creation errors."));
	}

	UBlendSpace* BlendSpace = Cast<UBlendSpace>(CreatedAsset);
	if (!BlendSpace)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2001,
			TEXT("Created asset is not a UBlendSpace"),
			TEXT("Internal error — unexpected asset type."));
	}

	BlendSpace->SetSkeleton(Skeleton);

	// Configure axis parameters via reflection (UE 5.7: GetBlendParameter returns const)
	{
		FProperty* BlendParamsProp = BlendSpace->GetClass()->FindPropertyByName(TEXT("BlendParameters"));
		if (BlendParamsProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(BlendParamsProp);
			if (ArrayProp)
			{
				void* ArrayPtr = BlendParamsProp->ContainerPtrToValuePtr<void>(BlendSpace);
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
				if (ArrayHelper.Num() > 0)
				{
					FBlendParameter* ParamX = (FBlendParameter*)ArrayHelper.GetRawPtr(0);
					ParamX->DisplayName = AxisX;
					ParamX->Min = (float)MinX;
					ParamX->Max = (float)MaxX;
				}
				if (ArrayHelper.Num() > 1)
				{
					FBlendParameter* ParamY = (FBlendParameter*)ArrayHelper.GetRawPtr(1);
					ParamY->DisplayName = AxisY;
					ParamY->Min = (float)MinY;
					ParamY->Max = (float)MaxY;
				}
			}
		}
	}

	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), CreatedAsset->GetPathName());
	Data->SetStringField(TEXT("axis_x"), AxisX);
	Data->SetStringField(TEXT("axis_y"), AxisY);

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("BlendSpace created at '%s' (axes: %s / %s)"), *CreatedAsset->GetPathName(), *AxisX, *AxisY), Data);
#else
	return MakeError(TEXT("animation_asset"), ActionName, 3000,
		TEXT("create_blendspace requires WITH_EDITOR"),
		TEXT("This action is only available in editor builds."));
#endif
}

// ---------------------------------------------------------------------------
// add_blendspace_sample
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_AddBlendSpaceSample(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("add_blendspace_sample");

	FString AssetPath, SequencePath;
	double X = 0.0, Y = 0.0;

	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
	    !Params->TryGetStringField(TEXT("sequence"), SequencePath) || SequencePath.IsEmpty() ||
	    !Params->TryGetNumberField(TEXT("x"), X) ||
	    !Params->TryGetNumberField(TEXT("y"), Y))
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("add_blendspace_sample: 'asset_path', 'sequence', 'x', and 'y' are required"),
			TEXT("Provide all four parameters."));
	}

	UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *AssetPath);
	if (!BlendSpace)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BlendSpace = LoadObject<UBlendSpace>(nullptr, *Suffix);
	}
	if (!BlendSpace)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("No UBlendSpace found at '%s'"), *AssetPath),
			TEXT("Verify the content path points to a BlendSpace asset."));
	}

	UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *SequencePath);
	if (!Sequence)
	{
		const FString Suffix = SequencePath + TEXT(".") + FPackageName::GetLongPackageAssetName(SequencePath);
		Sequence = LoadObject<UAnimSequence>(nullptr, *Suffix);
	}
	if (!Sequence)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("No UAnimSequence found at '%s'"), *SequencePath),
			TEXT("Verify the sequence content path."));
	}

	const FVector SampleValue((float)X, (float)Y, 0.0f);
	BlendSpace->AddSample(Sequence, SampleValue);
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("Sample '%s' added at (%.2f, %.2f) to blendspace '%s'"),
			*SequencePath, (float)X, (float)Y, *AssetPath));
}

// ---------------------------------------------------------------------------
// get_sequence_info
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_GetSequenceInfo(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("get_sequence_info");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("get_sequence_info: 'asset_path' is required"),
			TEXT("Provide the content path to a UAnimSequence."));
	}

	UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	if (!Sequence)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Sequence = LoadObject<UAnimSequence>(nullptr, *Suffix);
	}
	if (!Sequence)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("No UAnimSequence found at '%s'"), *AssetPath),
			TEXT("Verify the content path points to an AnimSequence."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("play_length"), Sequence->GetPlayLength());

	const int32 FrameCount = Sequence->GetNumberOfSampledKeys();
	Data->SetNumberField(TEXT("frame_count"), FrameCount);

	const FFrameRate SamplingRate = Sequence->GetSamplingFrameRate();
	Data->SetStringField(TEXT("frame_rate"), FString::Printf(TEXT("%d/%d"), SamplingRate.Numerator, SamplingRate.Denominator));

	if (USkeleton* Skel = Sequence->GetSkeleton())
	{
		Data->SetStringField(TEXT("skeleton"), Skel->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("skeleton"), TEXT(""));
	}

	// UE 5.7: Curve count via DataModel
	int32 NumCurves = 0;
	if (const IAnimationDataModel* DataModel = Sequence->GetDataModel())
	{
		NumCurves = DataModel->GetNumberOfTransformCurves() + DataModel->GetNumberOfFloatCurves();
	}
	Data->SetNumberField(TEXT("num_curves"), (double)NumCurves);
	Data->SetNumberField(TEXT("num_notifies"), Sequence->Notifies.Num());
	Data->SetBoolField(TEXT("is_additive"), Sequence->IsValidAdditive());
	Data->SetBoolField(TEXT("has_root_motion"), Sequence->HasRootMotion());

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("Sequence info for '%s': %.2fs, %d frames"), *AssetPath, Sequence->GetPlayLength(), FrameCount), Data);
}

// ---------------------------------------------------------------------------
// get_montage_sections
// ---------------------------------------------------------------------------

FBridgeResult UAnimationAssetHandler::Action_GetMontageSections(TSharedPtr<FJsonObject> Params)
{
	const FString ActionName = TEXT("get_montage_sections");

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeError(TEXT("animation_asset"), ActionName, 1000,
			TEXT("get_montage_sections: 'asset_path' is required"),
			TEXT("Provide the content path to a UAnimMontage."));
	}

	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *AssetPath);
	if (!Montage)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Montage = LoadObject<UAnimMontage>(nullptr, *Suffix);
	}
	if (!Montage)
	{
		return MakeError(TEXT("animation_asset"), ActionName, 2000,
			FString::Printf(TEXT("No UAnimMontage found at '%s'"), *AssetPath),
			TEXT("Verify the content path points to an AnimMontage."));
	}

	TArray<TSharedPtr<FJsonValue>> SectionsArray;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
		SectionObj->SetNumberField(TEXT("time"), Section.GetTime());
		SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("sections"), SectionsArray);
	Data->SetNumberField(TEXT("section_count"), SectionsArray.Num());
	Data->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());

	return MakeSuccess(TEXT("animation_asset"), ActionName,
		FString::Printf(TEXT("Montage '%s': %d sections, %d slot tracks"), *AssetPath, SectionsArray.Num(), Montage->SlotAnimTracks.Num()), Data);
}

TSharedPtr<FJsonObject> UAnimationAssetHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create an AnimMontage asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new montage"))); Ps->SetObjectField(TEXT("skeleton"), P(TEXT("string"), true, TEXT("Content path of the USkeleton"))); Ps->SetObjectField(TEXT("source_sequence"), P(TEXT("string"), false, TEXT("Content path of a source AnimSequence to add as segment"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_montage"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add an anim notify to a sequence or montage")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the anim asset"))); Ps->SetObjectField(TEXT("notify_class"), P(TEXT("string"), true, TEXT("Notify class name (e.g. AnimNotify_PlaySound)"))); Ps->SetObjectField(TEXT("time"), P(TEXT("float"), true, TEXT("Time in seconds to place the notify"))); Ps->SetObjectField(TEXT("track_name"), P(TEXT("string"), true, TEXT("Notify track name"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_notify"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Create a BlendSpace asset")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new blendspace"))); Ps->SetObjectField(TEXT("skeleton"), P(TEXT("string"), true, TEXT("Content path of the USkeleton"))); Ps->SetObjectField(TEXT("axis_x"), P(TEXT("string"), true, TEXT("Display name for X axis"))); Ps->SetObjectField(TEXT("axis_y"), P(TEXT("string"), true, TEXT("Display name for Y axis"))); Ps->SetObjectField(TEXT("min_x"), P(TEXT("float"), false, TEXT("Minimum X value (default -100)"))); Ps->SetObjectField(TEXT("max_x"), P(TEXT("float"), false, TEXT("Maximum X value (default 100)"))); Ps->SetObjectField(TEXT("min_y"), P(TEXT("float"), false, TEXT("Minimum Y value (default -100)"))); Ps->SetObjectField(TEXT("max_y"), P(TEXT("float"), false, TEXT("Maximum Y value (default 100)"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("create_blendspace"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Add a sample animation to a BlendSpace")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the BlendSpace"))); Ps->SetObjectField(TEXT("sequence"), P(TEXT("string"), true, TEXT("Content path of the AnimSequence"))); Ps->SetObjectField(TEXT("x"), P(TEXT("float"), true, TEXT("X position in the blendspace"))); Ps->SetObjectField(TEXT("y"), P(TEXT("float"), true, TEXT("Y position in the blendspace"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("add_blendspace_sample"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get info about an AnimSequence (length, frames, curves)")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the AnimSequence"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_sequence_info"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("Get sections and slot tracks from an AnimMontage")); TSharedPtr<FJsonObject> Ps = MakeShared<FJsonObject>(); Ps->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the AnimMontage"))); A->SetObjectField(TEXT("params"), Ps); Root->SetObjectField(TEXT("get_montage_sections"), A); }

	return Root;
}

// ===========================================================================
// CRUD-symmetry: removal counterparts
// ===========================================================================

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"

FBridgeResult UAnimationAssetHandler::Action_RemoveNotify(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	int32 NotifyIndex = -1;
	double TimeMatch = -1.0;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("animation_asset"), TEXT("remove_notify"), 1000, TEXT("'asset_path' is required"));
	const bool bByIndex = Params->TryGetNumberField(TEXT("notify_index"), NotifyIndex) && NotifyIndex >= 0;
	const bool bByTime  = Params->TryGetNumberField(TEXT("time"), TimeMatch);
	if (!bByIndex && !bByTime)
		return MakeError(TEXT("animation_asset"), TEXT("remove_notify"), 1000,
			TEXT("'notify_index' (>=0) or 'time' (float) is required"));

	UAnimSequenceBase* Seq = LoadObject<UAnimSequenceBase>(nullptr, *AssetPath);
	if (!Seq)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		Seq = LoadObject<UAnimSequenceBase>(nullptr, *Suffix);
	}
	if (!Seq) return MakeError(TEXT("animation_asset"), TEXT("remove_notify"), 2000,
		FString::Printf(TEXT("Sequence not found: %s"), *AssetPath));

	int32 RemovedCount = 0;
	if (bByIndex)
	{
		if (!Seq->Notifies.IsValidIndex(NotifyIndex))
			return MakeError(TEXT("animation_asset"), TEXT("remove_notify"), 1001,
				FString::Printf(TEXT("notify_index %d out of range (have %d)"), NotifyIndex, Seq->Notifies.Num()));
		Seq->Notifies.RemoveAt(NotifyIndex);
		RemovedCount = 1;
	}
	else
	{
		const float Tol = 1e-3f;
		for (int32 i = Seq->Notifies.Num() - 1; i >= 0; --i)
		{
			if (FMath::IsNearlyEqual(Seq->Notifies[i].GetTime(), (float)TimeMatch, Tol))
			{
				Seq->Notifies.RemoveAt(i);
				++RemovedCount;
			}
		}
		if (RemovedCount == 0)
			return MakeError(TEXT("animation_asset"), TEXT("remove_notify"), 2000,
				FString::Printf(TEXT("No notify near t=%.4f"), TimeMatch));
	}

	Seq->RefreshCacheData();
	Seq->MarkPackageDirty();
	return MakeSuccess(TEXT("animation_asset"), TEXT("remove_notify"),
		FString::Printf(TEXT("Removed %d notify(s)"), RemovedCount));
}

FBridgeResult UAnimationAssetHandler::Action_RemoveBlendSpaceSample(TSharedPtr<FJsonObject> Params)
{
	FString AssetPath;
	int32 SampleIndex = -1;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeError(TEXT("animation_asset"), TEXT("remove_blendspace_sample"), 1000, TEXT("'asset_path' is required"));
	if (!Params->TryGetNumberField(TEXT("sample_index"), SampleIndex) || SampleIndex < 0)
		return MakeError(TEXT("animation_asset"), TEXT("remove_blendspace_sample"), 1000, TEXT("'sample_index' (>=0) is required"));

	UBlendSpace* BS = LoadObject<UBlendSpace>(nullptr, *AssetPath);
	if (!BS)
	{
		const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
		BS = LoadObject<UBlendSpace>(nullptr, *Suffix);
	}
	if (!BS) return MakeError(TEXT("animation_asset"), TEXT("remove_blendspace_sample"), 2000,
		FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	// 5.7: UBlendSpace does not expose a public RemoveSample. SampleData is private.
	// Reflection-based removal: walk the SampleData FArrayProperty and RemoveAt.
	FProperty* SampleDataProp = BS->GetClass()->FindPropertyByName(TEXT("SampleData"));
	FArrayProperty* SampleArray = CastField<FArrayProperty>(SampleDataProp);
	if (!SampleArray)
		return MakeError(TEXT("animation_asset"), TEXT("remove_blendspace_sample"), 3000,
			TEXT("SampleData property not resolvable via reflection"));
	FScriptArrayHelper Helper(SampleArray, SampleArray->ContainerPtrToValuePtr<void>(BS));
	if (!Helper.IsValidIndex(SampleIndex))
		return MakeError(TEXT("animation_asset"), TEXT("remove_blendspace_sample"), 1001,
			FString::Printf(TEXT("SampleIndex %d out of range (have %d)"), SampleIndex, Helper.Num()));
	Helper.RemoveValues(SampleIndex, 1);
	BS->PostEditChange();  // trigger rebuild of triangulation/grid
	BS->MarkPackageDirty();
	return MakeSuccess(TEXT("animation_asset"), TEXT("remove_blendspace_sample"),
		FString::Printf(TEXT("Removed blendspace sample %d"), SampleIndex));
}
