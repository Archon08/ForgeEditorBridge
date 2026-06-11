#include "Handlers/PCGHandler.h"
#include "ForgeAISubsystem.h"
#include "PCGComponent.h"
#include "Editor.h"
#include "EngineUtils.h"

// ---------------------------------------------------------------------------
// HandleCommand dispatch
// ---------------------------------------------------------------------------

FBridgeResult UPCGHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg"), Action);

	if (!Params.IsValid())
	{
		Result.Message = TEXT("Params object is null");
		Result.ErrorCode = 1000;
		return Result;
	}

	if (Action == TEXT("execute_graph"))    return ExecuteGraph(Params);
	if (Action == TEXT("set_pcg_parameter")) return SetPCGParameter(Params);

	Result.Message = FString::Printf(
		TEXT("Unknown PCG action '%s'. Supported: execute_graph, set_pcg_parameter"), *Action);
	Result.ErrorCode = 1001;
	return Result;
}

// ---------------------------------------------------------------------------
// execute_graph
// ---------------------------------------------------------------------------

FBridgeResult UPCGHandler::ExecuteGraph(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg"), TEXT("execute_graph"));

	FString ActorPath;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		Result.Message = TEXT("execute_graph requires: actor_path");
		Result.ErrorCode = 1000;
		return Result;
	}

	FString ComponentName;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);

	if (!GEditor)
	{
		Result.Message = TEXT("GEditor is null — execute_graph requires the editor");
		Result.ErrorCode = 3000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("No editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	AActor* TargetActor = FindActorByPath(World, ActorPath);
	if (!TargetActor)
	{
		Result.Message = FString::Printf(TEXT("Actor not found: %s"), *ActorPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the actor label, name, or path in the current level.");
		return Result;
	}

	// Locate the PCG component — by name if provided, otherwise the first one found
	UPCGComponent* PCGComp = nullptr;
	if (!ComponentName.IsEmpty())
	{
		TArray<UPCGComponent*> Comps;
		TargetActor->GetComponents<UPCGComponent>(Comps);
		for (UPCGComponent* Comp : Comps)
		{
			if (Comp && Comp->GetName() == ComponentName)
			{
				PCGComp = Comp;
				break;
			}
		}
	}
	else
	{
		PCGComp = TargetActor->FindComponentByClass<UPCGComponent>();
	}

	if (!PCGComp)
	{
		Result.Message = ComponentName.IsEmpty()
			? FString::Printf(TEXT("No PCGComponent on actor '%s'"), *TargetActor->GetActorLabel())
			: FString::Printf(TEXT("PCGComponent '%s' not found on actor '%s'"),
				*ComponentName, *TargetActor->GetActorLabel());
		Result.ErrorCode = 2000;
		return Result;
	}

	PCGComp->GenerateLocal(/*bForce=*/false);

	Result.bSuccess = true;
	Result.AffectedPath = ActorPath;
	Result.Message = FString::Printf(TEXT("PCG graph generation triggered on '%s' (component: %s)"),
		*TargetActor->GetActorLabel(), *PCGComp->GetName());
	return Result;
}

// ---------------------------------------------------------------------------
// set_pcg_parameter
// ---------------------------------------------------------------------------

FBridgeResult UPCGHandler::SetPCGParameter(TSharedPtr<FJsonObject> Params)
{
	FBridgeResult Result = CreateResult(TEXT("pcg"), TEXT("set_pcg_parameter"));

	FString ActorPath, ParamName, Value;
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) ||
		!Params->TryGetStringField(TEXT("param_name"), ParamName) ||
		!Params->TryGetStringField(TEXT("value"), Value))
	{
		Result.Message = TEXT("set_pcg_parameter requires: actor_path, param_name, value");
		Result.ErrorCode = 1000;
		return Result;
	}

	if (!GEditor)
	{
		Result.Message = TEXT("GEditor is null");
		Result.ErrorCode = 3000;
		return Result;
	}

	UWorld* World = UBridgeHandlerBase::GetSafeEditorWorld();
	if (!World)
	{
		Result.Message = TEXT("No editor world available");
		Result.ErrorCode = 3000;
		return Result;
	}

	AActor* TargetActor = FindActorByPath(World, ActorPath);
	if (!TargetActor)
	{
		Result.Message = FString::Printf(TEXT("Actor not found: %s"), *ActorPath);
		Result.ErrorCode = 2000;
		Result.RecoveryHint = TEXT("Verify the actor label, name, or path in the current level.");
		return Result;
	}

	UPCGComponent* PCGComp = TargetActor->FindComponentByClass<UPCGComponent>();

	// Try the PCGComponent first, then fall back to the actor itself
	UObject* Target = (PCGComp != nullptr) ? static_cast<UObject*>(PCGComp) : static_cast<UObject*>(TargetActor);
	FString Error = SetReflectedProperty(Target, ParamName, Value);

	if (!Error.IsEmpty() && PCGComp)
	{
		// PCGComponent didn't have it — try the actor directly
		Error = SetReflectedProperty(TargetActor, ParamName, Value);
	}

	if (!Error.IsEmpty())
	{
		Result.Message = FString::Printf(TEXT("Property '%s' not found on PCGComponent or actor '%s': %s"),
			*ParamName, *TargetActor->GetActorLabel(), *Error);
		Result.ErrorCode = 2000;
		return Result;
	}

	Result.bSuccess = true;
	Result.AffectedPath = ActorPath;
	Result.Message = FString::Printf(TEXT("Set '%s' = '%s' on '%s'"),
		*ParamName, *Value, *TargetActor->GetActorLabel());
	return Result;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AActor* UPCGHandler::FindActorByPath(UWorld* World, const FString& ActorPath) const
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		if (Actor->GetActorLabel() == ActorPath ||
			Actor->GetName()       == ActorPath ||
			Actor->GetPathName()   == ActorPath)
		{
			return Actor;
		}
	}
	return nullptr;
}

FString UPCGHandler::SetReflectedProperty(UObject* Target, const FString& PropName, const FString& Value) const
{
	if (!Target) return TEXT("Target object is null");

	FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Target->GetClass()->GetName());

	void* Container = Target;

	if (FFloatProperty* PFloat = CastField<FFloatProperty>(Prop))
	{
		PFloat->SetPropertyValue_InContainer(Container, FCString::Atof(*Value));
	}
	else if (FDoubleProperty* PDouble = CastField<FDoubleProperty>(Prop))
	{
		PDouble->SetPropertyValue_InContainer(Container, (double)FCString::Atof(*Value));
	}
	else if (FIntProperty* PInt = CastField<FIntProperty>(Prop))
	{
		PInt->SetPropertyValue_InContainer(Container, FCString::Atoi(*Value));
	}
	else if (FInt64Property* PInt64 = CastField<FInt64Property>(Prop))
	{
		PInt64->SetPropertyValue_InContainer(Container, (int64)FCString::Atoi64(*Value));
	}
	else if (FBoolProperty* PBool = CastField<FBoolProperty>(Prop))
	{
		PBool->SetPropertyValue_InContainer(Container, Value.ToBool());
	}
	else if (FStrProperty* PStr = CastField<FStrProperty>(Prop))
	{
		PStr->SetPropertyValue_InContainer(Container, Value);
	}
	else if (FNameProperty* PName = CastField<FNameProperty>(Prop))
	{
		PName->SetPropertyValue_InContainer(Container, FName(*Value));
	}
	else
	{
		return FString::Printf(TEXT("Property '%s' has unsupported type '%s' for string conversion"),
			*PropName, *Prop->GetClass()->GetName());
	}

	Target->MarkPackageDirty();
	return FString(); // empty = success
}
