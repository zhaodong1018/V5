// Copyright Epic Games, Inc. All Rights Reserved.

#include "VT/VirtualTextureLevelRedirector.h"

FVirtualTextureLevelRedirector::FVirtualTextureLevelRedirector(IVirtualTexture* InVirtualTexture0, IVirtualTexture* InVirtualTexture1, int32 InTransitionLevel)
	: VirtualTextures{ InVirtualTexture0, InVirtualTexture1 }
	, TransitionLevel(InTransitionLevel)
{
}

FVirtualTextureLevelRedirector::~FVirtualTextureLevelRedirector()
{
	delete VirtualTextures[0];
	delete VirtualTextures[1];
}

FVTRequestPageResult FVirtualTextureLevelRedirector::RequestPageData(
	const FVirtualTextureProducerHandle& ProducerHandle,
	uint8 LayerMask,
	uint8 vLevel,
	uint64 vAddress,
	EVTRequestPagePriority Priority)
{
	int32 VirtualTextureIndex = vLevel < TransitionLevel ? 0 : 1;
	int32 vLevelOffset = vLevel < TransitionLevel ? 0 : TransitionLevel;
	return VirtualTextures[VirtualTextureIndex]->RequestPageData(ProducerHandle, LayerMask, vLevel - vLevelOffset, vAddress, Priority);
}

IVirtualTextureFinalizer* FVirtualTextureLevelRedirector::ProducePageData(
	FRHICommandListImmediate& RHICmdList,
	ERHIFeatureLevel::Type FeatureLevel,
	EVTProducePageFlags Flags,
	const FVirtualTextureProducerHandle& ProducerHandle,
	uint8 LayerMask,
	uint8 vLevel,
	uint64 vAddress,
	uint64 RequestHandle,
	const FVTProduceTargetLayer* TargetLayers)
{
	int32 VirtualTextureIndex = vLevel < TransitionLevel ? 0 : 1;
	int32 vLevelOffset = vLevel < TransitionLevel ? 0 : TransitionLevel;
	return VirtualTextures[VirtualTextureIndex]->ProducePageData(RHICmdList, FeatureLevel, Flags, ProducerHandle, LayerMask, vLevel - vLevelOffset, vAddress, RequestHandle, TargetLayers);
}