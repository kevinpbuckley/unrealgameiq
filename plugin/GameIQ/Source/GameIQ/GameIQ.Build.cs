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
			});
		}
	}
}
