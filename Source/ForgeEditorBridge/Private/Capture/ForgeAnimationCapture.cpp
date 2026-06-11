#include "Capture/ForgeAnimationCapture.h"
#include "IO/ForgeContextWriter.h"

// --- Skeletal mesh / animation ---
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"

// --- UObject reflection ---
#include "UObject/UnrealType.h"
#include "UObject/TopLevelAssetPath.h"

// --- World / Actor iteration ---
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// --- Asset Registry (PoseSearch + Chooser global scan) ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

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
	TSharedRef<FJsonObject> MakeVecObj(const FVector& V)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	TSharedRef<FJsonObject> MakeQuatObj(const FQuat& Q)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Q.X);
		Obj->SetNumberField(TEXT("y"), Q.Y);
		Obj->SetNumberField(TEXT("z"), Q.Z);
		Obj->SetNumberField(TEXT("w"), Q.W);
		return Obj;
	}

	TSharedRef<FJsonObject> MakeTransformObj(const FTransform& T)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeVecObj(FVector(T.GetLocation())));
		Obj->SetObjectField(TEXT("rotation"), MakeQuatObj(T.GetRotation()));
		Obj->SetObjectField(TEXT("scale"),    MakeVecObj(FVector(T.GetScale3D())));
		return Obj;
	}

	// Read an object-property asset path via reflection. Returns "(none)" on miss.
	FString TryReadObjectPath(UObject* Container, const TCHAR* FieldName)
	{
		if (!Container) return TEXT("(none)");
		if (const FObjectProperty* P = FindFProperty<FObjectProperty>(Container->GetClass(), FieldName))
		{
			const UObject* Val = P->GetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(Container));
			if (Val) return Val->GetPathName();
		}
		return TEXT("(none)");
	}

	// Read a raw UObject* via an object property. Returns nullptr on miss.
	UObject* TryGetObjectRef(UObject* Container, const TCHAR* FieldName)
	{
		if (!Container) return nullptr;
		if (const FObjectProperty* P = FindFProperty<FObjectProperty>(Container->GetClass(), FieldName))
			return const_cast<UObject*>(P->GetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(Container)));
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeAnimationCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("animation")));
	UE_LOG(LogTemp, Log, TEXT("ForgeAnimation: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportAnimationData
// ---------------------------------------------------------------------------

bool UForgeAnimationCapture::ExportAnimationData()
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeAnimation: GEditor is null"));
		return false;
	}

	// GEditor->GetEditorWorldContext() calls check(0) when no editor context exists.
	// Use safe GEngine world iteration instead (same fix as ForgeHeightmapCapture).
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
		UE_LOG(LogTemp, Warning, TEXT("ForgeAnimation: No editor world"));
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"),  FForgeContextWriter::NowISO8601());
	Root->SetStringField(TEXT("level_name"), World->GetName());

	TArray<TSharedPtr<FJsonValue>> Issues;

	// IK Rig component class — resolved once via soft path; null if IKRig plugin absent
	UClass* IKRigComponentClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRigComponent"));

	// -------------------------------------------------------------------------
	// Skeletal mesh components in the editor level
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> MeshesArr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		TArray<USkeletalMeshComponent*> SkelComps;
		Actor->GetComponents<USkeletalMeshComponent>(SkelComps);
		if (SkelComps.IsEmpty()) continue;

		for (USkeletalMeshComponent* SkelComp : SkelComps)
		{
			if (!SkelComp) continue;

			USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
			if (!SkelMesh) continue;

			TSharedRef<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("actor_name"),     Actor->GetActorNameOrLabel());
			CompObj->SetStringField(TEXT("component_name"), SkelComp->GetName());
			CompObj->SetStringField(TEXT("mesh_asset"),     SkelMesh->GetPathName());

			const USkeleton* Skeleton = SkelMesh->GetSkeleton();
			CompObj->SetStringField(TEXT("skeleton_asset"),
				Skeleton ? Skeleton->GetPathName() : TEXT("(none)"));

			// -----------------------------------------------------------------
			// Bone hierarchy (capped at 100 for token budget)
			// -----------------------------------------------------------------
			if (Skeleton)
			{
				const FReferenceSkeleton& RefSkel    = Skeleton->GetReferenceSkeleton();
				const TArray<FMeshBoneInfo>& BoneInfo = RefSkel.GetRawRefBoneInfo();
				const TArray<FTransform>&    RefPose  = RefSkel.GetRawRefBonePose();

				const int32 BoneCount  = BoneInfo.Num();
				const int32 MaxExport  = FMath::Min(BoneCount, 100);

				CompObj->SetNumberField(TEXT("bone_count"),     BoneCount);
				CompObj->SetBoolField  (TEXT("bones_truncated"), BoneCount > MaxExport);

				TArray<TSharedPtr<FJsonValue>> BonesArr;
				for (int32 i = 0; i < MaxExport; ++i)
				{
					TSharedRef<FJsonObject> BoneObj = MakeShared<FJsonObject>();
					BoneObj->SetStringField(TEXT("name"),         BoneInfo[i].Name.ToString());
					BoneObj->SetNumberField(TEXT("parent_index"), BoneInfo[i].ParentIndex);
					if (RefPose.IsValidIndex(i))
						BoneObj->SetObjectField(TEXT("local_transform"), MakeTransformObj(RefPose[i]));
					BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));
				}
				CompObj->SetArrayField(TEXT("bones"), BonesArr);

				// AUDIT: DEEP_BONE_HIERARCHY
				if (BoneCount > 200)
				{
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("issue_type"), TEXT("DEEP_BONE_HIERARCHY"));
					Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
					Issue->SetStringField(TEXT("detail"),
						FString::Printf(
							TEXT("Mesh '%s' has %d bones (> 200 budget). Deep hierarchies increase skinning "
							     "cost and complicate IK solving. Merge rarely-animated bones or use proxy "
							     "meshes for higher LODs."),
							*SkelMesh->GetName(), BoneCount));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}
			else
			{
				CompObj->SetNumberField(TEXT("bone_count"), 0);
				CompObj->SetBoolField  (TEXT("bones_truncated"), false);
				CompObj->SetArrayField (TEXT("bones"), TArray<TSharedPtr<FJsonValue>>());
			}

			// -----------------------------------------------------------------
			// Sockets (mesh-level + skeleton-level, deduplicated by name)
			// -----------------------------------------------------------------
			{
				TSet<FName> SeenSockets;
				TArray<TSharedPtr<FJsonValue>> SocketsArr;

				auto SerializeSocket = [&](const USkeletalMeshSocket* Sock)
				{
					if (!Sock || SeenSockets.Contains(Sock->SocketName)) return;
					SeenSockets.Add(Sock->SocketName);

					TSharedRef<FJsonObject> SockObj = MakeShared<FJsonObject>();
					SockObj->SetStringField(TEXT("name"),        Sock->SocketName.ToString());
					SockObj->SetStringField(TEXT("parent_bone"), Sock->BoneName.ToString());

					FTransform RelT(Sock->RelativeRotation, Sock->RelativeLocation, Sock->RelativeScale);
					SockObj->SetObjectField(TEXT("relative_transform"), MakeTransformObj(RelT));
					SocketsArr.Add(MakeShared<FJsonValueObject>(SockObj));
				};

				for (const USkeletalMeshSocket* S : SkelMesh->GetMeshOnlySocketList()) SerializeSocket(S);
				if (Skeleton)
					for (const USkeletalMeshSocket* S : Skeleton->Sockets) SerializeSocket(S);

				CompObj->SetNumberField(TEXT("socket_count"), SocketsArr.Num());
				CompObj->SetArrayField (TEXT("sockets"),      SocketsArr);
			}

			// -----------------------------------------------------------------
			// Skin Weight Profiles
			// -----------------------------------------------------------------
			{
				TArray<TSharedPtr<FJsonValue>> ProfilesArr;
				for (const FSkinWeightProfileInfo& P : SkelMesh->GetSkinWeightProfiles())
					ProfilesArr.Add(MakeShared<FJsonValueString>(P.Name.ToString()));
				CompObj->SetArrayField(TEXT("skin_weight_profiles"), ProfilesArr);
			}

			// -----------------------------------------------------------------
			// IK Rig (UIKRigComponent on same actor, resolved via soft class ref)
			// -----------------------------------------------------------------
			{
				TSharedRef<FJsonObject> IKObj = MakeShared<FJsonObject>();
				bool bHasIK = false;

				if (IKRigComponentClass)
				{
					if (UActorComponent* IKComp = Actor->FindComponentByClass(IKRigComponentClass))
					{
						bHasIK = true;
						IKObj->SetBoolField  (TEXT("found"), true);
						IKObj->SetStringField(TEXT("asset"), TryReadObjectPath(IKComp, TEXT("IKRigAsset")));

						// Solver type names from UIKRigDefinition::Solvers array
						TArray<TSharedPtr<FJsonValue>> SolversArr;
						UObject* IKAsset = TryGetObjectRef(IKComp, TEXT("IKRigAsset"));
						if (IKAsset)
						{
							if (const FArrayProperty* SolversProp =
								FindFProperty<FArrayProperty>(IKAsset->GetClass(), TEXT("Solvers")))
							{
								FScriptArrayHelper ArrHelper(SolversProp,
									SolversProp->ContainerPtrToValuePtr<void>(IKAsset));
								const FObjectProperty* ElemProp =
									CastField<FObjectProperty>(SolversProp->Inner);
								if (ElemProp)
								{
									for (int32 i = 0; i < ArrHelper.Num(); ++i)
									{
										const UObject* Solver =
											ElemProp->GetObjectPropertyValue(ArrHelper.GetRawPtr(i));
										if (Solver)
											SolversArr.Add(MakeShared<FJsonValueString>(
												Solver->GetClass()->GetName()));
									}
								}
							}
						}
						IKObj->SetArrayField(TEXT("solvers"), SolversArr);
					}
				}

				if (!bHasIK)
				{
					IKObj->SetBoolField(TEXT("found"), false);

					// AUDIT: MISSING_IK_RIG
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("issue_type"), TEXT("MISSING_IK_RIG"));
					Issue->SetStringField(TEXT("severity"),   TEXT("error"));
					Issue->SetStringField(TEXT("detail"),
						FString::Printf(
							TEXT("Actor '%s' has a SkeletalMeshComponent but no IKRigComponent. Without "
							     "an IK Rig, foot placement, procedural aim, and retargeting pipelines "
							     "will not function. Add an IKRigComponent referencing the appropriate "
							     "UIKRigDefinition asset."),
							*Actor->GetActorNameOrLabel()));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}

				CompObj->SetObjectField(TEXT("ik_rig"), IKObj);
			}

			// -----------------------------------------------------------------
			// GPU Deformer Graph (UE5.3+ — USkeletalMeshComponent::MeshDeformer)
			// -----------------------------------------------------------------
			{
				TSharedRef<FJsonObject> DefObj = MakeShared<FJsonObject>();

				UObject* DeformerAsset = TryGetObjectRef(SkelComp, TEXT("MeshDeformer"));
				if (DeformerAsset)
				{
					DefObj->SetBoolField  (TEXT("found"),      true);
					DefObj->SetStringField(TEXT("asset"),      DeformerAsset->GetPathName());
					DefObj->SetStringField(TEXT("class_name"), DeformerAsset->GetClass()->GetName());

					// Optimus deformer: ComputeKernels array -> kernel names
					TArray<TSharedPtr<FJsonValue>> KernelsArr;
					if (const FArrayProperty* KernelsProp =
						FindFProperty<FArrayProperty>(DeformerAsset->GetClass(), TEXT("ComputeKernels")))
					{
						FScriptArrayHelper ArrHelper(KernelsProp,
							KernelsProp->ContainerPtrToValuePtr<void>(DeformerAsset));
						const FObjectProperty* ElemProp =
							CastField<FObjectProperty>(KernelsProp->Inner);
						if (ElemProp)
						{
							for (int32 i = 0; i < ArrHelper.Num(); ++i)
							{
								const UObject* Kernel =
									ElemProp->GetObjectPropertyValue(ArrHelper.GetRawPtr(i));
								if (Kernel)
									KernelsArr.Add(MakeShared<FJsonValueString>(Kernel->GetName()));
							}
						}
					}
					DefObj->SetArrayField(TEXT("kernel_names"), KernelsArr);
				}
				else
				{
					DefObj->SetBoolField(TEXT("found"), false);
				}

				CompObj->SetObjectField(TEXT("deformer"), DefObj);
			}

			MeshesArr.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}

	Root->SetArrayField(TEXT("skeletal_meshes"), MeshesArr);

	// -------------------------------------------------------------------------
	// Pose Search Databases (project-wide scan via Asset Registry)
	// -------------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> DBArr;
		IAssetRegistry& AR =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		// UE5.1+: use ClassPaths (ClassNames was deprecated in 5.1)
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/PoseSearch"), TEXT("PoseSearchDatabase")));

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		for (const FAssetData& AD : Assets)
		{
			TSharedRef<FJsonObject> DBObj = MakeShared<FJsonObject>();
			DBObj->SetStringField(TEXT("asset"), AD.PackageName.ToString());

			UObject* DBAsset = AD.GetAsset();
			if (DBAsset)
			{
				// Schema name
				FString SchemaName = TEXT("(none)");
				if (const FObjectProperty* SchemaProp =
					FindFProperty<FObjectProperty>(DBAsset->GetClass(), TEXT("Schema")))
				{
					const UObject* SchemaObj =
						SchemaProp->GetObjectPropertyValue(SchemaProp->ContainerPtrToValuePtr<void>(DBAsset));
					if (SchemaObj) SchemaName = SchemaObj->GetName();
				}
				DBObj->SetStringField(TEXT("schema"), SchemaName);

				// Is indexed — try multiple field name candidates across UE versions
				bool bIndexed = false;
				for (const TCHAR* FieldName : { TEXT("bIndexed"), TEXT("bIsIndexed"), TEXT("IsIndexed") })
				{
					if (const FBoolProperty* P =
						FindFProperty<FBoolProperty>(DBAsset->GetClass(), FieldName))
					{
						bIndexed = P->GetPropertyValue(P->ContainerPtrToValuePtr<void>(DBAsset));
						break;
					}
				}
				DBObj->SetBoolField(TEXT("is_indexed"), bIndexed);

				// Pose count — try multiple field name candidates
				int32 PoseCount = 0;
				for (const TCHAR* FieldName :
					{ TEXT("NumberOfPoses"), TEXT("PoseCount"), TEXT("NumPoses") })
				{
					if (const FIntProperty* P =
						FindFProperty<FIntProperty>(DBAsset->GetClass(), FieldName))
					{
						PoseCount = P->GetPropertyValue(P->ContainerPtrToValuePtr<void>(DBAsset));
						break;
					}
				}
				DBObj->SetNumberField(TEXT("pose_count"), PoseCount);

				// AUDIT: STALE_POSE_INDEX
				if (!bIndexed)
				{
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("issue_type"), TEXT("STALE_POSE_INDEX"));
					Issue->SetStringField(TEXT("severity"),   TEXT("warning"));
					Issue->SetStringField(TEXT("detail"),
						FString::Printf(
							TEXT("PoseSearchDatabase '%s' is not indexed. Motion Matching queries will "
							     "fall back to brute-force search or return no results at runtime. "
							     "Open the asset and click 'Build Index' before shipping."),
							*AD.AssetName.ToString()));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}
			DBArr.Add(MakeShared<FJsonValueObject>(DBObj));
		}

		Root->SetArrayField(TEXT("pose_search_databases"), DBArr);
	}

	// -------------------------------------------------------------------------
	// Chooser Tables (project-wide scan via Asset Registry)
	// -------------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> ChoosersArr;
		IAssetRegistry& AR =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Chooser"), TEXT("ChooserTable")));

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		for (const FAssetData& AD : Assets)
		{
			TSharedRef<FJsonObject> ChooserObj = MakeShared<FJsonObject>();
			ChooserObj->SetStringField(TEXT("asset"), AD.PackageName.ToString());

			UObject* ChooserAsset = AD.GetAsset();
			if (ChooserAsset)
			{
				// Result type — OutputObjectType is typically a FInstancedStruct or FObjectProperty
				FString ResultType = TEXT("(unknown)");
				if (const FObjectProperty* OutProp =
					FindFProperty<FObjectProperty>(ChooserAsset->GetClass(), TEXT("OutputObjectType")))
				{
					const UObject* OT =
						OutProp->GetObjectPropertyValue(OutProp->ContainerPtrToValuePtr<void>(ChooserAsset));
					if (OT) ResultType = OT->GetName();
				}
				else if (const FStructProperty* OutSProp =
					FindFProperty<FStructProperty>(ChooserAsset->GetClass(), TEXT("OutputObjectType")))
				{
					if (OutSProp->Struct) ResultType = OutSProp->Struct->GetName();
				}
				ChooserObj->SetStringField(TEXT("result_type"), ResultType);

				// Row count from Rows array
				int32 RowCount = 0;
				if (const FArrayProperty* RowsProp =
					FindFProperty<FArrayProperty>(ChooserAsset->GetClass(), TEXT("Rows")))
				{
					FScriptArrayHelper ArrHelper(RowsProp,
						RowsProp->ContainerPtrToValuePtr<void>(ChooserAsset));
					RowCount = ArrHelper.Num();
				}
				ChooserObj->SetNumberField(TEXT("row_count"), RowCount);
			}

			ChoosersArr.Add(MakeShared<FJsonValueObject>(ChooserObj));
		}

		Root->SetArrayField(TEXT("choosers"), ChoosersArr);
	}

	// -------------------------------------------------------------------------
	// Audit summary
	// -------------------------------------------------------------------------
	{
		TSharedRef<FJsonObject> AuditObj = MakeShared<FJsonObject>();
		AuditObj->SetNumberField(TEXT("total_issues"), Issues.Num());
		AuditObj->SetArrayField (TEXT("issues"),       Issues);
		Root->SetObjectField(TEXT("audit"), AuditObj);
	}

	bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("animation"), TEXT("skeletal_data"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log,
			TEXT("ForgeAnimation: Exported -> animation/skeletal_data.json (%d mesh(es), %d issue(s))"),
			MeshesArr.Num(), Issues.Num());
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeAnimationCapture::UpdateIndexFile()
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
	Section->SetStringField(TEXT("file"),         TEXT("animation/skeletal_data.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("animation"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
