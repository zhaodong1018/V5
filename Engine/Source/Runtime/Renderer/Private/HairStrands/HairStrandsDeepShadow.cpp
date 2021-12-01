// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsDeepShadow.h"
#include "HairStrandsRasterCommon.h"
#include "HairStrandsCluster.h"
#include "HairStrandsUtils.h"
#include "HairStrandsData.h"
#include "LightSceneInfo.h"
#include "ScenePrivate.h"

// this is temporary until we split the voxelize and DOM path
static int32 GDeepShadowResolution = 2048;
static FAutoConsoleVariableRef CVarDeepShadowResolution(TEXT("r.HairStrands.DeepShadow.Resolution"), GDeepShadowResolution, TEXT("Shadow resolution for Deep Opacity Map rendering. (default = 2048)"));

static int32 GDeepShadowGPUDriven = 1;
static FAutoConsoleVariableRef CVarDeepShadowGPUDriven(TEXT("r.HairStrands.DeepShadow.GPUDriven"), GDeepShadowGPUDriven, TEXT("Enable deep shadow to be driven by GPU bounding box, rather CPU ones. This allows more robust behavior"));

static int32 GDeepShadowInjectVoxelDepth = 0;
static FAutoConsoleVariableRef CVarDeepShadowInjectVoxelDepth(TEXT("r.HairStrands.DeepShadow.InjectVoxelDepth"), GDeepShadowInjectVoxelDepth, TEXT("Inject voxel content to generate the deep shadow map instead of rasterizing groom. This is an experimental path"));

///////////////////////////////////////////////////////////////////////////////////////////////////
// Inject voxel structure into shadow map to amortize the tracing, and rely on look up kernel to 
// filter limited resolution
BEGIN_SHADER_PARAMETER_STRUCT(FHairStransShadowDepthInjectionParameters, )
	SHADER_PARAMETER(FMatrix44f, CPU_WorldToClip)

	SHADER_PARAMETER(FVector2f, OutputResolution)
	SHADER_PARAMETER(uint32, AtlasSlotIndex)
	SHADER_PARAMETER(uint32, bIsGPUDriven)

	SHADER_PARAMETER(FVector3f, LightDirection)
	SHADER_PARAMETER(uint32, MacroGroupId)

	SHADER_PARAMETER(FVector3f, LightPosition)
	SHADER_PARAMETER(uint32, bIsDirectional)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FDeepShadowViewInfo>, DeepShadowViewInfoBuffer)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FHairStrandsShadowDepthInjection : public FGlobalShader
{
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_DEPTH_INJECTION"), 1);
	}

	FHairStrandsShadowDepthInjection() = default;
	FHairStrandsShadowDepthInjection(const CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}
};

class FHairStrandsShadowDepthInjectionVS : public FHairStrandsShadowDepthInjection
{
	DECLARE_GLOBAL_SHADER(FHairStrandsShadowDepthInjectionVS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsShadowDepthInjectionVS, FHairStrandsShadowDepthInjection);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStransShadowDepthInjectionParameters, Pass)
		END_SHADER_PARAMETER_STRUCT()
};

class FHairStrandsShadowDepthInjectionPS : public FHairStrandsShadowDepthInjection
{
	DECLARE_GLOBAL_SHADER(FHairStrandsShadowDepthInjectionPS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsShadowDepthInjectionPS, FHairStrandsShadowDepthInjection);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStransShadowDepthInjectionParameters, Pass)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHairStrandsShadowDepthInjectionPS, "/Engine/Private/HairStrands/HairStrandsVoxelDepthInjection.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FHairStrandsShadowDepthInjectionVS, "/Engine/Private/HairStrands/HairStrandsVoxelDepthInjection.usf", "MainVS", SF_Vertex);

void AddInjectHairVoxelShadowCaster(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const bool bClear,
	const FHairStrandsDeepShadowData& DomData,
	FMatrix CPU_WorldToClipMatrix,
	FIntRect AtlasRect,
	uint32 AtlasSlotIndex,
	FIntPoint AtlasSlotResolution,
	FHairStrandsVoxelResources& VoxelResources,
	FRDGBufferSRVRef DeepShadowViewInfoBufferSRV,
	FRDGTextureRef OutDepthTexture)
{
	FHairStransShadowDepthInjectionParameters* Parameters = GraphBuilder.AllocParameters<FHairStransShadowDepthInjectionParameters>();
	Parameters->OutputResolution = AtlasSlotResolution;
	Parameters->CPU_WorldToClip = CPU_WorldToClipMatrix;
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(OutDepthTexture, bClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
	Parameters->VirtualVoxel = VoxelResources.UniformBuffer;
	Parameters->LightDirection = DomData.LightDirection;
	Parameters->LightPosition = DomData.LightPosition;
	Parameters->bIsDirectional = DomData.bIsLightDirectional ? 1 : 0;
	Parameters->MacroGroupId = DomData.MacroGroupId;
	Parameters->DeepShadowViewInfoBuffer = DeepShadowViewInfoBufferSRV;
	Parameters->bIsGPUDriven = GDeepShadowGPUDriven > 0;
	Parameters->AtlasSlotIndex = AtlasSlotIndex;

	TShaderMapRef<FHairStrandsShadowDepthInjectionVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairStrandsShadowDepthInjectionPS> PixelShader(View.ShaderMap);
	FHairStrandsShadowDepthInjectionVS::FParameters ParametersVS;
	FHairStrandsShadowDepthInjectionPS::FParameters ParametersPS;
	ParametersVS.Pass = *Parameters;
	ParametersPS.Pass = *Parameters;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrandsShadowDepthInjection"),
		Parameters,
		ERDGPassFlags::Raster,
		[ParametersVS, ParametersPS, VertexShader, PixelShader, AtlasRect](FRHICommandList& RHICmdList)
		{

			// Apply additive blending pipeline state.
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Max, BF_SourceColor, BF_DestColor, BO_Max, BF_SourceAlpha, BF_DestAlpha>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Greater>::GetRHI();
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), ParametersVS);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), ParametersPS);

			// Emit an instanced quad draw call on the order of the number of pixels on the screen.	
			RHICmdList.SetViewport(AtlasRect.Min.X, AtlasRect.Min.Y, 0.0f, AtlasRect.Max.X, AtlasRect.Max.Y, 1.0f);
			RHICmdList.DrawPrimitive(0, 12, 1);
		});
}


///////////////////////////////////////////////////////////////////////////////////////////////////

typedef TArray<const FLightSceneInfo*, SceneRenderingAllocator> FLightSceneInfos;

static FLightSceneInfos GetVisibleDeepShadowLights(const FScene* Scene, const FViewInfo& View)
{
	// Collect all visible lights for the current view
	FLightSceneInfos Out;
	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* const LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

		if (!LightSceneInfo->ShouldRenderLightViewIndependent())
			continue;

		// Check if the light is visible in any of the views.
		{
			const bool bIsCompatible = LightSceneInfo->ShouldRenderLight(View) && LightSceneInfo->Proxy->CastsHairStrandsDeepShadow();
			if (!bIsCompatible)
				continue;

			Out.Add(LightSceneInfo);
		}
	}

	return Out;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FDeepShadowCreateViewInfoCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeepShadowCreateViewInfoCS);
	SHADER_USE_PARAMETER_STRUCT(FDeepShadowCreateViewInfoCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_ARRAY(FVector4f,	LightDirections,	[FHairStrandsDeepShadowData::MaxMacroGroupCount])
		SHADER_PARAMETER_ARRAY(FVector4f,	LightPositions,		[FHairStrandsDeepShadowData::MaxMacroGroupCount])
		SHADER_PARAMETER_ARRAY(FIntVector4,	MacroGroupIndices,	[FHairStrandsDeepShadowData::MaxMacroGroupCount])

		SHADER_PARAMETER(FVector3f, CPU_MinAABB)
		SHADER_PARAMETER(uint32, CPU_bUseCPUData)
		SHADER_PARAMETER(FVector3f, CPU_MaxAABB)
		SHADER_PARAMETER(float, RasterizationScale)

		SHADER_PARAMETER(FIntPoint, SlotResolution)
		SHADER_PARAMETER(uint32, SlotIndexCount)
		SHADER_PARAMETER(uint32, MacroGroupCount)

		SHADER_PARAMETER(float, AABBScale)
		SHADER_PARAMETER(float, MaxHafFovInRad)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>, MacroGroupAABBBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDeepShadowViewInfo>, OutShadowViewInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4x4>, OutShadowWorldToLightTransformBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_ALLOCATE"), 1);
		OutEnvironment.SetDefine(TEXT("MAX_SLOT_COUNT"), FHairStrandsDeepShadowData::MaxMacroGroupCount);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeepShadowCreateViewInfoCS, "/Engine/Private/HairStrands/HairStrandsDeepShadowAllocation.usf", "CreateViewInfo", SF_Compute);
///////////////////////////////////////////////////////////////////////////////////////////////////

float GetDeepShadowMaxFovAngle();
float GetDeepShadowRasterizationScale();
float GetDeepShadowAABBScale();
FVector4f ComputeDeepShadowLayerDepths(float LayerDistribution);

void RenderHairStrandsDeepShadows(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	FViewInfo& View,
	FInstanceCullingManager& InstanceCullingManager)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_CLM_RenderDeepShadow);
	DECLARE_GPU_STAT(HairStrandsDeepShadow);
	RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsDeepShadow");
	RDG_GPU_STAT_SCOPE(GraphBuilder, HairStrandsDeepShadow);

	const FLightSceneInfos VisibleLights = GetVisibleDeepShadowLights(Scene, View);
	FHairStrandsMacroGroupDatas& MacroGroupDatas = View.HairStrandsViewData.MacroGroupDatas;
	FHairStrandsMacroGroupResources MacroGroupResources = View.HairStrandsViewData.MacroGroupResources;
	FHairStrandsDeepShadowResources& DeepShadowResources = View.HairStrandsViewData.DeepShadowResources;
	FHairStrandsVoxelResources VirtualVoxelResources = View.HairStrandsViewData.VirtualVoxelResources;

	{
		if (!View.Family)
		{
			return;
		}

		if (MacroGroupDatas.Num() == 0 || 
			VisibleLights.Num() == 0 ||
			IsHairStrandsForVoxelTransmittanceAndShadowEnable()) 
		{
			return; 
		}

		// Compute the number of DOM which need to be created and insert default value
		uint32 DOMSlotCount = 0;
		for (const FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
		{			
			const FBoxSphereBounds MacroGroupBounds = MacroGroup.Bounds;
			for (const FLightSceneInfo* LightInfo : VisibleLights)
			{
				const FLightSceneProxy* LightProxy = LightInfo->Proxy;
				if (!LightProxy->AffectsBounds(MacroGroupBounds))
				{
					return;
				}

				// Run out of atlas slot
				if (DOMSlotCount >= FHairStrandsDeepShadowResources::MaxAtlasSlotCount)
				{
					return;
				}

				DOMSlotCount++;
			}
		}

		if (DOMSlotCount == 0)
			return;

		const uint32 AtlasSlotX = FGenericPlatformMath::CeilToInt(FMath::Sqrt(static_cast<float>(DOMSlotCount)));
		const FIntPoint AtlasSlotDimension(AtlasSlotX, AtlasSlotX == DOMSlotCount ? 1 : AtlasSlotX);
		const FIntPoint AtlasSlotResolution(GDeepShadowResolution, GDeepShadowResolution);
		const FIntPoint AtlasResolution(AtlasSlotResolution.X * AtlasSlotDimension.X, AtlasSlotResolution.Y * AtlasSlotDimension.Y);

		
		DeepShadowResources.TotalAtlasSlotCount = 0;

		// Create Atlas resources for DOM. It is shared for all lights, across all views
		bool bClear = true;
		FRDGTextureRef FrontDepthAtlasTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(AtlasResolution, PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource), TEXT("Hair.ShadowDepth"));
		FRDGTextureRef DeepShadowLayersAtlasTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(AtlasResolution, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_ShaderResource), TEXT("Hair.DeepShadowLayers"));

		// TODO add support for multiple view: need to deduplicate light which are visible accross several views
		// Allocate atlas CPU slot
		uint32 TotalAtlasSlotIndex = 0;
		for (FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
		{
			// List of all the light in the scene.
			for (const FLightSceneInfo* LightInfo : VisibleLights)
			{
				FBoxSphereBounds MacroGroupBounds = MacroGroup.Bounds;

				const FLightSceneProxy* LightProxy = LightInfo->Proxy;
				if (!LightProxy->AffectsBounds(MacroGroupBounds))
				{
					continue;
				}
					
				if (TotalAtlasSlotIndex >= FHairStrandsDeepShadowResources::MaxAtlasSlotCount)
				{
					continue;
				}

				const ELightComponentType LightType = (ELightComponentType)LightProxy->GetLightType();
				const bool bIsDirectional = LightType == ELightComponentType::LightType_Directional;
				FMinHairRadiusAtDepth1 MinStrandRadiusAtDepth1;
				const FIntPoint AtlasRectOffset(
					(TotalAtlasSlotIndex % AtlasSlotDimension.X) * AtlasSlotResolution.X,
					(TotalAtlasSlotIndex / AtlasSlotDimension.X) * AtlasSlotResolution.Y);

				// Note: LightPosition.W is used in the transmittance mask shader to differentiate between directional and local lights.
				FHairStrandsDeepShadowData& DomData = MacroGroup.DeepShadowDatas.AddZeroed_GetRef();
				ComputeWorldToLightClip(DomData.CPU_WorldToLightTransform, MinStrandRadiusAtDepth1, MacroGroupBounds, *LightProxy, LightType, AtlasSlotResolution);
				DomData.LightDirection = LightProxy->GetDirection();
				DomData.LightPosition = FVector4f(FVector(LightProxy->GetPosition()), bIsDirectional ? 0 : 1);
				DomData.LightLuminance = LightProxy->GetColor();
				DomData.LayerDistribution = LightProxy->GetDeepShadowLayerDistribution();
				DomData.bIsLightDirectional = bIsDirectional;
				DomData.LightId = LightInfo->Id;
				DomData.ShadowResolution = AtlasSlotResolution;
				DomData.Bounds = MacroGroupBounds;
				DomData.AtlasRect = FIntRect(AtlasRectOffset, AtlasRectOffset + AtlasSlotResolution);
				DomData.MacroGroupId = MacroGroup.MacroGroupId;
				DomData.CPU_MinStrandRadiusAtDepth1 = MinStrandRadiusAtDepth1;
				DomData.AtlasSlotIndex = TotalAtlasSlotIndex;
				TotalAtlasSlotIndex++;
			}
		}

		// Sanity check
		check(DOMSlotCount == TotalAtlasSlotIndex); 

		DeepShadowResources.TotalAtlasSlotCount = TotalAtlasSlotIndex;
		DeepShadowResources.AtlasSlotResolution = AtlasSlotResolution;

		FRDGBufferRef DeepShadowViewInfoBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(20 * sizeof(float), FMath::Max(1u, TotalAtlasSlotIndex)), TEXT("Hair.DeepShadowViewInfo"));
		FRDGBufferRef DeepShadowWorldToLightBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16 * sizeof(float), FMath::Max(1u, TotalAtlasSlotIndex)), TEXT("Hair.DeepShadowWorldToLightTransform"));
		FRDGBufferSRVRef DeepShadowViewInfoBufferSRV = GraphBuilder.CreateSRV(DeepShadowViewInfoBuffer);

		DeepShadowResources.bIsGPUDriven = GDeepShadowGPUDriven > 0;
		{
			check(TotalAtlasSlotIndex < FHairStrandsDeepShadowResources::MaxAtlasSlotCount);

			// Allocate and create projection matrix and Min radius
			// Stored FDeepShadowViewInfo structs
			// See HairStrandsDeepShadowCommonStruct.ush for more details
			FDeepShadowCreateViewInfoCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeepShadowCreateViewInfoCS::FParameters>();

			for (FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
			{
				for (FHairStrandsDeepShadowData& DomData : MacroGroup.DeepShadowDatas)
				{				
					Parameters->LightDirections[DomData.AtlasSlotIndex]		= FVector4f(DomData.LightDirection.X, DomData.LightDirection.Y, DomData.LightDirection.Z, 0);
					Parameters->LightPositions[DomData.AtlasSlotIndex]		= FVector4f(DomData.LightPosition.X, DomData.LightPosition.Y, DomData.LightPosition.Z, DomData.bIsLightDirectional ? 0 : 1);
					Parameters->MacroGroupIndices[DomData.AtlasSlotIndex]	= FIntVector4(DomData.MacroGroupId, 0,0,0);
				}
			}

			Parameters->SlotResolution = DeepShadowResources.AtlasSlotResolution;
			Parameters->SlotIndexCount = DeepShadowResources.TotalAtlasSlotCount;
			Parameters->MacroGroupCount = MacroGroupDatas.Num();
			Parameters->MacroGroupAABBBuffer = GraphBuilder.CreateSRV(MacroGroupResources.MacroGroupAABBsBuffer, PF_R32_SINT);
			Parameters->OutShadowViewInfoBuffer = GraphBuilder.CreateUAV(DeepShadowViewInfoBuffer);
			Parameters->OutShadowWorldToLightTransformBuffer = GraphBuilder.CreateUAV(DeepShadowWorldToLightBuffer);

			Parameters->MaxHafFovInRad = 0.5f * FMath::DegreesToRadians(GetDeepShadowMaxFovAngle());
			Parameters->AABBScale = GetDeepShadowAABBScale();
			Parameters->RasterizationScale = GetDeepShadowRasterizationScale();
			Parameters->CPU_bUseCPUData	= 0;
			Parameters->CPU_MinAABB		= FVector::ZeroVector;
			Parameters->CPU_MaxAABB		= FVector::ZeroVector;

			// Currently support only 32 instance group at max
			TShaderMapRef<FDeepShadowCreateViewInfoCS> ComputeShader(View.ShaderMap);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("HairStrandsDeepShadowAllocate"),
				ComputeShader,
				Parameters,
				FIntVector(1, 1, 1));
		}

		// Render deep shadows
		for (FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
		{
			for (FHairStrandsDeepShadowData& DomData : MacroGroup.DeepShadowDatas)
			{
				const bool bIsOrtho = DomData.bIsLightDirectional;
				const FVector4f HairRenderInfo = PackHairRenderInfo(DomData.CPU_MinStrandRadiusAtDepth1.Primary, DomData.CPU_MinStrandRadiusAtDepth1.Stable, DomData.CPU_MinStrandRadiusAtDepth1.Primary, 1);
				const uint32 HairRenderInfoBits = PackHairRenderInfoBits(bIsOrtho, DeepShadowResources.bIsGPUDriven);
					
				const bool bDeepShadow = GDeepShadowInjectVoxelDepth == 0;
				// Inject voxel result into the deep shadow
				if (!bDeepShadow)
				{
					DECLARE_GPU_STAT(HairStrandsDeepShadowFrontDepth);
					RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsDeepShadowFrontDepth");
					RDG_GPU_STAT_SCOPE(GraphBuilder, HairStrandsDeepShadowFrontDepth);

					AddInjectHairVoxelShadowCaster(
						GraphBuilder,
						View,
						bClear,
						DomData,
						DomData.CPU_WorldToLightTransform,
						DomData.AtlasRect,
						DomData.AtlasSlotIndex,
						AtlasSlotResolution,
						VirtualVoxelResources,
						DeepShadowViewInfoBufferSRV,
						FrontDepthAtlasTexture);

					if (bClear)
					{
						AddClearRenderTargetPass(GraphBuilder, DeepShadowLayersAtlasTexture);
					}
				}
					
				const FVector4f LayerDepths = ComputeDeepShadowLayerDepths(DomData.LayerDistribution);
				// Front depth
				if (bDeepShadow)
				{
					const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

					DECLARE_GPU_STAT(HairStrandsDeepShadowFrontDepth);
					RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsDeepShadowFrontDepth");
					RDG_GPU_STAT_SCOPE(GraphBuilder, HairStrandsDeepShadowFrontDepth);

					FHairDeepShadowRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FHairDeepShadowRasterPassParameters>();

					{
						FHairDeepShadowRasterUniformParameters* UniformParameters = GraphBuilder.AllocParameters<FHairDeepShadowRasterUniformParameters>();

						UniformParameters->CPU_WorldToClipMatrix = DomData.CPU_WorldToLightTransform;;
						UniformParameters->SliceValue = FVector4f(1, 1, 1, 1);
						UniformParameters->AtlasRect = DomData.AtlasRect;
						UniformParameters->AtlasSlotIndex = DomData.AtlasSlotIndex;
						UniformParameters->LayerDepths = LayerDepths;
						UniformParameters->ViewportResolution = AtlasSlotResolution;
						UniformParameters->FrontDepthTexture = SystemTextures.DepthDummy;
						UniformParameters->DeepShadowViewInfoBuffer = DeepShadowViewInfoBufferSRV;

						PassParameters->UniformBuffer = GraphBuilder.CreateUniformBuffer(UniformParameters);
					}

					PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(FrontDepthAtlasTexture, bClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

					AddHairDeepShadowRasterPass(
						GraphBuilder,
						Scene,
						&View,
						MacroGroup.PrimitivesInfos,
						EHairStrandsRasterPassType::FrontDepth,
						DomData.AtlasRect,
						HairRenderInfo,
						HairRenderInfoBits,
						DomData.LightDirection,
						PassParameters,
						InstanceCullingManager);
				}

				// Deep layers
				if (bDeepShadow)
				{
					DECLARE_GPU_STAT(HairStrandsDeepShadowLayers);
					RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsDeepShadowLayers");
					RDG_GPU_STAT_SCOPE(GraphBuilder, HairStrandsDeepShadowLayers);

					FHairDeepShadowRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FHairDeepShadowRasterPassParameters>();

					{
						FHairDeepShadowRasterUniformParameters* UniformParameters = GraphBuilder.AllocParameters<FHairDeepShadowRasterUniformParameters>();

						UniformParameters->CPU_WorldToClipMatrix = DomData.CPU_WorldToLightTransform;;
						UniformParameters->SliceValue = FVector4f(1, 1, 1, 1);
						UniformParameters->AtlasRect = DomData.AtlasRect;
						UniformParameters->AtlasSlotIndex = DomData.AtlasSlotIndex;
						UniformParameters->LayerDepths = LayerDepths;
						UniformParameters->ViewportResolution = AtlasSlotResolution;
						UniformParameters->FrontDepthTexture = FrontDepthAtlasTexture;
						UniformParameters->DeepShadowViewInfoBuffer = DeepShadowViewInfoBufferSRV;

						PassParameters->UniformBuffer = GraphBuilder.CreateUniformBuffer(UniformParameters);
					}

					PassParameters->RenderTargets[0] = FRenderTargetBinding(DeepShadowLayersAtlasTexture, bClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, 0);

					AddHairDeepShadowRasterPass(
						GraphBuilder,
						Scene,
						&View,
						MacroGroup.PrimitivesInfos,
						EHairStrandsRasterPassType::DeepOpacityMap,
						DomData.AtlasRect,
						HairRenderInfo,
						HairRenderInfoBits,
						DomData.LightDirection,
						PassParameters,
						InstanceCullingManager);
				}
				bClear = false;
			}
		}
		DeepShadowResources.DepthAtlasTexture = FrontDepthAtlasTexture;
		DeepShadowResources.LayersAtlasTexture = DeepShadowLayersAtlasTexture;
		DeepShadowResources.DeepShadowWorldToLightTransforms = DeepShadowWorldToLightBuffer;
	}
}
