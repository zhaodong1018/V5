// Copyright Epic Games, Inc. All Rights Reserved.

#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessAA.h"
#if WITH_EDITOR
	#include "PostProcess/PostProcessBufferInspector.h"
#endif
#include "PostProcess/DiaphragmDOF.h"
#include "PostProcess/PostProcessMaterial.h"
#include "PostProcess/PostProcessWeightedSampleSum.h"
#include "PostProcess/PostProcessBloomSetup.h"
#include "PostProcess/PostProcessMobile.h"
#include "PostProcess/PostProcessDownsample.h"
#include "PostProcess/PostProcessHistogram.h"
#include "PostProcess/PostProcessLocalExposure.h"
#include "PostProcess/PostProcessVisualizeHDR.h"
#include "PostProcess/PostProcessVisualizeLocalExposure.h"
#include "PostProcess/VisualizeShadingModels.h"
#include "PostProcess/PostProcessSelectionOutline.h"
#include "PostProcess/PostProcessVisualizeLevelInstance.h"
#include "PostProcess/PostProcessGBufferHints.h"
#include "PostProcess/PostProcessVisualizeBuffer.h"
#include "PostProcess/PostProcessVisualizeNanite.h"
#include "PostProcess/PostProcessEyeAdaptation.h"
#include "PostProcess/PostProcessTonemap.h"
#include "PostProcess/PostProcessLensFlares.h"
#include "PostProcess/PostProcessBokehDOF.h"
#include "PostProcess/PostProcessCombineLUTs.h"
#include "PostProcess/PostProcessDeviceEncodingOnly.h"
#include "PostProcess/TemporalAA.h"
#include "PostProcess/PostProcessMotionBlur.h"
#include "PostProcess/PostProcessDOF.h"
#include "PostProcess/PostProcessUpscale.h"
#include "PostProcess/PostProcessHMD.h"
#include "PostProcess/PostProcessVisualizeComplexity.h"
#include "PostProcess/PostProcessCompositeEditorPrimitives.h"
#include "PostProcess/PostProcessTestImage.h"
#include "PostProcess/PostProcessVisualizeCalibrationMaterial.h"
#include "PostProcess/PostProcessFFTBloom.h"
#include "PostProcess/PostProcessStreamingAccuracyLegend.h"
#include "PostProcess/PostProcessSubsurface.h"
#include "ShaderPrint.h"
#include "ShaderDebug.h"
#include "HighResScreenshot.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "DeferredShadingRenderer.h"
#include "MobileSeparateTranslucencyPass.h"
#include "MobileDistortionPass.h"
#include "SceneTextureParameters.h"
#include "PixelShaderUtils.h"
#include "ScreenSpaceRayTracing.h"
#include "SceneViewExtension.h"
#include "FXSystem.h"
#include "SkyAtmosphereRendering.h"
#include "Strata/Strata.h"

bool IsMobileEyeAdaptationEnabled(const FViewInfo& View);

bool IsValidBloomSetupVariation(bool bUseBloom, bool bUseSun, bool bUseDof, bool bUseEyeAdaptation);

namespace
{
TAutoConsoleVariable<float> CVarDepthOfFieldNearBlurSizeThreshold(
	TEXT("r.DepthOfField.NearBlurSizeThreshold"),
	0.01f,
	TEXT("Sets the minimum near blur size before the effect is forcably disabled. Currently only affects Gaussian DOF.\n")
	TEXT(" (default: 0.01)"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarDepthOfFieldMaxSize(
	TEXT("r.DepthOfField.MaxSize"),
	100.0f,
	TEXT("Allows to clamp the gaussian depth of field radius (for better performance), default: 100"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPostProcessingPropagateAlpha(
	TEXT("r.PostProcessing.PropagateAlpha"),
	0,
	TEXT("0 to disable scene alpha channel support in the post processing.\n")
	TEXT(" 0: disabled (default);\n")
	TEXT(" 1: enabled in linear color space;\n")
	TEXT(" 2: same as 1, but also enable it through the tonemapper. Compositing after the tonemapper is incorrect, as their is no meaning to tonemap the alpha channel. This is only meant to be use exclusively for broadcasting hardware that does not support linear color space compositing and tonemapping."),
	ECVF_ReadOnly);

TAutoConsoleVariable<int32> CVarPostProcessingPreferCompute(
	TEXT("r.PostProcessing.PreferCompute"),
	0,
	TEXT("Will use compute shaders for post processing where implementations available."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPostProcessingQuarterResolutionDownsample(
	TEXT("r.PostProcessing.QuarterResolutionDownsample"),
	0,
	TEXT("Uses quarter resolution downsample instead of half resolution to feed into exposure / bloom."),
	ECVF_RenderThreadSafe);

#if !(UE_BUILD_SHIPPING)
TAutoConsoleVariable<int32> CVarPostProcessingForceAsyncDispatch(
	TEXT("r.PostProcessing.ForceAsyncDispatch"),
	0,
	TEXT("Will force asynchronous dispatch for post processing compute shaders where implementations available.\n")
	TEXT("Only available for testing in non-shipping builds."),
	ECVF_RenderThreadSafe);
#endif

} //! namespace

bool IsPostProcessingWithComputeEnabled(ERHIFeatureLevel::Type FeatureLevel)
{
	// Any thread is used due to FViewInfo initialization.
	return CVarPostProcessingPreferCompute.GetValueOnAnyThread() && FeatureLevel >= ERHIFeatureLevel::SM5;
}

bool IsPostProcessingOutputInHDR()
{
	static const auto CVarDumpFramesAsHDR = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.BufferVisualizationDumpFramesAsHDR"));

	return CVarDumpFramesAsHDR->GetValueOnRenderThread() != 0 || GetHighResScreenshotConfig().bCaptureHDR;
}

bool IsPostProcessingEnabled(const FViewInfo& View)
{
	if (View.GetFeatureLevel() >= ERHIFeatureLevel::SM5)
	{
		return
			 View.Family->EngineShowFlags.PostProcessing &&
			!View.Family->EngineShowFlags.VisualizeDistanceFieldAO &&
			!View.Family->EngineShowFlags.VisualizeShadingModels &&
			!View.Family->EngineShowFlags.VisualizeGlobalDistanceField &&
			!View.Family->EngineShowFlags.VisualizeVolumetricCloudConservativeDensity &&
			!View.Family->EngineShowFlags.VisualizeLumenIndirectDiffuse &&
			!View.Family->EngineShowFlags.ShaderComplexity;
	}
	else
	{
		return View.Family->EngineShowFlags.PostProcessing && !View.Family->EngineShowFlags.ShaderComplexity && IsMobileHDR();
	}
}

bool IsPostProcessingQuarterResolutionDownsampleEnabled()
{
	return CVarPostProcessingQuarterResolutionDownsample.GetValueOnRenderThread() != 0;
}

bool IsPostProcessingWithAlphaChannelSupported()
{
	return CVarPostProcessingPropagateAlpha.GetValueOnAnyThread() != 0;
}

class FComposeSeparateTranslucencyPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComposeSeparateTranslucencyPS);
	SHADER_USE_PARAMETER_STRUCT(FComposeSeparateTranslucencyPS, FGlobalShader);

	class FNearestDepthNeighborUpsampling : SHADER_PERMUTATION_BOOL("PERMUTATION_NEARESTDEPTHNEIGHBOR");
	using FPermutationDomain = TShaderPermutationDomain<FNearestDepthNeighborUpsampling>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Color)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Translucency)
		SHADER_PARAMETER(FScreenTransform, ColorToTranslucency)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateTranslucency)
		SHADER_PARAMETER_SAMPLER(SamplerState, SeparateTranslucencySampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateModulation)
		SHADER_PARAMETER_SAMPLER(SamplerState, SeparateModulationSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LowResDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LowResDepthSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FullResDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, FullResDepthSampler)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FComposeSeparateTranslucencyPS, "/Engine/Private/ComposeSeparateTranslucency.usf", "MainPS", SF_Pixel);

extern bool GetUseTranslucencyNearestDepthNeighborUpsample(float DownsampleScale);

#if DEBUG_POST_PROCESS_VOLUME_ENABLE
FScreenPassTexture AddFinalPostProcessDebugInfoPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, FScreenPassTexture& ScreenPassSceneColor);
#endif

FRDGTextureRef AddTranslucencyCompositionPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FScreenPassTexture& SceneColor,
	const FScreenPassTexture& SceneDepth,
	const FSeparateTranslucencyTextures& SeparateTranslucencyTextures,
	bool bPostMotionBlur,
	bool bApplyModulateOnly)
{
	FRDGTextureRef SeparateModulationTexture;
	FRDGTextureRef SeparateTranslucencyTexture;

	if (bPostMotionBlur)
	{
		check(!bApplyModulateOnly);

		// if nothing is rendered into the separate translucency, then just return the existing Scenecolor
		if (!SeparateTranslucencyTextures.IsPostMotionBlurColorValid())
		{
			return SceneColor.Texture;
		}

		SeparateTranslucencyTexture = SeparateTranslucencyTextures.GetPostMotionBlurColorForRead(GraphBuilder);
		SeparateModulationTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.WhiteDummy);
	}
	else
	{
		// if nothing is rendered into the separate translucency, then just return the existing Scenecolor
		if (!SeparateTranslucencyTextures.IsColorValid() && !SeparateTranslucencyTextures.IsColorModulateValid())
		{
			return SceneColor.Texture;
		}

		SeparateTranslucencyTexture = SeparateTranslucencyTextures.GetColorForRead(GraphBuilder);
		SeparateModulationTexture = SeparateTranslucencyTextures.GetColorModulateForRead(GraphBuilder);

		if (bApplyModulateOnly)
		{
			if (!SeparateTranslucencyTextures.IsColorModulateValid())
			{
				return SceneColor.Texture;
			}

			SeparateTranslucencyTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackAlphaOneDummy);
		}
	}
	
	FRDGTextureRef NewSceneColor;
	{
		FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
		OutputDesc.Reset();

		if (OutputDesc.Format == PF_FloatRGBA && !IsPostProcessingWithAlphaChannelSupported())
		{
			OutputDesc.Format = PF_FloatRGB;
		}

		NewSceneColor = GraphBuilder.CreateTexture(
			OutputDesc,
			bPostMotionBlur ? TEXT("PostMotionBlurTranslucency.SceneColor") : TEXT("PostDOFTranslucency.SceneColor"));
	}

	const FVector2D SceneColorSize(SceneColor.ViewRect.Size());
	const FVector2D SceneColorSizeInv = FVector2D(1.0f, 1.0f) / SceneColorSize;	
	const FVector2D SceneColorExtent(NewSceneColor->Desc.Extent);
	const FVector2D SceneColorExtentInv = FVector2D(1.0f, 1.0f) / SceneColorExtent;
	
	const FIntRect SeparateTranslucencyRect = SeparateTranslucencyTextures.GetDimensions().GetViewport(View.ViewRect).Rect;
	const FVector2D SeparateTranslucencySize(SeparateTranslucencyRect.Size());
	const FVector2D SeparateTranslucencyExtent((bApplyModulateOnly ? SeparateModulationTexture : SeparateTranslucencyTexture)->Desc.Extent);
	const FVector2D SeparateTranslucencyExtentInv = FVector2D(1.0f, 1.0f) / SeparateTranslucencyExtent;

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FScreenPassTextureViewport TranslucencyViewport = SeparateTranslucencyTextures.GetDimensions().GetViewport(View.ViewRect);

	const float DownsampleScale = SeparateTranslucencyTextures.GetDimensions().Scale;

	const bool bScaleSeparateTranslucency = DownsampleScale != 1.0f;
	const bool DepthUpscampling = !bPostMotionBlur && GetUseTranslucencyNearestDepthNeighborUpsample(DownsampleScale) ? 1 : 0;

	FComposeSeparateTranslucencyPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComposeSeparateTranslucencyPS::FParameters>();
	PassParameters->Color = GetScreenPassTextureViewportParameters(SceneColorViewport);
	PassParameters->Translucency = GetScreenPassTextureViewportParameters(TranslucencyViewport);
	PassParameters->ColorToTranslucency = FScreenTransform::ChangeTextureUVCoordinateFromTo(SceneColorViewport, TranslucencyViewport);
	PassParameters->SceneColor = SceneColor.Texture;
	PassParameters->SceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->SeparateTranslucency = SeparateTranslucencyTexture;
	PassParameters->SeparateTranslucencySampler = bScaleSeparateTranslucency ? TStaticSamplerState<SF_Bilinear>::GetRHI() : TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->SeparateModulation = SeparateModulationTexture;
	PassParameters->SeparateModulationSampler = bScaleSeparateTranslucency ? TStaticSamplerState<SF_Bilinear>::GetRHI() : TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(NewSceneColor, ERenderTargetLoadAction::ENoAction);

	if (DepthUpscampling)
	{
		PassParameters->LowResDepthTexture = SeparateTranslucencyTextures.GetDepthForRead(GraphBuilder);
		PassParameters->LowResDepthSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->FullResDepthTexture = SceneDepth.Texture;
		PassParameters->FullResDepthSampler = TStaticSamplerState<SF_Point>::GetRHI();
	}

	FComposeSeparateTranslucencyPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FComposeSeparateTranslucencyPS::FNearestDepthNeighborUpsampling>(DepthUpscampling);

	TShaderMapRef<FComposeSeparateTranslucencyPS> PixelShader(View.ShaderMap, PermutationVector);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME(
			"ComposeTranslucency(%s%s%s) %dx%d",
			bPostMotionBlur ? TEXT("PostMB") : TEXT("PostDOF"),
			bApplyModulateOnly ? TEXT(" ModulateOnly") : TEXT(""),
			bScaleSeparateTranslucency ? TEXT(" Rescale") : TEXT(""),
			SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height()),
		PixelShader,
		PassParameters,
		SceneColor.ViewRect);

	return NewSceneColor;
}

void AddTemporalAA2Passes(
	FRDGBuilder& GraphBuilder,
	const FSceneTextureParameters& SceneTextures,
	const FViewInfo& View,
	FRDGTextureRef InputSceneColorTexture,
	FRDGTextureRef* OutSceneColorTexture,
	FIntRect* OutSceneColorViewRect);

bool ComposeSeparateTranslucencyInTSR(const FViewInfo& View);

void AddPostProcessingPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FPostProcessingInputs& Inputs, const Nanite::FRasterResults* NaniteRasterResults, FInstanceCullingManager& InstanceCullingManager)
{
	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderPostProcessing);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_PostProcessing_Process);

	check(IsInRenderingThread());
	check(View.VerifyMembersChecks());
	Inputs.Validate();

	FScene* Scene = View.Family->Scene->GetRenderScene();

	const FIntRect PrimaryViewRect = View.ViewRect;
	const FIntRect SeparateTranslucencyRect = Inputs.SeparateTranslucencyTextures->GetDimensions().GetViewport(PrimaryViewRect).Rect;

	const FSceneTextureParameters SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, Inputs.SceneTextures);

	const FScreenPassRenderTarget ViewFamilyOutput = FScreenPassRenderTarget::CreateViewFamilyOutput(Inputs.ViewFamilyTexture, View);
	const FScreenPassTexture SceneDepth(SceneTextureParameters.SceneDepthTexture, PrimaryViewRect);
	const FScreenPassTexture SeparateTranslucency(Inputs.SeparateTranslucencyTextures->GetColorForRead(GraphBuilder), PrimaryViewRect);
	const FScreenPassTexture CustomDepth(Inputs.CustomDepthTexture, PrimaryViewRect);
	const FScreenPassTexture Velocity(SceneTextureParameters.GBufferVelocityTexture, PrimaryViewRect);
	const FScreenPassTexture BlackDummy(GSystemTextures.GetBlackDummy(GraphBuilder));
	
	// Post-MotionBlur translucency need only be valid if we rendered it
	FScreenPassTexture PostMotionBlurTranslucency;
	if (Inputs.SeparateTranslucencyTextures->IsPostMotionBlurColorValid())
	{
		PostMotionBlurTranslucency = FScreenPassTexture(Inputs.SeparateTranslucencyTextures->GetPostMotionBlurColorForRead(GraphBuilder), SeparateTranslucencyRect);
	}

	// Scene color is updated incrementally through the post process pipeline.
	FScreenPassTexture SceneColor((*Inputs.SceneTextures)->SceneColorTexture, PrimaryViewRect);

	// Assigned before and after the tonemapper.
	FScreenPassTexture SceneColorBeforeTonemap;
	FScreenPassTexture SceneColorAfterTonemap;

	// Unprocessed scene color stores the original input.
	const FScreenPassTexture OriginalSceneColor = SceneColor;

	// Default the new eye adaptation to the last one in case it's not generated this frame.
	const FEyeAdaptationParameters EyeAdaptationParameters = GetEyeAdaptationParameters(View, ERHIFeatureLevel::SM5);
	FRDGTextureRef LastEyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);
	FRDGTextureRef EyeAdaptationTexture = LastEyeAdaptationTexture;

	// Histogram defaults to black because the histogram eye adaptation pass is used for the manual metering mode.
	FRDGTextureRef HistogramTexture = BlackDummy.Texture;

	FRDGTextureRef LocalExposureTexture = nullptr;
	FRDGTextureRef LocalExposureBlurredLogLumTexture = BlackDummy.Texture;

	const FEngineShowFlags& EngineShowFlags = View.Family->EngineShowFlags;
	const bool bVisualizeHDR = EngineShowFlags.VisualizeHDR;
	const bool bViewFamilyOutputInHDR = GRHISupportsHDROutput && IsHDREnabled();
	const bool bVisualizeGBufferOverview = IsVisualizeGBufferOverviewEnabled(View);
	const bool bVisualizeGBufferDumpToFile = IsVisualizeGBufferDumpToFileEnabled(View);
	const bool bVisualizeGBufferDumpToPIpe = IsVisualizeGBufferDumpToPipeEnabled(View);
	const bool bOutputInHDR = IsPostProcessingOutputInHDR();

	const FPaniniProjectionConfig PaniniConfig(View);

	enum class EPass : uint32
	{
		MotionBlur,
		Tonemap,
		FXAA,
		PostProcessMaterialAfterTonemapping,
		VisualizeDepthOfField,
		VisualizeStationaryLightOverlap,
		VisualizeLightCulling,
		VisualizePostProcessStack,
		VisualizeStrata,
		VisualizeSkyAtmosphere,
		VisualizeLevelInstance,
		SelectionOutline,
		EditorPrimitive,
		VisualizeShadingModels,
		VisualizeGBufferHints,
		VisualizeSubsurface,
		VisualizeGBufferOverview,
		VisualizeHDR,
		VisualizeLocalExposure,
		PixelInspector,
		HMDDistortion,
		HighResolutionScreenshotMask,
		PrimaryUpscale,
		SecondaryUpscale,
		MAX
	};

	const auto TranslatePass = [](ISceneViewExtension::EPostProcessingPass Pass) -> EPass
	{
		switch (Pass)
		{
			case ISceneViewExtension::EPostProcessingPass::MotionBlur            : return EPass::MotionBlur;
			case ISceneViewExtension::EPostProcessingPass::Tonemap               : return EPass::Tonemap;
			case ISceneViewExtension::EPostProcessingPass::FXAA                  : return EPass::FXAA;
			case ISceneViewExtension::EPostProcessingPass::VisualizeDepthOfField : return EPass::VisualizeDepthOfField;

			default:
				check(false);
				return EPass::MAX;
		};
	};

	const TCHAR* PassNames[] =
	{
		TEXT("MotionBlur"),
		TEXT("Tonemap"),
		TEXT("FXAA"),
		TEXT("PostProcessMaterial (AfterTonemapping)"),
		TEXT("VisualizeDepthOfField"),
		TEXT("VisualizeStationaryLightOverlap"),
		TEXT("VisualizeLightCulling"),
		TEXT("VisualizePostProcessStack"),
		TEXT("VisualizeStrata"),
		TEXT("VisualizeSkyAtmosphere"),
		TEXT("VisualizeLevelInstance"),
		TEXT("SelectionOutline"),
		TEXT("EditorPrimitive"),
		TEXT("VisualizeShadingModels"),
		TEXT("VisualizeGBufferHints"),
		TEXT("VisualizeSubsurface"),
		TEXT("VisualizeGBufferOverview"),
		TEXT("VisualizeHDR"),
		TEXT("VisualizeLocalExposure"),
		TEXT("PixelInspector"),
		TEXT("HMDDistortion"),
		TEXT("HighResolutionScreenshotMask"),
		TEXT("PrimaryUpscale"),
		TEXT("SecondaryUpscale")
	};

	static_assert(static_cast<uint32>(EPass::MAX) == UE_ARRAY_COUNT(PassNames), "EPass does not match PassNames.");

	TOverridePassSequence<EPass> PassSequence(ViewFamilyOutput);
	PassSequence.SetNames(PassNames, UE_ARRAY_COUNT(PassNames));
	PassSequence.SetEnabled(EPass::VisualizeStationaryLightOverlap, EngineShowFlags.StationaryLightOverlap);
	PassSequence.SetEnabled(EPass::VisualizeLightCulling, EngineShowFlags.VisualizeLightCulling);
#if DEBUG_POST_PROCESS_VOLUME_ENABLE
	PassSequence.SetEnabled(EPass::VisualizePostProcessStack, EngineShowFlags.VisualizePostProcessStack);
#else
	PassSequence.SetEnabled(EPass::VisualizePostProcessStack, false);
#endif
	PassSequence.SetEnabled(EPass::VisualizeStrata, Strata::ShouldRenderStrataDebugPasses(View));
#if WITH_EDITOR
	PassSequence.SetEnabled(EPass::VisualizeSkyAtmosphere, Scene && View.Family && View.Family->EngineShowFlags.VisualizeSkyAtmosphere&& ShouldRenderSkyAtmosphereDebugPasses(Scene, View.Family->EngineShowFlags));
	PassSequence.SetEnabled(EPass::VisualizeLevelInstance, GIsEditor && EngineShowFlags.EditingLevelInstance && EngineShowFlags.VisualizeLevelInstanceEditing && !bVisualizeHDR);
	PassSequence.SetEnabled(EPass::SelectionOutline, GIsEditor && EngineShowFlags.Selection && EngineShowFlags.SelectionOutline && !EngineShowFlags.Wireframe && !bVisualizeHDR && !IStereoRendering::IsStereoEyeView(View));
	PassSequence.SetEnabled(EPass::EditorPrimitive, FSceneRenderer::ShouldCompositeEditorPrimitives(View));
#else
	PassSequence.SetEnabled(EPass::VisualizeSkyAtmosphere, false);
	PassSequence.SetEnabled(EPass::VisualizeLevelInstance, false);
	PassSequence.SetEnabled(EPass::SelectionOutline, false);
	PassSequence.SetEnabled(EPass::EditorPrimitive, false);
#endif
	PassSequence.SetEnabled(EPass::VisualizeShadingModels, EngineShowFlags.VisualizeShadingModels);
	PassSequence.SetEnabled(EPass::VisualizeGBufferHints, EngineShowFlags.GBufferHints);
	PassSequence.SetEnabled(EPass::VisualizeSubsurface, EngineShowFlags.VisualizeSSS);
	PassSequence.SetEnabled(EPass::VisualizeGBufferOverview, bVisualizeGBufferOverview || bVisualizeGBufferDumpToFile || bVisualizeGBufferDumpToPIpe);
	PassSequence.SetEnabled(EPass::VisualizeHDR, EngineShowFlags.VisualizeHDR);
#if WITH_EDITOR
	PassSequence.SetEnabled(EPass::PixelInspector, View.bUsePixelInspector);
#else
	PassSequence.SetEnabled(EPass::PixelInspector, false);
#endif
	PassSequence.SetEnabled(EPass::HMDDistortion, EngineShowFlags.StereoRendering && EngineShowFlags.HMDDistortion);
	PassSequence.SetEnabled(EPass::HighResolutionScreenshotMask, IsHighResolutionScreenshotMaskEnabled(View));
	PassSequence.SetEnabled(EPass::PrimaryUpscale, PaniniConfig.IsEnabled() || (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::SpatialUpscale && PrimaryViewRect.Size() != View.GetSecondaryViewRectSize()));
	PassSequence.SetEnabled(EPass::SecondaryUpscale, View.RequiresSecondaryUpscale() || View.Family->GetSecondarySpatialUpscalerInterface() != nullptr);

	const auto GetPostProcessMaterialInputs = [&](FScreenPassTexture InSceneColor)
	{ 
		FPostProcessMaterialInputs PostProcessMaterialInputs;

		PostProcessMaterialInputs.SetInput(EPostProcessMaterialInput::SceneColor, InSceneColor);
		PostProcessMaterialInputs.SetInput(EPostProcessMaterialInput::SeparateTranslucency, SeparateTranslucency);
		PostProcessMaterialInputs.SetInput(EPostProcessMaterialInput::Velocity, Velocity);
		PostProcessMaterialInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);
		PostProcessMaterialInputs.CustomDepthTexture = CustomDepth.Texture;

		return PostProcessMaterialInputs;
	};

	const auto AddAfterPass = [&](EPass InPass, FScreenPassTexture InSceneColor) -> FScreenPassTexture
	{
		// In some cases (e.g. OCIO color conversion) we want View Extensions to be able to add extra custom post processing after the pass.

		FAfterPassCallbackDelegateArray& PassCallbacks = PassSequence.GetAfterPassCallbacks(InPass);

		if (PassCallbacks.Num())
		{
			FPostProcessMaterialInputs InOutPostProcessAfterPassInputs = GetPostProcessMaterialInputs(InSceneColor);

			for (int32 AfterPassCallbackIndex = 0; AfterPassCallbackIndex < PassCallbacks.Num(); AfterPassCallbackIndex++)
			{
				FAfterPassCallbackDelegate& AfterPassCallback = PassCallbacks[AfterPassCallbackIndex];
				PassSequence.AcceptOverrideIfLastPass(InPass, InOutPostProcessAfterPassInputs.OverrideOutput, AfterPassCallbackIndex);
				InSceneColor = AfterPassCallback.Execute(GraphBuilder, View, InOutPostProcessAfterPassInputs);
			}
		}

		return MoveTemp(InSceneColor);
	};

	if (IsPostProcessingEnabled(View))
	{
		const bool bPrimaryView = IStereoRendering::IsAPrimaryView(View);
		const bool bHasViewState = View.ViewState != nullptr;
		const bool bDepthOfFieldEnabled = DiaphragmDOF::IsEnabled(View);
		const bool bVisualizeDepthOfField = bDepthOfFieldEnabled && EngineShowFlags.VisualizeDOF;
		const bool bVisualizeMotionBlur = IsVisualizeMotionBlurEnabled(View);

		const EAutoExposureMethod AutoExposureMethod = GetAutoExposureMethod(View);
		const EAntiAliasingMethod AntiAliasingMethod = !bVisualizeDepthOfField ? View.AntiAliasingMethod : AAM_None;
		const EDownsampleQuality DownsampleQuality = GetDownsampleQuality();
		const EPixelFormat DownsampleOverrideFormat = PF_FloatRGB;

		// Motion blur gets replaced by the visualization pass.
		const bool bMotionBlurEnabled = !bVisualizeMotionBlur && IsMotionBlurEnabled(View);

		// Skip tonemapping for visualizers which overwrite the HDR scene color.
		const bool bTonemapEnabled = !bVisualizeMotionBlur;
		const bool bTonemapOutputInHDR = View.Family->SceneCaptureSource == SCS_FinalColorHDR || View.Family->SceneCaptureSource == SCS_FinalToneCurveHDR || bOutputInHDR || bViewFamilyOutputInHDR;

		// We don't test for the EyeAdaptation engine show flag here. If disabled, the auto exposure pass is still executes but performs a clamp.
		const bool bEyeAdaptationEnabled =
			// Skip for transient views.
			bHasViewState &&
			// Skip for secondary views in a stereo setup.
			bPrimaryView;

		const bool bHistogramEnabled =
			// Force the histogram on when we are visualizing HDR.
			bVisualizeHDR ||
			// Skip if not using histogram eye adaptation.
			(bEyeAdaptationEnabled && AutoExposureMethod == EAutoExposureMethod::AEM_Histogram &&
			// Skip if we don't have any exposure range to generate (eye adaptation will clamp).
			View.FinalPostProcessSettings.AutoExposureMinBrightness < View.FinalPostProcessSettings.AutoExposureMaxBrightness);

		const bool bLocalExposureEnabled =
			EngineShowFlags.VisualizeLocalExposure || 
			View.FinalPostProcessSettings.LocalExposureContrastReduction < 1.0 ||
			!FMath::IsNearlyEqual(View.FinalPostProcessSettings.LocalExposureDetailStrength, 1.0f);

		const bool bBloomEnabled = View.FinalPostProcessSettings.BloomIntensity > 0.0f;


		// Temporal Anti-aliasing. Also may perform a temporal upsample from primary to secondary view rect.
		EMainTAAPassConfig TAAConfig = ITemporalUpscaler::GetMainTAAPassConfig(View);

		// Whether separate translucency is composed in TSR.
		bool bComposeSeparateTranslucencyInTSR = TAAConfig == EMainTAAPassConfig::TSR && ComposeSeparateTranslucencyInTSR(View);

		const FPostProcessMaterialChain PostProcessMaterialAfterTonemappingChain = GetPostProcessMaterialChain(View, BL_AfterTonemapping);

		PassSequence.SetEnabled(EPass::MotionBlur, bVisualizeMotionBlur || bMotionBlurEnabled);
		PassSequence.SetEnabled(EPass::Tonemap, bTonemapEnabled);
		PassSequence.SetEnabled(EPass::FXAA, AntiAliasingMethod == AAM_FXAA);
		PassSequence.SetEnabled(EPass::PostProcessMaterialAfterTonemapping, PostProcessMaterialAfterTonemappingChain.Num() != 0);
		PassSequence.SetEnabled(EPass::VisualizeDepthOfField, bVisualizeDepthOfField);
		PassSequence.SetEnabled(EPass::VisualizeLocalExposure, EngineShowFlags.VisualizeLocalExposure);

		for (int32 ViewExt = 0; ViewExt < View.Family->ViewExtensions.Num(); ++ViewExt)
		{
			for (int32 SceneViewPassId = 0; SceneViewPassId != static_cast<int>(ISceneViewExtension::EPostProcessingPass::MAX); SceneViewPassId++)
			{
				ISceneViewExtension::EPostProcessingPass SceneViewPass = static_cast<ISceneViewExtension::EPostProcessingPass>(SceneViewPassId);
				EPass PostProcessingPass = TranslatePass(SceneViewPass);

				View.Family->ViewExtensions[ViewExt]->SubscribeToPostProcessingPass(
					SceneViewPass,
					PassSequence.GetAfterPassCallbacks(PostProcessingPass),
					PassSequence.IsEnabled(PostProcessingPass));
			}
		}

		PassSequence.Finalize();

		// Post Process Material Chain - Before Translucency
		{
			const FPostProcessMaterialChain MaterialChain = GetPostProcessMaterialChain(View, BL_BeforeTranslucency);

			if (MaterialChain.Num())
			{
				SceneColor = AddPostProcessMaterialChain(GraphBuilder, View, GetPostProcessMaterialInputs(SceneColor), MaterialChain);
			}
		}

		// Diaphragm Depth of Field
		{
			FRDGTextureRef InputSceneColorTexture = SceneColor.Texture;

			if (bDepthOfFieldEnabled)
			{
				FSeparateTranslucencyDimensions DummyTranslucencyDimensions;
				FSeparateTranslucencyTextures DummyTranslucency(DummyTranslucencyDimensions);

				SceneColor.Texture = DiaphragmDOF::AddPasses(
					GraphBuilder,
					SceneTextureParameters,
					View,
					SceneColor.Texture,
					bComposeSeparateTranslucencyInTSR ? DummyTranslucencyDimensions : *Inputs.SeparateTranslucencyTextures);
			}

			// DOF passes were not added, therefore need to compose Separate translucency manually.
			if (SceneColor.Texture == InputSceneColorTexture || bComposeSeparateTranslucencyInTSR)
			{
				SceneColor.Texture = AddTranslucencyCompositionPass(
					GraphBuilder,
					View,
					SceneColor,
					SceneDepth,
					*Inputs.SeparateTranslucencyTextures,
					/* bPostMotionBlur = */ false,
					/* bApplyModulateOnly = */ bComposeSeparateTranslucencyInTSR);
			}

			if (GetHairStrandsComposition() == EHairStrandsCompositionType::AfterSeparateTranslucent)
			{
				RenderHairComposition(GraphBuilder, View, SceneColor.Texture, SceneDepth.Texture);
			}
		}

		// Post Process Material Chain - Before Tonemapping
		{
			const FPostProcessMaterialChain MaterialChain = GetPostProcessMaterialChain(View, BL_BeforeTonemapping);

			if (MaterialChain.Num())
			{
				SceneColor = AddPostProcessMaterialChain(GraphBuilder, View, GetPostProcessMaterialInputs(SceneColor), MaterialChain);
			}
		}

		FScreenPassTexture DownsampledSceneColor;

		// Scene color view rectangle after temporal AA upscale to secondary screen percentage.
		FIntRect SecondaryViewRect = PrimaryViewRect;

		if (TAAConfig != EMainTAAPassConfig::Disabled)
		{
			// Whether we allow the temporal AA pass to downsample scene color. It may choose not to based on internal context,
			// in which case the output half resolution texture will remain null.
			const bool bAllowSceneDownsample =
				IsTemporalAASceneDownsampleAllowed(View) &&
				// We can only merge if the normal downsample pass would happen immediately after.
				!bMotionBlurEnabled && !bVisualizeMotionBlur &&
				// TemporalAA is only able to match the low quality mode (box filter).
				GetDownsampleQuality() == EDownsampleQuality::Low;

			const ITemporalUpscaler* UpscalerToUse = (TAAConfig == EMainTAAPassConfig::ThirdParty) ? View.Family->GetTemporalUpscalerInterface() : ITemporalUpscaler::GetDefaultTemporalUpscaler();
			check(UpscalerToUse);
			const TCHAR* UpscalerName = UpscalerToUse->GetDebugName();

			// Standard event scope for temporal upscaler to have all profiling information not matter what,
			// and with explicit detection of third party.
			RDG_EVENT_SCOPE_CONDITIONAL(
				GraphBuilder,
				TAAConfig == EMainTAAPassConfig::ThirdParty,
				"ThirdParty %s %dx%d -> %dx%d",
				UpscalerToUse->GetDebugName(),
				View.ViewRect.Width(), View.ViewRect.Height(),
				View.GetSecondaryViewRectSize().X, View.GetSecondaryViewRectSize().Y);

			ITemporalUpscaler::FPassInputs UpscalerPassInputs;

			UpscalerPassInputs.bAllowDownsampleSceneColor = bAllowSceneDownsample;
			UpscalerPassInputs.DownsampleOverrideFormat = DownsampleOverrideFormat;
			UpscalerPassInputs.SceneColorTexture = SceneColor.Texture;
			UpscalerPassInputs.SceneDepthTexture = SceneDepth.Texture;
			UpscalerPassInputs.SceneVelocityTexture = Velocity.Texture;
			UpscalerPassInputs.SeparateTranslucencyTextures = Inputs.SeparateTranslucencyTextures;

			UpscalerToUse->AddPasses(
				GraphBuilder,
				View,
				UpscalerPassInputs,
				&SceneColor.Texture,
				&SecondaryViewRect,
				&DownsampledSceneColor.Texture,
				&DownsampledSceneColor.ViewRect);
		}
		else if (ScreenSpaceRayTracing::ShouldRenderScreenSpaceReflections(View))
		{
			// If we need SSR, and TAA is enabled, then AddTemporalAAPass() has already handled the scene history.
			// If we need SSR, and TAA is not enable, then we just need to extract the history.
			if (!View.bStatePrevViewInfoIsReadOnly)
			{
				check(View.ViewState);
				FTemporalAAHistory& OutputHistory = View.ViewState->PrevFrameViewInfo.TemporalAAHistory;
				GraphBuilder.QueueTextureExtraction(SceneColor.Texture, &OutputHistory.RT[0]);

				// For SSR, we still fill up the rest of the OutputHistory data using shared math from FTAAPassParameters.
				FTAAPassParameters TAAInputs(View);
				TAAInputs.SceneColorInput = SceneColor.Texture;
				TAAInputs.SetupViewRect(View);
				OutputHistory.ViewportRect = TAAInputs.OutputViewRect;
				OutputHistory.ReferenceBufferSize = TAAInputs.GetOutputExtent() * TAAInputs.ResolutionDivisor;
			}
		}

		//! SceneColorTexture is now upsampled to the SecondaryViewRect. Use SecondaryViewRect for input / output.
		SceneColor.ViewRect = SecondaryViewRect;

		// Post Process Material Chain - SSR Input
		if (View.ViewState && !View.bStatePrevViewInfoIsReadOnly)
		{
			const FPostProcessMaterialChain MaterialChain = GetPostProcessMaterialChain(View, BL_SSRInput);

			if (MaterialChain.Num())
			{
				// Save off SSR post process output for the next frame.
				FScreenPassTexture PassOutput = AddPostProcessMaterialChain(GraphBuilder, View, GetPostProcessMaterialInputs(SceneColor), MaterialChain);
				GraphBuilder.QueueTextureExtraction(PassOutput.Texture, &View.ViewState->PrevFrameViewInfo.CustomSSRInput.RT[0]);

				View.ViewState->PrevFrameViewInfo.CustomSSRInput.ViewportRect = PassOutput.ViewRect;
				View.ViewState->PrevFrameViewInfo.CustomSSRInput.ReferenceBufferSize = PassOutput.Texture->Desc.Extent;
			}
		}

		if (PassSequence.IsEnabled(EPass::MotionBlur))
		{
			FMotionBlurInputs PassInputs;
			PassSequence.AcceptOverrideIfLastPass(EPass::MotionBlur, PassInputs.OverrideOutput);
			PassInputs.SceneColor = SceneColor;
			PassInputs.SceneDepth = SceneDepth;
			PassInputs.SceneVelocity = Velocity;
			PassInputs.PostMotionBlurTranslucency = PostMotionBlurTranslucency;
			PassInputs.Quality = GetMotionBlurQuality();
			PassInputs.Filter = GetMotionBlurFilter();

			// Motion blur visualization replaces motion blur when enabled.
			if (bVisualizeMotionBlur)
			{
				SceneColor = AddVisualizeMotionBlurPass(GraphBuilder, View, PassInputs);
			}
			else
			{
				SceneColor = AddMotionBlurPass(GraphBuilder, View, PassInputs);
			}
		}
		else
		{
			// Compose Post-MotionBlur translucency

			SceneColor.Texture = AddTranslucencyCompositionPass(
				GraphBuilder,
				View,
				SceneColor,
				SceneDepth,
				*Inputs.SeparateTranslucencyTextures,
				/* bPostMotionBlur = */ true,
				/* bApplyModulateOnly = */ false);
		}

		SceneColor = AddAfterPass(EPass::MotionBlur, SceneColor);

		// If TAA didn't do it, downsample the scene color texture by half.
		if (!DownsampledSceneColor.IsValid())
		{
			FDownsamplePassInputs PassInputs;
			PassInputs.Name = TEXT("HalfResolutionSceneColor");
			PassInputs.SceneColor = SceneColor;
			PassInputs.Quality = DownsampleQuality;
			PassInputs.FormatOverride = DownsampleOverrideFormat;
			PassInputs.UserSuppliedOutput = View.PrevViewInfo.HalfResTemporalAAHistory;

			DownsampledSceneColor = AddDownsamplePass(GraphBuilder, View, PassInputs);
		}

		if (IsPostProcessingQuarterResolutionDownsampleEnabled())
		{
			FDownsamplePassInputs PassInputs;
			PassInputs.Name = TEXT("QuarterResolutionSceneColor");
			PassInputs.SceneColor = DownsampledSceneColor;
			PassInputs.Quality = DownsampleQuality;

			DownsampledSceneColor = AddDownsamplePass(GraphBuilder, View, PassInputs);
		}

		// Store half res scene color in the history
		extern int32 GSSRHalfResSceneColor;
		if (ScreenSpaceRayTracing::ShouldRenderScreenSpaceReflections(View) && !View.bStatePrevViewInfoIsReadOnly && GSSRHalfResSceneColor)
		{
			check(View.ViewState);
			GraphBuilder.QueueTextureExtraction(DownsampledSceneColor.Texture, &View.ViewState->PrevFrameViewInfo.HalfResTemporalAAHistory);
		}

		if (bLocalExposureEnabled)
		{
			LocalExposureTexture = AddLocalExposurePass(GraphBuilder, View, EyeAdaptationParameters, DownsampledSceneColor);
		}

		if (bHistogramEnabled)
		{
			HistogramTexture = AddHistogramPass(GraphBuilder, View, EyeAdaptationParameters, DownsampledSceneColor, LastEyeAdaptationTexture);
		}

		const bool bFFTBloomEnabled = IsFFTBloomEnabled(View);
		const bool bBasicEyeAdaptationEnabled = bEyeAdaptationEnabled && (AutoExposureMethod == EAutoExposureMethod::AEM_Basic);
		const bool bLocalExposureBlurredLum = bLocalExposureEnabled && View.FinalPostProcessSettings.LocalExposureBlurredLuminanceBlend > 0.0f;

		FSceneDownsampleChain SceneDownsampleChain;
		if (bBasicEyeAdaptationEnabled || (bBloomEnabled && bFFTBloomEnabled) || bLocalExposureBlurredLum)
		{
			const bool bLogLumaInAlpha = true;
			SceneDownsampleChain.Init(GraphBuilder, View, EyeAdaptationParameters, DownsampledSceneColor, DownsampleQuality, bLogLumaInAlpha);
		}

		if (bLocalExposureBlurredLum)
		{
			LocalExposureBlurredLogLumTexture = AddLocalExposureBlurredLogLuminancePass(GraphBuilder, View, EyeAdaptationParameters, SceneDownsampleChain.GetTexture(4));
		}

		if (bBasicEyeAdaptationEnabled)
		{
			// Use the alpha channel in the last downsample (smallest) to compute eye adaptations values.
			EyeAdaptationTexture = AddBasicEyeAdaptationPass(GraphBuilder, View, EyeAdaptationParameters, SceneDownsampleChain.GetLastTexture(), LastEyeAdaptationTexture);
		}
		// Add histogram eye adaptation pass even if no histogram exists to support the manual clamping mode.
		else if (bEyeAdaptationEnabled)
		{
			EyeAdaptationTexture = AddHistogramEyeAdaptationPass(GraphBuilder, View, EyeAdaptationParameters, HistogramTexture);
		}

		FBloomOutputs Bloom;

		if (bBloomEnabled)
		{
			FSceneDownsampleChain BloomDownsampleChain;

			const bool bBloomThresholdEnabled = View.FinalPostProcessSettings.BloomThreshold > -1.0f;

			// Reuse the main scene downsample chain if a threshold isn't required for gaussian bloom.
			const FSceneDownsampleChain* GaussianBloomSceneDownsampleChain;
			if (SceneDownsampleChain.IsInitialized() && !bBloomThresholdEnabled)
			{
				GaussianBloomSceneDownsampleChain = &SceneDownsampleChain;
			}
			else
			{
				FScreenPassTexture DownsampleInput = DownsampledSceneColor;

				if (bBloomThresholdEnabled)
				{
					const float BloomThreshold = View.FinalPostProcessSettings.BloomThreshold;

					FBloomSetupInputs SetupPassInputs;
					SetupPassInputs.SceneColor = DownsampleInput;
					SetupPassInputs.EyeAdaptationTexture = EyeAdaptationTexture;
					SetupPassInputs.Threshold = BloomThreshold;

					DownsampleInput = AddBloomSetupPass(GraphBuilder, View, SetupPassInputs);
				}

				const bool bLogLumaInAlpha = false;
				BloomDownsampleChain.Init(GraphBuilder, View, EyeAdaptationParameters, DownsampleInput, DownsampleQuality, bLogLumaInAlpha);

				GaussianBloomSceneDownsampleChain = &BloomDownsampleChain;
			}

			if (bFFTBloomEnabled)
			{
				FScreenPassTexture HalfResolution;
				if (IsFFTBloomQuarterResolutionEnabled())
				{
					HalfResolution = SceneDownsampleChain.GetTexture(1);
				}
				else
				{
					HalfResolution = SceneDownsampleChain.GetFirstTexture();
				}

				FFFTBloomInputs PassInputs;
				PassInputs.FullResolutionTexture = SceneColor.Texture;
				PassInputs.FullResolutionViewRect = SceneColor.ViewRect;
				PassInputs.HalfResolutionTexture = HalfResolution.Texture;
				PassInputs.HalfResolutionViewRect = HalfResolution.ViewRect;

				Bloom = AddFFTBloomPass(GraphBuilder, View, PassInputs);
			}
			else
			{
				Bloom = AddGaussianBloomPasses(GraphBuilder, View, GaussianBloomSceneDownsampleChain);
			}

			FScreenPassTexture LensFlares = AddLensFlaresPass(GraphBuilder, View, Bloom.Bloom, *GaussianBloomSceneDownsampleChain);

			if (LensFlares.IsValid())
			{
				// Lens flares are composited with bloom.
				Bloom.Bloom = LensFlares;
			}
		}

		SceneColorBeforeTonemap = SceneColor;

		if (PassSequence.IsEnabled(EPass::Tonemap))
		{
			const FPostProcessMaterialChain MaterialChain = GetPostProcessMaterialChain(View, BL_ReplacingTonemapper);

			if (MaterialChain.Num())
			{
				const UMaterialInterface* HighestPriorityMaterial = MaterialChain[0];

				FPostProcessMaterialInputs PassInputs;
				PassSequence.AcceptOverrideIfLastPass(EPass::Tonemap, PassInputs.OverrideOutput);
				PassInputs.SetInput(EPostProcessMaterialInput::SceneColor, SceneColor);
				PassInputs.SetInput(EPostProcessMaterialInput::SeparateTranslucency, SeparateTranslucency);
				PassInputs.SetInput(EPostProcessMaterialInput::CombinedBloom, Bloom.Bloom);
				PassInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);
				PassInputs.CustomDepthTexture = CustomDepth.Texture;

				SceneColor = AddPostProcessMaterialPass(GraphBuilder, View, PassInputs, HighestPriorityMaterial);
			}
			else
			{
				FRDGTextureRef ColorGradingTexture = nullptr;

				if (bPrimaryView)
				{
					ColorGradingTexture = AddCombineLUTPass(GraphBuilder, View);
				}
				// We can re-use the color grading texture from the primary view.
				else if (View.GetTonemappingLUT())
				{
					ColorGradingTexture = TryRegisterExternalTexture(GraphBuilder, View.GetTonemappingLUT());
				}
				else
				{
					const FViewInfo* PrimaryView = static_cast<const FViewInfo*>(View.Family->Views[0]);
					ColorGradingTexture = TryRegisterExternalTexture(GraphBuilder, PrimaryView->GetTonemappingLUT());
				}

				FTonemapInputs PassInputs;
				PassSequence.AcceptOverrideIfLastPass(EPass::Tonemap, PassInputs.OverrideOutput);
				PassInputs.SceneColor = SceneColor;
				PassInputs.Bloom = Bloom;
				PassInputs.LocalExposureTexture = LocalExposureTexture;
				PassInputs.BlurredLogLuminanceTexture = LocalExposureBlurredLogLumTexture;
				PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;
				PassInputs.EyeAdaptationTexture = EyeAdaptationTexture;
				PassInputs.ColorGradingTexture = ColorGradingTexture;
				PassInputs.bWriteAlphaChannel = AntiAliasingMethod == AAM_FXAA || IsPostProcessingWithAlphaChannelSupported();
				PassInputs.bOutputInHDR = bTonemapOutputInHDR;

				SceneColor = AddTonemapPass(GraphBuilder, View, PassInputs);
			}
		}
		
		SceneColor = AddAfterPass(EPass::Tonemap, SceneColor);

		SceneColorAfterTonemap = SceneColor;

		if (PassSequence.IsEnabled(EPass::FXAA))
		{
			FFXAAInputs PassInputs;
			PassSequence.AcceptOverrideIfLastPass(EPass::FXAA, PassInputs.OverrideOutput);
			PassInputs.SceneColor = SceneColor;
			PassInputs.Quality = GetFXAAQuality();

			SceneColor = AddFXAAPass(GraphBuilder, View, PassInputs);
		}

		SceneColor = AddAfterPass(EPass::FXAA, SceneColor);

		// Post Process Material Chain - After Tonemapping
		if (PassSequence.IsEnabled(EPass::PostProcessMaterialAfterTonemapping))
		{
			FPostProcessMaterialInputs PassInputs = GetPostProcessMaterialInputs(SceneColor);
			PassSequence.AcceptOverrideIfLastPass(EPass::PostProcessMaterialAfterTonemapping, PassInputs.OverrideOutput);
			PassInputs.SetInput(EPostProcessMaterialInput::PreTonemapHDRColor, SceneColorBeforeTonemap);
			PassInputs.SetInput(EPostProcessMaterialInput::PostTonemapHDRColor, SceneColorAfterTonemap);
			PassInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);

			SceneColor = AddPostProcessMaterialChain(GraphBuilder, View, PassInputs, PostProcessMaterialAfterTonemappingChain);
		}

		if (PassSequence.IsEnabled(EPass::VisualizeDepthOfField))
		{
			FVisualizeDOFInputs PassInputs;
			PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeDepthOfField, PassInputs.OverrideOutput);
			PassInputs.SceneColor = SceneColor;
			PassInputs.SceneDepth = SceneDepth;

			SceneColor = AddVisualizeDOFPass(GraphBuilder, View, PassInputs);
		}

		SceneColor = AddAfterPass(EPass::VisualizeDepthOfField, SceneColor);
	}
	// Minimal PostProcessing - Separate translucency composition and gamma-correction only.
	else
	{
		PassSequence.SetEnabled(EPass::MotionBlur, false);
		PassSequence.SetEnabled(EPass::Tonemap, true);
		PassSequence.SetEnabled(EPass::FXAA, false);
		PassSequence.SetEnabled(EPass::PostProcessMaterialAfterTonemapping, false);
		PassSequence.SetEnabled(EPass::VisualizeDepthOfField, false);
		PassSequence.SetEnabled(EPass::VisualizeLocalExposure, false);
		PassSequence.Finalize();

		// Compose separate translucency passes
		SceneColor.Texture = AddTranslucencyCompositionPass(
			GraphBuilder,
			View,
			SceneColor,
			SceneDepth,
			*Inputs.SeparateTranslucencyTextures,
			/* bPostMotionBlur = */ false,
			/* bApplyModulateOnly = */ false);
		SceneColor.Texture = AddTranslucencyCompositionPass(
			GraphBuilder,
			View,
			SceneColor,
			SceneDepth,
			*Inputs.SeparateTranslucencyTextures,
			/* bPostMotionBlur = */ true,
			/* bApplyModulateOnly = */ false);

		SceneColorBeforeTonemap = SceneColor;

		if (PassSequence.IsEnabled(EPass::Tonemap))
		{
			FTonemapInputs PassInputs;
			PassSequence.AcceptOverrideIfLastPass(EPass::Tonemap, PassInputs.OverrideOutput);
			PassInputs.SceneColor = SceneColor;
			PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;
			PassInputs.EyeAdaptationTexture = EyeAdaptationTexture;
			PassInputs.bOutputInHDR = bViewFamilyOutputInHDR;
			PassInputs.bGammaOnly = true;

			SceneColor = AddTonemapPass(GraphBuilder, View, PassInputs);
		}

		SceneColor = AddAfterPass(EPass::Tonemap, SceneColor);

		SceneColorAfterTonemap = SceneColor;
	}

	if (PassSequence.IsEnabled(EPass::VisualizeStationaryLightOverlap))
	{
		ensureMsgf(View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("TAAU should be disabled when visualizing stationary light overlap."));

		FVisualizeComplexityInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeStationaryLightOverlap, PassInputs.OverrideOutput);
		PassInputs.SceneColor = OriginalSceneColor;
		PassInputs.Colors = GEngine->StationaryLightOverlapColors;
		PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Ramp;
		PassInputs.bDrawLegend = true;

		SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeLightCulling))
	{
		ensureMsgf(View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("TAAU should be disabled when visualizing light culling."));

		// 0.1f comes from the values used in LightAccumulator_GetResult
		const float ComplexityScale = 1.0f / (float)(GEngine->LightComplexityColors.Num() - 1) / 0.1f;

		FVisualizeComplexityInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeLightCulling, PassInputs.OverrideOutput);
		PassInputs.SceneColor = OriginalSceneColor;
		PassInputs.Colors = GEngine->LightComplexityColors;
		PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Linear;
		PassInputs.ComplexityScale = ComplexityScale;
		PassInputs.bDrawLegend = true;

		SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
	}

#if DEBUG_POST_PROCESS_VOLUME_ENABLE
	if (PassSequence.IsEnabled(EPass::VisualizePostProcessStack))
	{
		FScreenPassRenderTarget OverrideOutput;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizePostProcessStack, OverrideOutput);
		OverrideOutput = OverrideOutput.IsValid() ? OverrideOutput : FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("VisualizePostProcessStack"));
		SceneColor = AddFinalPostProcessDebugInfoPasses(GraphBuilder, View, OverrideOutput);
	}
#endif

	if (PassSequence.IsEnabled(EPass::VisualizeStrata))
	{
		FScreenPassRenderTarget OverrideOutput;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeStrata, OverrideOutput);
		SceneColor = Strata::AddStrataDebugPasses(GraphBuilder, View, SceneColor);
	}

#if WITH_EDITOR
	if (PassSequence.IsEnabled(EPass::VisualizeSkyAtmosphere))
	{
		FScreenPassRenderTarget OverrideOutput;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeSkyAtmosphere, OverrideOutput);
		OverrideOutput = OverrideOutput.IsValid() ? OverrideOutput : FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("VisualizeSkyAtmosphere"));
		SceneColor = AddSkyAtmosphereDebugPasses(GraphBuilder, Scene, *View.Family, View, OverrideOutput);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeLevelInstance))
	{
		FVisualizeLevelInstanceInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeLevelInstance, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;
		PassInputs.SceneTextures.SceneTextures = Inputs.SceneTextures;

		SceneColor = AddVisualizeLevelInstancePass(GraphBuilder, View, PassInputs, NaniteRasterResults);
	}
	
	if (PassSequence.IsEnabled(EPass::SelectionOutline))
	{
		FSelectionOutlineInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::SelectionOutline, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;
		PassInputs.SceneTextures.SceneTextures = Inputs.SceneTextures;

		SceneColor = AddSelectionOutlinePass(GraphBuilder, View, PassInputs, NaniteRasterResults);
	}

	if (PassSequence.IsEnabled(EPass::EditorPrimitive))
	{
		FEditorPrimitiveInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::EditorPrimitive, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;
		PassInputs.BasePassType = FEditorPrimitiveInputs::EBasePassType::Deferred;

		SceneColor = AddEditorPrimitivePass(GraphBuilder, View, PassInputs, InstanceCullingManager);
	}
#endif

	if (PassSequence.IsEnabled(EPass::VisualizeShadingModels))
	{
		ensureMsgf(View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("TAAU should be disabled when visualizing shading models."));

		FVisualizeShadingModelInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeShadingModels, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneTextures = Inputs.SceneTextures;

		SceneColor = AddVisualizeShadingModelPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeGBufferHints))
	{
		ensureMsgf(View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("TAAU should be disabled when visualizing gbuffer hints."));

		FVisualizeGBufferHintsInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeGBufferHints, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.OriginalSceneColor = OriginalSceneColor;
		PassInputs.SceneTextures = Inputs.SceneTextures;

		SceneColor = AddVisualizeGBufferHintsPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeSubsurface))
	{
		ensureMsgf(View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("TAAU should be disabled when visualizing subsurface."));

		FVisualizeSubsurfaceInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeSubsurface, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneTextures = Inputs.SceneTextures;

		SceneColor = AddVisualizeSubsurfacePass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeGBufferOverview))
	{
		FVisualizeGBufferOverviewInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeGBufferOverview, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneColorBeforeTonemap = SceneColorBeforeTonemap;
		PassInputs.SceneColorAfterTonemap = SceneColorAfterTonemap;
		PassInputs.SeparateTranslucency = SeparateTranslucency;
		PassInputs.Velocity = Velocity;
		PassInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);
		PassInputs.bOverview = bVisualizeGBufferOverview;
		PassInputs.bDumpToFile = bVisualizeGBufferDumpToFile;
		PassInputs.bOutputInHDR = bOutputInHDR;

		SceneColor = AddVisualizeGBufferOverviewPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeHDR))
	{
		FVisualizeHDRInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeHDR, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneColorBeforeTonemap = SceneColorBeforeTonemap;
		PassInputs.HistogramTexture = HistogramTexture;
		PassInputs.EyeAdaptationTexture = EyeAdaptationTexture;
		PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;

		SceneColor = AddVisualizeHDRPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::VisualizeLocalExposure))
	{
		FVisualizeLocalExposureInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::VisualizeLocalExposure, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.HDRSceneColor = SceneColorBeforeTonemap;
		PassInputs.LumBilateralGridTexture = LocalExposureTexture;
		PassInputs.BlurredLumTexture = LocalExposureBlurredLogLumTexture;
		PassInputs.EyeAdaptationTexture = EyeAdaptationTexture;
		PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;

		SceneColor = AddVisualizeLocalExposurePass(GraphBuilder, View, PassInputs);
	}

#if WITH_EDITOR
	if (PassSequence.IsEnabled(EPass::PixelInspector))
	{
		FPixelInspectorInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::PixelInspector, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneColorBeforeTonemap = SceneColorBeforeTonemap;
		PassInputs.OriginalSceneColor = OriginalSceneColor;

		SceneColor = AddPixelInspectorPass(GraphBuilder, View, PassInputs);
	}
#endif

	if (PassSequence.IsEnabled(EPass::HMDDistortion))
	{
		FHMDDistortionInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::HMDDistortion, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;

		SceneColor = AddHMDDistortionPass(GraphBuilder, View, PassInputs);
	}

	if (EngineShowFlags.TestImage)
	{
		AddTestImagePass(GraphBuilder, View, SceneColor);
	}

	if (EngineShowFlags.VisualizeNanite && NaniteRasterResults != nullptr)
	{
		AddVisualizeNanitePass(GraphBuilder, View, SceneColor, *NaniteRasterResults);
	}

	if (ShaderDrawDebug::IsEnabled(View))
	{
		ShaderDrawDebug::DrawView(GraphBuilder, View, SceneColor.Texture, SceneDepth.Texture);
	}

	if (ShaderPrint::IsEnabled(View))
	{
		ShaderPrint::DrawView(GraphBuilder, View, SceneColor.Texture);
	}

	if ( View.Family && View.Family->Scene )
	{
		if (FFXSystemInterface* FXSystem = View.Family->Scene->GetFXSystem())
		{
			FXSystem->DrawSceneDebug_RenderThread(GraphBuilder, View, SceneColor.Texture, SceneDepth.Texture);
		}
	}

	if (PassSequence.IsEnabled(EPass::HighResolutionScreenshotMask))
	{
		FHighResolutionScreenshotMaskInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::HighResolutionScreenshotMask, PassInputs.OverrideOutput);
		PassInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);
		PassInputs.SceneColor = SceneColor;
		PassInputs.Material = View.FinalPostProcessSettings.HighResScreenshotMaterial;
		PassInputs.MaskMaterial = View.FinalPostProcessSettings.HighResScreenshotMaskMaterial;
		PassInputs.CaptureRegionMaterial = View.FinalPostProcessSettings.HighResScreenshotCaptureRegionMaterial;

		SceneColor = AddHighResolutionScreenshotMaskPass(GraphBuilder, View, PassInputs);
	}

	if (PassSequence.IsEnabled(EPass::PrimaryUpscale))
	{
		ISpatialUpscaler::FInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::PrimaryUpscale, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.Stage = PassSequence.IsEnabled(EPass::SecondaryUpscale) ? EUpscaleStage::PrimaryToSecondary : EUpscaleStage::PrimaryToOutput;

		const ISpatialUpscaler* CustomUpscaler = View.Family ? View.Family->GetPrimarySpatialUpscalerInterface() : nullptr;
		if (CustomUpscaler)
		{
			RDG_EVENT_SCOPE(
				GraphBuilder,
				"ThirdParty PrimaryUpscale %s %dx%d -> %dx%d",
				CustomUpscaler->GetDebugName(),
				SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
				View.GetSecondaryViewRectSize().X, View.GetSecondaryViewRectSize().Y);

			SceneColor = CustomUpscaler->AddPasses(GraphBuilder, View, PassInputs);

			if (PassSequence.IsLastPass(EPass::PrimaryUpscale))
			{
				check(SceneColor == ViewFamilyOutput);
			}
			else
			{
				check(SceneColor.ViewRect.Size() == View.GetSecondaryViewRectSize());
			}
		}
		else
		{
			EUpscaleMethod Method = GetUpscaleMethod();

			SceneColor = ISpatialUpscaler::AddDefaultUpscalePass(GraphBuilder, View, PassInputs, Method, PaniniConfig);
		}
	}

	if (PassSequence.IsEnabled(EPass::SecondaryUpscale))
	{
		ISpatialUpscaler::FInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::SecondaryUpscale, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.Stage = EUpscaleStage::SecondaryToOutput;

		const ISpatialUpscaler* CustomUpscaler = View.Family ? View.Family->GetSecondarySpatialUpscalerInterface() : nullptr;
		if (CustomUpscaler)
		{
			RDG_EVENT_SCOPE(
				GraphBuilder,
				"ThirdParty SecondaryUpscale %s %dx%d -> %dx%d",
				CustomUpscaler->GetDebugName(),
				SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
				View.UnscaledViewRect.Width(), View.UnscaledViewRect.Height());

			SceneColor = CustomUpscaler->AddPasses(GraphBuilder, View, PassInputs);
			check(SceneColor == ViewFamilyOutput);
		}
		else
		{
			EUpscaleMethod Method = View.Family && View.Family->SecondaryScreenPercentageMethod == ESecondaryScreenPercentageMethod::LowerPixelDensitySimulation
				? EUpscaleMethod::SmoothStep
				: EUpscaleMethod::Nearest;

			SceneColor = ISpatialUpscaler::AddDefaultUpscalePass(GraphBuilder, View, PassInputs, Method, FPaniniProjectionConfig());
		}
	}
}

void AddDebugViewPostProcessingPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FPostProcessingInputs& Inputs, const Nanite::FRasterResults* NaniteRasterResults)
{
	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderPostProcessing);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_PostProcessing_Process);

	check(IsInRenderingThread());
	check(View.VerifyMembersChecks());
	Inputs.Validate();

	const FIntRect PrimaryViewRect = View.ViewRect;

	const FSceneTextureParameters SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, Inputs.SceneTextures);

	const FScreenPassRenderTarget ViewFamilyOutput = FScreenPassRenderTarget::CreateViewFamilyOutput(Inputs.ViewFamilyTexture, View);
	const FScreenPassTexture SceneDepth(SceneTextureParameters.SceneDepthTexture, PrimaryViewRect);
	FScreenPassTexture SceneColor((*Inputs.SceneTextures)->SceneColorTexture, PrimaryViewRect);

	// Some view modes do not actually output a color so they should not be tonemapped.
	const bool bTonemapAfter = View.Family->EngineShowFlags.RayTracingDebug;
	const bool bTonemapBefore = !bTonemapAfter && !View.Family->EngineShowFlags.ShaderComplexity;
	const bool bViewFamilyOutputInHDR = GRHISupportsHDROutput && IsHDREnabled();

	enum class EPass : uint32
	{
		Visualize,
		TonemapAfter,
		SelectionOutline,
		PrimaryUpscale,
		SecondaryUpscale,
		MAX
	};

	const TCHAR* PassNames[] =
	{
		TEXT("Visualize"),
		TEXT("TonemapAfter"),
		TEXT("SelectionOutline"),
		TEXT("PrimaryUpscale"),
		TEXT("SecondaryUpscale")
	};

	static_assert(static_cast<uint32>(EPass::MAX) == UE_ARRAY_COUNT(PassNames), "EPass does not match PassNames.");

	TOverridePassSequence<EPass> PassSequence(ViewFamilyOutput);
	PassSequence.SetNames(PassNames, UE_ARRAY_COUNT(PassNames));
	PassSequence.SetEnabled(EPass::Visualize, true);
	PassSequence.SetEnabled(EPass::TonemapAfter, bTonemapAfter);
	PassSequence.SetEnabled(EPass::SelectionOutline, GIsEditor);
	PassSequence.SetEnabled(EPass::PrimaryUpscale, View.ViewRect.Size() != View.GetSecondaryViewRectSize() && View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale);
	PassSequence.SetEnabled(EPass::SecondaryUpscale, View.RequiresSecondaryUpscale() || View.Family->GetSecondarySpatialUpscalerInterface() != nullptr);
	PassSequence.Finalize();

	if (bTonemapBefore)
	{
		const FEyeAdaptationParameters EyeAdaptationParameters = GetEyeAdaptationParameters(View, ERHIFeatureLevel::SM5);

		FTonemapInputs PassInputs;
		PassInputs.SceneColor = SceneColor;
		PassInputs.bOutputInHDR = bViewFamilyOutputInHDR;
		PassInputs.bGammaOnly = true;
		PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;

		SceneColor = AddTonemapPass(GraphBuilder, View, PassInputs);
	}

	check(PassSequence.IsEnabled(EPass::Visualize));
	{

		FScreenPassRenderTarget OverrideOutput;
		PassSequence.AcceptOverrideIfLastPass(EPass::Visualize, OverrideOutput);

		switch (View.Family->GetDebugViewShaderMode())
		{
		case DVSM_QuadComplexity:
		{
			float ComplexityScale = 1.f / (float)(GEngine->QuadComplexityColors.Num() - 1) / NormalizedQuadComplexityValue; // .1f comes from the values used in LightAccumulator_GetResult

			FVisualizeComplexityInputs PassInputs;
			PassInputs.OverrideOutput = OverrideOutput;
			PassInputs.SceneColor = SceneColor;
			PassInputs.Colors = GEngine->QuadComplexityColors;
			PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Stair;
			PassInputs.ComplexityScale = ComplexityScale;
			PassInputs.bDrawLegend = true;

			SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
			break;
		}
		case DVSM_ShaderComplexity:
		case DVSM_ShaderComplexityContainedQuadOverhead:
		case DVSM_ShaderComplexityBleedingQuadOverhead:
		{
			FVisualizeComplexityInputs PassInputs;
			PassInputs.OverrideOutput = OverrideOutput;
			PassInputs.SceneColor = SceneColor;
			PassInputs.Colors = GEngine->ShaderComplexityColors;
			PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Ramp;
			PassInputs.ComplexityScale = 1.0f;
			PassInputs.bDrawLegend = true;

			SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
			break;
		}
		case DVSM_PrimitiveDistanceAccuracy:
		case DVSM_MeshUVDensityAccuracy:
		case DVSM_MaterialTextureScaleAccuracy:
		case DVSM_RequiredTextureResolution:
		{
			FStreamingAccuracyLegendInputs PassInputs;
			PassInputs.OverrideOutput = OverrideOutput;
			PassInputs.SceneColor = SceneColor;
			PassInputs.Colors = GEngine->StreamingAccuracyColors;

			SceneColor = AddStreamingAccuracyLegendPass(GraphBuilder, View, PassInputs);
			break;
		}
		case DVSM_RayTracingDebug:
		{
			FTAAPassParameters Parameters(View);
			Parameters.SceneDepthTexture = SceneTextureParameters.SceneDepthTexture;
			Parameters.SceneVelocityTexture = SceneTextureParameters.GBufferVelocityTexture;
			Parameters.SceneColorInput = SceneColor.Texture;
			Parameters.Pass = View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale
				? ETAAPassConfig::MainUpsampling
				: ETAAPassConfig::Main;
			Parameters.SetupViewRect(View);

			const FTemporalAAHistory& InputHistory = View.PrevViewInfo.TemporalAAHistory;
			FTemporalAAHistory* OutputHistory = &View.ViewState->PrevFrameViewInfo.TemporalAAHistory;

			FTAAOutputs Outputs = AddTemporalAAPass(GraphBuilder, View, Parameters, InputHistory, OutputHistory);
			SceneColor.Texture = Outputs.SceneColor;
			SceneColor.ViewRect = Parameters.OutputViewRect;

			break;
		}
		case DVSM_LODColoration:
			break;
		default:
			ensure(false);
			break;
		}
	}

	if (PassSequence.IsEnabled(EPass::TonemapAfter))
	{
		const FEyeAdaptationParameters EyeAdaptationParameters = GetEyeAdaptationParameters(View, ERHIFeatureLevel::SM5);

		FTonemapInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::TonemapAfter, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.bOutputInHDR = bViewFamilyOutputInHDR;
		PassInputs.bGammaOnly = true;
		// Do eye adaptation in ray tracing debug modes to match raster buffer visualization modes
		PassInputs.EyeAdaptationParameters = &EyeAdaptationParameters;
		PassInputs.EyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);

		SceneColor = AddTonemapPass(GraphBuilder, View, PassInputs);
	}

#if WITH_EDITOR
	if (PassSequence.IsEnabled(EPass::SelectionOutline))
	{
		FSelectionOutlineInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::SelectionOutline, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;

		SceneColor = AddSelectionOutlinePass(GraphBuilder, View, PassInputs, NaniteRasterResults);
	}
#endif

	if (PassSequence.IsEnabled(EPass::PrimaryUpscale))
	{
		ISpatialUpscaler::FInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::PrimaryUpscale, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.Stage = PassSequence.IsEnabled(EPass::SecondaryUpscale) ? EUpscaleStage::PrimaryToSecondary : EUpscaleStage::PrimaryToOutput;

		if (const ISpatialUpscaler* CustomUpscaler = View.Family->GetPrimarySpatialUpscalerInterface())
		{
			RDG_EVENT_SCOPE(
				GraphBuilder,
				"ThirdParty PrimaryUpscale %s %dx%d -> %dx%d",
				CustomUpscaler->GetDebugName(),
				SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
				View.GetSecondaryViewRectSize().X, View.GetSecondaryViewRectSize().Y);

			SceneColor = CustomUpscaler->AddPasses(GraphBuilder, View, PassInputs);

			if (PassSequence.IsLastPass(EPass::PrimaryUpscale))
			{
				check(SceneColor == ViewFamilyOutput);
			}
			else
			{
				check(SceneColor.ViewRect.Size() == View.GetSecondaryViewRectSize());
			}
		}
		else
		{
			EUpscaleMethod Method = GetUpscaleMethod();

			SceneColor = ISpatialUpscaler::AddDefaultUpscalePass(GraphBuilder, View, PassInputs, Method, FPaniniProjectionConfig());
		}
	}

	if (PassSequence.IsEnabled(EPass::SecondaryUpscale))
	{
		ISpatialUpscaler::FInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::SecondaryUpscale, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.Stage = EUpscaleStage::SecondaryToOutput;

		if (const ISpatialUpscaler* CustomUpscaler = View.Family->GetSecondarySpatialUpscalerInterface())
		{
			RDG_EVENT_SCOPE(
				GraphBuilder,
				"ThirdParty SecondaryUpscale %s %dx%d -> %dx%d",
				CustomUpscaler->GetDebugName(),
				SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
				View.UnscaledViewRect.Width(), View.UnscaledViewRect.Height());

			SceneColor = CustomUpscaler->AddPasses(GraphBuilder, View, PassInputs);
			check(SceneColor == ViewFamilyOutput);
		}
		else
		{
			EUpscaleMethod Method = View.Family->SecondaryScreenPercentageMethod == ESecondaryScreenPercentageMethod::LowerPixelDensitySimulation
				? EUpscaleMethod::SmoothStep
				: EUpscaleMethod::Nearest;

			SceneColor = ISpatialUpscaler::AddDefaultUpscalePass(GraphBuilder, View, PassInputs, Method, FPaniniProjectionConfig());
		}
	}
}

#if !(UE_BUILD_SHIPPING)

void AddVisualizeCalibrationMaterialPostProcessingPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FPostProcessingInputs& Inputs, const UMaterialInterface* InMaterialInterface)
{
	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderPostProcessing);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_PostProcessing_Process);

	check(IsInRenderingThread());
	check(View.VerifyMembersChecks());
	check(InMaterialInterface);
	Inputs.Validate();

	const FIntRect PrimaryViewRect = View.ViewRect;

	const FSceneTextureParameters& SceneTextures = GetSceneTextureParameters(GraphBuilder, Inputs.SceneTextures);
	const FScreenPassRenderTarget ViewFamilyOutput = FScreenPassRenderTarget::CreateViewFamilyOutput(Inputs.ViewFamilyTexture, View);

	// Scene color is updated incrementally through the post process pipeline.
	FScreenPassTexture SceneColor((*Inputs.SceneTextures)->SceneColorTexture, PrimaryViewRect);

	const FEngineShowFlags& EngineShowFlags = View.Family->EngineShowFlags;
	const bool bVisualizeHDR = EngineShowFlags.VisualizeHDR;
	const bool bViewFamilyOutputInHDR = GRHISupportsHDROutput && IsHDREnabled();
	const bool bOutputInHDR = IsPostProcessingOutputInHDR();

	// Post Process Material - Before Color Correction
	FPostProcessMaterialInputs PostProcessMaterialInputs;
	PostProcessMaterialInputs.SetInput(EPostProcessMaterialInput::SceneColor, SceneColor);
	PostProcessMaterialInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);

	SceneColor = AddPostProcessMaterialPass(GraphBuilder, View, PostProcessMaterialInputs, InMaterialInterface);

	// Replace tonemapper with device encoding only pass, which converts the scene color to device-specific color.
	FDeviceEncodingOnlyInputs PassInputs;
	PassInputs.OverrideOutput = ViewFamilyOutput;
	PassInputs.SceneColor = SceneColor;
	PassInputs.bOutputInHDR = bViewFamilyOutputInHDR;

	SceneColor = AddDeviceEncodingOnlyPass(GraphBuilder, View, PassInputs);
}

#endif

///////////////////////////////////////////////////////////////////////////
// Mobile Post Processing
//////////////////////////////////////////////////////////////////////////

static bool IsGaussianActive(const FViewInfo& View)
{
	float FarSize = View.FinalPostProcessSettings.DepthOfFieldFarBlurSize;
	float NearSize = View.FinalPostProcessSettings.DepthOfFieldNearBlurSize;

	float MaxSize = CVarDepthOfFieldMaxSize.GetValueOnRenderThread();

	FarSize = FMath::Min(FarSize, MaxSize);
	NearSize = FMath::Min(NearSize, MaxSize);
	const float CVarThreshold = CVarDepthOfFieldNearBlurSizeThreshold.GetValueOnRenderThread();

	if ((FarSize < 0.01f) && (NearSize < CVarThreshold))
	{
		return false;
	}
	return true;
}

void AddMobilePostProcessingPasses(FRDGBuilder& GraphBuilder, FScene* Scene, const FViewInfo& View, const FMobilePostProcessingInputs& Inputs, FInstanceCullingManager& InstanceCullingManager)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderPostProcessing);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_PostProcessing_Process);

	check(IsInRenderingThread());
	Inputs.Validate();

	const FIntRect FinalOutputViewRect = View.ViewRect;

	const FScreenPassRenderTarget ViewFamilyOutput = FScreenPassRenderTarget::CreateViewFamilyOutput(Inputs.ViewFamilyTexture, View);
	const FScreenPassTexture SceneDepth((*Inputs.SceneTextures)->SceneDepthTexture, FinalOutputViewRect);
	const FScreenPassTexture CustomDepth((*Inputs.SceneTextures)->CustomDepthTexture, FinalOutputViewRect);
	const FScreenPassTexture Velocity((*Inputs.SceneTextures)->SceneVelocityTexture, FinalOutputViewRect);
	const FScreenPassTexture BlackAlphaOneDummy(GSystemTextures.GetBlackAlphaOneDummy(GraphBuilder));

	// Scene color is updated incrementally through the post process pipeline.
	FScreenPassTexture SceneColor((*Inputs.SceneTextures)->SceneColorTexture, FinalOutputViewRect);
	FScreenPassTexture SceneDepthAux((*Inputs.SceneTextures)->SceneDepthAuxTexture, FinalOutputViewRect);

	// Default the new eye adaptation to the last one in case it's not generated this frame.
	const FEyeAdaptationParameters EyeAdaptationParameters = GetEyeAdaptationParameters(View, ERHIFeatureLevel::ES3_1);
	FRDGBufferRef LastEyeAdaptationBuffer = GetEyeAdaptationBuffer(GraphBuilder, View);

	const FPaniniProjectionConfig PaniniConfig(View);

	enum class EPass : uint32
	{
		Distortion,
		SunMask,
		BloomSetup,
		DepthOfField,
		Bloom,
		EyeAdaptation,
		SunMerge,
		SeparateTranslucency,
		TAA,
		Tonemap,
		PostProcessMaterialAfterTonemapping,
		FXAA,
		HighResolutionScreenshotMask,
		SelectionOutline,
		EditorPrimitive,
		PrimaryUpscale,
		Visualize,
		HMDDistortion,
		MAX
	};

	const TCHAR* PassNames[] =
	{
		TEXT("Distortion"),
		TEXT("SunMask"),
		TEXT("BloomSetup"),
		TEXT("DepthOfField"),
		TEXT("Bloom"),
		TEXT("EyeAdaptation"),
		TEXT("SunMerge"),
		TEXT("SeparateTranslucency"),
		TEXT("TAA"),
		TEXT("Tonemap"),
		TEXT("PostProcessMaterial (AfterTonemapping)"),
		TEXT("FXAA"),
		TEXT("HighResolutionScreenshotMask"),
		TEXT("SelectionOutline"),
		TEXT("EditorPrimitive"),
		TEXT("PrimaryUpscale"),
		TEXT("Visualize"),
		TEXT("HMDDistortion")
	};

	static_assert(static_cast<uint32>(EPass::MAX) == UE_ARRAY_COUNT(PassNames), "EPass does not match PassNames.");

	TOverridePassSequence<EPass> PassSequence(ViewFamilyOutput);
	PassSequence.SetNames(PassNames, UE_ARRAY_COUNT(PassNames));

	// This page: https://udn.epicgames.com/Three/RenderingOverview#Rendering%20state%20defaults 
	// describes what state a pass can expect and to what state it need to be set back.

	// All post processing is happening on the render thread side. All passes can access FinalPostProcessSettings and all
	// view settings. Those are copies for the RT then never get access by the main thread again.
	// Pointers to other structures might be unsafe to touch.

	const EDebugViewShaderMode DebugViewShaderMode = View.Family->GetDebugViewShaderMode();

	FScreenPassTexture BloomOutput;
	FScreenPassTexture DofOutput;
	FScreenPassTexture PostProcessSunShaftAndDof;

	// temporary solution for SP_METAL using HW sRGB flag during read vs all other mob platforms using
	// incorrect UTexture::SRGB state. (UTexture::SRGB != HW texture state)
	bool bSRGBAwareTarget = View.Family->RenderTarget->GetDisplayGamma() == 1.0f
		&& View.bIsSceneCapture
		&& IsMetalMobilePlatform(View.GetShaderPlatform());
	
	const EAutoExposureMethod AutoExposureMethod = GetAutoExposureMethod(View);
	const bool bUseEyeAdaptation = IsMobileEyeAdaptationEnabled(View);

	
	//The input scene color has been encoded to non-linear space and needs to decode somewhere if MSAA enabled on Metal platform
	bool bMetalMSAAHDRDecode = GSupportsShaderFramebufferFetch && IsMetalMobilePlatform(View.GetShaderPlatform()) && GetDefaultMSAACount(ERHIFeatureLevel::ES3_1) > 1;

	// add the passes we want to add to the graph (commenting a line means the pass is not inserted into the graph) ---------

	// HQ gaussian 
	bool bUseDof = GetMobileDepthOfFieldScale(View) > 0.0f && View.Family->EngineShowFlags.DepthOfField && !View.Family->EngineShowFlags.VisualizeDOF;
	bool bUseMobileDof = bUseDof && !View.FinalPostProcessSettings.bMobileHQGaussian;

	bool bUseToneMapper = !View.Family->EngineShowFlags.ShaderComplexity && IsMobileHDR();

	bool bUseHighResolutionScreenshotMask = IsHighResolutionScreenshotMaskEnabled(View);

	static const auto VarTonemapperUpscale = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileTonemapperUpscale"));
	bool bDisableUpscaleInTonemapper = !VarTonemapperUpscale || VarTonemapperUpscale->GetValueOnRenderThread() == 0;

	bool bShouldPrimaryUpscale = IsMobilePropagateAlphaEnabled(View.GetShaderPlatform()) || (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::SpatialUpscale && View.UnscaledViewRect != View.ViewRect);

	PassSequence.SetEnabled(EPass::Tonemap, bUseToneMapper);
	PassSequence.SetEnabled(EPass::HighResolutionScreenshotMask, bUseHighResolutionScreenshotMask);
#if WITH_EDITOR
	PassSequence.SetEnabled(EPass::SelectionOutline, GIsEditor && View.Family->EngineShowFlags.Selection && View.Family->EngineShowFlags.SelectionOutline && !View.Family->EngineShowFlags.Wireframe);
	PassSequence.SetEnabled(EPass::EditorPrimitive, FSceneRenderer::ShouldCompositeEditorPrimitives(View));
#else
	PassSequence.SetEnabled(EPass::SelectionOutline, false);
	PassSequence.SetEnabled(EPass::EditorPrimitive, false);
#endif
	PassSequence.SetEnabled(EPass::PrimaryUpscale, PaniniConfig.IsEnabled() || (bShouldPrimaryUpscale && bDisableUpscaleInTonemapper));

	PassSequence.SetEnabled(EPass::Visualize, View.Family->EngineShowFlags.ShaderComplexity);

	PassSequence.SetEnabled(EPass::HMDDistortion, View.Family->EngineShowFlags.StereoRendering && View.Family->EngineShowFlags.HMDDistortion);

	// Always evaluate custom post processes
	// The scene color will be decoded at the first post-process material and output linear color space for the following passes
	// bMetalMSAAHDRDecode will be set to false if there is any post-process material exist

	auto AddPostProcessMaterialPass = [&GraphBuilder, &View, &Inputs, &SceneColor, &CustomDepth, &bMetalMSAAHDRDecode, &PassSequence](EBlendableLocation BlendableLocation, bool bLastPass)
	{
		FPostProcessMaterialInputs PostProcessMaterialInputs;

		if (BlendableLocation == BL_AfterTonemapping && PassSequence.IsEnabled(EPass::PostProcessMaterialAfterTonemapping))
		{
			PassSequence.AcceptOverrideIfLastPass(EPass::PostProcessMaterialAfterTonemapping, PostProcessMaterialInputs.OverrideOutput);
		}

		PostProcessMaterialInputs.SetInput(EPostProcessMaterialInput::SceneColor, SceneColor);

		PostProcessMaterialInputs.CustomDepthTexture = CustomDepth.Texture;

		PostProcessMaterialInputs.bFlipYAxis = RHINeedsToSwitchVerticalAxis(View.GetShaderPlatform()) && bLastPass;

		PostProcessMaterialInputs.bMetalMSAAHDRDecode = bMetalMSAAHDRDecode;

		PostProcessMaterialInputs.SceneTextures = GetSceneTextureShaderParameters(Inputs.SceneTextures);

		const FPostProcessMaterialChain MaterialChain = GetPostProcessMaterialChain(View, BlendableLocation);

		if (MaterialChain.Num())
		{
			SceneColor = AddPostProcessMaterialChain(GraphBuilder, View, PostProcessMaterialInputs, MaterialChain);

			// For solid material, we decode the input color and output the linear color
			// For blend material, we force it rendering to an intermediate render target and decode there
			bMetalMSAAHDRDecode = false;
		}
	};

	if (IsPostProcessingEnabled(View))
	{
		bool bUseSun = View.MobileLightShaft.IsSet();
			
		bool bUseBloom = View.FinalPostProcessSettings.BloomIntensity > 0.0f;

		bool bUseBasicEyeAdaptation = bUseEyeAdaptation && (AutoExposureMethod == EAutoExposureMethod::AEM_Basic) &&
			// Skip if we don't have any exposure range to generate (eye adaptation will clamp).
			View.FinalPostProcessSettings.AutoExposureMinBrightness < View.FinalPostProcessSettings.AutoExposureMaxBrightness;

		bool bUseHistogramEyeAdaptation = bUseEyeAdaptation && (AutoExposureMethod == EAutoExposureMethod::AEM_Histogram) &&
			// Skip if we don't have any exposure range to generate (eye adaptation will clamp).
			View.FinalPostProcessSettings.AutoExposureMinBrightness < View.FinalPostProcessSettings.AutoExposureMaxBrightness;

		bool bUseTAA = View.AntiAliasingMethod == AAM_TemporalAA;
		ensure(View.AntiAliasingMethod != AAM_TSR);

		bool bUseDistortion = IsMobileDistortionActive(View);

		bool bUseSeparateTranslucency = IsMobileSeparateTranslucencyActive(View);

		const FPostProcessMaterialChain PostProcessMaterialAfterTonemappingChain = GetPostProcessMaterialChain(View, BL_AfterTonemapping);

		PassSequence.SetEnabled(EPass::Distortion, bUseDistortion);
		PassSequence.SetEnabled(EPass::SunMask, bUseSun || bUseDof);
		PassSequence.SetEnabled(EPass::BloomSetup, bUseSun || bUseMobileDof || bUseBloom || bUseBasicEyeAdaptation || bUseHistogramEyeAdaptation);
		PassSequence.SetEnabled(EPass::DepthOfField, bUseDof);
		PassSequence.SetEnabled(EPass::Bloom, bUseBloom);
		PassSequence.SetEnabled(EPass::EyeAdaptation, bUseEyeAdaptation);
		PassSequence.SetEnabled(EPass::SunMerge, bUseBloom || bUseSun);
		PassSequence.SetEnabled(EPass::SeparateTranslucency, bUseSeparateTranslucency);
		PassSequence.SetEnabled(EPass::TAA, bUseTAA);
		PassSequence.SetEnabled(EPass::PostProcessMaterialAfterTonemapping, PostProcessMaterialAfterTonemappingChain.Num() != 0);
		PassSequence.SetEnabled(EPass::FXAA, View.AntiAliasingMethod == AAM_FXAA);
		PassSequence.Finalize();
			
		if (PassSequence.IsEnabled(EPass::Distortion))
		{
			PassSequence.AcceptPass(EPass::Distortion);
			FMobileDistortionAccumulateInputs DistortionAccumulateInputs;
			DistortionAccumulateInputs.SceneColor = SceneColor;

			FMobileDistortionAccumulateOutputs DistortionAccumulateOutputs = AddMobileDistortionAccumulatePass(GraphBuilder, Scene, View, DistortionAccumulateInputs);

			FMobileDistortionMergeInputs DistortionMergeInputs;
			DistortionMergeInputs.SceneColor = SceneColor;
			DistortionMergeInputs.DistortionAccumulate = DistortionAccumulateOutputs.DistortionAccumulate;

			SceneColor = AddMobileDistortionMergePass(GraphBuilder, View, DistortionMergeInputs);
		}

		AddPostProcessMaterialPass(BL_BeforeTranslucency, false);

		// Optional fixed pass processes
		if (PassSequence.IsEnabled(EPass::SunMask))
		{
			PassSequence.AcceptPass(EPass::SunMask);
			bool bUseDepthTexture = SceneColor.Texture->Desc.Format == PF_FloatR11G11B10 && !SceneDepthAux.IsValid();

			FMobileSunMaskInputs SunMaskInputs;
			SunMaskInputs.bUseDepthTexture = bUseDepthTexture;
			SunMaskInputs.bUseDof = bUseDof;
			SunMaskInputs.bUseMetalMSAAHDRDecode = bMetalMSAAHDRDecode;
			SunMaskInputs.bUseSun = bUseSun;
			SunMaskInputs.SceneColor = SceneColor;
			SunMaskInputs.SceneTextures = Inputs.SceneTextures;

			// Convert depth to {circle of confusion, sun shaft intensity}
			FMobileSunMaskOutputs SunMaskOutputs = AddMobileSunMaskPass(GraphBuilder, View, SunMaskInputs);

			PostProcessSunShaftAndDof = SunMaskOutputs.SunMask;

			if (!bUseDepthTexture)
			{
				SceneColor = SunMaskOutputs.SceneColor;
			}

			// The scene color will be decoded after sun mask pass and output to linear color space for following passes if sun shaft enabled
			// set bMetalMSAAHDRDecode to false if sun shaft enabled
			bMetalMSAAHDRDecode = (bMetalMSAAHDRDecode && !bUseSun);
			//@todo Ronin sunmask pass isnt clipping to image only.
		}

		FMobileBloomSetupOutputs BloomSetupOutputs;
		if (PassSequence.IsEnabled(EPass::BloomSetup))
		{
			PassSequence.AcceptPass(EPass::BloomSetup);
			bool bHasEyeAdaptationPass = (bUseBasicEyeAdaptation || bUseHistogramEyeAdaptation);

			FMobileBloomSetupInputs BloomSetupInputs;
			BloomSetupInputs.bUseBloom = bUseBloom;
			BloomSetupInputs.bUseDof = bUseMobileDof;
			BloomSetupInputs.bUseEyeAdaptation = bHasEyeAdaptationPass;
			BloomSetupInputs.bUseMetalMSAAHDRDecode = bMetalMSAAHDRDecode;
			BloomSetupInputs.bUseSun = bUseSun;
			BloomSetupInputs.SceneColor = SceneColor;
			BloomSetupInputs.SunShaftAndDof = PostProcessSunShaftAndDof;

			BloomSetupOutputs = AddMobileBloomSetupPass(GraphBuilder, View, EyeAdaptationParameters, BloomSetupInputs);
		}

		if (PassSequence.IsEnabled(EPass::DepthOfField))
		{
			PassSequence.AcceptPass(EPass::DepthOfField);
			if (bUseMobileDof)
			{
				// Near dilation circle of confusion size.
				// Samples at 1/16 area, writes to 1/16 area.
				FMobileDofNearInputs DofNearInputs;
				DofNearInputs.BloomSetup_SunShaftAndDof = BloomSetupOutputs.SunShaftAndDof;
				DofNearInputs.bUseSun = bUseSun;

				FMobileDofNearOutputs DofNearOutputs = AddMobileDofNearPass(GraphBuilder, View, DofNearInputs);

				// DOF downsample pass.
				// Samples at full resolution, writes to 1/4 area.
				FMobileDofDownInputs DofDownInputs;
				DofDownInputs.bUseSun = bUseSun;
				DofDownInputs.DofNear = DofNearOutputs.DofNear;
				DofDownInputs.SceneColor = SceneColor;
				DofDownInputs.SunShaftAndDof = PostProcessSunShaftAndDof;

				FMobileDofDownOutputs DofDownOutputs = AddMobileDofDownPass(GraphBuilder, View, DofDownInputs);

				// DOF blur pass.
				// Samples at 1/4 area, writes to 1/4 area.
				FMobileDofBlurInputs DofBlurInputs;
				DofBlurInputs.DofDown = DofDownOutputs.DofDown;
				DofBlurInputs.DofNear = DofNearOutputs.DofNear;

				FMobileDofBlurOutputs DofBlurOutputs = AddMobileDofBlurPass(GraphBuilder, View, DofBlurInputs);

				DofOutput = DofBlurOutputs.DofBlur;

				FMobileIntegrateDofInputs IntegrateDofInputs;
				IntegrateDofInputs.DofBlur = DofBlurOutputs.DofBlur;
				IntegrateDofInputs.SceneColor = SceneColor;
				IntegrateDofInputs.SunShaftAndDof = PostProcessSunShaftAndDof;

				SceneColor = AddMobileIntegrateDofPass(GraphBuilder, View, IntegrateDofInputs);
			}
			else
			{
				bool bDepthOfField = IsGaussianActive(View);

				if (bDepthOfField)
				{
					float FarSize = View.FinalPostProcessSettings.DepthOfFieldFarBlurSize;
					float NearSize = View.FinalPostProcessSettings.DepthOfFieldNearBlurSize;
					const float MaxSize = CVarDepthOfFieldMaxSize.GetValueOnRenderThread();
					FarSize = FMath::Min(FarSize, MaxSize);
					NearSize = FMath::Min(NearSize, MaxSize);
					const bool bFar = FarSize >= 0.01f;
					const bool bNear = NearSize >= CVarDepthOfFieldNearBlurSizeThreshold.GetValueOnRenderThread();
					const bool bCombinedNearFarPass = bFar && bNear;

					if (bFar || bNear)
					{
						// AddGaussianDofBlurPass produces a blurred image from setup or potentially from taa result.
						auto AddGaussianDofBlurPass = [&GraphBuilder, &View](FScreenPassTexture& DOFSetup, bool bFarPass, float KernelSizePercent)
						{
							const TCHAR* BlurDebugX = bFarPass ? TEXT("FarDOFBlurX") : TEXT("NearDOFBlurX");
							const TCHAR* BlurDebugY = bFarPass ? TEXT("FarDOFBlurY") : TEXT("NearDOFBlurY");

							FGaussianBlurInputs GaussianBlurInputs;
							GaussianBlurInputs.NameX = BlurDebugX;
							GaussianBlurInputs.NameY = BlurDebugY;
							GaussianBlurInputs.Filter = DOFSetup;
							GaussianBlurInputs.TintColor = FLinearColor::White;
							GaussianBlurInputs.CrossCenterWeight = FVector2D::ZeroVector;
							GaussianBlurInputs.KernelSizePercent = KernelSizePercent;

							return AddGaussianBlurPass(GraphBuilder, View, GaussianBlurInputs);
						};

						FMobileDofSetupInputs DofSetupInputs;
						DofSetupInputs.bFarBlur = bFar;
						DofSetupInputs.bNearBlur = bNear;
						DofSetupInputs.SceneColor = SceneColor;
						DofSetupInputs.SunShaftAndDof = PostProcessSunShaftAndDof;
						FMobileDofSetupOutputs DofSetupOutputs = AddMobileDofSetupPass(GraphBuilder, View, DofSetupInputs);

						FScreenPassTexture DofFarBlur, DofNearBlur;
						if (bFar)
						{
							DofFarBlur = AddGaussianDofBlurPass(DofSetupOutputs.DofSetupFar, true, FarSize);
						}

						if (bNear)
						{
							DofNearBlur = AddGaussianDofBlurPass(DofSetupOutputs.DofSetupNear, false, NearSize);
						}

						FMobileDofRecombineInputs DofRecombineInputs;
						DofRecombineInputs.bFarBlur = bFar;
						DofRecombineInputs.bNearBlur = bNear;
						DofRecombineInputs.DofFarBlur = DofFarBlur;
						DofRecombineInputs.DofNearBlur = DofNearBlur;
						DofRecombineInputs.SceneColor = SceneColor;
						DofRecombineInputs.SunShaftAndDof = PostProcessSunShaftAndDof;

						SceneColor = AddMobileDofRecombinePass(GraphBuilder, View, DofRecombineInputs);
					}
				}
			}
		}

		// Bloom.
		FScreenPassTexture BloomUpOutputs;

		if (PassSequence.IsEnabled(EPass::Bloom))
		{
			PassSequence.AcceptPass(EPass::Bloom);
			auto AddBloomDownPass = [&GraphBuilder, &View](FScreenPassTexture& BloomDownSource, float BloomDownScale)
			{
				FMobileBloomDownInputs BloomDownInputs;
				BloomDownInputs.BloomDownScale = BloomDownScale;
				BloomDownInputs.BloomDownSource = BloomDownSource;

				return AddMobileBloomDownPass(GraphBuilder, View, BloomDownInputs);
			};

			float BloomDownScale = 0.66f * 4.0f;

			FScreenPassTexture PostProcessDownsample_Bloom[4];

			for (int32 i = 0; i < 4; ++i)
			{
				PostProcessDownsample_Bloom[i] = AddBloomDownPass(i == 0 ? BloomSetupOutputs.Bloom : PostProcessDownsample_Bloom[i - 1], BloomDownScale);
			}

			const FFinalPostProcessSettings& Settings = View.FinalPostProcessSettings;

			auto AddBloomUpPass = [&GraphBuilder, &View](FScreenPassTexture& BloomUpSourceA, FScreenPassTexture& BloomUpSourceB, float BloomSourceScale, const FVector4f& TintA, const FVector4f& TintB)
			{
				FMobileBloomUpInputs BloomUpInputs;
				BloomUpInputs.BloomUpSourceA = BloomUpSourceA;
				BloomUpInputs.BloomUpSourceB = BloomUpSourceB;
				BloomUpInputs.ScaleAB = FVector2D(BloomSourceScale, BloomSourceScale);
				BloomUpInputs.TintA = TintA;
				BloomUpInputs.TintB = TintB;

				return AddMobileBloomUpPass(GraphBuilder, View, BloomUpInputs);
			};

			float BloomUpScale = 0.66f * 2.0f;
			// Upsample by 2
			{
				FVector4f TintA = FVector4f(Settings.Bloom4Tint.R, Settings.Bloom4Tint.G, Settings.Bloom4Tint.B, 0.0f);
				FVector4f TintB = FVector4f(Settings.Bloom5Tint.R, Settings.Bloom5Tint.G, Settings.Bloom5Tint.B, 0.0f);
				TintA *= Settings.BloomIntensity;
				TintB *= Settings.BloomIntensity;

				BloomUpOutputs = AddBloomUpPass(PostProcessDownsample_Bloom[2], PostProcessDownsample_Bloom[3], BloomUpScale, TintA, TintB);
			}

			// Upsample by 2
			{
				FVector4f TintA = FVector4f(Settings.Bloom3Tint.R, Settings.Bloom3Tint.G, Settings.Bloom3Tint.B, 0.0f);
				TintA *= Settings.BloomIntensity;
				FVector4f TintB = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);

				BloomUpOutputs = AddBloomUpPass(PostProcessDownsample_Bloom[1], BloomUpOutputs, BloomUpScale, TintA, TintB);
			}

			// Upsample by 2
			{
				FVector4f TintA = FVector4f(Settings.Bloom2Tint.R, Settings.Bloom2Tint.G, Settings.Bloom2Tint.B, 0.0f);
				TintA *= Settings.BloomIntensity;
				// Scaling Bloom2 by extra factor to match filter area difference between PC default and mobile.
				TintA *= 0.5;
				FVector4f TintB = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);

				BloomUpOutputs = AddBloomUpPass(PostProcessDownsample_Bloom[0], BloomUpOutputs, BloomUpScale, TintA, TintB);
			}
		}

		if (PassSequence.IsEnabled(EPass::EyeAdaptation))
		{
			PassSequence.AcceptPass(EPass::EyeAdaptation);
			FMobileEyeAdaptationSetupInputs EyeAdaptationSetupInputs;
			
			EyeAdaptationSetupInputs.bUseBasicEyeAdaptation = bUseBasicEyeAdaptation;
			EyeAdaptationSetupInputs.bUseHistogramEyeAdaptation = bUseHistogramEyeAdaptation;
			EyeAdaptationSetupInputs.BloomSetup_EyeAdaptation = FScreenPassTexture(TryRegisterExternalTexture(GraphBuilder, View.PrevViewInfo.MobileBloomSetup_EyeAdaptation));
			if (!EyeAdaptationSetupInputs.BloomSetup_EyeAdaptation.IsValid())
			{
				EyeAdaptationSetupInputs.BloomSetup_EyeAdaptation = BloomSetupOutputs.EyeAdaptation;
			}

			FMobileEyeAdaptationSetupOutputs EyeAdaptationSetupOutputs = AddMobileEyeAdaptationSetupPass(GraphBuilder, View, EyeAdaptationParameters, EyeAdaptationSetupInputs);

			FMobileEyeAdaptationInputs EyeAdaptationInputs;
			EyeAdaptationInputs.bUseBasicEyeAdaptation = bUseBasicEyeAdaptation;
			EyeAdaptationInputs.bUseHistogramEyeAdaptation = bUseHistogramEyeAdaptation;
			EyeAdaptationInputs.EyeAdaptationSetupSRV = EyeAdaptationSetupOutputs.EyeAdaptationSetupSRV;
			EyeAdaptationInputs.EyeAdaptationBuffer = LastEyeAdaptationBuffer;

			AddMobileEyeAdaptationPass(GraphBuilder, View, EyeAdaptationParameters, EyeAdaptationInputs);

			if ((bUseBasicEyeAdaptation || bUseHistogramEyeAdaptation) && View.ViewState && !View.bStatePrevViewInfoIsReadOnly)
			{
				GraphBuilder.QueueTextureExtraction(BloomSetupOutputs.EyeAdaptation.Texture, &View.ViewState->PrevFrameViewInfo.MobileBloomSetup_EyeAdaptation);
			}
		}

		if (PassSequence.IsEnabled(EPass::SunMerge))
		{
			PassSequence.AcceptPass(EPass::SunMerge);
			FScreenPassTexture SunBlurOutputs;
			
			if (bUseSun)
			{
				FMobileSunAlphaInputs SunAlphaInputs;
				SunAlphaInputs.BloomSetup_SunShaftAndDof = BloomSetupOutputs.SunShaftAndDof;
				SunAlphaInputs.bUseMobileDof = bUseMobileDof;

				FScreenPassTexture SunAlphaOutputs = AddMobileSunAlphaPass(GraphBuilder, View, SunAlphaInputs);

				FMobileSunBlurInputs SunBlurInputs;
				SunBlurInputs.SunAlpha = SunAlphaOutputs;

				SunBlurOutputs = AddMobileSunBlurPass(GraphBuilder, View, SunBlurInputs);
			}

			FMobileSunMergeInputs SunMergeInputs;
			SunMergeInputs.BloomSetup_Bloom = BloomSetupOutputs.Bloom;
			SunMergeInputs.BloomUp = BloomUpOutputs;
			SunMergeInputs.SunBlur = SunBlurOutputs;
			SunMergeInputs.bUseBloom = bUseBloom;
			SunMergeInputs.bUseSun = bUseSun;

			BloomOutput = AddMobileSunMergePass(GraphBuilder, View, SunMergeInputs);

		}

		// mobile separate translucency 
		if (PassSequence.IsEnabled(EPass::SeparateTranslucency))
		{
			PassSequence.AcceptPass(EPass::SeparateTranslucency);
			FMobileSeparateTranslucencyInputs SeparateTranslucencyInputs;
			SeparateTranslucencyInputs.SceneColor = SceneColor;
			SeparateTranslucencyInputs.SceneDepth = SceneDepth;

			AddMobileSeparateTranslucencyPass(GraphBuilder, View, SeparateTranslucencyInputs);
		}

		AddPostProcessMaterialPass(BL_BeforeTonemapping, false);

		// Temporal Anti-aliasing. Also may perform a temporal upsample from primary to secondary view rect.
		if (PassSequence.IsEnabled(EPass::TAA))
		{
			PassSequence.AcceptPass(EPass::TAA);

			EMainTAAPassConfig TAAConfig = ITemporalUpscaler::GetMainTAAPassConfig(View);
			checkSlow(TAAConfig != EMainTAAPassConfig::Disabled);

			const ITemporalUpscaler* UpscalerToUse = (TAAConfig == EMainTAAPassConfig::ThirdParty) ? View.Family->GetTemporalUpscalerInterface() : ITemporalUpscaler::GetDefaultTemporalUpscaler();

			const TCHAR* UpscalerName = UpscalerToUse->GetDebugName();

			// Standard event scope for temporal upscaler to have all profiling information not matter what, and with explicit detection of third party.
			RDG_EVENT_SCOPE_CONDITIONAL(
				GraphBuilder,
				TAAConfig == EMainTAAPassConfig::ThirdParty,
				"ThirdParty %s %dx%d -> %dx%d",
				UpscalerToUse->GetDebugName(),
				View.ViewRect.Width(), View.ViewRect.Height(),
				View.GetSecondaryViewRectSize().X, View.GetSecondaryViewRectSize().Y);

			ITemporalUpscaler::FPassInputs UpscalerPassInputs;

			UpscalerPassInputs.bAllowDownsampleSceneColor = false;
			UpscalerPassInputs.SceneColorTexture = SceneColor.Texture;
			UpscalerPassInputs.SceneDepthTexture = SceneDepth.Texture;
			UpscalerPassInputs.SceneVelocityTexture = Velocity.Texture;

			FIntRect SecondaryViewRect;
			FScreenPassTexture HalfResolutionSceneColor;

			UpscalerToUse->AddPasses(
				GraphBuilder,
				View,
				UpscalerPassInputs,
				&SceneColor.Texture,
				&SecondaryViewRect,
				&HalfResolutionSceneColor.Texture,
				&HalfResolutionSceneColor.ViewRect);

			//! SceneColorTexture is now upsampled to the SecondaryViewRect. Use SecondaryViewRect for input / output.
			SceneColor.ViewRect = SecondaryViewRect;
		}
	}
	else
	{
		PassSequence.SetEnabled(EPass::Distortion, false);
		PassSequence.SetEnabled(EPass::SunMask, false);
		PassSequence.SetEnabled(EPass::BloomSetup, false);
		PassSequence.SetEnabled(EPass::DepthOfField, false);
		PassSequence.SetEnabled(EPass::Bloom, false);
		PassSequence.SetEnabled(EPass::EyeAdaptation, false);
		PassSequence.SetEnabled(EPass::SunMerge, false);
		PassSequence.SetEnabled(EPass::SeparateTranslucency, false);
		PassSequence.SetEnabled(EPass::TAA, false);
		PassSequence.SetEnabled(EPass::PostProcessMaterialAfterTonemapping, false);
		PassSequence.SetEnabled(EPass::FXAA, false);
		PassSequence.Finalize();
	}
	
	if (PassSequence.IsEnabled(EPass::Tonemap))
	{
		bool bHDRTonemapperOutput = false;

		if (!BloomOutput.IsValid())
		{
			BloomOutput = BlackAlphaOneDummy;
		}

		bool bDoGammaOnly = false;

		FRDGTextureRef ColorGradingTexture = nullptr;

		if (IStereoRendering::IsAPrimaryView(View))
		{
			ColorGradingTexture = AddCombineLUTPass(GraphBuilder, View);
		}
		// We can re-use the color grading texture from the primary view.
		else if (View.GetTonemappingLUT())
		{
			ColorGradingTexture = TryRegisterExternalTexture(GraphBuilder, View.GetTonemappingLUT());
		}
		else
		{
			const FViewInfo* PrimaryView = static_cast<const FViewInfo*>(View.Family->Views[0]);
			ColorGradingTexture = TryRegisterExternalTexture(GraphBuilder, PrimaryView->GetTonemappingLUT());
		}

		FTonemapInputs TonemapperInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::Tonemap, TonemapperInputs.OverrideOutput);

		// This is the view family render target.
		if (TonemapperInputs.OverrideOutput.Texture)
		{
			FIntRect OutputViewRect;
			if (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::RawOutput)
			{
				OutputViewRect = View.ViewRect;
			}
			else
			{
				OutputViewRect = View.UnscaledViewRect;
			}
			ERenderTargetLoadAction  OutputLoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

			TonemapperInputs.OverrideOutput.ViewRect = OutputViewRect;
			TonemapperInputs.OverrideOutput.LoadAction = OutputLoadAction;
		}
			
		TonemapperInputs.SceneColor = SceneColor;
		TonemapperInputs.Bloom.Bloom = BloomOutput;
		TonemapperInputs.EyeAdaptationParameters = &EyeAdaptationParameters;
		TonemapperInputs.EyeAdaptationTexture = nullptr;
		TonemapperInputs.ColorGradingTexture = ColorGradingTexture;
		TonemapperInputs.bWriteAlphaChannel = View.AntiAliasingMethod == AAM_FXAA || IsPostProcessingWithAlphaChannelSupported() || bUseMobileDof || IsMobilePropagateAlphaEnabled(View.GetShaderPlatform());
		TonemapperInputs.bFlipYAxis = RHINeedsToSwitchVerticalAxis(View.GetShaderPlatform()) && !PassSequence.IsEnabled(EPass::PostProcessMaterialAfterTonemapping);
		TonemapperInputs.bOutputInHDR = bHDRTonemapperOutput;
		TonemapperInputs.bGammaOnly = bDoGammaOnly;
		TonemapperInputs.bMetalMSAAHDRDecode = bMetalMSAAHDRDecode;
		TonemapperInputs.EyeAdaptationBuffer = bUseEyeAdaptation ? LastEyeAdaptationBuffer : nullptr;

		SceneColor = AddTonemapPass(GraphBuilder, View, TonemapperInputs);

		//The output color should been decoded to linear space after tone mapper apparently
		bMetalMSAAHDRDecode = false;
	}

	if (IsPostProcessingEnabled(View))
	{
		AddPostProcessMaterialPass(BL_AfterTonemapping, true);

		if (PassSequence.IsEnabled(EPass::FXAA))
		{
			FFXAAInputs PassInputs;
			PassSequence.AcceptOverrideIfLastPass(EPass::FXAA, PassInputs.OverrideOutput);
			PassInputs.SceneColor = SceneColor;
			PassInputs.Quality = GetFXAAQuality();

			SceneColor = AddFXAAPass(GraphBuilder, View, PassInputs);
		}
	}

	if (PassSequence.IsEnabled(EPass::HighResolutionScreenshotMask))
	{
		FHighResolutionScreenshotMaskInputs HighResolutionScreenshotMaskInputs;
		HighResolutionScreenshotMaskInputs.SceneColor = SceneColor;
		PassSequence.AcceptOverrideIfLastPass(EPass::Tonemap, HighResolutionScreenshotMaskInputs.OverrideOutput);
		HighResolutionScreenshotMaskInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

		SceneColor = AddHighResolutionScreenshotMaskPass(GraphBuilder, View, HighResolutionScreenshotMaskInputs);
	}

#if WITH_EDITOR
	// Show the selection outline if it is in the editor and we aren't in wireframe 
	// If the engine is in demo mode and game view is on we also do not show the selection outline
	if (PassSequence.IsEnabled(EPass::SelectionOutline))
	{
		FSelectionOutlineInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::SelectionOutline, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;
		PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

		// TODO: Nanite - pipe through results
		SceneColor = AddSelectionOutlinePass(GraphBuilder, View, PassInputs, nullptr);
	}

	if (PassSequence.IsEnabled(EPass::EditorPrimitive))
	{
		FEditorPrimitiveInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::EditorPrimitive, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.SceneDepth = SceneDepth;
		PassInputs.BasePassType = FEditorPrimitiveInputs::EBasePassType::Mobile;
		PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

		SceneColor = AddEditorPrimitivePass(GraphBuilder, View, PassInputs, InstanceCullingManager);
	}
#endif

	// Apply ScreenPercentage
	if (PassSequence.IsEnabled(EPass::PrimaryUpscale))
	{
		ISpatialUpscaler::FInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::PrimaryUpscale, PassInputs.OverrideOutput);
		PassInputs.Stage = EUpscaleStage::PrimaryToOutput;
		PassInputs.SceneColor = SceneColor;
		PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

		if (const ISpatialUpscaler* CustomUpscaler = View.Family->GetPrimarySpatialUpscalerInterface())
		{
			RDG_EVENT_SCOPE(
				GraphBuilder,
				"ThirdParty PrimaryUpscale %s %dx%d -> %dx%d",
				CustomUpscaler->GetDebugName(),
				SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(),
				View.UnscaledViewRect.Width(), View.UnscaledViewRect.Height());

			SceneColor = CustomUpscaler->AddPasses(GraphBuilder, View, PassInputs);

			if (PassSequence.IsLastPass(EPass::PrimaryUpscale))
			{
				check(SceneColor == ViewFamilyOutput);
			}
			else
			{
				check(SceneColor.ViewRect.Size() == View.UnscaledViewRect.Size());
			}
		}
		else
		{
			SceneColor = ISpatialUpscaler::AddDefaultUpscalePass(GraphBuilder, View, PassInputs, EUpscaleMethod::Bilinear, PaniniConfig);
		}
	}

	if (PassSequence.IsEnabled(EPass::Visualize))
	{
		FScreenPassRenderTarget OverrideOutput;
		PassSequence.AcceptOverrideIfLastPass(EPass::Visualize, OverrideOutput);

		switch (View.Family->GetDebugViewShaderMode())
		{
		case DVSM_QuadComplexity:
		{
			float ComplexityScale = 1.f / (float)(GEngine->QuadComplexityColors.Num() - 1) / NormalizedQuadComplexityValue; // .1f comes from the values used in LightAccumulator_GetResult

			FVisualizeComplexityInputs PassInputs;
			PassInputs.OverrideOutput = OverrideOutput;
			PassInputs.SceneColor = SceneColor;
			PassInputs.Colors = GEngine->QuadComplexityColors;
			PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Stair;
			PassInputs.ComplexityScale = ComplexityScale;
			PassInputs.bDrawLegend = true;
			PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

			SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
			break;
		}
		case DVSM_ShaderComplexity:
		case DVSM_ShaderComplexityContainedQuadOverhead:
		case DVSM_ShaderComplexityBleedingQuadOverhead:
		{
			FVisualizeComplexityInputs PassInputs;
			PassInputs.OverrideOutput = OverrideOutput;
			PassInputs.SceneColor = SceneColor;
			PassInputs.Colors = GEngine->ShaderComplexityColors;
			PassInputs.ColorSamplingMethod = FVisualizeComplexityInputs::EColorSamplingMethod::Ramp;
			PassInputs.ComplexityScale = 1.0f;
			PassInputs.bDrawLegend = true;
			PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

			SceneColor = AddVisualizeComplexityPass(GraphBuilder, View, PassInputs);
			break;
		}
		default:
			ensure(false);
			break;
		}
	}

	if (PassSequence.IsEnabled(EPass::HMDDistortion))
	{
		FHMDDistortionInputs PassInputs;
		PassSequence.AcceptOverrideIfLastPass(EPass::HMDDistortion, PassInputs.OverrideOutput);
		PassInputs.SceneColor = SceneColor;
		PassInputs.OverrideOutput.LoadAction = View.IsFirstInFamily() ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

		SceneColor = AddHMDDistortionPass(GraphBuilder, View, PassInputs);
	}
}

FRDGTextureRef AddProcessPlanarReflectionPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef SceneColorTexture)
{
	FSceneViewState* ViewState = View.ViewState;
	const EAntiAliasingMethod AntiAliasingMethod = View.AntiAliasingMethod;

	if (IsTemporalAccumulationBasedMethod(AntiAliasingMethod))
	{
		check(ViewState);

		FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

		const FTemporalAAHistory& InputHistory = View.PrevViewInfo.TemporalAAHistory;
		FTemporalAAHistory* OutputHistory = &ViewState->PrevFrameViewInfo.TemporalAAHistory;

		FTAAPassParameters Parameters(View);
		Parameters.SceneDepthTexture = SceneTextures.SceneDepthTexture;

		// Planar reflections don't support velocity.
		Parameters.SceneVelocityTexture = nullptr;

		Parameters.SceneColorInput = SceneColorTexture;

		FTAAOutputs PassOutputs = AddTemporalAAPass(
			GraphBuilder,
			View,
			Parameters,
			InputHistory,
			OutputHistory);

		return PassOutputs.SceneColor;
	}
	else
	{
		return SceneColorTexture;
	}
}

#if DEBUG_POST_PROCESS_VOLUME_ENABLE
FScreenPassTexture AddFinalPostProcessDebugInfoPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, FScreenPassTexture& ScreenPassSceneColor)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FinalPostProcessDebugInfo");

	FRDGTextureRef SceneColor = ScreenPassSceneColor.Texture;

	AddDrawCanvasPass(GraphBuilder, RDG_EVENT_NAME("PostProcessDebug"), View, FScreenPassRenderTarget(SceneColor, View.ViewRect, ERenderTargetLoadAction::ELoad),
		[&View](FCanvas& Canvas)
		{
			FLinearColor TextColor(FLinearColor::White);
			FLinearColor GrayTextColor(FLinearColor::Gray);
			FLinearColor GreenTextColor(FLinearColor::Green);
			FString Text;

			const float ViewPortWidth = float(View.ViewRect.Width());
			const float ViewPortHeight = float(View.ViewRect.Height());

			const float CRHeight = 20.0f;
			const float PrintX_CR = ViewPortWidth * 0.1f;

			float PrintX = PrintX_CR;
			float PrintY = ViewPortHeight * 0.2f;

			Text = FString::Printf(TEXT("Post-processing volume debug (count = %i)"), View.FinalPostProcessDebugInfo.Num());
			Canvas.DrawShadowedString(PrintX, PrintY, *Text, GetStatsFont(), GreenTextColor);					PrintX = PrintX_CR; PrintY += CRHeight * 1.5;

			Canvas.DrawShadowedString(PrintX, PrintY, *FString("Name"), GetStatsFont(), GrayTextColor);				PrintX += 256.0f;
			Canvas.DrawShadowedString(PrintX, PrintY, *FString("IsEnabled"), GetStatsFont(), GrayTextColor);		PrintX += 96.0f;
			Canvas.DrawShadowedString(PrintX, PrintY, *FString("Priority"), GetStatsFont(), GrayTextColor);			PrintX += 96.0f;
			Canvas.DrawShadowedString(PrintX, PrintY, *FString("CurrentWeight"), GetStatsFont(), GrayTextColor);	PrintX += 96.0f;
			Canvas.DrawShadowedString(PrintX, PrintY, *FString("bIsUnbound"), GetStatsFont(), GrayTextColor);		PrintX += 96.0f;
			
			PrintY += CRHeight;
			PrintX = PrintX_CR;

			const int32 PPDebugInfoCount = View.FinalPostProcessDebugInfo.Num() - 1;
			for (int32 i = PPDebugInfoCount; i >= 0 ; --i)
			{
				const FPostProcessSettingsDebugInfo& PPDebugInfo = View.FinalPostProcessDebugInfo[i];

				Text = FString::Printf(TEXT("%s"), *PPDebugInfo.Name.Left(40)); // Clamp the name to a reasonable length
				Canvas.DrawShadowedString(PrintX, PrintY, *Text, GetStatsFont(), TextColor); PrintX += 256.0f;

				Text = FString::Printf(TEXT("%d"), PPDebugInfo.bIsEnabled ? 1 : 0);
				Canvas.DrawShadowedString(PrintX+32.0f, PrintY, *Text, GetStatsFont(), TextColor); PrintX += 96.0f;

				Text = FString::Printf(TEXT("%.3f"), PPDebugInfo.Priority);
				Canvas.DrawShadowedString(PrintX, PrintY, *Text, GetStatsFont(), TextColor); PrintX += 96.0f;

				Text = FString::Printf(TEXT("%3.3f"), PPDebugInfo.CurrentBlendWeight);
				Canvas.DrawShadowedString(PrintX+32.0f, PrintY, *Text, GetStatsFont(), TextColor); PrintX += 96.0f;

				Text = FString::Printf(TEXT("%d"), PPDebugInfo.bIsUnbound ? 1 : 0);
				Canvas.DrawShadowedString(PrintX+32.0f, PrintY, *Text, GetStatsFont(), TextColor); PrintX += 96.0f;

				Canvas.DrawShadowedString(PrintX_CR, PrintY+3.0f, *FString("______________________________________________________________________________________________________________"), GetStatsFont(), TextColor);

				PrintX = PrintX_CR;
				PrintY += CRHeight;
			}
		});

	return MoveTemp(ScreenPassSceneColor);
}
#endif
