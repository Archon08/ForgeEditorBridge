#include "Handlers/GeometryScriptHandler.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("geom");

namespace
{
    bool ReadVec(const TSharedPtr<FJsonObject>& Params, const FString& Key, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params->TryGetObjectField(Key, Obj) || !Obj || !Obj->IsValid()) return false;
        double X=0, Y=0, Z=0;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        (*Obj)->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X,Y,Z);
        return true;
    }

    bool ReadRot(const TSharedPtr<FJsonObject>& Params, const FString& Key, FRotator& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params->TryGetObjectField(Key, Obj) || !Obj || !Obj->IsValid()) return false;
        double P=0, Yw=0, R=0;
        (*Obj)->TryGetNumberField(TEXT("pitch"), P);
        (*Obj)->TryGetNumberField(TEXT("yaw"),   Yw);
        (*Obj)->TryGetNumberField(TEXT("roll"),  R);
        Out = FRotator(P, Yw, R);
        return true;
    }

    FTransform ReadTransform(const TSharedPtr<FJsonObject>& Params)
    {
        FVector Loc = FVector::ZeroVector;
        FRotator Rot = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
        ReadVec(Params, TEXT("location"), Loc);
        ReadRot(Params, TEXT("rotation"), Rot);
        ReadVec(Params, TEXT("scale"), Scale);
        return FTransform(Rot, Loc, Scale);
    }
}

UDynamicMesh* UGeometryScriptHandler::GetOrError(const FString& MeshId, const FString& Action, FBridgeResult& OutErr)
{
    if (MeshId.IsEmpty())
    {
        OutErr = MakeError(DOMAIN, Action, 1000, TEXT("'mesh_id' is required"));
        return nullptr;
    }
    TObjectPtr<UDynamicMesh>* Found = Meshes.Find(MeshId);
    if (!Found || !*Found)
    {
        OutErr = MakeError(DOMAIN, Action, 2000,
            FString::Printf(TEXT("Mesh '%s' not found — call create_mesh first"), *MeshId));
        return nullptr;
    }
    return Found->Get();
}

FBridgeResult UGeometryScriptHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));

    if (Action == TEXT("create_mesh"))                 return Action_CreateMesh(Params);
    if (Action == TEXT("delete_mesh"))                 return Action_DeleteMesh(Params);
    if (Action == TEXT("list_meshes"))                 return Action_ListMeshes(Params);
    if (Action == TEXT("get_mesh_info"))               return Action_GetMeshInfo(Params);
    if (Action == TEXT("append_box"))                  return Action_AppendBox(Params);
    if (Action == TEXT("append_sphere"))               return Action_AppendSphere(Params);
    if (Action == TEXT("append_cylinder"))             return Action_AppendCylinder(Params);
    if (Action == TEXT("append_cone"))                 return Action_AppendCone(Params);
    if (Action == TEXT("append_torus"))                return Action_AppendTorus(Params);
    if (Action == TEXT("append_plane"))                return Action_AppendPlane(Params);
    if (Action == TEXT("union_meshes"))                return Action_UnionMeshes(Params);
    if (Action == TEXT("subtract_meshes"))             return Action_SubtractMeshes(Params);
    if (Action == TEXT("intersect_meshes"))            return Action_IntersectMeshes(Params);
    if (Action == TEXT("transform_mesh"))              return Action_TransformMesh(Params);
    if (Action == TEXT("translate_mesh"))              return Action_TranslateMesh(Params);
    if (Action == TEXT("compute_normals"))             return Action_ComputeNormals(Params);
    if (Action == TEXT("apply_planar_uvs"))            return Action_ApplyPlanarUVs(Params);
    if (Action == TEXT("apply_box_uvs"))               return Action_ApplyBoxUVs(Params);
    if (Action == TEXT("subdivide_mesh"))              return Action_SubdivideMesh(Params);
    if (Action == TEXT("simplify_to_triangle_count"))  return Action_SimplifyToTriangleCount(Params);
    if (Action == TEXT("weld_vertices"))               return Action_WeldVertices(Params);
    if (Action == TEXT("copy_mesh_to_static_mesh"))    return Action_CopyMeshToStaticMesh(Params);
    if (Action == TEXT("copy_mesh_from_static_mesh"))  return Action_CopyMeshFromStaticMesh(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_mesh, delete_mesh, list_meshes, get_mesh_info, append_box, append_sphere, append_cylinder, append_cone, append_torus, append_plane, union_meshes, subtract_meshes, intersect_meshes, transform_mesh, translate_mesh, compute_normals, apply_planar_uvs, apply_box_uvs, subdivide_mesh, simplify_to_triangle_count, weld_vertices, copy_mesh_to_static_mesh, copy_mesh_from_static_mesh"));
}

FBridgeResult UGeometryScriptHandler::Action_CreateMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    if (!Params->TryGetStringField(TEXT("mesh_id"), MeshId) || MeshId.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_mesh"), 1000, TEXT("'mesh_id' is required"));
    if (Meshes.Contains(MeshId))
        return MakeError(DOMAIN, TEXT("create_mesh"), 2002,
            FString::Printf(TEXT("Mesh '%s' already exists"), *MeshId));

    UDynamicMesh* M = NewObject<UDynamicMesh>(this);
    if (!M) return MakeError(DOMAIN, TEXT("create_mesh"), 3000, TEXT("NewObject failed"));
    Meshes.Add(MeshId, M);
    return MakeSuccess(DOMAIN, TEXT("create_mesh"),
        FString::Printf(TEXT("Created mesh '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_DeleteMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    if (!Params->TryGetStringField(TEXT("mesh_id"), MeshId) || MeshId.IsEmpty())
        return MakeError(DOMAIN, TEXT("delete_mesh"), 1000, TEXT("'mesh_id' is required"));
    if (Meshes.Remove(MeshId) == 0)
        return MakeError(DOMAIN, TEXT("delete_mesh"), 2000,
            FString::Printf(TEXT("Mesh '%s' not found"), *MeshId));
    return MakeSuccess(DOMAIN, TEXT("delete_mesh"),
        FString::Printf(TEXT("Deleted mesh '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_ListMeshes(TSharedPtr<FJsonObject> Params)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TPair<FString, TObjectPtr<UDynamicMesh>>& Pair : Meshes)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("mesh_id"), Pair.Key);
        if (Pair.Value)
        {
            int32 Tris = 0, Verts = 0;
            Pair.Value->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
            {
                Tris  = Mesh.TriangleCount();
                Verts = Mesh.VertexCount();
            });
            Entry->SetNumberField(TEXT("triangles"), Tris);
            Entry->SetNumberField(TEXT("vertices"), Verts);
        }
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("meshes"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_meshes"),
        FString::Printf(TEXT("%d mesh(es)"), Arr.Num()), Data);
}

FBridgeResult UGeometryScriptHandler::Action_GetMeshInfo(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("get_mesh_info"), Err);
    if (!M) return Err;

    int32 Tris = 0, Verts = 0;
    M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
    {
        Tris  = Mesh.TriangleCount();
        Verts = Mesh.VertexCount();
    });
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("mesh_id"), MeshId);
    Data->SetNumberField(TEXT("triangles"), Tris);
    Data->SetNumberField(TEXT("vertices"), Verts);
    return MakeSuccess(DOMAIN, TEXT("get_mesh_info"),
        FString::Printf(TEXT("'%s': %d tris, %d verts"), *MeshId, Tris, Verts), Data);
}

FBridgeResult UGeometryScriptHandler::Action_AppendBox(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_box"), Err);
    if (!M) return Err;

    FVector Dim(100, 100, 100);
    ReadVec(Params, TEXT("dimensions"), Dim);
    int32 Subdivs = 0;
    Params->TryGetNumberField(TEXT("subdivisions"), Subdivs);

    FGeometryScriptPrimitiveOptions Opts;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        M, Opts, ReadTransform(Params), Dim.X, Dim.Y, Dim.Z, Subdivs, Subdivs, Subdivs);
    return MakeSuccess(DOMAIN, TEXT("append_box"),
        FString::Printf(TEXT("Box appended to '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_AppendSphere(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_sphere"), Err);
    if (!M) return Err;

    double Radius = 50.0;
    int32 Lat = 16, Lng = 16;
    Params->TryGetNumberField(TEXT("radius"), Radius);
    Params->TryGetNumberField(TEXT("latitude_steps"), Lat);
    Params->TryGetNumberField(TEXT("longitude_steps"), Lng);

    FGeometryScriptPrimitiveOptions Opts;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        M, Opts, ReadTransform(Params), Radius, Lat, Lng);
    return MakeSuccess(DOMAIN, TEXT("append_sphere"),
        FString::Printf(TEXT("Sphere appended to '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_AppendCylinder(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_cylinder"), Err);
    if (!M) return Err;

    double Radius = 50, Height = 100;
    int32 Steps = 16, HeightSteps = 0;
    Params->TryGetNumberField(TEXT("radius"), Radius);
    Params->TryGetNumberField(TEXT("height"), Height);
    Params->TryGetNumberField(TEXT("radial_steps"), Steps);
    Params->TryGetNumberField(TEXT("height_steps"), HeightSteps);

    FGeometryScriptPrimitiveOptions Opts;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
        M, Opts, ReadTransform(Params), Radius, Height, Steps, HeightSteps);
    return MakeSuccess(DOMAIN, TEXT("append_cylinder"),
        FString::Printf(TEXT("Cylinder appended to '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_AppendCone(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_cone"), Err);
    if (!M) return Err;

    double BaseR = 50, TopR = 0, Height = 100;
    int32 Steps = 16, HeightSteps = 0;
    Params->TryGetNumberField(TEXT("base_radius"), BaseR);
    Params->TryGetNumberField(TEXT("top_radius"),  TopR);
    Params->TryGetNumberField(TEXT("height"),      Height);
    Params->TryGetNumberField(TEXT("radial_steps"), Steps);
    Params->TryGetNumberField(TEXT("height_steps"), HeightSteps);

    FGeometryScriptPrimitiveOptions Opts;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
        M, Opts, ReadTransform(Params), BaseR, TopR, Height, Steps, HeightSteps);
    return MakeSuccess(DOMAIN, TEXT("append_cone"),
        FString::Printf(TEXT("Cone appended to '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_AppendTorus(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_torus"), Err);
    if (!M) return Err;

    double MajorR = 50, MinorR = 10;
    int32 MajorSteps = 16, MinorSteps = 16;
    Params->TryGetNumberField(TEXT("major_radius"), MajorR);
    Params->TryGetNumberField(TEXT("minor_radius"), MinorR);
    Params->TryGetNumberField(TEXT("major_steps"), MajorSteps);
    Params->TryGetNumberField(TEXT("minor_steps"), MinorSteps);

    FGeometryScriptPrimitiveOptions Opts;
    FGeometryScriptRevolveOptions Rev;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
        M, Opts, ReadTransform(Params), Rev, MajorR, MinorR, MajorSteps, MinorSteps);
    return MakeSuccess(DOMAIN, TEXT("append_torus"),
        FString::Printf(TEXT("Torus appended to '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_AppendPlane(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("append_plane"), Err);
    if (!M) return Err;

    double DimX = 100, DimY = 100;
    int32 StepsX = 0, StepsY = 0;
    Params->TryGetNumberField(TEXT("dim_x"), DimX);
    Params->TryGetNumberField(TEXT("dim_y"), DimY);
    Params->TryGetNumberField(TEXT("steps_x"), StepsX);
    Params->TryGetNumberField(TEXT("steps_y"), StepsY);

    FGeometryScriptPrimitiveOptions Opts;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(
        M, Opts, ReadTransform(Params), DimX, DimY, StepsX, StepsY);
    return MakeSuccess(DOMAIN, TEXT("append_plane"),
        FString::Printf(TEXT("Plane appended to '%s'"), *MeshId));
}

// Boolean op runner — must be a member function (or have member access) because
// MakeError/MakeSuccess are protected static on UBridgeHandlerBase. The earlier
// namespace function failed C2248. Using a #define-based local helper instead.
#define RUN_BOOL_OP(ActionName, OpEnum)                                                              \
    FString TargetId, SourceId;                                                                       \
    Params->TryGetStringField(TEXT("target_mesh_id"), TargetId);                                       \
    Params->TryGetStringField(TEXT("source_mesh_id"), SourceId);                                       \
    if (TargetId.IsEmpty() || SourceId.IsEmpty())                                                      \
        return MakeError(DOMAIN, ActionName, 1000, TEXT("'target_mesh_id' and 'source_mesh_id' are required")); \
    TObjectPtr<UDynamicMesh>* T = Meshes.Find(TargetId);                                               \
    TObjectPtr<UDynamicMesh>* S = Meshes.Find(SourceId);                                               \
    if (!T || !*T || !S || !*S)                                                                        \
        return MakeError(DOMAIN, ActionName, 2000, TEXT("target or source mesh not found"));            \
    FGeometryScriptMeshBooleanOptions Opts;                                                            \
    UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(                                     \
        T->Get(), FTransform::Identity, S->Get(), FTransform::Identity, OpEnum, Opts);                 \
    return MakeSuccess(DOMAIN, ActionName,                                                             \
        FString::Printf(TEXT("Boolean op on '%s' (with '%s')"), *TargetId, *SourceId))

FBridgeResult UGeometryScriptHandler::Action_UnionMeshes(TSharedPtr<FJsonObject> Params)
{
    RUN_BOOL_OP(TEXT("union_meshes"), EGeometryScriptBooleanOperation::Union);
}

FBridgeResult UGeometryScriptHandler::Action_SubtractMeshes(TSharedPtr<FJsonObject> Params)
{
    RUN_BOOL_OP(TEXT("subtract_meshes"), EGeometryScriptBooleanOperation::Subtract);
}

FBridgeResult UGeometryScriptHandler::Action_IntersectMeshes(TSharedPtr<FJsonObject> Params)
{
    // 5.7: enum value is `Intersection`, not `Intersect`.
    RUN_BOOL_OP(TEXT("intersect_meshes"), EGeometryScriptBooleanOperation::Intersection);
}

#undef RUN_BOOL_OP

FBridgeResult UGeometryScriptHandler::Action_TransformMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("transform_mesh"), Err);
    if (!M) return Err;

    UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(M, ReadTransform(Params));
    return MakeSuccess(DOMAIN, TEXT("transform_mesh"),
        FString::Printf(TEXT("Transformed '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_TranslateMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("translate_mesh"), Err);
    if (!M) return Err;

    FVector Loc = FVector::ZeroVector;
    if (!ReadVec(Params, TEXT("location"), Loc))
        return MakeError(DOMAIN, TEXT("translate_mesh"), 1000, TEXT("'location' {x,y,z} is required"));

    UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(M, Loc);
    return MakeSuccess(DOMAIN, TEXT("translate_mesh"),
        FString::Printf(TEXT("Translated '%s' by (%.1f,%.1f,%.1f)"), *MeshId, Loc.X, Loc.Y, Loc.Z));
}

FBridgeResult UGeometryScriptHandler::Action_ComputeNormals(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("compute_normals"), Err);
    if (!M) return Err;

    FGeometryScriptCalculateNormalsOptions Opts;
    UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(M, Opts);
    return MakeSuccess(DOMAIN, TEXT("compute_normals"),
        FString::Printf(TEXT("Recomputed normals on '%s'"), *MeshId));
}

FBridgeResult UGeometryScriptHandler::Action_ApplyPlanarUVs(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("apply_planar_uvs"), Err);
    if (!M) return Err;

    int32 UVChannel = 0;
    Params->TryGetNumberField(TEXT("uv_channel"), UVChannel);
    // 5.7: SetMeshUVsFromPlanarProjection takes (Mesh, UVSetIndex, FTransform, FGeometryScriptMeshSelection, Debug=nullptr)
    FGeometryScriptMeshSelection EmptySelection;
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
        M, UVChannel, FTransform::Identity, EmptySelection);
    return MakeSuccess(DOMAIN, TEXT("apply_planar_uvs"),
        FString::Printf(TEXT("Planar UVs applied on '%s' (ch=%d)"), *MeshId, UVChannel));
}

FBridgeResult UGeometryScriptHandler::Action_ApplyBoxUVs(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("apply_box_uvs"), Err);
    if (!M) return Err;

    int32 UVChannel = 0;
    Params->TryGetNumberField(TEXT("uv_channel"), UVChannel);
    FGeometryScriptMeshSelection EmptySelection;
    // 5.7: takes (Mesh, UVSetIndex, FTransform, FGeometryScriptMeshSelection, MinIslandTriCount=2, Debug=nullptr)
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
        M, UVChannel, FTransform::Identity, EmptySelection);
    return MakeSuccess(DOMAIN, TEXT("apply_box_uvs"),
        FString::Printf(TEXT("Box UVs applied on '%s' (ch=%d)"), *MeshId, UVChannel));
}

FBridgeResult UGeometryScriptHandler::Action_SubdivideMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("subdivide_mesh"), Err);
    if (!M) return Err;

    int32 Iters = 1;
    Params->TryGetNumberField(TEXT("iterations"), Iters);
    Iters = FMath::Clamp(Iters, 1, 6);
    FGeometryScriptPNTessellateOptions Opts;
    UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(M, Opts, Iters);
    return MakeSuccess(DOMAIN, TEXT("subdivide_mesh"),
        FString::Printf(TEXT("Subdivided '%s' x%d"), *MeshId, Iters));
}

FBridgeResult UGeometryScriptHandler::Action_SimplifyToTriangleCount(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("simplify_to_triangle_count"), Err);
    if (!M) return Err;

    int32 Target = 0;
    if (!Params->TryGetNumberField(TEXT("target_triangle_count"), Target) || Target <= 0)
        return MakeError(DOMAIN, TEXT("simplify_to_triangle_count"), 1000,
            TEXT("'target_triangle_count' must be > 0"));

    // 5.7: option struct is FGeometryScriptSimplifyMeshOptions, not FGeometryScriptMeshSimplifyOptions
    FGeometryScriptSimplifyMeshOptions Opts;
    UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(M, Target, Opts);
    return MakeSuccess(DOMAIN, TEXT("simplify_to_triangle_count"),
        FString::Printf(TEXT("Simplified '%s' to ~%d triangles"), *MeshId, Target));
}

FBridgeResult UGeometryScriptHandler::Action_WeldVertices(TSharedPtr<FJsonObject> Params)
{
    FString MeshId;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("weld_vertices"), Err);
    if (!M) return Err;

    double Tolerance = 0.001;
    Params->TryGetNumberField(TEXT("tolerance"), Tolerance);

    // 5.7: MergeAllOverlappingVertices is not in UGeometryScriptLibrary_MeshRepairFunctions.
    // Closest available primitive is WeldMeshEdges. Signature is (Mesh, Options, Debug*=nullptr) — no NumWelded out-param.
    FGeometryScriptWeldEdgesOptions WeldOpts;
    WeldOpts.Tolerance = (float)Tolerance;
    UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(M, WeldOpts);
    return MakeSuccess(DOMAIN, TEXT("weld_vertices"),
        FString::Printf(TEXT("Welded boundary edges on '%s' (tol=%.4f)"), *MeshId, Tolerance));
}

FBridgeResult UGeometryScriptHandler::Action_CopyMeshToStaticMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId, AssetPath;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("copy_mesh_to_static_mesh"), 1000, TEXT("'asset_path' is required"));
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("copy_mesh_to_static_mesh"), Err);
    if (!M) return Err;

    // Locate or create the static mesh asset
    UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!SM)
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package) return MakeError(DOMAIN, TEXT("copy_mesh_to_static_mesh"), 3000,
            FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName));
        Package->FullyLoad();
        SM = NewObject<UStaticMesh>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!SM) return MakeError(DOMAIN, TEXT("copy_mesh_to_static_mesh"), 3000, TEXT("NewObject failed"));
        FAssetRegistryModule::AssetCreated(SM);
    }

    FGeometryScriptCopyMeshToAssetOptions Opts;
    Opts.bEnableRecomputeNormals  = false;
    Opts.bEnableRecomputeTangents = false;
    Opts.bReplaceMaterials        = false;
    FGeometryScriptMeshWriteLOD WriteLOD;
    EGeometryScriptOutcomePins Outcome;

    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        M, SM, Opts, WriteLOD, Outcome, /*bUseSectionMaterials=*/true);

    SM->MarkPackageDirty();

    return MakeSuccess(DOMAIN, TEXT("copy_mesh_to_static_mesh"),
        FString::Printf(TEXT("Wrote '%s' to '%s'"), *MeshId, *AssetPath));
}

FBridgeResult UGeometryScriptHandler::Action_CopyMeshFromStaticMesh(TSharedPtr<FJsonObject> Params)
{
    FString MeshId, AssetPath;
    Params->TryGetStringField(TEXT("mesh_id"), MeshId);
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("copy_mesh_from_static_mesh"), 1000, TEXT("'asset_path' is required"));
    FBridgeResult Err;
    UDynamicMesh* M = GetOrError(MeshId, TEXT("copy_mesh_from_static_mesh"), Err);
    if (!M) return Err;

    UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!SM)
    {
        const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        SM = LoadObject<UStaticMesh>(nullptr, *Suffix);
    }
    if (!SM) return MakeError(DOMAIN, TEXT("copy_mesh_from_static_mesh"), 2000,
        FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

    FGeometryScriptCopyMeshFromAssetOptions Opts;
    FGeometryScriptMeshReadLOD ReadLOD;
    EGeometryScriptOutcomePins Outcome;

    // 5.7: CopyMeshFromStaticMesh non-V2 form is 6-arg (no bUseSectionMaterials);
    // it forwards internally to V2 with true. Use the V2 form directly so we are
    // explicit about the section-materials choice and don't trip the deprecation warning.
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
        SM, M, Opts, ReadLOD, Outcome, /*bUseSectionMaterials=*/true);

    return MakeSuccess(DOMAIN, TEXT("copy_mesh_from_static_mesh"),
        FString::Printf(TEXT("Read '%s' into '%s'"), *AssetPath, *MeshId));
}
