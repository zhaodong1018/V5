// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SourceControlWindows : ModuleRules
{
	public SourceControlWindows(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicIncludePaths.Add("Editor/SourceControlWindows/Public");

		PrivateIncludePaths.Add("Editor/SourceControlWindows/Private");

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core", 
				"CoreUObject", 
                "InputCore",
				"Engine", 
				"Slate",
				"SlateCore",
                "EditorStyle",
				"SourceControl", 
				"UncontrolledChangelists",
				"AssetTools",
				"EditorFramework",
				"WorkspaceMenuStructure",
				"DeveloperSettings",
				"UnrealEd"			// We need this dependency here because we use PackageTools.
			}
		);

		if(Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"ToolMenus"
				}
			);
		}
	}
}
