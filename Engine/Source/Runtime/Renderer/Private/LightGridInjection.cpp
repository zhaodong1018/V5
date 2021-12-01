// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
LightGridInjection.cpp
=============================================================================*/

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "UniformBuffer.h"
#include "ShaderParameters.h"
#include "RendererInterface.h"
#include "EngineDefines.h"
#include "PrimitiveSceneProxy.h"
#include "Shader.h"
#include "SceneUtils.h"
#include "PostProcess/SceneRenderTargets.h"
#include "LightSceneInfo.h"
#include "GlobalShader.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "BasePassRendering.h"
#include "RendererModule.h"
#include "ScenePrivate.h"
#include "ClearQuad.h"
#include "VolumetricFog.h"
#include "VolumetricCloudRendering.h"
#include "Components/LightComponent.h"
#include "Engine/MapBuildDataRegistry.h"

// Workaround for platforms that don't support implicit conversion from 16bit integers on the CPU to uint32 in the shader
#define	CHANGE_LIGHTINDEXTYPE_SIZE	(PLATFORM_MAC || PLATFORM_IOS) 

int32 GLightGridPixelSize = 64;
FAutoConsoleVariableRef CVarLightGridPixelSize(
	TEXT("r.Forward.LightGridPixelSize"),
	GLightGridPixelSize,
	TEXT("Size of a cell in the light grid, in pixels."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLightGridSizeZ = 32;
FAutoConsoleVariableRef CVarLightGridSizeZ(
	TEXT("r.Forward.LightGridSizeZ"),
	GLightGridSizeZ,
	TEXT("Number of Z slices in the light grid."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GMaxCulledLightsPerCell = 32;
FAutoConsoleVariableRef CVarMaxCulledLightsPerCell(
	TEXT("r.Forward.MaxCulledLightsPerCell"),
	GMaxCulledLightsPerCell,
	TEXT("Controls how much memory is allocated for each cell for light culling.  When r.Forward.LightLinkedListCulling is enabled, this is used to compute a global max instead of a per-cell limit on culled lights."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLightLinkedListCulling = 1;
FAutoConsoleVariableRef CVarLightLinkedListCulling(
	TEXT("r.Forward.LightLinkedListCulling"),
	GLightLinkedListCulling,
	TEXT("Uses a reverse linked list to store culled lights, removing the fixed limit on how many lights can affect a cell - it becomes a global limit instead."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLightCullingQuality = 1;
FAutoConsoleVariableRef CVarLightCullingQuality(
	TEXT("r.LightCulling.Quality"),
	GLightCullingQuality,
	TEXT("Whether to run compute light culling pass.\n")
	TEXT(" 0: off \n")
	TEXT(" 1: on (default)\n"),
	ECVF_RenderThreadSafe
);

float GLightCullingMaxDistanceOverrideKilometers = -1.0f;
FAutoConsoleVariableRef CVarLightCullingMaxDistanceOverride(
	TEXT("r.LightCulling.MaxDistanceOverrideKilometers"),
	GLightCullingMaxDistanceOverrideKilometers,
	TEXT("Used to override the maximum far distance at which we can store data in the light grid.\n If this is increase, you might want to update r.Forward.LightGridSizeZ to a reasonable value according to your use case light count and distribution.")
	TEXT(" <=0: off \n")
	TEXT(" >0: the far distance in kilometers.\n"),
	ECVF_RenderThreadSafe
);

extern TAutoConsoleVariable<int32> CVarVirtualShadowOnePassProjection;

/** A minimal forwarding lighting setup. */
class FMinimalDummyForwardLightingResources : public FRenderResource
{
public:
	FForwardLightingViewResources ForwardLightingResources;

	/** Destructor. */
	virtual ~FMinimalDummyForwardLightingResources()
	{}

	virtual void InitRHI()
	{
		if (GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5)
		{
			ForwardLightingResources.ForwardLocalLightBuffer.Initialize(TEXT("ForwardLocalLightBuffer"), sizeof(FVector4f), sizeof(FForwardLocalLightData) / sizeof(FVector4f), PF_A32B32G32R32F, BUF_Dynamic);
			ForwardLightingResources.NumCulledLightsGrid.Initialize(TEXT("NumCulledLightsGrid"), sizeof(uint32), 1, PF_R32_UINT);

			const bool bSupportFormatConversion = RHISupportsBufferLoadTypeConversion(GMaxRHIShaderPlatform);

			if (bSupportFormatConversion)
			{
				ForwardLightingResources.CulledLightDataGrid.Initialize(TEXT("CulledLightDataGrid"), sizeof(uint16), 1, PF_R16_UINT);
			}
			else
			{
				ForwardLightingResources.CulledLightDataGrid.Initialize(TEXT("CulledLightDataGrid"), sizeof(uint32), 1, PF_R32_UINT);
			}

			ForwardLightingResources.ForwardLightData.ForwardLocalLightBuffer = ForwardLightingResources.ForwardLocalLightBuffer.SRV;
			ForwardLightingResources.ForwardLightData.NumCulledLightsGrid = ForwardLightingResources.NumCulledLightsGrid.SRV;
			ForwardLightingResources.ForwardLightData.CulledLightDataGrid = ForwardLightingResources.CulledLightDataGrid.SRV;

			ForwardLightingResources.ForwardLightDataUniformBuffer = TUniformBufferRef<FForwardLightData>::CreateUniformBufferImmediate(ForwardLightingResources.ForwardLightData, UniformBuffer_MultiFrame);
		}
	}

	virtual void ReleaseRHI()
	{
		ForwardLightingResources.Release();
	}
};

FForwardLightingViewResources* GetMinimalDummyForwardLightingResources()
{
	static TGlobalResource<FMinimalDummyForwardLightingResources>* GMinimalDummyForwardLightingResources = nullptr;

	if (!GMinimalDummyForwardLightingResources)
	{
		GMinimalDummyForwardLightingResources = new TGlobalResource<FMinimalDummyForwardLightingResources>();
	}

	return &GMinimalDummyForwardLightingResources->ForwardLightingResources;
}

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FForwardLightData, "ForwardLightData");

FForwardLightData::FForwardLightData()
{
	FMemory::Memzero(*this);
	DirectionalLightShadowmapAtlas = GBlackTexture->TextureRHI;
	ShadowmapSampler = TStaticSamplerState<SF_Point,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
	DirectionalLightStaticShadowmap = GBlackTexture->TextureRHI;
	StaticShadowmapSampler = TStaticSamplerState<SF_Bilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();

	DummyRectLightSourceTexture = GWhiteTexture->TextureRHI;

	ForwardLocalLightBuffer = nullptr;
	NumCulledLightsGrid = nullptr;
	CulledLightDataGrid = nullptr;
}

int32 NumCulledLightsGridStride = 2;
int32 NumCulledGridPrimitiveTypes = 2;
int32 LightLinkStride = 2;

// 65k indexable light limit
typedef uint16 FLightIndexType;
// UINT_MAX indexable light limit
typedef uint32 FLightIndexType32;

uint32 LightGridInjectionGroupSize = 4;


class FLightGridInjectionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLightGridInjectionCS);
	SHADER_USE_PARAMETER_STRUCT(FLightGridInjectionCS, FGlobalShader)
public:
	class FUseLinkedListDim : SHADER_PERMUTATION_BOOL("USE_LINKED_CULL_LIST");
	using FPermutationDomain = TShaderPermutationDomain<FUseLinkedListDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		//SHADER_PARAMETER_STRUCT_INCLUDE(FLightGridInjectionCommonParameters, CommonParameters)
		SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureShaderData, ReflectionCapture)
		SHADER_PARAMETER_STRUCT_REF(FForwardLightData, Forward)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, RWNumCulledLightsGrid)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, RWCulledLightDataGrid)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNextCulledLightLink)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWStartOffsetGrid)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCulledLightLinks)
		SHADER_PARAMETER_SRV(Buffer<float4>, LightViewSpacePositionAndRadius)
		SHADER_PARAMETER_SRV(Buffer<float4>, LightViewSpaceDirAndPreprocAngle)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) || IsMobileDeferredShadingEnabled(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), LightGridInjectionGroupSize);
		FForwardLightingParameters::ModifyCompilationEnvironment(Parameters.Platform, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("LIGHT_LINK_STRIDE"), LightLinkStride);
		OutEnvironment.SetDefine(TEXT("ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA"), ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLightGridInjectionCS, "/Engine/Private/LightGridInjection.usf", "LightGridInjectionCS", SF_Compute);


class FLightGridCompactCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLightGridCompactCS)
	SHADER_USE_PARAMETER_STRUCT(FLightGridCompactCS, FGlobalShader)
public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FForwardLightData, Forward)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, RWNumCulledLightsGrid)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, RWCulledLightDataGrid)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNextCulledLightData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StartOffsetGrid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CulledLightLinks)

	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) || IsMobileDeferredShadingEnabled(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), LightGridInjectionGroupSize);
		FForwardLightingParameters::ModifyCompilationEnvironment(Parameters.Platform, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("LIGHT_LINK_STRIDE"), LightLinkStride);
		OutEnvironment.SetDefine(TEXT("MAX_CAPTURES"), GMaxNumReflectionCaptures);
		OutEnvironment.SetDefine(TEXT("ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA"), ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLightGridCompactCS, "/Engine/Private/LightGridInjection.usf", "LightGridCompactCS", SF_Compute);

/**
 */
FORCEINLINE float GetTanRadAngleOrZero(float coneAngle)
{
	if (coneAngle < PI / 2.001f)
	{
		return FMath::Tan(coneAngle);
	}

	return 0.0f;
}


FVector GetLightGridZParams(float NearPlane, float FarPlane)
{
	// S = distribution scale
	// B, O are solved for given the z distances of the first+last slice, and the # of slices.
	//
	// slice = log2(z*B + O) * S

	// Don't spend lots of resolution right in front of the near plane
	double NearOffset = .095 * 100;
	// Space out the slices so they aren't all clustered at the near plane
	double S = 4.05;

	double N = NearPlane + NearOffset;
	double F = FarPlane;

	double O = (F - N * exp2((GLightGridSizeZ - 1) / S)) / (F - N);
	double B = (1 - O) / N;

	return FVector(B, O, S);
}

// TODO: Might already exist as utility somewhere
template <typename T>
void UpdateDynamicVector4BufferData(const TArray<T, SceneRenderingAllocator> &DataArray, FDynamicReadBuffer &Buffer, const TCHAR* DebugName)
{
	const uint32 NumBytesRequired = DataArray.Num() * DataArray.GetTypeSize();

	if (Buffer.NumBytes < NumBytesRequired)
	{
		Buffer.Release();
		Buffer.Initialize(DebugName, sizeof(FVector4f), NumBytesRequired / sizeof(FVector4f), PF_A32B32G32R32F, BUF_Volatile);
	}

	Buffer.Lock();
	FPlatformMemory::Memcpy(Buffer.MappedBuffer, DataArray.GetData(), DataArray.Num() * DataArray.GetTypeSize());
	Buffer.Unlock();
}


void FSceneRenderer::ComputeLightGrid(FRDGBuilder& GraphBuilder, bool bCullLightsToGrid, FSortedLightSetSceneInfo &SortedLightSet)
{
	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, ComputeLightGrid);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_ComputeLightGrid);
	RDG_EVENT_SCOPE(GraphBuilder, "ComputeLightGrid");

	static const auto AllowStaticLightingVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowStaticLighting"));
	const bool bAllowStaticLighting = (!AllowStaticLightingVar || AllowStaticLightingVar->GetValueOnRenderThread() != 0);
	const bool bAllowFormatConversion = RHISupportsBufferLoadTypeConversion(GMaxRHIShaderPlatform);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];
		FForwardLightData& ForwardLightData = View.ForwardLightingResources->ForwardLightData;
		ForwardLightData = FForwardLightData();
		View.ForwardLightingResources->SelectedForwardDirectionalLightProxy = nullptr;

		TArray<FForwardLocalLightData, SceneRenderingAllocator> ForwardLocalLightData;
		TArray<int32, SceneRenderingAllocator>  LocalLightVisibleLightInfosIndex;
#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
		// TODO: In the future these should be generated by CPU-side render graph passes that fill the GPU buffers only when actually needed.
		// TODO: Should use 'Empty' to pre-allocate enough space for all lights?
		TArray<FVector4f, SceneRenderingAllocator> ViewSpacePosAndRadiusData;
		TArray<FVector4f, SceneRenderingAllocator> ViewSpaceDirAndPreprocAngleData;
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
		float FurthestLight = 1000;

		// Track the end markers for different types
		int32 SimpleLightsEnd = 0;
		int32 ClusteredSupportedEnd = 0;

		if (bCullLightsToGrid)
		{
			// Simple lights are copied without view dependent checks, so same in and out
			SimpleLightsEnd = SortedLightSet.SimpleLightsEnd;
			// 1. insert simple lights
			if (SimpleLightsEnd > 0)
			{
				const FSimpleLightArray &SimpleLights = SortedLightSet.SimpleLights;

				// Pack both values into a single float to keep float4 alignment
				const FFloat16 SimpleLightSourceLength16f = FFloat16(0);
				FLightingChannels SimpleLightLightingChannels;
				// Put simple lights in all lighting channels
				SimpleLightLightingChannels.bChannel0 = SimpleLightLightingChannels.bChannel1 = SimpleLightLightingChannels.bChannel2 = true;
				const uint32 SimpleLightLightingChannelMask = GetLightingChannelMaskForStruct(SimpleLightLightingChannels);

				// Now using the sorted lights, and keep track of ranges as we go.
				for (int SortedIndex = 0; SortedIndex < SortedLightSet.SimpleLightsEnd; ++SortedIndex)
				{
					check(SortedLightSet.SortedLights[SortedIndex].LightSceneInfo == nullptr);
					check(!SortedLightSet.SortedLights[SortedIndex].SortKey.Fields.bIsNotSimpleLight);

					int32 SimpleLightIndex = SortedLightSet.SortedLights[SortedIndex].SimpleLightIndex;

					ForwardLocalLightData.AddUninitialized(1);
					FForwardLocalLightData& LightData = ForwardLocalLightData.Last();
					// Simple lights have no 'VisibleLight' info
					LocalLightVisibleLightInfosIndex.Add(INDEX_NONE);

					const FSimpleLightEntry& SimpleLight = SimpleLights.InstanceData[SimpleLightIndex];
					const FSimpleLightPerViewEntry& SimpleLightPerViewData = SimpleLights.GetViewDependentData(SimpleLightIndex, ViewIndex, Views.Num());
					LightData.LightPositionAndInvRadius = FVector4f(SimpleLightPerViewData.Position, 1.0f / FMath::Max(SimpleLight.Radius, KINDA_SMALL_NUMBER));
					LightData.LightColorAndFalloffExponent = FVector4f(SimpleLight.Color, SimpleLight.Exponent);

					// No shadowmap channels for simple lights
					uint32 ShadowMapChannelMask = 0;
					ShadowMapChannelMask |= SimpleLightLightingChannelMask << 8;

					LightData.LightDirectionAndShadowMapChannelMask = FVector4f(FVector(1, 0, 0), *((float*)&ShadowMapChannelMask));

					// Pack both values into a single float to keep float4 alignment
					const FFloat16 VolumetricScatteringIntensity16f = FFloat16(SimpleLight.VolumetricScatteringIntensity);
					const uint32 PackedWInt = ((uint32)SimpleLightSourceLength16f.Encoded) | ((uint32)VolumetricScatteringIntensity16f.Encoded << 16);

					LightData.SpotAnglesAndSourceRadiusPacked = FVector4f(-2, 1, 0, *(float*)&PackedWInt);
					LightData.LightTangentAndSoftSourceRadius = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
					LightData.RectBarnDoor = FVector4f(0, -2, 0, 0);

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
					FVector4f ViewSpacePosAndRadius(FVector(View.ViewMatrices.GetViewMatrix().TransformPosition(SimpleLightPerViewData.Position)), SimpleLight.Radius);
					ViewSpacePosAndRadiusData.Add(ViewSpacePosAndRadius);
					ViewSpaceDirAndPreprocAngleData.AddZeroed();
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
				}
			}

			float SelectedForwardDirectionalLightIntensitySq = 0.0f;
			const TArray<FSortedLightSceneInfo, SceneRenderingAllocator>& SortedLights = SortedLightSet.SortedLights;
			ClusteredSupportedEnd = SimpleLightsEnd;
			// Next add all the other lights, track the end index for clustered supporting lights
			for (int SortedIndex = SimpleLightsEnd; SortedIndex < SortedLights.Num(); ++SortedIndex)
			{
				const FSortedLightSceneInfo& SortedLightInfo = SortedLights[SortedIndex];
				const FLightSceneInfo* const LightSceneInfo = SortedLightInfo.LightSceneInfo;
				const FLightSceneProxy* LightProxy = LightSceneInfo->Proxy;

				if (LightSceneInfo->ShouldRenderLight(View))
				{
					FLightShaderParameters LightParameters;
					LightProxy->GetLightShaderParameters(LightParameters);

					if (LightProxy->IsInverseSquared())
					{
						LightParameters.FalloffExponent = 0;
					}

					// When rendering reflection captures, the direct lighting of the light is actually the indirect specular from the main view
					if (View.bIsReflectionCapture)
					{
						LightParameters.Color *= LightProxy->GetIndirectLightingScale();
					}

					int32 ShadowMapChannel = LightProxy->GetShadowMapChannel();
					int32 DynamicShadowMapChannel = LightSceneInfo->GetDynamicShadowMapChannel();

					if (!bAllowStaticLighting)
					{
						ShadowMapChannel = INDEX_NONE;
					}

					// Static shadowing uses ShadowMapChannel, dynamic shadows are packed into light attenuation using DynamicShadowMapChannel
					uint32 LightTypeAndShadowMapChannelMaskPacked =
						(ShadowMapChannel == 0 ? 1 : 0) |
						(ShadowMapChannel == 1 ? 2 : 0) |
						(ShadowMapChannel == 2 ? 4 : 0) |
						(ShadowMapChannel == 3 ? 8 : 0) |
						(DynamicShadowMapChannel == 0 ? 16 : 0) |
						(DynamicShadowMapChannel == 1 ? 32 : 0) |
						(DynamicShadowMapChannel == 2 ? 64 : 0) |
						(DynamicShadowMapChannel == 3 ? 128 : 0);

					LightTypeAndShadowMapChannelMaskPacked |= LightProxy->GetLightingChannelMask() << 8;
					// pack light type in this uint32 as well
					LightTypeAndShadowMapChannelMaskPacked |= SortedLightInfo.SortKey.Fields.LightType << 16;

					const bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows && VisibleLightInfos.IsValidIndex(LightSceneInfo->Id);
					const int32 VirtualShadowMapId = bDynamicShadows ? VisibleLightInfos[LightSceneInfo->Id].GetVirtualShadowMapId( &View ) : INDEX_NONE;

					if ((SortedLightInfo.SortKey.Fields.LightType == LightType_Point && ViewFamily.EngineShowFlags.PointLights) ||
						(SortedLightInfo.SortKey.Fields.LightType == LightType_Spot && ViewFamily.EngineShowFlags.SpotLights) ||
						(SortedLightInfo.SortKey.Fields.LightType == LightType_Rect && ViewFamily.EngineShowFlags.RectLights))
					{
						ForwardLocalLightData.AddUninitialized(1);
						FForwardLocalLightData& LightData = ForwardLocalLightData.Last();
						LocalLightVisibleLightInfosIndex.Add(LightSceneInfo->Id);

						// Track the last one to support clustered deferred
						if (!SortedLightInfo.SortKey.Fields.bClusteredDeferredNotSupported)
						{
							ClusteredSupportedEnd = FMath::Max(ClusteredSupportedEnd, ForwardLocalLightData.Num());
						}
						const float LightFade = GetLightFadeFactor(View, LightProxy);
						LightParameters.Color *= LightFade;

						LightData.LightPositionAndInvRadius = FVector4f(LightParameters.Position, LightParameters.InvRadius);
						LightData.LightColorAndFalloffExponent = FVector4f(LightParameters.Color, LightParameters.FalloffExponent);
						LightData.LightDirectionAndShadowMapChannelMask = FVector4f(LightParameters.Direction, *((float*)&LightTypeAndShadowMapChannelMaskPacked));

						LightData.SpotAnglesAndSourceRadiusPacked = FVector4f(LightParameters.SpotAngles.X, LightParameters.SpotAngles.Y, LightParameters.SourceRadius, 0);

						LightData.LightTangentAndSoftSourceRadius = FVector4f(LightParameters.Tangent, LightParameters.SoftSourceRadius);

						LightData.RectBarnDoor = FVector4f(LightParameters.RectLightBarnCosAngle, LightParameters.RectLightBarnLength, *((float*)&VirtualShadowMapId), 0);

						float VolumetricScatteringIntensity = LightProxy->GetVolumetricScatteringIntensity();

						if (LightNeedsSeparateInjectionIntoVolumetricFogForOpaqueShadow(View, LightSceneInfo, VisibleLightInfos[LightSceneInfo->Id]) 
							|| (LightNeedsSeparateInjectionIntoVolumetricFogForLightFunction(LightSceneInfo) && CheckForLightFunction(LightSceneInfo)))
						{
							// Disable this lights forward shading volumetric scattering contribution
							VolumetricScatteringIntensity = 0;
						}

						// Pack both values into a single float to keep float4 alignment
						const FFloat16 SourceLength16f = FFloat16(LightParameters.SourceLength);
						const FFloat16 VolumetricScatteringIntensity16f = FFloat16(VolumetricScatteringIntensity);
						const uint32 PackedWInt = ((uint32)SourceLength16f.Encoded) | ((uint32)VolumetricScatteringIntensity16f.Encoded << 16);
						LightData.SpotAnglesAndSourceRadiusPacked.W = *(float*)&PackedWInt;

						const FSphere BoundingSphere = LightProxy->GetBoundingSphere();
						const float Distance = View.ViewMatrices.GetViewMatrix().TransformPosition(BoundingSphere.Center).Z + BoundingSphere.W;
						FurthestLight = FMath::Max(FurthestLight, Distance);

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
						// Note: inverting radius twice seems stupid (but done in shader anyway otherwise)
						// LWC_TODO: precision loss
						FVector4f ViewSpacePosAndRadius(FVector3f(View.ViewMatrices.GetViewMatrix().TransformPosition(LightParameters.Position)), 1.0f / LightParameters.InvRadius);
						ViewSpacePosAndRadiusData.Add(ViewSpacePosAndRadius);

						float PreProcAngle = SortedLightInfo.SortKey.Fields.LightType == LightType_Spot ? GetTanRadAngleOrZero(LightSceneInfo->Proxy->GetOuterConeAngle()) : 0.0f;

						FVector4f ViewSpaceDirAndPreprocAngle(View.ViewMatrices.GetViewMatrix().TransformVector(LightParameters.Direction), PreProcAngle);
						ViewSpaceDirAndPreprocAngleData.Add(ViewSpaceDirAndPreprocAngle);
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
					}
					else if (SortedLightInfo.SortKey.Fields.LightType == LightType_Directional && ViewFamily.EngineShowFlags.DirectionalLights)
					{
						// The selected forward directional light is also used for volumetric lighting using ForwardLightData UB.
						// Also some people noticed that depending on the order a two directional lights are made visible in a level, the selected light for volumetric fog lighting will be different.
						// So to be clear and avoid such issue, we select the most intense directional light for forward shading and volumetric lighting.
						const float LightIntensitySq = LightParameters.Color.SizeSquared();
						if (LightIntensitySq > SelectedForwardDirectionalLightIntensitySq)
						{
							SelectedForwardDirectionalLightIntensitySq = LightIntensitySq;
							View.ForwardLightingResources->SelectedForwardDirectionalLightProxy = LightProxy;

							ForwardLightData.HasDirectionalLight = 1;
							ForwardLightData.DirectionalLightColor = LightParameters.Color;
							ForwardLightData.DirectionalLightVolumetricScatteringIntensity = LightProxy->GetVolumetricScatteringIntensity();
							ForwardLightData.DirectionalLightDirection = LightParameters.Direction;
							ForwardLightData.DirectionalLightShadowMapChannelMask = LightTypeAndShadowMapChannelMaskPacked;
							ForwardLightData.DirectionalLightVSM = INDEX_NONE;

							const FVector2D FadeParams = LightProxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), LightSceneInfo->IsPrecomputedLightingValid(), View.MaxShadowCascades);

							ForwardLightData.DirectionalLightDistanceFadeMAD = FVector2D(FadeParams.Y, -FadeParams.X * FadeParams.Y);

							if (bDynamicShadows)
							{
								const TArray<FProjectedShadowInfo*, SceneRenderingAllocator>& DirectionalLightShadowInfos = VisibleLightInfos[LightSceneInfo->Id].AllProjectedShadows;

								ForwardLightData.DirectionalLightVSM = VirtualShadowMapId;

								ForwardLightData.NumDirectionalLightCascades = 0;
								// Unused cascades should compare > all scene depths
								ForwardLightData.CascadeEndDepths = FVector4f(MAX_FLT, MAX_FLT, MAX_FLT, MAX_FLT);

								for (const FProjectedShadowInfo* ShadowInfo : DirectionalLightShadowInfos)
								{

									if (ShadowInfo->DependentView && ShadowInfo->DependentView != &View)
									{
										continue;
									}

									const int32 CascadeIndex = ShadowInfo->CascadeSettings.ShadowSplitIndex;

									if (ShadowInfo->IsWholeSceneDirectionalShadow() && !ShadowInfo->HasVirtualShadowMap() && ShadowInfo->bAllocated && CascadeIndex < GMaxForwardShadowCascades)
									{
										ForwardLightData.NumDirectionalLightCascades++;
										ForwardLightData.DirectionalLightWorldToShadowMatrix[CascadeIndex] = ShadowInfo->GetWorldToShadowMatrix(ForwardLightData.DirectionalLightShadowmapMinMax[CascadeIndex]);
										ForwardLightData.CascadeEndDepths[CascadeIndex] = ShadowInfo->CascadeSettings.SplitFar;

										if (CascadeIndex == 0)
										{
											ForwardLightData.DirectionalLightShadowmapAtlas = ShadowInfo->RenderTargets.DepthTarget->GetRenderTargetItem().ShaderResourceTexture.GetReference();
											ForwardLightData.DirectionalLightDepthBias = ShadowInfo->GetShaderDepthBias();
											FVector2D AtlasSize = ShadowInfo->RenderTargets.DepthTarget->GetDesc().Extent;
											ForwardLightData.DirectionalLightShadowmapAtlasBufferSize = FVector4f(AtlasSize.X, AtlasSize.Y, 1.0f / AtlasSize.X, 1.0f / AtlasSize.Y);
										}
									}
								}
							}

							const FStaticShadowDepthMap* StaticShadowDepthMap = LightSceneInfo->Proxy->GetStaticShadowDepthMap();
							const uint32 bStaticallyShadowedValue = LightSceneInfo->IsPrecomputedLightingValid() && StaticShadowDepthMap && StaticShadowDepthMap->Data && StaticShadowDepthMap->TextureRHI ? 1 : 0;

							ForwardLightData.DirectionalLightUseStaticShadowing = bStaticallyShadowedValue;
							ForwardLightData.DirectionalLightStaticShadowBufferSize = bStaticallyShadowedValue ? FVector4f(StaticShadowDepthMap->Data->ShadowMapSizeX, StaticShadowDepthMap->Data->ShadowMapSizeY, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeX, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeY) : FVector4f(0, 0, 0, 0);
							ForwardLightData.DirectionalLightWorldToStaticShadow = bStaticallyShadowedValue ? StaticShadowDepthMap->Data->WorldToLight : FMatrix::Identity;
							ForwardLightData.DirectionalLightStaticShadowmap = bStaticallyShadowedValue ? StaticShadowDepthMap->TextureRHI : GWhiteTexture->TextureRHI;
						}
					}
				}
			}
		}


		// Store off the number of lights before we add a fake entry
		const int32 NumLocalLightsFinal = ForwardLocalLightData.Num();

		if (ForwardLocalLightData.Num() == 0)
		{
			// Make sure the buffer gets created even though we're not going to read from it in the shader, for platforms like PS4 that assert on null resources being bound
			ForwardLocalLightData.AddZeroed();
		}

		UpdateDynamicVector4BufferData(ForwardLocalLightData, View.ForwardLightingResources->ForwardLocalLightBuffer, TEXT("ForwardLocalLightBuffer"));
		View.ForwardLightingResources->LocalLightVisibleLightInfosIndex = LocalLightVisibleLightInfosIndex;

		const FIntPoint LightGridSizeXY = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), GLightGridPixelSize);
		ForwardLightData.ForwardLocalLightBuffer = View.ForwardLightingResources->ForwardLocalLightBuffer.SRV;
		ForwardLightData.NumLocalLights = NumLocalLightsFinal;
		ForwardLightData.NumReflectionCaptures = View.NumBoxReflectionCaptures + View.NumSphereReflectionCaptures;
		ForwardLightData.NumGridCells = LightGridSizeXY.X * LightGridSizeXY.Y * GLightGridSizeZ;
		ForwardLightData.CulledGridSize = FIntVector(LightGridSizeXY.X, LightGridSizeXY.Y, GLightGridSizeZ);
		ForwardLightData.MaxCulledLightsPerCell = GMaxCulledLightsPerCell;
		ForwardLightData.LightGridPixelSizeShift = FMath::FloorLog2(GLightGridPixelSize);
		ForwardLightData.SimpleLightsEndIndex = SimpleLightsEnd;
		ForwardLightData.ClusteredDeferredSupportedEndIndex = ClusteredSupportedEnd;
		ForwardLightData.DirectLightingShowFlag = ViewFamily.EngineShowFlags.DirectLighting ? 1 : 0;

		// Clamp far plane to something reasonable
		const float KilometersToCentimeters = 100000.0f;
		const float LightCullingMaxDistance = GLightCullingMaxDistanceOverrideKilometers <= 0.0f ? (float)HALF_WORLD_MAX / 5.0f : GLightCullingMaxDistanceOverrideKilometers * KilometersToCentimeters;
		float FarPlane = FMath::Min(FMath::Max(FurthestLight, View.FurthestReflectionCaptureDistance), LightCullingMaxDistance);
		FVector ZParams = GetLightGridZParams(View.NearClippingDistance, FarPlane + 10.f);
		ForwardLightData.LightGridZParams = ZParams;

		const uint64 NumIndexableLights = CHANGE_LIGHTINDEXTYPE_SIZE && !bAllowFormatConversion ? (1llu << (sizeof(FLightIndexType32) * 8llu)) : (1llu << (sizeof(FLightIndexType) * 8llu));

		if ((uint64)ForwardLocalLightData.Num() > NumIndexableLights)
		{
			static bool bWarned = false;

			if (!bWarned)
			{
				UE_LOG(LogRenderer, Warning, TEXT("Exceeded indexable light count, glitches will be visible (%u / %llu)"), ForwardLocalLightData.Num(), NumIndexableLights);
				bWarned = true;
			}
		}

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
		const SIZE_T LightIndexTypeSize = CHANGE_LIGHTINDEXTYPE_SIZE && !bAllowFormatConversion ? sizeof(FLightIndexType32) : sizeof(FLightIndexType);
		// Fuse these loops as I see no reason why not and we build some temporary data that is needed in the build pass and is 
		// not needed to be stored permanently.
#else // !ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
	}

	const SIZE_T LightIndexTypeSize = CHANGE_LIGHTINDEXTYPE_SIZE && !bAllowFormatConversion ? sizeof(FLightIndexType32) : sizeof(FLightIndexType);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];
		FForwardLightData& ForwardLightData = View.ForwardLightingResources->ForwardLightData;
		const FIntPoint LightGridSizeXY = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), GLightGridPixelSize);

#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA

		// Allocate buffers using the scene render targets size so we won't reallocate every frame with dynamic resolution
		const FIntPoint MaxLightGridSizeXY = FIntPoint::DivideAndRoundUp(GetSceneTextureExtent(), GLightGridPixelSize);

		const int32 MaxNumCells = MaxLightGridSizeXY.X * MaxLightGridSizeXY.Y * GLightGridSizeZ * NumCulledGridPrimitiveTypes;

		if (View.ForwardLightingResources->NumCulledLightsGrid.NumBytes != MaxNumCells * NumCulledLightsGridStride * sizeof(uint32))
		{
			UE_CLOG(MaxNumCells * NumCulledLightsGridStride * sizeof(uint32) > 256llu * (1llu << 20llu), LogRenderer, Warning,
				TEXT("Attempt to allocate large FRWBuffer (not supported by Metal): View.ForwardLightingResources->NumCulledLightsGrid %u Bytes, LightGridSize %dx%dx%d, NumCulledGridPrimitiveTypes %d, MaxNumCells %d, NumCulledLightsGridStride %d, View Resolution %dx%d"),
				MaxNumCells * NumCulledLightsGridStride * sizeof(uint32), MaxLightGridSizeXY.X, MaxLightGridSizeXY.Y, GLightGridSizeZ, NumCulledGridPrimitiveTypes, MaxNumCells, NumCulledLightsGridStride, View.ViewRect.Size().X, View.ViewRect.Size().Y);

			View.ForwardLightingResources->NumCulledLightsGrid.Initialize(TEXT("NumCulledLightsGrid"), sizeof(uint32), MaxNumCells * NumCulledLightsGridStride, PF_R32_UINT);
		}

		if (View.ForwardLightingResources->CulledLightDataGrid.NumBytes != MaxNumCells * GMaxCulledLightsPerCell * LightIndexTypeSize)
		{
			UE_CLOG(MaxNumCells * GMaxCulledLightsPerCell * sizeof(FLightIndexType) > 256llu * (1llu << 20llu), LogRenderer, Warning,
				TEXT("Attempt to allocate large FRWBuffer (not supported by Metal): View.ForwardLightingResources->CulledLightDataGrid %u Bytes, LightGridSize %dx%dx%d, NumCulledGridPrimitiveTypes %d, MaxNumCells %d, GMaxCulledLightsPerCell %d, View Resolution %dx%d"),
				MaxNumCells * GMaxCulledLightsPerCell * sizeof(FLightIndexType), MaxLightGridSizeXY.X, MaxLightGridSizeXY.Y, GLightGridSizeZ, NumCulledGridPrimitiveTypes, MaxNumCells, GMaxCulledLightsPerCell, View.ViewRect.Size().X, View.ViewRect.Size().Y);

			View.ForwardLightingResources->CulledLightDataGrid.Initialize(TEXT("CulledLightDataGrid"), LightIndexTypeSize, MaxNumCells * GMaxCulledLightsPerCell, LightIndexTypeSize == sizeof(uint16) ? PF_R16_UINT : PF_R32_UINT);
		}

		const bool bShouldCacheTemporaryBuffers = View.ViewState != nullptr;
		FForwardLightingCullingResources& ForwardLightingCullingResources = bShouldCacheTemporaryBuffers
			? View.ViewState->ForwardLightingCullingResources
			: *GraphBuilder.AllocObject<FForwardLightingCullingResources>();

		const uint32 CulledLightLinksElements = MaxNumCells * GMaxCulledLightsPerCell * LightLinkStride;

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
		if (ViewSpacePosAndRadiusData.Num() == 0)
		{
			// Make sure the buffer gets created even though we're not going to read from it in the shader, for platforms like PS4 that assert on null resources being bound
			ViewSpacePosAndRadiusData.AddZeroed();
			ViewSpaceDirAndPreprocAngleData.AddZeroed();
		}
		check(ViewSpacePosAndRadiusData.Num() == ForwardLocalLightData.Num());
		check(ViewSpaceDirAndPreprocAngleData.Num() == ForwardLocalLightData.Num());

		UpdateDynamicVector4BufferData(ViewSpacePosAndRadiusData, ForwardLightingCullingResources.ViewSpacePosAndRadiusData, TEXT("ViewSpacePosAndRadiusData"));
		UpdateDynamicVector4BufferData(ViewSpaceDirAndPreprocAngleData, ForwardLightingCullingResources.ViewSpaceDirAndPreprocAngleData, TEXT("ViewSpacePosAndRadiusData"));
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA

		// Used to pass to the GetDynamicLighting but not actually used, since USE_SOURCE_TEXTURE is 0
		ForwardLightData.DummyRectLightSourceTexture = GWhiteTexture->TextureRHI;
		ForwardLightData.NumCulledLightsGrid = View.ForwardLightingResources->NumCulledLightsGrid.SRV;
		ForwardLightData.CulledLightDataGrid = View.ForwardLightingResources->CulledLightDataGrid.SRV;

		View.ForwardLightingResources->ForwardLightDataUniformBuffer = TUniformBufferRef<FForwardLightData>::CreateUniformBufferImmediate(ForwardLightData, UniformBuffer_SingleFrame);

		const FIntVector NumGroups = FIntVector::DivideAndRoundUp(FIntVector(LightGridSizeXY.X, LightGridSizeXY.Y, GLightGridSizeZ), LightGridInjectionGroupSize);

		// TODO: When external buffers and extract can be set up maybe use that to get these out instead.
		TArray<FRHIUnorderedAccessView*, TInlineAllocator<2>> OutUAVs({
			View.ForwardLightingResources->NumCulledLightsGrid.UAV,
			View.ForwardLightingResources->CulledLightDataGrid.UAV });

		auto& UAVTransitions = *GraphBuilder.AllocObject<TArray<FRHITransitionInfo, TInlineAllocator<2, SceneRenderingAllocator>>>();
		for (FRHIUnorderedAccessView* UAV : OutUAVs)
		{
			UAVTransitions.Add(FRHITransitionInfo(UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		}

		AddPass(GraphBuilder, RDG_EVENT_NAME("TransitionToUAV"), [&UAVTransitions](FRHIComputeCommandList& RHICmdList)
		{
			RHICmdList.Transition(MakeArrayView(UAVTransitions.GetData(), UAVTransitions.Num()));
		});

		{
			RDG_EVENT_SCOPE(GraphBuilder, "CullLights %ux%ux%u NumLights %u NumCaptures %u",
				ForwardLightData.CulledGridSize.X,
				ForwardLightData.CulledGridSize.Y,
				ForwardLightData.CulledGridSize.Z,
				ForwardLightData.NumLocalLights,
				ForwardLightData.NumReflectionCaptures);

			// TODO: change to render graph?
			//FRDGBufferRef NumCulledLightsGridBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxNumCells * NumCulledLightsGridStride), TEXT("NumCulledLightsGrid"));
			//FRDGBufferRef CulledLightDataGridBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(LightIndexTypeSize, MaxNumCells * GMaxCulledLightsPerCell), TEXT("CulledLightDataGrid"));

			FRDGBufferRef CulledLightLinksBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CulledLightLinksElements), TEXT("CulledLightLinks"));
			FRDGBufferRef StartOffsetGridBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxNumCells), TEXT("StartOffsetGrid"));
			FRDGBufferRef NextCulledLightLinkBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("NextCulledLightLink"));
			FRDGBufferRef NextCulledLightDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("NextCulledLightData"));

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
			// TODO: When CPU-side generative passes are added this can also use render graph
			//FRDGBufferRef ViewSpacePosAndRadiusDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), ViewSpacePosAndRadiusData.Num()), TEXT("ViewSpacePosAndRadiusData"));
			//FRDGBufferRef ViewSpaceDirAndPreprocAngleDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), ViewSpaceDirAndPreprocAngleData.Num()), TEXT("ViewSpacePosAndRadiusData"));
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA

			FLightGridInjectionCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FLightGridInjectionCS::FParameters>();

			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->ReflectionCapture = View.ReflectionCaptureUniformBuffer;
			PassParameters->Forward = View.ForwardLightingResources->ForwardLightDataUniformBuffer;
			PassParameters->RWNumCulledLightsGrid = View.ForwardLightingResources->NumCulledLightsGrid.UAV;
			PassParameters->RWCulledLightDataGrid = View.ForwardLightingResources->CulledLightDataGrid.UAV;
			PassParameters->RWNextCulledLightLink = GraphBuilder.CreateUAV(NextCulledLightLinkBuffer, PF_R32_UINT);
			PassParameters->RWStartOffsetGrid = GraphBuilder.CreateUAV(StartOffsetGridBuffer, PF_R32_UINT);
			PassParameters->RWCulledLightLinks = GraphBuilder.CreateUAV(CulledLightLinksBuffer, PF_R32_UINT);

#if ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA
			// TODO: When CPU-side generative passes are added this can also use render graph
			//PassParameters->LightViewSpacePositionAndRadius = GraphBuilder.CreateSRV(ViewSpacePosAndRadiusDataBuffer);
			//PassParameters->LightViewSpaceDirAndPreprocAngle = GraphBuilder.CreateSRV(ViewSpaceDirAndPreprocAngleDataBuffer);
			PassParameters->LightViewSpacePositionAndRadius = ForwardLightingCullingResources.ViewSpacePosAndRadiusData.SRV;
			PassParameters->LightViewSpaceDirAndPreprocAngle = ForwardLightingCullingResources.ViewSpaceDirAndPreprocAngleData.SRV;
#endif // ENABLE_LIGHT_CULLING_VIEW_SPACE_BUILD_DATA

			FLightGridInjectionCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FLightGridInjectionCS::FUseLinkedListDim>(GLightLinkedListCulling != 0);
			TShaderMapRef<FLightGridInjectionCS> ComputeShader(View.ShaderMap, PermutationVector);

			if (GLightLinkedListCulling != 0)
			{
				AddClearUAVPass(GraphBuilder, PassParameters->RWStartOffsetGrid, 0xFFFFFFFF);
				AddClearUAVPass(GraphBuilder, PassParameters->RWNextCulledLightLink, 0);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NextCulledLightDataBuffer, PF_R32_UINT), 0);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightGridInject:LinkedList"), ComputeShader, PassParameters, NumGroups);


				{
					TShaderMapRef<FLightGridCompactCS> ComputeShaderCompact(View.ShaderMap);
					FLightGridCompactCS::FParameters *PassParametersCompact = GraphBuilder.AllocParameters<FLightGridCompactCS::FParameters>();
					PassParametersCompact->View = View.ViewUniformBuffer;
					PassParametersCompact->Forward = View.ForwardLightingResources->ForwardLightDataUniformBuffer;

					PassParametersCompact->CulledLightLinks = GraphBuilder.CreateSRV(CulledLightLinksBuffer, PF_R32_UINT);
					PassParametersCompact->RWNumCulledLightsGrid = View.ForwardLightingResources->NumCulledLightsGrid.UAV;
					PassParametersCompact->RWCulledLightDataGrid = View.ForwardLightingResources->CulledLightDataGrid.UAV;
					PassParametersCompact->RWNextCulledLightData = GraphBuilder.CreateUAV(NextCulledLightDataBuffer, PF_R32_UINT);
					PassParametersCompact->StartOffsetGrid = GraphBuilder.CreateSRV(StartOffsetGridBuffer, PF_R32_UINT);

					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CompactLinks"), ComputeShaderCompact, PassParametersCompact, NumGroups);
				}
			}
			else
			{
				AddPass(GraphBuilder, RDG_EVENT_NAME("ClearUAVInt"), [&View](FRHICommandList& RHICmdList)
				{
					RHICmdList.ClearUAVUint(View.ForwardLightingResources->NumCulledLightsGrid.UAV, FUintVector4(0, 0, 0, 0));
				});
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightGridInject:NotLinkedList"), ComputeShader, PassParameters, NumGroups);
			}
		}

		auto& SRVTransitions = *GraphBuilder.AllocObject<TArray<FRHITransitionInfo, TInlineAllocator<2, SceneRenderingAllocator>>>();
		for (FRHIUnorderedAccessView* UAV : OutUAVs)
		{
			SRVTransitions.Add(FRHITransitionInfo(UAV, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		}

		AddPass(GraphBuilder, RDG_EVENT_NAME("TransitionToSRV"), [&SRVTransitions](FRHIComputeCommandList& RHICmdList)
		{
			RHICmdList.Transition(MakeArrayView(SRVTransitions.GetData(), SRVTransitions.Num()));
		});
	}
}

void FDeferredShadingSceneRenderer::GatherLightsAndComputeLightGrid(FRDGBuilder& GraphBuilder, bool bNeedLightGrid, FSortedLightSetSceneInfo &SortedLightSet)
{
	bool bShadowedLightsInClustered = ShouldUseClusteredDeferredShading()
		&& CVarVirtualShadowOnePassProjection.GetValueOnRenderThread()
		&& VirtualShadowMapArray.IsEnabled();

	GatherAndSortLights(SortedLightSet, bShadowedLightsInClustered);
	
	if (!bNeedLightGrid)
	{
		for (auto& View : Views)
		{
			View.ForwardLightingResources = GetMinimalDummyForwardLightingResources();
		}
		return;
	}

	bool bAnyViewUsesForwardLighting = false;
	bool bAnyViewUsesLumen = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		bAnyViewUsesForwardLighting |= View.bTranslucentSurfaceLighting || ShouldRenderVolumetricFog() || View.bHasSingleLayerWaterMaterial || VolumetricCloudWantsToSampleLocalLights(Scene, ViewFamily.EngineShowFlags);
		bAnyViewUsesLumen |= GetViewPipelineState(View).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen || GetViewPipelineState(View).ReflectionsMethod == EReflectionsMethod::Lumen;
	}
	
	const bool bCullLightsToGrid = GLightCullingQuality 
		&& (IsForwardShadingEnabled(ShaderPlatform) || bAnyViewUsesForwardLighting || IsRayTracingEnabled() || ShouldUseClusteredDeferredShading() || bAnyViewUsesLumen || ViewFamily.EngineShowFlags.VisualizeMeshDistanceFields || VirtualShadowMapArray.IsEnabled());

	// Store this flag if lights are injected in the grids, check with 'AreLightsInLightGrid()'
	bAreLightsInLightGrid = bCullLightsToGrid;
	
	ComputeLightGrid(GraphBuilder, bCullLightsToGrid, SortedLightSet);
}

void FDeferredShadingSceneRenderer::RenderForwardShadowProjections(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef& OutForwardScreenSpaceShadowMask,
	FRDGTextureRef& OutForwardScreenSpaceShadowMaskSubPixel)
{
	CheckShadowDepthRenderCompleted();

	const bool bIsHairEnable = HairStrands::HasViewHairStrandsData(Views);
	bool bScreenShadowMaskNeeded = false;

	FRDGTextureRef SceneDepthTexture = SceneTextures.Depth.Target;

	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* const LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		const FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];

		bScreenShadowMaskNeeded |= VisibleLightInfo.ShadowsToProject.Num() > 0 || VisibleLightInfo.CapsuleShadowsToProject.Num() > 0 || LightSceneInfo->Proxy->GetLightFunctionMaterial() != nullptr;
	}

	if (bScreenShadowMaskNeeded)
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderForwardShadingShadowProjections);

		FRDGTextureMSAA ForwardScreenSpaceShadowMask;
		FRDGTextureMSAA ForwardScreenSpaceShadowMaskSubPixel;

		{
			FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_B8G8R8A8, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource));
			Desc.NumSamples = SceneDepthTexture->Desc.NumSamples;
			ForwardScreenSpaceShadowMask = CreateTextureMSAA(GraphBuilder, Desc, TEXT("ShadowMaskTexture"), GFastVRamConfig.ScreenSpaceShadowMask);
			if (bIsHairEnable)
			{
				ForwardScreenSpaceShadowMaskSubPixel = CreateTextureMSAA(GraphBuilder, Desc, TEXT("ShadowMaskSubPixelTexture"), GFastVRamConfig.ScreenSpaceShadowMask);
			}
		}

		RDG_EVENT_SCOPE(GraphBuilder, "ShadowProjectionOnOpaque");
		RDG_GPU_STAT_SCOPE(GraphBuilder, ShadowProjection);

		// All shadows render with min blending
		AddClearRenderTargetPass(GraphBuilder, ForwardScreenSpaceShadowMask.Target);
		if (bIsHairEnable)
		{
			AddClearRenderTargetPass(GraphBuilder, ForwardScreenSpaceShadowMaskSubPixel.Target);
		}

		const bool bProjectingForForwardShading = true;

		for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
		{
			const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
			const FLightSceneInfo* const LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
			FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];

			const bool bIssueLightDrawEvent = VisibleLightInfo.ShadowsToProject.Num() > 0 || VisibleLightInfo.CapsuleShadowsToProject.Num() > 0;

			FString LightNameWithLevel;
			GetLightNameForDrawEvent(LightSceneInfo->Proxy, LightNameWithLevel);
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, bIssueLightDrawEvent, "%s", *LightNameWithLevel);

			if (VisibleLightInfo.ShadowsToProject.Num() > 0)
			{
				RenderShadowProjections(
					GraphBuilder,
					SceneTextures,
					ForwardScreenSpaceShadowMask.Target,
					ForwardScreenSpaceShadowMaskSubPixel.Target,
					LightSceneInfo,
					bProjectingForForwardShading);

				if (bIsHairEnable)
				{
					RenderHairStrandsShadowMask(GraphBuilder, Views, LightSceneInfo, ForwardScreenSpaceShadowMask.Target);
				}
			}

			RenderCapsuleDirectShadows(GraphBuilder, SceneTextures.UniformBuffer, *LightSceneInfo, ForwardScreenSpaceShadowMask.Target, VisibleLightInfo.CapsuleShadowsToProject, bProjectingForForwardShading);

			if (LightSceneInfo->GetDynamicShadowMapChannel() >= 0 && LightSceneInfo->GetDynamicShadowMapChannel() < 4)
			{
				RenderLightFunction(
					GraphBuilder,
					SceneTextures,
					LightSceneInfo,
					ForwardScreenSpaceShadowMask.Target,
					true, true, false);
			}
		}

			AddCopyToResolveTargetPass(GraphBuilder, ForwardScreenSpaceShadowMask.Target, ForwardScreenSpaceShadowMask.Resolve, FResolveParams());
			OutForwardScreenSpaceShadowMask = ForwardScreenSpaceShadowMask.Resolve;
			if (bIsHairEnable)
			{
				AddCopyToResolveTargetPass(GraphBuilder, ForwardScreenSpaceShadowMaskSubPixel.Target, ForwardScreenSpaceShadowMaskSubPixel.Resolve, FResolveParams());
				OutForwardScreenSpaceShadowMaskSubPixel = ForwardScreenSpaceShadowMaskSubPixel.Resolve;
			}
		}
	}

#undef CHANGE_LIGHTINDEXTYPE_SIZE
