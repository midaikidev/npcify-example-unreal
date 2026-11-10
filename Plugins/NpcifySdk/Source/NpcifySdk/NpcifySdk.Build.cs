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
                // Sight: the scaled copy and the readback of the viewport or a texture, and the JPEG.
                "ImageWrapper",
                "RenderCore",
                "RHI",
                "Slate",
                "SlateCore",
            }
        );

        // Sight reads the readback of an Apple GPU from shared memory (Private/Apple).
        if (Target.IsInPlatformGroup(UnrealPlatformGroup.Apple))
        {
            PublicFrameworks.Add("Metal");
        }

        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
    }
}
