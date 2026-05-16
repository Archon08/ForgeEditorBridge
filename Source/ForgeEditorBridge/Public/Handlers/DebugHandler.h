#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "DebugHandler.generated.h"

/**
 * DebugHandler — domain "debug"  (v0.1.0 / UE 5.7)
 *
 * Synchronous viewport debug drawing over HTTP.
 * All shapes are drawn in the current editor world.
 * Duration 0 = persistent until clear is called.
 * Duration > 0 = auto-expires after that many seconds.
 *
 * Color format: hex "#RRGGBB" / "#RRGGBBAA" or named:
 *   Red, Green, Blue, White, Black, Yellow, Cyan, Magenta, Orange, Purple
 *
 * Actions:
 *   draw_line   → start{x,y,z}, end{x,y,z}, color (default "White"),
 *                 duration (default 0 = persistent), thickness (default 0)
 *   draw_sphere → center{x,y,z}, radius (default 50), color (default "White"),
 *                 duration (default 0), thickness (default 0)
 *   draw_box    → center{x,y,z}, extent{x,y,z} (default 50 each),
 *                 rotation{pitch,yaw,roll} (default 0), color (default "White"),
 *                 duration (default 0), thickness (default 0)
 *   draw_text   → text (required), location{x,y,z}, color (default "White"),
 *                 duration (default -1 = persistent)
 *   draw_arrow  → start{x,y,z}, end{x,y,z}, arrow_size (default 40),
 *                 color (default "Yellow"), duration (default 0), thickness (default 0)
 *   clear       → (no params) — flush all persistent debug lines and strings
 */
UCLASS()
class FORGEEDITORBRIDGE_API UDebugHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual FString GetDomainName() const override { return TEXT("debug"); }
	virtual TArray<FString> GetSupportedActions() const override
	{
		return {
			TEXT("draw_line"),
			TEXT("draw_sphere"),
			TEXT("draw_box"),
			TEXT("draw_text"),
			TEXT("draw_arrow"),
			TEXT("clear"),
			TEXT("get_blueprint_errors"),
			TEXT("run_asset_validation"),
			TEXT("get_memory_report"),
		};
	}
	virtual TSharedPtr<FJsonObject> GetActionSchemas() const override;
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_DrawLine  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DrawSphere(TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DrawBox   (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DrawText  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_DrawArrow (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_Clear     (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetBlueprintErrors (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RunAssetValidation (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetMemoryReport    (TSharedPtr<FJsonObject> Params);

	/** Safely get the current editor world without asserting. */
	UWorld* GetEditorWorld() const;

	/**
	 * Parse a color from hex "#RRGGBB[AA]" or a named color string.
	 * Falls back to Default if the string is unrecognized.
	 */
	static FColor ParseColor(const FString& ColorStr, const FColor& Default = FColor::White);
};
