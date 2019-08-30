using System.IO;
using System.Collections.Generic;
using Tools.DotNETCommon;

namespace UnrealBuildTool.Rules
{

    public class RTSPStreaming : ModuleRules
    {
        public RTSPStreaming(ReadOnlyTargetRules Target) : base(Target)
        {
            PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

            PublicIncludePaths.Add(ModuleDirectory);
            PrivateIncludePaths.Add(ModuleDirectory);
            PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "../ThirdParty"));
            PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "./Public"));

            // NOTE: General rule is not to access the private folder of another module,
            // but to use the ISubmixBufferListener interface, we  need to include some private headers
            //PrivateIncludePaths.Add(System.IO.Path.Combine(Directory.GetCurrentDirectory(), "./Runtime/AudioMixer/Private"));

            PublicDependencyModuleNames.AddRange(
                new string[]
                {
                    "ApplicationCore",
                    "Core",
                    "CoreUObject",
                "Engine",
                    "InputCore",
                    "InputDevice",
                    "Json",
                "RenderCore",
                "AnimGraphRuntime",
                "RHI",
                "Slate",
                "SlateCore",
                "Sockets",
                "Networking"
                }
            );


            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
                "Slate",
                "SlateCore",
                    "AudioMixer",
                    "Json"
                }
            );

            if (Target.Platform == UnrealTargetPlatform.Win32 || Target.Platform == UnrealTargetPlatform.Win64)
            {
                PrivateDependencyModuleNames.AddRange(new string[] { "D3D11RHI" });
                PrivateIncludePaths.AddRange(
                    new string[] {
                    "C:/Program Files/Epic Games/UE_4.22/Engine/Source/Runtime/Windows/D3D11RHI/Private",
                    //"D:/mattf/ProgramFiles/UnrealEngine/UE_4.22/Engine/Source/Runtime/Windows/D3D11RHI/Private",
                    "C:/Program Files/Epic Games/UE_4.22/Engine/Source/Runtime/Windows/D3D11RHI/Private/Windows",
                    //"D:/mattf/ProgramFiles/UnrealEngine/UE_4.22/Engine/Source/Runtime/Windows/D3D11RHI/Private/Windows"
                    });
                AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelMetricsDiscovery");
                AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAftermath");
            }
        }
    }
}
