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

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracing(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing"),
	1,
	TEXT("0. Software raytracing of diffuse indirect from Lumen cubemap tree.")
	TEXT("1. Enable hardware ray tracing of diffuse indirect. (Default)\n"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracingLightingMode(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.LightingMode"),
	0,
	TEXT("Determines the lighting mode (Default = 0)\n")
	TEXT("0: interpolate final lighting from the surface cache\n")
	TEXT("1: evaluate material, and interpolate irradiance and indirect irradiance from the surface cache\n")
	TEXT("2: evaluate material and direct lighting, and interpolate indirect irradiance from the surface cache"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracingDeferredMaterial(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.DeferredMaterial"),
	1,
	TEXT("Enables deferred material pipeline (Default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracingMinimalPayload(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.MinimalPayload"),
	1,
	TEXT("Enables deferred material pipeline (Default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracingDeferredMaterialTileSize(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.DeferredMaterial.TileDimension"),
	64,
	TEXT("Determines the tile dimension for material sorting (Default = 64)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenScreenProbeGatherHardwareRayTracingMaxTranslucentSkipCount(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.MaxTranslucentSkipCount"),
	2,
	TEXT("Determines the maximum number of translucent surfaces skipped during ray traversal (Default = 2)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenHardwareRayTracingNormalBias(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.NormalBias"),
	.1f,
	TEXT("Bias along the shading normal, useful when the Ray Tracing geometry doesn't match the GBuffer (Nanite Proxy geometry)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingCullBackFacingTriangles(
	TEXT("r.Lumen.ScreenProbeGather.HardwareRayTracing.CullBackFacingTriangles"),
	0,
	TEXT("Whether to cull backfacing triangles during ray traversal.  Culling can introduce leaking, especially where biases are required (Nanite proxy geometry)"),
	ECVF_RenderThreadSafe
);

#endif // RHI_RAYTRACING

namespace Lumen
{
	bool UseHardwareRayTracedScreenProbeGather()
	{
#if RHI_RAYTRACING
		return IsRayTracingEnabled()
			&& Lumen::UseHardwareRayTracing()
			&& (CVarLumenScreenProbeGatherHardwareRayTracing.GetValueOnRenderThread() != 0);
#else
		return false;
#endif
	}

	EHardwareRayTracingLightingMode GetScreenProbeGatherHardwareRayTracingLightingMode()
	{
#if RHI_RAYTRACING
		return EHardwareRayTracingLightingMode(FMath::Clamp(CVarLumenScreenProbeGatherHardwareRayTracingLightingMode.GetValueOnRenderThread(), 0, 2));
#else
		return EHardwareRayTracingLightingMode::LightingFromSurfaceCache;
#endif
	}

#if RHI_RAYTRACING
	FHardwareRayTracingPermutationSettings GetScreenProbeGatherHardwareRayTracingPermutationSettings()
	{
		FHardwareRayTracingPermutationSettings ModesAndPermutationSettings;
		ModesAndPermutationSettings.LightingMode = GetScreenProbeGatherHardwareRayTracingLightingMode();
		ModesAndPermutationSettings.bUseMinimalPayload = (ModesAndPermutationSettings.LightingMode == Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache);
		ModesAndPermutationSettings.bUseDeferredMaterial = (CVarLumenScreenProbeGatherHardwareRayTracingDeferredMaterial.GetValueOnRenderThread()) != 0 && !ModesAndPermutationSettings.bUseMinimalPayload;
		return ModesAndPermutationSettings;
	}
#endif
}

#if RHI_RAYTRACING

class FLumenScreenProbeGatherHardwareRayTracingRGS : public FLumenHardwareRayTracingRGS
{
	DECLARE_GLOBAL_SHADER(FLumenScreenProbeGatherHardwareRayTracingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenScreenProbeGatherHardwareRayTracingRGS, FLumenHardwareRayTracingRGS)

	class FDeferredMaterialModeDim : SHADER_PERMUTATION_BOOL("DIM_DEFERRED_MATERIAL_MODE");
	class FLightingModeDim : SHADER_PERMUTATION_INT("DIM_LIGHTING_MODE", static_cast<int32>(Lumen::EHardwareRayTracingLightingMode::MAX));
	class FRadianceCacheDim : SHADER_PERMUTATION_BOOL("DIM_RADIANCE_CACHE");
	class FStructuredImportanceSamplingDim : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	using FPermutationDomain = TShaderPermutationDomain<FDeferredMaterialModeDim, FLightingModeDim, FRadianceCacheDim, FStructuredImportanceSamplingDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingRGS::FSharedParameters, SharedParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FDeferredMaterialPayload>, DeferredMaterialBuffer)

		// Screen probes
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)

		// Constants
		SHADER_PARAMETER(float, PullbackBias)
		SHADER_PARAMETER(float, NormalBias)
		SHADER_PARAMETER(uint32, CullBackFacingTriangles)
		SHADER_PARAMETER(int, MaxTranslucentSkipCount)

		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FLumenHardwareRayTracingRGS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_DISPATCH_1D"), 1);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FLightingModeDim>() == static_cast<int32>(Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache) &&
			!PermutationVector.Get<FDeferredMaterialModeDim>())
		{
			OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_LIGHTWEIGHT_CLOSEST_HIT_SHADER"), 1);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FLumenScreenProbeGatherHardwareRayTracingRGS, "/Engine/Private/Lumen/LumenScreenProbeHardwareRayTracing.usf", "LumenScreenProbeGatherHardwareRayTracingRGS", SF_RayGen);

class FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS : public FLumenHardwareRayTracingDeferredMaterialRGS
{
	DECLARE_GLOBAL_SHADER(FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS, FLumenHardwareRayTracingDeferredMaterialRGS)

	class FRadianceCacheDim : SHADER_PERMUTATION_BOOL("DIM_RADIANCE_CACHE");
	class FStructuredImportanceSamplingDim : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	using FPermutationDomain = TShaderPermutationDomain<FRadianceCacheDim, FStructuredImportanceSamplingDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingDeferredMaterialRGS::FDeferredMaterialParameters, DeferredMaterialParameters)

		// Screen probes
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)

		// Constants
		SHADER_PARAMETER(float, PullbackBias)
		SHADER_PARAMETER(float, NormalBias)
		SHADER_PARAMETER(uint32, CullBackFacingTriangles)

		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FLumenHardwareRayTracingRGS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_DISPATCH_1D"), 1);
		OutEnvironment.SetDefine(TEXT("DIM_DEFERRED_MATERIAL_MODE"), 0);
		OutEnvironment.SetDefine(TEXT("DIM_RADIANCE_CACHE"), 0);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS, "/Engine/Private/Lumen/LumenScreenProbeHardwareRayTracing.usf", "LumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingScreenProbeGather(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (Lumen::UseHardwareRayTracedScreenProbeGather())
	{
		Lumen::FHardwareRayTracingPermutationSettings PermutationSettings = Lumen::GetScreenProbeGatherHardwareRayTracingPermutationSettings();

		FLumenScreenProbeGatherHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FDeferredMaterialModeDim>(PermutationSettings.bUseDeferredMaterial);
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FLightingModeDim>(static_cast<int>(PermutationSettings.LightingMode));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FRadianceCacheDim>(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FStructuredImportanceSamplingDim>(LumenScreenProbeGather::UseImportanceSampling(View));
		TShaderRef<FLumenScreenProbeGatherHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenScreenProbeGatherHardwareRayTracingRGS>(PermutationVector);

		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingScreenProbeGatherDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	Lumen::FHardwareRayTracingPermutationSettings PermutationSettings = Lumen::GetScreenProbeGatherHardwareRayTracingPermutationSettings();

	if (Lumen::UseHardwareRayTracedScreenProbeGather() && PermutationSettings.bUseDeferredMaterial)
	{
		FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FRadianceCacheDim>(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FStructuredImportanceSamplingDim>(LumenScreenProbeGather::UseImportanceSampling(View));
		TShaderRef<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingScreenProbeGatherLumenMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	Lumen::FHardwareRayTracingPermutationSettings PermutationSettings = Lumen::GetScreenProbeGatherHardwareRayTracingPermutationSettings();

	if (Lumen::UseHardwareRayTracedScreenProbeGather() && PermutationSettings.bUseMinimalPayload)
	{
		FLumenScreenProbeGatherHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FDeferredMaterialModeDim>(PermutationSettings.bUseDeferredMaterial);
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FLightingModeDim>(static_cast<int>(PermutationSettings.LightingMode));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FRadianceCacheDim>(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FStructuredImportanceSamplingDim>(LumenScreenProbeGather::UseImportanceSampling(View));
		TShaderRef<FLumenScreenProbeGatherHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenScreenProbeGatherHardwareRayTracingRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

#endif // RHI_RAYTRACING

void RenderHardwareRayTracingScreenProbe(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FSceneTextureParameters& SceneTextures,
	FScreenProbeParameters& ScreenProbeParameters,
	const FViewInfo& View,
	const FLumenCardTracingInputs& TracingInputs,
	FLumenIndirectTracingParameters& IndirectTracingParameters,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	const FCompactedTraceParameters& CompactedTraceParameters)
#if RHI_RAYTRACING
{
	const uint32 NumTracesPerProbe = ScreenProbeParameters.ScreenProbeTracingOctahedronResolution * ScreenProbeParameters.ScreenProbeTracingOctahedronResolution;
	FIntPoint RayTracingResolution = FIntPoint(ScreenProbeParameters.ScreenProbeAtlasViewSize.X * ScreenProbeParameters.ScreenProbeAtlasViewSize.Y * NumTracesPerProbe, 1);

	int TileSize = CVarLumenScreenProbeGatherHardwareRayTracingDeferredMaterialTileSize.GetValueOnRenderThread();
	FIntPoint DeferredMaterialBufferResolution = RayTracingResolution;
	DeferredMaterialBufferResolution.X = FMath::DivideAndRoundUp(DeferredMaterialBufferResolution.X, TileSize) * TileSize;

	int DeferredMaterialBufferNumElements = DeferredMaterialBufferResolution.X * DeferredMaterialBufferResolution.Y;
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDeferredMaterialPayload), DeferredMaterialBufferNumElements);
	FRDGBufferRef DeferredMaterialBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("Lumen.ScreenProbeGather.VisualizeHardwareRayTracingDeferredMaterialBuffer"));

	Lumen::FHardwareRayTracingPermutationSettings PermutationSettings = Lumen::GetScreenProbeGatherHardwareRayTracingPermutationSettings();
	if (PermutationSettings.bUseDeferredMaterial)
	{
		FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FParameters>();
		SetLumenHardwareRayTracingSharedParameters(
			GraphBuilder,
			SceneTextures,
			View,
			TracingInputs,
			&PassParameters->DeferredMaterialParameters.SharedParameters);

		PassParameters->IndirectTracingParameters = IndirectTracingParameters;
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;

		// Constants
		PassParameters->PullbackBias = Lumen::GetHardwareRayTracingPullbackBias();
		PassParameters->NormalBias = CVarLumenHardwareRayTracingNormalBias.GetValueOnRenderThread();
		PassParameters->CullBackFacingTriangles = CVarLumenHardwareRayTracingCullBackFacingTriangles.GetValueOnRenderThread();

		PassParameters->RadianceCacheParameters = RadianceCacheParameters;
		PassParameters->CompactedTraceParameters = CompactedTraceParameters;

		// Compact tracing becomes a 1D buffer..
		DeferredMaterialBufferResolution = FIntPoint(DeferredMaterialBufferNumElements, 1);

		// Output..
		PassParameters->DeferredMaterialParameters.RWDeferredMaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters->DeferredMaterialParameters.DeferredMaterialBufferResolution = DeferredMaterialBufferResolution;
		PassParameters->DeferredMaterialParameters.TileSize = TileSize;

		// Permutation settings
		FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FRadianceCacheDim>(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS::FStructuredImportanceSamplingDim>(LumenScreenProbeGather::UseImportanceSampling(View));
		TShaderRef<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS> RayGenerationShader =
			View.ShaderMap->GetShader<FLumenScreenProbeGatherHardwareRayTracingDeferredMaterialRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HardwareRayTracing(Payload=Deferred) %ux%u", DeferredMaterialBufferResolution.X, DeferredMaterialBufferResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DeferredMaterialBufferResolution](FRHIRayTracingCommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.GetRayTracingSceneChecked();
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialGatherPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DeferredMaterialBufferResolution.X, DeferredMaterialBufferResolution.Y);
			}
		);

		// Sort by material-id
		const uint32 SortSize = 5; // 4096 elements
		SortDeferredMaterials(GraphBuilder, View, SortSize, DeferredMaterialBufferNumElements, DeferredMaterialBuffer);
	}

	// Trace and shade
	{
		FLumenScreenProbeGatherHardwareRayTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenScreenProbeGatherHardwareRayTracingRGS::FParameters>();

		SetLumenHardwareRayTracingSharedParameters(
			GraphBuilder,
			SceneTextures,
			View,
			TracingInputs,
			&PassParameters->SharedParameters
		);
		PassParameters->DeferredMaterialBuffer = GraphBuilder.CreateSRV(DeferredMaterialBuffer);

		// Screen-probe gather arguments
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;

		// Constants
		PassParameters->PullbackBias = Lumen::GetHardwareRayTracingPullbackBias();
		PassParameters->NormalBias = CVarLumenHardwareRayTracingNormalBias.GetValueOnRenderThread();
		PassParameters->CullBackFacingTriangles = CVarLumenHardwareRayTracingCullBackFacingTriangles.GetValueOnRenderThread();
		PassParameters->MaxTranslucentSkipCount = CVarLumenScreenProbeGatherHardwareRayTracingMaxTranslucentSkipCount.GetValueOnRenderThread();

		PassParameters->RadianceCacheParameters = RadianceCacheParameters;
		PassParameters->CompactedTraceParameters = CompactedTraceParameters;

		FLumenScreenProbeGatherHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FDeferredMaterialModeDim>(PermutationSettings.bUseDeferredMaterial);
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FLightingModeDim>(static_cast<int>(PermutationSettings.LightingMode));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FRadianceCacheDim>(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FLumenScreenProbeGatherHardwareRayTracingRGS::FStructuredImportanceSamplingDim>(LumenScreenProbeGather::UseImportanceSampling(View));

		TShaderRef<FLumenScreenProbeGatherHardwareRayTracingRGS> RayGenerationShader =
			View.ShaderMap->GetShader<FLumenScreenProbeGatherHardwareRayTracingRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		const TCHAR* PassName = PermutationSettings.bUseDeferredMaterial ? TEXT("DeferredMaterialAndLighting") : TEXT("HardwareRayTracing");
		const TCHAR* LightingModeName = Lumen::GetRayTracedLightingModeName(PermutationSettings.LightingMode);
		const TCHAR* PayloadName = PermutationSettings.bUseMinimalPayload ? TEXT("Minimal") : TEXT("Default");
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("%s(LightingMode=%s Payload=%s) %ux%u", PassName, LightingModeName, PayloadName, RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, RayTracingResolution, PermutationSettings](FRHIRayTracingCommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.GetRayTracingSceneChecked();
				FRayTracingPipelineState* RayTracingPipeline = PermutationSettings.bUseMinimalPayload ? View.LumenHardwareRayTracingMaterialPipeline : View.RayTracingMaterialPipeline;
				RHICmdList.RayTraceDispatch(RayTracingPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
			}
		);
	}
}
#else // RHI_RAYTRACING
{
	unimplemented();
}
#endif // RHI_RAYTRACING
