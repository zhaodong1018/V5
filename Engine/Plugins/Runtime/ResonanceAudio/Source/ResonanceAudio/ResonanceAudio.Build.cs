//
// Copyright (C) Google Inc. 2017. All rights reserved.
//
using UnrealBuildTool;
using System.IO;

public class ResonanceAudio : ModuleRules
{
	protected virtual bool bSupportsProceduralMesh { get { return true; } }

	public ResonanceAudio(ReadOnlyTargetRules Target) : base(Target)
    {
        string ResonanceAudioPath = ModuleDirectory + "/Private/ResonanceAudioLibrary";
        string ResonanceAudioLibraryPath = ModuleDirectory + "/Private/ResonanceAudioLibrary/resonance_audio";
        string PFFTPath = ModuleDirectory + "/Private/ResonanceAudioLibrary/third_party/pfft";

        PublicIncludePaths.AddRange(
            new string[] {
            }
        );

        PrivateIncludePaths.AddRange(
            new string[] {
                "ResonanceAudio/Private",
                ResonanceAudioPath,
                ResonanceAudioLibraryPath,
                PFFTPath,
                "../../../../Source/Runtime/AudioMixer/Private"
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[] {
                "Core",
                "CoreUObject",
                "Engine",
                "AudioMixer",
				"SoundFieldRendering",
                "ProceduralMeshComponent",
            }
        );

        PrivateIncludePathModuleNames.AddRange(
            new string[] {
                "TargetPlatform"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[] {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "Projects",
                "AudioMixer",
                "ProceduralMeshComponent",
                "AudioExtensions"
            }
        );

        if (Target.bBuildEditor == true)
        {
			PrivateDependencyModuleNames.Add("EditorFramework");
            PrivateDependencyModuleNames.Add("UnrealEd");
            PrivateDependencyModuleNames.Add("Landscape");
        }

		ShadowVariableWarningLevel = WarningLevel.Off;

        AddEngineThirdPartyPrivateStaticDependencies(Target,
                "UEOgg",
                "Vorbis",
                "VorbisFile",
                "Eigen"
                );

		if (bSupportsProceduralMesh)
		{
			PrivateDependencyModuleNames.Add("ProceduralMeshComponent");
			PrivateDefinitions.Add("SUPPORTS_PROCEDURAL_MESH=1");
		}
		else
		{
			PrivateDefinitions.Add("SUPPORTS_PROCEDURAL_MESH=0");
		}

		if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PrivateDefinitions.Add("PFFFT_SIMD_DISABLE=1");
            PrivateDefinitions.Add("EIGEN_HAS_CXX11_MATH=0");
        }

        // Always use the official version of IntelTBB
        string IntelTBBLibs = Target.UEThirdPartyBinariesDirectory + "Intel/TBB/";
        string IntelEmbreeLibs = Target.UEThirdPartyBinariesDirectory + "Intel/Embree/Embree2140";

		//Embree support:
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string SDKDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree2140/Win64/";

            PublicIncludePaths.Add(SDKDir + "include");
            PublicAdditionalLibraries.Add(SDKDir + "lib/embree.2.14.0.lib");
            RuntimeDependencies.Add("$(TargetOutputDir)/embree.2.14.0.dll", SDKDir + "lib/embree.2.14.0.dll");
            RuntimeDependencies.Add("$(TargetOutputDir)/tbb.dll", IntelTBBLibs + "Win64/tbb.dll");
            RuntimeDependencies.Add("$(TargetOutputDir)/tbbmalloc.dll", IntelTBBLibs + "Win64/tbbmalloc.dll");
			PublicDefinitions.Add("USE_EMBREE=1");
			PrivateDefinitions.Add("USE_EMBRE_FOR_RESONANCE=1");
		}
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
			// In platforms that don't support Embree, we implement no-op versions of the functions.
			string SDKDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree2140/";
			PublicIncludePaths.Add(SDKDir + "include");
			PrivateDefinitions.Add("USE_EMBRE_FOR_RESONANCE=0");
			PrivateDefinitions.Add("EMBREE_STATIC_LIB=1");
		}
		else if (Target.IsInPlatformGroup(UnrealPlatformGroup.Unix) && Target.Architecture.StartsWith("x86_64")) // no support for arm64 yet
		{
			string IncludeDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree2140/Linux/x86_64-unknown-linux-gnu";
			string SDKDir = Path.Combine(IntelEmbreeLibs, "Linux/x86_64-unknown-linux-gnu/lib");

			PublicIncludePaths.Add(Path.Combine(IncludeDir, "include"));
			PublicAdditionalLibraries.Add(Path.Combine(SDKDir, "libembree.so"));
			PublicAdditionalLibraries.Add(Path.Combine(IntelTBBLibs, "Linux/libtbb.so"));
			// disabled for Linux atm due to a bug in libtbbmalloc on exit
			// PublicAdditionalLibraries.Add(Path.Combine(IntelTBBLibs, "Linux/libtbbmalloc.so"));
			RuntimeDependencies.Add(Path.Combine(SDKDir, "libembree.so"));
			RuntimeDependencies.Add(Path.Combine(SDKDir, "libembree.so.2"));
			RuntimeDependencies.Add(Path.Combine(IntelTBBLibs, "Linux/libtbb.so"));
			RuntimeDependencies.Add(Path.Combine(IntelTBBLibs, "Linux/libtbb.so.2"));
			// disabled for Linux atm due to a bug in libtbbmalloc on exit
			// RuntimeDependencies.Add(Path.Combine(IntelTBBLibs, "Linux/libtbbmalloc.so"));
			PublicDefinitions.Add("USE_EMBREE=1");
			PrivateDefinitions.Add("USE_EMBRE_FOR_RESONANCE=1");
		}
		else
        {
            // In platforms that don't support Embree, we implement no-op versions of the functions.
            string SDKDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree2140/";
            PublicIncludePaths.Add(SDKDir + "include");
			PrivateDefinitions.Add("USE_EMBRE_FOR_RESONANCE=0");
			PrivateDefinitions.Add("USE_EMBREE=0");
			PrivateDefinitions.Add("EMBREE_STATIC_LIB=1");
        }
    }
}
