// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/LowLevelMemTracker.h"

extern bool ShouldRenderLumenDiffuseGI(const FScene* Scene, const FSceneView& View, bool bSkipTracingDataCheck = false, bool bSkipProjectCheck = false);
extern bool ShouldRenderLumenReflections(const FViewInfo& View, bool bSkipTracingDataCheck = false, bool bSkipProjectCheck = false);

inline double BoxSurfaceArea(FVector Extent)
{
	return 2.0 * (Extent.X * Extent.Y + Extent.Y * Extent.Z + Extent.Z * Extent.X);
}

namespace Lumen
{
	// Must match usf
	constexpr uint32 PhysicalPageSize = 128;
	constexpr uint32 VirtualPageSize = PhysicalPageSize - 1; // 0.5 texel border around page
	constexpr uint32 MinCardResolution = 8;
	constexpr uint32 MinResLevel = 3; // 2^3 = MinCardResolution
	constexpr uint32 MaxResLevel = 11; // 2^11 = 2048 texels
	constexpr uint32 SubAllocationResLevel = 7; // log2(PHYSICAL_PAGE_SIZE)
	constexpr uint32 NumResLevels = MaxResLevel - MinResLevel + 1;
	constexpr uint32 CardTileSize = 8;

	enum class ETracingPermutation
	{
		Cards,
		VoxelsAfterCards,
		Voxels,
		MAX
	};

	void DebugResetSurfaceCache();
	float GetDistanceSceneNaniteLODScaleFactor();
	bool UseMeshSDFTracing();
	float GetMaxTraceDistance();
	bool AnyLumenHardwareRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View);
	bool IsSoftwareRayTracingSupported();
	bool IsLumenFeatureAllowedForView(const FScene* Scene, const FSceneView& View, bool bSkipTracingDataCheck = false, bool bSkipProjectCheck = false);
	bool ShouldVisualizeHardwareRayTracing(const FViewInfo& View);
	bool ShouldHandleSkyLight(const FScene* Scene, const FSceneViewFamily& ViewFamily);
	bool UseVirtualShadowMaps();
	void ExpandDistanceFieldUpdateTrackingBounds(const FSceneViewState* ViewState, DistanceField::FUpdateTrackingBounds& UpdateTrackingBounds);

	int32 GetGlobalDFResolution();
	float GetGlobalDFClipmapExtent();
	float GetFirstClipmapWorldExtent();

	// Features
	bool IsRadiosityEnabled();
	uint32 GetRadiosityDownsampleFactor();

	// Surface cache
	float GetSurfaceCacheOffscreenShadowingMaxTraceDistance();
	bool IsSurfaceCacheFrozen();
	bool IsSurfaceCacheUpdateFrameFrozen();

	// Software ray tracing
	bool UseVoxelLighting(const FViewInfo& View);

	// Hardware ray tracing
	bool UseHardwareRayTracing();
	bool UseHardwareRayTracedSceneLighting();
	bool UseHardwareRayTracedDirectLighting();
	bool UseHardwareRayTracedReflections();
	bool UseHardwareRayTracedScreenProbeGather();
	bool UseHardwareRayTracedRadianceCache();
	bool UseHardwareRayTracedRadiosity();

	enum class EHardwareRayTracingLightingMode
	{
		LightingFromSurfaceCache = 0,
		EvaluateMaterial,
		EvaluateMaterialAndDirectLighting,
		EvaluateMaterialAndDirectLightingAndSkyLighting,
		MAX
	};
	EHardwareRayTracingLightingMode GetReflectionsHardwareRayTracingLightingMode(const FViewInfo& View);
	EHardwareRayTracingLightingMode GetScreenProbeGatherHardwareRayTracingLightingMode();
	EHardwareRayTracingLightingMode GetRadianceCacheHardwareRayTracingLightingMode();
	EHardwareRayTracingLightingMode GetVisualizeHardwareRayTracingLightingMode();

	const TCHAR* GetRayTracedLightingModeName(EHardwareRayTracingLightingMode LightingMode);
	const TCHAR* GetRayTracedNormalModeName(int NormalMode);
	float GetHardwareRayTracingPullbackBias();

	bool UseFarField();
	float GetFarFieldMaxTraceDistance();
	FVector GetFarFieldReferencePos();
};

extern int32 GLumenFastCameraMode;
extern int32 GLumenDistantScene;

LLM_DECLARE_TAG(Lumen);
