#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "SplineHandler.generated.h"

class USplineComponent;

/**
 * SplineHandler — domain "spline"  (Phase 4 / UE 5.7)
 *
 * Actions:
 *   create_spline_actor → actor_name, location{x,y,z}, points[{x,y,z}...]
 *   add_point           → actor_path, position{x,y,z}, index(-1=append)
 *   set_point           → actor_path, index, position{x,y,z}
 *   set_tangent         → actor_path, index, arrive_tangent{x,y,z}, leave_tangent{x,y,z}
 *   get_points          → actor_path — returns all points with tangents, type, closed, length
 *   close_loop          → actor_path, closed(bool)
 *   set_point_type      → actor_path, index, type(Linear/Curve/Constant/CurveClamped)
 *   delete_point        → actor_path, index — remove the point at the given index
 *   clear_points        → actor_path — remove all spline points
 *   get_length          → actor_label, component_name? — returns arc length, point count, closed state
 *   get_point_at_distance → actor_label, component_name?, distance, coordinate_space?,
 *                           include_tangent?, include_normal?, include_rotation?
 */
UCLASS()
class FORGEEDITORBRIDGE_API USplineHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("spline"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("create_spline_actor"), TEXT("add_point"), TEXT("set_point"),
			TEXT("set_tangent"), TEXT("get_points"), TEXT("close_loop"), TEXT("set_point_type"),
			TEXT("delete_point"), TEXT("clear_points"),
			TEXT("get_length"), TEXT("get_point_at_distance")
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_CreateSplineActor(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_AddPoint         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPoint         (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetTangent       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPoints        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_CloseLoop        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_SetPointType     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DeletePoint      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ClearPoints      (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetLength            (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetPointAtDistance   (TSharedPtr<FJsonObject> Params);

	/** Find an actor by label in the editor world. Returns nullptr and populates OutError on failure. */
	AActor* FindActorByLabel(const FString& ActorLabel, FString& OutError);

	/** Get the first USplineComponent on an actor. Returns nullptr and populates OutError on failure. */
	USplineComponent* GetSplineComponent(AActor* Actor, FString& OutError);

	/**
	 * Find a named USplineComponent on an actor, or fall back to the first one if name is empty.
	 * Returns nullptr and populates OutError on failure.
	 */
	USplineComponent* GetSplineComponentByName(AActor* Actor, const FString& ComponentName, FString& OutError);

	/** Parse a {x,y,z} sub-object from Params. */
	static FVector ParseVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& bFound);
};
