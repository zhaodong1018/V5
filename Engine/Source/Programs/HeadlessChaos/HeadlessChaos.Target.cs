// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

[SupportedPlatforms("Win64")]
public class HeadlessChaosTarget : TargetRules
{
	public HeadlessChaosTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;

		ExeBinariesSubFolder = LaunchModuleName = "HeadlessChaos";

		bBuildDeveloperTools = false;

		// HeadlessChaos doesn't ever compile with the engine linked in
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;

        bHasExports = false;

        bUseLoggingInShipping = true;

        // UnrealHeaderTool is a console application, not a Windows app (sets entry point to main(), instead of WinMain())
        bIsBuildingConsoleApplication = true;

		GlobalDefinitions.Add("CHAOS_SERIALIZE_OUT=1");

		// Force enable Chaos as the physics engine for this project as this
		// is a Chaos unit test framework - it doesn't build with PhysX enabled
		bUseChaos = true;
		bCompileChaos = true;
		bCompilePhysX = false;
		bCompileAPEX = false;
		bCompileNvCloth = false;
	}
}
