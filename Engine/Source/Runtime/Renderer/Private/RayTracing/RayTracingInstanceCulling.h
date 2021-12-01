// Copyright Epic Games, Inc.All Rights Reserved.

#pragma once

#include "SceneRendering.h"
#include "ScenePrivate.h"

#if RHI_RAYTRACING

int32 GetRayTracingCulling();
float GetRayTracingCullingRadius();
int32 GetRayTracingCullingPerInstance();

namespace RayTracing
{

bool ShouldCullBounds(const FRayTracingCullingParameters& CullingParameters, FBoxSphereBounds ObjectBounds, bool bIsFarFieldPrimitive);
bool ShouldSkipPerInstanceCullingForPrimitive(const FRayTracingCullingParameters& CullingParameters, FBoxSphereBounds ObjectBounds, FBoxSphereBounds SmallestInstanceBounds, bool bIsFarFieldPrimitive);

}

struct FRayTracingCullPrimitiveInstancesClosure
{
	FScene* Scene;
	int32 PrimitiveIndex;
	const FPrimitiveSceneInfo* SceneInfo;
	bool bIsFarFieldPrimitive;
	TArrayView<uint32> OutInstanceActivationMask;

	const FRayTracingCullingParameters* CullingParameters;

	void operator()() const;
};

#endif
