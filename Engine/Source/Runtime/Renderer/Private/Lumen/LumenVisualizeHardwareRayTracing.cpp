// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "SceneTextureParameters.h"
#include "IndirectLightRendering.h"

// Actual screen-probe requirements..
#include "LumenRadianceCache.h"
#include "LumenScreenProbeGather.h"

#if RHI_RAYTRACING
#include "RayTracing/RayTracingDeferredMaterials.h"
#include "RayTracing/RaytracingOptions.h"
#include "RayTracing/RayTracingLighting.h"
#include "LumenHardwareRayTracingCommon.h"

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracing(
	TEXT("r.Lumen.Visualize.HardwareRayTracing"),
	1,
	TEXT("Enables visualization of hardware ray tracing (Default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingLightingMode(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.LightingMode"),
	0,
	TEXT("Determines the lighting mode (Default = 0)\n")
	TEXT("0: interpolate final lighting from the surface cache\n")
	TEXT("1: evaluate material, and interpolate irradiance and indirect irradiance from the surface cache\n")
	TEXT("2: evaluate material and direct lighting, and interpolate indirect irradiance from the surface cache\n")
	TEXT("3: evaluate material, direct lighting, and unshadowed skylighting at the hit point"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingDeferredMaterial(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.DeferredMaterial"),
	1,
	TEXT("Enables deferred material pipeline (Default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingDeferredMaterialTileSize(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.DeferredMaterial.TileDimension"),
	64,
	TEXT("Determines the tile dimension for material sorting (Default = 64)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingMaxTranslucentSkipCount(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.MaxTranslucentSkipCount"),
	2,
	TEXT("Determines the maximum number of translucent surfaces skipped during ray traversal (Default = 2)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingThreadCount(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.ThreadCount"),
	64,
	TEXT("Determines the active group count when dispatching raygen shader (default = 64"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingGroupCount(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.GroupCount"),
	4096,
	TEXT("Determines the active group count when dispatching raygen shader (default = 4096"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingRetraceHitLighting(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.Retrace.HitLighting"),
	1,
	TEXT("Determines whether a second trace will be fired for hit-lighting for invalid surface-cache hits (default = 1"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingRetraceFarField(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.Retrace.FarField"),
	1,
	TEXT("Determines whether a second trace will be fired for far-field contribution (default = 1"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingCompact(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.Compact"),
	1,
	TEXT("Determines whether a second trace will be compacted before traversal (default = 1"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingBucketMaterials(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.BucketMaterials"),
	1,
	TEXT("Determines whether a secondary traces will be bucketed for coherent material access (default = 1"),
	ECVF_RenderThreadSafe
);

#endif // RHI_RAYTRACING

namespace Lumen
{
	EHardwareRayTracingLightingMode GetVisualizeHardwareRayTracingLightingMode()
	{
#if RHI_RAYTRACING
		return EHardwareRayTracingLightingMode(
			FMath::Clamp(CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread(), 0, static_cast<int32>(EHardwareRayTracingLightingMode::MAX) - 1));
#else
		return EHardwareRayTracingLightingMode::LightingFromSurfaceCache;
#endif
	}

	bool ShouldVisualizeHardwareRayTracing(const FViewInfo& View)
	{
		bool bVisualize = false;
#if RHI_RAYTRACING
		bVisualize = IsRayTracingEnabled()
			&& Lumen::UseHardwareRayTracing()
			&& View.Family
			&& View.Family->EngineShowFlags.VisualizeLumenScene
			&& (CVarLumenVisualizeHardwareRayTracing.GetValueOnRenderThread() != 0);
#endif
		return bVisualize;
	}

#if RHI_RAYTRACING
	FHardwareRayTracingPermutationSettings GetVisualizeHardwareRayTracingPermutationSettings()
	{
		FHardwareRayTracingPermutationSettings ModesAndPermutationSettings;
		ModesAndPermutationSettings.LightingMode = GetVisualizeHardwareRayTracingLightingMode();
		ModesAndPermutationSettings.bUseMinimalPayload = (ModesAndPermutationSettings.LightingMode == Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache);
		ModesAndPermutationSettings.bUseDeferredMaterial = (CVarLumenVisualizeHardwareRayTracingDeferredMaterial.GetValueOnRenderThread()) != 0 && !ModesAndPermutationSettings.bUseMinimalPayload;
		return ModesAndPermutationSettings;
	}
#endif
} // namespace Lumen

#if RHI_RAYTRACING

namespace LumenVisualize
{
	// Struct definitions much match those in LumenVisualizeHardwareRayTracing.usf
	struct FTileDataPacked
	{
		uint32 PackedData;
	};

	struct FRayDataPacked
	{
		uint32 PackedData;
	};

	struct FTraceDataPacked
	{
		uint32 PackedData[2];
	};

	enum class ECompactMode
	{
		// Permutations for compaction modes
		HitLightingRetrace,
		FarFieldRetrace,
		ForceHitLighting,

		MAX
	};

} // namespace LumenVisualize

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualizeDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
}

class FLumenVisualizeCreateTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeCreateTilesCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeCreateTilesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTileAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FTileData>, RWTileDataPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeCreateTilesCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeCreateTilesCS", SF_Compute);

class FLumenVisualizeCreateRaysCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeCreateRaysCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeCreateRaysCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FTileDataPacked>, TileDataPacked)
		SHADER_PARAMETER(float, MaxTraceDistance)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWRayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FRayDataPacked>, RWRayDataPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeCreateRaysCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeCreateRaysCS", SF_Compute);

class FLumenVisualizeCompactRaysIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeCompactRaysIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeCompactRaysIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactRaysIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeCompactRaysIndirectArgsCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeCompactRaysIndirectArgsCS", SF_Compute);

class FLumenVisualizeCompactRaysCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeCompactRaysCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeCompactRaysCS, FGlobalShader);

	class FCompactModeDim : SHADER_PERMUTATION_ENUM_CLASS("DIM_COMPACT_MODE", LumenVisualize::ECompactMode);
	using FPermutationDomain = TShaderPermutationDomain<FCompactModeDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FRayDataPacked>, RayDataPacked)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FTraceDataPacked>, TraceDataPacked)

		SHADER_PARAMETER(int, MaxRayAllocationCount)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedRayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FRayDataPacked>, RWCompactedRayDataPacked)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FTraceDataPacked>, RWCompactedTraceDataPacked)

		// Indirect
		RDG_BUFFER_ACCESS(CompactRaysIndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeCompactRaysCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeCompactRaysCS", SF_Compute);

class FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWBucketRaysByMaterialIdIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 16; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS", SF_Compute);


class FLumenVisualizeBucketRaysByMaterialIdCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeBucketRaysByMaterialIdCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeBucketRaysByMaterialIdCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FRayDataPacked>, RayDataPacked)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FTraceDataPacked>, TraceDataPacked)

		SHADER_PARAMETER(int, MaxRayAllocationCount)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FRayDataPacked>, RWRayDataPacked)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FTraceDataPacked>, RWTraceDataPacked)

		// Indirect args
		RDG_BUFFER_ACCESS(BucketRaysByMaterialIdIndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 16; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeBucketRaysByMaterialIdCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeBucketRaysByMaterialIdCS", SF_Compute);

enum class ETraceMode
{
	// Permutations for tracing modes
	DefaultTrace,
	HitLightingRetrace,
	FarFieldRetrace,

	MAX
};

class FLumenVisualizeHardwareRayTracingRGS : public FLumenHardwareRayTracingRGS
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenVisualizeHardwareRayTracingRGS, FLumenHardwareRayTracingRGS)

	class FTraceModeDim : SHADER_PERMUTATION_ENUM_CLASS("DIM_TRACE_MODE", ETraceMode);
	using FPermutationDomain = TShaderPermutationDomain<FTraceModeDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingRGS::FSharedParameters, SharedParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RayAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FRayDataPacked>, RayDataPacked)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<LumenVisualize::FTraceDataPacked>, TraceDataPacked)

		SHADER_PARAMETER(int, ThreadCount)
		SHADER_PARAMETER(int, GroupCount)
		SHADER_PARAMETER(int, LightingMode)
		SHADER_PARAMETER(int, VisualizeHiResSurface)
		SHADER_PARAMETER(int, VisualizeMode)
		SHADER_PARAMETER(int, MaxTranslucentSkipCount)
		SHADER_PARAMETER(int, MaxRayAllocationCount)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(FVector3f, FarFieldReferencePos)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadiance)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<LumenVisualize::FTraceDataPacked>, RWTraceDataPacked)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FLumenHardwareRayTracingRGS::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_FEEDBACK"), 1);

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FTraceModeDim>() != ETraceMode::DefaultTrace)
		{
			OutEnvironment.SetDefine(TEXT("ENABLE_FAR_FIELD_TRACING"), 1);
		}

		if (PermutationVector.Get<FTraceModeDim>() == ETraceMode::DefaultTrace)
		{
			OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_LIGHTWEIGHT_CLOSEST_HIT_SHADER"), 1);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingRGS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "LumenVisualizeHardwareRayTracingRGS", SF_RayGen);

class FLumenVisualizeApplySkylightCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeApplySkylightCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenVisualizeApplySkylightCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(int, VisualizeMode)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadiance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeApplySkylightCS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "FLumenVisualizeApplySkylightCS", SF_Compute);

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualize(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Shading pass
	if (Lumen::ShouldVisualizeHardwareRayTracing(View))
	{
		for (int TraceMode = static_cast<int>(ETraceMode::HitLightingRetrace); TraceMode < static_cast<int>(ETraceMode::MAX); ++TraceMode)
		{
			FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FTraceModeDim>(static_cast<ETraceMode>(TraceMode));
			TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualizeLumenMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Fixed-function lighting version
	Lumen::FHardwareRayTracingPermutationSettings PermutationSettings = Lumen::GetVisualizeHardwareRayTracingPermutationSettings();

	if (Lumen::ShouldVisualizeHardwareRayTracing(View))
	{
		{
			FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FTraceModeDim>(ETraceMode::DefaultTrace);
			TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
}

#endif // RHI_RAYTRACING

void VisualizeHardwareRayTracing(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FSceneTextureParameters& SceneTextures,
	const FViewInfo& View,
	const FLumenCardTracingInputs& TracingInputs,
	FLumenIndirectTracingParameters& IndirectTracingParameters,
	FRDGTextureRef SceneColor
)
#if RHI_RAYTRACING
{
	FIntPoint ViewRectSize = View.ViewRect.Size();

	// Cache near-field and far-field trace distances
	const float FarFieldMaxTraceDistance = IndirectTracingParameters.MaxTraceDistance;
	const float MaxTraceDistance = (GetRayTracingCulling() != 0) ? GetRayTracingCullingRadius() : IndirectTracingParameters.MaxTraceDistance;

	extern int32 GVisualizeLumenSceneHiResSurface;
	extern int32 GLumenVisualizeMode;

	// Generate tiles
	FRDGBufferRef TileAllocatorBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Visualize.TileAllocator"));
	FIntPoint TileCount = FMath::DivideAndRoundUp(ViewRectSize, FIntPoint(FLumenVisualizeCreateRaysCS::GetThreadGroupSize2D()));
	uint32 MaxTileCount = TileCount.X * TileCount.Y;
	FRDGBufferRef TileDataPackedStructuredBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FTileDataPacked), MaxTileCount), TEXT("Lumen.Visualize.TileDataPacked"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TileAllocatorBuffer, PF_R32_UINT), 0);
	{
		FLumenVisualizeCreateTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCreateTilesCS::FParameters>();
		{
			// Input
			PassParameters->View = View.ViewUniformBuffer;

			// Output
			PassParameters->RWTileAllocator = GraphBuilder.CreateUAV(TileAllocatorBuffer, PF_R32_UINT);
			PassParameters->RWTileDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TileDataPackedStructuredBuffer));
		}

		TShaderRef<FLumenVisualizeCreateTilesCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCreateTilesCS>();

		const FIntVector GroupSize(TileCount.X, TileCount.Y, 1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("FLumenVisualizeCreateTilesCS"),
			ComputeShader,
			PassParameters,
			GroupSize);
	}

	// Generate rays
	// NOTE: GroupCount for emulated indirect-dispatch of raygen shaders dictates the maximum allocation size if GroupCount > MaxTileCount
	uint32 RayGenThreadCount = CVarLumenVisualizeHardwareRayTracingThreadCount.GetValueOnRenderThread();
	uint32 RayGenGroupCount = CVarLumenVisualizeHardwareRayTracingGroupCount.GetValueOnRenderThread();
	uint32 RayCount = FMath::Max(MaxTileCount, RayGenGroupCount) * FLumenVisualizeCreateRaysCS::GetThreadGroupSize1D();

	// Create rays within tiles
	FRDGBufferRef RayAllocatorBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Visualize.RayAllocator"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RayAllocatorBuffer, PF_R32_UINT), 0);

	FRDGBufferRef RayDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FRayDataPacked), RayCount), TEXT("Lumen.Visualize.RayDataPacked"));
	{
		FLumenVisualizeCreateRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCreateRaysCS::FParameters>();
		{
			// Input
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTextures = SceneTextures;
			PassParameters->TileDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TileDataPackedStructuredBuffer));
			PassParameters->MaxTraceDistance = FarFieldMaxTraceDistance;

			// Output
			PassParameters->RWRayAllocator = GraphBuilder.CreateUAV(RayAllocatorBuffer, PF_R32_UINT);
			PassParameters->RWRayDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RayDataPackedBuffer));
		}

		TShaderRef<FLumenVisualizeCreateRaysCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCreateRaysCS>();

		const FIntVector GroupSize(FMath::DivideAndRoundUp<int32>(RayCount, FLumenVisualizeCreateRaysCS::GetThreadGroupSize1D()), 1, 1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("FLumenVisualizeCreateRaysCS"),
			ComputeShader,
			PassParameters,
			GroupSize);
	}

	// Dispatch rays, resolving some of the screen with surface cache entries and collecting secondary rays for hit-lighting
	FRDGBufferRef TraceDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FTraceDataPacked), RayCount), TEXT("Lumen.Visualize.TraceDataPacked"));
	{
		FLumenVisualizeHardwareRayTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeHardwareRayTracingRGS::FParameters>();
		{
			SetLumenHardwareRayTracingSharedParameters(
				GraphBuilder,
				SceneTextures,
				View,
				TracingInputs,
				&PassParameters->SharedParameters);

			// Input
			PassParameters->RayAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayAllocatorBuffer, PF_R32_UINT));
			PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayDataPackedBuffer));

			PassParameters->ThreadCount = RayGenThreadCount;
			PassParameters->GroupCount = RayGenGroupCount;
			PassParameters->VisualizeHiResSurface = GVisualizeLumenSceneHiResSurface ? 1 : 0;;
			PassParameters->VisualizeMode = GLumenVisualizeMode;
			PassParameters->MaxTranslucentSkipCount = CVarLumenVisualizeHardwareRayTracingMaxTranslucentSkipCount.GetValueOnRenderThread();
			PassParameters->MaxRayAllocationCount = RayCount;
			PassParameters->MaxTraceDistance = MaxTraceDistance;
			PassParameters->FarFieldReferencePos = Lumen::GetFarFieldReferencePos();

			// Output
			PassParameters->RWRadiance = GraphBuilder.CreateUAV(SceneColor);
			PassParameters->RWTraceDataPacked = GraphBuilder.CreateUAV(TraceDataPackedBuffer);
		}

		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FTraceModeDim>(ETraceMode::DefaultTrace);
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);

		FIntPoint DispatchResolution = FIntPoint(RayGenThreadCount, RayGenGroupCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VisualizeHardwareRayTracing %ux%u", DispatchResolution.X, DispatchResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchResolution](FRHIRayTracingCommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.GetRayTracingSceneChecked();
				FRayTracingPipelineState* Pipeline = View.LumenHardwareRayTracingMaterialPipeline;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchResolution.X, DispatchResolution.Y);
			}
		);
	}

	// Cache current (possibly compacted) buffers for far-field tracing
	FRDGBufferRef FarFieldRayAllocatorBuffer = RayAllocatorBuffer;
	FRDGBufferRef FarFieldRayDataPackedBuffer = RayDataPackedBuffer;
	FRDGBufferRef FarFieldTraceDataPackedBuffer = TraceDataPackedBuffer;

	// Fire secondary rays for hit-lighting, resolving some of the screen and collecting miss rays
	bool bRetraceForHitLighting = CVarLumenVisualizeHardwareRayTracingRetraceHitLighting.GetValueOnRenderThread() != 0 && GLumenVisualizeMode == 0;
	bool bForceHitLighting = CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread() != 0;
	if (bRetraceForHitLighting || bForceHitLighting)
	{
		// Compact rays which need to be re-traced
		if ((CVarLumenVisualizeHardwareRayTracingCompact.GetValueOnRenderThread() != 0) || bForceHitLighting)
		{
			FRDGBufferRef CompactRaysIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Visualize.CompactTracingIndirectArgs"));
			{
				FLumenVisualizeCompactRaysIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCompactRaysIndirectArgsCS::FParameters>();
				{
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(RayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RWCompactRaysIndirectArgs = GraphBuilder.CreateUAV(CompactRaysIndirectArgsBuffer, PF_R32_UINT);
				}

				TShaderRef<FLumenVisualizeCompactRaysIndirectArgsCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCompactRaysIndirectArgsCS>();
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeCompactRaysIndirectArgsCS"),
					ComputeShader,
					PassParameters,
					FIntVector(1, 1, 1));
			}

			FRDGBufferRef CompactedRayAllocatorBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Visualize.CompactedRayAllocator"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CompactedRayAllocatorBuffer, PF_R32_UINT), 0);

			FRDGBufferRef CompactedRayDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FRayDataPacked), RayCount), TEXT("Lumen.Visualize.CompactedRayDataPacked"));
			FRDGBufferRef CompactedTraceDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FTraceDataPacked), RayCount), TEXT("Lumen.Visualize.CompactedTraceDataPacked"));
			{
				FLumenVisualizeCompactRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCompactRaysCS::FParameters>();
				{
					// Input
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(RayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayDataPackedBuffer));
					PassParameters->TraceDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TraceDataPackedBuffer));
					PassParameters->MaxRayAllocationCount = RayCount;

					// Output
					PassParameters->RWCompactedRayAllocator = GraphBuilder.CreateUAV(CompactedRayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RWCompactedRayDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CompactedRayDataPackedBuffer));
					PassParameters->RWCompactedTraceDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CompactedTraceDataPackedBuffer));

					// Indirect args
					PassParameters->CompactRaysIndirectArgs = CompactRaysIndirectArgsBuffer;
				}

				FLumenVisualizeCompactRaysCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FLumenVisualizeCompactRaysCS::FCompactModeDim>(bForceHitLighting ? LumenVisualize::ECompactMode::ForceHitLighting : LumenVisualize::ECompactMode::HitLightingRetrace);
				TShaderRef<FLumenVisualizeCompactRaysCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCompactRaysCS>(PermutationVector);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeCompactRaysCS"),
					ComputeShader,
					PassParameters,
					PassParameters->CompactRaysIndirectArgs,
					0);
			}
			RayAllocatorBuffer = CompactedRayAllocatorBuffer;
			RayDataPackedBuffer = CompactedRayDataPackedBuffer;
			TraceDataPackedBuffer = CompactedTraceDataPackedBuffer;
		}

		// Bucket rays which hit objects, but do not have a surface-cache id by their material id
		if (CVarLumenVisualizeHardwareRayTracingBucketMaterials.GetValueOnRenderThread())
		{
			FRDGBufferRef BucketRaysByMaterialIdIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Visualize.BucketRaysByMaterialIdIndirectArgsBuffer"));
			{
				FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS::FParameters>();
				{
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(RayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RWBucketRaysByMaterialIdIndirectArgs = GraphBuilder.CreateUAV(BucketRaysByMaterialIdIndirectArgsBuffer, PF_R32_UINT);
				}

				TShaderRef<FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS>();
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeBucketRaysByMaterialIdIndirectArgsCS"),
					ComputeShader,
					PassParameters,
					FIntVector(1, 1, 1));
			}

			FRDGBufferRef BucketedRayDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FRayDataPacked), RayCount), TEXT("Lumen.Visualize.BucketedRayDataPacked"));
			FRDGBufferRef BucketedTraceDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FTraceDataPacked), RayCount), TEXT("Lumen.Visualize.BucketedTraceDataPacked"));
			{
				FLumenVisualizeBucketRaysByMaterialIdCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeBucketRaysByMaterialIdCS::FParameters>();
				{
					// Input
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(RayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayDataPackedBuffer));
					PassParameters->TraceDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TraceDataPackedBuffer));
					PassParameters->MaxRayAllocationCount = RayCount;

					// Output
					PassParameters->RWRayDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BucketedRayDataPackedBuffer));
					PassParameters->RWTraceDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BucketedTraceDataPackedBuffer));

					// Indirect args
					PassParameters->BucketRaysByMaterialIdIndirectArgs = BucketRaysByMaterialIdIndirectArgsBuffer;
				}

				TShaderRef<FLumenVisualizeBucketRaysByMaterialIdCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeBucketRaysByMaterialIdCS>();
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeBucketRaysByMaterialIdCS"),
					ComputeShader,
					PassParameters,
					PassParameters->BucketRaysByMaterialIdIndirectArgs,
					0);

				RayDataPackedBuffer = BucketedRayDataPackedBuffer;
				TraceDataPackedBuffer = BucketedTraceDataPackedBuffer;
			}
		}

		FLumenVisualizeHardwareRayTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeHardwareRayTracingRGS::FParameters>();
		{
			SetLumenHardwareRayTracingSharedParameters(
				GraphBuilder,
				SceneTextures,
				View,
				TracingInputs,
				&PassParameters->SharedParameters);

			// Input
			PassParameters->RayAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayAllocatorBuffer, PF_R32_UINT));
			PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RayDataPackedBuffer));
			PassParameters->TraceDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TraceDataPackedBuffer));

			PassParameters->ThreadCount = RayGenThreadCount;
			PassParameters->GroupCount = RayGenGroupCount;
			PassParameters->LightingMode = CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread();
			PassParameters->VisualizeHiResSurface = GVisualizeLumenSceneHiResSurface ? 1 : 0;;
			PassParameters->VisualizeMode = GLumenVisualizeMode;
			PassParameters->MaxTranslucentSkipCount = CVarLumenVisualizeHardwareRayTracingMaxTranslucentSkipCount.GetValueOnRenderThread();
			PassParameters->MaxRayAllocationCount = RayCount;
			PassParameters->MaxTraceDistance = MaxTraceDistance;
			PassParameters->FarFieldReferencePos = Lumen::GetFarFieldReferencePos();

			// Output
			PassParameters->RWRadiance = GraphBuilder.CreateUAV(SceneColor);
		}

		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FTraceModeDim>(ETraceMode::HitLightingRetrace);
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);

		FIntPoint DispatchResolution = FIntPoint(RayGenThreadCount, RayGenGroupCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VisualizeHardwareRayTracing[retrace for hit-lighting] %ux%u", DispatchResolution.X, DispatchResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchResolution](FRHIRayTracingCommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.GetRayTracingSceneChecked();
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchResolution.X, DispatchResolution.Y);
			}
		);
	}

	// Resolve miss rays by firing against the distance scene (with hit-lighting)
	if (CVarLumenVisualizeHardwareRayTracingRetraceFarField.GetValueOnRenderThread() != 0
		&& GLumenVisualizeMode == 0)
	{
		// Compact rays which need to be re-traced
		if (CVarLumenVisualizeHardwareRayTracingCompact.GetValueOnRenderThread())
		{
			FRDGBufferRef CompactRaysIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Visualize.CompactTracingIndirectArgs"));
			{
				FLumenVisualizeCompactRaysIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCompactRaysIndirectArgsCS::FParameters>();
				{
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(FarFieldRayAllocatorBuffer, PF_R32_UINT);;
					PassParameters->RWCompactRaysIndirectArgs = GraphBuilder.CreateUAV(CompactRaysIndirectArgsBuffer, PF_R32_UINT);
				}

				TShaderRef<FLumenVisualizeCompactRaysIndirectArgsCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCompactRaysIndirectArgsCS>();
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeCompactRaysIndirectArgsCS"),
					ComputeShader,
					PassParameters,
					FIntVector(1, 1, 1));
			}

			FRDGBufferRef CompactedRayAllocatorBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Visualize.CompactedRayAllocator"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CompactedRayAllocatorBuffer, PF_R32_UINT), 0);

			FRDGBufferRef CompactedRayDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FRayDataPacked), RayCount), TEXT("Lumen.Visualize.CompactedRayDataPacked"));
			FRDGBufferRef CompactedTraceDataPackedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(LumenVisualize::FTraceDataPacked), RayCount), TEXT("Lumen.Visualize.CompactedTraceDataPacked"));
			{
				FLumenVisualizeCompactRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeCompactRaysCS::FParameters>();
				{
					// Input
					PassParameters->RayAllocator = GraphBuilder.CreateSRV(FarFieldRayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FarFieldRayDataPackedBuffer));
					PassParameters->TraceDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FarFieldTraceDataPackedBuffer));
					PassParameters->MaxRayAllocationCount = RayCount;

					// Output
					PassParameters->RWCompactedRayAllocator = GraphBuilder.CreateUAV(CompactedRayAllocatorBuffer, PF_R32_UINT);
					PassParameters->RWCompactedRayDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CompactedRayDataPackedBuffer));
					PassParameters->RWCompactedTraceDataPacked = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CompactedTraceDataPackedBuffer));

					// Indirect args
					PassParameters->CompactRaysIndirectArgs = CompactRaysIndirectArgsBuffer;
				}

				FLumenVisualizeCompactRaysCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FLumenVisualizeCompactRaysCS::FCompactModeDim>(LumenVisualize::ECompactMode::FarFieldRetrace);
				TShaderRef<FLumenVisualizeCompactRaysCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeCompactRaysCS>(PermutationVector);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FLumenVisualizeCompactRaysCS"),
					ComputeShader,
					PassParameters,
					PassParameters->CompactRaysIndirectArgs,
					0);
			}
			FarFieldRayAllocatorBuffer = CompactedRayAllocatorBuffer;
			FarFieldRayDataPackedBuffer = CompactedRayDataPackedBuffer;
			FarFieldTraceDataPackedBuffer = CompactedTraceDataPackedBuffer;
		}

		// Traversal to mark material

		// Bucket by material

		// Re-trace with full material for hit-lighting
		FLumenVisualizeHardwareRayTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeHardwareRayTracingRGS::FParameters>();
		{
			SetLumenHardwareRayTracingSharedParameters(
				GraphBuilder,
				SceneTextures,
				View,
				TracingInputs,
				&PassParameters->SharedParameters);

			// Input
			PassParameters->RayAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FarFieldRayAllocatorBuffer, PF_R32_UINT));
			PassParameters->RayDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FarFieldRayDataPackedBuffer));
			PassParameters->TraceDataPacked = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FarFieldTraceDataPackedBuffer));

			PassParameters->ThreadCount = RayGenThreadCount;
			PassParameters->GroupCount = RayGenGroupCount;
			PassParameters->LightingMode = CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread();
			PassParameters->VisualizeHiResSurface = GVisualizeLumenSceneHiResSurface ? 1 : 0;;
			PassParameters->VisualizeMode = GLumenVisualizeMode;
			PassParameters->MaxTranslucentSkipCount = CVarLumenVisualizeHardwareRayTracingMaxTranslucentSkipCount.GetValueOnRenderThread();
			PassParameters->MaxTraceDistance = FarFieldMaxTraceDistance;
			PassParameters->FarFieldReferencePos = Lumen::GetFarFieldReferencePos();

			// Output
			PassParameters->RWRadiance = GraphBuilder.CreateUAV(SceneColor);
		}

		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FTraceModeDim>(ETraceMode::FarFieldRetrace);
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);

		FIntPoint DispatchResolution = FIntPoint(RayGenThreadCount, RayGenGroupCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VisualizeHardwareRayTracing[retrace for far-field] %ux%u", DispatchResolution.X, DispatchResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchResolution](FRHIRayTracingCommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.GetRayTracingSceneChecked();
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchResolution.X, DispatchResolution.Y);
			}
		);
	}
	
	// Apply Sky Lighting for rays that would begin beyond FarFieldMaxTraceDistance
	{
		FLumenVisualizeApplySkylightCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeApplySkylightCS::FParameters>();
		{
			// Input
			PassParameters->SceneTextures = SceneTextures;
			GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);

			PassParameters->MaxTraceDistance = FarFieldMaxTraceDistance;
			PassParameters->VisualizeMode = GLumenVisualizeMode;

			// Output
			PassParameters->RWRadiance = GraphBuilder.CreateUAV(SceneColor);
		}

		TShaderRef<FLumenVisualizeApplySkylightCS> ComputeShader = View.ShaderMap->GetShader<FLumenVisualizeApplySkylightCS>();

		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(ViewRectSize, FLumenVisualizeApplySkylightCS::GetThreadGroupSize2D());
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("FLumenVisualizeApplySkylightCS"),
			ComputeShader,
			PassParameters,
			GroupCount);
	}
}
#else
{
	unimplemented();
}
#endif