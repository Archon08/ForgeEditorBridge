#pragma once

#include "CoreMinimal.h"
#include "Handlers/BridgeHandlerBase.h"
#include "GeometryScriptHandler.generated.h"

class UDynamicMesh;

/**
 * GeometryScriptHandler — domain "geom"  (UE 5.7)
 *
 * Wraps the GeometryScripting plugin. Maintains a session-keyed map of
 * UDynamicMesh instances so multi-step authoring chains (e.g. box -> subtract
 * sphere -> simplify -> compute normals -> save to static mesh) work over
 * separate HTTP calls.
 *
 * Workflow:
 *   1) create_mesh mesh_id="my_mesh"
 *   2) append_box / append_sphere / etc.
 *   3) union_meshes / subtract_meshes
 *   4) compute_normals / apply_planar_uvs
 *   5) copy_mesh_to_static_mesh asset_path="/Game/.../MyMesh"
 *   6) delete_mesh
 *
 * Actions:
 *   create_mesh, delete_mesh, list_meshes, get_mesh_info
 *   append_box, append_sphere, append_cylinder, append_cone, append_torus, append_plane
 *   union_meshes, subtract_meshes, intersect_meshes
 *   transform_mesh, translate_mesh
 *   compute_normals, apply_planar_uvs, apply_box_uvs
 *   subdivide_mesh, simplify_to_triangle_count
 *   weld_vertices
 *   copy_mesh_to_static_mesh, copy_mesh_from_static_mesh
 */
UCLASS()
class FORGEEDITORBRIDGE_API UGeometryScriptHandler : public UBridgeHandlerBase
{
    GENERATED_BODY()

public:
    virtual FString GetDomainName() const override { return TEXT("geom"); }
    virtual TArray<FString> GetSupportedActions() const override
    {
        return {
            TEXT("create_mesh"), TEXT("delete_mesh"), TEXT("list_meshes"), TEXT("get_mesh_info"),
            TEXT("append_box"), TEXT("append_sphere"), TEXT("append_cylinder"),
            TEXT("append_cone"), TEXT("append_torus"), TEXT("append_plane"),
            TEXT("union_meshes"), TEXT("subtract_meshes"), TEXT("intersect_meshes"),
            TEXT("transform_mesh"), TEXT("translate_mesh"),
            TEXT("compute_normals"), TEXT("apply_planar_uvs"), TEXT("apply_box_uvs"),
            TEXT("subdivide_mesh"), TEXT("simplify_to_triangle_count"),
            TEXT("weld_vertices"),
            TEXT("copy_mesh_to_static_mesh"), TEXT("copy_mesh_from_static_mesh")
        };
    }
    virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
    UPROPERTY()
    TMap<FString, TObjectPtr<UDynamicMesh>> Meshes;

    UDynamicMesh* GetOrError(const FString& MeshId, const FString& Action, FBridgeResult& OutErr);

    FBridgeResult Action_CreateMesh                (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_DeleteMesh                (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ListMeshes                (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_GetMeshInfo               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendBox                 (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendSphere              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendCylinder            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendCone                (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendTorus               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_AppendPlane               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_UnionMeshes               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SubtractMeshes            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_IntersectMeshes           (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_TransformMesh             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_TranslateMesh             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ComputeNormals            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ApplyPlanarUVs            (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_ApplyBoxUVs               (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SubdivideMesh             (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_SimplifyToTriangleCount   (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_WeldVertices              (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CopyMeshToStaticMesh      (TSharedPtr<FJsonObject> Params);
    FBridgeResult Action_CopyMeshFromStaticMesh    (TSharedPtr<FJsonObject> Params);
};
