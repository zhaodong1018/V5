// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "Layout/SlateRect.h"

class FSlatePostProcessResource;
class IRendererModule;

struct FPostProcessRectParams
{
	FTexture2DRHIRef SourceTexture;
	FSlateRect SourceRect;
	FSlateRect DestRect;
	FVector4 CornerRadius;
	FIntPoint SourceTextureSize;
	TFunction<void(FRHICommandListImmediate&, FGraphicsPipelineStateInitializer&)> RestoreStateFunc;
	uint32 StencilRef{};
};

struct FBlurRectParams
{
	int32 KernelSize;
	int32 DownsampleAmount;
	float Strength;
};

class FSlatePostProcessor
{
public:
	FSlatePostProcessor();
	~FSlatePostProcessor();

	void BlurRect(FRHICommandListImmediate& RHICmdList, IRendererModule& RendererModule, const FBlurRectParams& Params, const FPostProcessRectParams& RectParams);
	
	void ColorDeficiency(FRHICommandListImmediate& RHICmdList, IRendererModule& RendererModule, const FPostProcessRectParams& RectParams);
	
	void ReleaseRenderTargets();

private:
	void DownsampleRect(FRHICommandListImmediate& RHICmdList, IRendererModule& RendererModule, const FPostProcessRectParams& Params, const FIntPoint& DownsampleSize);
	void UpsampleRect(FRHICommandListImmediate& RHICmdList, IRendererModule& RendererModule, const FPostProcessRectParams& Params, const FIntPoint& DownsampleSize, FSamplerStateRHIRef& Sampler);
	int32 ComputeBlurWeights(int32 KernelSize, float StdDev, TArray<FVector4f>& OutWeightsAndOffsets);
private:
	FSlatePostProcessResource* IntermediateTargets;
};