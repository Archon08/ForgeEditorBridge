#include "Handlers/SubsystemQueryHandler.h"
#include "ForgeAISubsystem.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#endif

#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Windows <errno.h> defines DOMAIN as 1
#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("subsystem_query");

FBridgeResult USubsystemQueryHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
	if (Action == TEXT("list_editor_subsystems"))
	{
#if WITH_EDITOR
		TArray<UClass*> SubsystemClasses;
		GetDerivedClasses(UEditorSubsystem::StaticClass(), SubsystemClasses, true);

		TArray<TSharedPtr<FJsonValue>> SubsystemArray;
		for (UClass* Class : SubsystemClasses)
		{
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), Class->GetName());
			SubsystemArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("subsystems"), SubsystemArray);
		Data->SetNumberField(TEXT("count"), SubsystemArray.Num());

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Found %d editor subsystem class(es)"), SubsystemArray.Num()), Data);
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("Editor subsystems are only available in editor builds"), TEXT("Run from an editor build"));
#endif
	}

	if (Action == TEXT("list_world_subsystems"))
	{
#if WITH_EDITOR
		if (!GEditor)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("GEditor is not available"), TEXT("Ensure the editor is running"));
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return MakeError(DOMAIN, Action, 3000, TEXT("No editor world context available"), TEXT("Open a level in the editor"));
		}

		TArray<UClass*> SubsystemClasses;
		GetDerivedClasses(UWorldSubsystem::StaticClass(), SubsystemClasses, true);

		TArray<TSharedPtr<FJsonValue>> SubsystemArray;
		for (UClass* Class : SubsystemClasses)
		{
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), Class->GetName());
			SubsystemArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("subsystems"), SubsystemArray);
		Data->SetNumberField(TEXT("count"), SubsystemArray.Num());

		return MakeSuccess(DOMAIN, Action,
			FString::Printf(TEXT("Found %d world subsystem class(es)"), SubsystemArray.Num()), Data);
#else
		return MakeError(DOMAIN, Action, 3000, TEXT("World subsystems query requires an editor build"), TEXT("Run from an editor build"));
#endif
	}

	return MakeError(DOMAIN, Action, 1001,
		FString::Printf(TEXT("Unknown action '%s'"), *Action), TEXT("subsystem_query capabilities"));
}

TSharedPtr<FJsonObject> USubsystemQueryHandler::GetActionSchemas() const
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

	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all non-abstract UEditorSubsystem classes")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("list_editor_subsystems"), A); }
	{ TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("desc"), TEXT("List all non-abstract UWorldSubsystem classes")); A->SetObjectField(TEXT("params"), MakeShared<FJsonObject>()); Root->SetObjectField(TEXT("list_world_subsystems"), A); }

	return Root;
}
