#include "Server/BridgeHttpServer.h"
#include "ForgeAISubsystem.h"
#include "IO/BridgeResultWriter.h"
#include "Handlers/QuarantineHandler.h"
#include "Handlers/BridgeHandlerBase.h"
#include "IO/ForgeContextWriter.h"
#include "HttpServerModule.h"
#include "HttpPath.h"
#include "IHttpRouter.h"
#include "HttpRequestHandler.h"
#include "HttpResultCallback.h"
#include "HttpServerConstants.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"
#include "Misc/MessageDialog.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "FileHelpers.h"
#include "UObject/UObjectIterator.h"

// ---- Capture includes (for export_all_captures / health_check) ----
#include "Capture/ForgeGASCapture.h"
#include "Capture/ForgeHeightmapCapture.h"
#include "Capture/ForgeBlueprintCapture.h"
#include "Capture/ForgeMaterialCapture.h"
#include "Capture/ForgeNiagaraCapture.h"
#include "Capture/ForgeAnimationCapture.h"
#include "Capture/ForgeInputCapture.h"
#include "Capture/ForgeDataTableCapture.h"
#include "Capture/ForgeWeatherCapture.h"
#include "Capture/ForgeWorldGenCapture.h"
#include "Capture/ForgeSymbolCapture.h"
#include "Capture/ForgePerformanceCapture.h"
#include "Capture/ForgeNetworkCapture.h"
#include "Capture/ForgeAssetRegistryCapture.h"
#include "HAL/PlatformFileManager.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/Package.h"
#endif

FBridgeHttpServer::FBridgeHttpServer(UForgeAISubsystem* InSubsystem)
	: Subsystem(InSubsystem)
{
}

FBridgeHttpServer::~FBridgeHttpServer()
{
	Stop();
}

void FBridgeHttpServer::Start(const FString& OutputDir, int32 Port, const FString& ConfiguredToken)
{
	OutputDirectory = OutputDir;
	ServerPort = Port;
	// Honor a fixed token from settings (UForgeAISettings::AuthToken); fall back to a
	// random per-session GUID when left blank.
	AuthToken = ConfiguredToken.IsEmpty()
		? FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)
		: ConfiguredToken;

	TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("active"), true);
	Status->SetNumberField(TEXT("port"), (double)Port);
	Status->SetStringField(TEXT("auth_token"), AuthToken);
	Status->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
	Status->SetStringField(TEXT("version"), TEXT("0.2.6"));

	// Build domain list dynamically from registered handlers
	TArray<TSharedPtr<FJsonValue>> Domains;
	Domains.Add(MakeShared<FJsonValueString>(TEXT("system")));
	for (const FString& D : Subsystem->GetRegisteredDomains())
	{
		Domains.Add(MakeShared<FJsonValueString>(D));
	}
	Status->SetArrayField(TEXT("available_domains"), Domains);

	FForgeContextWriter::WriteJSON(OutputDirectory, TEXT("bridge-status.json"), Status.ToSharedRef());

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	HttpRouter = HttpServerModule.GetHttpRouter(Port);

	if (HttpRouter.IsValid())
	{
		CommandRouteHandle = HttpRouter->BindRoute(
			FHttpPath(TEXT("/command")),
			EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				return HandleRequest(Request, OnComplete);
			}));

		BatchRouteHandle = HttpRouter->BindRoute(
			FHttpPath(TEXT("/batch")),
			EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				return HandleBatchRequest(Request, OnComplete);
			}));

		HttpServerModule.StartAllListeners();
		UE_LOG(LogTemp, Log, TEXT("ForgeEditorBridge Server started on port %d (routes: /command, /batch)"), Port);
	}
}

void FBridgeHttpServer::Stop()
{
	if (HttpRouter.IsValid())
	{
		if (CommandRouteHandle.IsValid()) HttpRouter->UnbindRoute(CommandRouteHandle);
		if (BatchRouteHandle.IsValid())   HttpRouter->UnbindRoute(BatchRouteHandle);
	}
}

bool FBridgeHttpServer::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Security: reject non-localhost connections
	const FString PeerAddress = Request.PeerAddress.IsValid() ? Request.PeerAddress->ToString(false) : TEXT("");
	if (!PeerAddress.IsEmpty() && PeerAddress != TEXT("127.0.0.1") && PeerAddress != TEXT("::1"))
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeEditorBridge: Rejected non-localhost connection from %s"), *PeerAddress);
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Denied,
			TEXT("Forbidden"), TEXT("Only localhost connections are allowed")));
		return true;
	}

	if (!ValidateAuth(Request))
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Denied,
			TEXT("Unauthorized"), TEXT("Authorization token missing or invalid")));
		return true;
	}

	FString JsonString = FString(Request.Body.Num(), (UTF8CHAR*)Request.Body.GetData());
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		FString Domain = JsonObject->GetStringField(TEXT("domain"));
		FString Action = JsonObject->GetStringField(TEXT("action"));

		// "params" is optional and may be missing or non-object. GetObjectField would
		// return a null TSharedPtr that handlers dereference unchecked, so substitute an
		// empty object — handlers report their own missing-field errors.
		const TSharedPtr<FJsonObject>* ParamsField = nullptr;
		TSharedPtr<FJsonObject> Params = (JsonObject->TryGetObjectField(TEXT("params"), ParamsField) && ParamsField)
			? *ParamsField : MakeShared<FJsonObject>();

		FBridgeResult Result = DispatchCommand(Domain, Action, Params);

		Subsystem->ResultWriter->WriteResult(Result);
		UpdateStatus(Domain, Action);

		TSharedPtr<FJsonObject> ResponseJson = ResultToJson(Result);

		FString ResponseBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseBody);
		FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);

		OnComplete(FHttpServerResponse::Create(ResponseBody, TEXT("application/json")));
	}
	else
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::BadRequest,
			TEXT("BadRequest"), TEXT("Invalid JSON body")));
	}

	return true;
}

bool FBridgeHttpServer::HandleBatchRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Security: reject non-localhost connections
	const FString PeerAddress = Request.PeerAddress.IsValid() ? Request.PeerAddress->ToString(false) : TEXT("");
	if (!PeerAddress.IsEmpty() && PeerAddress != TEXT("127.0.0.1") && PeerAddress != TEXT("::1"))
	{
		UE_LOG(LogTemp, Warning, TEXT("ForgeEditorBridge /batch: Rejected non-localhost connection from %s"), *PeerAddress);
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Denied,
			TEXT("Forbidden"), TEXT("Only localhost connections are allowed")));
		return true;
	}

	if (!ValidateAuth(Request))
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Denied,
			TEXT("Unauthorized"), TEXT("Authorization token missing or invalid")));
		return true;
	}

	FString JsonString = FString(Request.Body.Num(), (UTF8CHAR*)Request.Body.GetData());
	TArray<TSharedPtr<FJsonValue>> CommandsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, CommandsArray))
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::BadRequest,
			TEXT("BadRequest"), TEXT("POST /batch body must be a JSON array: [{domain, action, params}, ...]")));
		return true;
	}

	TSharedPtr<FJsonObject> Response = ExecuteBatchCommands(CommandsArray);

	FString ResponseBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseBody);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

	OnComplete(FHttpServerResponse::Create(ResponseBody, TEXT("application/json")));
	return true;
}

TSharedPtr<FJsonObject> FBridgeHttpServer::ExecuteBatchCommands(const TArray<TSharedPtr<FJsonValue>>& Commands)
{
#if WITH_EDITOR
	FScopedTransaction Transaction(FText::FromString(TEXT("ForgeEditorBridge Atomic Batch")));
#endif

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;

	for (const TSharedPtr<FJsonValue>& CmdVal : Commands)
	{
		const TSharedPtr<FJsonObject> CmdObj = CmdVal->AsObject();
		if (!CmdObj.IsValid()) continue;

		FString CmdDomain, CmdAction;
		CmdObj->TryGetStringField(TEXT("domain"), CmdDomain);
		CmdObj->TryGetStringField(TEXT("action"), CmdAction);

		// Optional "params": substitute an empty object when missing/non-object so
		// handlers never dereference a null TSharedPtr (mirrors HandleRequest).
		const TSharedPtr<FJsonObject>* CmdParamsField = nullptr;
		TSharedPtr<FJsonObject> CmdParams = (CmdObj->TryGetObjectField(TEXT("params"), CmdParamsField) && CmdParamsField)
			? *CmdParamsField : MakeShared<FJsonObject>();

		FBridgeResult CmdResult = DispatchCommand(CmdDomain, CmdAction, CmdParams);
		if (CmdResult.bSuccess) SuccessCount++;

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultToJson(CmdResult)));
	}

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("total"),   (double)Commands.Num());
	Summary->SetNumberField(TEXT("success"), (double)SuccessCount);
	Summary->SetNumberField(TEXT("fail"),    (double)(Commands.Num() - SuccessCount));

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField  (TEXT("ok"),      SuccessCount == Commands.Num());
	Response->SetArrayField (TEXT("results"), ResultsArray);
	Response->SetObjectField(TEXT("summary"), Summary);
	return Response;
}

bool FBridgeHttpServer::ValidateAuth(const FHttpServerRequest& Request)
{
	const TArray<FString>* TokenValues = Request.Headers.Find(TEXT("X-Bridge-Token"));
	return TokenValues && TokenValues->Num() > 0 && (*TokenValues)[0] == AuthToken;
}

TSharedPtr<FJsonObject> FBridgeHttpServer::ResultToJson(const FBridgeResult& Result) const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("ok"), Result.bSuccess);
	Json->SetStringField(TEXT("message"), Result.Message);
	Json->SetStringField(TEXT("domain"), Result.Domain);
	Json->SetStringField(TEXT("action"), Result.Action);
	Json->SetNumberField(TEXT("error_code"), Result.ErrorCode);

	if (!Result.AffectedPath.IsEmpty())
	{
		Json->SetStringField(TEXT("affected_path"), Result.AffectedPath);
	}
	if (!Result.RecoveryHint.IsEmpty())
	{
		Json->SetStringField(TEXT("recovery_hint"), Result.RecoveryHint);
	}
	if (Result.Data.IsValid())
	{
		Json->SetObjectField(TEXT("data"), Result.Data);
	}
	else if (!Result.ExtraData.IsEmpty())
	{
		// ExtraData is a legacy raw JSON string — parse and inject as "data"
		TSharedPtr<FJsonValue> ExtraVal;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.ExtraData);
		if (FJsonSerializer::Deserialize(Reader, ExtraVal) && ExtraVal.IsValid())
		{
			Json->SetField(TEXT("data"), ExtraVal);
		}
		else
		{
			// Plain text (e.g. git output) — wrap as string
			Json->SetStringField(TEXT("data"), Result.ExtraData);
		}
	}

	return Json;
}

FBridgeResult FBridgeHttpServer::DispatchCommand(const FString& Domain, const FString& Action, TSharedPtr<FJsonObject> Params)
{
	// ---- System domain: handled internally ----
	if (Domain == TEXT("system"))
	{
		return HandleSystemCommand(Action, Params);
	}

	// ---- Global delete intercept — always quarantine destructive ops ----
	if (Action == TEXT("delete") || Action == TEXT("remove") || Action == TEXT("destroy"))
	{
		FString Path, Reason;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("path"), Path);
			Params->TryGetStringField(TEXT("reason"), Reason);
		}
		return Subsystem->Quarantine->QuarantineAsset(Path, Reason);
	}

	// ---- Map-based domain dispatch ----
	UBridgeHandlerBase* Handler = Subsystem->GetHandler(Domain);
	if (Handler)
	{
		return Handler->HandleCommand(Action, Params);
	}

	// ---- Unknown domain ----
	FBridgeResult Result;
	Result.Domain    = Domain;
	Result.Action    = Action;
	Result.bSuccess  = false;
	Result.ErrorCode = 5000;
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();
	Result.Message   = FString::Printf(TEXT("Domain '%s' is not registered. Use system/capabilities to list available domains."), *Domain);
	Result.RecoveryHint = TEXT("system/capabilities");
	return Result;
}

FBridgeResult FBridgeHttpServer::HandleSystemCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result;
	Result.Domain = TEXT("system");
	Result.Action = Action;
	Result.Timestamp = FDateTime::UtcNow().ToIso8601();

	// ---- ping ----
	if (Action == TEXT("ping"))
	{
		Result.bSuccess = true;
		Result.Message = TEXT("pong");
		return Result;
	}

	// ---- capabilities ----
	if (Action == TEXT("capabilities"))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

		// System actions
		TArray<TSharedPtr<FJsonValue>> SystemActions;
		for (const FString& A : TArray<FString>{
			TEXT("ping"), TEXT("capabilities"), TEXT("describe"), TEXT("describe_all"), TEXT("batch"), TEXT("undo"), TEXT("redo"),
			TEXT("get_editor_state"), TEXT("get_last_error"), TEXT("save_all"),
			TEXT("health_check"), TEXT("export_all_captures")})
		{
			SystemActions.Add(MakeShared<FJsonValueString>(A));
		}

		TSharedPtr<FJsonObject> SystemDomain = MakeShared<FJsonObject>();
		SystemDomain->SetArrayField(TEXT("actions"), SystemActions);
		Data->SetObjectField(TEXT("system"), SystemDomain);

		// All registered domains
		for (const auto& Pair : Subsystem->GetHandlerMap())
		{
			TSharedPtr<FJsonObject> DomainObj = MakeShared<FJsonObject>();
			TArray<FString> Actions = Pair.Value->GetSupportedActions();

			TArray<TSharedPtr<FJsonValue>> ActionValues;
			for (const FString& A : Actions)
			{
				ActionValues.Add(MakeShared<FJsonValueString>(A));
			}
			DomainObj->SetArrayField(TEXT("actions"), ActionValues);
			Data->SetObjectField(Pair.Key, DomainObj);
		}

		Result.bSuccess = true;
		Result.Message = FString::Printf(TEXT("%d domains registered."), Subsystem->GetHandlerMap().Num() + 1);
		Result.Data = Data;
		return Result;
	}

	// ---- batch ----
	if (Action == TEXT("batch"))
	{
		if (!Params.IsValid() || !Params->HasField(TEXT("commands")))
		{
			Result.bSuccess = false;
			Result.ErrorCode = 1000;
			Result.Message = TEXT("batch requires 'commands' array in params.");
			Result.RecoveryHint = TEXT("params.commands = [{domain, action, params}, ...] or POST /batch with a top-level array");
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>& Commands = Params->GetArrayField(TEXT("commands"));
		TSharedPtr<FJsonObject> BatchResponse = ExecuteBatchCommands(Commands);

		const TSharedPtr<FJsonObject>* SummaryPtr = nullptr;
		BatchResponse->TryGetObjectField(TEXT("summary"), SummaryPtr);
		int32 Total   = SummaryPtr ? (int32)(*SummaryPtr)->GetNumberField(TEXT("total"))   : Commands.Num();
		int32 Success = SummaryPtr ? (int32)(*SummaryPtr)->GetNumberField(TEXT("success")) : 0;

		Result.bSuccess = BatchResponse->GetBoolField(TEXT("ok"));
		Result.Message  = FString::Printf(TEXT("Batch: %d/%d succeeded."), Success, Total);
		Result.Data     = BatchResponse;
		return Result;
	}

	// ---- undo ----
	if (Action == TEXT("undo"))
	{
#if WITH_EDITOR
		if (GEditor && GEditor->UndoTransaction())
		{
			Result.bSuccess = true;
			Result.Message = TEXT("Undo successful.");
		}
		else
		{
			Result.bSuccess = false;
			Result.ErrorCode = 3000;
			Result.Message = TEXT("Nothing to undo.");
		}
#else
		Result.bSuccess = false;
		Result.ErrorCode = 3000;
		Result.Message = TEXT("Undo requires editor context.");
#endif
		return Result;
	}

	// ---- redo ----
	if (Action == TEXT("redo"))
	{
#if WITH_EDITOR
		if (GEditor && GEditor->RedoTransaction())
		{
			Result.bSuccess = true;
			Result.Message = TEXT("Redo successful.");
		}
		else
		{
			Result.bSuccess = false;
			Result.ErrorCode = 3000;
			Result.Message = TEXT("Nothing to redo.");
		}
#else
		Result.bSuccess = false;
		Result.ErrorCode = 3000;
		Result.Message = TEXT("Redo requires editor context.");
#endif
		return Result;
	}

	// ---- describe_all — alias for describe with no domain filter ----
	if (Action == TEXT("describe_all"))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		for (const auto& Pair : Subsystem->GetHandlerMap())
		{
			TSharedPtr<FJsonObject> Schema = Pair.Value->GetActionSchemas();
			if (Schema.IsValid())
				Data->SetObjectField(Pair.Key, Schema);
		}
		Result.bSuccess = true;
		Result.Message  = FString::Printf(TEXT("Full schema for all %d registered domains."),
			Subsystem->GetHandlerMap().Num());
		Result.Data = Data;
		return Result;
	}

	// ---- describe ----
	if (Action == TEXT("describe"))
	{
		FString DomainName;
		if (Params.IsValid()) Params->TryGetStringField(TEXT("domain"), DomainName);

		if (DomainName.IsEmpty())
		{
			// Return all domains with schemas
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			for (const auto& Pair : Subsystem->GetHandlerMap())
			{
				TSharedPtr<FJsonObject> Schema = Pair.Value->GetActionSchemas();
				if (Schema.IsValid())
				{
					Data->SetObjectField(Pair.Key, Schema);
				}
			}
			Result.bSuccess = true;
			Result.Message = TEXT("Action schemas for all domains with documentation.");
			Result.Data = Data;
		}
		else
		{
			UBridgeHandlerBase* Handler = Subsystem->GetHandler(DomainName);
			if (!Handler)
			{
				Result.bSuccess = false;
				Result.ErrorCode = 2000;
				Result.Message = FString::Printf(TEXT("Domain '%s' not found"), *DomainName);
				Result.RecoveryHint = TEXT("system/capabilities");
			}
			else
			{
				TSharedPtr<FJsonObject> Schema = Handler->GetActionSchemas();
				Result.bSuccess = true;
				Result.Message = FString::Printf(TEXT("Schema for domain '%s'"), *DomainName);
				Result.Data = Schema.IsValid() ? Schema : MakeShared<FJsonObject>();
			}
		}
		return Result;
	}

	// ---- get_editor_state ----
	if (Action == TEXT("get_editor_state"))
	{
#if WITH_EDITOR
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

		if (GEditor)
		{
			UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
			Data->SetStringField(TEXT("current_level"), World ? World->GetPathName() : TEXT("none"));
			Data->SetBoolField(TEXT("pie_running"), GEditor->IsPlayingSessionInEditor());
			Data->SetNumberField(TEXT("selected_actors"), (double)GEditor->GetSelectedActorCount());

			// Dirty packages
			TArray<TSharedPtr<FJsonValue>> DirtyArray;
			TArray<UPackage*> DirtyPackages;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				if (It->IsDirty() && !It->GetName().StartsWith(TEXT("/Temp/")))
				{
					DirtyArray.Add(MakeShared<FJsonValueString>(It->GetName()));
					if (DirtyArray.Num() >= 50) break; // Cap output
				}
			}
			Data->SetArrayField(TEXT("dirty_packages"), DirtyArray);
			Data->SetNumberField(TEXT("dirty_package_count"), (double)DirtyArray.Num());

			// Domain count
			Data->SetNumberField(TEXT("registered_domains"), (double)Subsystem->GetHandlerMap().Num());
		}

		Result.bSuccess = true;
		Result.Message = TEXT("Editor state retrieved.");
		Result.Data = Data;
#else
		Result.bSuccess = false;
		Result.ErrorCode = 3000;
		Result.Message = TEXT("get_editor_state requires editor context.");
#endif
		return Result;
	}

	// ---- get_last_error ----
	if (Action == TEXT("get_last_error"))
	{
		// Retrieve last error from the PIE message log
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

		FMessageLog PIELog(TEXT("PIE"));
		// Note: FMessageLog doesn't expose a "get last" API directly.
		// We provide what we can — the last output log error is captured
		// via GLog if available. For now, return a pointer to check Output Log.
		Data->SetStringField(TEXT("hint"), TEXT("Check Output Log (Window > Developer Tools > Output Log) for detailed error messages."));
		Data->SetStringField(TEXT("log_category"), TEXT("PIE"));

		Result.bSuccess = true;
		Result.Message = TEXT("Use Output Log for detailed error information.");
		Result.Data = Data;
		return Result;
	}

	// ---- save_all ----
	if (Action == TEXT("save_all"))
	{
		bool bSaved = FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave=*/false, /*bSaveMapPackages=*/true, /*bSaveContentPackages=*/true);
		if (bSaved)
		{
			Result.bSuccess = true;
			Result.Message = TEXT("All dirty packages saved.");
		}
		else
		{
			Result.bSuccess = false;
			Result.ErrorCode = 3000;
			Result.Message = TEXT("Some packages failed to save.");
		}
		return Result;
	}

	// ---- health_check ----
	if (Action == TEXT("health_check"))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Issues;

		// Output directory
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const bool bDirExists   = PF.DirectoryExists(*OutputDirectory);
		const bool bDirWritable = bDirExists && [&]() -> bool
		{
			const FString TestFile = OutputDirectory / TEXT(".bridge_health_write_test");
			FFileHelper::SaveStringToFile(TEXT("ok"), *TestFile);
			const bool bOk = PF.FileExists(*TestFile);
			if (bOk) PF.DeleteFile(*TestFile);
			return bOk;
		}();

		Data->SetStringField(TEXT("output_dir"), OutputDirectory);
		Data->SetBoolField  (TEXT("output_dir_exists"),   bDirExists);
		Data->SetBoolField  (TEXT("output_dir_writable"),  bDirWritable);
		if (!bDirWritable) Issues.Add(MakeShared<FJsonValueString>(TEXT("output_dir not writable")));

		// Domain handler count
		Data->SetNumberField(TEXT("domains"), (double)(Subsystem->GetHandlerMap().Num() + 1));

		// Attention manager
		const bool bAttnOk = Subsystem->AttentionManager != nullptr;
		Data->SetBoolField(TEXT("attention_manager"), bAttnOk);
		if (!bAttnOk) Issues.Add(MakeShared<FJsonValueString>(TEXT("AttentionManager is null")));

		// Captures — check each UPROPERTY for null
		// Template lambda: accepts both raw T* and TObjectPtr<T> without two-step implicit conversion
		auto CheckCapture = [&]<typename T>(const FString& Name, T Cap)
		{
			const bool bOk = (Cap != nullptr);
			Data->SetBoolField(Name, bOk);
			if (!bOk) Issues.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s capture is null"), *Name)));
		};
		CheckCapture(TEXT("gas"),            Subsystem->GASCapture);
		CheckCapture(TEXT("heightmap"),      Subsystem->HeightmapCapture);
		CheckCapture(TEXT("blueprint"),      Subsystem->BlueprintCapture);
		CheckCapture(TEXT("material"),       Subsystem->MaterialCapture);
		CheckCapture(TEXT("niagara"),        Subsystem->NiagaraCapture);
		CheckCapture(TEXT("umg"),            Subsystem->UMGCapture);
		CheckCapture(TEXT("animation"),      Subsystem->AnimationCapture);
		CheckCapture(TEXT("input"),          Subsystem->InputCapture);
		CheckCapture(TEXT("datatable"),      Subsystem->DataTableCapture);
		CheckCapture(TEXT("weather"),        Subsystem->WeatherCapture);
		CheckCapture(TEXT("worldgen"),       Subsystem->WorldGenCapture);
		CheckCapture(TEXT("symbols"),        Subsystem->SymbolCapture);
		CheckCapture(TEXT("performance"),    Subsystem->PerformanceCapture);
		CheckCapture(TEXT("network"),        Subsystem->NetworkCapture);
		CheckCapture(TEXT("asset_registry"), Subsystem->AssetRegistryCapture);
		CheckCapture(TEXT("screenshot"),     Subsystem->ScreenshotCapture);
		CheckCapture(TEXT("pcg"),            Subsystem->PCGCapture);
		CheckCapture(TEXT("build"),          Subsystem->BuildCapture);
		CheckCapture(TEXT("runtime"),        Subsystem->RuntimeCapture);
		CheckCapture(TEXT("command_channel"),Subsystem->CommandChannel);

#if WITH_EDITOR
		Data->SetBoolField(TEXT("pie_active"), GEditor && GEditor->IsPlayingSessionInEditor());
#else
		Data->SetBoolField(TEXT("pie_active"), false);
#endif

		Data->SetArrayField(TEXT("issues"), Issues);

		Result.bSuccess = (Issues.Num() == 0);
		Result.Message  = Issues.Num() == 0
			? TEXT("Bridge is healthy.")
			: FString::Printf(TEXT("Bridge has %d issue(s)."), Issues.Num());
		Result.Data = Data;
		return Result;
	}

	// ---- export_all_captures ----
	if (Action == TEXT("export_all_captures"))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		int32 Succeeded = 0, Failed = 0;

		auto Record = [&](const FString& Name, bool bOk)
		{
			Data->SetBoolField(Name, bOk);
			bOk ? ++Succeeded : ++Failed;
		};

		if (Subsystem->GASCapture)           Record(TEXT("gas"),            Subsystem->GASCapture->ExportGASAudit());
		if (Subsystem->HeightmapCapture)     Record(TEXT("heightmap"),      Subsystem->HeightmapCapture->ExportHeightmap());
		if (Subsystem->BlueprintCapture)     Record(TEXT("blueprints"),     Subsystem->BlueprintCapture->ExportAllBlueprints() >= 0);
		if (Subsystem->MaterialCapture)      Record(TEXT("materials"),      Subsystem->MaterialCapture->ExportAllMaterials() >= 0);
		if (Subsystem->NiagaraCapture)       Record(TEXT("niagara"),        Subsystem->NiagaraCapture->ExportAllNiagaraSystems() >= 0);
		if (Subsystem->AnimationCapture)     Record(TEXT("animation"),      Subsystem->AnimationCapture->ExportAnimationData());
		if (Subsystem->InputCapture)         Record(TEXT("input"),          Subsystem->InputCapture->ExportInputAudit());
		if (Subsystem->DataTableCapture)     Record(TEXT("datatables"),     Subsystem->DataTableCapture->ExportAllDataTables() >= 0);
		if (Subsystem->WeatherCapture)       Record(TEXT("weather"),        Subsystem->WeatherCapture->ExportWeatherState());
		if (Subsystem->WorldGenCapture)      Record(TEXT("worldgen"),       Subsystem->WorldGenCapture->ExportWorldGenState());
		if (Subsystem->SymbolCapture)        Record(TEXT("symbols"),        Subsystem->SymbolCapture->ExportSymbolIndex());
		if (Subsystem->PerformanceCapture)   Record(TEXT("performance"),    Subsystem->PerformanceCapture->ExportPerformanceSnapshot());
		if (Subsystem->NetworkCapture)       Record(TEXT("network"),        Subsystem->NetworkCapture->ExportNetworkAudit());
		if (Subsystem->AssetRegistryCapture) Record(TEXT("asset_registry"), Subsystem->AssetRegistryCapture->ExportAssetRegistry() >= 0);

		Result.bSuccess = (Failed == 0);
		Result.Message  = FString::Printf(TEXT("Captured %d/%d. Output: %s"),
			Succeeded, Succeeded + Failed, *OutputDirectory);
		Result.Data = Data;
		return Result;
	}

	// ---- Unknown system action ----
	Result.bSuccess = false;
	Result.ErrorCode = 1001;
	Result.Message = FString::Printf(TEXT("Unknown system action '%s'. Available: ping, capabilities, describe, describe_all, batch, undo, redo, get_editor_state, get_last_error, save_all, health_check, export_all_captures."), *Action);
	Result.RecoveryHint = TEXT("system/capabilities");
	return Result;
}

void FBridgeHttpServer::UpdateStatus(const FString& Domain, const FString& Action)
{
	// Periodic status update could go here
}
