// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShaderParameterMacros.h"
#include "GlobalShader.h"
#include "UnifiedBuffer.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "Rendering/NaniteResources.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNanite, Warning, All);

DECLARE_GPU_STAT_NAMED_EXTERN(NaniteDebug, TEXT("Nanite Debug"));

struct FSceneTextures;
struct FDBufferTextures;

namespace Nanite
{

// Must match FStats in NaniteDataDecode.ush
struct FNaniteStats
{
	uint32 NumTris;
	uint32 NumVerts;
	uint32 NumViews;
	uint32 NumMainInstancesPreCull;
	uint32 NumMainInstancesPostCull;
	uint32 NumMainVisitedNodes;
	uint32 NumMainCandidateClusters;
	uint32 NumPostInstancesPreCull;
	uint32 NumPostInstancesPostCull;
	uint32 NumPostVisitedNodes;
	uint32 NumPostCandidateClusters;
	uint32 NumLargePageRectClusters;
	uint32 NumPrimaryViews;
	uint32 NumTotalViews;
};

struct FPackedView
{
	FMatrix44f	SVPositionToTranslatedWorld;
	FMatrix44f	ViewToTranslatedWorld;

	FMatrix44f	TranslatedWorldToView;
	FMatrix44f	TranslatedWorldToClip;
	FMatrix44f	ViewToClip;
	FMatrix44f	ClipToRelativeWorld;

	FMatrix44f	PrevTranslatedWorldToView;
	FMatrix44f	PrevTranslatedWorldToClip;
	FMatrix44f	PrevViewToClip;
	FMatrix44f	PrevClipToRelativeWorld;

	FIntVector4	ViewRect;
	FVector4f	ViewSizeAndInvSize;
	FVector4f	ClipSpaceScaleOffset;
	FVector4f	PreViewTranslation;
	FVector4f	PrevPreViewTranslation;
	FVector4f	WorldCameraOrigin;
	FVector4f	ViewForwardAndNearPlane;

	FVector3f	ViewTilePosition;
	uint32		Padding0;

	FVector3f	MatrixTilePosition;
	uint32		Padding1;

	FVector2f	LODScales;
	float		MinBoundsRadiusSq;
	uint32		StreamingPriorityCategory_AndFlags;

	FIntVector4 TargetLayerIdX_AndMipLevelY_AndNumMipLevelsZ;

	FIntVector4	HZBTestViewRect;	// In full resolution

	/**
	 * Calculates the LOD scales assuming view size and projection is already set up.
	 * TODO: perhaps more elegant/robust if this happened at construction time, and input was a non-packed NaniteView.
	 * Note: depends on the global 'GNaniteMaxPixelsPerEdge'.
	 */
	void UpdateLODScales();
};

struct FPackedViewParams
{
	FViewMatrices ViewMatrices;
	FViewMatrices PrevViewMatrices;
	FIntRect ViewRect;
	FIntPoint RasterContextSize;
	uint32 StreamingPriorityCategory = 0;
	float MinBoundsRadius = 0.0f;
	float LODScaleFactor = 1.0f;
	uint32 Flags = 0;

	int32 TargetLayerIndex = 0;
	int32 PrevTargetLayerIndex = INDEX_NONE;
	int32 TargetMipLevel = 0;
	int32 TargetMipCount = 1;

	FIntRect HZBTestViewRect = {0, 0, 0, 0};
};

FPackedView CreatePackedView(const FPackedViewParams& Params);

// Convenience function to pull relevant packed view parameters out of a FViewInfo
FPackedView CreatePackedViewFromViewInfo(
	const FViewInfo& View,
	FIntPoint RasterContextSize,
	uint32 Flags,
	uint32 StreamingPriorityCategory = 0,
	float MinBoundsRadius = 0.0f,
	float LODScaleFactor = 1.0f
);

struct FVisualizeResult
{
	FRDGTextureRef ModeOutput;
	FName ModeName;
	int32 ModeID;
	uint8 bCompositeScene : 1;
	uint8 bSkippedTile    : 1;
};

} // namespace Nanite

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FNaniteUniformParameters, )
	SHADER_PARAMETER(FIntVector4,					PageConstants)
	SHADER_PARAMETER(FIntVector4,					MaterialConfig) // .x mode, .yz grid size, .w unused
	SHADER_PARAMETER(uint32,						MaxNodes)
	SHADER_PARAMETER(uint32,						MaxVisibleClusters)
	SHADER_PARAMETER(uint32,						RenderFlags)
	SHADER_PARAMETER(FVector4f,						RectScaleOffset) // xy: scale, zw: offset
	SHADER_PARAMETER_SRV(ByteAddressBuffer,			ClusterPageData)
	SHADER_PARAMETER_SRV(ByteAddressBuffer,			VisibleClustersSWHW)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint>,	MaterialTileRemap)
	SHADER_PARAMETER_TEXTURE(Texture2D<UlongType>,	VisBuffer64)
	SHADER_PARAMETER_TEXTURE(Texture2D<UlongType>,	DbgBuffer64)
	SHADER_PARAMETER_TEXTURE(Texture2D<uint>,		DbgBuffer32)
	// Multi view
	SHADER_PARAMETER(uint32,									MultiViewEnabled)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint>,				MultiViewIndices)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>,				MultiViewRectScaleOffsets)
	SHADER_PARAMETER_SRV(StructuredBuffer<FPackedNaniteView>,	InViews)
END_SHADER_PARAMETER_STRUCT()

class FNaniteShader : public FGlobalShader
{
public:
	FNaniteShader() = default;
	FNaniteShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	: FGlobalShader(Initializer)
	{
	}
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	/**
	* Can be overridden by FVertexFactory subclasses to modify their compile environment just before compilation occurs.
	*/
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
	}
};

extern bool ShouldRenderNanite(const FScene* Scene, const FViewInfo& View, bool bCheckForAtomicSupport = true);

/** Checks whether Nanite would be rendered in this view. Used to give a visual warning about the project settings that can disable Nanite. */
extern bool WouldRenderNanite(const FScene* Scene, const FViewInfo& View, bool bCheckForAtomicSupport = true, bool bCheckForProjectSetting = true);

extern bool UseComputeDepthExport();
