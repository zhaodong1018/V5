// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RenderGraph.h"
#include "ScenePrivate.h"

struct FBloomOutputs;

// Returns whether FFT bloom is enabled for the view.
bool IsFFTBloomEnabled(const FViewInfo& View);

struct FFFTBloomInputs
{
	FRDGTextureRef FullResolutionTexture;
	FIntRect FullResolutionViewRect;

	FRDGTextureRef HalfResolutionTexture;
	FIntRect HalfResolutionViewRect;
};

bool IsFFTBloomQuarterResolutionEnabled();

FBloomOutputs AddFFTBloomPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FFFTBloomInputs& Inputs);
