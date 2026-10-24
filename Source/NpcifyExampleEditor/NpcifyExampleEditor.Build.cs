using UnrealBuildTool;

public class NpcifyExampleEditor : ModuleRules
{
	public NpcifyExampleEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"UnrealEd",
			"VRM4U",
			"VRM4UImporter",
			"IKRig",
			"IKRigEditor"
		});
	}
}
