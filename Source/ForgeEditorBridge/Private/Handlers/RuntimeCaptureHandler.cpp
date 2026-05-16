#include "Handlers/RuntimeCaptureHandler.h"
#include "ForgeAISubsystem.h"
#include "Capture/ForgeRuntimeCapture.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- Actor iteration -------------------------------------------------------
#include "EngineUtils.h"            // TActorIterator

// ---- GAS (optional tags capture) -------------------------------------------
#include "AbilitySystemComponent.h" // UAbilitySystemComponent (GameplayAbilities — already in Build.cs)
#include "AbilitySystemInterface.h" // IAbilitySystemInterface

// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

// ---- Engine ----------------------------------------------------------------
#include "Engine/Engine.h"          // GEngine, FWorldContext
#include "Engine/World.h"

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void URuntimeCaptureHandler::Initialize(UBridgeSubsystem* InSubsystem)
{
	Super::Initialize(InSubsystem);
}

// ---------------------------------------------------------------------------
// HandleCommand — dispatch
// ---------------------------------------------------------------------------

FBridgeResult URuntimeCaptureHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		FBridgeResult R = CreateResult(TEXT("runtime_capture"), Action);
		R.Message = TEXT("Params object is null");
		R.ErrorCode = 1000;
		return R;
	}

	if (Action == TEXT("capture_pie_state"))        return Action_CapturePIEState(Params);
	if (Action == TEXT("capture_runtime_variables")) return Action_CaptureRuntimeVariables(Params);

	FBridgeResult R = CreateResult(TEXT("runtime_capture"), Action);
	R.Message = FString::Printf(
		TEXT("Unknown runtime_capture action '%s'. Valid: capture_pie_state, capture_runtime_variables"), *Action);
	R.ErrorCode = 1001;
	return R;
}

// ---------------------------------------------------------------------------
// capture_pie_state
// ---------------------------------------------------------------------------

FBridgeResult URuntimeCaptureHandler::Action_CapturePIEState(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("runtime_capture"), TEXT("capture_pie_state"));

	double MaxActorsD = 200.0;
	bool   bIncludeTags = true;
	Params->TryGetNumberField(TEXT("max_actors"),   MaxActorsD);
	Params->TryGetBoolField(TEXT("include_tags"),   bIncludeTags);
	const int32 MaxActors = FMath::Max(1, (int32)MaxActorsD);

	// Find the active PIE world
	UWorld* PIEWorld = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				PIEWorld = Ctx.World();
				break;
			}
		}
	}

	if (!PIEWorld)
	{
		Result.Message = TEXT("capture_pie_state: no PIE world is running. Press Play in the editor first.");
		Result.ErrorCode = 3004;
		Result.RecoveryHint = TEXT("Press Play in the editor to start a PIE session first.");
		return Result;
	}

	// Build the JSON snapshot
	TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("captured_at"), FDateTime::UtcNow().ToIso8601());
	Snapshot->SetStringField(TEXT("level"), PIEWorld->GetCurrentLevel()
		? PIEWorld->GetCurrentLevel()->GetOutermost()->GetName() : TEXT("unknown"));

	TArray<TSharedPtr<FJsonValue>> ActorArray;

	for (TActorIterator<AActor> It(PIEWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKillPending()) continue;

		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"),  Actor->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

		// Location
		const FVector Loc = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		ActorObj->SetObjectField(TEXT("location"), LocObj);

		// Rotation
		const FRotator Rot = Actor->GetActorRotation();
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
		ActorObj->SetObjectField(TEXT("rotation"), RotObj);

		// GAS gameplay tags (if ASC present and include_tags=true)
		if (bIncludeTags)
		{
			UAbilitySystemComponent* ASC = nullptr;
			if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor))
			{
				ASC = ASI->GetAbilitySystemComponent();
			}
			if (!ASC)
			{
				ASC = Actor->FindComponentByClass<UAbilitySystemComponent>();
			}
			if (ASC)
			{
				FGameplayTagContainer Tags;
				ASC->GetOwnedGameplayTags(Tags);
				TArray<TSharedPtr<FJsonValue>> TagValues;
				for (const FGameplayTag& Tag : Tags)
				{
					TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				ActorObj->SetArrayField(TEXT("gameplay_tags"), TagValues);
			}
		}

		ActorArray.Add(MakeShared<FJsonValueObject>(ActorObj));
		if (ActorArray.Num() >= MaxActors) break;
	}

	Snapshot->SetArrayField(TEXT("actors"),      ActorArray);
	Snapshot->SetNumberField(TEXT("actor_count"), (double)ActorArray.Num());

	// Serialise to string and return in Message
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Snapshot.ToSharedRef(), Writer);

	Result.bSuccess = true;
	Result.Message  = JsonString;
	return Result;
}

// ---------------------------------------------------------------------------
// capture_runtime_variables
// ---------------------------------------------------------------------------

FBridgeResult URuntimeCaptureHandler::Action_CaptureRuntimeVariables(TSharedPtr<FJsonObject> Params)
{
	if (Subsystem && Subsystem->RuntimeCapture)
		Subsystem->RuntimeCapture->CaptureVariableSnapshot();
	FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("runtime/variables.json"));
	FString FileContent;
	FBridgeResult Res = MakeSuccess(TEXT("runtime_capture"), TEXT("capture_runtime_variables"), TEXT("Runtime variable snapshot captured"));
	if (FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
			Res.Data = JsonObj;
	}
	return Res;
}
