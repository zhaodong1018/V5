// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MeshModelingToolsExp : ModuleRules
{
	public MeshModelingToolsExp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"AnimationCore",			// For the BoneWeights.h include
			}
		);
	
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				// These dependencies were commented out when we split out MeshModelingToolsEditorOnly
				// Many of them are editor only and should not be depended on here.  If you re-add any
				// dependent modules, please confirm by "launching" or packaging that you are not introducing
				// editor dependencies
				//
				//"MeshDescription",
				//"ProxyLODMeshReduction", // currently required to be public due to IVoxelBasedCSG API

				"Core",
				"Eigen",
                "InteractiveToolsFramework",
				"GeometryCore",
				"GeometryFramework",
				"GeometryAlgorithms",
				"DynamicMesh",
				"MeshConversion",
				"MeshDescription",
				"MeshModelingTools",
                "StaticMeshDescription",
                "SkeletalMeshDescription",
				"ModelingComponents",
				"ModelingOperators",

				// ... add other public dependencies that you statically link with here ...
			}
            );
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// These dependencies were commented out when we split out MeshModelingToolsEditorOnly
				// Many of them are editor only and should not be depended on here.  If you re-add any
				// dependent modules, please confirm by "launching" or packaging that you are not introducing
				// editor dependencies
				//
				//"RenderCore",
				//"RHI",
				//"MeshUtilities",    // temp for saving mesh asset
				//"UnrealEd",
				//"MeshBuilder",
                //"MeshDescriptionOperations",
				//"MeshUtilitiesCommon",
				//"MeshReductionInterface", // for UE standard simplification
                //"ProxyLODMeshReduction", // for mesh merging voxel-based csg
				//"Slate",
				//"SlateCore",

				"CoreUObject",
				"Engine",
				"RenderCore",
				"ModelingOperators",
				"InputCore",
				"PhysicsCore",

				// ... add private dependencies that you statically link with here ...
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
