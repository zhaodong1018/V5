// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/AreTypesEqual.h"
#include "Misc/LargeWorldCoordinates.h"

namespace Chaos
{
	/**
	* Specific precision types, this should be used in very targeted places
	* where the use of a specific type is necessary over using FReal
	*/
	using FRealDouble = double;
	using FRealSingle = float;

	/**
	* Common data types for the Chaos physics engine. Unless a specific
	* precision of type is required most code should use these existing types
	* (e.g. FVec3) to adapt to global changes in precision.
	*/
#if UE_LARGE_WORLD_COORDINATES_DISABLED
	using FReal = FRealSingle;
#else
	//using FReal = FRealSingle;
	using FReal = FRealDouble;	// LWC_TODO: Requires FVector4 + FVector2D to support doubles
#endif

	/**
	* ISPC optimization supports float, this allows classes that uses ISPC to branch to the right implementation 
	* without having to check the actual underlying type of FReal
	*/
	constexpr bool bRealTypeCompatibleWithISPC = (TAreTypesEqual<FReal, float>::Value == true);
}
