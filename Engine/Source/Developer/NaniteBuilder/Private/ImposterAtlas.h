// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FBounds;

namespace Nanite
{

class FCluster;

class FImposterAtlas
{
public:
	static constexpr uint32	AtlasSize	= 12;
	static constexpr uint32	TileSize	= 12;

				FImposterAtlas( TArray< uint16 >& InPixels, const FBounds& MeshBounds );
	void		Rasterize( const FIntPoint& TilePos, const FCluster& Cluster, uint32 ClusterIndex );

private:
	TArray< uint16 >&	Pixels;

	FVector3f	BoundsCenter;
	FVector3f	BoundsExtent;

	FMatrix44f	GetLocalToImposter(const FIntPoint& TilePos) const;
};

} // namespace Nanite
