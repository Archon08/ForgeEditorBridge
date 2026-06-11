#include "Capture/ForgeCollisionCapture.h"
#include "IO/ForgeContextWriter.h"

// ---- Collision ---------------------------------------------------------------
#include "Engine/CollisionProfile.h"
#include "Components/PrimitiveComponent.h"

// ---- World / Actor iteration -------------------------------------------------
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// ---- Config ------------------------------------------------------------------
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

// ---- JSON + IO ---------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Static channel name table (indices 0-7 match ECollisionChannel built-ins)
// ---------------------------------------------------------------------------

static const TCHAR* GBuiltinChannelNames[] = {
	TEXT("WorldStatic"),    // ECC_WorldStatic  = 0
	TEXT("WorldDynamic"),   // ECC_WorldDynamic = 1
	TEXT("Pawn"),           // ECC_Pawn         = 2
	TEXT("Visibility"),     // ECC_Visibility   = 3
	TEXT("Camera"),         // ECC_Camera       = 4
	TEXT("PhysicsBody"),    // ECC_PhysicsBody  = 5
	TEXT("Vehicle"),        // ECC_Vehicle      = 6
	TEXT("Destructible"),   // ECC_Destructible = 7
};
static constexpr int32 GNumBuiltin = 8;
static constexpr int32 GMaxChannels = 32;   // ECC_MAX in UE5

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace
{
	FString CollisionEnabledStr(ECollisionEnabled::Type T)
	{
		switch (T)
		{
		case ECollisionEnabled::NoCollision:     return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly:       return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly:     return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
		default:                                  return TEXT("Unknown");
		}
	}

	FString CollisionResponseStr(ECollisionResponse R)
	{
		switch (R)
		{
		case ECR_Ignore:  return TEXT("Ignore");
		case ECR_Overlap: return TEXT("Overlap");
		case ECR_Block:   return TEXT("Block");
		default:          return TEXT("Unknown");
		}
	}

	// Build a channel name lookup table indexed 0..GMaxChannels-1.
	// Slots 0-7 use the hardcoded built-in names.
	// Slots 8+ are populated from GConfig DefaultChannelResponses.
	// Returns the names array and fills OutCustomNames with just the custom ones.
	TArray<FString> BuildChannelNameTable(TArray<FString>& OutCustomNames)
	{
		TArray<FString> Table;
		Table.SetNum(GMaxChannels);
		for (int32 i = 0; i < GNumBuiltin; ++i)
			Table[i] = GBuiltinChannelNames[i];
		for (int32 i = GNumBuiltin; i < GMaxChannels; ++i)
			Table[i] = FString::Printf(TEXT("GameTraceChannel%d"), i - GNumBuiltin + 1);

		TArray<FString> RawEntries;
		GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"),
			TEXT("DefaultChannelResponses"), RawEntries, GEngineIni);

		int32 CustomIdx = 0;
		for (const FString& Entry : RawEntries)
		{
			if (CustomIdx >= GMaxChannels - GNumBuiltin) break;

			// Parse Name="X" from the struct string
			FString Inner = Entry.TrimStartAndEnd();
			if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
				Inner = Inner.Mid(1, Inner.Len() - 2);

			FString ChannelName;
			TArray<FString> Parts;
			Inner.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				FString K, V;
				if (Part.Split(TEXT("="), &K, &V))
				{
					K.TrimStartAndEndInline();
					V.TrimStartAndEndInline();
					if (K == TEXT("Name"))
					{
						ChannelName = V.TrimQuotes();
						break;
					}
				}
			}

			if (!ChannelName.IsEmpty())
			{
				Table[GNumBuiltin + CustomIdx] = ChannelName;
				OutCustomNames.Add(ChannelName);
			}
			++CustomIdx;
		}

		return Table;
	}
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void UForgeCollisionCapture::Initialize(const FString& InOutputDir)
{
	OutputDir = InOutputDir;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*(OutputDir / TEXT("collision")));
	UE_LOG(LogTemp, Log, TEXT("ForgeCollision: Initialized"));
}

// ---------------------------------------------------------------------------
// ExportCollisionProfiles
// ---------------------------------------------------------------------------

bool UForgeCollisionCapture::ExportCollisionProfiles()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated"), FForgeContextWriter::NowISO8601());

	// Build channel name table once — shared by channels, profiles, and audit sections
	TArray<FString> CustomChannelNames;
	const TArray<FString> ChannelNames = BuildChannelNameTable(CustomChannelNames);

	// -------------------------------------------------------------------------
	// Channels section
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ChannelArr;

	// Built-in channels
	for (int32 i = 0; i < GNumBuiltin; ++i)
	{
		TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetNumberField(TEXT("index"),   i);
		CObj->SetStringField(TEXT("name"),    GBuiltinChannelNames[i]);
		CObj->SetBoolField  (TEXT("builtin"), true);
		ChannelArr.Add(MakeShared<FJsonValueObject>(CObj));
	}

	// Custom channels from ini
	{
		TArray<FString> RawEntries;
		GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"),
			TEXT("DefaultChannelResponses"), RawEntries, GEngineIni);

		int32 CustomIdx = 0;
		for (const FString& Entry : RawEntries)
		{
			FString Inner = Entry.TrimStartAndEnd();
			if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
				Inner = Inner.Mid(1, Inner.Len() - 2);

			FString ChannelName, DefaultResponse;
			TArray<FString> Parts;
			Inner.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				FString K, V;
				if (Part.Split(TEXT("="), &K, &V))
				{
					K.TrimStartAndEndInline();
					V.TrimStartAndEndInline();
					if (K == TEXT("Name"))             ChannelName = V.TrimQuotes();
					if (K == TEXT("DefaultResponse"))  DefaultResponse = V.TrimQuotes();
				}
			}

			TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
			CObj->SetNumberField(TEXT("index"),            GNumBuiltin + CustomIdx);
			CObj->SetStringField(TEXT("name"),             ChannelNames[GNumBuiltin + CustomIdx]);
			CObj->SetBoolField  (TEXT("builtin"),          false);
			CObj->SetStringField(TEXT("default_response"), DefaultResponse);
			ChannelArr.Add(MakeShared<FJsonValueObject>(CObj));
			++CustomIdx;
		}
	}

	Root->SetArrayField(TEXT("channels"), ChannelArr);

	// -------------------------------------------------------------------------
	// Profiles section + track channel references for CHANNEL_UNUSED
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ProfileArr;
	TSet<FString> ProfileReferencedChannels;

	UCollisionProfile* CP = UCollisionProfile::Get();
	if (CP)
	{
		TArray<TSharedPtr<FName>> ProfileNames;
		CP->GetProfileNames(ProfileNames);

		for (const TSharedPtr<FName>& NamePtr : ProfileNames)
		{
			if (!NamePtr.IsValid()) continue;

			FCollisionResponseTemplate Template;
			if (!CP->GetProfileTemplate(*NamePtr, Template)) continue;

			TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"),              Template.Name.ToString());
			PObj->SetStringField(TEXT("collision_enabled"), CollisionEnabledStr(Template.CollisionEnabled));
			PObj->SetStringField(TEXT("object_type"),       Template.ObjectTypeName.ToString());
			PObj->SetStringField(TEXT("help_message"),      Template.HelpMessage);

			// Per-channel responses — only emit non-Ignore entries to keep JSON tidy
			TArray<TSharedPtr<FJsonValue>> ResponseArr;
			int32 OverlapCount = 0;
			int32 BlockCount   = 0;

			for (int32 ci = 0; ci < GMaxChannels; ++ci)
			{
				const ECollisionResponse Resp = (ECollisionResponse)Template.ResponseToChannels.EnumArray[ci];
				if (Resp == ECR_Ignore) continue;

				const FString ChName = ChannelNames[ci];
				ProfileReferencedChannels.Add(ChName);

				TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
				RObj->SetNumberField(TEXT("channel_index"), ci);
				RObj->SetStringField(TEXT("channel"),       ChName);
				RObj->SetStringField(TEXT("response"),      CollisionResponseStr(Resp));
				ResponseArr.Add(MakeShared<FJsonValueObject>(RObj));

				if (Resp == ECR_Overlap) ++OverlapCount;
				if (Resp == ECR_Block)   ++BlockCount;
			}
			PObj->SetArrayField(TEXT("responses"), ResponseArr);

			// Profile-level audit flags
			TArray<TSharedPtr<FJsonValue>> FlagArr;
			const bool bCollisionOn = (Template.CollisionEnabled != ECollisionEnabled::NoCollision);
			if (bCollisionOn && BlockCount == 0 && OverlapCount > 0)
				FlagArr.Add(MakeShared<FJsonValueString>(TEXT("OVERLAP_ALL")));
			PObj->SetArrayField(TEXT("flags"), FlagArr);

			ProfileArr.Add(MakeShared<FJsonValueObject>(PObj));
		}
	}

	Root->SetArrayField(TEXT("profiles"), ProfileArr);

	// -------------------------------------------------------------------------
	// CHANNEL_UNUSED — custom channels defined in ini but not in any profile
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> UnusedArr;
	for (const FString& ChName : CustomChannelNames)
	{
		if (!ProfileReferencedChannels.Contains(ChName))
			UnusedArr.Add(MakeShared<FJsonValueString>(ChName));
	}
	Root->SetArrayField(TEXT("unused_channels"), UnusedArr);

	// -------------------------------------------------------------------------
	// Actor audit — scan current level (capped at 1000 actors for perf)
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ActorAuditArr;
	int32 OverlapAllActors    = 0;
	int32 MissingProfileActors = 0;
	int32 ScannedActors        = 0;
	static constexpr int32 MaxActorScan = 1000;

	if (GEditor && GEngine)
	{
		// GetEditorWorldContext() check(0)-asserts when no editor context exists —
		// use safe GEngine world iteration (same fix as ForgeHeightmapCapture).
		UWorld* World = nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Editor) { World = Ctx.World(); break; }
		}
		if (World)
		{
			for (TActorIterator<AActor> It(World); It && ScannedActors < MaxActorScan; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;

				UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
				if (!RootPrim) continue;
				if (RootPrim->GetCollisionEnabled() == ECollisionEnabled::NoCollision) continue;

				++ScannedActors;

				const FName ProfileFName = RootPrim->GetCollisionProfileName();
				const FString ProfileStr = ProfileFName.ToString();

				TArray<TSharedPtr<FJsonValue>> FlagArr;

				// MISSING_PROFILE
				if (ProfileStr == TEXT("Default") || ProfileStr == TEXT("Custom") || ProfileStr.IsEmpty())
				{
					FlagArr.Add(MakeShared<FJsonValueString>(TEXT("MISSING_PROFILE")));
					++MissingProfileActors;
				}

				// OVERLAP_ALL — check if all components on the actor overlap everything
				{
					TArray<UPrimitiveComponent*> Prims;
					Actor->GetComponents<UPrimitiveComponent>(Prims);
					bool bAllOverlap = !Prims.IsEmpty();
					for (UPrimitiveComponent* PC : Prims)
					{
						if (!PC || PC->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
							continue;
						const FCollisionResponseContainer& Resp = PC->GetCollisionResponseToChannels();
						for (int32 ci = 0; ci < GNumBuiltin; ++ci)
						{
							if ((ECollisionResponse)Resp.EnumArray[ci] != ECR_Overlap)
							{
								bAllOverlap = false;
								break;
							}
						}
						if (!bAllOverlap) break;
					}
					if (bAllOverlap && Prims.Num() > 0)
					{
						FlagArr.Add(MakeShared<FJsonValueString>(TEXT("OVERLAP_ALL")));
						++OverlapAllActors;
					}
				}

				if (FlagArr.Num() > 0)
				{
					TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
					AObj->SetStringField(TEXT("actor"),   Actor->GetActorNameOrLabel());
					AObj->SetStringField(TEXT("class"),   Actor->GetClass()->GetName());
					AObj->SetStringField(TEXT("profile"), ProfileStr);
					AObj->SetArrayField (TEXT("flags"),   FlagArr);
					ActorAuditArr.Add(MakeShared<FJsonValueObject>(AObj));
				}
			}
		}
	}

	Root->SetArrayField(TEXT("actor_audit"), ActorAuditArr);

	// -------------------------------------------------------------------------
	// Summary
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("total_profiles"),       ProfileArr.Num());
	Summary->SetNumberField(TEXT("total_channels"),       ChannelArr.Num());
	Summary->SetNumberField(TEXT("custom_channels"),      CustomChannelNames.Num());
	Summary->SetNumberField(TEXT("unused_channels"),      UnusedArr.Num());
	Summary->SetNumberField(TEXT("actors_scanned"),       ScannedActors);
	Summary->SetNumberField(TEXT("overlap_all_actors"),   OverlapAllActors);
	Summary->SetNumberField(TEXT("missing_profile"),      MissingProfileActors);
	Root->SetObjectField(TEXT("summary"), Summary);

	bool bOK = FForgeContextWriter::WriteJSON(OutputDir / TEXT("collision"), TEXT("profiles"), Root);
	if (bOK)
	{
		UE_LOG(LogTemp, Log, TEXT("ForgeCollision: Exported -> collision/profiles.json (%d profiles, %d channels)"),
			ProfileArr.Num(), ChannelArr.Num());
		UpdateIndexFile();
	}
	return bOK;
}

// ---------------------------------------------------------------------------
// UpdateIndexFile (READ-MERGE-WRITE)
// ---------------------------------------------------------------------------

void UForgeCollisionCapture::UpdateIndexFile()
{
	const FString IndexPath = OutputDir / TEXT("index.json");

	TSharedPtr<FJsonObject> Root;
	FString Raw;
	if (FFileHelper::LoadFileToString(Raw, *IndexPath))
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		FJsonSerializer::Deserialize(Reader, Root);
	}
	if (!Root.IsValid()) Root = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Captures;
	if (const TSharedPtr<FJsonValue>* Found = Root->Values.Find(TEXT("captures_available")))
	{
		if (Found->IsValid() && (*Found)->Type == EJson::Object)
			Captures = (*Found)->AsObject();
	}
	if (!Captures.IsValid())
	{
		Captures = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("captures_available"), Captures);
	}

	TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
	Section->SetStringField(TEXT("file"),         TEXT("collision/profiles.json"));
	Section->SetStringField(TEXT("last_updated"), FForgeContextWriter::NowISO8601());
	Captures->SetObjectField(TEXT("collision"), Section);

	Root->SetStringField(TEXT("updated"), FForgeContextWriter::NowISO8601());
	FForgeContextWriter::WriteJSON(OutputDir, TEXT("index"), Root.ToSharedRef());
}
