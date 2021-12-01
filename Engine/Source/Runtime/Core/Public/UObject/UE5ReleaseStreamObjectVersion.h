// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "Misc/Guid.h"

// Custom serialization version for changes made in //UE5/Release-* stream
struct CORE_API FUE5ReleaseStreamObjectVersion
{
	enum Type
	{
		// Before any version changes were made
		BeforeCustomVersionWasAdded = 0,

		// Added Lumen reflections to new reflection enum, changed defaults
		ReflectionMethodEnum,
		
		// Serialize HLOD info in WorldPartitionActorDesc
		WorldPartitionActorDescSerializeHLODInfo,

		// Removing Tessellation from materials and meshes.
		RemovingTessellation,

		// LevelInstance serialize runtime behavior
		LevelInstanceSerializeRuntimeBehavior,

		// Refactoring Pose Asset runtime data structures
		PoseAssetRuntimeRefactor,

		// Serialize the folder path of actor descs
		WorldPartitionActorDescSerializeActorFolderPath,

		// Change hair strands vertex format
		HairStrandsVertexFormatChange,
		
		// Added max linear and angular speed to Chaos bodies
		AddChaosMaxLinearAngularSpeed,

		// PackedLevelInstance version
		PackedLevelInstanceVersion,

		// PackedLevelInstance bounds fix
		PackedLevelInstanceBoundsFix,

		// Custom property anim graph nodes (linked anim graphs, control rig etc.) now use optional pin manager
		CustomPropertyAnimGraphNodesUseOptionalPinManager,

		// Add native double and int64 support to FFormatArgumentData
		TextFormatArgumentData64bitSupport,

		// Material layer stacks are no longer considered 'static parameters'
		MaterialLayerStacksAreNotParameters,

		// CachedExpressionData is moved from UMaterial to UMaterialInterface
		MaterialInterfaceSavedCachedData,
		
		// Add support for multiple cloth deformer LODs to be able to raytrace cloth with a different LOD than the one it is rendered with
		AddClothMappingLODBias,

		// Add support for different external actor packaging schemes
		AddLevelActorPackagingScheme,

		// -----<new versions can be added above this line>-------------------------------------------------
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	// The GUID for this custom version number
	const static FGuid GUID;

	FUE5ReleaseStreamObjectVersion() = delete;
};
