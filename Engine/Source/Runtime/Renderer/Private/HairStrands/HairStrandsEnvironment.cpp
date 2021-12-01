// Copyright Epic Games, Inc. All Rights Reserved.


#include "HairStrandsEnvironment.h"
#include "Shader.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "SceneTextureParameters.h"
#include "SceneRenderTargetParameters.h"
#include "RenderGraph.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "PixelShaderUtils.h"
#include "SystemTextures.h"
#include "HairStrandsRendering.h"
#include "ReflectionEnvironment.h"
#include "ScenePrivate.h"
#include "RenderGraphEvent.h"
#include "PostProcess/PostProcessing.h"
#include "ShaderDebug.h"
#include "Lumen/LumenRadianceCache.h"
#include "Lumen/LumenScreenProbeGather.h"

///////////////////////////////////////////////////////////////////////////////////////////////////

static int32 GHairScatterSceneLighting = 1;
static FAutoConsoleVariableRef CVarHairScatterSceneLighting(TEXT("r.HairStrands.ScatterSceneLighting"), GHairScatterSceneLighting, TEXT("Enable scene color lighting scattering into hair (valid for short hair only)."));

static int32 GHairSkylightingEnable = 1;
static FAutoConsoleVariableRef CVarHairSkylightingEnable(TEXT("r.HairStrands.SkyLighting"), GHairSkylightingEnable, TEXT("Enable sky lighting on hair."));

static int32 GHairSkyAOEnable = 1;
static FAutoConsoleVariableRef CVarHairSkyAOEnable(TEXT("r.HairStrands.SkyAO"), GHairSkyAOEnable, TEXT("Enable (sky) AO on hair."));

static float GHairSkylightingConeAngle = 3;
static FAutoConsoleVariableRef CVarHairSkylightingConeAngle(TEXT("r.HairStrands.SkyLighting.ConeAngle"), GHairSkylightingConeAngle, TEXT("Cone angle for tracing sky lighting on hair."));

static int32 GHairStrandsSkyLightingSampleCount = 16;
static FAutoConsoleVariableRef CVarHairStrandsSkyLightingSampleCount(TEXT("r.HairStrands.SkyLighting.SampleCount"), GHairStrandsSkyLightingSampleCount, TEXT("Number of samples used for evaluating multiple scattering and visible area (default is set to 16)."), ECVF_Scalability | ECVF_RenderThreadSafe);

static int32 GHairStrandsSkyAOSampleCount = 4;
static FAutoConsoleVariableRef CVarHairStrandsSkyAOSampleCount(TEXT("r.HairStrands.SkyAO.SampleCount"), GHairStrandsSkyAOSampleCount, TEXT("Number of samples used for evaluating hair AO (default is set to 16)."), ECVF_Scalability | ECVF_RenderThreadSafe);

static float GHairStrandsTransmissionDensityScaleFactor = 10;
static FAutoConsoleVariableRef CVarHairStrandsTransmissionDensityScaleFactor(TEXT("r.HairStrands.SkyLighting.TransmissionDensityScale"), GHairStrandsTransmissionDensityScaleFactor, TEXT("Density scale for controlling how much sky lighting is transmitted."), ECVF_Scalability | ECVF_RenderThreadSafe);

static int32 GHairStrandsSkyLightingUseHairCountTexture = 1;
static FAutoConsoleVariableRef CVarHairStrandsSkyLightingUseHairCountTexture(TEXT("r.HairStrands.SkyLighting.UseViewHairCount"), GHairStrandsSkyLightingUseHairCountTexture, TEXT("Use the view hair count texture for estimating background transmitted light (enabled by default)."), ECVF_Scalability | ECVF_RenderThreadSafe);

static float GHairStrandsSkyAODistanceThreshold = 10;
static float GHairStrandsSkyLightingDistanceThreshold = 10;
static FAutoConsoleVariableRef CVarHairStrandsSkyAOThreshold(TEXT("r.HairStrands.SkyAO.DistanceThreshold"), GHairStrandsSkyAODistanceThreshold, TEXT("Max distance for occlusion search."), ECVF_Scalability | ECVF_RenderThreadSafe);
static FAutoConsoleVariableRef CVarHairStrandsSkyLightingDistanceThreshold(TEXT("r.HairStrands.SkyLighting.DistanceThreshold"), GHairStrandsSkyLightingDistanceThreshold, TEXT("Max distance for occlusion search."), ECVF_Scalability | ECVF_RenderThreadSafe);

static int32 GHairStrandsSkyLighting_IntegrationType = 2;
static FAutoConsoleVariableRef CVarHairStrandsSkyLighting_IntegrationType(TEXT("r.HairStrands.SkyLighting.IntegrationType"), GHairStrandsSkyLighting_IntegrationType, TEXT("Hair env. lighting integration type (0:Adhoc, 1:Uniform."), ECVF_Scalability | ECVF_RenderThreadSafe);

static int32 GHairStrandsSkyLighting_DebugSample = 0;
static FAutoConsoleVariableRef CVarHairStrandsSkyLighting_DebugSample(TEXT("r.HairStrands.SkyLighting.DebugSample"), GHairStrandsSkyLighting_DebugSample, TEXT("Enable debug view for visualizing sample used for the sky integration"), ECVF_Scalability | ECVF_RenderThreadSafe);

///////////////////////////////////////////////////////////////////////////////////////////////////

enum class EHairLightingSourceType : uint8
{
	SceneColor = 0,
	ReflectionProbe = 1,
	Lumen = 2,
	Count
};

enum class EHairLightingIntegrationType : uint8
{
	SceneColor = 0,
	AdHoc = 1,
	Uniform = 2,
	SH = 3,
	Count
};

bool GetHairStrandsSkyLightingEnable()				{ return GHairSkylightingEnable > 0; }
bool GetHairStrandsSkyLightingDebugEnable()			{ return GHairSkylightingEnable > 0 && GHairStrandsSkyLighting_DebugSample > 0; }
static bool GetHairStrandsSkyAOEnable()				{ return GHairSkyAOEnable > 0; }
static float GetHairStrandsSkyLightingConeAngle()	{ return FMath::Max(0.f, GHairSkylightingConeAngle); }
DECLARE_GPU_STAT_NAMED(HairStrandsReflectionEnvironment, TEXT("Hair Strands Reflection Environment"));

///////////////////////////////////////////////////////////////////////////////////////////////////
// AO
class FHairEnvironmentAO : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairEnvironmentAO);
	SHADER_USE_PARAMETER_STRUCT(FHairEnvironmentAO, FGlobalShader)

	class FSampleSet : SHADER_PERMUTATION_INT("PERMUTATION_SAMPLESET", 2);
	using FPermutationDomain = TShaderPermutationDomain<FSampleSet>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(uint32, Voxel_MacroGroupId)
		SHADER_PARAMETER(float, Voxel_TanConeAngle)
		SHADER_PARAMETER(float, AO_Power)
		SHADER_PARAMETER(float, AO_Intensity)
		SHADER_PARAMETER(uint32, AO_SampleCount)
		SHADER_PARAMETER(float, AO_DistanceThreshold)
		SHADER_PARAMETER(uint32, Output_bHalfRes)
		SHADER_PARAMETER(FVector2f, Output_InvResolution)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)

		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHairEnvironmentAO, "/Engine/Private/HairStrands/HairStrandsEnvironmentAO.usf", "MainPS", SF_Pixel);

static void AddHairStrandsEnvironmentAOPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsVoxelResources& VoxelResources,
	const FHairStrandsMacroGroupData& MacroGroupData,
	FRDGTextureRef Output)
{
	check(Output);
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	const FIntRect Viewport = View.ViewRect;
	const FIntRect HalfResViewport = FIntRect::DivideAndRoundUp(Viewport, 2);
	const bool bHalfRes = Output->Desc.Extent.X == HalfResViewport.Width();

	FHairEnvironmentAO::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairEnvironmentAO::FParameters>();
	PassParameters->Voxel_MacroGroupId = MacroGroupData.MacroGroupId;
	PassParameters->Voxel_TanConeAngle = FMath::Tan(FMath::DegreesToRadians(GetHairStrandsSkyLightingConeAngle()));
	PassParameters->SceneTextures = SceneTextures;
	PassParameters->VirtualVoxel = VoxelResources.UniformBuffer;

	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
	const FFinalPostProcessSettings& Settings = View.FinalPostProcessSettings;
	PassParameters->AO_Power = Settings.AmbientOcclusionPower;
	PassParameters->AO_Intensity = Settings.AmbientOcclusionIntensity;
	PassParameters->AO_SampleCount = FMath::Max(uint32(GHairStrandsSkyAOSampleCount), 1u);
	PassParameters->AO_DistanceThreshold = FMath::Max(GHairStrandsSkyAODistanceThreshold, 1.f);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(Output, ERenderTargetLoadAction::ELoad);
	PassParameters->Output_bHalfRes = bHalfRes;
	PassParameters->Output_InvResolution = FVector2D(1.f/Output->Desc.Extent.X, 1.f/Output->Desc.Extent.Y);

	FIntRect ViewRect;
	if (bHalfRes)
	{
		ViewRect.Min.X = 0;
		ViewRect.Min.Y = 0;
		ViewRect.Max = Output->Desc.Extent;
	}
	else
	{
		ViewRect = View.ViewRect;
	}

	if (ShaderDrawDebug::IsEnabled(View))
	{
		ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, PassParameters->ShaderDrawParameters);
	}

	FHairEnvironmentAO::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairEnvironmentAO::FSampleSet>(PassParameters->AO_SampleCount <= 16 ? 0 : 1);

	TShaderMapRef<FHairEnvironmentAO> PixelShader(View.ShaderMap, PermutationVector);
	ClearUnusedGraphResources(PixelShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrandsAO %dx%d", ViewRect.Width(), ViewRect.Height()),
		PassParameters,
		ERDGPassFlags::Raster,
		[PassParameters, &View, PixelShader, ViewRect](FRHICommandList& InRHICmdList)
	{
		InRHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		FPixelShaderUtils::InitFullscreenPipelineState(InRHICmdList, View.ShaderMap, PixelShader, GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Min, BF_SourceColor, BF_DestColor, BO_Add, BF_Zero, BF_DestAlpha>::GetRHI();
		SetGraphicsPipelineState(InRHICmdList, GraphicsPSOInit, 0);
		SetShaderParameters(InRHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
		FPixelShaderUtils::DrawFullscreenTriangle(InRHICmdList);
	});
}

///////////////////////////////////////////////////////////////////////////////////////////////////

class FHairEnvironmentLighting
{
public:
	class FIntegrationType	: SHADER_PERMUTATION_INT("PERMUTATION_INTEGRATION_TYPE", uint32(EHairLightingIntegrationType::Count));
	class FDebug			: SHADER_PERMUTATION_BOOL("PERMUTATION_DEBUG"); 
	class FLumen			: SHADER_PERMUTATION_BOOL("PERMUTATION_LUMEN"); 
	using FPermutationDomain = TShaderPermutationDomain<FIntegrationType, FLumen, FDebug>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FForwardLightingParameters::ModifyCompilationEnvironment(Parameters.Platform, OutEnvironment);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		// Compile debug permutation only for the uniform integrator
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FIntegrationType>() != int32(EHairLightingIntegrationType::Uniform) && PermutationVector.Get<FDebug>())
		{
			return false;
		}
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		if (PermutationVector.Get<FIntegrationType>() != int32(EHairLightingIntegrationType::Uniform))
		{
			PermutationVector.Set<FDebug>(false);
		}
		return PermutationVector;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, Voxel_TanConeAngle)

		SHADER_PARAMETER(uint32, MultipleScatterSampleCount)

		SHADER_PARAMETER(float,  HairDualScatteringRoughnessOverride)
		SHADER_PARAMETER(float, TransmissionDensityScaleFactor)
		SHADER_PARAMETER(float, HairDistanceThreshold)

		SHADER_PARAMETER(FVector4f, SkyLight_OcclusionTintAndMinOcclusion)

		SHADER_PARAMETER(uint32, SkyLight_OcclusionCombineMode)
		SHADER_PARAMETER(float, SkyLight_OcclusionExponent)
		SHADER_PARAMETER(uint32, bHairUseViewHairCount)

		SHADER_PARAMETER_TEXTURE(Texture2D, PreIntegratedGF)
		SHADER_PARAMETER_SAMPLER(SamplerState, PreIntegratedGFSampler)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairEnergyLUTTexture)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer, OutLightingBuffer)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FReflectionUniformParameters, ReflectionsParameters)
		SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureShaderData, ReflectionCaptureData)
		SHADER_PARAMETER_STRUCT_REF(FForwardLightData, ForwardLightData)
	END_SHADER_PARAMETER_STRUCT()
};

class FHairEnvironmentLightingVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairEnvironmentLightingVS);
	SHADER_USE_PARAMETER_STRUCT(FHairEnvironmentLightingVS, FGlobalShader)
	//using FPermutationDomain = FHairEnvironmentLighting::FPermutationDomain;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairEnvironmentLighting::FParameters, Common)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FHairEnvironmentLighting::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("LIGHTING_VS"), 1);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
};

class FHairEnvironmentLightingPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairEnvironmentLightingPS);
	SHADER_USE_PARAMETER_STRUCT(FHairEnvironmentLightingPS, FGlobalShader)
	using FPermutationDomain = FHairEnvironmentLighting::FPermutationDomain;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairEnvironmentLighting::FParameters, Common)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCache)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStrandsDebugData::FWriteParameters, DebugData)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FHairEnvironmentLighting::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("LIGHTING_PS"), 1);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FHairEnvironmentLighting::ShouldCompilePermutation(Parameters);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairEnvironmentLightingPS, "/Engine/Private/HairStrands/HairStrandsEnvironmentLighting.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FHairEnvironmentLightingVS, "/Engine/Private/HairStrands/HairStrandsEnvironmentLighting.usf", "MainVS", SF_Vertex);

static void AddHairStrandsEnvironmentLightingPassPS(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FHairStrandsVisibilityData& VisibilityData,
	const FHairStrandsVoxelResources& VirtualVoxelResources,
	const FRDGTextureRef SceneColorTexture,
	const EHairLightingSourceType LightingType,
	const FHairStrandsDebugData::Data* DebugData)
{
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	check(VirtualVoxelResources.IsValid());

	// Render the reflection environment with tiled deferred culling
	const bool bHasBoxCaptures = (View.NumBoxReflectionCaptures > 0);
	const bool bHasSphereCaptures = (View.NumSphereReflectionCaptures > 0);
	FHairEnvironmentLightingPS::FParameters* ParametersPS = GraphBuilder.AllocParameters<FHairEnvironmentLightingPS::FParameters>();
	FHairEnvironmentLighting::FParameters* PassParameters = &ParametersPS->Common;

	PassParameters->HairEnergyLUTTexture = GetHairLUT(GraphBuilder, View, HairLUTType_MeanEnergy);

	EHairLightingIntegrationType IntegrationType = EHairLightingIntegrationType::AdHoc;
	if (LightingType == EHairLightingSourceType::SceneColor)
	{
		check(SceneColorTexture != nullptr);
		IntegrationType = EHairLightingIntegrationType::SceneColor;
		PassParameters->SceneColorTexture = SceneColorTexture;
	}
	else
	{
		switch (GHairStrandsSkyLighting_IntegrationType)
		{
			case 0: IntegrationType = EHairLightingIntegrationType::AdHoc; break;
			case 1: IntegrationType = EHairLightingIntegrationType::Uniform; break;
			case 2: IntegrationType = EHairLightingIntegrationType::SH; break;
		}
	}

	float SkyLightContrast = 0.01f;
	float SkyLightOcclusionExponent = 1.0f;
	FVector4f SkyLightOcclusionTintAndMinOcclusion(0.0f, 0.0f, 0.0f, 0.0f);
	EOcclusionCombineMode SkyLightOcclusionCombineMode = EOcclusionCombineMode::OCM_MAX;
	if (FSkyLightSceneProxy* SkyLight = Scene->SkyLight)
	{
		SkyLightOcclusionExponent = SkyLight->OcclusionExponent;
		SkyLightOcclusionTintAndMinOcclusion = FVector4f(SkyLight->OcclusionTint);
		SkyLightOcclusionTintAndMinOcclusion.W = SkyLight->MinOcclusion;
		SkyLightOcclusionCombineMode = SkyLight->OcclusionCombineMode;
	}

	PassParameters->SkyLight_OcclusionCombineMode = SkyLightOcclusionCombineMode == OCM_Minimum ? 0.0f : 1.0f;
	PassParameters->SkyLight_OcclusionExponent = SkyLightOcclusionExponent;
	PassParameters->SkyLight_OcclusionTintAndMinOcclusion = SkyLightOcclusionTintAndMinOcclusion;
	PassParameters->Voxel_TanConeAngle = FMath::Tan(FMath::DegreesToRadians(GetHairStrandsSkyLightingConeAngle()));
	PassParameters->HairDistanceThreshold = FMath::Max(GHairStrandsSkyLightingDistanceThreshold, 1.f);
	PassParameters->bHairUseViewHairCount = VisibilityData.ViewHairCountTexture && GHairStrandsSkyLightingUseHairCountTexture ? 1 : 0;
	PassParameters->MultipleScatterSampleCount = FMath::Max(uint32(GHairStrandsSkyLightingSampleCount), 1u);
	PassParameters->HairDualScatteringRoughnessOverride = GetHairDualScatteringRoughnessOverride();
	PassParameters->TransmissionDensityScaleFactor = FMath::Max(0.f, GHairStrandsTransmissionDensityScaleFactor);
	PassParameters->PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture;
	PassParameters->PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->SceneTextures = SceneTextures;
	PassParameters->VirtualVoxel = VirtualVoxelResources.UniformBuffer;
	PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->ReflectionCaptureData = View.ReflectionCaptureUniformBuffer;
	{
		FReflectionUniformParameters ReflectionUniformParameters;
		SetupReflectionUniformParameters(View, ReflectionUniformParameters);
		PassParameters->ReflectionsParameters = CreateUniformBufferImmediate(ReflectionUniformParameters, UniformBuffer_SingleDraw);
	}

	if (LightingType == EHairLightingSourceType::Lumen)
	{
		const FRadianceCacheState& RadianceCacheState = View.ViewState->RadianceCacheState;
		const LumenRadianceCache::FRadianceCacheInputs RadianceCacheInputs = LumenScreenProbeGatherRadianceCache::SetupRadianceCacheInputs();
		LumenRadianceCache::GetInterpolationParameters(View, GraphBuilder, RadianceCacheState, RadianceCacheInputs, ParametersPS->RadianceCache);
	}
	PassParameters->ForwardLightData = View.ForwardLightingResources->ForwardLightDataUniformBuffer;
	PassParameters->OutLightingBuffer = nullptr;

	if (ShaderDrawDebug::IsEnabled(View))
	{
		ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, ParametersPS->ShaderDrawParameters);
	}

	if (DebugData)
	{
		FHairStrandsDebugData::SetParameters(GraphBuilder, *DebugData, ParametersPS->DebugData);
	}

	FHairEnvironmentLightingPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairEnvironmentLighting::FIntegrationType>(uint32(IntegrationType));
	PermutationVector.Set<FHairEnvironmentLighting::FLumen>(LightingType == EHairLightingSourceType::Lumen);
	PermutationVector.Set<FHairEnvironmentLighting::FDebug>(DebugData != nullptr);
	PermutationVector = FHairEnvironmentLighting::RemapPermutation(PermutationVector);

	FIntPoint ViewportResolution = VisibilityData.SampleLightingViewportResolution;
	const FViewInfo* CapturedView = &View;
	TShaderMapRef<FHairEnvironmentLightingVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairEnvironmentLightingPS> PixelShader(View.ShaderMap, PermutationVector);

	check(VisibilityData.SampleLightingTexture);
	ParametersPS->RenderTargets[0] = FRenderTargetBinding(VisibilityData.SampleLightingTexture, ERenderTargetLoadAction::ELoad);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairEnvLightingPS(%s)", LightingType == EHairLightingSourceType::SceneColor ? TEXT("SceneScatter") : (LightingType == EHairLightingSourceType::Lumen ? TEXT("Lumen") : TEXT("ReflectionProbe")) ),
		ParametersPS,
		ERDGPassFlags::Raster,
		[ParametersPS, VertexShader, PixelShader, ViewportResolution, CapturedView](FRHICommandList& RHICmdList)
	{
		FHairEnvironmentLightingVS::FParameters ParametersVS;
		ParametersVS.Common = ParametersPS->Common;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Max, BF_SourceAlpha, BF_DestAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), ParametersVS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *ParametersPS);

		RHICmdList.SetViewport(0, 0, 0.0f, ViewportResolution.X, ViewportResolution.Y, 1.0f);
		RHICmdList.SetStreamSource(0, nullptr, 0);
		RHICmdList.DrawPrimitive(0, 1, 1);
	});
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void RenderHairStrandsSceneColorScattering(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SceneColorTexture,
	const FScene* Scene,
	TArrayView<const FViewInfo> Views)
{
	if (Views.Num() == 0 || GHairScatterSceneLighting <= 0)
		return;

	for (const FViewInfo& View : Views)
	{
		if (!HairStrands::HasViewHairStrandsData(View))
		{
			continue;
		}

		const FHairStrandsVisibilityData& VisibilityData = View.HairStrandsViewData.VisibilityData;
		const FHairStrandsVoxelResources& VoxelResources = View.HairStrandsViewData.VirtualVoxelResources;
		check(VoxelResources.IsValid());

		bool bNeedScatterSceneLighting = false;
		for (const FHairStrandsMacroGroupData& MacroGroupData : View.HairStrandsViewData.MacroGroupDatas)
		{
			if (MacroGroupData.bNeedScatterSceneLighting)
			{
				bNeedScatterSceneLighting = true;
				break;
			}
		}

		if (bNeedScatterSceneLighting)
		{
			AddHairStrandsEnvironmentLightingPassPS(GraphBuilder, Scene, View, VisibilityData, VoxelResources, SceneColorTexture, EHairLightingSourceType::SceneColor, nullptr);
		}
	}
}

static void InternalRenderHairStrandsEnvironmentLighting(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	EHairLightingSourceType LightingType)
{
	if (!GetHairStrandsSkyLightingEnable() || !HairStrands::HasViewHairStrandsData(View))
		return;

	const FHairStrandsVisibilityData& VisibilityData = View.HairStrandsViewData.VisibilityData;
	const FHairStrandsVoxelResources& VoxelResources = View.HairStrandsViewData.VirtualVoxelResources;
	const bool bRenderHairLighting = VoxelResources.IsValid() && VisibilityData.CoverageTexture;
	if (!bRenderHairLighting)
	{
		return;
	}
	
	AddHairStrandsEnvironmentLightingPassPS(GraphBuilder, Scene, View, VisibilityData, VoxelResources, nullptr, LightingType, View.HairStrandsViewData.DebugData.IsPlotDataValid() ? &View.HairStrandsViewData.DebugData.Resources : nullptr);
}

void RenderHairStrandsLumenLighting(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View)
{
	InternalRenderHairStrandsEnvironmentLighting(GraphBuilder, Scene, View, EHairLightingSourceType::Lumen);
}

void RenderHairStrandsEnvironmentLighting(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View)
{
	InternalRenderHairStrandsEnvironmentLighting(GraphBuilder, Scene, View, EHairLightingSourceType::ReflectionProbe);
}

void RenderHairStrandsAmbientOcclusion(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FRDGTextureRef& InAOTexture)
{
	if (!GetHairStrandsSkyAOEnable() || !HairStrands::HasViewHairStrandsData(View) || !InAOTexture)
		return;

	const FHairStrandsVisibilityData& VisibilityData = View.HairStrandsViewData.VisibilityData;
	const FHairStrandsVoxelResources& VoxelResources = View.HairStrandsViewData.VirtualVoxelResources;
	const bool bRenderHairLighting = VoxelResources.IsValid() && VisibilityData.CoverageTexture;
	check(bRenderHairLighting);

	for (const FHairStrandsMacroGroupData& MacroGroupData : View.HairStrandsViewData.MacroGroupDatas)
	{
		AddHairStrandsEnvironmentAOPass(GraphBuilder, View, VoxelResources, MacroGroupData, InAOTexture);
	}
}
