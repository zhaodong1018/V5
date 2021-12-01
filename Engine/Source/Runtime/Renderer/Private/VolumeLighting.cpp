// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LightShadowShaderParameters.h
=============================================================================*/

#include "VolumeLighting.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "LightSceneInfo.h"
#include "ShadowRendering.h"
#include "Components/LightComponent.h"
#include "Engine/MapBuildDataRegistry.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVolumeShadowingShaderParametersGlobal0, "Light0Shadow");
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVolumeShadowingShaderParametersGlobal1, "Light1Shadow");

const FProjectedShadowInfo* GetFirstWholeSceneShadowMap(const FVisibleLightInfo& VisibleLightInfo)
{
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		const FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			&& !ProjectedShadowInfo->bRayTracedDistanceField)
		{
			return ProjectedShadowInfo;
		}
	}
	return nullptr;
}

static auto SetVolumeShadowingDefaultShaderParametersGlobal = [](FRDGBuilder& GraphBuilder, auto& ShaderParams)
{
	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
	FRDGTextureRef BlackDepthCubeTexture = SystemTextures.BlackDepthCube;

	ShaderParams.WorldToShadowMatrix = FMatrix::Identity;
	ShaderParams.ShadowmapMinMax = FVector4f(1.0f);
	ShaderParams.DepthBiasParameters = FVector4f(1.0f);
	ShaderParams.ShadowInjectParams = FVector4f(1.0f);
	memset(ShaderParams.ClippingPlanes.GetData(), 0, sizeof(ShaderParams.ClippingPlanes));
	ShaderParams.bStaticallyShadowed = 0;
	ShaderParams.WorldToStaticShadowMatrix = FMatrix::Identity;
	ShaderParams.StaticShadowBufferSize = FVector4f(1.0f);
	ShaderParams.ShadowDepthTexture = SystemTextures.White;
	ShaderParams.StaticShadowDepthTexture = GWhiteTexture->TextureRHI;
	ShaderParams.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	ShaderParams.StaticShadowDepthTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	memset(ShaderParams.OnePassPointShadowProjection.ShadowViewProjectionMatrices.GetData(), 0, sizeof(ShaderParams.OnePassPointShadowProjection.ShadowViewProjectionMatrices));
	ShaderParams.OnePassPointShadowProjection.InvShadowmapResolution = 1.0f;
	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTexture = BlackDepthCubeTexture;
	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTexture2 = BlackDepthCubeTexture;
	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 0, 0, SCF_Less>::GetRHI();
};



static auto GetVolumeShadowingShaderParametersGlobal = [](
	FRDGBuilder& GraphBuilder,
	auto& ShaderParams,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	const FProjectedShadowInfo* ShadowInfo,
	int32 InnerSplitIndex)
{
	const bool bDynamicallyShadowed = ShadowInfo != NULL;
	if (bDynamicallyShadowed)
	{
		FVector4f ShadowmapMinMaxValue;
		ShaderParams.WorldToShadowMatrix = ShadowInfo->GetWorldToShadowMatrix(ShaderParams.ShadowmapMinMax);
	}
	else
	{
		ShaderParams.WorldToShadowMatrix = FMatrix::Identity;
		ShaderParams.ShadowmapMinMax = FVector4f(1.0f);
	}

	// default to ignore the plane
	FVector4f Planes[2] = { FVector4f(0, 0, 0, -1), FVector4f(0, 0, 0, -1) };
	// .zw:DistanceFadeMAD to use MAD for efficiency in the shader, default to ignore the plane
	FVector4f ShadowInjectParamValue(1, 1, 0, 0);

	if (InnerSplitIndex != INDEX_NONE)
	{
		FShadowCascadeSettings ShadowCascadeSettings;

		LightSceneInfo->Proxy->GetShadowSplitBounds(View, InnerSplitIndex, LightSceneInfo->IsPrecomputedLightingValid(), &ShadowCascadeSettings);
		ensureMsgf(ShadowCascadeSettings.ShadowSplitIndex != INDEX_NONE, TEXT("FLightSceneProxy::GetShadowSplitBounds did not return an initialized ShadowCascadeSettings"));

		// near cascade plane
		{
			ShadowInjectParamValue.X = ShadowCascadeSettings.SplitNearFadeRegion == 0 ? 1.0f : 1.0f / ShadowCascadeSettings.SplitNearFadeRegion;
			Planes[0] = FVector4f((FVector)(ShadowCascadeSettings.NearFrustumPlane), -ShadowCascadeSettings.NearFrustumPlane.W);
		}

		uint32 CascadeCount = LightSceneInfo->Proxy->GetNumViewDependentWholeSceneShadows(View, LightSceneInfo->IsPrecomputedLightingValid());

		// far cascade plane
		if (InnerSplitIndex != CascadeCount - 1)
		{
			ShadowInjectParamValue.Y = 1.0f / (ShadowCascadeSettings.SplitFarFadeRegion == 0.0f ? 0.0001f : ShadowCascadeSettings.SplitFarFadeRegion);
			Planes[1] = FVector4f((FVector)(ShadowCascadeSettings.FarFrustumPlane), -ShadowCascadeSettings.FarFrustumPlane.W);
		}

		const FVector2D FadeParams = LightSceneInfo->Proxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), LightSceneInfo->IsPrecomputedLightingValid(), View.MaxShadowCascades);

		// setup constants for the MAD in shader
		ShadowInjectParamValue.Z = FadeParams.Y;
		ShadowInjectParamValue.W = -FadeParams.X * FadeParams.Y;
	}
	ShaderParams.ShadowInjectParams = ShadowInjectParamValue;
	ShaderParams.ClippingPlanes[0] = Planes[0];
	ShaderParams.ClippingPlanes[1] = Planes[1];

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	ELightComponentType LightType = (ELightComponentType)LightSceneInfo->Proxy->GetLightType();
	FRDGTexture* ShadowDepthTextureResource = nullptr;
	if (bDynamicallyShadowed)
	{
		ShaderParams.DepthBiasParameters = FVector4f(ShadowInfo->GetShaderDepthBias(), ShadowInfo->GetShaderSlopeDepthBias(), ShadowInfo->GetShaderMaxSlopeDepthBias(), 1.0f / (ShadowInfo->MaxSubjectZ - ShadowInfo->MinSubjectZ));

		if (LightType == LightType_Point || LightType == LightType_Rect)
		{
			ShadowDepthTextureResource = SystemTextures.Black;
		}
		else
		{
			ShadowDepthTextureResource = GraphBuilder.RegisterExternalTexture(ShadowInfo->RenderTargets.DepthTarget);
		}
	}
	else
	{
		ShadowDepthTextureResource = SystemTextures.Black;
		ShaderParams.DepthBiasParameters = FVector(1.0f);
	}
	check(ShadowDepthTextureResource)
		ShaderParams.ShadowDepthTexture = ShadowDepthTextureResource;
	ShaderParams.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	const FStaticShadowDepthMap* StaticShadowDepthMap = LightSceneInfo->Proxy->GetStaticShadowDepthMap();
	const uint32 bStaticallyShadowedValue = LightSceneInfo->IsPrecomputedLightingValid() && StaticShadowDepthMap && StaticShadowDepthMap->Data && StaticShadowDepthMap->TextureRHI ? 1 : 0;
	FRHITexture* StaticShadowDepthMapTexture = bStaticallyShadowedValue ? StaticShadowDepthMap->TextureRHI : GWhiteTexture->TextureRHI;
	const FMatrix WorldToStaticShadow = bStaticallyShadowedValue ? StaticShadowDepthMap->Data->WorldToLight : FMatrix::Identity;
	const FVector4f StaticShadowBufferSizeValue = bStaticallyShadowedValue ? FVector4f(StaticShadowDepthMap->Data->ShadowMapSizeX, StaticShadowDepthMap->Data->ShadowMapSizeY, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeX, 1.0f / StaticShadowDepthMap->Data->ShadowMapSizeY) : FVector4f(0, 0, 0, 0);

	ShaderParams.bStaticallyShadowed = bStaticallyShadowedValue;

	ShaderParams.StaticShadowDepthTexture = StaticShadowDepthMapTexture;
	ShaderParams.StaticShadowDepthTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	ShaderParams.WorldToStaticShadowMatrix = WorldToStaticShadow;
	ShaderParams.StaticShadowBufferSize = StaticShadowBufferSizeValue;

	//
	// See FOnePassPointShadowProjectionShaderParameters from ShadowRendering.h
	//
	FRDGTexture* ShadowDepthTextureValue = ShadowInfo
		? GraphBuilder.RegisterExternalTexture(ShadowInfo->RenderTargets.DepthTarget) 
		: SystemTextures.BlackDepthCube;

	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTexture = ShadowDepthTextureValue;
	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTexture2 = ShadowDepthTextureValue;
	ShaderParams.OnePassPointShadowProjection.ShadowDepthCubeTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 0, 0, SCF_Less>::GetRHI();

	if (bDynamicallyShadowed)
	{
		if (ShadowInfo)
		{
			TArray<FMatrix44f> SIOnePassShadowViewProjectionMatrices = LWC::ConvertArrayType<FMatrix44f>(ShadowInfo->OnePassShadowViewProjectionMatrices);	// LWC_TODO: Precision loss. Perf pessimization
			memcpy(ShaderParams.OnePassPointShadowProjection.ShadowViewProjectionMatrices.GetData(), SIOnePassShadowViewProjectionMatrices.GetData(), SIOnePassShadowViewProjectionMatrices.Num() * sizeof(FMatrix44f));
			ShaderParams.OnePassPointShadowProjection.InvShadowmapResolution = 1.0f / float(ShadowInfo->ResolutionX);
		}
		else
		{
			memset(ShaderParams.OnePassPointShadowProjection.ShadowViewProjectionMatrices.GetData(), 0, sizeof(ShaderParams.OnePassPointShadowProjection.ShadowViewProjectionMatrices));
			ShaderParams.OnePassPointShadowProjection.InvShadowmapResolution = 0.0f;
		}
	}
};



void SetVolumeShadowingShaderParameters(
	FRDGBuilder& GraphBuilder,
	FVolumeShadowingShaderParametersGlobal0& ShaderParams,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	const FProjectedShadowInfo* ShadowInfo,
	int32 InnerSplitIndex)
{
	FLightShaderParameters LightParameters;
	LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);
	ShaderParams.Position = LightParameters.Position;
	ShaderParams.InvRadius = LightParameters.InvRadius;

	GetVolumeShadowingShaderParametersGlobal(
		GraphBuilder,
		ShaderParams.VolumeShadowingShaderParameters,
		View,
		LightSceneInfo,
		ShadowInfo,
		InnerSplitIndex);
}

void SetVolumeShadowingShaderParameters(
	FRDGBuilder& GraphBuilder,
	FVolumeShadowingShaderParametersGlobal1& ShaderParams,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	const FProjectedShadowInfo* ShadowInfo,
	int32 InnerSplitIndex)
{
	FLightShaderParameters LightParameters;
	LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);
	ShaderParams.Position = LightParameters.Position;
	ShaderParams.InvRadius = LightParameters.InvRadius;

	GetVolumeShadowingShaderParametersGlobal(
		GraphBuilder,
		ShaderParams.VolumeShadowingShaderParameters,
		View,
		LightSceneInfo,
		ShadowInfo,
		InnerSplitIndex);
}

void SetVolumeShadowingDefaultShaderParameters(FRDGBuilder& GraphBuilder, FVolumeShadowingShaderParametersGlobal0& ShaderParams)
{
	ShaderParams.Position = FVector(1.0f);
	ShaderParams.InvRadius = 1.0f;
	SetVolumeShadowingDefaultShaderParametersGlobal(GraphBuilder, ShaderParams.VolumeShadowingShaderParameters);
}

void SetVolumeShadowingDefaultShaderParameters(FRDGBuilder& GraphBuilder, FVolumeShadowingShaderParametersGlobal1& ShaderParams)
{
	ShaderParams.Position = FVector(1.0f);
	ShaderParams.InvRadius = 1.0f;
	SetVolumeShadowingDefaultShaderParametersGlobal(GraphBuilder, ShaderParams.VolumeShadowingShaderParameters);
}

