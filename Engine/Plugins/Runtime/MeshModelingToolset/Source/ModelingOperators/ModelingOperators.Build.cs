// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ModelingOperators : ModuleRules
{
	public ModelingOperators(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				// These dependencies were commented out when we split out ModelingOperatorsEditorOnly
				// Many of them are editor only and should not be depended on here.  If you re-add any
				// dependent modules, please confirm by "launching" or packaging that you are not introducing
				// editor dependencies
				//
                //"ProxyLODMeshReduction", // currently required to be public due to IVoxelBasedCSG API
                //"MeshDescription",

				"Core",
                "InteractiveToolsFramework",
				"GeometryCore",
				"DynamicMesh",
				"MeshConversion",
                "GeometryAlgorithms", // required for constrained Delaunay triangulation
				// ... add other public dependencies that you statically link with here ...
			}
            );
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// These dependencies were commented out when we split out ModelingOperatorsEditorOnly
				// Many of them are editor only and should not be depended on here.  If you re-add any
				// dependent modules, please confirm by "launching" or packaging that you are not introducing
				// editor dependencies
				//
				//"MeshBuilder",
                //"MeshDescriptionOperations",
				//"MeshUtilitiesCommon",
				//"MeshReductionInterface", // for standard UE simplification

				"CoreUObject",
				"Engine",

			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
