using System.IO;
using UnrealBuildTool;

public class NpcifySdk : ModuleRules
{
    public NpcifySdk(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;

        PublicIncludePaths.AddRange(
            new string[]
            {
                Path.Combine(ModuleDirectory, "Public"),
            }
        );

        PrivateIncludePaths.AddRange(
            new string[]
            {
                Path.Combine(ModuleDirectory, "Private"),
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "HTTP",
                "InputCore",
                "Json",
                "JsonUtilities",
            }
        );

        // Voice input records with UAudioCapture of the engine's AudioCapture plugin.
        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "AudioCapture",
                "AudioMixer",
                // The conversation stream of live motion (Socket.IO over a WebSocket).
                "WebSockets",
            }
        );

        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
    }
}
