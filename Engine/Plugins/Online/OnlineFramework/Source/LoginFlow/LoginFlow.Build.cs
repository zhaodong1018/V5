// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LoginFlow : ModuleRules
{
	public LoginFlow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreOnline",
				"CoreUObject",
				"InputCore",
				"Slate",
			}
		);

		PrivateDependencyModuleNames.AddRange(
		new string[]
			{
				"SlateCore",
				"WebBrowser",
				"OnlineSubsystem",
			}
		);

		PrivateIncludePaths.AddRange(
			new string[]
			{
			}
		);

		PublicIncludePaths.AddRange(
			new string[]
			{
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
		new string[] {
				"Analytics",
				"AnalyticsET",
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
				"Analytics",
				"AnalyticsET",
			}
		);
	}
}
