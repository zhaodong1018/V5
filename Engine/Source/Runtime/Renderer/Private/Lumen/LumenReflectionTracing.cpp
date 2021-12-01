// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenReflectionTracing.cpp
=============================================================================*/

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "LumenReflections.h"
#include "HairStrands/HairStrandsData.h"
#include "LumenTracingUtils.h"

int32 GLumenReflectionScreenTraces = 1;
FAutoConsoleVariableRef CVarLumenReflectionScreenTraces(
	TEXT("r.Lumen.Reflections.ScreenTraces"),
	GLumenReflectionScreenTraces,
	TEXT("Whether to trace against the screen for reflections before falling back to other methods."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionHierarchicalScreenTracesMaxIterations = 50;
FAutoConsoleVariableRef CVarLumenReflectionHierarchicalScreenTracesMaxIterations(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.MaxIterations"),
	GLumenReflectionHierarchicalScreenTracesMaxIterations,
	TEXT("Max iterations for HZB tracing."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold = .01f;
FAutoConsoleVariableRef GVarLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.RelativeDepthThickness"),
	GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold,
	TEXT("Determines depth thickness of objects hit by HZB tracing, as a relative depth threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionHairStrands_VoxelTrace = 1;
FAutoConsoleVariableRef GVarLumenReflectionHairStrands_VoxelTrace(
	TEXT("r.Lumen.Reflections.HairStrands.VoxelTrace"),
	GLumenReflectionHairStrands_VoxelTrace,
	TEXT("Whether to trace against hair voxel structure for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionHairStrands_ScreenTrace = 1;
FAutoConsoleVariableRef GVarLumenReflectionHairStrands_ScreenTrace(
	TEXT("r.Lumen.Reflections.HairStrands.ScreenTrace"),
	GLumenReflectionHairStrands_ScreenTrace,
	TEXT("Whether to trace against hair depth for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTraceCompactionGroupSizeInTiles = 16;
FAutoConsoleVariableRef GVarLumenReflectionTraceCompactionGroupSizeInTiles(
	TEXT("r.Lumen.Reflections.TraceCompaction.GroupSizeInTraceTiles"),
	GLumenReflectionTraceCompactionGroupSizeInTiles,
	TEXT("Size of the trace compaction threadgroup.  Larger group = better coherency in the compacted traces.  Currently only supported by WaveOps path."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTraceCompactionWaveOps = 1;
FAutoConsoleVariableRef CVarLumenReflectionTraceCompactionWaveOps(
	TEXT("r.Lumen.Reflections.TraceCompaction.WaveOps"),
	GLumenReflectionTraceCompactionWaveOps,
	TEXT("Whether to use Wave Ops path for trace compaction."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

class FReflectionClearTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionClearTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionClearTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionClearTracesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionClearTracesCS", SF_Compute);

class FReflectionTraceScreenTexturesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceScreenTexturesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceScreenTexturesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHZBScreenTraceParameters, HZBScreenTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER(float, MaxHierarchicalScreenTraceIterations)
		SHADER_PARAMETER(float, RelativeDepthThickness)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
	END_SHADER_PARAMETER_STRUCT()

	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_SCREEN");
	using FPermutationDomain = TShaderPermutationDomain<FHairStrands>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceScreenTexturesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceScreenTexturesCS", SF_Compute);

class FSetupCompactionIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupCompactionIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupCompactionIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactionIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionTracingTileIndirectArgs)
		SHADER_PARAMETER(uint32, CompactionThreadGroupSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupCompactionIndirectArgsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "SetupCompactionIndirectArgsCS", SF_Compute);


class FReflectionCompactTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionCompactTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionCompactTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER(float, CompactionTracingEndDistanceFromCamera)
		SHADER_PARAMETER(float, CompactionMaxTraceDistance)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionTracingTileIndirectArgs)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static uint32 GetThreadGroupSize(uint32 GroupSizeInTracingTiles)
	{
		if (GroupSizeInTracingTiles == 1)
		{
			return 64;
		}
		else if (GroupSizeInTracingTiles == 2)
		{
			return 128;
		}
		else if (GroupSizeInTracingTiles <= 4)
		{
			return 256;
		}
		else if (GroupSizeInTracingTiles <= 8)
		{
			return 512;
		}

		return 1024;
	}

	class FWaveOps : SHADER_PERMUTATION_BOOL("WAVE_OPS");
	class FThreadGroupSize : SHADER_PERMUTATION_SPARSE_INT("THREADGROUP_SIZE", 64, 128, 256, 512, 1024);
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps, FThreadGroupSize>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIFFUSE_TRACE_CARDS"), 1);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionCompactTracesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionCompactTracesCS", SF_Compute);


class FSetupReflectionCompactedTracesIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupReflectionCompactedTracesIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupReflectionCompactedTracesIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactTracingIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactRayTraceDispatchIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupReflectionCompactedTracesIndirectArgsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "SetupCompactedTracesIndirectArgsCS", SF_Compute);

class FReflectionTraceMeshSDFsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceMeshSDFsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceMeshSDFsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenMeshSDFGridParameters, MeshSDFGridParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedReflectionTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()
		
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	using FPermutationDomain = TShaderPermutationDomain<FHairStrands>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceMeshSDFsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceMeshSDFsCS", SF_Compute);


class FReflectionTraceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceVoxelsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceVoxelsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedReflectionTraceParameters, CompactedTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	using FPermutationDomain = TShaderPermutationDomain<FHairStrands, FRadianceCache>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceVoxelsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceVoxelsCS", SF_Compute);

FCompactedReflectionTraceParameters CompactTraces(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	float CompactionTracingEndDistanceFromCamera,
	float CompactionMaxTraceDistance)
{
	FRDGBufferRef CompactedTraceTexelAllocator = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Reflections.CompactedTraceTexelAllocator"));
	const int32 NumCompactedTraceTexelDataElements = ReflectionTracingParameters.ReflectionTracingBufferSize.X * ReflectionTracingParameters.ReflectionTracingBufferSize.Y;
	FRDGBufferRef CompactedTraceTexelData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32) * 2, NumCompactedTraceTexelDataElements), TEXT("Lumen.Reflections.CompactedTraceTexelData"));

	const bool bWaveOps = GLumenReflectionTraceCompactionWaveOps != 0 && GRHISupportsWaveOperations && GRHIMinimumWaveSize >= 32 && RHISupportsWaveOperations(View.GetShaderPlatform());
	// Only the wave ops path maintains trace order, switch to smaller groups without it to preserve coherency in the traces
	const uint32 CompactionThreadGroupSize = FReflectionCompactTracesCS::GetThreadGroupSize(bWaveOps ? GLumenReflectionTraceCompactionGroupSizeInTiles : 1);
	FRDGBufferRef ReflectionCompactionIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.CompactionIndirectArgs"));

	{
		FSetupCompactionIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupCompactionIndirectArgsCS::FParameters>();
		PassParameters->RWCompactedTraceTexelAllocator = GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT);
		PassParameters->RWReflectionCompactionIndirectArgs = GraphBuilder.CreateUAV(ReflectionCompactionIndirectArgs, PF_R32_UINT);
		PassParameters->ReflectionTracingTileIndirectArgs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionTileParameters.TracingIndirectArgs, PF_R32_UINT));
		PassParameters->CompactionThreadGroupSize = CompactionThreadGroupSize;

		auto ComputeShader = View.ShaderMap->GetShader<FSetupCompactionIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupCompactionIndirectArgs"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	{
		FReflectionCompactTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionCompactTracesCS::FParameters>();
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->RWCompactedTraceTexelAllocator = GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT);
		PassParameters->RWCompactedTraceTexelData = GraphBuilder.CreateUAV(CompactedTraceTexelData, PF_R32G32_UINT);
		PassParameters->ReflectionTracingTileIndirectArgs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionTileParameters.TracingIndirectArgs, PF_R32_UINT));
		PassParameters->CompactionTracingEndDistanceFromCamera = CompactionTracingEndDistanceFromCamera;
		PassParameters->CompactionMaxTraceDistance = CompactionMaxTraceDistance;
		PassParameters->IndirectArgs = ReflectionCompactionIndirectArgs;

		FReflectionCompactTracesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionCompactTracesCS::FWaveOps >(bWaveOps);
		PermutationVector.Set< FReflectionCompactTracesCS::FThreadGroupSize >(CompactionThreadGroupSize);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionCompactTracesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			bWaveOps ? RDG_EVENT_NAME("CompactTracesOrderedWaveOps %u", CompactionThreadGroupSize) : RDG_EVENT_NAME("CompactTraces"),
			ComputeShader,
			PassParameters,
			ReflectionCompactionIndirectArgs,
			0);
	}

	FCompactedReflectionTraceParameters CompactedTraceParameters;

	CompactedTraceParameters.IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.CompactTracingIndirectArgs"));
	CompactedTraceParameters.RayTraceDispatchIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.CompactRayTraceDispatchIndirectArgs"));

	{
		FSetupReflectionCompactedTracesIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupReflectionCompactedTracesIndirectArgsCS::FParameters>();
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->RWReflectionCompactTracingIndirectArgs = GraphBuilder.CreateUAV(CompactedTraceParameters.IndirectArgs, PF_R32_UINT);
		PassParameters->RWReflectionCompactRayTraceDispatchIndirectArgs = GraphBuilder.CreateUAV(CompactedTraceParameters.RayTraceDispatchIndirectArgs, PF_R32_UINT);
		PassParameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));

		auto ComputeShader = View.ShaderMap->GetShader<FSetupReflectionCompactedTracesIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupCompactedTracesIndirectArgs"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	CompactedTraceParameters.CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));
	CompactedTraceParameters.CompactedTraceTexelData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelData, PF_R32G32_UINT));

	return CompactedTraceParameters;
}

void SetupIndirectTracingParametersForReflections(FLumenIndirectTracingParameters& OutParameters)
{
	//@todo - cleanup
	OutParameters.StepFactor = 1.0f;
	OutParameters.VoxelStepFactor = 1.0f;
	extern float GDiffuseCardTraceEndDistanceFromCamera;
	OutParameters.CardTraceEndDistanceFromCamera = GDiffuseCardTraceEndDistanceFromCamera;
	OutParameters.MinSampleRadius = 0.0f;
	OutParameters.MinTraceDistance = 0.0f;
	OutParameters.MaxTraceDistance = Lumen::GetMaxTraceDistance();
	extern FLumenGatherCvarState GLumenGatherCvars;
	OutParameters.MaxMeshSDFTraceDistance = FMath::Clamp(GLumenGatherCvars.MeshSDFTraceDistance, OutParameters.MinTraceDistance, OutParameters.MaxTraceDistance);
	OutParameters.SurfaceBias = FMath::Clamp(GLumenGatherCvars.SurfaceBias, .01f, 100.0f);
	OutParameters.CardInterpolateInfluenceRadius = 10.0f;
	OutParameters.DiffuseConeHalfAngle = 0.0f;
	OutParameters.TanDiffuseConeHalfAngle = 0.0f;
	OutParameters.SpecularFromDiffuseRoughnessStart = 0.0f;
	OutParameters.SpecularFromDiffuseRoughnessEnd = 0.0f;
}

FLumenHZBScreenTraceParameters SetupHZBScreenTraceParameters(
	FRDGBuilder& GraphBuilder, 
	const FViewInfo& View,
	const FSceneTextures& SceneTextures)
{
	FRDGTextureRef CurrentSceneColor = SceneTextures.Color.Resolve;

	FRDGTextureRef InputColor = CurrentSceneColor;
	FIntPoint ViewportOffset = View.ViewRect.Min;
	FIntPoint ViewportExtent = View.ViewRect.Size();
	FIntPoint BufferSize = SceneTextures.Config.Extent;

	if (View.PrevViewInfo.CustomSSRInput.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.CustomSSRInput.RT[0]);
		ViewportOffset = View.PrevViewInfo.CustomSSRInput.ViewportRect.Min;
		ViewportExtent = View.PrevViewInfo.CustomSSRInput.ViewportRect.Size();
		BufferSize = InputColor->Desc.Extent;
	}
	else if (View.PrevViewInfo.TSRHistory.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.TSRHistory.LowFrequency);
		ViewportOffset = View.PrevViewInfo.TSRHistory.OutputViewportRect.Min;
		ViewportExtent = View.PrevViewInfo.TSRHistory.OutputViewportRect.Size();
		BufferSize = InputColor->Desc.Extent;
	}
	else if (View.PrevViewInfo.TemporalAAHistory.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.TemporalAAHistory.RT[0]);
		ViewportOffset = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Min;
		ViewportExtent = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Size();
		BufferSize = View.PrevViewInfo.TemporalAAHistory.ReferenceBufferSize;
	}
	else if (View.PrevViewInfo.ScreenSpaceRayTracingInput.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.ScreenSpaceRayTracingInput);
		ViewportOffset = View.PrevViewInfo.ViewRect.Min;
		ViewportExtent = View.PrevViewInfo.ViewRect.Size();
		BufferSize = InputColor->Desc.Extent;
	}

	FLumenHZBScreenTraceParameters Parameters;
	{
		const FVector2D HZBUvFactor(
			float(View.ViewRect.Width()) / float(2 * View.HZBMipmap0Size.X),
			float(View.ViewRect.Height()) / float(2 * View.HZBMipmap0Size.Y));
		Parameters.HZBUvFactorAndInvFactor = FVector4(
			HZBUvFactor.X,
			HZBUvFactor.Y,
			1.0f / HZBUvFactor.X,
			1.0f / HZBUvFactor.Y);

		const FVector4 ScreenPositionScaleBias = View.GetScreenPositionScaleBias(SceneTextures.Config.Extent, View.ViewRect);
		const FVector2D HZBUVToScreenUVScale = FVector2D(1.0f / HZBUvFactor.X, 1.0f / HZBUvFactor.Y) * FVector2D(2.0f, -2.0f) * FVector2D(ScreenPositionScaleBias.X, ScreenPositionScaleBias.Y);
		const FVector2D HZBUVToScreenUVBias = FVector2D(-1.0f, 1.0f) * FVector2D(ScreenPositionScaleBias.X, ScreenPositionScaleBias.Y) + FVector2D(ScreenPositionScaleBias.W, ScreenPositionScaleBias.Z);
		Parameters.HZBUVToScreenUVScaleBias = FVector4(HZBUVToScreenUVScale, HZBUVToScreenUVBias);
	}

	{
		FVector2D InvBufferSize(1.0f / float(BufferSize.X), 1.0f / float(BufferSize.Y));

		Parameters.PrevScreenPositionScaleBias = FVector4(
			ViewportExtent.X * 0.5f * InvBufferSize.X,
			-ViewportExtent.Y * 0.5f * InvBufferSize.Y,
			(ViewportExtent.X * 0.5f + ViewportOffset.X) * InvBufferSize.X,
			(ViewportExtent.Y * 0.5f + ViewportOffset.Y) * InvBufferSize.Y);
	}

	Parameters.PrevSceneColorPreExposureCorrection = InputColor != CurrentSceneColor ? View.PreExposure / View.PrevViewInfo.SceneColorPreExposure : 1.0f;

	Parameters.PrevSceneColorTexture = InputColor;
	Parameters.HistorySceneDepth = View.PrevViewInfo.DepthBuffer ? GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.DepthBuffer) : SceneTextures.Depth.Target;

	checkf(View.ClosestHZB, TEXT("Lumen screen tracing: ClosestHZB was not setup, should have been setup by FDeferredShadingSceneRenderer::RenderHzb"));
	Parameters.ClosestHZBTexture = View.ClosestHZB;
	Parameters.HZBBaseTexelSize = FVector2D(1.0f / View.ClosestHZB->Desc.Extent.X, 1.0f / View.ClosestHZB->Desc.Extent.Y);

	return Parameters;
}

void TraceReflections(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	bool bTraceMeshSDFs,
	const FSceneTextures& SceneTextures,
	const FLumenCardTracingInputs& TracingInputs,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	const FLumenMeshSDFGridParameters& InMeshSDFGridParameters,
	bool bUseRadianceCache,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters)
{
	{
		FReflectionClearTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionClearTracesCS::FParameters>();
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;

		auto ComputeShader = View.ShaderMap->GetShader<FReflectionClearTracesCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearTraces"),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}

	FLumenIndirectTracingParameters IndirectTracingParameters;
	SetupIndirectTracingParametersForReflections(IndirectTracingParameters);

	const FSceneTextureParameters& SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures);

	const bool bScreenTraces = GLumenReflectionScreenTraces != 0;

	if (bScreenTraces)
	{
		FReflectionTraceScreenTexturesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceScreenTexturesCS::FParameters>();

		PassParameters->HZBScreenTraceParameters = SetupHZBScreenTraceParameters(GraphBuilder, View, SceneTextures);
		PassParameters->View = View.ViewUniformBuffer;
		
		PassParameters->SceneTextures = SceneTextureParameters;

		if (PassParameters->HZBScreenTraceParameters.PrevSceneColorTexture == SceneTextures.Color.Resolve || !PassParameters->SceneTextures.GBufferVelocityTexture)
		{
			PassParameters->SceneTextures.GBufferVelocityTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		}

		PassParameters->MaxHierarchicalScreenTraceIterations = GLumenReflectionHierarchicalScreenTracesMaxIterations;
		PassParameters->RelativeDepthThickness = GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold;

		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;

		const bool bHasHairStrands = HairStrands::HasViewHairStrandsData(View) && GLumenReflectionHairStrands_ScreenTrace > 0;
		if (bHasHairStrands)
		{
			PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
		}

		FReflectionTraceScreenTexturesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionTraceScreenTexturesCS::FHairStrands>(bHasHairStrands);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceScreenTexturesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TraceScreen(%s)", bHasHairStrands ? TEXT("Scene, HairStrands") : TEXT("Scene")),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}
	
	bool bNeedTraceHairVoxel = HairStrands::HasViewHairStrandsVoxelData(View) && GLumenReflectionHairStrands_VoxelTrace > 0;

	if (Lumen::UseHardwareRayTracedReflections())
	{
		FCompactedReflectionTraceParameters CompactedTraceParameters = CompactTraces(
			GraphBuilder,
			View,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			WORLD_MAX,
			IndirectTracingParameters.MaxTraceDistance);

		RenderLumenHardwareRayTracingReflections(
			GraphBuilder,
			SceneTextureParameters,
			Scene,
			View,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			TracingInputs,
			CompactedTraceParameters,
			IndirectTracingParameters.MaxTraceDistance,
			bUseRadianceCache,
			RadianceCacheParameters
			);
	}
	else 
	{
		if (bTraceMeshSDFs)
		{
			FLumenMeshSDFGridParameters MeshSDFGridParameters = InMeshSDFGridParameters;
			if (!MeshSDFGridParameters.NumGridCulledMeshSDFObjects)
			{
				CullForCardTracing(
					GraphBuilder,
					Scene, View,
					TracingInputs,
					IndirectTracingParameters,
					/* out */ MeshSDFGridParameters);
			}

			if (MeshSDFGridParameters.TracingParameters.DistanceFieldObjectBuffers.NumSceneObjects > 0)
			{
				FCompactedReflectionTraceParameters CompactedTraceParameters = CompactTraces(
					GraphBuilder,
					View,
					ReflectionTracingParameters,
					ReflectionTileParameters,
					IndirectTracingParameters.CardTraceEndDistanceFromCamera,
					IndirectTracingParameters.MaxMeshSDFTraceDistance);

				{
					FReflectionTraceMeshSDFsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceMeshSDFsCS::FParameters>();
					GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);
					PassParameters->MeshSDFGridParameters = MeshSDFGridParameters;
					PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
					PassParameters->IndirectTracingParameters = IndirectTracingParameters;
					PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
					PassParameters->CompactedTraceParameters = CompactedTraceParameters;
					if (bNeedTraceHairVoxel)
					{
						PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
					}

					FReflectionTraceMeshSDFsCS::FPermutationDomain PermutationVector;
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FHairStrands >(bNeedTraceHairVoxel);
					auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceMeshSDFsCS>(PermutationVector);

					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("TraceMeshSDFs(%s)", bNeedTraceHairVoxel ? TEXT("Scene, HairStrands") : TEXT("Scene")),
						ComputeShader,
						PassParameters,
						CompactedTraceParameters.IndirectArgs,
						0);
					bNeedTraceHairVoxel = false;
				}
			}
		}

		FCompactedReflectionTraceParameters CompactedTraceParameters = CompactTraces(
			GraphBuilder,
			View,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			WORLD_MAX,
			IndirectTracingParameters.MaxTraceDistance);

		{
			FReflectionTraceVoxelsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceVoxelsCS::FParameters>();
			GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);
			PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
			PassParameters->IndirectTracingParameters = IndirectTracingParameters;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->CompactedTraceParameters = CompactedTraceParameters;
			PassParameters->RadianceCacheParameters = RadianceCacheParameters;
			if (bNeedTraceHairVoxel)
			{
				PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
			}

			FReflectionTraceVoxelsCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< FReflectionTraceVoxelsCS::FHairStrands >(bNeedTraceHairVoxel);
			PermutationVector.Set< FReflectionTraceVoxelsCS::FRadianceCache >(bUseRadianceCache);
			auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceVoxelsCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TraceVoxels(%s)", bNeedTraceHairVoxel ? TEXT("Scene, HairStrands") : TEXT("Scene")),
				ComputeShader,
				PassParameters,
				CompactedTraceParameters.IndirectArgs,
				0);
			bNeedTraceHairVoxel = false;
		}
	}
}
