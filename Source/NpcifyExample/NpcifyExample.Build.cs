// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NpcifyExample : ModuleRules
{
	public NpcifyExample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"SlateCore",
			"NpcifySdk",
			"AudioMixer"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"HTTP",
			"Json",
			"Projects"
		});

		PublicIncludePaths.AddRange(new string[] {
			"NpcifyExample",
			"NpcifyExample/Npcify"
		});
	}
}
