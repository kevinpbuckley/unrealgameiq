// Copyright Buckley Builds LLC. All Rights Reserved.

using UnrealBuildTool;

public class GameIQ : ModuleRules
{
	public GameIQ(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"AssetRegistry",     // Tier 0: identity + dependency/referencer graph, no asset loading
			"Json",              // emit the ExtractorOutput contract as JSON
			"Projects",          // plugin/project paths
			"DeveloperSettings", // Project Settings entry (UGameIQSettings)
			"EnhancedInput",     // Tier 1 recipe: InputMappingContext key->action mappings (default UE5 input)
			"InputCore",         // FKey for input mappings
			"SQLiteCore",        // in-editor toolset reads the .gameiq/index.db (FTS5) natively, no Node
			"ToolsetRegistry",   // UE 5.8 native AI toolset registry — exposes GameIQ queries on Epic's MCP endpoint
			"ImageWrapper",      // read image dimensions (png/jpg/…) for the docs image extractor (issue #7)
			"zlib",              // inflate PDF FlateDecode content streams for text extraction (issue #5)
		});

		// Editor-only machinery for the live in-editor bridge and Tier 2 graph export
		// (FEdGraphUtilities::ExportNodesToText lives in UnrealEd). Kept editor-gated.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"BlueprintGraph",
				"Kismet",
				"EditorSubsystem", // in-editor save hook (incremental index updates)
				"Slate",           // Tools > Game IQ panel (index stats + rebuild)
				"SlateCore",
				"ToolMenus",       // register the Tools menu entry
				"WorkspaceMenuStructure", // Tools category for the dockable tab
				"FileUtilities",   // FZipArchiveReader (over bundled libzip) — read DOCX word/document.xml (issue #5)
			});
		}
	}
}
