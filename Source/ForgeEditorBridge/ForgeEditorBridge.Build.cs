using UnrealBuildTool;

public class ForgeEditorBridge : ModuleRules
{
	public ForgeEditorBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// ---- Core Engine ----
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"EditorSubsystem",
			"DeveloperSettings",        // UDeveloperSettings base for ForgeAISettings

			// ---- HTTP / JSON ----
			"HTTP",
			"HTTPServer",
			"Json",
			"JsonUtilities",

			// ---- UMG / UI ----
			"UMG",
			"UMGEditor",
			"SlateCore",               // FMargin UPROPERTY in FForgeWidgetDescriptor

			// ---- Blueprint ----
			"BlueprintGraph",
			"Kismet",
			"KismetCompiler",
			"BlueprintEditorLibrary",

			// ---- Assets ----
			"AssetTools",
			"AssetRegistry",

			// ---- Networking / Sockets ----
			"Networking",
			"Sockets",

			// ---- MovieScene ----
			"MovieScene",
			"MovieSceneTracks",

			// ---- Materials ----
			"MaterialEditor",

			// ---- Editor Scripting ----
			"EditorScriptingUtilities",

			// ---- Live Coding ----
			"LiveCoding",
			"HotReload",

			// ---- Message Log ----
			"MessageLog",

			// ---- GAS ----
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",

			// ---- PCG ----
			"PCG",
			"PCGEditor",

			// ---- Animation ----
			"AnimationBlueprintEditor",
			"AnimGraph",

			// ---- Niagara ----
			"Niagara",
			"NiagaraEditor",

			// ---- Data Tables ----
			"DataTableEditor",

			// ---- Character & Creature Systems ----
			"PhysicsCore",
			"SkeletalMeshEditor",
			"ClothingSystemRuntimeInterface",
			"IKRig",
			"IKRigEditor",
			"ControlRig",
			"ControlRigDeveloper",         // UE 5.7: UControlRigBlueprint lives here (ControlRigBlueprintLegacy.h)
			"ControlRigEditor",
			"RigVM",
			"RigVMDeveloper",

			// ---- Environment & Level Systems ----
			"Landscape",
			"LandscapeEditor",
			"Foliage",
			"CinematicCamera",

			// ---- AI & Game Systems ----
			"AIModule",
			"BehaviorTreeEditor",
			"NavigationSystem",
			"EnhancedInput",
			"InputCore",

			// ---- Audio & Sequencer ----
			"AudioEditor",
			"MetasoundFrontend",
			"MetasoundEditor",
			"MetasoundEngine",
			"LevelSequence",
			"LevelSequenceEditor",

			// ---- Chaos Physics ----
			"Chaos",                       // FManagedArrayCollection, FTransformCollection
			"GeometryCollectionEngine",
			"GeometryCollectionEditor",
			"ChaosSolverEngine",
			"FieldSystemEngine",

			// ---- Validation ----
			"DataValidation",

			// ---- Reflection & Utilities ----
			"Blutility",
			"LevelEditor",

			// ---- Source Control & Project (Phase 4) ----
			"SourceControl",
			"Projects",

			// ---- Phase 5: Completeness Domains ----
			"MassEntity",
			"MassSpawner",
			"StateTreeModule",
			"StateTreeEditorModule",
			"SmartObjectsModule",
			"Water",
			"MediaAssets",

			// ---- Wave 3: New Handlers ----
			"TargetPlatform",          // PackagingHandler — ITargetPlatformManagerModule

		// ---- Context Capture ----
			"ImageCore",
			"ImageWrapper",
			"WorldPartitionEditor",
			"DataLayerEditor",
			"RHI",
			"RenderCore",

		// ---- UE 5.7 Gap-Closure Handlers (PIE/Curve/Layers/Bookmark/Transaction/
		//      ToolMenus/Placement/GameFeatures/MRQ/Chooser/PoseSearch/GeometryScript/MetaSoundBuilder) ----
			"ToolMenus",                       // ToolMenusHandler
			"EditorFramework",                 // PlacementHandler — UPlacementSubsystem (5.7: in EditorFramework, not PlacementMode)
			"TypedElementFramework",           // PlacementHandler — FTypedElementHandle
			"TypedElementRuntime",             // PlacementHandler — ActorElementDataUtil
			"GameFeatures",                    // GameFeaturesHandler — UGameFeaturesSubsystem
			"ModularGameplay",                 // companion to GameFeatures
			"MovieRenderPipelineCore",         // MovieRenderQueueHandler — UMoviePipelineQueue
			"MovieRenderPipelineEditor",       // MovieRenderQueueHandler — UMoviePipelineQueueSubsystem
			"Chooser",                         // ChooserHandler — UChooserTable
			"PoseSearch",                      // PoseSearchHandler — UPoseSearchSchema/Database
			"Layers",                          // LayersHandler — ULayersSubsystem
			"GeometryFramework",               // GeometryScriptHandler — UDynamicMesh
			"GeometryCore",                    // GeometryScriptHandler — FDynamicMesh3
			"GeometryScriptingCore",           // GeometryScriptHandler — UGeometryScriptLibrary_*
			"GeometryScriptingEditor",         // GeometryScriptHandler — StaticMeshFunctions (editor-side)

		// ---- Wave 5: deeper gap-closure (Decal/RenderTarget/AssetValidator/AssetEvents/IKRig/DDC + Packaging DLC) ----
			"DeveloperToolSettings",           // PackagingHandler — UProjectPackagingSettings (5.7: moved from EngineSettings)

		// ---- Wave 6: Tier 1+2 closure (Notifications/Layouts/TakeRecorder/LiveLink/NiagaraBaker/Concert/
		//      Mover/NDisplay/PixelStreaming/Online) ----
			"Slate",                           // EditorNotifications + EditorLayouts (FSlateNotificationManager, FGlobalTabmanager)
			"LiveLink",                        // LiveLinkHandler — ILiveLinkClient
			"LiveLinkInterface",               // LiveLinkHandler — ILiveLinkClient::ModularFeatureName + FLiveLinkSubjectKey
			"TakeRecorder",                    // TakeRecorderHandler — UTakeRecorderBlueprintLibrary
			"TakesCore",                       // TakeRecorderHandler — UTakePreset

		// ---- Wave 9: Mover real impl + Concert real impl ----
			"Mover",                           // MoverHandler — UMoverComponent::QueueNextMode/AddMovementMode
			"Concert",                         // base concert types
			"ConcertClient",                   // IConcertClient lives in ConcertMain/Source/ConcertClient
			"ConcertSyncClient",               // IConcertSyncClient + IConcertSyncClientModule
			"ConcertTransport",                // Concert transport runtime
		});

		// 5.7: UChooserTable lives in Chooser/Internal/Chooser.h — expose Internal/ to compile
		PrivateIncludePathModuleNames.AddRange(new string[] {
			"Chooser",
		});

		if (Target.Type != TargetType.Editor && Target.Type != TargetType.Program)
		{
			throw new BuildException("ForgeEditorBridge is editor-only.");
		}
	}
}
