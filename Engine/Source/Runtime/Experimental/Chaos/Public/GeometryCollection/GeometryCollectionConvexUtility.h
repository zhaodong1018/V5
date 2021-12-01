// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GeometryCollection/ManagedArrayCollection.h"

class FGeometryCollection;

namespace Chaos
{
	class FConvex;
}

class CHAOS_API FGeometryCollectionConvexUtility
{
public:

	struct FGeometryCollectionConvexData
	{
		TManagedArray<TSet<int32>>& TransformToConvexIndices;
		TManagedArray<TUniquePtr<Chaos::FConvex>>& ConvexHull;
	};

	/** Ensure that convex hull data exists for the Geometry Collection and construct it if not (or if some data is missing. */
	static FGeometryCollectionConvexData GetValidConvexHullData(FGeometryCollection* GeometryCollection);

	/**
	 Create non-overlapping convex hull data for all transforms in the geometry collection (except transforms where it would be better to just use the hulls of the children) 

	 @param GeometryCollection					The collection to add convex hulls to
	 @param FractionAllowRemove					The fraction of a convex body we can cut away to remove overlaps with neighbors, before we fall back to using the hulls of the children directly.  (Does not affect leaves of hierarchy)
	 @param SimplificationDistanceThreshold		Approximate minimum distance between vertices, below which we remove vertices to generate a simpler convex shape.  If 0.0, no simplification will occur.
	 */
	static FGeometryCollectionConvexData CreateNonOverlappingConvexHullData(FGeometryCollection* GeometryCollection, double FractionAllowRemove = .3, double SimplificationDistanceThreshold = 0.0);

	/** Returns the convex hull of the vertices contained in the specified geometry. */
	static TUniquePtr<Chaos::FConvex> FindConvexHull(const FGeometryCollection* GeometryCollection, int32 GeometryIndex);

	/** Delete the convex hulls pointed at by the transform indices provided. */
	static void RemoveConvexHulls(FGeometryCollection* GeometryCollection, const TArray<int32>& SortedTransformDeletes);

	/** Set default values for convex hull related managed arrays. */
	static void SetDefaults(FGeometryCollection* GeometryCollection, FName Group, uint32 StartSize, uint32 NumElements);

};

