// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RayTracingInstance.cpp: Helper functions for creating a ray tracing instance.
=============================================================================*/

#include "RayTracingInstance.h"

#if RHI_RAYTRACING

#include "RayTracingDefinitions.h"

void FRayTracingInstance::BuildInstanceMaskAndFlags(ERHIFeatureLevel::Type FeatureLevel)
{
	TArrayView<const FMeshBatch> MeshBatches = GetMaterials();
	FRayTracingMaskAndFlags MaskAndFlags = BuildRayTracingInstanceMaskAndFlags(MeshBatches, FeatureLevel);

	Mask |= MaskAndFlags.Mask;
	bForceOpaque = bForceOpaque || MaskAndFlags.bForceOpaque;
	bDoubleSided = bDoubleSided || MaskAndFlags.bDoubleSided;
}

FRayTracingMaskAndFlags BuildRayTracingInstanceMaskAndFlags(TArrayView<const FMeshBatch> MeshBatches, ERHIFeatureLevel::Type FeatureLevel)
{
	FRayTracingMaskAndFlags Result;

	ensureMsgf(MeshBatches.Num() > 0, TEXT("You need to add MeshBatches first for instance mask and flags to build upon."));

	Result.Mask = 0;

	bool bAllSegmentsOpaque = true;
	bool bAnySegmentsCastShadow = false;
	bool bAllSegmentsCastShadow = true;
	bool bDoubleSided = false;

	for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
	{
		const FMeshBatch& MeshBatch = MeshBatches[SegmentIndex];

		// Mesh Batches can "null" when they have zero triangles.  Check the MaterialRenderProxy before accessing.
		if (MeshBatch.bUseForMaterial && MeshBatch.MaterialRenderProxy)
		{
			const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
			const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, FallbackMaterialRenderProxyPtr);
			const EBlendMode BlendMode = Material.GetBlendMode();
			Result.Mask |= ComputeBlendModeMask(BlendMode);
			bAllSegmentsOpaque &= BlendMode == BLEND_Opaque;
			bAnySegmentsCastShadow |= MeshBatch.CastRayTracedShadow && Material.CastsRayTracedShadows();
			bAllSegmentsCastShadow &= MeshBatch.CastRayTracedShadow && Material.CastsRayTracedShadows();
			bDoubleSided |= MeshBatch.bDisableBackfaceCulling || Material.IsTwoSided();
		}
	}

	Result.bForceOpaque = bAllSegmentsOpaque && bAllSegmentsCastShadow;
	Result.bDoubleSided = bDoubleSided;
	Result.Mask |= bAnySegmentsCastShadow ? RAY_TRACING_MASK_SHADOW : 0;

	return Result;
}

uint8 ComputeBlendModeMask(const EBlendMode BlendMode)
{
	return (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked) ? RAY_TRACING_MASK_OPAQUE : RAY_TRACING_MASK_TRANSLUCENT;
}

#endif
