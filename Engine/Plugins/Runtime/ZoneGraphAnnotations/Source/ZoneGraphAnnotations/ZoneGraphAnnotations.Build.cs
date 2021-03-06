// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class ZoneGraphAnnotations: ModuleRules
	{
		public ZoneGraphAnnotations(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

			PublicIncludePaths.AddRange(
				new string[] {
					"Runtime/AIModule/Public",
					ModuleDirectory + "/Public",
				}
			);

			PrivateIncludePaths.AddRange(
				new string[] {
				}
			);

			PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"Engine",
					"RHI",
					"StructUtils",
					"ZoneGraph",
					"ZoneGraphDebug"
				}
			);
		}
	}
}