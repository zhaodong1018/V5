// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class StateTreeModule : ModuleRules
	{
		public StateTreeModule(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicIncludePaths.AddRange(
				new string[] {
				}
			);

			PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"Engine",
					"AIModule",
					"GameplayBehaviorsModule",
					"GameplayTasks",
					"GameplayTags",
					"GameplayAbilities",
					"SmartObjectsModule",
					"StructUtils",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"RenderCore",
				}
			);
			
			if (Target.Type == TargetType.Editor)
			{
				PrivateDependencyModuleNames.AddRange(
					new string[]
					{
						"UnrealEd"	// Editor callbacks
					}
				);
			}

		}

	}
}
