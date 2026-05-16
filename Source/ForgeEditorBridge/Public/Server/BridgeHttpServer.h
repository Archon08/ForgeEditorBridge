#pragma once

#include "CoreMinimal.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Results/BridgeResult.h"

class UForgeAISubsystem;

class FORGEEDITORBRIDGE_API FBridgeHttpServer : public TSharedFromThis<FBridgeHttpServer>
{
public:
	FBridgeHttpServer(UForgeAISubsystem* InSubsystem);
	~FBridgeHttpServer();

	void Start(const FString& OutputDir, int32 Port = 8765);
	void Stop();

	const FString& GetAuthToken() const { return AuthToken; }

private:
	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleBatchRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool ValidateAuth(const FHttpServerRequest& Request);

	FBridgeResult DispatchCommand(const FString& Domain, const FString& Action, TSharedPtr<FJsonObject> Params);
	FBridgeResult HandleSystemCommand(const FString& Action, TSharedPtr<FJsonObject> Params);
	void UpdateStatus(const FString& Domain, const FString& Action);

	/**
	 * Core batch execution: wraps all commands in a single "ForgeEditorBridge Atomic Batch"
	 * transaction, continues on failure, and returns:
	 *   { "ok": bool, "results": [...], "summary": { "total": N, "success": N, "fail": N } }
	 * Shared by the POST /batch endpoint and the system/batch action.
	 */
	TSharedPtr<FJsonObject> ExecuteBatchCommands(const TArray<TSharedPtr<FJsonValue>>& Commands);

	/** Serialize an FBridgeResult into a JSON response object. */
	TSharedPtr<FJsonObject> ResultToJson(const FBridgeResult& Result) const;

	UForgeAISubsystem* Subsystem;
	FString AuthToken;
	FString OutputDirectory;
	int32 ServerPort;
	TSharedPtr<IHttpRouter> HttpRouter;
	FHttpRouteHandle CommandRouteHandle;
	FHttpRouteHandle BatchRouteHandle;
};
