// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
VolumetricFog.cpp
=============================================================================*/

#include "VolumetricFog.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "GlobalDistanceField.h"
#include "GlobalDistanceFieldParameters.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "DistanceFieldLightingShared.h"
#include "VolumetricFogShared.h"
#include "VolumeRendering.h"
#include "ScreenRendering.h"
#include "VolumeLighting.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "Math/Halton.h"
#include "VolumetricCloudRendering.h"
#include "Lumen/LumenTranslucencyVolumeLighting.h"
#include "GenerateConservativeDepthBuffer.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

IMPLEMENT_TYPE_LAYOUT(FVolumetricFogIntegrationParameters);
IMPLEMENT_TYPE_LAYOUT(FVolumeShadowingParameters);

int32 GVolumetricFog = 1;
FAutoConsoleVariableRef CVarVolumetricFog(
	TEXT("r.VolumetricFog"),
	GVolumetricFog,
	TEXT("Whether to allow the volumetric fog feature."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogInjectShadowedLightsSeparately = 1;
FAutoConsoleVariableRef CVarVolumetricFogInjectShadowedLightsSeparately(
	TEXT("r.VolumetricFog.InjectShadowedLightsSeparately"),
	GVolumetricFogInjectShadowedLightsSeparately,
	TEXT("Whether to allow the volumetric fog feature."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GVolumetricFogDepthDistributionScale = 32.0f;
FAutoConsoleVariableRef CVarVolumetricFogDepthDistributionScale(
	TEXT("r.VolumetricFog.DepthDistributionScale"),
	GVolumetricFogDepthDistributionScale,
	TEXT("Scales the slice depth distribution."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogGridPixelSize = 16;
FAutoConsoleVariableRef CVarVolumetricFogGridPixelSize(
	TEXT("r.VolumetricFog.GridPixelSize"),
	GVolumetricFogGridPixelSize,
	TEXT("XY Size of a cell in the voxel grid, in pixels."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogGridSizeZ = 64;
FAutoConsoleVariableRef CVarVolumetricFogGridSizeZ(
	TEXT("r.VolumetricFog.GridSizeZ"),
	GVolumetricFogGridSizeZ,
	TEXT("How many Volumetric Fog cells to use in z."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogTemporalReprojection = 1;
FAutoConsoleVariableRef CVarVolumetricFogTemporalReprojection(
	TEXT("r.VolumetricFog.TemporalReprojection"),
	GVolumetricFogTemporalReprojection,
	TEXT("Whether to use temporal reprojection on volumetric fog."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogJitter = 1;
FAutoConsoleVariableRef CVarVolumetricFogJitter(
	TEXT("r.VolumetricFog.Jitter"),
	GVolumetricFogJitter,
	TEXT("Whether to apply jitter to each frame's volumetric fog computation, achieving temporal super sampling."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GVolumetricFogHistoryWeight = .9f;
FAutoConsoleVariableRef CVarVolumetricFogHistoryWeight(
	TEXT("r.VolumetricFog.HistoryWeight"),
	GVolumetricFogHistoryWeight,
	TEXT("How much the history value should be weighted each frame.  This is a tradeoff between visible jittering and responsiveness."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GVolumetricFogHistoryMissSupersampleCount = 4;
FAutoConsoleVariableRef CVarVolumetricFogHistoryMissSupersampleCount(
	TEXT("r.VolumetricFog.HistoryMissSupersampleCount"),
	GVolumetricFogHistoryMissSupersampleCount,
	TEXT("Number of lighting samples to compute for voxels whose history value is not available.\n")
	TEXT("This reduces noise when panning or on camera cuts, but introduces a variable cost to volumetric fog computation.  Valid range [1, 16]."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GInverseSquaredLightDistanceBiasScale = 1.0f;
FAutoConsoleVariableRef CVarInverseSquaredLightDistanceBiasScale(
	TEXT("r.VolumetricFog.InverseSquaredLightDistanceBiasScale"),
	GInverseSquaredLightDistanceBiasScale,
	TEXT("Scales the amount added to the inverse squared falloff denominator.  This effectively removes the spike from inverse squared falloff that causes extreme aliasing."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GVolumetricFogEmissive = 1;
FAutoConsoleVariableRef CVarVolumetricFogEmissive(
	TEXT("r.VolumetricFog.Emissive"),
	GVolumetricFogEmissive,
	TEXT("Whether to allow the volumetric fog emissive component."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GVolumetricFogConservativeDepth = 1;
FAutoConsoleVariableRef CVarVolumetricFogConservativeDepth(
	TEXT("r.VolumetricFog.ConservativeDepth"),
	GVolumetricFogConservativeDepth,
	TEXT("Whether to allow the volumetric to use conservative depth to accelerate computations."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int GVolumetricFogLightFunction = 1;
FAutoConsoleVariableRef CVarVolumetricFogLightFunction(
	TEXT("r.VolumetricFog.LightFunction"),
	GVolumetricFogLightFunction,
	TEXT("Whether light functions are generated to be sampled when rendering volumetric fog."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLightScatteringSampleJitterMultiplier = 0;
FAutoConsoleVariableRef CVarLightScatteringSampleJitterMultiplier(
	TEXT("r.VolumetricFog.LightScatteringSampleJitterMultiplier"),
	GLightScatteringSampleJitterMultiplier,
	TEXT("Multiplier for random offset value used to jitter each world sample position when generating the 3D fog volume. Enable/disable with r.VolumetricFog.Jitter"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVolumetricFogGlobalData, "VolumetricFog");

DECLARE_GPU_STAT(VolumetricFog);

FVolumetricFogGlobalData::FVolumetricFogGlobalData()
{}

FVector VolumetricFogTemporalRandom(uint32 FrameNumber)
{
	// Center of the voxel
	FVector RandomOffsetValue(.5f, .5f, .5f);

	if (GVolumetricFogJitter && GVolumetricFogTemporalReprojection)
	{
		RandomOffsetValue = FVector(Halton(FrameNumber & 1023, 2), Halton(FrameNumber & 1023, 3), Halton(FrameNumber & 1023, 5));
	}

	return RandomOffsetValue;
}

static const uint32 VolumetricFogGridInjectionGroupSize = 4;

namespace
{
class FPermutationUseEmissive : SHADER_PERMUTATION_BOOL("USE_EMISSIVE");
}

class FVolumetricFogMaterialSetupCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVolumetricFogMaterialSetupCS)

	using FPermutationDomain = TShaderPermutationDomain<FPermutationUseEmissive>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FLinearColor, GlobalAlbedo)
		SHADER_PARAMETER(FLinearColor, GlobalEmissive)
		SHADER_PARAMETER(float, GlobalExtinctionScale)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFogUniformParameters, Fog)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWVBufferA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWVBufferB)
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportVolumetricFog(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), VolumetricFogGridInjectionGroupSize);
	}

	FVolumetricFogMaterialSetupCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(this, Initializer.PermutationId, Initializer.ParameterMap, *FParameters::FTypeInfo::GetStructMetadata());

		VolumetricFogParameters.Bind(Initializer.ParameterMap);
	}

	FVolumetricFogMaterialSetupCS()
	{
	}

	void SetParameters(
		FRHICommandList& RHICmdList,
		const FViewInfo& View,
		const FVolumetricFogIntegrationParameterData& IntegrationData)
	{
		FRHIComputeShader* ShaderRHI = RHICmdList.GetBoundComputeShader();
		VolumetricFogParameters.Set(RHICmdList, ShaderRHI, View, IntegrationData);
	}

private:

	LAYOUT_FIELD(FVolumetricFogIntegrationParameters, VolumetricFogParameters);
};

IMPLEMENT_GLOBAL_SHADER(FVolumetricFogMaterialSetupCS, "/Engine/Private/VolumetricFog.usf", "MaterialSetupCS", SF_Compute);

/** Vertex shader used to write to a range of slices of a 3d volume texture. */
class FWriteToBoundingSphereVS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FWriteToBoundingSphereVS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportVolumetricFog(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_VertexToGeometryShader);
	}

	FWriteToBoundingSphereVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		MinZ.Bind(Initializer.ParameterMap, TEXT("MinZ"));
		ViewSpaceBoundingSphere.Bind(Initializer.ParameterMap, TEXT("ViewSpaceBoundingSphere"));
		ViewToVolumeClip.Bind(Initializer.ParameterMap, TEXT("ViewToVolumeClip"));
		VolumetricFogParameters.Bind(Initializer.ParameterMap);
	}

	FWriteToBoundingSphereVS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FViewInfo& View, const FVolumetricFogIntegrationParameterData& IntegrationData, const FSphere& BoundingSphere, int32 MinZValue)
	{
		SetShaderValue(RHICmdList, RHICmdList.GetBoundVertexShader(), MinZ, MinZValue);

		const FVector ViewSpaceBoundingSphereCenter = View.ViewMatrices.GetViewMatrix().TransformPosition(BoundingSphere.Center);
		SetShaderValue(RHICmdList, RHICmdList.GetBoundVertexShader(), ViewSpaceBoundingSphere, FVector4f(ViewSpaceBoundingSphereCenter, BoundingSphere.W));

		const FMatrix44f ProjectionMatrix = View.ViewMatrices.ComputeProjectionNoAAMatrix();
		SetShaderValue(RHICmdList, RHICmdList.GetBoundVertexShader(), ViewToVolumeClip, ProjectionMatrix);

		VolumetricFogParameters.Set(RHICmdList, RHICmdList.GetBoundVertexShader(), View, IntegrationData);
	}

private:
	LAYOUT_FIELD(FShaderParameter, MinZ);
	LAYOUT_FIELD(FShaderParameter, ViewSpaceBoundingSphere);
	LAYOUT_FIELD(FShaderParameter, ViewToVolumeClip);
	LAYOUT_FIELD(FVolumetricFogIntegrationParameters, VolumetricFogParameters);
};

IMPLEMENT_SHADER_TYPE(, FWriteToBoundingSphereVS, TEXT("/Engine/Private/VolumetricFog.usf"), TEXT("WriteToBoundingSphereVS"), SF_Vertex);


/** Shader that adds direct lighting contribution from the given light to the current volume lighting cascade. */
class TInjectShadowedLocalLightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(TInjectShadowedLocalLightPS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ConservativeDepthTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVirtualShadowMapSamplingParameters, VirtualShadowMapSamplingParameters)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WhiteDummyTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightFunctionAtlasTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LightFunctionAtlasSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FDynamicallyShadowed	: SHADER_PERMUTATION_BOOL("DYNAMICALLY_SHADOWED");
	class FInverseSquared		: SHADER_PERMUTATION_BOOL("INVERSE_SQUARED_FALLOFF");
	class FTemporalReprojection : SHADER_PERMUTATION_BOOL("USE_TEMPORAL_REPROJECTION");
	class FLightFunction		: SHADER_PERMUTATION_BOOL("USE_LIGHT_FUNCTION");
	class FEnableShadows		: SHADER_PERMUTATION_BOOL("ENABLE_SHADOW_COMPUTATION");
	class FVirtualShadowMap		: SHADER_PERMUTATION_BOOL("VIRTUAL_SHADOW_MAP");

	using FPermutationDomain = TShaderPermutationDomain<
		FDynamicallyShadowed,
		FInverseSquared,
		FTemporalReprojection,
		FLightFunction,
		FEnableShadows,
		FVirtualShadowMap >;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportVolumetricFog(Parameters.Platform);
	}

	TInjectShadowedLocalLightPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		BindForLegacyShaderParameters<FParameters>(this, Initializer.PermutationId, Initializer.ParameterMap);
		Bindings.BindForLegacyShaderParameters(this, Initializer.PermutationId, Initializer.ParameterMap, *FParameters::FTypeInfo::GetStructMetadata());

		PhaseG.Bind(Initializer.ParameterMap, TEXT("PhaseG"));
		InverseSquaredLightDistanceBiasScale.Bind(Initializer.ParameterMap, TEXT("InverseSquaredLightDistanceBiasScale"));
		VolumetricFogParameters.Bind(Initializer.ParameterMap);
		VolumeShadowingParameters.Bind(Initializer.ParameterMap);

		LightFunctionMatrixParam.Bind(Initializer.ParameterMap, TEXT("LocalLightFunctionMatrix"));
		LightFunctionAtlasTileMinMaxUvBoundParam.Bind(Initializer.ParameterMap, TEXT("LightFunctionAtlasTileMinMaxUvBound"));
		LightFunctionAtlasTextureParam.Bind(Initializer.ParameterMap, TEXT("LightFunctionAtlasTexture"));
		LightFunctionAtlasSamplerParam.Bind(Initializer.ParameterMap, TEXT("LightFunctionAtlasSampler"));

		UseConservativeDepthTexture.Bind(Initializer.ParameterMap, TEXT("UseConservativeDepthTexture"));
		VirtualShadowMapIdParameter.Bind(Initializer.ParameterMap, TEXT("VirtualShadowMapId"));
	}

	TInjectShadowedLocalLightPS() {}

public:
	// @param InnerSplitIndex which CSM shadow map level, INDEX_NONE if no directional light
	// @param VolumeCascadeIndexValue which volume we render to
	void SetParameters(
		FRHICommandList& RHICmdList,
		const FViewInfo& View,
		const FVolumetricFogIntegrationParameterData& IntegrationData,
		const FLightSceneInfo* LightSceneInfo,
		const FExponentialHeightFogSceneInfo& FogInfo,
		const FProjectedShadowInfo* ShadowMap,
		bool bDynamicallyShadowed,
		int32 VirtualShadowMapId,
		const FMatrix& LightFunctionMatrix,
		FRDGTextureRef LightFunctionAtlasTexture,
		FVector4f LightFunctionAtlasTileMinMaxUvBound)
	{
		FRHIPixelShader* ShaderRHI = RHICmdList.GetBoundPixelShader();

		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, ShaderRHI, View.ViewUniformBuffer);

		SetDeferredLightParameters(RHICmdList, ShaderRHI, GetUniformBufferParameter<FDeferredLightUniformStruct>(), LightSceneInfo, View);

		VolumetricFogParameters.Set(RHICmdList, ShaderRHI, View, IntegrationData);

		SetShaderValue(RHICmdList, ShaderRHI, PhaseG, FogInfo.VolumetricFogScatteringDistribution);
		SetShaderValue(RHICmdList, ShaderRHI, InverseSquaredLightDistanceBiasScale, GInverseSquaredLightDistanceBiasScale);

		SetShaderValue(RHICmdList, ShaderRHI, LightFunctionAtlasTileMinMaxUvBoundParam, LightFunctionAtlasTileMinMaxUvBound);
		SetShaderValue(RHICmdList, ShaderRHI, LightFunctionMatrixParam, (FMatrix44f)LightFunctionMatrix);
		if (LightFunctionAtlasTextureParam.IsBound())
		{
			SetTextureParameter(RHICmdList, ShaderRHI, LightFunctionAtlasTextureParam, LightFunctionAtlasSamplerParam,
				TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI(), LightFunctionAtlasTexture->GetRHI());
		}

		SetShaderValue(RHICmdList, ShaderRHI, UseConservativeDepthTexture, GVolumetricFogConservativeDepth > 0 ? 1 : 0);

		VolumeShadowingParameters.Set(RHICmdList, ShaderRHI, View, LightSceneInfo, ShadowMap, INDEX_NONE, bDynamicallyShadowed);

		SetShaderValue(RHICmdList, ShaderRHI, VirtualShadowMapIdParameter, VirtualShadowMapId);
	}

private:
	LAYOUT_FIELD(FShaderParameter, PhaseG);
	LAYOUT_FIELD(FShaderParameter, InverseSquaredLightDistanceBiasScale);
	LAYOUT_FIELD(FVolumetricFogIntegrationParameters, VolumetricFogParameters);
	LAYOUT_FIELD(FVolumeShadowingParameters, VolumeShadowingParameters);
	LAYOUT_FIELD(FShaderParameter, UseConservativeDepthTexture);
	LAYOUT_FIELD(FShaderParameter, VirtualShadowMapIdParameter);
	LAYOUT_FIELD(FShaderParameter, LightFunctionAtlasTileMinMaxUvBoundParam);
	LAYOUT_FIELD(FShaderParameter, LightFunctionMatrixParam);
	LAYOUT_FIELD(FShaderResourceParameter, LightFunctionAtlasTextureParam);
	LAYOUT_FIELD(FShaderResourceParameter, LightFunctionAtlasSamplerParam);
};

IMPLEMENT_GLOBAL_SHADER(TInjectShadowedLocalLightPS, "/Engine/Private/VolumetricFog.usf", "InjectShadowedLocalLightPS", SF_Pixel);

void GetVolumeShadowingShaderParameters(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FLightSceneInfo* LightSceneInfo, 
	const FProjectedShadowInfo* ShadowMap, 
	int32 InnerSplitIndex, 
	bool bDynamicallyShadowed,
	FVolumeShadowingShaderParameters& OutParameters)
{
	//@todo DynamicGI: remove duplication with FVolumeShadowingParameters

	if (bDynamicallyShadowed && ShadowMap != nullptr)
	{
		FVector4f ShadowmapMinMaxValue;
		FMatrix WorldToShadowMatrixValue = ShadowMap->GetWorldToShadowMatrix(ShadowmapMinMaxValue);

		OutParameters.WorldToShadowMatrix = WorldToShadowMatrixValue;
		OutParameters.ShadowmapMinMax = ShadowmapMinMaxValue;
	}
	else
	{
		OutParameters.WorldToShadowMatrix = FMatrix::Identity;
		OutParameters.ShadowmapMinMax = FVector(1.0f);
	}

	// default to ignore the plane
	FVector4f Planes[2] = { FVector4f(0, 0, 0, -1), FVector4f(0, 0, 0, -1) };
	// .zw:DistanceFadeMAD to use MAD for efficiency in the shader, default to ignore the plane
	FVector4f ShadowInjectParamValue(1, 1, 0, 0);

	if (InnerSplitIndex >= 0)
	{
		FShadowCascadeSettings ShadowCascadeSettings;

		LightSceneInfo->Proxy->GetShadowSplitBounds(View, (uint32)InnerSplitIndex, LightSceneInfo->IsPrecomputedLightingValid(), &ShadowCascadeSettings);

		// near cascade plane
		{
			ShadowInjectParamValue.X = ShadowCascadeSettings.SplitNearFadeRegion == 0 ? 1.0f : 1.0f / ShadowCascadeSettings.SplitNearFadeRegion;
			Planes[0] = FVector4f((FVector)(ShadowCascadeSettings.NearFrustumPlane),  -ShadowCascadeSettings.NearFrustumPlane.W);
		}

		uint32 CascadeCount = LightSceneInfo->Proxy->GetNumViewDependentWholeSceneShadows(View, LightSceneInfo->IsPrecomputedLightingValid());

		// far cascade plane
		if(InnerSplitIndex != CascadeCount - 1)
		{
			ShadowInjectParamValue.Y = 1.0f / ShadowCascadeSettings.SplitFarFadeRegion;
			Planes[1] = FVector4f((FVector)(ShadowCascadeSettings.FarFrustumPlane), -ShadowCascadeSettings.FarFrustumPlane.W);
		}

		const FVector2D FadeParams = LightSceneInfo->Proxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), LightSceneInfo->IsPrecomputedLightingValid(), View.MaxShadowCascades);

		// setup constants for the MAD in shader
		ShadowInjectParamValue.Z = FadeParams.Y;
		ShadowInjectParamValue.W = -FadeParams.X * FadeParams.Y;
	}

	OutParameters.ClippingPlanes[0] = Planes[0];
	OutParameters.ClippingPlanes[1] = Planes[1];
	OutParameters.ShadowInjectParams = ShadowInjectParamValue;
	
	ELightComponentType LightType = (ELightComponentType)LightSceneInfo->Proxy->GetLightType();

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
	OutParameters.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	if (bDynamicallyShadowed && ShadowMap)
	{
		OutParameters.DepthBiasParameters = FVector4f(ShadowMap->GetShaderDepthBias(), ShadowMap->GetShaderSlopeDepthBias(), ShadowMap->GetShaderMaxSlopeDepthBias(), 1.0f / (ShadowMap->MaxSubjectZ - ShadowMap->MinSubjectZ));

		FRDGTexture* ShadowDepthTextureResource = nullptr;
		if (LightType == LightType_Point || LightType == LightType_Rect)
		{
			ShadowDepthTextureResource = SystemTextures.Black;
		}
		else
		{
			ShadowDepthTextureResource = GraphBuilder.RegisterExternalTexture(ShadowMap->RenderTargets.DepthTarget);
		}

		OutParameters.ShadowDepthTexture = ShadowDepthTextureResource;
	}
	else
	{
		OutParameters.DepthBiasParameters = FVector(1.0f);
		OutParameters.ShadowDepthTexture = SystemTextures.Black;

	}

	GetOnePassPointShadowProjectionParameters(GraphBuilder, bDynamicallyShadowed && (LightType == LightType_Point || LightType == LightType_Rect) ? ShadowMap : NULL, OutParameters.OnePassPointShadowProjection);

	const FStaticShadowDepthMap* StaticShadowDepthMap = LightSceneInfo->Proxy->GetStaticShadowDepthMap();
	const uint32 bStaticallyShadowedValue = LightSceneInfo->IsPrecomputedLightingValid() && StaticShadowDepthMap && StaticShadowDepthMap->Data && StaticShadowDepthMap->TextureRHI ? 1 : 0;
	FRHITexture* StaticShadowDepthMapTexture = bStaticallyShadowedValue ? StaticShadowDepthMap->TextureRHI : GWhiteTexture->TextureRHI;
	const FMatrix WorldToStaticShadow = bStaticallyShadowedValue ? StaticShadowDepthMap->Data->WorldToLight : FMatrix::Identity;
	const FVector4f StaticShadowBufferSizeValue = bStaticallyShadowedValue ? FVector4f(StaticShadowDepthMap->Data->ShadowMapSizeX, StaticShadowDepthMap->Data->ShadowMapSizeY, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeX, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeY) : FVector4f(0, 0, 0, 0);

	OutParameters.bStaticallyShadowed = bStaticallyShadowedValue;

	OutParameters.StaticShadowDepthTexture = StaticShadowDepthMapTexture;
	OutParameters.StaticShadowDepthTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	OutParameters.WorldToStaticShadowMatrix = WorldToStaticShadow;
	OutParameters.StaticShadowBufferSize = StaticShadowBufferSizeValue;
}

const FProjectedShadowInfo* GetShadowForInjectionIntoVolumetricFog(const FVisibleLightInfo& VisibleLightInfo)
{
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			&& !ProjectedShadowInfo->bRayTracedDistanceField)
		{
			return ProjectedShadowInfo;
		}
	}
	return nullptr;
}

bool LightNeedsSeparateInjectionIntoVolumetricFogForOpaqueShadow(const FViewInfo& View, const FLightSceneInfo* LightSceneInfo, const FVisibleLightInfo& VisibleLightInfo)
{
	const FLightSceneProxy* LightProxy = LightSceneInfo->Proxy;

	if (GVolumetricFogInjectShadowedLightsSeparately
		&& (LightProxy->GetLightType() == LightType_Point || LightProxy->GetLightType() == LightType_Spot || LightProxy->GetLightType() == LightType_Rect)
		&& !LightProxy->HasStaticLighting()
		&& LightProxy->CastsDynamicShadow()
		&& LightProxy->CastsVolumetricShadow())
	{
		const FStaticShadowDepthMap* StaticShadowDepthMap = LightProxy->GetStaticShadowDepthMap();
		const bool bStaticallyShadowed = LightSceneInfo->IsPrecomputedLightingValid() && StaticShadowDepthMap && StaticShadowDepthMap->Data && StaticShadowDepthMap->TextureRHI;
		const bool bHasVirtualShadowMap = VisibleLightInfo.GetVirtualShadowMapId( &View ) != INDEX_NONE;

		return GetShadowForInjectionIntoVolumetricFog(VisibleLightInfo) != NULL || bStaticallyShadowed || bHasVirtualShadowMap;
	}

	return false;
}

bool LightNeedsSeparateInjectionIntoVolumetricFogForLightFunction(const FLightSceneInfo* LightSceneInfo)
{
	// No directional light type because it is handled in a specific way in RenderLightFunctionForVolumetricFog.
	// TODO: add support for rect lights.
	return GVolumetricFogLightFunction > 0 && (LightSceneInfo->Proxy->GetLightType() == LightType_Point || LightSceneInfo->Proxy->GetLightType() == LightType_Spot);
}

FIntPoint CalculateVolumetricFogBoundsForLight(const FSphere& LightBounds, const FViewInfo& View, FIntVector VolumetricFogGridSize, FVector GridZParams)
{
	FIntPoint VolumeZBounds;

	FVector ViewSpaceLightBoundsOrigin = View.ViewMatrices.GetViewMatrix().TransformPosition(LightBounds.Center);

	int32 FurthestSliceIndexUnclamped = ComputeZSliceFromDepth(ViewSpaceLightBoundsOrigin.Z + LightBounds.W, GridZParams);
	int32 ClosestSliceIndexUnclamped = ComputeZSliceFromDepth(ViewSpaceLightBoundsOrigin.Z - LightBounds.W, GridZParams);

	VolumeZBounds.X = FMath::Clamp(ClosestSliceIndexUnclamped, 0, VolumetricFogGridSize.Z - 1);
	VolumeZBounds.Y = FMath::Clamp(FurthestSliceIndexUnclamped, 0, VolumetricFogGridSize.Z - 1);

	return VolumeZBounds;
}

static bool OverrideDirectionalLightInScatteringUsingHeightFog(const FViewInfo& View, const FExponentialHeightFogSceneInfo& FogInfo)
{
	return FogInfo.bOverrideLightColorsWithFogInscatteringColors && View.bUseDirectionalInscattering && !View.FogInscatteringColorCubemap;
}

static bool OverrideSkyLightInScatteringUsingHeightFog(const FViewInfo& View, const FExponentialHeightFogSceneInfo& FogInfo)
{
	return FogInfo.bOverrideLightColorsWithFogInscatteringColors;
}

/**  */
class FCircleRasterizeVertexBuffer : public FVertexBuffer
{
public:

	virtual void InitRHI() override
	{
		const int32 NumTriangles = NumVertices - 2;
		const uint32 Size = NumVertices * sizeof(FScreenVertex);
		FRHIResourceCreateInfo CreateInfo(TEXT("FCircleRasterizeVertexBuffer"));
		VertexBufferRHI = RHICreateBuffer(Size, BUF_Static | BUF_VertexBuffer, 0, ERHIAccess::VertexOrIndexBuffer, CreateInfo);
		FScreenVertex* DestVertex = (FScreenVertex*)RHILockBuffer(VertexBufferRHI, 0, Size, RLM_WriteOnly);

		const int32 NumRings = NumVertices;
		const float RadiansPerRingSegment = PI / (float)NumRings;

		// Boost the effective radius so that the edges of the circle approximation lie on the circle, instead of the vertices
		const float RadiusScale = 1.0f / FMath::Cos(RadiansPerRingSegment);

		for (int32 VertexIndex = 0; VertexIndex < NumVertices; VertexIndex++)
		{
			float Angle = VertexIndex / (float)(NumVertices - 1) * 2 * PI;
			// WriteToBoundingSphereVS only uses UV
			DestVertex[VertexIndex].Position = FVector2D(0, 0);
			DestVertex[VertexIndex].UV = FVector2D(RadiusScale * FMath::Cos(Angle) * .5f + .5f, RadiusScale * FMath::Sin(Angle) * .5f + .5f);
		}

		RHIUnlockBuffer(VertexBufferRHI);
	}

	static int32 NumVertices;
};

int32 FCircleRasterizeVertexBuffer::NumVertices = 8;

TGlobalResource<FCircleRasterizeVertexBuffer> GCircleRasterizeVertexBuffer;

/**  */
class FCircleRasterizeIndexBuffer : public FIndexBuffer
{
public:

	virtual void InitRHI() override
	{
		const int32 NumTriangles = FCircleRasterizeVertexBuffer::NumVertices - 2;

		TResourceArray<uint16, INDEXBUFFER_ALIGNMENT> Indices;
		Indices.Empty(NumTriangles * 3);

		for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles; TriangleIndex++)
		{
			int32 LeadingVertexIndex = TriangleIndex + 2;
			Indices.Add(0);
			Indices.Add(LeadingVertexIndex - 1);
			Indices.Add(LeadingVertexIndex);
		}

		const uint32 Size = Indices.GetResourceDataSize();
		const uint32 Stride = sizeof(uint16);

		// Create index buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(TEXT("FCircleRasterizeIndexBuffer"), &Indices);
		IndexBufferRHI = RHICreateIndexBuffer(Stride, Size, BUF_Static, CreateInfo);
	}
};

TGlobalResource<FCircleRasterizeIndexBuffer> GCircleRasterizeIndexBuffer;

void FDeferredShadingSceneRenderer::RenderLocalLightsForVolumetricFog(
	FRDGBuilder& GraphBuilder,
	FViewInfo& View,
	bool bUseTemporalReprojection,
	const FVolumetricFogIntegrationParameterData& IntegrationData,
	const FExponentialHeightFogSceneInfo& FogInfo,
	FIntVector VolumetricFogGridSize,
	FVector GridZParams,
	const FRDGTextureDesc& VolumeDesc,
	FRDGTexture*& OutLocalShadowedLightScattering,
	FRDGTextureRef ConservativeDepthTexture)
{
	TMap<FLightSceneInfo*, FVolumetricFogLocalLightFunctionInfo>& LocalLightFunctionData = View.VolumetricFogResources.LocalLightFunctionData;
	TArray<const FLightSceneInfo*, SceneRenderingAllocator> LightsToInject;

	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

		bool bIsShadowed = LightNeedsSeparateInjectionIntoVolumetricFogForOpaqueShadow(View, LightSceneInfo, VisibleLightInfos[LightSceneInfo->Id]);
		bool bUsesLightFunction = ViewFamily.EngineShowFlags.LightFunctions 
			&& CheckForLightFunction(LightSceneInfo) && LightNeedsSeparateInjectionIntoVolumetricFogForLightFunction(LightSceneInfo);

		if (LightSceneInfo->ShouldRenderLightViewIndependent()
			&& LightSceneInfo->ShouldRenderLight(View)
			&& (bIsShadowed || bUsesLightFunction)
			&& LightSceneInfo->Proxy->GetVolumetricScatteringIntensity() > 0)
		{
			const FSphere LightBounds = LightSceneInfo->Proxy->GetBoundingSphere();

			if ((View.ViewMatrices.GetViewOrigin() - LightBounds.Center).SizeSquared() < (FogInfo.VolumetricFogDistance + LightBounds.W) * (FogInfo.VolumetricFogDistance + LightBounds.W))
			{
				LightsToInject.Add(LightSceneInfo);
			}
		}
	}

	if (LightsToInject.Num() > 0)
	{
		OutLocalShadowedLightScattering = GraphBuilder.CreateTexture(VolumeDesc, TEXT("VolumetricFog.LocalShadowedLightScattering"));

		TInjectShadowedLocalLightPS::FParameters* PassParameters = GraphBuilder.AllocParameters<TInjectShadowedLocalLightPS::FParameters>();
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutLocalShadowedLightScattering, ERenderTargetLoadAction::EClear);
		PassParameters->VirtualShadowMapSamplingParameters = VirtualShadowMapArray.GetSamplingParameters(GraphBuilder);
		PassParameters->ConservativeDepthTexture = ConservativeDepthTexture;
		PassParameters->LightFunctionAtlasTexture = View.VolumetricFogResources.TransientLightFunctionTextureAtlas ? View.VolumetricFogResources.TransientLightFunctionTextureAtlas->GetTransientLightFunctionAtlasTexture() : GSystemTextures.GetWhiteDummy(GraphBuilder);
		PassParameters->LightFunctionAtlasSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		// We also bind the default light function texture because when we are out of atlas tile, we fallback to use a white light function so we need the RHI to be created
		PassParameters->WhiteDummyTexture = View.VolumetricFogResources.TransientLightFunctionTextureAtlas ? View.VolumetricFogResources.TransientLightFunctionTextureAtlas->GetDefaultLightFunctionTexture() : GSystemTextures.GetWhiteDummy(GraphBuilder);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ShadowedLights"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, &View, this, LightsToInject, VolumetricFogGridSize, GridZParams, bUseTemporalReprojection, IntegrationData, FogInfo](FRHICommandList& RHICmdList)
			{
				TMap<FLightSceneInfo*, FVolumetricFogLocalLightFunctionInfo>& LocalLightFunctionData = View.VolumetricFogResources.LocalLightFunctionData;

				for (int32 LightIndex = 0; LightIndex < LightsToInject.Num(); LightIndex++)
				{
					const FLightSceneInfo* LightSceneInfo = LightsToInject[LightIndex];
					const FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];
					const FProjectedShadowInfo* ProjectedShadowInfo = GetShadowForInjectionIntoVolumetricFog(VisibleLightInfo);

					const bool bInverseSquared = LightSceneInfo->Proxy->IsInverseSquared();
					const bool bDynamicallyShadowed = ProjectedShadowInfo != NULL;
					
					const FSphere LightBounds = LightSceneInfo->Proxy->GetBoundingSphere();
					const FIntPoint VolumeZBounds = CalculateVolumetricFogBoundsForLight(LightBounds, View, VolumetricFogGridSize, GridZParams);

					if (VolumeZBounds.X < VolumeZBounds.Y)
					{
						bool bIsShadowed = LightNeedsSeparateInjectionIntoVolumetricFogForOpaqueShadow(View, LightSceneInfo, VisibleLightInfo);
						bool bUsesLightFunction = ViewFamily.EngineShowFlags.LightFunctions 
							&& CheckForLightFunction(LightSceneInfo) && LightNeedsSeparateInjectionIntoVolumetricFogForLightFunction(LightSceneInfo);

						int32 VirtualShadowMapId = VisibleLightInfo.GetVirtualShadowMapId( &View );
						const bool bUseVSM = bIsShadowed && VirtualShadowMapArray.IsAllocated() && VirtualShadowMapId != INDEX_NONE;

						FRDGTextureRef LightFunctionTexture = PassParameters->LightFunctionAtlasTexture;
						FMatrix LightFunctionMatrix = FMatrix::Identity;
						FVector4f LightFunctionAtlasTileMinMaxUvBound = FVector4f(ForceInitToZero);
						if (bUsesLightFunction)
						{
							FVolumetricFogLocalLightFunctionInfo* LightFunctionData = LocalLightFunctionData.Find(LightSceneInfo);

							if (!ensure(LightFunctionData != nullptr))
							{
								// The light function data is missing but the light requires it. Skip this light for now.
								continue;
							}

							LightFunctionMatrix = LightFunctionData->LightFunctionMatrix;
							LightFunctionTexture = LightFunctionData->AtlasTile.Texture;
							LightFunctionAtlasTileMinMaxUvBound = LightFunctionData->AtlasTile.MinMaxUvBound;
						}

						TInjectShadowedLocalLightPS::FPermutationDomain PermutationVector;
						PermutationVector.Set< TInjectShadowedLocalLightPS::FDynamicallyShadowed >(bDynamicallyShadowed);
						PermutationVector.Set< TInjectShadowedLocalLightPS::FInverseSquared >(bInverseSquared);
						PermutationVector.Set< TInjectShadowedLocalLightPS::FTemporalReprojection >(bUseTemporalReprojection);
						PermutationVector.Set< TInjectShadowedLocalLightPS::FLightFunction >(bUsesLightFunction);
						PermutationVector.Set< TInjectShadowedLocalLightPS::FEnableShadows >(bIsShadowed);
						PermutationVector.Set< TInjectShadowedLocalLightPS::FVirtualShadowMap >(bUseVSM);

						auto VertexShader = View.ShaderMap->GetShader< FWriteToBoundingSphereVS >();
						TOptionalShaderMapRef<FWriteToSliceGS> GeometryShader(View.ShaderMap);
						auto PixelShader = View.ShaderMap->GetShader< TInjectShadowedLocalLightPS >(PermutationVector);

						FGraphicsPipelineStateInitializer GraphicsPSOInit;
						RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
						GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
						GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
						// Accumulate the contribution of multiple lights
						GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_One>::GetRHI();

						GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GScreenVertexDeclaration.VertexDeclarationRHI;
						GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
						GraphicsPSOInit.BoundShaderState.SetGeometryShader(GeometryShader.GetGeometryShader());
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
						GraphicsPSOInit.PrimitiveType = PT_TriangleList;

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

						PixelShader->SetParameters(RHICmdList, View, IntegrationData, LightSceneInfo, FogInfo, ProjectedShadowInfo, bDynamicallyShadowed, VirtualShadowMapId,
							LightFunctionMatrix, LightFunctionTexture, LightFunctionAtlasTileMinMaxUvBound);
						VertexShader->SetParameters(RHICmdList, View, IntegrationData, LightBounds, VolumeZBounds.X);

						if (GeometryShader.IsValid())
						{
							GeometryShader->SetParameters(RHICmdList, VolumeZBounds.X);
						}

						SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

						RHICmdList.SetStreamSource(0, GCircleRasterizeVertexBuffer.VertexBufferRHI, 0);
						const int32 NumInstances = VolumeZBounds.Y - VolumeZBounds.X;
						const int32 NumTriangles = FCircleRasterizeVertexBuffer::NumVertices - 2;
						RHICmdList.DrawIndexedPrimitive(GCircleRasterizeIndexBuffer.IndexBufferRHI, 0, 0, FCircleRasterizeVertexBuffer::NumVertices, 0, NumTriangles, NumInstances);
					}
				}
			});
	}
}

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FLumenTranslucencyLightingUniforms, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenTranslucencyLightingParameters, Parameters)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLumenTranslucencyLightingUniforms, "LumenGIVolumeStruct");

class TVolumetricFogLightScatteringCS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(TVolumetricFogLightScatteringCS, Global)

	class FTemporalReprojection			: SHADER_PERMUTATION_BOOL("USE_TEMPORAL_REPROJECTION");
	class FDistanceFieldSkyOcclusion	: SHADER_PERMUTATION_BOOL("DISTANCE_FIELD_SKY_OCCLUSION");
	class FSuperSampleCount				: SHADER_PERMUTATION_RANGE_INT("HISTORY_MISS_SUPER_SAMPLE_COUNT", 1, 16);
	class FLumenGI						: SHADER_PERMUTATION_BOOL("LUMEN_GI");
	class FVirtualShadowMap				: SHADER_PERMUTATION_BOOL("VIRTUAL_SHADOW_MAP");
	class FCloudTransmittance			: SHADER_PERMUTATION_BOOL("USE_CLOUD_TRANSMITTANCE");
	
	using FPermutationDomain = TShaderPermutationDomain<
		FSuperSampleCount,
		FTemporalReprojection,
		FDistanceFieldSkyOcclusion,
		FLumenGI,
		FVirtualShadowMap,
		FCloudTransmittance>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFogUniformParameters, Fog)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VBufferA)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VBufferB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LocalShadowedLightScattering)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightFunctionTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ConservativeDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevConservativeDepthTexture)
		SHADER_PARAMETER(FVector2f, PrevConservativeDepthTextureSize)
		SHADER_PARAMETER(uint32, UseConservativeDepthTexture)
		SHADER_PARAMETER(uint32, UseEmissive)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenTranslucencyLightingUniforms, LumenGIVolumeStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVirtualShadowMapSamplingParameters, VirtualShadowMapSamplingParameters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWLightScattering)
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportVolumetricFog(Parameters.Platform);
	}

	static FIntVector GetGroupSize()
	{
		return FIntVector(4, 4, 4);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), GetGroupSize().X);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), GetGroupSize().Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), GetGroupSize().Z);
		FForwardLightingParameters::ModifyCompilationEnvironment(Parameters.Platform, OutEnvironment);
		FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
	}

	TVolumetricFogLightScatteringCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(this, Initializer.PermutationId, Initializer.ParameterMap, *FParameters::FTypeInfo::GetStructMetadata());

		LocalShadowedLightScattering.Bind(Initializer.ParameterMap, TEXT("LocalShadowedLightScattering"));
		LightScatteringHistory.Bind(Initializer.ParameterMap, TEXT("LightScatteringHistory"));
		LightScatteringHistorySampler.Bind(Initializer.ParameterMap, TEXT("LightScatteringHistorySampler"));
		VolumetricFogParameters.Bind(Initializer.ParameterMap);
		DirectionalLightFunctionWorldToShadow.Bind(Initializer.ParameterMap, TEXT("DirectionalLightFunctionWorldToShadow"));
		LightFunctionTexture.Bind(Initializer.ParameterMap, TEXT("LightFunctionTexture"));
		LightFunctionSampler.Bind(Initializer.ParameterMap, TEXT("LightFunctionSampler"));
		ConservativeDepthTexture.Bind(Initializer.ParameterMap, TEXT("ConservativeDepthTexture"));
		PrevConservativeDepthTexture.Bind(Initializer.ParameterMap, TEXT("PrevConservativeDepthTexture"));
		PrevConservativeDepthTextureSize.Bind(Initializer.ParameterMap, TEXT("PrevConservativeDepthTextureSize"));
		UseConservativeDepthTexture.Bind(Initializer.ParameterMap, TEXT("UseConservativeDepthTexture"));
		StaticLightingScatteringIntensity.Bind(Initializer.ParameterMap, TEXT("StaticLightingScatteringIntensity"));
		SkyLightUseStaticShadowing.Bind(Initializer.ParameterMap, TEXT("SkyLightUseStaticShadowing"));
		SkyLightVolumetricScatteringIntensity.Bind(Initializer.ParameterMap, TEXT("SkyLightVolumetricScatteringIntensity"));
		SkySH.Bind(Initializer.ParameterMap, TEXT("SkySH"));
		PhaseG.Bind(Initializer.ParameterMap, TEXT("PhaseG"));
		InverseSquaredLightDistanceBiasScale.Bind(Initializer.ParameterMap, TEXT("InverseSquaredLightDistanceBiasScale"));
		UseHeightFogColors.Bind(Initializer.ParameterMap, TEXT("UseHeightFogColors"));
		UseDirectionalLightShadowing.Bind(Initializer.ParameterMap, TEXT("UseDirectionalLightShadowing"));
		AOParameters.Bind(Initializer.ParameterMap);
		GlobalDistanceFieldParameters.Bind(Initializer.ParameterMap);
		LightScatteringSampleJitterMultiplier.Bind(Initializer.ParameterMap, TEXT("LightScatteringSampleJitterMultiplier"));

		CloudShadowmapTexture.Bind(Initializer.ParameterMap, TEXT("CloudShadowmapTexture"));
		CloudShadowmapSampler.Bind(Initializer.ParameterMap, TEXT("CloudShadowmapSampler"));
		CloudShadowmapFarDepthKm.Bind(Initializer.ParameterMap, TEXT("CloudShadowmapFarDepthKm"));
		CloudShadowmapWorldToLightClipMatrix.Bind(Initializer.ParameterMap, TEXT("CloudShadowmapWorldToLightClipMatrix"));
		CloudShadowmapStrength.Bind(Initializer.ParameterMap, TEXT("CloudShadowmapStrength"));
	}

	TVolumetricFogLightScatteringCS()
	{
	}

	void SetParameters(
		FRHICommandList& RHICmdList,
		const FViewInfo& View,
		const FVolumetricFogIntegrationParameterData& IntegrationData,
		const FExponentialHeightFogSceneInfo& FogInfo,
		FRHITexture* LightScatteringHistoryTexture,
		bool bUseDirectionalLightShadowing,
		const FMatrix44f& DirectionalLightFunctionWorldToShadowValue,
		const int AtmosphericDirectionalLightIndex,
		const FLightSceneProxy* AtmosphereLightProxy,
		const FVolumetricCloudRenderSceneInfo* CloudInfo)
	{
		FRHIComputeShader* ShaderRHI = RHICmdList.GetBoundComputeShader();

		if (!LightScatteringHistoryTexture)
		{
			LightScatteringHistoryTexture = GBlackVolumeTexture->TextureRHI;
		}

		SetTextureParameter(
			RHICmdList,
			ShaderRHI,
			LightScatteringHistory,
			LightScatteringHistorySampler,
			TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI(),
			LightScatteringHistoryTexture);

		VolumetricFogParameters.Set(RHICmdList, ShaderRHI, View, IntegrationData);
		SetUniformBufferParameter(RHICmdList, ShaderRHI, GetUniformBufferParameter<FForwardLightData>(), View.ForwardLightingResources->ForwardLightDataUniformBuffer);

		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightFunctionWorldToShadow, DirectionalLightFunctionWorldToShadowValue);

		SetSamplerParameter(RHICmdList, ShaderRHI, LightFunctionSampler, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());

		FScene* Scene = (FScene*)View.Family->Scene;
		FDistanceFieldAOParameters AOParameterData(Scene->DefaultMaxDistanceFieldOcclusionDistance);
		FSkyLightSceneProxy* SkyLight = Scene->SkyLight;

		if (SkyLight
			// Skylights with static lighting had their diffuse contribution baked into lightmaps
			&& !SkyLight->bHasStaticLighting
			&& View.Family->EngineShowFlags.SkyLighting)
		{
			const float LocalSkyLightUseStaticShadowing = SkyLight->bWantsStaticShadowing && SkyLight->bCastShadows ? 1.0f : 0.0f;
			SetShaderValue(RHICmdList, ShaderRHI, SkyLightUseStaticShadowing, LocalSkyLightUseStaticShadowing);
			SetShaderValue(RHICmdList, ShaderRHI, SkyLightVolumetricScatteringIntensity, SkyLight->VolumetricScatteringIntensity);

			const FSHVectorRGB3& SkyIrradiance = SkyLight->IrradianceEnvironmentMap;
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, (FVector4f&)SkyIrradiance.R.V, 0);
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, (FVector4f&)SkyIrradiance.G.V, 1);
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, (FVector4f&)SkyIrradiance.B.V, 2);

			AOParameterData = FDistanceFieldAOParameters(SkyLight->OcclusionMaxDistance, SkyLight->Contrast);
		}
		else
		{
			SetShaderValue(RHICmdList, ShaderRHI, SkyLightUseStaticShadowing, 0.0f);
			SetShaderValue(RHICmdList, ShaderRHI, SkyLightVolumetricScatteringIntensity, 0.0f);
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, FVector4f(0, 0, 0, 0), 0);
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, FVector4f(0, 0, 0, 0), 1);
			SetShaderValue(RHICmdList, ShaderRHI, SkySH, FVector4f(0, 0, 0, 0), 2);
		}

		float StaticLightingScatteringIntensityValue = 0;

		if (View.Family->EngineShowFlags.GlobalIllumination && View.Family->EngineShowFlags.VolumetricLightmap)
		{
			StaticLightingScatteringIntensityValue = FogInfo.VolumetricFogStaticLightingScatteringIntensity;
		}

		SetShaderValue(RHICmdList, ShaderRHI, StaticLightingScatteringIntensity, StaticLightingScatteringIntensityValue);

		SetShaderValue(RHICmdList, ShaderRHI, PhaseG, FogInfo.VolumetricFogScatteringDistribution);
		SetShaderValue(RHICmdList, ShaderRHI, InverseSquaredLightDistanceBiasScale, GInverseSquaredLightDistanceBiasScale);
		SetShaderValue(RHICmdList, ShaderRHI, UseDirectionalLightShadowing, bUseDirectionalLightShadowing ? 1.0f : 0.0f);

		SetShaderValue(RHICmdList, ShaderRHI, UseHeightFogColors, FVector2f(
			OverrideDirectionalLightInScatteringUsingHeightFog(View, FogInfo) ? 1.0f : 0.0f,
			OverrideSkyLightInScatteringUsingHeightFog(View, FogInfo) ? 1.0f : 0.0f ));

		AOParameters.Set(RHICmdList, ShaderRHI, AOParameterData);
		GlobalDistanceFieldParameters.Set(RHICmdList, ShaderRHI, View.GlobalDistanceFieldInfo.ParameterData);

		if (CloudShadowmapTexture.IsBound())
		{
			FMatrix CloudWorldToLightClipShadowMatrix = FMatrix::Identity;
			float CloudShadowmap_FarDepthKm = 0.0f;
			float CloudShadowmap_Strength = 0.0f;
			IPooledRenderTarget* CloudShadowmap_Texture = nullptr;
			if (CloudInfo && AtmosphericDirectionalLightIndex >= 0 && AtmosphereLightProxy)
			{
				CloudShadowmap_Texture = View.VolumetricCloudShadowExtractedRenderTarget[AtmosphericDirectionalLightIndex];
				CloudWorldToLightClipShadowMatrix = CloudInfo->GetVolumetricCloudCommonShaderParameters().CloudShadowmapWorldToLightClipMatrix[AtmosphericDirectionalLightIndex];
				CloudShadowmap_FarDepthKm = CloudInfo->GetVolumetricCloudCommonShaderParameters().CloudShadowmapFarDepthKm[AtmosphericDirectionalLightIndex].X;
				CloudShadowmap_Strength = AtmosphereLightProxy->GetCloudShadowOnSurfaceStrength();
			}

			SetTextureParameter(
				RHICmdList,
				ShaderRHI,
				CloudShadowmapTexture,
				CloudShadowmapSampler,
				TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI(),
				CloudShadowmap_Texture ? CloudShadowmap_Texture->GetRenderTargetItem().ShaderResourceTexture : GBlackTexture->TextureRHI);

			SetShaderValue(
				RHICmdList,
				ShaderRHI,
				CloudShadowmapFarDepthKm,
				CloudShadowmap_FarDepthKm);

			SetShaderValue(
				RHICmdList,
				ShaderRHI,
				CloudShadowmapWorldToLightClipMatrix,
				(FMatrix44f)CloudWorldToLightClipShadowMatrix);

			SetShaderValue(
				RHICmdList,
				ShaderRHI,
				CloudShadowmapStrength,
				CloudShadowmap_Strength);
		}

		SetShaderValue(RHICmdList, ShaderRHI, LightScatteringSampleJitterMultiplier, GVolumetricFogJitter ? GLightScatteringSampleJitterMultiplier : 0);
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, LocalShadowedLightScattering);
	LAYOUT_FIELD(FShaderResourceParameter, LightScatteringHistory);
	LAYOUT_FIELD(FShaderResourceParameter, LightScatteringHistorySampler);
	LAYOUT_FIELD(FVolumetricFogIntegrationParameters, VolumetricFogParameters);
	LAYOUT_FIELD(FShaderParameter, DirectionalLightFunctionWorldToShadow);
	LAYOUT_FIELD(FShaderResourceParameter, LightFunctionTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LightFunctionSampler);
	LAYOUT_FIELD(FShaderParameter, StaticLightingScatteringIntensity);
	LAYOUT_FIELD(FShaderParameter, SkyLightUseStaticShadowing);
	LAYOUT_FIELD(FShaderParameter, SkyLightVolumetricScatteringIntensity);
	LAYOUT_FIELD(FShaderParameter, SkySH);
	LAYOUT_FIELD(FShaderParameter, PhaseG);
	LAYOUT_FIELD(FShaderParameter, InverseSquaredLightDistanceBiasScale);
	LAYOUT_FIELD(FShaderParameter, UseHeightFogColors);
	LAYOUT_FIELD(FShaderParameter, UseDirectionalLightShadowing);
	LAYOUT_FIELD(FAOParameters, AOParameters);
	LAYOUT_FIELD(FGlobalDistanceFieldParameters, GlobalDistanceFieldParameters);
	LAYOUT_FIELD(FShaderResourceParameter, CloudShadowmapTexture);
	LAYOUT_FIELD(FShaderResourceParameter, CloudShadowmapSampler);
	LAYOUT_FIELD(FShaderParameter, CloudShadowmapFarDepthKm);
	LAYOUT_FIELD(FShaderParameter, CloudShadowmapWorldToLightClipMatrix);
	LAYOUT_FIELD(FShaderParameter, CloudShadowmapStrength);
	LAYOUT_FIELD(FShaderResourceParameter, ConservativeDepthTexture);
	LAYOUT_FIELD(FShaderResourceParameter, PrevConservativeDepthTexture);
	LAYOUT_FIELD(FShaderParameter, PrevConservativeDepthTextureSize);
	LAYOUT_FIELD(FShaderParameter, UseConservativeDepthTexture);
	LAYOUT_FIELD(FShaderParameter, LightScatteringSampleJitterMultiplier)
};

IMPLEMENT_GLOBAL_SHADER(TVolumetricFogLightScatteringCS, "/Engine/Private/VolumetricFog.usf", "LightScatteringCS", SF_Compute);

uint32 VolumetricFogIntegrationGroupSize = 8;

class FVolumetricFogFinalIntegrationCS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FVolumetricFogFinalIntegrationCS, Global)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, LightScattering)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWIntegratedLightScattering)
	END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportVolumetricFog(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), VolumetricFogIntegrationGroupSize);
	}

	FVolumetricFogFinalIntegrationCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(this, Initializer.PermutationId, Initializer.ParameterMap, *FParameters::FTypeInfo::GetStructMetadata());

		VolumetricFogParameters.Bind(Initializer.ParameterMap);
	}

	FVolumetricFogFinalIntegrationCS()
	{
	}

public:
	void SetParameters(FRHICommandList& RHICmdList, const FViewInfo& View, const FVolumetricFogIntegrationParameterData& IntegrationData)
	{
		FRHIComputeShader* ShaderRHI = RHICmdList.GetBoundComputeShader();
		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, ShaderRHI, View.ViewUniformBuffer);
		VolumetricFogParameters.Set(RHICmdList, ShaderRHI, View, IntegrationData);
	}

private:

	LAYOUT_FIELD(FVolumetricFogIntegrationParameters, VolumetricFogParameters);
};

IMPLEMENT_SHADER_TYPE(, FVolumetricFogFinalIntegrationCS, TEXT("/Engine/Private/VolumetricFog.usf"), TEXT("FinalIntegrationCS"), SF_Compute);

bool ShouldRenderVolumetricFog(const FScene* Scene, const FSceneViewFamily& ViewFamily)
{
	return ShouldRenderFog(ViewFamily)
		&& Scene
		&& Scene->GetFeatureLevel() >= ERHIFeatureLevel::SM5
		&& DoesPlatformSupportVolumetricFog(Scene->GetShaderPlatform())
		&& GVolumetricFog
		&& ViewFamily.EngineShowFlags.VolumetricFog
		&& Scene->ExponentialFogs.Num() > 0
		&& Scene->ExponentialFogs[0].bEnableVolumetricFog
		&& Scene->ExponentialFogs[0].VolumetricFogDistance > 0;
}

FVector GetVolumetricFogGridZParams(float NearPlane, float FarPlane, int32 GridSizeZ)
{
	// S = distribution scale
	// B, O are solved for given the z distances of the first+last slice, and the # of slices.
	//
	// slice = log2(z*B + O) * S

	// Don't spend lots of resolution right in front of the near plane
	double NearOffset = .095 * 100;
	// Space out the slices so they aren't all clustered at the near plane
	double S = GVolumetricFogDepthDistributionScale;

	double N = NearPlane + NearOffset;
	double F = FarPlane;

	double O = (F - N * FMath::Exp2((GridSizeZ - 1) / S)) / (F - N);
	double B = (1 - O) / N;

	double O2 = (FMath::Exp2((GridSizeZ - 1) / S) - F / N) / (-F / N + 1);

	float FloatN = (float)N;
	float FloatF = (float)F;
	float FloatB = (float)B;
	float FloatO = (float)O;
	float FloatS = (float)S;

	float NSlice = FMath::Log2(FloatN*FloatB + FloatO) * FloatS;
	float NearPlaneSlice = FMath::Log2(NearPlane*FloatB + FloatO) * FloatS;
	float FSlice = FMath::Log2(FloatF*FloatB + FloatO) * FloatS;
	// y = log2(z*B + O) * S
	// f(N) = 0 = log2(N*B + O) * S
	// 1 = N*B + O
	// O = 1 - N*B
	// B = (1 - O) / N

	// f(F) = GLightGridSizeZ - 1 = log2(F*B + O) * S
	// exp2((GLightGridSizeZ - 1) / S) = F*B + O
	// exp2((GLightGridSizeZ - 1) / S) = F * (1 - O) / N + O
	// exp2((GLightGridSizeZ - 1) / S) = F / N - F / N * O + O
	// exp2((GLightGridSizeZ - 1) / S) = F / N + (-F / N + 1) * O
	// O = (exp2((GLightGridSizeZ - 1) / S) - F / N) / (-F / N + 1)

	return FVector(B, O, S);
}

FIntVector GetVolumetricFogGridSize(FIntPoint ViewRectSize, int32& OutVolumetricFogGridPixelSize)
{
	extern int32 GLightGridSizeZ;
	FIntPoint VolumetricFogGridSizeXY;
	int32 VolumetricFogGridPixelSize = GVolumetricFogGridPixelSize;
	VolumetricFogGridSizeXY = FIntPoint::DivideAndRoundUp(ViewRectSize, VolumetricFogGridPixelSize);
	if(VolumetricFogGridSizeXY.X > GMaxVolumeTextureDimensions || VolumetricFogGridSizeXY.Y > GMaxVolumeTextureDimensions) //clamp to max volume texture dimensions. only happens for extreme resolutions (~8x2k)
	{
		float PixelSizeX = (float)ViewRectSize.X / GMaxVolumeTextureDimensions;
		float PixelSizeY = (float)ViewRectSize.Y / GMaxVolumeTextureDimensions;
		VolumetricFogGridPixelSize = FMath::Max(FMath::CeilToInt(PixelSizeX), FMath::CeilToInt(PixelSizeY));
		VolumetricFogGridSizeXY = FIntPoint::DivideAndRoundUp(ViewRectSize, VolumetricFogGridPixelSize);
	}
	OutVolumetricFogGridPixelSize = VolumetricFogGridPixelSize;
	return FIntVector(VolumetricFogGridSizeXY.X, VolumetricFogGridSizeXY.Y, GVolumetricFogGridSizeZ);
}

void SetupVolumetricFogGlobalData(const FViewInfo& View, FVolumetricFogGlobalData& Parameters)
{
	const FScene* Scene = (FScene*)View.Family->Scene;
	const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

	int32 VolumetricFogGridPixelSize;
	const FIntVector VolumetricFogGridSize = GetVolumetricFogGridSize(View.ViewRect.Size(), VolumetricFogGridPixelSize);

	Parameters.GridSizeInt = VolumetricFogGridSize;
	Parameters.GridSize = FVector3f(VolumetricFogGridSize);

	FVector ZParams = GetVolumetricFogGridZParams(View.NearClippingDistance, FogInfo.VolumetricFogDistance, VolumetricFogGridSize.Z);
	Parameters.GridZParams = ZParams;

	Parameters.SVPosToVolumeUV = FVector2D(1.0f, 1.0f) / (FVector2D(VolumetricFogGridSize.X, VolumetricFogGridSize.Y) * VolumetricFogGridPixelSize);
	Parameters.FogGridToPixelXY = FIntPoint(VolumetricFogGridPixelSize, VolumetricFogGridPixelSize);
	Parameters.MaxDistance = FogInfo.VolumetricFogDistance;

	Parameters.HeightFogInscatteringColor = View.ExponentialFogColor;

	Parameters.HeightFogDirectionalLightInscatteringColor = FVector3f::ZeroVector;
	if (OverrideDirectionalLightInScatteringUsingHeightFog(View, FogInfo))
	{
		Parameters.HeightFogDirectionalLightInscatteringColor = FVector3f(View.DirectionalInscatteringColor);
	}
}

void FViewInfo::SetupVolumetricFogUniformBufferParameters(FViewUniformShaderParameters& ViewUniformShaderParameters) const
{
	const FScene* Scene = (const FScene*)Family->Scene;

	if (ShouldRenderVolumetricFog(Scene, *Family))
	{
		const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

		int32 VolumetricFogGridPixelSize;
		const FIntVector VolumetricFogGridSize = GetVolumetricFogGridSize(ViewRect.Size(), VolumetricFogGridPixelSize);

		ViewUniformShaderParameters.VolumetricFogInvGridSize = FVector(1.0f / VolumetricFogGridSize.X, 1.0f / VolumetricFogGridSize.Y, 1.0f / VolumetricFogGridSize.Z);

		const FVector ZParams = GetVolumetricFogGridZParams(NearClippingDistance, FogInfo.VolumetricFogDistance, VolumetricFogGridSize.Z);
		ViewUniformShaderParameters.VolumetricFogGridZParams = ZParams;

		ViewUniformShaderParameters.VolumetricFogSVPosToVolumeUV = FVector2D(1.0f, 1.0f) / (FVector2D(VolumetricFogGridSize.X, VolumetricFogGridSize.Y) * VolumetricFogGridPixelSize);
		ViewUniformShaderParameters.VolumetricFogMaxDistance = FogInfo.VolumetricFogDistance;
	}
	else
	{
		ViewUniformShaderParameters.VolumetricFogInvGridSize = FVector::ZeroVector;
		ViewUniformShaderParameters.VolumetricFogGridZParams = FVector::ZeroVector;
		ViewUniformShaderParameters.VolumetricFogSVPosToVolumeUV = FVector2D(0, 0);
		ViewUniformShaderParameters.VolumetricFogMaxDistance = 0;
	}
}

bool FDeferredShadingSceneRenderer::ShouldRenderVolumetricFog() const
{
	return ::ShouldRenderVolumetricFog(Scene, ViewFamily);
}

void FDeferredShadingSceneRenderer::SetupVolumetricFog()
{
	if (ShouldRenderVolumetricFog())
	{
		const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			FViewInfo& View = Views[ViewIndex];
			
			int32 VolumetricFogGridPixelSize;
			const FIntVector VolumetricFogGridSize = GetVolumetricFogGridSize(View.ViewRect.Size(), VolumetricFogGridPixelSize);

			FVolumetricFogGlobalData GlobalData;
			SetupVolumetricFogGlobalData(View, GlobalData);
			View.VolumetricFogResources.VolumetricFogGlobalData = TUniformBufferRef<FVolumetricFogGlobalData>::CreateUniformBufferImmediate(GlobalData, UniformBuffer_SingleFrame);
		}
	}
	else
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			FViewInfo& View = Views[ViewIndex];

			if (View.ViewState)
			{
				View.ViewState->LightScatteringHistory = NULL;
			}
		}
	}
}

void FDeferredShadingSceneRenderer::ComputeVolumetricFog(FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures)
{
	if (!ShouldRenderVolumetricFog())
	{
		return;
	}

	const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

	TRACE_CPUPROFILER_EVENT_SCOPE(FDeferredShadingSceneRenderer::ComputeVolumetricFog);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_VolumetricFog);
	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, VolumetricFog);
	RDG_GPU_STAT_SCOPE(GraphBuilder, VolumetricFog);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		int32 VolumetricFogGridPixelSize;
		const FIntVector VolumetricFogGridSize = GetVolumetricFogGridSize(View.ViewRect.Size(), VolumetricFogGridPixelSize);
		const FVector GridZParams = GetVolumetricFogGridZParams(View.NearClippingDistance, FogInfo.VolumetricFogDistance, VolumetricFogGridSize.Z);
		const FVector FrameJitterOffsetValue = VolumetricFogTemporalRandom(View.Family->FrameNumber);

		FVolumetricFogIntegrationParameterData IntegrationData;
		IntegrationData.FrameJitterOffsetValues.Empty(16);
		IntegrationData.FrameJitterOffsetValues.AddZeroed(16);
		IntegrationData.FrameJitterOffsetValues[0] = VolumetricFogTemporalRandom(View.Family->FrameNumber);

		for (int32 FrameOffsetIndex = 1; FrameOffsetIndex < GVolumetricFogHistoryMissSupersampleCount; FrameOffsetIndex++)
		{
			IntegrationData.FrameJitterOffsetValues[FrameOffsetIndex] = VolumetricFogTemporalRandom(View.Family->FrameNumber - FrameOffsetIndex);
		}

		const bool bUseTemporalReprojection =
			GVolumetricFogTemporalReprojection
			&& View.ViewState;

		IntegrationData.bTemporalHistoryIsValid =
			bUseTemporalReprojection
			&& !View.bCameraCut
			&& !View.bPrevTransformsReset
			&& ViewFamily.bRealtimeUpdate
			&& View.ViewState->LightScatteringHistory;

		FMatrix DirectionalLightFunctionWorldToShadow;

		RDG_EVENT_SCOPE(GraphBuilder, "VolumetricFog");

#if WITH_MGPU
		static const FName NameForTemporalEffect("ComputeVolumetricFog");
		GraphBuilder.SetNameForTemporalEffect(FName(NameForTemporalEffect, View.ViewState ? View.ViewState->UniqueID : 0));
#endif

		FRDGTextureRef ConservativeDepthTexture;
		// To use a depth target format, and depth tests, we will have to render depth from a PS depth output. Keeping it simple for now with all the tests happening in shader.
		if (GVolumetricFogConservativeDepth > 0)
		{
			FIntPoint ConservativeDepthTextureSize = FIntPoint(VolumetricFogGridSize.X, VolumetricFogGridSize.Y);
			ConservativeDepthTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(ConservativeDepthTextureSize, PF_R16F,
				FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV), TEXT("VolumetricFog.ConservativeDepthTexture"));
			AddGenerateConservativeDepthBufferPass(View, GraphBuilder, ConservativeDepthTexture, GVolumetricFogGridPixelSize);
		}
		else
		{
			ConservativeDepthTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		}

		FRDGTexture* LightFunctionTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.WhiteDummy);
		FRDGTexture* BlackDummyTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		FRDGTexture* VolumetricBlackDummyTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.VolumetricBlackDummy);
		const bool bUseEmissive = GVolumetricFogEmissive > 0;

		// The potential light function for the main directional light is kept separate to be applied during the main VolumetricFogLightScattering pass (as an optimisation).
		FRDGTexture* DirectionalLightFunctionTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.WhiteDummy);
		bool bUseDirectionalLightShadowing = false;

		// Recover the information about the light use as the forward directional light for cloud shadowing
		int AtmosphericDirectionalLightIndex = -1;
		FLightSceneProxy* AtmosphereLightProxy = nullptr;
		if(View.ForwardLightingResources->SelectedForwardDirectionalLightProxy)
		{
			FLightSceneProxy* AtmosphereLight0Proxy = Scene->AtmosphereLights[0] ? Scene->AtmosphereLights[0]->Proxy : nullptr;
			FLightSceneProxy* AtmosphereLight1Proxy = Scene->AtmosphereLights[1] ? Scene->AtmosphereLights[1]->Proxy : nullptr;
			FVolumetricCloudRenderSceneInfo* CloudInfo = Scene->GetVolumetricCloudSceneInfo();
			const bool VolumetricCloudShadowMap0Valid = View.VolumetricCloudShadowExtractedRenderTarget[0] != nullptr;
			const bool VolumetricCloudShadowMap1Valid = View.VolumetricCloudShadowExtractedRenderTarget[1] != nullptr;
			const bool bLight0CloudPerPixelTransmittance = CloudInfo && VolumetricCloudShadowMap0Valid && View.ForwardLightingResources->SelectedForwardDirectionalLightProxy == AtmosphereLight0Proxy && AtmosphereLight0Proxy && AtmosphereLight0Proxy->GetCloudShadowOnSurfaceStrength() > 0.0f;
			const bool bLight1CloudPerPixelTransmittance = CloudInfo && VolumetricCloudShadowMap1Valid && View.ForwardLightingResources->SelectedForwardDirectionalLightProxy == AtmosphereLight1Proxy && AtmosphereLight1Proxy && AtmosphereLight1Proxy->GetCloudShadowOnSurfaceStrength() > 0.0f;
			if (bLight0CloudPerPixelTransmittance)
			{
				AtmosphereLightProxy = AtmosphereLight0Proxy;
				AtmosphericDirectionalLightIndex = 0;
			}
			else if (bLight1CloudPerPixelTransmittance)
			{
				AtmosphereLightProxy = AtmosphereLight1Proxy;
				AtmosphericDirectionalLightIndex = 1;
			}
		}

		RenderLightFunctionForVolumetricFog(
			GraphBuilder,
			View,
			SceneTextures,
			VolumetricFogGridSize,
			FogInfo.VolumetricFogDistance,
			DirectionalLightFunctionWorldToShadow,
			DirectionalLightFunctionTexture,
			bUseDirectionalLightShadowing);
			
		View.VolumetricFogResources.IntegratedLightScatteringTexture = nullptr;
		TRDGUniformBufferRef<FFogUniformParameters> FogUniformBuffer = CreateFogUniformBuffer(GraphBuilder, View);

		FRDGTextureDesc VolumeDesc(FRDGTextureDesc::Create3D(
			VolumetricFogGridSize,
			PF_FloatRGBA, 
			FClearValueBinding::Black, 
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ReduceMemoryWithTilingMode | TexCreate_3DTiling));

		FRDGTextureDesc VolumeDescFastVRAM = VolumeDesc;
		VolumeDescFastVRAM.Flags |= GFastVRamConfig.VolumetricFog;

		IntegrationData.VBufferA = GraphBuilder.CreateTexture(VolumeDescFastVRAM, TEXT("VolumetricFog.VBufferA"));
		IntegrationData.VBufferA_UAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(IntegrationData.VBufferA));
		IntegrationData.VBufferB = nullptr;
		IntegrationData.VBufferB_UAV = nullptr;
		if (bUseEmissive)
		{
			IntegrationData.VBufferB = GraphBuilder.CreateTexture(VolumeDescFastVRAM, TEXT("VolumetricFog.VBufferB"));
			IntegrationData.VBufferB_UAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(IntegrationData.VBufferB));
		}

		FRDGTexture* LocalShadowedLightScattering = GraphBuilder.RegisterExternalTexture(GSystemTextures.VolumetricBlackDummy);
		RenderLocalLightsForVolumetricFog(GraphBuilder, View, bUseTemporalReprojection, IntegrationData, FogInfo, 
			VolumetricFogGridSize, GridZParams, VolumeDescFastVRAM, LocalShadowedLightScattering, ConservativeDepthTexture);

		{
			FVolumetricFogMaterialSetupCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVolumetricFogMaterialSetupCS::FParameters>();
			PassParameters->GlobalAlbedo = FogInfo.VolumetricFogAlbedo;
			PassParameters->GlobalEmissive = FogInfo.VolumetricFogEmissive;
			PassParameters->GlobalExtinctionScale = FogInfo.VolumetricFogExtinctionScale;

			PassParameters->RWVBufferA = IntegrationData.VBufferA_UAV;
			PassParameters->RWVBufferB = IntegrationData.VBufferB_UAV; // FVolumetricFogMaterialSetupCS uses a permutation to not reference that UAV when bUseEmissive is false.

			PassParameters->Fog = FogUniformBuffer; 
			PassParameters->View = View.ViewUniformBuffer;

			FVolumetricFogMaterialSetupCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< FPermutationUseEmissive >(bUseEmissive);
			auto ComputeShader = View.ShaderMap->GetShader< FVolumetricFogMaterialSetupCS >(PermutationVector);
			ClearUnusedGraphResources(ComputeShader, PassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("InitializeVolumeAttributes"),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, &View, VolumetricFogGridSize, IntegrationData, ComputeShader](FRHICommandList& RHICmdList)
			{
				const FIntVector NumGroups = FIntVector::DivideAndRoundUp(VolumetricFogGridSize, VolumetricFogGridInjectionGroupSize);

				RHICmdList.SetComputeShader(ComputeShader.GetComputeShader());

				ComputeShader->SetParameters(RHICmdList, View, IntegrationData);

				SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), *PassParameters);
				DispatchComputeShader(RHICmdList, ComputeShader.GetShader(), NumGroups.X, NumGroups.Y, NumGroups.Z);
				UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
			});

			VoxelizeFogVolumePrimitives(
				GraphBuilder,
				View,
				IntegrationData,
				VolumetricFogGridSize,
				GridZParams,
				FogInfo.VolumetricFogDistance,
				bUseEmissive);
		}

		IntegrationData.LightScattering = GraphBuilder.CreateTexture(VolumeDesc, TEXT("VolumetricFog.LightScattering"), ERDGTextureFlags::MultiFrame);
		IntegrationData.LightScatteringUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(IntegrationData.LightScattering));

		{
			TVolumetricFogLightScatteringCS::FParameters* PassParameters = GraphBuilder.AllocParameters<TVolumetricFogLightScatteringCS::FParameters>();

			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->Fog = FogUniformBuffer;
			PassParameters->VBufferA = IntegrationData.VBufferA;
			PassParameters->VBufferB = IntegrationData.VBufferB ? IntegrationData.VBufferB : VolumetricBlackDummyTexture;
			PassParameters->LocalShadowedLightScattering = LocalShadowedLightScattering;
			PassParameters->LightFunctionTexture = DirectionalLightFunctionTexture;
			PassParameters->ConservativeDepthTexture = ConservativeDepthTexture;
			PassParameters->UseConservativeDepthTexture = GVolumetricFogConservativeDepth > 0 ? 1 : 0;
			PassParameters->UseEmissive = bUseEmissive ? 1 : 0;
			if (GVolumetricFogConservativeDepth > 0 && bUseTemporalReprojection && View.ViewState->PrevLightScatteringConservativeDepthTexture.IsValid())
			{
				PassParameters->PrevConservativeDepthTexture = GraphBuilder.RegisterExternalTexture(View.ViewState->PrevLightScatteringConservativeDepthTexture);
				FIntVector TextureSize = View.ViewState->PrevLightScatteringConservativeDepthTexture->GetDesc().GetSize();
				PassParameters->PrevConservativeDepthTextureSize = FVector2D(TextureSize.X, TextureSize.Y);
			}
			else
			{
				PassParameters->PrevConservativeDepthTexture = BlackDummyTexture;
				PassParameters->PrevConservativeDepthTextureSize = FVector2D(1, 1);
			}

			auto* LumenUniforms = GraphBuilder.AllocParameters<FLumenTranslucencyLightingUniforms>();
			LumenUniforms->Parameters = GetLumenTranslucencyLightingParameters(GraphBuilder, View.LumenTranslucencyGIVolume);
			PassParameters->LumenGIVolumeStruct = GraphBuilder.CreateUniformBuffer(LumenUniforms);
			PassParameters->RWLightScattering = IntegrationData.LightScatteringUAV;
			PassParameters->VirtualShadowMapSamplingParameters = VirtualShadowMapArray.GetSamplingParameters(GraphBuilder);

			const bool bUseLumenGI = View.LumenTranslucencyGIVolume.Texture0 != nullptr;
			const bool bUseGlobalDistanceField = UseGlobalDistanceField() && Scene->DistanceFieldSceneData.NumObjectsInBuffer > 0;

			const bool bUseDistanceFieldSkyOcclusion =
				ViewFamily.EngineShowFlags.AmbientOcclusion
				&& !bUseLumenGI
				&& Scene->SkyLight
				&& Scene->SkyLight->bCastShadows
				&& Scene->SkyLight->bCastVolumetricShadow
				&& ShouldRenderDistanceFieldAO()
				&& SupportsDistanceFieldAO(View.GetFeatureLevel(), View.GetShaderPlatform())
				&& bUseGlobalDistanceField
				&& Views.Num() == 1
				&& View.IsPerspectiveProjection();

			TVolumetricFogLightScatteringCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FTemporalReprojection >(bUseTemporalReprojection);
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FDistanceFieldSkyOcclusion >(bUseDistanceFieldSkyOcclusion);
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FSuperSampleCount >(GVolumetricFogHistoryMissSupersampleCount);
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FLumenGI >(bUseLumenGI);
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FVirtualShadowMap >( VirtualShadowMapArray.IsAllocated() );
			PermutationVector.Set< TVolumetricFogLightScatteringCS::FCloudTransmittance >(AtmosphericDirectionalLightIndex >= 0);

			auto ComputeShader = View.ShaderMap->GetShader< TVolumetricFogLightScatteringCS >(PermutationVector);
			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FRHITexture* LightScatteringHistoryTexture = GBlackVolumeTexture->TextureRHI;
			if (bUseTemporalReprojection && View.ViewState->LightScatteringHistory.IsValid())
			{
				LightScatteringHistoryTexture = View.ViewState->LightScatteringHistory->GetRenderTargetItem().ShaderResourceTexture;
			}

			FVolumetricCloudRenderSceneInfo* CloudInfo = Scene->GetVolumetricCloudSceneInfo();

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("LightScattering %dx%dx%d SS:%d %s %s %s",
					VolumetricFogGridSize.X,
					VolumetricFogGridSize.Y,
					VolumetricFogGridSize.Z,
					GVolumetricFogHistoryMissSupersampleCount,
					bUseDistanceFieldSkyOcclusion ? TEXT("DFAO") : TEXT(""),
					PassParameters->LightFunctionTexture ? TEXT("LF") : TEXT(""),
					bUseLumenGI ? TEXT("Lumen") : TEXT("")),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, ComputeShader, &View, this, FogInfo, bUseTemporalReprojection, LightScatteringHistoryTexture, CloudInfo, VolumetricFogGridSize, IntegrationData, bUseDirectionalLightShadowing, bUseDistanceFieldSkyOcclusion, DirectionalLightFunctionWorldToShadow, AtmosphericDirectionalLightIndex, AtmosphereLightProxy](FRHICommandList& RHICmdList)
			{
				const FIntVector NumGroups = FComputeShaderUtils::GetGroupCount(VolumetricFogGridSize, TVolumetricFogLightScatteringCS::GetGroupSize());

				RHICmdList.SetComputeShader(ComputeShader.GetComputeShader());

				ComputeShader->SetParameters(RHICmdList, View, IntegrationData, FogInfo, LightScatteringHistoryTexture, bUseDirectionalLightShadowing, DirectionalLightFunctionWorldToShadow, AtmosphericDirectionalLightIndex, AtmosphereLightProxy, CloudInfo);

				SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), *PassParameters);
				DispatchComputeShader(RHICmdList, ComputeShader.GetShader(), NumGroups.X, NumGroups.Y, NumGroups.Z);
				UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
			});
		}

		FRDGTexture* IntegratedLightScattering = GraphBuilder.CreateTexture(VolumeDesc, TEXT("VolumetricFog.IntegratedLightScattering"));
		FRDGTextureUAV* IntegratedLightScatteringUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(IntegratedLightScattering));

		{
			FVolumetricFogFinalIntegrationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVolumetricFogFinalIntegrationCS::FParameters>();
			PassParameters->LightScattering = IntegrationData.LightScattering;
			PassParameters->RWIntegratedLightScattering = IntegratedLightScatteringUAV;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("FinalIntegration"),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, &View, VolumetricFogGridSize, IntegrationData, this](FRHICommandList& RHICmdList)
			{
				const FIntVector NumGroups = FIntVector::DivideAndRoundUp(VolumetricFogGridSize, VolumetricFogIntegrationGroupSize);

				auto ComputeShader = View.ShaderMap->GetShader< FVolumetricFogFinalIntegrationCS >();
				RHICmdList.SetComputeShader(ComputeShader.GetComputeShader());
				ComputeShader->SetParameters(RHICmdList, View, IntegrationData);

				SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), *PassParameters);
				DispatchComputeShader(RHICmdList, ComputeShader.GetShader(), NumGroups.X, NumGroups.Y, 1);
				UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
			});
		}

		View.VolumetricFogResources.IntegratedLightScatteringTexture = IntegratedLightScattering;

		if (bUseTemporalReprojection)
		{
			GraphBuilder.QueueTextureExtraction(IntegrationData.LightScattering, &View.ViewState->LightScatteringHistory);
		}
		else if (View.ViewState)
		{
			View.ViewState->LightScatteringHistory = nullptr;
		}

		if (bUseTemporalReprojection && GVolumetricFogConservativeDepth > 0)
		{
			GraphBuilder.QueueTextureExtraction(ConservativeDepthTexture, &View.ViewState->PrevLightScatteringConservativeDepthTexture);
		}
		else if (View.ViewState)
		{
			View.ViewState->PrevLightScatteringConservativeDepthTexture = NULL;
		}
	}
}