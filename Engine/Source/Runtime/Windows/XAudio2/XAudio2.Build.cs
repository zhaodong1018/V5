// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class XAudio2 : ModuleRules
{
	public XAudio2(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePathModuleNames.Add("TargetPlatform");

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Engine",
				"BinkAudioDecoder",
                "AudioMixerCore" // for AudioPlatformSettings::GetPlatformSettings() in AudioMixerTypes.h.
			}
			);

		if (Target.Platform == UnrealTargetPlatform.Win64 ||
            Target.Platform == UnrealTargetPlatform.HoloLens)
		{
			// VS2015 updated some of the CRT definitions but not all of the Windows SDK has been updated to match.
			// Microsoft provides this shim library to enable building with VS2015 until they fix everything up.
			//@todo: remove when no longer neeeded (no other code changes should be necessary).
			if (Target.WindowsPlatform.bNeedsLegacyStdioDefinitionsLib)
			{
				PublicSystemLibraries.Add("legacy_stdio_definitions.lib");
			}

		}

		AddEngineThirdPartyPrivateStaticDependencies(Target, 
			"DX11Audio",
			"UEOgg",
			"Vorbis",
			"VorbisFile"
			);

		PublicDefinitions.Add("WITH_OGGVORBIS=1");
	}
}
