#pragma once
#include "Handlers/BridgeHandlerBase.h"
#include "GameplayTagHandler.generated.h"

/**
 * GameplayTagHandler — domain "gameplay_tag"  (v0.8.0 / UE 5.7)
 *
 * Manages the project's Gameplay Tag tree through the Bridge.
 * Tags are stored in the project's DefaultGameplayTags.ini (or a named source).
 *
 * Actions:
 *   add_tag_to_config  → tag (string), comment (string, optional),
 *                        source (string, optional — INI source name, default = "DefaultGameplayTags.ini")
 *                        Adds a new tag to the specified INI source via UGameplayTagsManager.
 *
 *   rename_tag         → old_name (string), new_name (string)
 *                        Renames a tag in its INI source.
 *
 *   list_tags          → prefix (string, optional — filter by prefix; empty = all)
 *                        Returns all registered Gameplay Tags as a JSON array in ExtraData.
 *
 *   remove_tag         → tag (string)
 *                        Removes a tag entry from DefaultGameplayTags.ini and refreshes the tree.
 *                        Note: existing asset references are not redirected — use rename_tag for that.
 *
 *   validate_tags      → tags (string[])
 *                        Checks each tag; returns per-tag valid/invalid status and any typo suggestions.
 *
 *   get_tag_hierarchy  → root (string, optional — root tag prefix; omit for full tree)
 *                        Returns the tag tree as a nested JSON object.
 */
UCLASS()
class FORGEEDITORBRIDGE_API UGameplayTagHandler : public UBridgeHandlerBase
{
	GENERATED_BODY()

public:
	virtual void Initialize(UBridgeSubsystem* InSubsystem) override;
	virtual FString GetDomainName() const override { return TEXT("gameplay_tag"); }
	virtual TArray<FString> GetSupportedActions() const override { return { TEXT("add_tag_to_config"), TEXT("rename_tag"), TEXT("list_tags"), TEXT("remove_tag"), TEXT("validate_tags"), TEXT("get_tag_hierarchy") }; }
	virtual FBridgeResult HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params) override;

private:
	FBridgeResult Action_AddTagToConfig  (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RenameTag       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ListTags        (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_RemoveTag       (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_ValidateTags    (TSharedPtr<FJsonObject> Params);
	FBridgeResult Action_GetTagHierarchy (TSharedPtr<FJsonObject> Params);
};
