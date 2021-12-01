// Copyright Epic Games, Inc. All Rights Reserved.
/*=============================================================================
	VirtualShadowMapArray.h:
=============================================================================*/
#pragma once

#include "../Nanite/Nanite.h"
#include "../MeshDrawCommands.h"
#include "SceneTypes.h"

struct FMinimalSceneTextures;
struct FSortedLightSetSceneInfo;
class FViewInfo;
class FProjectedShadowInfo;
class FVisibleLightInfo;
class FVirtualShadowMapCacheEntry;
class FVirtualShadowMapArrayCacheManager;
struct FSortedLightSetSceneInfo;

// TODO: does this exist?
constexpr uint32 ILog2Const(uint32 n)
{
	return (n > 1) ? 1 + ILog2Const(n / 2) : 0;
}

// See CalcLevelOffsets in PageAccessCommon.ush for some details on this logic
constexpr uint32 CalcVirtualShadowMapLevelOffsets(uint32 Level, uint32 Log2Level0DimPagesXY)
{
	uint32 NumBits = Level << 1;
	uint32 StartBit = (2U * Log2Level0DimPagesXY + 2U) - NumBits;
	uint32 Mask = ((1U << NumBits) - 1U) << StartBit;
	return 0x55555555U & Mask;
}

class FVirtualShadowMap
{
public:
	// PageSize * Level0DimPagesXY defines the virtual address space, e.g., 128x128 = 16k

	// 32x512 = 16k
	//static constexpr uint32 PageSize = 32U;
	//static constexpr uint32 Level0DimPagesXY = 512U;

	// 128x128 = 16k
	static constexpr uint32 PageSize = 128U;
	static constexpr uint32 Level0DimPagesXY = 128U;

	// 512x32 = 16k
	//static constexpr uint32 PageSize = 512U;
	//static constexpr uint32 Level0DimPagesXY = 32U;

	static constexpr uint32 PageSizeMask = PageSize - 1U;
	static constexpr uint32 Log2PageSize = ILog2Const(PageSize);
	static constexpr uint32 Log2Level0DimPagesXY = ILog2Const(Level0DimPagesXY);
	static constexpr uint32 MaxMipLevels = Log2Level0DimPagesXY + 1U;

	static constexpr uint32 PageTableSize = CalcVirtualShadowMapLevelOffsets(MaxMipLevels, Log2Level0DimPagesXY);

	static constexpr uint32 VirtualMaxResolutionXY = Level0DimPagesXY * PageSize;
	
	static constexpr uint32 PhysicalPageAddressBits = 16U;
	static constexpr uint32 MaxPhysicalTextureDimPages = 1U << PhysicalPageAddressBits;
	static constexpr uint32 MaxPhysicalTextureDimTexels = MaxPhysicalTextureDimPages * PageSize;

	static constexpr uint32 RasterWindowPages = 4u;
	
	FVirtualShadowMap(uint32 InID) : ID(InID)
	{
	}

	int32 ID = INDEX_NONE;
	TSharedPtr<FVirtualShadowMapCacheEntry> VirtualShadowMapCacheEntry;
};

// Useful data for both the page mapping shader and the projection shader
// as well as cached shadow maps
struct FVirtualShadowMapProjectionShaderData
{
	/**
	 * Transform from shadow-pre-translated world space to shadow view space, example use: (WorldSpacePos + ShadowPreViewTranslation) * TranslatedWorldToShadowViewMatrix
	 * TODO: Why don't we call it a rotation and store in a 3x3? Does it ever have translation in?
	 */
	FMatrix44f TranslatedWorldToShadowViewMatrix;
	FMatrix44f ShadowViewToClipMatrix;
	FMatrix44f TranslatedWorldToShadowUVMatrix;
	FMatrix44f TranslatedWorldToShadowUVNormalMatrix;

	FVector3f ShadowPreViewTranslation;
	uint32 LightType = ELightComponentType::LightType_Directional;
	
	// TODO: There are more local lights than directional
	// We should move the directional-specific stuff out to its own structure.
	FVector3f ClipmapWorldOrigin;
	float LightSourceRadius;				// This should live in shared light structure...
	
	FIntPoint ClipmapCornerOffset;
	int32 ClipmapIndex = 0;					// 0 .. ClipmapLevelCount-1
	int32 ClipmapLevel = 0;					// "Absolute" level, can be negative
	int32 ClipmapLevelCount = 0;
	float ClipmapResolutionLodBias = 0.0f;

	// Seems the FMatrix forces 16-byte alignment
	float Padding[2];
};
static_assert((sizeof(FVirtualShadowMapProjectionShaderData) % 16) == 0, "FVirtualShadowMapProjectionShaderData size should be a multiple of 16-bytes for alignment.");

struct FVirtualShadowMapHZBMetadata
{
	FViewMatrices ViewMatrices;
	FIntRect	  ViewRect;
	uint32		  TargetLayerIndex = INDEX_NONE;
};

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVirtualShadowMapUniformParameters, )
	SHADER_PARAMETER_SCALAR_ARRAY(uint32, HPageFlagLevelOffsets, [FVirtualShadowMap::MaxMipLevels])
	SHADER_PARAMETER(uint32, HPageTableSize)
	SHADER_PARAMETER(uint32, NumShadowMaps)
	SHADER_PARAMETER(uint32, NumDirectionalLights)
	SHADER_PARAMETER(uint32, MaxPhysicalPages)
	// use to map linear index to x,y page coord
	SHADER_PARAMETER(uint32, PhysicalPageRowMask)
	SHADER_PARAMETER(uint32, PhysicalPageRowShift)
	SHADER_PARAMETER(FVector4f, RecPhysicalPoolSize)
	SHADER_PARAMETER(FIntPoint, PhysicalPoolSize)
	SHADER_PARAMETER(FIntPoint, PhysicalPoolSizePages)

	SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, ProjectionData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PageTable)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, PhysicalPagePool)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FVirtualShadowMapSamplingParameters, )
	// NOTE: These parameters must only be uniform buffers/references! Loose parameters do not get bound
	// in some of the forward passes that use this structure.
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualShadowMapUniformParameters, VirtualShadowMap)
END_SHADER_PARAMETER_STRUCT()

/**
 * Use after page allocation but before rendering phase to access page table & related data structures, but not the physical backing.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FVirtualShadowMapPageTableParameters, )
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualShadowMapUniformParameters, VirtualShadowMap)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, PageTable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, PageFlags)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, HPageFlags)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint4 >, PageRectBounds)
END_SHADER_PARAMETER_STRUCT()

FMatrix CalcTranslatedWorldToShadowUVMatrix(const FMatrix& TranslatedWorldToShadowView, const FMatrix& ViewToClip);
FMatrix CalcTranslatedWorldToShadowUVNormalMatrix(const FMatrix& TranslatedWorldToShadowView, const FMatrix& ViewToClip);

class FVirtualShadowMapArray
{
public:	
	FVirtualShadowMapArray();
	~FVirtualShadowMapArray();

	void Initialize(FRDGBuilder& GraphBuilder, FVirtualShadowMapArrayCacheManager* InCacheManager, bool bInEnabled);

	// Returns true if virtual shadow maps are enabled
	bool IsEnabled() const
	{
		return bEnabled;
	}

	FVirtualShadowMap *Allocate()
	{
		check(IsEnabled());
		FVirtualShadowMap *SM = new(FMemStack::Get()) FVirtualShadowMap(ShadowMaps.Num());
		ShadowMaps.Add(SM);
		return SM;
	}

	FIntPoint GetPhysicalPoolSize() const;

	static void SetShaderDefines(FShaderCompilerEnvironment& OutEnvironment);

	void ClearPhysicalMemory(FRDGBuilder& GraphBuilder, FRDGTextureRef& PhysicalTexture);

	void BuildPageAllocations(
		FRDGBuilder& GraphBuilder,
		const FMinimalSceneTextures& SceneTextures,
		const TArray<FViewInfo> &Views, 
		const FSortedLightSetSceneInfo& SortedLights, 
		const TArray<FVisibleLightInfo, SceneRenderingAllocator> &VisibleLightInfos, 
		const TArray<Nanite::FRasterResults, TInlineAllocator<2>> &NaniteRasterResults, 

		FScene& Scene);

	bool IsAllocated() const
	{
		return PhysicalPagePoolRDG != nullptr && PageTableRDG != nullptr;
	}

	void CreateMipViews( TArray<Nanite::FPackedView, SceneRenderingAllocator>& Views ) const;

	/**
	 * Draw old-school hardware based shadow map tiles into virtual SM.
	 */
	void RenderVirtualShadowMapsHw(FRDGBuilder& GraphBuilder, const TArray<FProjectedShadowInfo*, SceneRenderingAllocator>& VirtualSmMeshCommandPasses, FScene& Scene);

	// Draw debug info into render target 'VSMDebug' of screen-size, the mode is controlled by 'r.Shadow.Virtual.DebugVisualize'.
	void RenderDebugInfo(FRDGBuilder& GraphBuilder);
	// 
	void PrintStats(FRDGBuilder& GraphBuilder, const FViewInfo& View);

	TRDGUniformBufferRef<FVirtualShadowMapUniformParameters> GetUniformBuffer(FRDGBuilder& GraphBuilder) const;

	// Get shader parameters necessary to sample virtual shadow maps
	// It is safe to bind this buffer even if VSMs are disabled, but the sampling should be branched around in the shader.
	// This data becomes valid after the shadow depths pass if VSMs are enabled
	FVirtualShadowMapSamplingParameters GetSamplingParameters(FRDGBuilder& GraphBuilder) const;

	bool HasAnyShadowData() const { return PhysicalPagePoolRDG != nullptr;  }

	void GetPageTableParameters(FRDGBuilder& GraphBuilder, FVirtualShadowMapPageTableParameters& OutParameters);

	bool ShouldCullBackfacingPixels() const { return bCullBackfacingPixels; }

	// We keep a reference to the cache manager that was used to initialize this frame as it owns some of the buffers
	FVirtualShadowMapArrayCacheManager* CacheManager = nullptr;

	TArray<FVirtualShadowMap*, SceneRenderingAllocator> ShadowMaps;

	FVirtualShadowMapUniformParameters UniformParameters;

	// Physical page pool shadow data
	// NOTE: The underlying texture is owned by FVirtualShadowMapCacheManager.
	// We just import and maintain a copy of the RDG reference for this frame here.
	FRDGTextureRef PhysicalPagePoolRDG = nullptr;

	// Buffer that serves as the page table for all virtual shadow maps
	FRDGBufferRef PageTableRDG = nullptr;
		
	// Buffer that stores flags (uints) marking each page that needs to be rendered and cache status, for all virtual shadow maps.
	// Flag values defined in PageAccessCommon.ush
	FRDGBufferRef PageFlagsRDG = nullptr;
	// HPageFlags is a hierarchy over the PageFlags for quick query
	FRDGBufferRef HPageFlagsRDG = nullptr;

	// Allocation info for each page.
	FRDGBufferRef CachedPageInfosRDG = nullptr;
	FRDGBufferRef PhysicalPageMetaDataRDG = nullptr;

	// TODO: make transient - Buffer that stores flags marking each page that received dynamic geo.
	FRDGBufferRef DynamicCasterPageFlagsRDG = nullptr;

	// Buffer that stores flags marking each instance that needs to be invalidated the subsequent frame (handled by the cache manager).
	// This covers things like WPO or GPU-side updates, and any other case where we determine an instance needs to invalidate its footprint. 
	// Buffer of uints, organized as as follows: InvalidatingInstancesRDG[0] == count, InvalidatingInstancesRDG[1+MaxInstanceCount:1+MaxInstanceCount+MaxInstanceCount/32] == flags, 
	// InvalidatingInstancesRDG[1:MaxInstanceCount] == growing compact array of instances that need invaldation
	FRDGBufferRef InvalidatingInstancesRDG = nullptr;
	int32 NumInvalidatingInstanceSlots = 0;
	
	// uint4 buffer with one rect for each mip level in all SMs, calculated to bound committed pages
	// Used to clip the rect size of clusters during culling.
	FRDGBufferRef PageRectBoundsRDG = nullptr;
	FRDGBufferRef AllocatedPageRectBoundsRDG = nullptr;
	FRDGBufferRef ShadowMapProjectionDataRDG = nullptr;

	// HZB generated for the *current* frame's physical page pool
	// We use the *previous* frame's HZB (from VirtualShadowMapCacheManager) for culling the current frame
	FRDGTextureRef HZBPhysical = nullptr;
	TMap<int32, FVirtualShadowMapHZBMetadata> HZBMetadata;

	static constexpr uint32 NumStats = 5;
	// 0 - allocated pages
	// 1 - re-usable pages
	// 2 - Touched by dynamic
	// 3 - NumSms
	// 4 - RandRobin invalidated
	FRDGBufferRef StatsBufferRDG = nullptr;

	TRefCountPtr<IPooledRenderTarget> DebugVisualizationOutput;
	int DebugOutputType = 0;	// 0 = Disabled
	// Base ID of the light that the user has selected for debug output (if present)
	int DebugVirtualShadowMapId = INDEX_NONE;
	FRDGTextureRef DebugVisualizationProjectionOutput = nullptr;

private:
	bool bInitialized = false;

	// Are virtual shadow maps enabled? We store this at the start of the frame to centralize the logic.
	bool bEnabled = false;

	// Is backface culling of pixels enabled? We store this here to keep it consistent between projection and generation
	bool bCullBackfacingPixels = false;
};
