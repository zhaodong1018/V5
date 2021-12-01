// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	InstancedStaticMesh.cpp: Static mesh rendering code.
=============================================================================*/

#include "Engine/InstancedStaticMesh.h"
#include "InstancedStaticMeshDelegates.h"
#include "AI/NavigationSystemBase.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Components/LightComponent.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "UnrealEngine.h"
#include "AI/NavigationSystemHelpers.h"
#include "AI/Navigation/NavCollisionBase.h"
#include "ShaderParameterUtils.h"
#include "Misc/UObjectToken.h"
#include "PhysXPublic.h"
#include "PhysicsEngine/PhysXSupport.h"
#include "PhysicsEngine/BodySetup.h"
#include "GameFramework/WorldSettings.h"
#include "ComponentRecreateRenderStateContext.h"
#include "SceneManagement.h"
#include "Algo/Transform.h"
#include "UObject/MobileObjectVersion.h"
#include "EngineStats.h"
#include "Interfaces/ITargetPlatform.h"
#if WITH_EDITOR
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#endif // WITH_EDITOR
#include "MeshMaterialShader.h"
#include "ProfilingDebugging/LoadTimeTracker.h"
#include "NaniteSceneProxy.h"

#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Elements/Interfaces/TypedElementWorldInterface.h"

#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif

#if WITH_EDITOR
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#endif // WITH_EDITOR
#include "UObject/FortniteMainBranchObjectVersion.h"
#include "UObject/EditorObjectVersion.h"
#include "UObject/RenderingObjectVersion.h"


#if WITH_EDITOR
#include "Rendering/StaticLightingSystemInterface.h"
#endif

IMPLEMENT_TYPE_LAYOUT(FInstancedStaticMeshVertexFactoryShaderParameters);


const int32 InstancedStaticMeshMaxTexCoord = 8;
static const int32 MaxSimulatedInstances = 256;

IMPLEMENT_HIT_PROXY(HInstancedStaticMeshInstance, HHitProxy);

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FInstancedStaticMeshVertexFactoryUniformShaderParameters, "InstanceVF");

TAutoConsoleVariable<int32> CVarMinLOD(
	TEXT("foliage.MinLOD"),
	-1,
	TEXT("Used to discard the top LODs for performance evaluation. -1: Disable all effects of this cvar."),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<int32> CVarRayTracingRenderInstances(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes"),
	1,
	TEXT("Include static mesh instances in ray tracing effects (default = 1 (Instances enabled in ray tracing))"));

static TAutoConsoleVariable<int32> CVarRayTracingRenderInstancesCulling(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.Culling"),
	1,
	TEXT("Enable culling for instances in ray tracing (default = 1 (Culling enabled))"));

static TAutoConsoleVariable<float> CVarRayTracingInstancesCullClusterMaxRadiusMultiplier(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.CullClusterMaxRadiusMultiplier"),
	20.0f, 
	TEXT("Multiplier for the maximum instance size (default = 20)"));

static TAutoConsoleVariable<float> CVarRayTracingInstancesCullClusterRadius(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.CullClusterRadius"),
	10000.0f, // 100 m
	TEXT("Ignore instances outside of this radius in ray tracing effects (default = 10000 (100m))"));

static TAutoConsoleVariable<float> CVarRayTracingInstancesLowScaleThreshold(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.LowScaleRadiusThreshold"),
	50.0f, // Instances with a radius smaller than this threshold get culled after CVarRayTracingInstancesLowScaleCullRadius
	TEXT("Threshold that classifies instances as small (default = 50cm))"));

static TAutoConsoleVariable<float> CVarRayTracingInstancesLowScaleCullRadius(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.LowScaleCullRadius"),
	1000.0f, 
	TEXT("Cull radius for small instances (default = 1000 (10m))"));

static TAutoConsoleVariable<float> CVarRayTracingInstancesCullAngle(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.CullAngle"),
	2.0f,
	TEXT("Solid angle to test instance bounds against for culling (default 2 degrees)\n")
	TEXT("  -1 => use distance based culling")
);

static TAutoConsoleVariable<int32> CVarRayTracingInstancesEvaluateWPO(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.EvaluateWPO"),
	0,
	TEXT("Whether to evaluate WPO on instanced static meshes\n")
	TEXT("  0 - off (default)")
	TEXT("  1 - on for all with WPO")
	TEXT(" -1 - on only for meshes with evaluate WPO enabled")
);

static TAutoConsoleVariable<int32> CVarRayTracingSimulatedInstanceCount(
	TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.SimulationCount"),
	1,
	TEXT("Maximum number of instances to simulate per instanced static mesh, presently capped to 256")
);


class FDummyFloatBuffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI() override
	{
		// Create the texture RHI.  		
		FRHIResourceCreateInfo CreateInfo(TEXT("DummyFloatBuffer"));

		const int32 NumFloats = 4;
		VertexBufferRHI = RHICreateVertexBuffer(sizeof(float)*NumFloats, BUF_Static | BUF_ShaderResource, CreateInfo);

		float* BufferData = (float*)RHILockBuffer(VertexBufferRHI, 0, sizeof(float)*NumFloats, RLM_WriteOnly);
		FMemory::Memzero(BufferData, sizeof(float)*NumFloats);
		RHIUnlockBuffer(VertexBufferRHI);

		// Create a view of the buffer
		ShaderResourceViewRHI = RHICreateShaderResourceView(VertexBufferRHI, sizeof(float), PF_R32_FLOAT);
	}
};

TGlobalResource<FDummyFloatBuffer> GDummyFloatBuffer;

FInstancedStaticMeshDelegates::FOnInstanceIndexUpdated FInstancedStaticMeshDelegates::OnInstanceIndexUpdated;

/** InstancedStaticMeshInstance hit proxy */
void HInstancedStaticMeshInstance::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Component);
}

FTypedElementHandle HInstancedStaticMeshInstance::GetElementHandle() const
{
#if WITH_EDITOR
	if (Component)
	{
		// Prefer per-instance selection if available
		// This may fail to return a handle if the feature is disabled, or if per-instance editing is disabled for this component
		if (FTypedElementHandle ElementHandle = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Component, InstanceIndex))
		{
			return ElementHandle;
		}

		// If per-instance selection isn't possible, fallback to general per-component selection (which may choose to select the owner actor instead)
		return UEngineElementsLibrary::AcquireEditorComponentElementHandle(Component);
	}
#endif	// WITH_EDITOR
	return FTypedElementHandle();
}

FInstanceUpdateCmdBuffer::FInstanceUpdateCmdBuffer()
	: NumAdds(0)
	, NumEdits(0)
{
}

void FInstanceUpdateCmdBuffer::HideInstance(int32 RenderIndex)
{
	check(RenderIndex >= 0);

	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FInstanceUpdateCmdBuffer::Hide;

	Edit();
}

void FInstanceUpdateCmdBuffer::AddInstance(const FMatrix& InTransform)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = INDEX_NONE;
	Cmd.Type = FInstanceUpdateCmdBuffer::Add;
	Cmd.XForm = InTransform;

	NumAdds++;
	Edit();
}

void FInstanceUpdateCmdBuffer::UpdateInstance(int32 RenderIndex, const FMatrix& InTransform)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FInstanceUpdateCmdBuffer::Update;
	Cmd.XForm = InTransform;

	Edit();
}

void FInstanceUpdateCmdBuffer::SetEditorData(int32 RenderIndex, const FColor& Color, bool bSelected)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FInstanceUpdateCmdBuffer::EditorData;
	Cmd.HitProxyColor = Color;
	Cmd.bSelected = bSelected;

	Edit();
}

void FInstanceUpdateCmdBuffer::SetLightMapData(int32 RenderIndex, const FVector2D& LightmapUVBias)
{
	// We only support 1 command to update lightmap/shadowmap
	bool CommandExist = false;

	for (FInstanceUpdateCommand& Cmd : Cmds)
	{
		if (Cmd.Type == FInstanceUpdateCmdBuffer::LightmapData && Cmd.InstanceIndex == RenderIndex)
		{
			CommandExist = true;
			Cmd.LightmapUVBias = LightmapUVBias;
			break;
		}
	}

	if (!CommandExist)
	{
		FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
		Cmd.InstanceIndex = RenderIndex;
		Cmd.Type = FInstanceUpdateCmdBuffer::LightmapData;
		Cmd.LightmapUVBias = LightmapUVBias;
	}

	Edit();
}

void FInstanceUpdateCmdBuffer::SetShadowMapData(int32 RenderIndex, const FVector2D& ShadowmapUVBias)
{
	// We only support 1 command to update lightmap/shadowmap
	bool CommandExist = false;

	for (FInstanceUpdateCommand& Cmd : Cmds)
	{
		if (Cmd.Type == FInstanceUpdateCmdBuffer::LightmapData && Cmd.InstanceIndex == RenderIndex)
		{
			CommandExist = true;
			Cmd.ShadowmapUVBias = ShadowmapUVBias;
			break;
		}
	}

	if (!CommandExist)
	{
		FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
		Cmd.InstanceIndex = RenderIndex;
		Cmd.Type = FInstanceUpdateCmdBuffer::LightmapData;
		Cmd.ShadowmapUVBias = ShadowmapUVBias;
	}

	Edit();
}

void FInstanceUpdateCmdBuffer::SetCustomData(int32 RenderIndex, const TArray<float>& CustomDataFloats)
{
	bool CommandExist = false;

	/*for (FInstanceUpdateCommand& Cmd : Cmds)
	{
		if (Cmd.Type == FInstanceUpdateCmdBuffer::CustomData && Cmd.InstanceIndex == RenderIndex)
		{
			CommandExist = true;
			for (int32 i = 0; i < MAX_CUSTOM_DATA_VECTORS; ++i)
				Cmd.CustomDataVector[i] = CustomDataVector[i];
			break;
		}
	}*/

	if (!CommandExist)
	{
		FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
		Cmd.InstanceIndex = RenderIndex;
		Cmd.Type = FInstanceUpdateCmdBuffer::CustomData;
		Cmd.CustomDataFloats = CustomDataFloats;
	}

	Edit();
}

void FInstanceUpdateCmdBuffer::ResetInlineCommands()
{
	Cmds.Empty();
	NumAdds = 0;
}

void FInstanceUpdateCmdBuffer::Edit()
{
	NumEdits++;
}

void FInstanceUpdateCmdBuffer::Reset()
{
	Cmds.Empty();
	NumAdds = 0;
	NumEdits = 0;
}

FStaticMeshInstanceBuffer::FStaticMeshInstanceBuffer(ERHIFeatureLevel::Type InFeatureLevel, bool InRequireCPUAccess, bool bDeferGPUUploadIn)
	: FRenderResource(InFeatureLevel)
	, RequireCPUAccess(InRequireCPUAccess)
	, bDeferGPUUpload(bDeferGPUUploadIn)
	, bFlushToGPUPending(false)
{
}

FStaticMeshInstanceBuffer::~FStaticMeshInstanceBuffer()
{
	CleanUp();
}

/** Delete existing resources */
void FStaticMeshInstanceBuffer::CleanUp()
{
	InstanceData.Reset();
}

void FStaticMeshInstanceBuffer::InitFromPreallocatedData(FStaticMeshInstanceData& Other)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_InitFromPreallocatedData);

	InstanceData = MakeShared<FStaticMeshInstanceData, ESPMode::ThreadSafe>();
	FMemory::Memswap(&Other, InstanceData.Get(), sizeof(FStaticMeshInstanceData));
	InstanceData->SetAllowCPUAccess(RequireCPUAccess);
}

void FStaticMeshInstanceBuffer::UpdateFromCommandBuffer_Concurrent(FInstanceUpdateCmdBuffer& CmdBuffer)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_UpdateFromCommandBuffer_Concurrent);
	
	FStaticMeshInstanceBuffer* InstanceBuffer = this; 
	FInstanceUpdateCmdBuffer* NewCmdBuffer = new FInstanceUpdateCmdBuffer();
	FMemory::Memswap(&CmdBuffer, NewCmdBuffer, sizeof(FInstanceUpdateCmdBuffer));
	
	// leave NumEdits unchanged in commandbuffer
	CmdBuffer.NumEdits = NewCmdBuffer->NumEdits; 
	CmdBuffer.ResetInlineCommands();
		
	ENQUEUE_RENDER_COMMAND(InstanceBuffer_UpdateFromPreallocatedData)(
		[InstanceBuffer, NewCmdBuffer](FRHICommandListImmediate& RHICmdList)
		{
			InstanceBuffer->UpdateFromCommandBuffer_RenderThread(*NewCmdBuffer);
			delete NewCmdBuffer;
		});
}

void FStaticMeshInstanceBuffer::UpdateFromCommandBuffer_RenderThread(FInstanceUpdateCmdBuffer& CmdBuffer)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_UpdateFromCommandBuffer_RenderThread);
	
	int32 NumCommands = CmdBuffer.NumInlineCommands();
	int32 NumAdds = CmdBuffer.NumAdds;
	int32 AddIndex = INDEX_NONE;

	if (NumAdds > 0)
	{
		AddIndex = InstanceData->GetNumInstances();
		int32 NewNumInstances = NumAdds + InstanceData->GetNumInstances();

		InstanceData->AllocateInstances(NewNumInstances, InstanceData->GetNumCustomDataFloats(), GIsEditor ? EResizeBufferFlags::AllowSlackOnGrow | EResizeBufferFlags::AllowSlackOnReduce : EResizeBufferFlags::None, false); // In Editor always permit overallocation, to prevent too much realloc
	}

	for (int32 i = 0; i < NumCommands; ++i)
	{
		const auto& Cmd = CmdBuffer.Cmds[i];

		int32 InstanceIndex = Cmd.Type != FInstanceUpdateCmdBuffer::Add ? Cmd.InstanceIndex : AddIndex++;
		if (!ensure(InstanceData->IsValidIndex(InstanceIndex)))
		{
			continue;
		}

		switch (Cmd.Type)
		{
		case FInstanceUpdateCmdBuffer::Add:
			InstanceData->SetInstance(InstanceIndex, Cmd.XForm, 0);
			break;
		case FInstanceUpdateCmdBuffer::Hide:
			InstanceData->NullifyInstance(InstanceIndex);
			break;
		case FInstanceUpdateCmdBuffer::Update:
			InstanceData->SetInstance(InstanceIndex, Cmd.XForm, 0);
			break;
		case FInstanceUpdateCmdBuffer::EditorData:
			InstanceData->SetInstanceEditorData(InstanceIndex, Cmd.HitProxyColor, Cmd.bSelected);
			break;
		case FInstanceUpdateCmdBuffer::LightmapData:
			InstanceData->SetInstanceLightMapData(InstanceIndex, Cmd.LightmapUVBias, Cmd.ShadowmapUVBias);
			break;
		case FInstanceUpdateCmdBuffer::CustomData:
			for (int32 j = 0; j < InstanceData->GetNumCustomDataFloats(); ++j)
			{
				InstanceData->SetInstanceCustomData(Cmd.InstanceIndex, j, Cmd.CustomDataFloats[j]);
			}
			break;
		default:
			check(false);
		}
	}

	if (!CondSetFlushToGPUPending())
	{
		UpdateRHI();
	}
}

/**
 * Specialized assignment operator, only used when importing LOD's.  
 */
void FStaticMeshInstanceBuffer::operator=(const FStaticMeshInstanceBuffer &Other)
{
	checkf(0, TEXT("Unexpected assignment call"));
}

void FStaticMeshInstanceBuffer::InitRHI()
{
	check(InstanceData);
	if (InstanceData->GetNumInstances() > 0)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_InitRHI);
		SCOPED_LOADTIMER(FStaticMeshInstanceBuffer_InitRHI);

		LLM_SCOPE(ELLMTag::InstancedMesh);
		auto AccessFlags = BUF_Static;
		CreateVertexBuffer(InstanceData->GetOriginResourceArray(), AccessFlags | BUF_ShaderResource, 16, PF_A32B32G32R32F, InstanceOriginBuffer.VertexBufferRHI, InstanceOriginSRV);
		CreateVertexBuffer(InstanceData->GetTransformResourceArray(), AccessFlags | BUF_ShaderResource, InstanceData->GetTranslationUsesHalfs() ? 8 : 16, InstanceData->GetTranslationUsesHalfs() ? PF_FloatRGBA : PF_A32B32G32R32F, InstanceTransformBuffer.VertexBufferRHI, InstanceTransformSRV);
		CreateVertexBuffer(InstanceData->GetLightMapResourceArray(), AccessFlags | BUF_ShaderResource, 8, PF_R16G16B16A16_SNORM, InstanceLightmapBuffer.VertexBufferRHI, InstanceLightmapSRV);
		if (InstanceData->GetNumCustomDataFloats() > 0)
		{
			CreateVertexBuffer(InstanceData->GetCustomDataResourceArray(), AccessFlags | BUF_ShaderResource, 4, PF_R32_FLOAT, InstanceCustomDataBuffer.VertexBufferRHI, InstanceCustomDataSRV);
			// Make sure we still create custom data SRV on platforms that do not support/use MVF 
			if (InstanceCustomDataSRV == nullptr)
			{
				InstanceCustomDataSRV = RHICreateShaderResourceView(InstanceCustomDataBuffer.VertexBufferRHI, 4, PF_R32_FLOAT);
			}
		}
		else
		{
			InstanceCustomDataSRV = GDummyFloatBuffer.ShaderResourceViewRHI;
		}
	}
}

void FStaticMeshInstanceBuffer::ReleaseRHI()
{
	InstanceOriginSRV.SafeRelease();
	InstanceTransformSRV.SafeRelease();
	InstanceLightmapSRV.SafeRelease();
	InstanceCustomDataSRV.SafeRelease();

	InstanceOriginBuffer.ReleaseRHI();
	InstanceTransformBuffer.ReleaseRHI();
	InstanceLightmapBuffer.ReleaseRHI();
	InstanceCustomDataBuffer.ReleaseRHI();
}

void FStaticMeshInstanceBuffer::InitResource()
{
	FRenderResource::InitResource();
	InstanceOriginBuffer.InitResource();
	InstanceTransformBuffer.InitResource();
	InstanceLightmapBuffer.InitResource();
	InstanceCustomDataBuffer.InitResource();
}

void FStaticMeshInstanceBuffer::ReleaseResource()
{
	FRenderResource::ReleaseResource();
	InstanceOriginBuffer.ReleaseResource();
	InstanceTransformBuffer.ReleaseResource();
	InstanceLightmapBuffer.ReleaseResource();
	InstanceCustomDataBuffer.ReleaseResource();
}

SIZE_T FStaticMeshInstanceBuffer::GetResourceSize() const
{
	if (InstanceData && InstanceData->GetNumInstances() > 0)
	{
		return InstanceData->GetResourceSize();
	}
	return 0;
}

void FStaticMeshInstanceBuffer::CreateVertexBuffer(FResourceArrayInterface* InResourceArray, EBufferUsageFlags InUsage, uint32 InStride, uint8 InFormat, FBufferRHIRef& OutVertexBufferRHI, FShaderResourceViewRHIRef& OutInstanceSRV)
{
	check(InResourceArray);
	check(InResourceArray->GetResourceDataSize() > 0);

	// TODO: possibility over allocated the vertex buffer when we support partial update for when working in the editor
	FRHIResourceCreateInfo CreateInfo(TEXT("FStaticMeshInstanceBuffer"), InResourceArray);
	OutVertexBufferRHI = RHICreateVertexBuffer(InResourceArray->GetResourceDataSize(), InUsage, CreateInfo);
	
	if (RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
	{
		OutInstanceSRV = RHICreateShaderResourceView(OutVertexBufferRHI, InStride, InFormat);
	}
}

void FStaticMeshInstanceBuffer::BindInstanceVertexBuffer(const class FVertexFactory* VertexFactory, FInstancedStaticMeshDataType& InstancedStaticMeshData) const
{
	if (InstanceData->GetNumInstances())
	{
		if (RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
		{
			check(InstanceOriginSRV);
			check(InstanceTransformSRV);
			check(InstanceLightmapSRV);
		}
		check(InstanceCustomDataSRV); // Should not be nullptr, but can be assigned a dummy buffer
	}

	{
		InstancedStaticMeshData.InstanceOriginSRV = InstanceOriginSRV;
		InstancedStaticMeshData.InstanceTransformSRV = InstanceTransformSRV;
		InstancedStaticMeshData.InstanceLightmapSRV = InstanceLightmapSRV;
		InstancedStaticMeshData.InstanceCustomDataSRV = InstanceCustomDataSRV;
		InstancedStaticMeshData.NumCustomDataFloats = InstanceData->GetNumCustomDataFloats();
	}

	{
		InstancedStaticMeshData.InstanceOriginComponent = FVertexStreamComponent(
			&InstanceOriginBuffer,
			0,
			16,
			VET_Float4,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);

		EVertexElementType TransformType = InstanceData->GetTranslationUsesHalfs() ? VET_Half4 : VET_Float4;
		uint32 TransformStride = InstanceData->GetTranslationUsesHalfs() ? 8 : 16;

		InstancedStaticMeshData.InstanceTransformComponent[0] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			0 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
		InstancedStaticMeshData.InstanceTransformComponent[1] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			1 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
		InstancedStaticMeshData.InstanceTransformComponent[2] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			2 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);

		InstancedStaticMeshData.InstanceLightmapAndShadowMapUVBiasComponent = FVertexStreamComponent(
			&InstanceLightmapBuffer,
			0,
			8,
			VET_Short4N,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
	}
}

void FStaticMeshInstanceBuffer::FlushGPUUpload()
{
	if (bFlushToGPUPending)
	{
		check(bDeferGPUUpload);

		if (!IsInitialized())
		{
			InitResource();
		}
		else
		{
			UpdateRHI();
		}
		bFlushToGPUPending = false;
	}
}


void FStaticMeshInstanceData::Serialize(FArchive& Ar)
{	
	Ar.UsingCustomVersion(FRenderingObjectVersion::GUID);

	const bool bCookConvertTransformsToFullFloat = Ar.IsCooking() && bUseHalfFloat && !Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::HalfFloatVertexFormat);

	if (bCookConvertTransformsToFullFloat)
	{
		bool bSaveUseHalfFloat = false;
		Ar << bSaveUseHalfFloat;
	}
	else
	{
		Ar << bUseHalfFloat;
	}

	Ar << NumInstances;

	if (!Ar.IsLoading() || Ar.CustomVer(FRenderingObjectVersion::GUID) >= FRenderingObjectVersion::PerInstanceCustomData)
	{
		Ar << NumCustomDataFloats;
	}

	if (Ar.IsLoading())
	{
		AllocateBuffers(NumInstances);
	}

	InstanceOriginData->Serialize(Ar);
	InstanceLightmapData->Serialize(Ar);

	if (bCookConvertTransformsToFullFloat)
	{
		TStaticMeshVertexData<FInstanceTransformMatrix<float>> FullInstanceTransformData;
		FullInstanceTransformData.ResizeBuffer(NumInstances);

		FInstanceTransformMatrix<FFloat16>* Src = (FInstanceTransformMatrix<FFloat16>*)InstanceTransformData->GetDataPointer();
		FInstanceTransformMatrix<float>* Dest = (FInstanceTransformMatrix<float>*)FullInstanceTransformData.GetDataPointer();
		for (int32 Idx = 0; Idx < NumInstances; Idx++)
		{
			Dest->InstanceTransform1[0] = Src->InstanceTransform1[0];
			Dest->InstanceTransform1[1] = Src->InstanceTransform1[1];
			Dest->InstanceTransform1[2] = Src->InstanceTransform1[2];
			Dest->InstanceTransform1[3] = Src->InstanceTransform1[3];
			Dest->InstanceTransform2[0] = Src->InstanceTransform2[0];
			Dest->InstanceTransform2[1] = Src->InstanceTransform2[1];
			Dest->InstanceTransform2[2] = Src->InstanceTransform2[2];
			Dest->InstanceTransform2[3] = Src->InstanceTransform2[3];
			Dest->InstanceTransform3[0] = Src->InstanceTransform3[0];
			Dest->InstanceTransform3[1] = Src->InstanceTransform3[1];
			Dest->InstanceTransform3[2] = Src->InstanceTransform3[2];
			Dest->InstanceTransform3[3] = Src->InstanceTransform3[3];
			Src++;
			Dest++;
		}

		FullInstanceTransformData.Serialize(Ar);
	}
	else
	{
		InstanceTransformData->Serialize(Ar);
	}

	if (!Ar.IsLoading() || Ar.CustomVer(FRenderingObjectVersion::GUID) >= FRenderingObjectVersion::PerInstanceCustomData)
	{
		InstanceCustomData->Serialize(Ar);
	}

	if (Ar.IsLoading())
	{
		InstanceOriginDataPtr = InstanceOriginData->GetDataPointer();
		InstanceLightmapDataPtr = InstanceLightmapData->GetDataPointer();
		InstanceTransformDataPtr = InstanceTransformData->GetDataPointer();
		InstanceCustomDataPtr = InstanceCustomData->GetDataPointer();
	}
}


/**
 * Should we cache the material's shadertype on this platform with this vertex factory? 
 */
bool FInstancedStaticMeshVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return (Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
			&& FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FInstancedStaticMeshVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	const bool ContainsManualVertexFetch = OutEnvironment.GetDefinitions().Contains("MANUAL_VERTEX_FETCH");
	if (!ContainsManualVertexFetch && RHISupportsManualVertexFetch(Parameters.Platform))
	{
		OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), TEXT("1"));
	}

	if (UseGPUScene(Parameters.Platform))
	{
		// USE_INSTANCE_CULLING - set up additional instancing attributes (basic instancing is the default)
		OutEnvironment.SetDefine(TEXT("USE_INSTANCE_CULLING"), TEXT("1"));
	}
	else
	{
		OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));
	}

	if (IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
	{
		OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES);
	}
	else
	{
		// On mobile dithered LOD transition has to be explicitly enabled in material and project settings
		OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), Parameters.MaterialParameters.bIsDitheredLODTransition && ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES);
	}

	FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
}


/**
 * Copy the data from another vertex factory
 * @param Other - factory to copy from
 */
void FInstancedStaticMeshVertexFactory::Copy(const FInstancedStaticMeshVertexFactory& Other)
{
	FInstancedStaticMeshVertexFactory* VertexFactory = this;
	const FDataType* DataCopy = &Other.Data;
	ENQUEUE_RENDER_COMMAND(FInstancedStaticMeshVertexFactoryCopyData)(
	[VertexFactory, DataCopy](FRHICommandListImmediate& RHICmdList)
	{
		VertexFactory->Data = *DataCopy;
	});
	BeginUpdateResourceRHI(this);
}

void FInstancedStaticMeshVertexFactory::InitRHI()
{
	SCOPED_LOADTIMER(FInstancedStaticMeshVertexFactory_InitRHI);

	check(HasValidFeatureLevel());

#if !ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES // position(and normal) only shaders cannot work with dithered LOD
	// If the vertex buffer containing position is not the same vertex buffer containing the rest of the data,
	// then initialize PositionStream and PositionDeclaration.
	if(Data.PositionComponent.VertexBuffer != Data.TangentBasisComponents[0].VertexBuffer)
	{
		auto AddDeclaration = [&Data](EVertexInputStreamType InputStreamType, bool bInstanced, bool bAddNormal)
		{
			FVertexDeclarationElementList StreamElements;
			StreamElements.Add(AccessPositionStreamComponent(Data.PositionComponent, 0));

			bAddNormal = bAddNormal && Data.TangentBasisComponents[1].VertexBuffer != NULL;
			if (bAddNormal)
			{
				StreamElements.Add(AccessStreamComponent(Data.TangentBasisComponents[1], 2, InputStreamType));
			}

			if (bInstanced)
			{
				// toss in the instanced location stream
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceOriginComponent, 8));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[0], 9));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[1], 10));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[2], 11));
			}

			InitDeclaration(StreamElements, InputStreamType);
		};
		AddDeclaration(EVertexInputStreamType::PositionOnly, bInstanced, false);
		AddDeclaration(EVertexInputStreamType::PositionAndNormalOnly, bInstanced, true);
	}
#endif

	FVertexDeclarationElementList Elements;
	if(Data.PositionComponent.VertexBuffer != NULL)
	{
		Elements.Add(AccessStreamComponent(Data.PositionComponent,0));
	}

	// only tangent,normal are used by the stream. the binormal is derived in the shader
	uint8 TangentBasisAttributes[2] = { 1, 2 };
	for(int32 AxisIndex = 0;AxisIndex < 2;AxisIndex++)
	{
		if(Data.TangentBasisComponents[AxisIndex].VertexBuffer != NULL)
		{
			Elements.Add(AccessStreamComponent(Data.TangentBasisComponents[AxisIndex],TangentBasisAttributes[AxisIndex]));
		}
	}

	if (Data.ColorComponentsSRV == nullptr)
	{
		Data.ColorComponentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
		Data.ColorIndexMask = 0;
	}

	if(Data.ColorComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.ColorComponent,3));
	}
	else
	{
		//If the mesh has no color component, set the null color buffer on a new stream with a stride of 0.
		//This wastes 4 bytes of bandwidth per vertex, but prevents having to compile out twice the number of vertex factories.
		FVertexStreamComponent NullColorComponent(&GNullColorVertexBuffer, 0, 0, VET_Color, EVertexStreamUsage::ManualFetch);
		Elements.Add(AccessStreamComponent(NullColorComponent, 3));
	}

	if(Data.TextureCoordinates.Num())
	{
		const int32 BaseTexCoordAttribute = 4;
		for(int32 CoordinateIndex = 0;CoordinateIndex < Data.TextureCoordinates.Num();CoordinateIndex++)
		{
			Elements.Add(AccessStreamComponent(
				Data.TextureCoordinates[CoordinateIndex],
				BaseTexCoordAttribute + CoordinateIndex
				));
		}

		for(int32 CoordinateIndex = Data.TextureCoordinates.Num(); CoordinateIndex < (InstancedStaticMeshMaxTexCoord + 1) / 2; CoordinateIndex++)
		{
			Elements.Add(AccessStreamComponent(
				Data.TextureCoordinates[Data.TextureCoordinates.Num() - 1],
				BaseTexCoordAttribute + CoordinateIndex
				));
		}
	}
	
	// on mobile with GPUScene enabled instanced attributes[8-12] are used for a general auto-instancing
	// so we add them only for desktop or if mobile has GPUScene disabled
	// FIXME mobile: instanced attributes encode some editor related data as well (selection etc), need to split it into separate SRV as it's not supported with auto-instancing
	uint8 AutoInstancingAttr_Mobile = 8;
	const bool bMobileUsesGPUScene = MobileSupportsGPUScene();
	
	if (GetFeatureLevel() > ERHIFeatureLevel::ES3_1 || !bMobileUsesGPUScene)
	{
		// toss in the instanced location stream
		check(Data.InstanceOriginComponent.VertexBuffer);
		if (Data.InstanceOriginComponent.VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(Data.InstanceOriginComponent, 8));
		}

		check(Data.InstanceTransformComponent[0].VertexBuffer);
		if (Data.InstanceTransformComponent[0].VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[0], 9));
			Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[1], 10));
			Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[2], 11));
		}

		if (Data.InstanceLightmapAndShadowMapUVBiasComponent.VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(Data.InstanceLightmapAndShadowMapUVBiasComponent,12));
		}

		// Do not add general auto-instancing attributes for mobile
		AutoInstancingAttr_Mobile = 0xff;
	}
	
	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, Elements, 13, AutoInstancingAttr_Mobile);

	if(Data.LightMapCoordinateComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.LightMapCoordinateComponent,15));
	}
	else if(Data.TextureCoordinates.Num())
	{
		Elements.Add(AccessStreamComponent(Data.TextureCoordinates[0],15));
	}
		
	// we don't need per-vertex shadow or lightmap rendering
	InitDeclaration(Elements);

	{
		FInstancedStaticMeshVertexFactoryUniformShaderParameters UniformParameters;
		UniformParameters.VertexFetch_InstanceOriginBuffer = GetInstanceOriginSRV();
		UniformParameters.VertexFetch_InstanceTransformBuffer = GetInstanceTransformSRV();
		UniformParameters.VertexFetch_InstanceLightmapBuffer = GetInstanceLightmapSRV();
		UniformParameters.InstanceCustomDataBuffer = GetInstanceCustomDataSRV();
		UniformParameters.NumCustomDataFloats = Data.NumCustomDataFloats;
		UniformBuffer = TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters>::CreateUniformBufferImmediate(UniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	}
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FInstancedStaticMeshVertexFactory, SF_Vertex, FInstancedStaticMeshVertexFactoryShaderParameters);
#if RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FInstancedStaticMeshVertexFactory, SF_RayHitGroup, FInstancedStaticMeshVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FInstancedStaticMeshVertexFactory, SF_Compute, FInstancedStaticMeshVertexFactoryShaderParameters);
#endif

IMPLEMENT_VERTEX_FACTORY_TYPE(FInstancedStaticMeshVertexFactory,"/Engine/Private/LocalVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsPositionOnly
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsRayTracing
	| EVertexFactoryFlags::SupportsRayTracingDynamicGeometry
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
);

void FInstancedStaticMeshRenderData::InitVertexFactories()
{
	// Allocate the vertex factories for each LOD
	for (int32 LODIndex = 0; LODIndex < LODModels.Num(); LODIndex++)
	{
		VertexFactories.Add(new FInstancedStaticMeshVertexFactory(FeatureLevel));
	}

	const int32 LightMapCoordinateIndex = Component->GetStaticMesh()->GetLightMapCoordinateIndex();
	ENQUEUE_RENDER_COMMAND(InstancedStaticMeshRenderData_InitVertexFactories)(
		[this, LightMapCoordinateIndex](FRHICommandListImmediate& RHICmdList)
	{
		PerInstanceRenderData->InstanceBuffer.FlushGPUUpload();

		for (int32 LODIndex = 0; LODIndex < VertexFactories.Num(); LODIndex++)
		{
			const FStaticMeshLODResources* RenderData = &LODModels[LODIndex];

			FInstancedStaticMeshVertexFactory::FDataType Data;
			// Assign to the vertex factory for this LOD.
			FInstancedStaticMeshVertexFactory& VertexFactory = VertexFactories[LODIndex];

			RenderData->VertexBuffers.PositionVertexBuffer.BindPositionVertexBuffer(&VertexFactory, Data);
			RenderData->VertexBuffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(&VertexFactory, Data);
			RenderData->VertexBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(&VertexFactory, Data);
			if (LightMapCoordinateIndex < (int32)RenderData->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() && LightMapCoordinateIndex >= 0)
			{
				RenderData->VertexBuffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(&VertexFactory, Data, LightMapCoordinateIndex);
			}

			if (RenderData->bHasColorVertexData)
			{
				RenderData->VertexBuffers.ColorVertexBuffer.BindColorVertexBuffer(&VertexFactory, Data);
			}
			else
			{
				FColorVertexBuffer::BindDefaultColorVertexBuffer(&VertexFactory, Data, FColorVertexBuffer::NullBindStride::FColorSizeForComponentOverride);
			}
			
			check(PerInstanceRenderData);
			
			PerInstanceRenderData->InstanceBuffer.BindInstanceVertexBuffer(&VertexFactory, Data);

			VertexFactory.SetData(Data);
			VertexFactory.InitResource();
		}
	});
}


FPerInstanceRenderData::FPerInstanceRenderData(FStaticMeshInstanceData& Other, ERHIFeatureLevel::Type InFeaureLevel, bool InRequireCPUAccess, FBox InBounds, bool bTrack, bool bDeferGPUUploadIn)
	: ResourceSize(InRequireCPUAccess ? Other.GetResourceSize() : 0)
	, InstanceBuffer(InFeaureLevel, InRequireCPUAccess, bDeferGPUUploadIn)
	, InstanceLocalBounds(InBounds)
	, bTrackBounds(bTrack)
	, bBoundsTransformsDirty(true)
{
	InstanceBuffer.InitFromPreallocatedData(Other);
	InstanceBuffer_GameThread = InstanceBuffer.InstanceData;
	if (!InstanceBuffer.CondSetFlushToGPUPending())
	{
		BeginInitResource(&InstanceBuffer);
	}
	UpdateBoundsTransforms_Concurrent();
}

FPerInstanceRenderData::~FPerInstanceRenderData()
{
	InstanceBuffer_GameThread.Reset();
	// Should be always destructed on rendering thread
	InstanceBuffer.ReleaseResource();
}

void FPerInstanceRenderData::UpdateFromPreallocatedData(FStaticMeshInstanceData& InOther)
{
	InstanceBuffer.RequireCPUAccess = (InOther.GetOriginResourceArray()->GetAllowCPUAccess() || InOther.GetTransformResourceArray()->GetAllowCPUAccess() || InOther.GetLightMapResourceArray()->GetAllowCPUAccess()) ? true : InstanceBuffer.RequireCPUAccess;
	ResourceSize = InstanceBuffer.RequireCPUAccess ? InOther.GetResourceSize() : 0;

	InOther.SetAllowCPUAccess(InstanceBuffer.RequireCPUAccess);

	InstanceBuffer_GameThread = MakeShared<FStaticMeshInstanceData, ESPMode::ThreadSafe>();
	FMemory::Memswap(&InOther, InstanceBuffer_GameThread.Get(), sizeof(FStaticMeshInstanceData));

	typedef TSharedPtr<FStaticMeshInstanceData, ESPMode::ThreadSafe> FStaticMeshInstanceDataPtr;

	FStaticMeshInstanceDataPtr InInstanceBufferDataPtr = InstanceBuffer_GameThread;
	FStaticMeshInstanceBuffer* InInstanceBuffer = &InstanceBuffer;
	ENQUEUE_RENDER_COMMAND(FInstanceBuffer_UpdateFromPreallocatedData)(
		[InInstanceBufferDataPtr, InInstanceBuffer, this](FRHICommandListImmediate& RHICmdList)
		{
			// The assignment to InstanceData shared pointer kills the old data
			// If UpdateBoundsTask is in-flight it will crash
			EnsureInstanceDataUpdated();
			InInstanceBuffer->InstanceData = InInstanceBufferDataPtr;
			if (!InInstanceBuffer->CondSetFlushToGPUPending())
			{
				InInstanceBuffer->UpdateRHI();
			}
			UpdateBoundsTransforms_Concurrent();
		}
	);
}

void FPerInstanceRenderData::UpdateBoundsTransforms_Concurrent()
{
	// Enqueue a render command to create a task to update the buffer data.
	// Yes double-wrapping a lambda looks a little silly, but the only safe way to update the render data
	// is to issue this task from the rendering thread.
	ENQUEUE_RENDER_COMMAND(FInstanceBuffer_UpdateBoundsTransforms)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			bBoundsTransformsDirty = true;
			if (!IsRayTracingEnabled() || !CVarRayTracingRenderInstances.GetValueOnRenderThread())
			{
				return;
			}

			FGraphEventArray Prerequisites{};
			if (UpdateBoundsTask.IsValid())
			{
				// There's already a task either in flight or unconsumed, but the instance data has now changed so its result might be incorrect.
				// This new task should run after the first one completes, so make the old one a prerequisite of the new one.
				Prerequisites = FGraphEventArray{ UpdateBoundsTask };
				UE_LOG(LogStaticMesh, Warning, TEXT("Unconsumed ISM bounds/transforms update task, we did more work than necessary"));
			}

			UpdateBoundsTask = FFunctionGraphTask::CreateAndDispatchWhenReady(
			[this]()
			{
				UpdateBoundsTransforms();
			},
			TStatId(),
			&Prerequisites
			);
		}
	);
}

void FPerInstanceRenderData::UpdateBoundsTransforms()
{
	const int32 InstanceCount = InstanceBuffer.GetNumInstances();
	PerInstanceTransforms.Empty(InstanceCount);

	if (bTrackBounds)
	{
		FBoxSphereBounds LocalBounds = FBoxSphereBounds(InstanceLocalBounds);
		PerInstanceBounds.Empty(InstanceCount);

		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
		{
			if (!InstanceBuffer.GetInstanceData() || !InstanceBuffer.GetInstanceData()->IsValidIndex(InstanceIndex))
		    {
			    continue;
		    }

			FRenderTransform InstTransform;
			InstanceBuffer.GetInstanceTransform(InstanceIndex, InstTransform);
			PerInstanceTransforms.Add(InstTransform);

			FBoxSphereBounds TransformedBounds = LocalBounds.TransformBy(InstTransform.ToMatrix());
			PerInstanceBounds.Add(FVector4(TransformedBounds.Origin, TransformedBounds.SphereRadius));
		}
	}
	else
	{
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
		{
			if (!InstanceBuffer.GetInstanceData() || !InstanceBuffer.GetInstanceData()->IsValidIndex(InstanceIndex))
		    {
			    continue;
		    }

			FRenderTransform InstTransform;
			InstanceBuffer.GetInstanceTransform(InstanceIndex, InstTransform);
			PerInstanceTransforms.Add(InstTransform);
		}
	}
}


void FPerInstanceRenderData::EnsureInstanceDataUpdated()
{
	check(IsInRenderingThread());

	// wait for bounds/transforms update to complete
	if (UpdateBoundsTask.IsValid())
	{
		UpdateBoundsTask->Wait(ENamedThreads::GetRenderThread_Local());
		UpdateBoundsTask.SafeRelease();
		bBoundsTransformsDirty = false;
	}

	// manually update if there is no pending update task
	if (bBoundsTransformsDirty)
	{
		UpdateBoundsTransforms();
		bBoundsTransformsDirty = false;
	}
}

const TArray<FVector4f>& FPerInstanceRenderData::GetPerInstanceBounds()
{
	check(bTrackBounds);
	EnsureInstanceDataUpdated();
	return PerInstanceBounds;
}

const TArray<FRenderTransform>& FPerInstanceRenderData::GetPerInstanceTransforms()
{
	EnsureInstanceDataUpdated();
	return PerInstanceTransforms;
}

void FPerInstanceRenderData::UpdateFromCommandBuffer(FInstanceUpdateCmdBuffer& CmdBuffer)
{
	// UpdateFromCommandBuffer reallocates InstanceData in InstanceBuffer
	// If UpdateBoundsTask is in-flight it will crash
	ENQUEUE_RENDER_COMMAND(EnsureInstanceDataUpdatedCmd)(
		[this](FRHICommandList&) {
		EnsureInstanceDataUpdated();
	});

	InstanceBuffer.UpdateFromCommandBuffer_Concurrent(CmdBuffer);
	UpdateBoundsTransforms_Concurrent();
}

SIZE_T FInstancedStaticMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FInstancedStaticMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_InstancedStaticMeshSceneProxy_GetMeshElements);

	const bool bSelectionRenderEnabled = GIsEditor && ViewFamily.EngineShowFlags.Selection;

	// If the first pass rendered selected instances only, we need to render the deselected instances in a second pass
	const int32 NumSelectionGroups = (bSelectionRenderEnabled && bHasSelectedInstances) ? 2 : 1;

	const FInstancingUserData* PassUserData[2] =
	{
		bHasSelectedInstances && bSelectionRenderEnabled ? &UserData_SelectedInstances : &UserData_AllInstances,
		&UserData_DeselectedInstances
	};

	bool BatchRenderSelection[2] = 
	{
		bSelectionRenderEnabled && IsSelected(),
		false
	};

	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];

			for (int32 SelectionGroupIndex = 0; SelectionGroupIndex < NumSelectionGroups; SelectionGroupIndex++)
			{
				const int32 LODIndex = GetLOD(View);
				const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[LODIndex];

				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
				{
					const int32 NumBatches = GetNumMeshBatches();

					for (int32 BatchIndex = 0; BatchIndex < NumBatches; BatchIndex++)
					{
						FMeshBatch& MeshElement = Collector.AllocateMesh();

						if (GetMeshElement(LODIndex, BatchIndex, SectionIndex, GetDepthPriorityGroup(View), BatchRenderSelection[SelectionGroupIndex], true, MeshElement))
						{
							//@todo-rco this is only supporting selection on the first element
							MeshElement.Elements[0].UserData = PassUserData[SelectionGroupIndex];
							MeshElement.Elements[0].bUserDataIsColorVertexBuffer = false;
							MeshElement.bCanApplyViewModeOverrides = true;
							MeshElement.bUseSelectionOutline = BatchRenderSelection[SelectionGroupIndex];
							MeshElement.bUseWireframeSelectionColoring = BatchRenderSelection[SelectionGroupIndex];

							if (View->bRenderFirstInstanceOnly)
							{
								for (int32 ElementIndex = 0; ElementIndex < MeshElement.Elements.Num(); ElementIndex++)
								{
									MeshElement.Elements[ElementIndex].NumInstances = FMath::Min<uint32>(MeshElement.Elements[ElementIndex].NumInstances, 1);
								}
							}

							Collector.AddMesh(ViewIndex, MeshElement);
							INC_DWORD_STAT_BY(STAT_StaticMeshTriangles, MeshElement.GetNumPrimitives());
						}
					}
				}
			}
		}
	}
}

void FInstancedStaticMeshSceneProxy::SetupProxy(UInstancedStaticMeshComponent* InComponent)
{
#if WITH_EDITOR
	if (bHasSelectedInstances)
	{
		// if we have selected indices, mark scene proxy as selected.
		SetSelection_GameThread(true);
	}
#endif

	bAnySegmentUsesWorldPositionOffset = false;

	// Make sure all the materials are okay to be rendered as an instanced mesh.
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); LODIndex++)
	{
		FStaticMeshSceneProxy::FLODInfo& LODInfo = LODs[LODIndex];
		for (int32 SectionIndex = 0; SectionIndex < LODInfo.Sections.Num(); SectionIndex++)
		{
			FStaticMeshSceneProxy::FLODInfo::FSectionInfo& Section = LODInfo.Sections[SectionIndex];
			if (!Section.Material->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes))
			{
				Section.Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
			bAnySegmentUsesWorldPositionOffset |= Section.Material->GetRelevance_Concurrent(GMaxRHIFeatureLevel).bUsesWorldPositionOffset;
		}
	}

	// Copy the parameters for LOD - all instances
	UserData_AllInstances.MeshRenderData = InComponent->GetStaticMesh()->GetRenderData();
	UserData_AllInstances.StartCullDistance = InComponent->InstanceStartCullDistance;
	UserData_AllInstances.EndCullDistance = InComponent->InstanceEndCullDistance;
	UserData_AllInstances.InstancingOffset = InComponent->GetStaticMesh()->GetBoundingBox().GetCenter();
	UserData_AllInstances.MinLOD = ClampedMinLOD;
	UserData_AllInstances.bRenderSelected = true;
	UserData_AllInstances.bRenderUnselected = true;
	UserData_AllInstances.RenderData = nullptr;

	FVector MinScale(0);
	FVector MaxScale(0);
	InComponent->GetInstancesMinMaxScale(MinScale, MaxScale);

	UserData_AllInstances.AverageInstancesScale = MinScale + (MaxScale - MinScale) / 2.0f;

	// selected only
	UserData_SelectedInstances = UserData_AllInstances;
	UserData_SelectedInstances.bRenderUnselected = false;

	// unselected only
	UserData_DeselectedInstances = UserData_AllInstances;
	UserData_DeselectedInstances.bRenderSelected = false;

#if RHI_RAYTRACING
	bSupportRayTracing = InComponent->GetStaticMesh()->bSupportRayTracing;
#endif

	if (UseGPUScene(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel()))
	{
		const TArray<int32>& InstanceReorderTable = InComponent->InstanceReorderTable;
		bSupportsInstanceDataBuffer = true;

		InstanceSceneData.SetNum(InComponent->GetInstanceCount());

		const bool bValidPreviousData = InComponent->PerInstancePrevTransform.Num() == InComponent->GetInstanceCount();
		InstanceDynamicData.SetNumUninitialized(bValidPreviousData ? InComponent->GetInstanceCount() : 0);

		InstanceRandomID.SetNumZeroed(InComponent->GetInstanceCount()); // TODO: Only allocate if material bound which uses this
		InstanceLightShadowUVBias.SetNumZeroed(InComponent->GetInstanceCount()); // TODO: Only allocate if static lighting is enabled for the project
		InstanceCustomData.SetNumZeroed(InComponent->GetInstanceCount() * InComponent->NumCustomDataFloats);
		//InstanceCustomData = InComponent->PerInstanceSMCustomData; // TODO: Use this once the hacky reorder table is removed
		//check(InComponent->NumCustomDataFloats == 0 || (InstanceCustomData.Num() / InComponent->NumCustomDataFloats == InComponent->GetInstanceCount()));

		uint32 InstanceDataFlags = 0;
		InstanceDataFlags |= InstanceLightShadowUVBias.Num() > 0 ? INSTANCE_SCENE_DATA_FLAG_HAS_LIGHTSHADOW_UV_BIAS : 0u;
		InstanceDataFlags |= InstanceDynamicData.Num() > 0 ? INSTANCE_SCENE_DATA_FLAG_HAS_DYNAMIC_DATA : 0u;
		InstanceDataFlags |= InstanceCustomData.Num() > 0 ? INSTANCE_SCENE_DATA_FLAG_HAS_CUSTOM_DATA : 0u;
		InstanceDataFlags |= InstanceRandomID.Num() > 0 ? INSTANCE_SCENE_DATA_FLAG_HAS_RANDOM : 0u;

		for (int32 InInstanceIndex = 0; InInstanceIndex < InstanceSceneData.Num(); ++InInstanceIndex)
		{
			// Make sure the instance is initialized, regardless of below remapping
			{
				FPrimitiveInstance& TmpSceneData = InstanceSceneData[InInstanceIndex];
				TmpSceneData.LocalToPrimitive = FRenderTransform::Identity;
				TmpSceneData.LocalBounds = InComponent->GetStaticMesh()->GetBounds();
				TmpSceneData.NaniteHierarchyOffset = NANITE_INVALID_HIERARCHY_OFFSET;
				TmpSceneData.Flags = InstanceDataFlags;
			}

			int32 OutInstanceIndex = InInstanceIndex;
			// GPUCULL_TODO: After deleting instances in a HISM the InstanceReorderTable often contains nonsense, this is then corrected
			// by the async build, which re-creates the proxy in a nearby future frame. All this should be removed in favour of GPU-side culling.
			if (OutInstanceIndex < InstanceReorderTable.Num() && InstanceReorderTable[OutInstanceIndex] < InstanceSceneData.Num())
			{
				// Temporary workaround for out of bound array access
				// TODO: fix this properly!!!!!!
				OutInstanceIndex = InstanceReorderTable[OutInstanceIndex] != INDEX_NONE ? InstanceReorderTable[OutInstanceIndex] : OutInstanceIndex;
			}

			FPrimitiveInstance& SceneData = InstanceSceneData[OutInstanceIndex];


			FTransform InstanceTransform;
			InComponent->GetInstanceTransform(InInstanceIndex, InstanceTransform);
			SceneData.LocalToPrimitive = InstanceTransform.ToMatrixWithScale();

			if (bValidPreviousData)
			{
				FPrimitiveInstanceDynamicData& DynamicData = InstanceDynamicData[OutInstanceIndex];

				FTransform InstancePrevTransform;
				const bool bHasPrevTransform = InComponent->GetInstancePrevTransform(InInstanceIndex, InstancePrevTransform);
				if (ensure(bHasPrevTransform)) // Should always be true here
				{
					DynamicData.PrevLocalToPrimitive = InstancePrevTransform.ToMatrixWithScale();
				}
				else
				{
					DynamicData.PrevLocalToPrimitive = SceneData.LocalToPrimitive;
				}
			}

			if (InComponent->NumCustomDataFloats > 0)
			{
				const int32 SrcCustomDataOffset = InInstanceIndex  * InComponent->NumCustomDataFloats;
				const int32 DstCustomDataOffset = OutInstanceIndex * InComponent->NumCustomDataFloats;
				FMemory::Memcpy
				(
					&InstanceCustomData[DstCustomDataOffset],
					&InComponent->PerInstanceSMCustomData[SrcCustomDataOffset],
					InComponent->NumCustomDataFloats * sizeof(float)
				);
			}
		}
	}
}


void FInstancedStaticMeshSceneProxy::CreateRenderThreadResources()
{
	FStaticMeshSceneProxy::CreateRenderThreadResources();
	
	// Flush upload of GPU data for ISM/HISM
	if (ensure(InstancedRenderData.PerInstanceRenderData.IsValid()))
	{
		FStaticMeshInstanceBuffer& InstanceBuffer = InstancedRenderData.PerInstanceRenderData->InstanceBuffer;
		InstanceBuffer.FlushGPUUpload();
	}

	if (UseGPUScene(GetScene().GetShaderPlatform(), GetScene().GetFeatureLevel()))
	{
		bSupportsInstanceDataBuffer = true;
		// TODO: can the PerInstanceRenderData ever not be valid here?
		if (ensure(InstancedRenderData.PerInstanceRenderData.IsValid()))
		{
			const FStaticMeshInstanceBuffer& InstanceBuffer = InstancedRenderData.PerInstanceRenderData->InstanceBuffer;
			ensureMsgf(InstanceBuffer.RequireCPUAccess, TEXT("GPU-Scene instance culling requires CPU access to instance data for setup."));

			// This happens when this is actually a HISM and the data is not present in the component (which is true for landscape grass
			// which manages its own setup.
			if (InstanceSceneData.Num() == 0)
			{
				InstanceSceneData.SetNum(InstanceBuffer.GetNumInstances());
				for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
				{
					FPrimitiveInstance& SceneData = InstanceSceneData[InstanceIndex];
					SceneData.NaniteHierarchyOffset = NANITE_INVALID_HIERARCHY_OFFSET;
					// TODO: Probably need to set  these flags up properly?
					SceneData.Flags = 0U;
				}
			}

			// NOTE: we set up partial data in the construction of ISM proxy (yep, awful but the equally awful way the InstanceBuffer is maintained means complete data is not available)
			if (InstanceSceneData.Num() == InstanceBuffer.GetNumInstances())
			{
				const bool bHasLightMapData = InstanceLightShadowUVBias.Num() == InstanceSceneData.Num();
				const bool bHasRandomID = InstanceRandomID.Num() == InstanceSceneData.Num();

				for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
				{
					FPrimitiveInstance& SceneData = InstanceSceneData[InstanceIndex];
					SceneData.LocalBounds = StaticMeshBounds; // TODO: redundant setting
					InstanceBuffer.GetInstanceTransform(InstanceIndex, SceneData.LocalToPrimitive);

					if (bHasRandomID)
					{
						InstanceBuffer.GetInstanceRandomID(InstanceIndex, InstanceRandomID[InstanceIndex]);
					}

					if (bHasLightMapData)
					{
						InstanceBuffer.GetInstanceLightMapData(InstanceIndex, InstanceLightShadowUVBias[InstanceIndex]);
					}
				}
			}
		}
	}
}

void FInstancedStaticMeshSceneProxy::DestroyRenderThreadResources()
{
	InstancedRenderData.ReleaseResources(&GetScene(), StaticMesh);
	FStaticMeshSceneProxy::DestroyRenderThreadResources();

#if RHI_RAYTRACING
	for (auto &DynamicRayTracingItem : RayTracingDynamicData)
	{
		DynamicRayTracingItem.DynamicGeometry.ReleaseResource();
		DynamicRayTracingItem.DynamicGeometryVertexBuffer.Release();
	}
#endif
}

void FInstancedStaticMeshSceneProxy::SetupInstancedMeshBatch(int32 LODIndex, int32 BatchIndex, FMeshBatch& OutMeshBatch) const
{
	OutMeshBatch.VertexFactory = &InstancedRenderData.VertexFactories[LODIndex];
	const uint32 NumInstances = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();
	FMeshBatchElement& BatchElement0 = OutMeshBatch.Elements[0];
	BatchElement0.UserData = (void*)&UserData_AllInstances;
	BatchElement0.bUserDataIsColorVertexBuffer = false;
	BatchElement0.InstancedLODIndex = LODIndex;
	BatchElement0.UserIndex = 0;
	BatchElement0.PrimitiveUniformBuffer = GetUniformBuffer();

	BatchElement0.NumInstances = NumInstances;
}

void FInstancedStaticMeshSceneProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	FStaticMeshSceneProxy::GetLightRelevance(LightSceneProxy, bDynamic, bRelevant, bLightMapped, bShadowMapped);

	if (InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances() == 0)
	{
		bRelevant = false;
	}
}

bool FInstancedStaticMeshSceneProxy::GetShadowMeshElement(int32 LODIndex, int32 BatchIndex, uint8 InDepthPriorityGroup, FMeshBatch& OutMeshBatch, bool bDitheredLODTransition) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetShadowMeshElement(LODIndex, BatchIndex, InDepthPriorityGroup, OutMeshBatch, bDitheredLODTransition))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
}

/** Sets up a FMeshBatch for a specific LOD and element. */
bool FInstancedStaticMeshSceneProxy::GetMeshElement(int32 LODIndex, int32 BatchIndex, int32 ElementIndex, uint8 InDepthPriorityGroup, bool bUseSelectionOutline, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetMeshElement(LODIndex, BatchIndex, ElementIndex, InDepthPriorityGroup, bUseSelectionOutline, bAllowPreCulledIndices, OutMeshBatch))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
};

/** Sets up a wireframe FMeshBatch for a specific LOD. */
bool FInstancedStaticMeshSceneProxy::GetWireframeMeshElement(int32 LODIndex, int32 BatchIndex, const FMaterialRenderProxy* WireframeRenderProxy, uint8 InDepthPriorityGroup, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetWireframeMeshElement(LODIndex, BatchIndex, WireframeRenderProxy, InDepthPriorityGroup, bAllowPreCulledIndices, OutMeshBatch))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
}

void FInstancedStaticMeshSceneProxy::GetDistanceFieldAtlasData(const FDistanceFieldVolumeData*& OutDistanceFieldData, float& SelfShadowBias) const
{
	FStaticMeshSceneProxy::GetDistanceFieldAtlasData(OutDistanceFieldData, SelfShadowBias);
}

void FInstancedStaticMeshSceneProxy::GetDistanceFieldInstanceData(TArray<FRenderTransform>& ObjectLocalToWorldTransforms) const
{
	ObjectLocalToWorldTransforms.Reset();

	if (ensureMsgf(InstancedRenderData.PerInstanceRenderData->InstanceBuffer.RequireCPUAccess, TEXT("GetDistanceFieldInstanceData requires a CPU copy of the per-instance data to be accessible. Possible mismatch in ComponentRequestsCPUAccess / IncludePrimitiveInDistanceFieldSceneData filtering.")))
	{
		const TArray<FRenderTransform>& PerInstanceTransforms = InstancedRenderData.PerInstanceRenderData->GetPerInstanceTransforms();
		for (const FRenderTransform& InstanceToLocal : PerInstanceTransforms)
		{
			ObjectLocalToWorldTransforms.Add(InstanceToLocal * (FMatrix44f)GetLocalToWorld());
		}
	}
}

HHitProxy* FInstancedStaticMeshSceneProxy::CreateHitProxies(UPrimitiveComponent* Component,TArray<TRefCountPtr<HHitProxy> >& OutHitProxies)
{
	if (InstancedRenderData.PerInstanceRenderData.IsValid() && InstancedRenderData.PerInstanceRenderData->HitProxies.Num())
	{
		// Add any per-instance hit proxies.
		OutHitProxies += InstancedRenderData.PerInstanceRenderData->HitProxies;

		// No default hit proxy.
		return nullptr;
	}

	return FStaticMeshSceneProxy::CreateHitProxies(Component, OutHitProxies);
}

#if RHI_RAYTRACING
void FInstancedStaticMeshSceneProxy::GetDynamicRayTracingInstances(struct FRayTracingMaterialGatheringContext& Context, TArray<FRayTracingInstance>& OutRayTracingInstances)
{
	if (!CVarRayTracingRenderInstances.GetValueOnRenderThread())
	{
		return;
	}

	if (!bSupportRayTracing)
	{
		return;
	}

	uint32 LOD = GetCurrentFirstLODIdx_RenderThread();
	if (!RenderData->LODResources[LOD].RayTracingGeometry.IsInitialized())
	{
		return;
	}

	const int32 InstanceCount = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();

	if (InstanceCount == 0)
	{
		return;
	}

	// TODO: Select different LOD when current LOD is still requested for build?
	if (RenderData->LODResources[LOD].RayTracingGeometry.HasPendingBuildRequest())
	{
		RenderData->LODResources[LOD].RayTracingGeometry.BoostBuildPriority();
		return;
	}

	//setup a 'template' for the instance first, so we aren't duplicating work
	//#dxr_todo: when multiple LODs are used, template needs to be an array of templates, probably best initialized on-demand via a lamda
	FRayTracingInstance RayTracingInstanceTemplate;
	FRayTracingInstance RayTracingWPOInstanceTemplate;  //template for evaluating the WPO instances into the world
	FRayTracingInstance RayTracingWPODynamicTemplate;   //template for simulating the WPO instances
	RayTracingInstanceTemplate.Geometry = &RenderData->LODResources[LOD].RayTracingGeometry;

	// Which index holds the reference to the particular simulated instance
	TArray<int32> ActiveInstances;

	const int32 RequestedSimulatedInstances = CVarRayTracingSimulatedInstanceCount.GetValueOnRenderThread();
	const int32 SimulatedInstances = FMath::Min(RequestedSimulatedInstances == -1 ? InstanceCount : FMath::Clamp(RequestedSimulatedInstances, 1, InstanceCount), MaxSimulatedInstances);

	const int32 WPOEvalMode = CVarRayTracingInstancesEvaluateWPO.GetValueOnRenderThread();
	const bool bWantsWPOEvaluation = WPOEvalMode < 0 ? bDynamicRayTracingGeometry : WPOEvalMode != 0;
	const bool bHasWorldPositionOffset = bWantsWPOEvaluation && bAnySegmentUsesWorldPositionOffset;

	if (bHasWorldPositionOffset)
	{
		int32 SectionCount = InstancedRenderData.LODModels[LOD].Sections.Num();

		for (int32 SectionIdx = 0; SectionIdx < SectionCount; ++SectionIdx)
		{
			//#dxr_todo: so far we use the parent static mesh path to get material data
			FMeshBatch MeshBatch;
			FMeshBatch DynamicMeshBatch;

			GetMeshElement(LOD, 0, SectionIdx, 0, false, false, DynamicMeshBatch);

			FStaticMeshSceneProxy::GetMeshElement(LOD, 0, SectionIdx, 0, false, false, MeshBatch);

			DynamicMeshBatch.VertexFactory = &InstancedRenderData.VertexFactories[LOD];

			RayTracingWPOInstanceTemplate.Materials.Add(MeshBatch);
			RayTracingWPODynamicTemplate.Materials.Add(DynamicMeshBatch);
		}
		RayTracingWPOInstanceTemplate.BuildInstanceMaskAndFlags(GetScene().GetFeatureLevel());
	
		

		if (RayTracingDynamicData.Num() != SimulatedInstances || LOD != CachedRayTracingLOD)
		{
			SetupRayTracingDynamicInstances(SimulatedInstances, LOD);
		}
		ActiveInstances.AddZeroed(SimulatedInstances);

		for (auto &Instance : ActiveInstances)
		{
			Instance = INDEX_NONE;
		}
	}

	//preallocate the worst-case to prevent an explosion of reallocs
	//#dxr_todo: possibly track used instances and reserve based on previous behavior
	RayTracingInstanceTemplate.InstanceTransforms.Reserve(InstanceCount);

	FMatrix ToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();

	// whether to use angular culling instead of distance, angle is halved as it is compared against the projection of the radius rather than the diameter
	const float CullAngle = FMath::Min(CVarRayTracingInstancesCullAngle.GetValueOnRenderThread(), 179.9f) * 0.5f;

	const TArray<FVector4f>& PerInstanceBounds = InstancedRenderData.PerInstanceRenderData->GetPerInstanceBounds();
	if (CVarRayTracingRenderInstancesCulling.GetValueOnRenderThread() > 0 && PerInstanceBounds.Num())
	{
		if (CullAngle < 0.0f)
		{
			//
			//  Distance based culling
			//    Check nodes for being within minimum distances
			//
			const float BVHCullRadius = CVarRayTracingInstancesCullClusterRadius.GetValueOnRenderThread();
			const float BVHLowScaleThreshold = CVarRayTracingInstancesLowScaleThreshold.GetValueOnRenderThread();
			const float BVHLowScaleRadius = CVarRayTracingInstancesLowScaleCullRadius.GetValueOnRenderThread();
			const bool ApplyGeneralCulling = BVHCullRadius > 0.0f;
			const bool ApplyLowScaleCulling = BVHLowScaleThreshold > 0.0f && BVHLowScaleRadius > 0.0f;

			FVector ScaleVector = GetLocalToWorld().GetScaleVector();
			FMatrix WorldToLocal = GetLocalToWorld().InverseFast();
			float Scale = FMath::Max3(ScaleVector.X, ScaleVector.Y, ScaleVector.Z);
			FVector LocalViewPosition = WorldToLocal.TransformPosition(Context.ReferenceView->ViewLocation);

			const TArray<FRenderTransform>& PerInstanceTransforms = InstancedRenderData.PerInstanceRenderData->GetPerInstanceTransforms();
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; InstanceIndex++)
			{
				FVector4 InstanceSphere = PerInstanceBounds[InstanceIndex];
				FVector InstanceLocation = InstanceSphere;
				FVector VToInstanceCenter = LocalViewPosition - InstanceLocation;
				float DistanceToInstanceCenter = VToInstanceCenter.Size();
				float InstanceRadius = InstanceSphere.W;
				float DistanceToInstanceStart = (DistanceToInstanceCenter - InstanceRadius) * Scale; //scale accounts for possibly scaling in LocalToWorld, since measurements are all in local

				// Cull instance based on distance
				if (DistanceToInstanceStart > BVHCullRadius && ApplyGeneralCulling)
					continue;

				// Special culling for small scale objects
				if (InstanceRadius < BVHLowScaleThreshold && ApplyLowScaleCulling)
				{
					if (DistanceToInstanceStart > BVHLowScaleRadius)
						continue;
				}

				const FMatrix InstanceTransform = PerInstanceTransforms[InstanceIndex].ToMatrix() * GetLocalToWorld();
				RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);

			}
		}
		else
		{
			//
			// Angle based culling
			//  Instead of culling objects based on distance check the radius of bounding sphere against a minimum culling angle
			//  This ensures objects essentially cull based on size as seen from viewer rather than distance. Provides much less
			//  popping for the same number of instances
			//
			float Ratio = FMath::Tan(CullAngle / 360.0f * 2.0f * PI);

			FVector ScaleVector = GetLocalToWorld().GetScaleVector();
			FMatrix WorldToLocal = GetLocalToWorld().InverseFast();
			float Scale = FMath::Max3(ScaleVector.X, ScaleVector.Y, ScaleVector.Z);
			FVector LocalViewPosition = WorldToLocal.TransformPosition(Context.ReferenceView->ViewLocation);

			const TArray<FRenderTransform>& PerInstanceTransforms = InstancedRenderData.PerInstanceRenderData->GetPerInstanceTransforms();
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; InstanceIndex++)
			{
				FVector4 InstanceSphere = PerInstanceBounds[InstanceIndex];
				FVector InstanceLocation = InstanceSphere;
				FVector VToInstanceCenter = LocalViewPosition - InstanceLocation;
						float DistanceToInstanceCenter = VToInstanceCenter.Size();

				if (DistanceToInstanceCenter * Ratio <= InstanceSphere.W * Scale)
				{
					FMatrix InstanceTransform = PerInstanceTransforms[InstanceIndex].ToMatrix() * GetLocalToWorld();
					const int32 DynamicInstanceIdx = InstanceIndex % SimulatedInstances;

					if (bHasWorldPositionOffset && InstancedRenderData.VertexFactories[LOD].GetType()->SupportsRayTracingDynamicGeometry())
					{
						FRayTracingInstance *DynamicInstance = nullptr;

						if (ActiveInstances[DynamicInstanceIdx] == INDEX_NONE)
						{
							// First case of this dynamic instance, setup the material and add it
							float InstanceRandom;
							InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetInstanceRandomID(InstanceIndex, InstanceRandom);

							const FStaticMeshLODResources& LODModel = RenderData->LODResources[LOD];

							FRayTracingDynamicData &DynamicData = RayTracingDynamicData[DynamicInstanceIdx];

							ActiveInstances[DynamicInstanceIdx] = OutRayTracingInstances.Num();
							FRayTracingInstance &RayTracingInstance = OutRayTracingInstances.Add_GetRef(RayTracingWPOInstanceTemplate);
							RayTracingInstance.Geometry = &DynamicData.DynamicGeometry;
							RayTracingInstance.InstanceTransforms.Reserve(InstanceCount);

							DynamicInstance = &RayTracingInstance;

							FRayTracingInstance SimulationInstance = RayTracingWPODynamicTemplate;

							// ToDo - deeper dive into ensuring better instance simulation matching
							FMatrix Passthrough = FMatrix::Identity;
							Passthrough.M[3][3] = InstanceRandom;

							Context.DynamicRayTracingGeometriesToUpdate.Add(
								FRayTracingDynamicGeometryUpdateParams
								{
									SimulationInstance.Materials,
									false,
									(uint32)LODModel.GetNumVertices(),
									uint32((SIZE_T)LODModel.GetNumVertices() * sizeof(FVector3f)),
									DynamicData.DynamicGeometry.Initializer.TotalPrimitiveCount,
									&DynamicData.DynamicGeometry,
									nullptr,
									true,
									Passthrough
								}
							);
						}
						else
						{
							DynamicInstance = &OutRayTracingInstances[ActiveInstances[DynamicInstanceIdx]];
						}

						DynamicInstance->InstanceTransforms.Add(InstanceTransform);

					}
					else
					{
						RayTracingInstanceTemplate.InstanceTransforms.Emplace(InstanceTransform);
					}
				}
			}
		}
	}
	else
	{
		// No culling
		const TArray<FRenderTransform>& PerInstanceTransforms = InstancedRenderData.PerInstanceRenderData->GetPerInstanceTransforms();
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
		{
			FMatrix InstanceTransform = PerInstanceTransforms[InstanceIndex].ToMatrix() * GetLocalToWorld();

			if (bHasWorldPositionOffset && InstancedRenderData.VertexFactories[LOD].GetType()->SupportsRayTracingDynamicGeometry())
			{
				FRayTracingInstance* DynamicInstance = nullptr;

				const int32 DynamicInstanceIdx = InstanceIndex % SimulatedInstances;

				if (ActiveInstances[DynamicInstanceIdx] == INDEX_NONE)
				{
					// First case of this dynamic instance, setup the material and add it
					float InstanceRandom;
					InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetInstanceRandomID(InstanceIndex, InstanceRandom);

					const FStaticMeshLODResources& LODModel = RenderData->LODResources[LOD];

					FRayTracingDynamicData &DynamicData = RayTracingDynamicData[DynamicInstanceIdx];

					ActiveInstances[DynamicInstanceIdx] = OutRayTracingInstances.Num();
					FRayTracingInstance &RayTracingInstance = OutRayTracingInstances.Add_GetRef(RayTracingWPOInstanceTemplate);
					RayTracingInstance.Geometry = &DynamicData.DynamicGeometry;
					RayTracingInstance.InstanceTransforms.Reserve(InstanceCount);

					DynamicInstance = &RayTracingInstance;

					FRayTracingInstance SimulationInstance = RayTracingWPODynamicTemplate;

					// ToDo - deeper dive into ensuring better instance simulation matching
					FMatrix Passthrough = FMatrix::Identity;
					Passthrough.M[3][3] = InstanceRandom;

					Context.DynamicRayTracingGeometriesToUpdate.Add(
						FRayTracingDynamicGeometryUpdateParams
						{
							SimulationInstance.Materials,
							false,
							(uint32)LODModel.GetNumVertices(),
							uint32((SIZE_T)LODModel.GetNumVertices() * sizeof(FVector3f)),
							DynamicData.DynamicGeometry.Initializer.TotalPrimitiveCount,
							&DynamicData.DynamicGeometry,
							nullptr,
							true,
							Passthrough
						}
					);

				}
				else
				{
					DynamicInstance = &OutRayTracingInstances[ActiveInstances[DynamicInstanceIdx]];
				}

				DynamicInstance->InstanceTransforms.Add(InstanceTransform);

			}
			else
			{
				RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);
			}
		}
	}


	if (RayTracingInstanceTemplate.InstanceTransforms.Num() > 0)
	{
		int32 SectionCount = InstancedRenderData.LODModels[LOD].Sections.Num();

		for (int32 SectionIdx = 0; SectionIdx < SectionCount; ++SectionIdx)
		{
			//#dxr_todo: so far we use the parent static mesh path to get material data
			FMeshBatch MeshBatch;
			FStaticMeshSceneProxy::GetMeshElement(LOD, 0, SectionIdx, 0, false, false, MeshBatch);

			RayTracingInstanceTemplate.Materials.Add(MeshBatch);
		}
		RayTracingInstanceTemplate.BuildInstanceMaskAndFlags(GetScene().GetFeatureLevel());

		OutRayTracingInstances.Add(RayTracingInstanceTemplate);
	}
}


void FInstancedStaticMeshSceneProxy::SetupRayTracingDynamicInstances(int32 NumDynamicInstances, int32 LOD)
{
	if (RayTracingDynamicData.Num() > NumDynamicInstances || CachedRayTracingLOD != LOD)
	{
		//free the unused/out of date entries

		int32 FirstToFree = (CachedRayTracingLOD != LOD) ? 0 : NumDynamicInstances;
		for (int32 Item = FirstToFree; Item < RayTracingDynamicData.Num(); Item++)
		{
			auto& DynamicRayTracingItem = RayTracingDynamicData[Item];
			DynamicRayTracingItem.DynamicGeometry.ReleaseResource();
			DynamicRayTracingItem.DynamicGeometryVertexBuffer.Release();
		}
		RayTracingDynamicData.SetNum(FirstToFree);
	}

	if (RayTracingDynamicData.Num() < NumDynamicInstances)
	{
		RayTracingDynamicData.Reserve(NumDynamicInstances);
		const int32 StartIndex = RayTracingDynamicData.Num();
		const FStaticMeshLODResources& LODModel = RenderData->LODResources[LOD];

		for (int32 Item = StartIndex; Item < NumDynamicInstances; Item++)
		{
			FRayTracingDynamicData &DynamicData = RayTracingDynamicData.AddDefaulted_GetRef();

			auto& Initializer = DynamicData.DynamicGeometry.Initializer;
			Initializer = LODModel.RayTracingGeometry.Initializer;
			for (FRayTracingGeometrySegment& Segment : Initializer.Segments)
			{
				Segment.VertexBuffer = nullptr; 
			}
			Initializer.bAllowUpdate = true;
			Initializer.bFastBuild = true;

			DynamicData.DynamicGeometry.InitResource();
		}
	}

	CachedRayTracingLOD = LOD;
}

#endif


/*-----------------------------------------------------------------------------
	UInstancedStaticMeshComponent
-----------------------------------------------------------------------------*/

UInstancedStaticMeshComponent::UInstancedStaticMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Mobility = EComponentMobility::Movable;
	BodyInstance.bSimulatePhysics = false;

	bDisallowMeshPaintPerInstance = true;
	bMultiBodyOverlap = true;

#if STATS
	{
		UObject const* StatObject = this->AdditionalStatObject();
		if (!StatObject)
		{
			StatObject = this;
		}
		StatId = StatObject->GetStatID(true);
	}
#endif
}

UInstancedStaticMeshComponent::UInstancedStaticMeshComponent(FVTableHelper& Helper)
	: Super(Helper)
{
}

UInstancedStaticMeshComponent::~UInstancedStaticMeshComponent()
{
	ReleasePerInstanceRenderData();
}

TStructOnScope<FActorComponentInstanceData> UInstancedStaticMeshComponent::GetComponentInstanceData() const
{
	TStructOnScope<FActorComponentInstanceData> InstanceData;
#if WITH_EDITOR
	InstanceData.InitializeAs<FInstancedStaticMeshComponentInstanceData>(this);
	FInstancedStaticMeshComponentInstanceData* StaticMeshInstanceData = InstanceData.Cast<FInstancedStaticMeshComponentInstanceData>();

	// Fill in info (copied from UStaticMeshComponent::GetComponentInstanceData)
	StaticMeshInstanceData->CachedStaticLighting.Transform = GetComponentTransform();

	for (const FStaticMeshComponentLODInfo& LODDataEntry : LODData)
	{
		StaticMeshInstanceData->CachedStaticLighting.MapBuildDataIds.Add(LODDataEntry.MapBuildDataId);
	}

	// Back up per-instance info
	StaticMeshInstanceData->PerInstanceSMData = PerInstanceSMData;
	StaticMeshInstanceData->PerInstanceSMCustomData = PerInstanceSMCustomData;

	// Back up instance selection
	StaticMeshInstanceData->SelectedInstances = SelectedInstances;

	// Back up random seed
	StaticMeshInstanceData->InstancingRandomSeed = InstancingRandomSeed;
	StaticMeshInstanceData->AdditionalRandomSeeds = AdditionalRandomSeeds;

	// Back up per-instance hit proxies
	StaticMeshInstanceData->bHasPerInstanceHitProxies = bHasPerInstanceHitProxies;
#endif
	return InstanceData;
}

void UInstancedStaticMeshComponent::GetComponentChildElements(TArray<FTypedElementHandle>& OutElementHandles, const bool bAllowCreate)
{
#if WITH_EDITOR
	for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); ++InstanceIndex)
	{
		if (FTypedElementHandle ElementHandle = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(this, InstanceIndex, bAllowCreate))
		{
			OutElementHandles.Add(MoveTemp(ElementHandle));
		}
	}
#endif	// WITH_EDITOR
}

void UInstancedStaticMeshComponent::ApplyComponentInstanceData(FInstancedStaticMeshComponentInstanceData* InstancedMeshData)
{
#if WITH_EDITOR
	check(InstancedMeshData);

	if (GetStaticMesh() != InstancedMeshData->StaticMesh)
	{
		return;
	}

	bool bMatch = false;

	// Check for any instance having moved as that would invalidate static lighting
	if (PerInstanceSMData.Num() == InstancedMeshData->PerInstanceSMData.Num() &&
		InstancedMeshData->CachedStaticLighting.Transform.Equals(GetComponentTransform()))
	{
		bMatch = true;

		for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); ++InstanceIndex)
		{
			if (PerInstanceSMData[InstanceIndex].Transform != InstancedMeshData->PerInstanceSMData[InstanceIndex].Transform)
			{
				bMatch = false;
				break;
			}
		}
	}

	// Restore static lighting if appropriate
	if (bMatch)
	{
		const int32 NumLODLightMaps = InstancedMeshData->CachedStaticLighting.MapBuildDataIds.Num();
		SetLODDataCount(NumLODLightMaps, NumLODLightMaps);

		for (int32 i = 0; i < NumLODLightMaps; ++i)
		{
			LODData[i].MapBuildDataId = InstancedMeshData->CachedStaticLighting.MapBuildDataIds[i];
		}

		PerInstanceSMData = InstancedMeshData->PerInstanceSMData;
	}

	SelectedInstances = InstancedMeshData->SelectedInstances;

	InstancingRandomSeed = InstancedMeshData->InstancingRandomSeed;
	AdditionalRandomSeeds = InstancedMeshData->AdditionalRandomSeeds;

	bHasPerInstanceHitProxies = InstancedMeshData->bHasPerInstanceHitProxies;

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
#endif
}

void UInstancedStaticMeshComponent::FlushInstanceUpdateCommands()
{
	InstanceUpdateCmdBuffer.Reset();

	FStaticMeshInstanceData RenderInstanceData = FStaticMeshInstanceData(GVertexElementTypeSupport.IsSupported(VET_Half2));
	BuildRenderData(RenderInstanceData, PerInstanceRenderData->HitProxies);
	PerInstanceRenderData->UpdateFromPreallocatedData(RenderInstanceData);
}

bool UInstancedStaticMeshComponent::IsHLODRelevant() const
{
	if (GetInstanceCount() == 0)
	{
		return false;
	}

	return Super::IsHLODRelevant();
}

FBodyInstance* UInstancedStaticMeshComponent::GetBodyInstance(FName BoneName, bool bGetWelded, int32 Index) const
{
	if (Index != INDEX_NONE && IsValidInstance(Index))
	{
		return const_cast<FBodyInstance*>(InstanceBodies[Index]);
	}
	return  const_cast<FBodyInstance*>(&BodyInstance); // If no index is specified we return the primitive component body instance instead
}

FPrimitiveSceneProxy* UInstancedStaticMeshComponent::CreateSceneProxy()
{
	static const auto NaniteProxyRenderModeVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.ProxyRenderMode"));
	const int32 NaniteProxyRenderMode = (NaniteProxyRenderModeVar != nullptr) ? (NaniteProxyRenderModeVar->GetInt() != 0) : 0;

	LLM_SCOPE(ELLMTag::InstancedMesh);

	ProxySize = 0;

	// Verify that the mesh is valid before using it.
	const bool bMeshIsValid =
		// make sure we have instances
		PerInstanceSMData.Num() > 0 &&
		// make sure we have an actual static mesh
		GetStaticMesh() &&
		GetStaticMesh()->IsCompiling() == false &&
		GetStaticMesh()->HasValidRenderData();

	if (bMeshIsValid)
	{
		check(InstancingRandomSeed != 0);
		
		// if instance data was modified, update GPU copy
		// generally happens only in editor 
		if (InstanceUpdateCmdBuffer.NumTotalCommands() != 0)
		{
			FlushInstanceUpdateCommands();
		}
		
		ProxySize = PerInstanceRenderData->ResourceSize;

		// Is Nanite supported, and is there built Nanite data for this static mesh?
		if (ShouldCreateNaniteProxy())
		{
			return ::new Nanite::FSceneProxy(this);
		}
		// If we didn't get a proxy, but Nanite was enabled on the asset when it was built, evaluate proxy creation
		else if (GetStaticMesh()->HasValidNaniteData() && NaniteProxyRenderMode != 0)
		{
			// Do not render Nanite proxy
			return nullptr;
		}
		else
		{
			return ::new FInstancedStaticMeshSceneProxy(this, GetWorld()->FeatureLevel);
		}
	}
	else
	{
		return nullptr;
	}
}

void UInstancedStaticMeshComponent::CreateHitProxyData(TArray<TRefCountPtr<HHitProxy>>& HitProxies)
{
	if (GIsEditor && bHasPerInstanceHitProxies)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_CreateHitProxyData);
		
		int32 NumProxies = PerInstanceSMData.Num();
		HitProxies.Empty(NumProxies);

		for (int32 InstanceIdx = 0; InstanceIdx < NumProxies; ++InstanceIdx)
		{
			HitProxies.Add(new HInstancedStaticMeshInstance(this, InstanceIdx));
		}
	}
	else
	{
		HitProxies.Empty();
	}
}

void UInstancedStaticMeshComponent::BuildRenderData(FStaticMeshInstanceData& OutData, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_BuildRenderData);

	CreateHitProxyData(OutHitProxies);
	
	int32 NumInstances = PerInstanceSMData.Num();
	if (NumInstances == 0)
	{
		return;
	}
	
	OutData.AllocateInstances(NumInstances, NumCustomDataFloats, GIsEditor ? EResizeBufferFlags::AllowSlackOnGrow | EResizeBufferFlags::AllowSlackOnReduce : EResizeBufferFlags::None, true); // In Editor always permit overallocation, to prevent too much realloc

	const FMeshMapBuildData* MeshMapBuildData = nullptr;

#if WITH_EDITOR
	MeshMapBuildData = FStaticLightingSystemInterface::GetPrimitiveMeshMapBuildData(this, 0);
#endif

	if (MeshMapBuildData == nullptr && LODData.Num() > 0)
	{
		MeshMapBuildData = GetMeshMapBuildData(LODData[0], false);
	}
	
	check(InstancingRandomSeed != 0);
	FRandomStream RandomStream = FRandomStream(InstancingRandomSeed);

	auto AdditionanlRandomSeedsIt = AdditionalRandomSeeds.CreateIterator();
	int32 SeedResetIndex = AdditionanlRandomSeedsIt ? AdditionanlRandomSeedsIt->StartInstanceIndex : INDEX_NONE;
	
	for (int32 Index = 0; Index < NumInstances; ++Index)
	{
		const int32 RenderIndex = GetRenderIndex(Index);
		if (RenderIndex == INDEX_NONE) 
		{
			// could be skipped by density settings
			continue;
		}

		// Reset the random stream if necessary
		if (Index == SeedResetIndex)
		{
			RandomStream = FRandomStream(AdditionanlRandomSeedsIt->RandomSeed);
			AdditionanlRandomSeedsIt++;
			SeedResetIndex = AdditionanlRandomSeedsIt ? AdditionanlRandomSeedsIt->StartInstanceIndex : INDEX_NONE;
		}

		const FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[Index];
		FVector2D LightmapUVBias = FVector2D(-1.0f, -1.0f);
		FVector2D ShadowmapUVBias = FVector2D(-1.0f, -1.0f);

		if (MeshMapBuildData != nullptr && MeshMapBuildData->PerInstanceLightmapData.IsValidIndex(Index))
		{
			LightmapUVBias = MeshMapBuildData->PerInstanceLightmapData[Index].LightmapUVBias;
			ShadowmapUVBias = MeshMapBuildData->PerInstanceLightmapData[Index].ShadowmapUVBias;
		}
	
		OutData.SetInstance(RenderIndex, InstanceData.Transform, RandomStream.GetFraction(), LightmapUVBias, ShadowmapUVBias);

		for (int32 CustomDataIndex = 0; CustomDataIndex < NumCustomDataFloats; ++CustomDataIndex)
		{
			OutData.SetInstanceCustomData(RenderIndex, CustomDataIndex, PerInstanceSMCustomData[Index * NumCustomDataFloats + CustomDataIndex]);
		}

#if WITH_EDITOR
		if (GIsEditor)
		{
			// Record if the instance is selected
			FColor HitProxyColor(ForceInit);
			bool bSelected = SelectedInstances.IsValidIndex(Index) && SelectedInstances[Index];

			if (OutHitProxies.IsValidIndex(Index))
			{
				HitProxyColor = OutHitProxies[Index]->Id.GetColor();
			}

			OutData.SetInstanceEditorData(RenderIndex, HitProxyColor, bSelected);
		}
#endif
	}
}

void UInstancedStaticMeshComponent::InitInstanceBody(int32 InstanceIdx, FBodyInstance* InstanceBodyInstance)
{
	if (!GetStaticMesh())
	{
		UE_LOG(LogStaticMesh, Warning, TEXT("Unabled to create a body instance for %s in Actor %s. No StaticMesh set."), *GetName(), GetOwner() ? *GetOwner()->GetName() : TEXT("?"));
		return;
	}

	check(InstanceIdx < PerInstanceSMData.Num());
	check(InstanceIdx < InstanceBodies.Num());
	check(InstanceBodyInstance);

	UBodySetup* BodySetup = GetBodySetup();
	check(BodySetup);

	// Get transform of the instance
	FTransform InstanceTransform = FTransform(PerInstanceSMData[InstanceIdx].Transform) * GetComponentTransform();
	
	InstanceBodyInstance->CopyBodyInstancePropertiesFrom(&BodyInstance);
	InstanceBodyInstance->InstanceBodyIndex = InstanceIdx; // Set body index 

	// make sure we never enable bSimulatePhysics for ISMComps
	InstanceBodyInstance->bSimulatePhysics = false;

#if WITH_PHYSX
	// Create physics body instance.
	InstanceBodyInstance->bAutoWeld = false;	//We don't support this for instanced meshes.
	InstanceBodyInstance->InitBody(BodySetup, InstanceTransform, this, GetWorld()->GetPhysicsScene(), nullptr);
#endif //WITH_PHYSX
}

void UInstancedStaticMeshComponent::CreateAllInstanceBodies()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInstancedStaticMeshComponent::CreateAllInstanceBodies);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_CreateAllInstanceBodies);
	STAT(FScopeCycleCounter Context(StatId);)

	const int32 NumBodies = PerInstanceSMData.Num();
	check(InstanceBodies.Num() == 0);

	if (UBodySetup* BodySetup = GetBodySetup())
	{
		FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();

		if (!BodyInstance.GetOverrideWalkableSlopeOnInstance())
		{
			BodyInstance.SetWalkableSlopeOverride(BodySetup->WalkableSlopeOverride, false);
		}

		InstanceBodies.SetNumUninitialized(NumBodies);

		// Sanitized array does not contain any nulls
		TArray<FBodyInstance*> InstanceBodiesSanitized;
		InstanceBodiesSanitized.Reserve(NumBodies);

		TArray<FTransform> Transforms;
	    Transforms.Reserve(NumBodies);
	    for (int32 i = 0; i < NumBodies; ++i)
	    {
			const FTransform InstanceTM = FTransform(PerInstanceSMData[i].Transform) * GetComponentTransform();
			if (InstanceTM.GetScale3D().IsNearlyZero())
			{
				InstanceBodies[i] = nullptr;
			}
			else
			{
				FBodyInstance* Instance = new FBodyInstance;

				InstanceBodiesSanitized.Add(Instance);
				InstanceBodies[i] = Instance;
				Instance->CopyBodyInstancePropertiesFrom(&BodyInstance);
				Instance->InstanceBodyIndex = i; // Set body index 
				Instance->bAutoWeld = false;

				// make sure we never enable bSimulatePhysics for ISMComps
				Instance->bSimulatePhysics = false;

				if (Mobility == EComponentMobility::Movable)
				{
					Instance->InitBody(BodySetup, InstanceTM, this, PhysScene, nullptr );
				}
				else
				{
					Transforms.Add(InstanceTM);
				}
			}
	    }

		if (InstanceBodiesSanitized.Num() > 0 && Mobility != EComponentMobility::Movable)
		{
			FBodyInstance::InitStaticBodies(InstanceBodiesSanitized, Transforms, BodySetup, this, GetWorld()->GetPhysicsScene());
		}
	}
	else
	{
		// In case we get into some bad state where the BodySetup is invalid but bPhysicsStateCreated is true,
		// issue a warning and add nullptrs to InstanceBodies.
		UE_LOG(LogStaticMesh, Warning, TEXT("Instance Static Mesh Component unable to create InstanceBodies!"));
		InstanceBodies.AddZeroed(NumBodies);
	}
}

void UInstancedStaticMeshComponent::ClearAllInstanceBodies()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_ClearAllInstanceBodies);
	STAT(FScopeCycleCounter Context(StatId);)

	for (int32 i = 0; i < InstanceBodies.Num(); i++)
	{
		if (InstanceBodies[i])
		{
			InstanceBodies[i]->TermBody();
			delete InstanceBodies[i];
		}
	}

	InstanceBodies.Empty();
}


void UInstancedStaticMeshComponent::OnCreatePhysicsState()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInstancedStaticMeshComponent::OnCreatePhysicsState)
	check(InstanceBodies.Num() == 0);

	FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();

	if (!PhysScene)
	{
		return;
	}

	// Create all the bodies.
	CreateAllInstanceBodies();

	USceneComponent::OnCreatePhysicsState();

	// Since StaticMeshComponent was not called
	// Navigation relevancy needs to be handled here
	bNavigationRelevant = IsNavigationRelevant();
	FNavigationSystem::UpdateComponentData(*this);
}

void UInstancedStaticMeshComponent::OnDestroyPhysicsState()
{
	USceneComponent::OnDestroyPhysicsState();

	// Release all physics representations
	ClearAllInstanceBodies();

	// Since StaticMeshComponent was not called
	// Navigation relevancy needs to be handled here
	bNavigationRelevant = IsNavigationRelevant();
	FNavigationSystem::UpdateComponentData(*this);
}

bool UInstancedStaticMeshComponent::CanEditSimulatePhysics()
{
	// if instancedstaticmeshcomponent, we will never allow it
	return false;
}

FBoxSphereBounds UInstancedStaticMeshComponent::CalcBounds(const FTransform& BoundTransform) const
{
	if(GetStaticMesh() && PerInstanceSMData.Num() > 0)
	{
		FMatrix BoundTransformMatrix = BoundTransform.ToMatrixWithScale();

		FBoxSphereBounds RenderBounds = GetStaticMesh()->GetBounds();
		FBoxSphereBounds NewBounds = RenderBounds.TransformBy(PerInstanceSMData[0].Transform * BoundTransformMatrix);

		for (int32 InstanceIndex = 1; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
		{
			NewBounds = NewBounds + RenderBounds.TransformBy(PerInstanceSMData[InstanceIndex].Transform * BoundTransformMatrix);
		}

		return NewBounds;
	}
	else
	{
		return FBoxSphereBounds(BoundTransform.GetLocation(), FVector::ZeroVector, 0.f);
	}
}

#if WITH_EDITOR
void UInstancedStaticMeshComponent::GetStaticLightingInfo(FStaticLightingPrimitiveInfo& OutPrimitiveInfo, const TArray<ULightComponent*>& InRelevantLights, const FLightingBuildOptions& Options)
{
	if (HasValidSettingsForStaticLighting(false))
	{
		// create static lighting for LOD 0
		int32 LightMapWidth = 0;
		int32 LightMapHeight = 0;
		GetLightMapResolution(LightMapWidth, LightMapHeight);

		bool bFit = false;
		bool bReduced = false;
		while (1)
		{
			const int32 OneLessThanMaximumSupportedResolution = 1 << (GMaxTextureMipCount - 2);

			const int32 MaxInstancesInMaxSizeLightmap = (OneLessThanMaximumSupportedResolution / LightMapWidth) * ((OneLessThanMaximumSupportedResolution / 2) / LightMapHeight);
			if (PerInstanceSMData.Num() > MaxInstancesInMaxSizeLightmap)
			{
				if (LightMapWidth < 4 || LightMapHeight < 4)
				{
					break;
				}
				LightMapWidth /= 2;
				LightMapHeight /= 2;
				bReduced = true;
			}
			else
			{
				bFit = true;
				break;
			}
		}

		if (!bFit)
		{
			FMessageLog("LightingResults").Message(EMessageSeverity::Error)
				->AddToken(FUObjectToken::Create(this))
				->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "FailedStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent is too big no matter how much we reduce the per-instance size, the number of mesh instances in this component must be reduced")));
			return;
		}
		if (bReduced)
		{
			FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
				->AddToken(FUObjectToken::Create(this))
				->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "ReducedStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent was too big and it was automatically reduced. Consider reducing the component's lightmap resolution or number of mesh instances in this component")));
		}

		const int32 LightMapSize = GetWorld()->GetWorldSettings()->PackedLightAndShadowMapTextureSize;
		const int32 MaxInstancesInDefaultSizeLightmap = (LightMapSize / LightMapWidth) * ((LightMapSize / 2) / LightMapHeight);
		if (PerInstanceSMData.Num() > MaxInstancesInDefaultSizeLightmap)
		{
			FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
				->AddToken(FUObjectToken::Create(this))
				->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "LargeStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent is large, consider reducing the component's lightmap resolution or number of mesh instances in this component")));
		}

		// TODO: Support separate static lighting in LODs for instanced meshes.

		if (!GetStaticMesh()->CanLODsShareStaticLighting())
		{
			//TODO: Detect if the UVs for all sub-LODs overlap the base LOD UVs and omit this warning if they do.
			FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
				->AddToken(FUObjectToken::Create(this))
				->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "UniqueStaticLightingForLODWarning", "Instanced meshes don't yet support unique static lighting for each LOD. Lighting on LOD 1+ may be incorrect unless lightmap UVs are the same for all LODs.")));
		}

		// Force sharing LOD 0 lightmaps for now.
		int32 NumLODs = 1;

		CachedMappings.Reset(PerInstanceSMData.Num() * NumLODs);
		CachedMappings.AddZeroed(PerInstanceSMData.Num() * NumLODs);

		NumPendingLightmaps = 0;

		for (int32 LODIndex = 0; LODIndex < NumLODs; LODIndex++)
		{
			const FStaticMeshLODResources& LODRenderData = GetStaticMesh()->GetRenderData()->LODResources[LODIndex];

			for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
			{
				auto* StaticLightingMesh = new FStaticLightingMesh_InstancedStaticMesh(this, LODIndex, InstanceIndex, InRelevantLights);
				OutPrimitiveInfo.Meshes.Add(StaticLightingMesh);

				auto* InstancedMapping = new FStaticLightingTextureMapping_InstancedStaticMesh(this, LODIndex, InstanceIndex, StaticLightingMesh, LightMapWidth, LightMapHeight, GetStaticMesh()->GetLightMapCoordinateIndex(), true);
				OutPrimitiveInfo.Mappings.Add(InstancedMapping);

				CachedMappings[LODIndex * PerInstanceSMData.Num() + InstanceIndex].Mapping = InstancedMapping;
				NumPendingLightmaps++;
			}

			// Shrink LOD texture lightmaps by half for each LOD level (minimum 4x4 px)
			LightMapWidth  = FMath::Max(LightMapWidth  / 2, 4);
			LightMapHeight = FMath::Max(LightMapHeight / 2, 4);
		}
	}
}

void UInstancedStaticMeshComponent::ApplyLightMapping(FStaticLightingTextureMapping_InstancedStaticMesh* InMapping, ULevel* LightingScenario)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTexturedLightmaps"));
	const bool bUseVirtualTextures = (CVar->GetValueOnAnyThread() != 0) && UseVirtualTexturing(GMaxRHIFeatureLevel);

	NumPendingLightmaps--;

	if (NumPendingLightmaps == 0)
	{
		// Calculate the range of each coefficient in this light-map and repack the data to have the same scale factor and bias across all instances
		// TODO: Per instance scale?

		// generate the final lightmaps for all the mappings for this component
		TArray<TUniquePtr<FQuantizedLightmapData>> AllQuantizedData;
		for (auto& MappingInfo : CachedMappings)
		{
			FStaticLightingTextureMapping_InstancedStaticMesh* Mapping = MappingInfo.Mapping;
			AllQuantizedData.Add(MoveTemp(Mapping->QuantizedData));
		}

		bool bNeedsShadowMap = false;
		TArray<TMap<ULightComponent*, TUniquePtr<FShadowMapData2D>>> AllShadowMapData;
		for (auto& MappingInfo : CachedMappings)
		{
			FStaticLightingTextureMapping_InstancedStaticMesh* Mapping = MappingInfo.Mapping;
			bNeedsShadowMap = bNeedsShadowMap || (Mapping->ShadowMapData.Num() > 0);
			AllShadowMapData.Add(MoveTemp(Mapping->ShadowMapData));
		}

		UStaticMesh* ResolvedMesh = GetStaticMesh();
		if (LODData.Num() != ResolvedMesh->GetNumLODs())
		{
			MarkPackageDirty();
		}

		// Ensure LODData has enough entries in it, free not required.
		SetLODDataCount(ResolvedMesh->GetNumLODs(), ResolvedMesh->GetNumLODs());
		FStaticMeshComponentLODInfo& LODInfo = LODData[0];

		// Ensure this LODInfo has a valid MapBuildDataId
		if (LODInfo.CreateMapBuildDataId(0))
		{
			MarkPackageDirty();
		}

		ULevel* StorageLevel = LightingScenario ? LightingScenario : GetOwner()->GetLevel();
		UMapBuildDataRegistry* Registry = StorageLevel->GetOrCreateMapBuildData();
		FMeshMapBuildData& MeshBuildData = Registry->AllocateMeshBuildData(LODInfo.MapBuildDataId, true);

		MeshBuildData.PerInstanceLightmapData.Empty(AllQuantizedData.Num());
		MeshBuildData.PerInstanceLightmapData.AddZeroed(AllQuantizedData.Num());

		// Create a light-map for the primitive.
		// When using VT, shadow map data is included with lightmap allocation
		const ELightMapPaddingType PaddingType = GAllowLightmapPadding ? LMPT_NormalPadding : LMPT_NoPadding;
		TArray<TMap<ULightComponent*, TUniquePtr<FShadowMapData2D>>> EmptyShadowMapData;
		TRefCountPtr<FLightMap2D> NewLightMap = FLightMap2D::AllocateInstancedLightMap(Registry, this,
			MoveTemp(AllQuantizedData),
			bUseVirtualTextures ? MoveTemp(AllShadowMapData) : MoveTemp(EmptyShadowMapData),
			Registry, LODInfo.MapBuildDataId, Bounds, PaddingType, LMF_Streamed);

		// Create a shadow-map for the primitive, only needed when not using VT
		TRefCountPtr<FShadowMap2D> NewShadowMap = (bNeedsShadowMap && !bUseVirtualTextures)
			? FShadowMap2D::AllocateInstancedShadowMap(Registry, this, MoveTemp(AllShadowMapData), Registry, LODInfo.MapBuildDataId, Bounds, PaddingType, SMF_Streamed)
			: nullptr;

		MeshBuildData.LightMap = NewLightMap;
		MeshBuildData.ShadowMap = NewShadowMap;

		// Build the list of statically irrelevant lights.
		// TODO: This should be stored per LOD.
		TSet<FGuid> RelevantLights;
		TSet<FGuid> PossiblyIrrelevantLights;
		for (auto& MappingInfo : CachedMappings)
		{
			for (const ULightComponent* Light : MappingInfo.Mapping->Mesh->RelevantLights)
			{
				// Check if the light is stored in the light-map.
				const bool bIsInLightMap = MeshBuildData.LightMap && MeshBuildData.LightMap->LightGuids.Contains(Light->LightGuid);

				// Check if the light is stored in the shadow-map.
				const bool bIsInShadowMap = MeshBuildData.ShadowMap && MeshBuildData.ShadowMap->LightGuids.Contains(Light->LightGuid);

				// If the light isn't already relevant to another mapping, add it to the potentially irrelevant list
				if (!bIsInLightMap && !bIsInShadowMap && !RelevantLights.Contains(Light->LightGuid))
				{
					PossiblyIrrelevantLights.Add(Light->LightGuid);
				}

				// Light is relevant
				if (bIsInLightMap || bIsInShadowMap)
				{
					RelevantLights.Add(Light->LightGuid);
					PossiblyIrrelevantLights.Remove(Light->LightGuid);
				}
			}
		}

		MeshBuildData.IrrelevantLights = PossiblyIrrelevantLights.Array();

		// Force recreation of the render data
		InstanceUpdateCmdBuffer.Edit();
		MarkRenderStateDirty();
	}
}

FBox UInstancedStaticMeshComponent::GetStreamingBounds() const
{
	return (GetStaticMesh() && PerInstanceSMData.Num()) ? Super::GetStreamingBounds() : FBox(ForceInit);
}
#endif

void UInstancedStaticMeshComponent::ReleasePerInstanceRenderData()
{
	if (PerInstanceRenderData.IsValid())
	{
		typedef TSharedPtr<FPerInstanceRenderData, ESPMode::ThreadSafe> FPerInstanceRenderDataPtr;

		PerInstanceRenderData->HitProxies.Empty();

		// Make shared pointer object on the heap
		FPerInstanceRenderDataPtr* CleanupRenderDataPtr = new FPerInstanceRenderDataPtr(PerInstanceRenderData);
		PerInstanceRenderData.Reset();

		FPerInstanceRenderDataPtr* InCleanupRenderDataPtr = CleanupRenderDataPtr;
		ENQUEUE_RENDER_COMMAND(FReleasePerInstanceRenderData)(
			[InCleanupRenderDataPtr](FRHICommandList& RHICmdList)
			{
				// Destroy the shared pointer object we allocated on the heap.
				// Resource will either be released here or by scene proxy on the render thread, whoever gets executed last
				delete InCleanupRenderDataPtr;
			});
	} //-V773
}

void UInstancedStaticMeshComponent::PropagateLightingScenarioChange()
{
	FComponentRecreateRenderStateContext Context(this);

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
}

void UInstancedStaticMeshComponent::GetLightAndShadowMapMemoryUsage( int32& LightMapMemoryUsage, int32& ShadowMapMemoryUsage ) const
{
	Super::GetLightAndShadowMapMemoryUsage(LightMapMemoryUsage, ShadowMapMemoryUsage);

	int32 NumInstances = PerInstanceSMData.Num();

	// Scale lighting demo by number of instances
	LightMapMemoryUsage *= NumInstances;
	ShadowMapMemoryUsage *= NumInstances;
}

// Deprecated version of PerInstanceSMData
struct FInstancedStaticMeshInstanceData_DEPRECATED
{
	FMatrix44f Transform;
	FVector2f LightmapUVBias;
	FVector2f ShadowmapUVBias;
	
	friend FArchive& operator<<(FArchive& Ar, FInstancedStaticMeshInstanceData_DEPRECATED& InstanceData)
	{
		// @warning BulkSerialize: FInstancedStaticMeshInstanceData is serialized as memory dump
		Ar << InstanceData.Transform << InstanceData.LightmapUVBias << InstanceData.ShadowmapUVBias;
		return Ar;
	}
};

static bool NeedRenderDataForTargetPlatform(const ITargetPlatform* TargetPlatform)
{
#if WITH_EDITOR
	const UDeviceProfile* DeviceProfile = UDeviceProfileManager::Get().FindProfile(TargetPlatform->IniPlatformName());
	if (DeviceProfile)
	{
		int32 CVarFoliageSaveRenderData = 1;
		if (DeviceProfile->GetConsolidatedCVarValue(TEXT("foliage.SaveRenderData"), CVarFoliageSaveRenderData))
		{
			return CVarFoliageSaveRenderData != 0;
		}
	}
#endif // WITH_EDITOR
	return true;
}

void UInstancedStaticMeshComponent::SerializeRenderData(FArchive& Ar)
{
	if (Ar.IsLoading())
	{
		uint64 RenderDataSizeBytes = 0;
		Ar << RenderDataSizeBytes; // TODO: can skip serialization if we know that data will be discarded

		if (RenderDataSizeBytes > 0)
		{
			InstanceDataBuffers = MakeUnique<FStaticMeshInstanceData>();
			InstanceDataBuffers->Serialize(Ar);
		}
	}
	else if (Ar.IsSaving())
	{
		uint64 RenderDataSizePos = Ar.Tell();
		
		// write render data size, will write real size later
		uint64 RenderDataSizeBytes = 0;
		Ar << RenderDataSizeBytes;

		bool bSaveRenderData = NeedRenderDataForTargetPlatform(Ar.CookingTarget());
		if (bSaveRenderData && !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			uint64 RenderDataPos = Ar.Tell();

			if (PerInstanceSMData.Num() > 0 && PerInstanceRenderData.IsValid())
			{
				check(PerInstanceRenderData.IsValid());

				// This will usually happen when having a BP adding instance through the construct script
				if (PerInstanceRenderData->InstanceBuffer.GetNumInstances() != PerInstanceSMData.Num() || InstanceUpdateCmdBuffer.NumTotalCommands() > 0)
				{
					FlushInstanceUpdateCommands();
					MarkRenderStateDirty();
				}
			}
		
			if (PerInstanceRenderData.IsValid())
			{
				if (PerInstanceRenderData->InstanceBuffer_GameThread && PerInstanceRenderData->InstanceBuffer_GameThread->GetNumInstances() > 0)
				{
					int32 NumInstances = PerInstanceRenderData->InstanceBuffer_GameThread->GetNumInstances();

					// Clear editor data for the cooked data
					for (int32 Index = 0; Index < NumInstances; ++Index)
					{
						const int32 RenderIndex = GetRenderIndex(Index);
						if (RenderIndex == INDEX_NONE)
						{
							// could be skipped by density settings
							continue;
						}

						PerInstanceRenderData->InstanceBuffer_GameThread->ClearInstanceEditorData(RenderIndex);
					}

					PerInstanceRenderData->InstanceBuffer_GameThread->Serialize(Ar);

#if WITH_EDITOR
					// Restore back the state we were in
					TArray<TRefCountPtr<HHitProxy>> HitProxies;
					CreateHitProxyData(HitProxies);

					for (int32 Index = 0; Index < NumInstances; ++Index)
					{
						const int32 RenderIndex = GetRenderIndex(Index);
						if (RenderIndex == INDEX_NONE)
						{
							// could be skipped by density settings
							continue;
						}

						// Record if the instance is selected
						FColor HitProxyColor(ForceInit);
						bool bSelected = SelectedInstances.IsValidIndex(Index) && SelectedInstances[Index];

						if (HitProxies.IsValidIndex(Index))
						{
							HitProxyColor = HitProxies[Index]->Id.GetColor();
						}

						PerInstanceRenderData->InstanceBuffer_GameThread->SetInstanceEditorData(RenderIndex, HitProxyColor, bSelected);
					}
#endif					
				}
			}

			// save render data real size
			uint64 CurPos = Ar.Tell();
			RenderDataSizeBytes = CurPos - RenderDataPos;
			Ar.Seek(RenderDataSizePos);
			Ar << RenderDataSizeBytes;
			Ar.Seek(CurPos);
		}
	}
}

void UInstancedStaticMeshComponent::Serialize(FArchive& Ar)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FMobileObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);	
	Ar.UsingCustomVersion(FRenderingObjectVersion::GUID);
	
	bool bCooked = Ar.IsCooking();
	if (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::SerializeInstancedStaticMeshRenderData || Ar.CustomVer(FEditorObjectVersion::GUID) >= FEditorObjectVersion::SerializeInstancedStaticMeshRenderData)
	{
		Ar << bCooked;
	}

#if WITH_EDITOR
	if (Ar.IsLoading() && Ar.CustomVer(FMobileObjectVersion::GUID) < FMobileObjectVersion::InstancedStaticMeshLightmapSerialization)
	{
		TArray<FInstancedStaticMeshInstanceData_DEPRECATED> DeprecatedData;
		DeprecatedData.BulkSerialize(Ar);
		PerInstanceSMData.Reset(DeprecatedData.Num());
		Algo::Transform(DeprecatedData, PerInstanceSMData, [](const FInstancedStaticMeshInstanceData_DEPRECATED& OldData){ 
			return FInstancedStaticMeshInstanceData(OldData.Transform);
		});
	}
	else
#endif //WITH_EDITOR
	{
		PerInstanceSMData.BulkSerialize(Ar, true); // LWC_TODO: Serializer. Need to force per element as PerInstance data contains an FMatrix. Should force using Ar.UEVer < LWC. 
	}

	if (!Ar.IsLoading() || Ar.CustomVer(FRenderingObjectVersion::GUID) >= FRenderingObjectVersion::PerInstanceCustomData)
	{
		PerInstanceSMCustomData.BulkSerialize(Ar);
	}

	if (bCooked && (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::SerializeInstancedStaticMeshRenderData || Ar.CustomVer(FEditorObjectVersion::GUID) >= FEditorObjectVersion::SerializeInstancedStaticMeshRenderData))
	{
		SerializeRenderData(Ar);
	}
	
#if WITH_EDITOR
	if( Ar.IsTransacting() )
	{
		Ar << SelectedInstances;
	}
#endif
}

void UInstancedStaticMeshComponent::PreAllocateInstancesMemory(int32 AddedInstanceCount)
{
	PerInstanceSMData.Reserve(PerInstanceSMData.Num() + AddedInstanceCount);
	PerInstanceSMCustomData.Reserve(PerInstanceSMCustomData.Num() + AddedInstanceCount * NumCustomDataFloats);
}

int32 UInstancedStaticMeshComponent::AddInstanceInternal(int32 InstanceIndex, FInstancedStaticMeshInstanceData* InNewInstanceData, const FTransform& InstanceTransform, bool bWorldSpace)
{
	FInstancedStaticMeshInstanceData* NewInstanceData = InNewInstanceData;

	if (NewInstanceData == nullptr)
	{
		NewInstanceData = &PerInstanceSMData.AddDefaulted_GetRef();
	}

	const FTransform LocalTransform = bWorldSpace ? InstanceTransform.GetRelativeTransform(GetComponentTransform()) : InstanceTransform;
	SetupNewInstanceData(*NewInstanceData, InstanceIndex, LocalTransform);

	// Add custom data to instance
	PerInstanceSMCustomData.AddZeroed(NumCustomDataFloats);

#if WITH_EDITOR
	if (SelectedInstances.Num())
	{
		SelectedInstances.Add(false);
	}
#endif

	PartialNavigationUpdate(InstanceIndex);

	if (FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.IsBound())
	{
		FInstancedStaticMeshDelegates::FInstanceIndexUpdateData IndexUpdate{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Added, InstanceIndex };
		FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.Broadcast(this, MakeArrayView(&IndexUpdate, 1));
	}

	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();

	return InstanceIndex;
}

int32 UInstancedStaticMeshComponent::AddInstance(const FTransform& InstanceTransform, bool bWorldSpace)
{
	return AddInstanceInternal(PerInstanceSMData.Num(), nullptr, InstanceTransform, bWorldSpace);
}

TArray<int32> UInstancedStaticMeshComponent::AddInstancesInternal(const TArray<FTransform>& InstanceTransforms, bool bShouldReturnIndices, bool bWorldSpace)
{
	const int32 Count = InstanceTransforms.Num();

	TArray<int32> NewInstanceIndices;

	if (bShouldReturnIndices)
	{
		NewInstanceIndices.Reserve(Count);
	}

	int32 InstanceIndex = PerInstanceSMData.Num();

	PerInstanceSMCustomData.AddZeroed(NumCustomDataFloats * Count);

#if WITH_EDITOR
	SelectedInstances.Add(false, Count);
#endif

	for (const FTransform& InstanceTransform : InstanceTransforms)
	{
		FInstancedStaticMeshInstanceData& NewInstanceData = PerInstanceSMData.AddDefaulted_GetRef();

		const FTransform LocalTransform = bWorldSpace ? InstanceTransform.GetRelativeTransform(GetComponentTransform()) : InstanceTransform;
		SetupNewInstanceData(NewInstanceData, InstanceIndex, LocalTransform);

		if (bShouldReturnIndices)
		{
			NewInstanceIndices.Add(InstanceIndex);
		}

		if (SupportsPartialNavigationUpdate())
		{
			PartialNavigationUpdate(InstanceIndex);
		}

		if (FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.IsBound())
		{
			FInstancedStaticMeshDelegates::FInstanceIndexUpdateData IndexUpdate{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Added, InstanceIndex };
			FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.Broadcast(this, MakeArrayView(&IndexUpdate, 1));
		}

		++InstanceIndex;
	}

	if (!SupportsPartialNavigationUpdate())
	{
		// Index parameter is ignored if partial navigation updates are not supported
		PartialNavigationUpdate(0);
	}

	// Batch update the render state after all instances are finished building
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();

	return NewInstanceIndices;
}

TArray<int32> UInstancedStaticMeshComponent::AddInstances(const TArray<FTransform>& InstanceTransforms, bool bShouldReturnIndices, bool bWorldSpace)
{
	return AddInstancesInternal(InstanceTransforms, bShouldReturnIndices, bWorldSpace);
}

// Per Instance Custom Data - Updating custom data for specific instance
bool UInstancedStaticMeshComponent::SetCustomDataValue(int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue, bool bMarkRenderStateDirty)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomDataIndex < 0 || CustomDataIndex >= NumCustomDataFloats)
	{
		return false;
	}

	Modify();

	PerInstanceSMCustomData[InstanceIndex * NumCustomDataFloats + CustomDataIndex] = CustomDataValue;

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::SetCustomData(int32 InstanceIndex, const TArray<float>& InCustomData, bool bMarkRenderStateDirty)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || InCustomData.Num() == 0)
	{
		return false;
	}

	if (bMarkRenderStateDirty)
	{
		Modify();
	}

	const int32 NumToCopy = FMath::Min(InCustomData.Num(), NumCustomDataFloats);
	FMemory::Memcpy(&PerInstanceSMCustomData[InstanceIndex * NumCustomDataFloats], InCustomData.GetData(), NumToCopy * InCustomData.GetTypeSize());

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::RemoveInstanceInternal(int32 InstanceIndex, bool InstanceAlreadyRemoved)
{
	// remove instance
	if (!InstanceAlreadyRemoved && PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		// Request navigation update
		PartialNavigationUpdate(InstanceIndex);

		PerInstanceSMData.RemoveAt(InstanceIndex);
		PerInstanceSMCustomData.RemoveAt(InstanceIndex * NumCustomDataFloats, NumCustomDataFloats);
	}

#if WITH_EDITOR
	// remove selection flag if array is filled in
	if (SelectedInstances.IsValidIndex(InstanceIndex))
	{
		SelectedInstances.RemoveAt(InstanceIndex);
	}
#endif

	// update the physics state
	if (bPhysicsStateCreated && InstanceBodies.IsValidIndex(InstanceIndex))
	{
		if (FBodyInstance*& InstanceBody = InstanceBodies[InstanceIndex])
		{
			InstanceBody->TermBody();
			delete InstanceBody;
			InstanceBody = nullptr;

			InstanceBodies.RemoveAt(InstanceIndex);

			// Re-target instance indices for shifting of array.
			for (int32 i = InstanceIndex; i < InstanceBodies.Num(); ++i)
			{
				InstanceBodies[i]->InstanceBodyIndex = i;
			}
		}
	}

	// Notify that these instances have been removed/relocated
	if (FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.IsBound())
	{
		TArray<FInstancedStaticMeshDelegates::FInstanceIndexUpdateData, TInlineAllocator<128>> IndexUpdates;
		IndexUpdates.Reserve(1 + (PerInstanceSMData.Num() - InstanceIndex));

		IndexUpdates.Add(FInstancedStaticMeshDelegates::FInstanceIndexUpdateData{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Removed, InstanceIndex });
		for (int32 MovedInstanceIndex = InstanceIndex; MovedInstanceIndex < PerInstanceSMData.Num(); ++MovedInstanceIndex)
		{
			// ISMs use standard remove, so each instance above our removal point is shuffled down by 1
			IndexUpdates.Add(FInstancedStaticMeshDelegates::FInstanceIndexUpdateData{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Relocated, MovedInstanceIndex, MovedInstanceIndex + 1 });
		}

		FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.Broadcast(this, IndexUpdates);
	}

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
	return true;
}

bool UInstancedStaticMeshComponent::RemoveInstance(int32 InstanceIndex)
{
	return RemoveInstanceInternal(InstanceIndex, false);
}

bool UInstancedStaticMeshComponent::RemoveInstances(const TArray<int32>& InstancesToRemove)
{
	// Sort so Remove doesn't alter the indices of items still to remove
	TArray<int32> SortedInstancesToRemove = InstancesToRemove;
	SortedInstancesToRemove.Sort(TGreater<int32>());

	if (!PerInstanceSMData.IsValidIndex(SortedInstancesToRemove[0]) || !PerInstanceSMData.IsValidIndex(SortedInstancesToRemove.Last()))
	{
		return false;
	}

	for (const int32 InstanceIndex : SortedInstancesToRemove)
	{
		RemoveInstanceInternal(InstanceIndex, false);
	}

	return true;
}

bool UInstancedStaticMeshComponent::GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	const FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];

	OutInstanceTransform = FTransform(InstanceData.Transform);
	if (bWorldSpace)
	{
		OutInstanceTransform = OutInstanceTransform * GetComponentTransform();
	}

	return true;
}

bool UInstancedStaticMeshComponent::GetInstancePrevTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	if (!PerInstancePrevTransform.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	const FMatrix& InstanceData = PerInstancePrevTransform[InstanceIndex];

	OutInstanceTransform = FTransform(InstanceData);
	if (bWorldSpace)
	{
		OutInstanceTransform = OutInstanceTransform * GetComponentTransform();
	}

	return true;
}


void UInstancedStaticMeshComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	// We are handling the physics move below, so don't handle it at higher levels
	Super::OnUpdateTransform(UpdateTransformFlags | EUpdateTransformFlags::SkipPhysicsUpdate, Teleport);

	const bool bTeleport = TeleportEnumToFlag(Teleport);

	// Always send new transform to physics
	if (bPhysicsStateCreated && !(EUpdateTransformFlags::SkipPhysicsUpdate & UpdateTransformFlags))
	{
		for (int32 i = 0; i < PerInstanceSMData.Num(); i++)
		{
			const FTransform InstanceTransform(PerInstanceSMData[i].Transform);
			UpdateInstanceBodyTransform(i, InstanceTransform * GetComponentTransform(), bTeleport);
		}
	}
}

void UInstancedStaticMeshComponent::UpdateInstanceBodyTransform(int32 InstanceIndex, const FTransform& WorldSpaceInstanceTransform, bool bTeleport)
{
	check(bPhysicsStateCreated);

	FBodyInstance*& InstanceBodyInstance = InstanceBodies[InstanceIndex];
#if WITH_PHYSX
	if (WorldSpaceInstanceTransform.GetScale3D().IsNearlyZero())
	{
		if (InstanceBodyInstance)
		{
			// delete BodyInstance
			InstanceBodyInstance->TermBody();
			delete InstanceBodyInstance;
			InstanceBodyInstance = nullptr;
		}
	}
	else
	{
		if (InstanceBodyInstance)
		{
			// Update existing BodyInstance
			InstanceBodyInstance->SetBodyTransform(WorldSpaceInstanceTransform, TeleportFlagToEnum(bTeleport));
			InstanceBodyInstance->UpdateBodyScale(WorldSpaceInstanceTransform.GetScale3D());
		}
		else
		{
			// create new BodyInstance
			InstanceBodyInstance = new FBodyInstance();
			InitInstanceBody(InstanceIndex, InstanceBodyInstance);
		}
	}
#endif //WITH_PHYSX
}

bool UInstancedStaticMeshComponent::UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	Modify();

	FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];

    // TODO: Computing LocalTransform is useless when we're updating the world location for the entire mesh.
	// Should find some way around this for performance.
    
	// Render data uses local transform of the instance
	FTransform LocalTransform = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(GetComponentTransform()) : NewInstanceTransform;
	InstanceData.Transform = LocalTransform.ToMatrixWithScale();

	if (bPhysicsStateCreated)
	{
		// Physics uses world transform of the instance
		FTransform WorldTransform = bWorldSpace ? NewInstanceTransform : (LocalTransform * GetComponentTransform());
		UpdateInstanceBodyTransform(InstanceIndex, WorldTransform, bTeleport);
	}

	// Request navigation update
	PartialNavigationUpdate(InstanceIndex);

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::BatchUpdateInstancesTransforms(int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, const TArray<FTransform>& NewInstancesPrevTransforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	// Number of current and prev transforms must match 
	check(NewInstancesTransforms.Num() == NewInstancesPrevTransforms.Num());

	// Early out if trying to update an invalid range
	if (!PerInstanceSMData.IsValidIndex(StartInstanceIndex) || !PerInstanceSMData.IsValidIndex(StartInstanceIndex + NewInstancesTransforms.Num() - 1))
	{
		return false;
	}

	// If the new transform index range is ok for PerInstanceSMData, it must also be ok for PerInstancePrevTransform
	check(PerInstancePrevTransform.IsValidIndex(StartInstanceIndex) && PerInstancePrevTransform.IsValidIndex(StartInstanceIndex + NewInstancesPrevTransforms.Num() - 1));

	Modify();

	for (int32 Index = 0; Index < NewInstancesTransforms.Num(); Index++)
	{
		const int32 InstanceIndex = StartInstanceIndex + Index;

		const FTransform& NewInstanceTransform = NewInstancesTransforms[Index];
		const FTransform& NewInstancePrevTransform = NewInstancesPrevTransforms[Index];

		FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];
		FMatrix& PrevInstanceData = PerInstancePrevTransform[InstanceIndex];

		// TODO: Computing LocalTransform is useless when we're updating the world location for the entire mesh.
		// Should find some way around this for performance.

		// Render data uses local transform of the instance
		FTransform LocalTransform = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(GetComponentTransform()) : NewInstanceTransform;
		InstanceData.Transform = LocalTransform.ToMatrixWithScale();

		FTransform LocalPrevTransform = bWorldSpace ? NewInstancePrevTransform.GetRelativeTransform(GetComponentTransform()) : NewInstancePrevTransform;
		PrevInstanceData = LocalPrevTransform.ToMatrixWithScale();

		if (bPhysicsStateCreated)
		{
			// Physics uses world transform of the instance
			FTransform WorldTransform = bWorldSpace ? NewInstanceTransform : (LocalTransform * GetComponentTransform());
			UpdateInstanceBodyTransform(InstanceIndex, WorldTransform, bTeleport);
		}

		
	}

	// Request navigation update - Execute on a single index as it updates everything anyway
	PartialNavigationUpdate(StartInstanceIndex);

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty || NewInstancesPrevTransforms.Num() > 0) // Hack: force invalidation since that's the only way to update the prev tansform on the render thread (proxy constructors)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::BatchUpdateInstancesTransforms(int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (!PerInstanceSMData.IsValidIndex(StartInstanceIndex) || !PerInstanceSMData.IsValidIndex(StartInstanceIndex + NewInstancesTransforms.Num() - 1))
	{
		return false;
	}

	Modify();

	int32 InstanceIndex = StartInstanceIndex;
	for (const FTransform& NewInstanceTransform : NewInstancesTransforms)
	{
		FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];

		// TODO: Computing LocalTransform is useless when we're updating the world location for the entire mesh.
		// Should find some way around this for performance.

		// Render data uses local transform of the instance
		FTransform LocalTransform = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(GetComponentTransform()) : NewInstanceTransform;
		InstanceData.Transform = LocalTransform.ToMatrixWithScale();

		if (bPhysicsStateCreated)
		{
			// Physics uses world transform of the instance
			FTransform WorldTransform = bWorldSpace ? NewInstanceTransform : (LocalTransform * GetComponentTransform());
			UpdateInstanceBodyTransform(InstanceIndex, WorldTransform, bTeleport);
		}

		InstanceIndex++;
	}

	// Request navigation update - Execute on a single index as it updates everything anyway
	PartialNavigationUpdate(StartInstanceIndex);

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::BatchUpdateInstancesTransform(int32 StartInstanceIndex, int32 NumInstances, const FTransform& NewInstancesTransform, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if(!PerInstanceSMData.IsValidIndex(StartInstanceIndex) || !PerInstanceSMData.IsValidIndex(StartInstanceIndex + NumInstances - 1))
	{
		return false;
	}

	Modify();

	int32 EndInstanceIndex = StartInstanceIndex + NumInstances;
	for(int32 InstanceIndex = StartInstanceIndex; InstanceIndex < EndInstanceIndex; ++InstanceIndex)
	{
		FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];

		// TODO: Computing LocalTransform is useless when we're updating the world location for the entire mesh.
		// Should find some way around this for performance.

		// Render data uses local transform of the instance
		FTransform LocalTransform = bWorldSpace ? NewInstancesTransform.GetRelativeTransform(GetComponentTransform()) : NewInstancesTransform;
		InstanceData.Transform = LocalTransform.ToMatrixWithScale();

		if(bPhysicsStateCreated)
		{
			// Physics uses world transform of the instance
			FTransform WorldTransform = bWorldSpace ? NewInstancesTransform : (LocalTransform * GetComponentTransform());
			UpdateInstanceBodyTransform(InstanceIndex, WorldTransform, bTeleport);
		}
	}

	// Request navigation update - Execute on a single index as it updates everything anyway
	PartialNavigationUpdate(StartInstanceIndex);

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if(bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UInstancedStaticMeshComponent::BatchUpdateInstancesData(int32 StartInstanceIndex, int32 NumInstances, FInstancedStaticMeshInstanceData* StartInstanceData, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (!PerInstanceSMData.IsValidIndex(StartInstanceIndex) || !PerInstanceSMData.IsValidIndex(StartInstanceIndex + NumInstances - 1))
	{
		return false;
	}

	Modify();

	for (int32 i = 0; i < NumInstances; ++i)
	{
		int32 InstanceIndex = StartInstanceIndex + i;
		FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];

		InstanceData = StartInstanceData[i];

		if (bPhysicsStateCreated)
		{
			// Physics uses world transform of the instance
			FTransform WorldTransform = FTransform(InstanceData.Transform) * GetComponentTransform();
			UpdateInstanceBodyTransform(InstanceIndex, WorldTransform, bTeleport);
		}
	}

	// Request navigation update - Execute on a single index as it updates everything anyway
	PartialNavigationUpdate(StartInstanceIndex);

	// Force recreation of the render data when proxy is created
	InstanceUpdateCmdBuffer.Edit();

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

TArray<int32> UInstancedStaticMeshComponent::GetInstancesOverlappingSphere(const FVector& Center, float Radius, bool bSphereInWorldSpace) const
{
	TArray<int32> Result;

	if (UStaticMesh* Mesh = GetStaticMesh())
	{
		FSphere Sphere(Center, Radius);
		if (bSphereInWorldSpace)
		{
			Sphere = Sphere.TransformBy(GetComponentTransform().Inverse());
		}

		const float StaticMeshBoundsRadius = Mesh->GetBounds().SphereRadius;

		for (int32 Index = 0; Index < PerInstanceSMData.Num(); Index++)
		{
			const FMatrix& Matrix = PerInstanceSMData[Index].Transform;
			const FSphere InstanceSphere(Matrix.GetOrigin(), StaticMeshBoundsRadius * Matrix.GetScaleVector().GetMax());

			if (Sphere.Intersects(InstanceSphere))
			{
				Result.Add(Index);
			}
		}
	}

	return Result;
}

TArray<int32> UInstancedStaticMeshComponent::GetInstancesOverlappingBox(const FBox& InBox, bool bBoxInWorldSpace) const
{
	TArray<int32> Result;

	if (UStaticMesh* Mesh = GetStaticMesh())
	{
		FBox Box(InBox);
		if (bBoxInWorldSpace)
		{
			Box = Box.TransformBy(GetComponentTransform().Inverse());
		}

		const FVector StaticMeshBoundsExtent = Mesh->GetBounds().BoxExtent;

		for (int32 Index = 0; Index < PerInstanceSMData.Num(); Index++)
		{
			const FMatrix& Matrix = PerInstanceSMData[Index].Transform;
			FBox InstanceBox(Matrix.GetOrigin() - StaticMeshBoundsExtent, Matrix.GetOrigin() + StaticMeshBoundsExtent);

			if (Box.Intersect(InstanceBox))
			{
				Result.Add(Index);
			}
		}
	}

	return Result;
}

bool UInstancedStaticMeshComponent::ShouldCreatePhysicsState() const
{
	return IsRegistered() && !IsBeingDestroyed() && GetStaticMesh() && !GetStaticMesh()->IsCompiling() && (bAlwaysCreatePhysicsState || IsCollisionEnabled());
}

float UInstancedStaticMeshComponent::GetTextureStreamingTransformScale() const
{
	// By default if there are no per instance data, use a scale of 1.
	// This is required because some derived class use the instancing system without filling the per instance data. (like landscape grass)
	// In those cases, we assume the instance are spreaded across the bounds with a scale of 1.
	float TransformScale = 1.f; 

	if (PerInstanceSMData.Num() > 0)
	{
		TransformScale = Super::GetTextureStreamingTransformScale();

		float WeightedAxisScaleSum = 0;
		float WeightSum = 0;

		for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
		{
			const float AxisScale = PerInstanceSMData[InstanceIndex].Transform.GetMaximumAxisScale();
			const float Weight = AxisScale; // The weight is the axis scale since we want to weight by surface coverage.
			WeightedAxisScaleSum += AxisScale * Weight;
			WeightSum += Weight;
		}

		if (WeightSum > SMALL_NUMBER)
		{
			TransformScale *= WeightedAxisScaleSum / WeightSum;
		}
	}
	return TransformScale;
}

bool UInstancedStaticMeshComponent::GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const
{
	// Same thing as StaticMesh but we take the full bounds to cover the instances.
	if (GetStaticMesh())
	{
		MaterialData.Material = GetMaterial(MaterialIndex);
		MaterialData.UVChannelData = GetStaticMesh()->GetUVChannelData(MaterialIndex);
		MaterialData.PackedRelativeBox = PackedRelativeBox_Identity;
	}
	return MaterialData.IsValid();
}

bool UInstancedStaticMeshComponent::BuildTextureStreamingDataImpl(ETextureStreamingBuildType BuildType, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel, TSet<FGuid>& DependentResources, bool& bOutSupportsBuildTextureStreamingData)
{
#if WITH_EDITORONLY_DATA // Only rebuild the data in editor 
	if (GetInstanceCount() > 0)
	{
		return Super::BuildTextureStreamingDataImpl(BuildType, QualityLevel, FeatureLevel, DependentResources, bOutSupportsBuildTextureStreamingData);
	}
#endif
	return true;
}

void UInstancedStaticMeshComponent::GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const
{
	// Don't only look the instance count but also if the bound is valid, as derived classes might not set PerInstanceSMData.
	if (GetInstanceCount() > 0 || Bounds.SphereRadius > 0)
	{
		return Super::GetStreamingRenderAssetInfo(LevelContext, OutStreamingRenderAssets);
	}
}

void UInstancedStaticMeshComponent::ClearInstances()
{
	const int32 PrevNumInstances = GetInstanceCount();

	// Clear all the per-instance data
	PerInstanceSMData.Empty();
	PerInstanceSMCustomData.Empty();
	InstanceReorderTable.Empty();
	InstanceDataBuffers.Reset();

	ProxySize = 0;

	// Release any physics representations
	ClearAllInstanceBodies();

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Reset();
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();

	// Notify that these instances have been cleared
	if (FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.IsBound())
	{
		FInstancedStaticMeshDelegates::FInstanceIndexUpdateData IndexUpdate{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Cleared, PrevNumInstances - 1 };
		FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.Broadcast(this, MakeArrayView(&IndexUpdate, 1));
	}

	FNavigationSystem::UpdateComponentData(*this);
}

int32 UInstancedStaticMeshComponent::GetInstanceCount() const
{
	return PerInstanceSMData.Num();
}

bool UInstancedStaticMeshComponent::IsValidInstance(int32 InstanceIndex) const
{
	return PerInstanceSMData.IsValidIndex(InstanceIndex);
}

void UInstancedStaticMeshComponent::SetCullDistances(int32 StartCullDistance, int32 EndCullDistance)
{
	if (InstanceStartCullDistance != StartCullDistance || InstanceEndCullDistance != EndCullDistance)
	{
		InstanceStartCullDistance = StartCullDistance;
		InstanceEndCullDistance = EndCullDistance;
		MarkRenderStateDirty();
	}
}

void UInstancedStaticMeshComponent::SetupNewInstanceData(FInstancedStaticMeshInstanceData& InOutNewInstanceData, int32 InInstanceIndex, const FTransform& InInstanceTransform)
{
	InOutNewInstanceData.Transform = InInstanceTransform.ToMatrixWithScale();

	if (bPhysicsStateCreated)
	{
		if (InInstanceTransform.GetScale3D().IsNearlyZero())
		{
			InstanceBodies.Insert(nullptr, InInstanceIndex);
		}
		else
		{
			FBodyInstance* NewBodyInstance = new FBodyInstance();
			int32 BodyIndex = InstanceBodies.Insert(NewBodyInstance, InInstanceIndex);
			check(InInstanceIndex == BodyIndex);
			InitInstanceBody(BodyIndex, NewBodyInstance);
		}
	}
}

static bool ComponentRequestsCPUAccess(UInstancedStaticMeshComponent* InComponent, ERHIFeatureLevel::Type FeatureLevel)
{
	bool bNeedsCPUAccess = false;

	// Ray tracing needs instance transforms on CPU
	bNeedsCPUAccess |= IsRayTracingEnabled();

	const UStaticMesh* StaticMesh = InComponent->GetStaticMesh();

	// Check mesh distance fields
	if (StaticMesh)
	{
		if ((FeatureLevel > ERHIFeatureLevel::ES3_1) || IsMobileDistanceFieldEnabled(GMaxRHIShaderPlatform))
		{
			// Mirror the conditions used in the FPrimitiveSceneProxy since these are used in IncludePrimitiveInDistanceFieldSceneData in RendererScene.cpp to filter the 
			// primitives that are included in the distance field scene. If these are not in sync, the host copy may be discarded and thus crashing in the distance field update.
			auto ShaderPlatform = GetFeatureLevelShaderPlatform(FeatureLevel);
			bNeedsCPUAccess |= PrimitiveNeedsDistanceFieldSceneData(
				ShouldAllPrimitivesHaveDistanceField(ShaderPlatform),
				/* bCastsDynamicIndirectShadow */ InComponent->bCastDynamicShadow && InComponent->CastShadow && InComponent->bCastDistanceFieldIndirectShadow && InComponent->Mobility != EComponentMobility::Static,
				InComponent->bAffectDistanceFieldLighting,
				true, /* conservatively overestimate DrawInGame - it has complex logic in the Proxy. */
				InComponent->bCastHiddenShadow,
				/* bCastsDynamicShadow */ InComponent->bCastDynamicShadow && InComponent->CastShadow && !InComponent->GetShadowIndirectOnly(),
				InComponent->bAffectDynamicIndirectLighting);
		}

		// Check Nanite
		if (FeatureLevel >= ERHIFeatureLevel::SM5)
		{
			// TODO: Call UseNanite(GetScene()->GetShaderPlatform())?
			//static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Nanite"));

		#if WITH_EDITOR
			const bool bHasNaniteData = StaticMesh->NaniteSettings.bEnabled;
		#else
			const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
			const bool bHasNaniteData = RenderData->NaniteResources.PageStreamingStates.Num() > 0;
		#endif

			bNeedsCPUAccess |= bHasNaniteData;
		}
	}

	return bNeedsCPUAccess;
}

void UInstancedStaticMeshComponent::GetInstancesMinMaxScale(FVector& MinScale, FVector& MaxScale) const
{
	if (PerInstanceSMData.Num() > 0)
	{
		MinScale = FVector(MAX_flt);
		MaxScale = FVector(-MAX_flt);

		for (int32 i = 0; i < PerInstanceSMData.Num(); ++i)
		{
			const FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[i];
			FVector ScaleVector = InstanceData.Transform.GetScaleVector();

			MinScale = MinScale.ComponentMin(ScaleVector);
			MaxScale = MaxScale.ComponentMax(ScaleVector);
		}
	}
	else
	{
		MinScale = FVector(1.0f);
		MaxScale = FVector(1.0f);
	}
}

void UInstancedStaticMeshComponent::InitPerInstanceRenderData(bool InitializeFromCurrentData, FStaticMeshInstanceData* InSharedInstanceBufferData, bool InRequireCPUAccess)
{
	if (PerInstanceRenderData.IsValid())
	{
		return;
	}

	LLM_SCOPE(ELLMTag::InstancedMesh);

	// If we don't have a random seed for this instanced static mesh component yet, then go ahead and
	// generate one now.  This will be saved with the static mesh component and used for future generation
	// of random numbers for this component's instances. (Used by the PerInstanceRandom material expression)
	while (InstancingRandomSeed == 0)
	{
		InstancingRandomSeed = FMath::Rand();
	}

	UWorld* World = GetWorld();
	ERHIFeatureLevel::Type FeatureLevel = World != nullptr ? World->FeatureLevel.GetValue() : GMaxRHIFeatureLevel;

	bool KeepInstanceBufferCPUAccess = UseGPUScene(GetFeatureLevelShaderPlatform(FeatureLevel), FeatureLevel) || GIsEditor || InRequireCPUAccess || ComponentRequestsCPUAccess(this, FeatureLevel);
	bool bTrackBounds = IsRayTracingEnabled() && bVisibleInRayTracing;

	FBox LocalBounds;
	GetLocalBounds(LocalBounds.Min, LocalBounds.Max);

	// If Nanite is used, we should defer the upload to GPU as the Nanite proxy simply will skip this step.
	// We can't just disable the upload, because at this point we can't know whether the Nanite proxy will be created in the end
	// this depends on the static mesh which may still be compiling/loading.
	// TODO: Perhaps make this specific to if this ISM actually has Nanite (if this can be detected reliably at this point) 
	const bool bDeferGPUUpload = UseNanite(GetFeatureLevelShaderPlatform(FeatureLevel));

	if (InSharedInstanceBufferData != nullptr)
	{
		PerInstanceRenderData = MakeShareable(new FPerInstanceRenderData(*InSharedInstanceBufferData, FeatureLevel, KeepInstanceBufferCPUAccess, LocalBounds, bTrackBounds, bDeferGPUUpload));
	}
	else
	{
		TArray<TRefCountPtr<HHitProxy>> HitProxies;
		FStaticMeshInstanceData InstanceBufferData = FStaticMeshInstanceData(GVertexElementTypeSupport.IsSupported(VET_Half2));
		
		if (InitializeFromCurrentData)
		{
			// since we recreate data, all pending edits will be uploaded
			InstanceUpdateCmdBuffer.Reset(); 
			BuildRenderData(InstanceBufferData, HitProxies);
		}
		
		PerInstanceRenderData = MakeShareable(new FPerInstanceRenderData(InstanceBufferData, FeatureLevel, KeepInstanceBufferCPUAccess, LocalBounds, bTrackBounds, bDeferGPUUpload));
		PerInstanceRenderData->HitProxies = MoveTemp(HitProxies);
	}
}

void UInstancedStaticMeshComponent::OnRegister()
{
	Super::OnRegister();

	if (FApp::CanEverRender() && !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// if we are pasting/duplicating this component, it may be created with some instances already in place
		// in this case, need to ensure that the instance render data is properly created
		// We only need to only init from current data if the reorder table == per instance data, but only for the HISM Component, in the case of ISM, the reorder table is never used.
		const bool InitializeFromCurrentData = PerInstanceSMData.Num() > 0 && (InstanceReorderTable.Num() == PerInstanceSMData.Num() || InstanceReorderTable.Num() == 0);
		InitPerInstanceRenderData(InitializeFromCurrentData);
	}
}

void UInstancedStaticMeshComponent::PostLoad()
{
	Super::PostLoad();

	// Has different implementation in HISMC
	OnPostLoadPerInstanceData();
}

void UInstancedStaticMeshComponent::OnPostLoadPerInstanceData()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInstancedStaticMeshComponent::OnPostLoadPerInstanceData);

	if (!HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject))
	{
		InitPerInstanceRenderData(true, InstanceDataBuffers.Get());
	}

	// release InstanceDataBuffers
	InstanceDataBuffers.Reset();

	if (PerInstanceRenderData.IsValid())
	{
		if (AActor* Owner = GetOwner())
		{
			ULevel* OwnerLevel = Owner->GetLevel();
			UWorld* OwnerWorld = OwnerLevel ? OwnerLevel->OwningWorld : nullptr;
			ULevel* ActiveLightingScenario = OwnerWorld ? OwnerWorld->GetActiveLightingScenario() : nullptr;

			if (ActiveLightingScenario && ActiveLightingScenario != OwnerLevel)
			{
				//update the instance data if the lighting scenario isn't the owner level
				InstanceUpdateCmdBuffer.Edit();
			}
		}
	}
}

void UInstancedStaticMeshComponent::PartialNavigationUpdate(int32 InstanceIdx)
{
	// Just update everything
	FNavigationSystem::UpdateComponentData(*this);
}

bool UInstancedStaticMeshComponent::DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const
{
	if (GetStaticMesh() && GetStaticMesh()->GetNavCollision())
	{
		UNavCollisionBase* NavCollision = GetStaticMesh()->GetNavCollision();
		if (NavCollision->IsDynamicObstacle())
		{
			return false;
		}
		
		if (NavCollision->HasConvexGeometry())
		{
			GeomExport.ExportCustomMesh(NavCollision->GetConvexCollision().VertexBuffer.GetData(), NavCollision->GetConvexCollision().VertexBuffer.Num(),
				NavCollision->GetConvexCollision().IndexBuffer.GetData(), NavCollision->GetConvexCollision().IndexBuffer.Num(), FTransform::Identity);

			GeomExport.ExportCustomMesh(NavCollision->GetTriMeshCollision().VertexBuffer.GetData(), NavCollision->GetTriMeshCollision().VertexBuffer.Num(),
				NavCollision->GetTriMeshCollision().IndexBuffer.GetData(), NavCollision->GetTriMeshCollision().IndexBuffer.Num(), FTransform::Identity);
		}
		else
		{
			UBodySetup* BodySetup = GetStaticMesh()->GetBodySetup();
			if (BodySetup)
			{
				GeomExport.ExportRigidBodySetup(*BodySetup, FTransform::Identity);
			}
		}

		// Hook per instance transform delegate
		GeomExport.SetNavDataPerInstanceTransformDelegate(FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UInstancedStaticMeshComponent::GetNavigationPerInstanceTransforms));
	}

	// we don't want "regular" collision export for this component
	return false;
}

void UInstancedStaticMeshComponent::GetNavigationData(FNavigationRelevantData& Data) const
{
	// Navigation data will get refreshed once async compilation finishes
	if (GetStaticMesh() && !GetStaticMesh()->IsCompiling() && GetStaticMesh()->GetNavCollision())
	{
		UNavCollisionBase* NavCollision = GetStaticMesh()->GetNavCollision();
		if (NavCollision->IsDynamicObstacle())
		{
			Data.Modifiers.MarkAsPerInstanceModifier();
			NavCollision->GetNavigationModifier(Data.Modifiers, FTransform::Identity);

			// Hook per instance transform delegate
			Data.NavDataPerInstanceTransformDelegate = FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UInstancedStaticMeshComponent::GetNavigationPerInstanceTransforms);
		}
	}
}

FBox UInstancedStaticMeshComponent::GetNavigationBounds() const
{
	return CalcBounds(GetComponentTransform()).GetBox();
}

bool UInstancedStaticMeshComponent::IsNavigationRelevant() const
{
	return GetInstanceCount() > 0 && Super::IsNavigationRelevant();
}

void UInstancedStaticMeshComponent::GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const
{
	for (const auto& InstancedData : PerInstanceSMData)
	{
		//TODO: Is it worth doing per instance bounds check here ?
		const FTransform InstanceToComponent(InstancedData.Transform);
		if (!InstanceToComponent.GetScale3D().IsZero())
		{
			InstanceData.Add(InstanceToComponent*GetComponentTransform());
		}
	}
}

void UInstancedStaticMeshComponent::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	if (PerInstanceRenderData.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceRenderData->ResourceSize); 
	}
	
	// component stuff
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceBodies.GetAllocatedSize());
	for (int32 i=0; i < InstanceBodies.Num(); ++i)
	{
		if (InstanceBodies[i] != NULL && InstanceBodies[i]->IsValidBodyInstance())
		{
			InstanceBodies[i]->GetBodyInstanceResourceSizeEx(CumulativeResourceSize);
		}
	}
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceSMData.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceSMCustomData.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceReorderTable.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceUpdateCmdBuffer.Cmds.GetAllocatedSize());
}

void UInstancedStaticMeshComponent::BeginDestroy()
{
	// Notify that these instances have been cleared due to the destroy
	if (FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.IsBound())
	{
		FInstancedStaticMeshDelegates::FInstanceIndexUpdateData IndexUpdate{ FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Destroyed, GetInstanceCount() - 1 };
		FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.Broadcast(this, MakeArrayView(&IndexUpdate, 1));
	}

	ReleasePerInstanceRenderData();

	Super::BeginDestroy();
}

#if WITH_EDITOR
void UInstancedStaticMeshComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	if(PropertyChangedEvent.Property != NULL)
	{
		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UInstancedStaticMeshComponent, PerInstanceSMData))
		{
			if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd
				|| PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate)
			{
				int32 AddedAtIndex = PropertyChangedEvent.GetArrayIndex(PropertyChangedEvent.Property->GetFName().ToString());
				check(AddedAtIndex != INDEX_NONE);

				AddInstanceInternal(AddedAtIndex, &PerInstanceSMData[AddedAtIndex], PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ? FTransform::Identity : FTransform(PerInstanceSMData[AddedAtIndex].Transform), /*bWorldSpace*/false);

				// added via the property editor, so we will want to interactively work with instances
				bHasPerInstanceHitProxies = true;
			}
			else if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove)
			{
				int32 RemovedAtIndex = PropertyChangedEvent.GetArrayIndex(PropertyChangedEvent.Property->GetFName().ToString());
				check(RemovedAtIndex != INDEX_NONE);

				RemoveInstanceInternal(RemovedAtIndex, true);
			}
			else if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear)
			{
				ClearInstances();
			}
			else if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ValueSet)
			{
				InstanceUpdateCmdBuffer.Edit();
			}
			
			MarkRenderStateDirty();
		}
		else if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(FInstancedStaticMeshInstanceData, Transform))
		{
			PartialNavigationUpdate(-1);
			// Force recreation of the render data
			InstanceUpdateCmdBuffer.Edit();
			MarkRenderStateDirty();
		}
		else if (PropertyChangedEvent.Property->GetFName() == "NumCustomDataFloats")
		{
			NumCustomDataFloats = FMath::Max(NumCustomDataFloats, 0);

			// Clear out and reinit to 0
			PerInstanceSMCustomData.Empty(PerInstanceSMData.Num() * NumCustomDataFloats);
			PerInstanceSMCustomData.SetNumZeroed(PerInstanceSMData.Num() * NumCustomDataFloats);

			InstanceUpdateCmdBuffer.Edit();
			MarkRenderStateDirty();
		}
		else if (PropertyChangedEvent.PropertyChain.GetActiveMemberNode()->GetValue()->GetFName() == "PerInstanceSMCustomData")
		{
			InstanceUpdateCmdBuffer.Edit();
			MarkRenderStateDirty();
		}
	}
	Super::PostEditChangeChainProperty(PropertyChangedEvent);
}

void UInstancedStaticMeshComponent::PostEditUndo()
{
	Super::PostEditUndo();

	FNavigationSystem::UpdateComponentData(*this);

	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
}
#endif

bool UInstancedStaticMeshComponent::IsInstanceSelected(int32 InInstanceIndex) const
{
#if WITH_EDITOR
	if(SelectedInstances.IsValidIndex(InInstanceIndex))
	{
		return SelectedInstances[InInstanceIndex];
	}
#endif

	return false;
}

void UInstancedStaticMeshComponent::SelectInstance(bool bInSelected, int32 InInstanceIndex, int32 InInstanceCount)
{
#if WITH_EDITOR
	if (InInstanceCount > 0)
	{
		if (PerInstanceSMData.Num() != SelectedInstances.Num())
		{
			SelectedInstances.Init(false, PerInstanceSMData.Num());
		}

		check(InInstanceIndex >= 0 && InInstanceCount > 0);
		check(InInstanceIndex + InInstanceCount - 1 < SelectedInstances.Num());
		
		for (int32 InstanceIndex = InInstanceIndex; InstanceIndex < InInstanceIndex + InInstanceCount; InstanceIndex++)
		{
			if (SelectedInstances.IsValidIndex(InInstanceIndex))
			{
				SelectedInstances[InstanceIndex] = bInSelected;

				if (PerInstanceRenderData.IsValid())
				{
					// Record if the instance is selected
					FColor HitProxyColor(ForceInit);
					if (PerInstanceRenderData->HitProxies.IsValidIndex(InstanceIndex))
					{
						HitProxyColor = PerInstanceRenderData->HitProxies[InstanceIndex]->Id.GetColor();
					}

					const int32 RenderIndex = GetRenderIndex(InstanceIndex);
					if (RenderIndex != INDEX_NONE)
					{
						InstanceUpdateCmdBuffer.SetEditorData(RenderIndex, HitProxyColor, bInSelected);
					}
				}
			}			
		}
		
		MarkRenderStateDirty();
	}
#endif
}

void UInstancedStaticMeshComponent::ClearInstanceSelection()
{
#if WITH_EDITOR
	int32 InstanceCount = SelectedInstances.Num();

	if (PerInstanceRenderData.IsValid())
	{
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; InstanceIndex++)
		{
			bool bSelected = SelectedInstances[InstanceIndex] != 0;
			if (bSelected)
			{
				FColor HitProxyColor(ForceInit);
				if (PerInstanceRenderData->HitProxies.IsValidIndex(InstanceIndex))
				{
					HitProxyColor = PerInstanceRenderData->HitProxies[InstanceIndex]->Id.GetColor();
				}
				
				const int32 RenderIndex = GetRenderIndex(InstanceIndex);
				if (RenderIndex != INDEX_NONE)
				{
					InstanceUpdateCmdBuffer.SetEditorData(RenderIndex, HitProxyColor, false);
				}
			}
		}
	}
	
	SelectedInstances.Empty();
	MarkRenderStateDirty();
#endif
}

bool UInstancedStaticMeshComponent::CanEditSMInstance(const FSMInstanceId& InstanceId) const
{
	check(InstanceId.ISMComponent == this);
	return IsEditableWhenInherited();
}

bool UInstancedStaticMeshComponent::CanMoveSMInstance(const FSMInstanceId& InstanceId, const ETypedElementWorldType InWorldType) const
{
	check(InstanceId.ISMComponent == this);
	return InWorldType == ETypedElementWorldType::Editor || InstanceId.ISMComponent->Mobility == EComponentMobility::Movable;
}

bool UInstancedStaticMeshComponent::GetSMInstanceTransform(const FSMInstanceId& InstanceId, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	check(InstanceId.ISMComponent == this);
	return GetInstanceTransform(InstanceId.InstanceIndex, OutInstanceTransform, bWorldSpace);
}

bool UInstancedStaticMeshComponent::SetSMInstanceTransform(const FSMInstanceId& InstanceId, const FTransform& InstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	check(InstanceId.ISMComponent == this);
	return UpdateInstanceTransform(InstanceId.InstanceIndex, InstanceTransform, bWorldSpace, bMarkRenderStateDirty, bTeleport);
}

void UInstancedStaticMeshComponent::NotifySMInstanceMovementStarted(const FSMInstanceId& InstanceId)
{
	check(InstanceId.ISMComponent == this);
}

void UInstancedStaticMeshComponent::NotifySMInstanceMovementOngoing(const FSMInstanceId& InstanceId)
{
	check(InstanceId.ISMComponent == this);
}

void UInstancedStaticMeshComponent::NotifySMInstanceMovementEnded(const FSMInstanceId& InstanceId)
{
	check(InstanceId.ISMComponent == this);
}

void UInstancedStaticMeshComponent::NotifySMInstanceSelectionChanged(const FSMInstanceId& InstanceId, const bool bIsSelected)
{
	check(InstanceId.ISMComponent == this);
	SelectInstance(bIsSelected, InstanceId.InstanceIndex);
}

bool UInstancedStaticMeshComponent::DeleteSMInstances(TArrayView<const FSMInstanceId> InstanceIds)
{
	TArray<int32> InstanceIndices;
	InstanceIndices.Reserve(InstanceIds.Num());
	for (const FSMInstanceId& InstanceId : InstanceIds)
	{
		check(InstanceId.ISMComponent == this);
		InstanceIndices.Add(InstanceId.InstanceIndex);
	}

	Modify();
	return RemoveInstances(InstanceIndices);
}

bool UInstancedStaticMeshComponent::DuplicateSMInstances(TArrayView<const FSMInstanceId> InstanceIds, TArray<FSMInstanceId>& OutNewInstanceIds)
{
	TArray<FTransform> NewInstanceTransforms;
	NewInstanceTransforms.Reserve(InstanceIds.Num());
	for (const FSMInstanceId& InstanceId : InstanceIds)
	{
		check(InstanceId.ISMComponent == this);
		FTransform& NewInstanceTransform = NewInstanceTransforms.Add_GetRef(FTransform::Identity);
		GetInstanceTransform(InstanceId.InstanceIndex, NewInstanceTransform);
	}

	Modify();
	const TArray<int32> NewInstanceIndices = AddInstances(NewInstanceTransforms, /*bShouldReturnIndices*/true);

	OutNewInstanceIds.Reset(NewInstanceIndices.Num());
	for (int32 NewInstanceIndex : NewInstanceIndices)
	{
		OutNewInstanceIds.Add(FSMInstanceId{ this, NewInstanceIndex });
	}

	return true;
}

static TAutoConsoleVariable<int32> CVarCullAllInVertexShader(
	TEXT("foliage.CullAllInVertexShader"),
	0,
	TEXT("Debugging, if this is greater than 0, cull all instances in the vertex shader."));

void FInstancedStaticMeshVertexFactoryShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams
	) const
{
	// Decode VertexFactoryUserData as VertexFactoryUniformBuffer
	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement, VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	const FInstancingUserData* InstancingUserData = (const FInstancingUserData*)BatchElement.UserData;
	const auto* InstancedVertexFactory = static_cast<const FInstancedStaticMeshVertexFactory*>(VertexFactory);
	const int32 InstanceOffsetValue = BatchElement.UserIndex;

	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(), InstancedVertexFactory->GetUniformBuffer());
	ShaderBindings.Add(InstanceOffset, InstanceOffsetValue);
	
	if (InstancedVertexFactory->SupportsManualVertexFetch(FeatureLevel))
	{
		ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, InstancedVertexFactory->GetInstanceOriginSRV());
		ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, InstancedVertexFactory->GetInstanceTransformSRV());
		ShaderBindings.Add(VertexFetch_InstanceLightmapBufferParameter, InstancedVertexFactory->GetInstanceLightmapSRV());
	}
	if (InstanceOffsetValue > 0 && VertexStreams.Num() > 0)
	{
		// GPUCULL_TODO: This here can still work together with the instance attributes for index, but note that all instance attributes then must assume they are offset wrt the on-the-fly generate buffer
		//          so with the new scheme there is no clear way this can work in the vanilla instancing way as there is an indirection. So either other attributes must be loaded in the shader or they
		//          would have to be copied as the instance ID is now - not good.
		VertexFactory->OffsetInstanceStreams(InstanceOffsetValue, InputStreamType, VertexStreams);
	}

	if( InstancingWorldViewOriginOneParameter.IsBound() )
	{
		FVector4f InstancingViewZCompareZero(MIN_flt, MIN_flt, MAX_flt, 1.0f);
		FVector4f InstancingViewZCompareOne(MIN_flt, MIN_flt, MAX_flt, 0.0f);
		FVector4f InstancingViewZConstant(ForceInit);
		FVector4f InstancingOffset(ForceInit);
		FVector4f InstancingTranslatedWorldViewOriginZero(ForceInit);
		FVector4f InstancingTranslatedWorldViewOriginOne(ForceInit);
		InstancingTranslatedWorldViewOriginOne.W = 1.0f;
		if (InstancingUserData && BatchElement.InstancedLODRange)
		{
			int32 FirstLOD = InstancingUserData->MinLOD;

			int32 DebugMin = FMath::Min(CVarMinLOD.GetValueOnRenderThread(), InstancingUserData->MeshRenderData->LODResources.Num() - 1);
			if (DebugMin >= 0)
			{
				FirstLOD = FMath::Max(FirstLOD, DebugMin);
			}

			FBoxSphereBounds ScaledBounds = InstancingUserData->MeshRenderData->Bounds.TransformBy(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, InstancingUserData->AverageInstancesScale));
			float SphereRadius = ScaledBounds.SphereRadius;
			float MinSize = View->ViewMatrices.IsPerspectiveProjection() ? CVarFoliageMinimumScreenSize.GetValueOnRenderThread() : 0.0f;
			float LODScale = CVarFoliageLODDistanceScale.GetValueOnRenderThread();
			float LODRandom = CVarRandomLODRange.GetValueOnRenderThread();
			float MaxDrawDistanceScale = GetCachedScalabilityCVars().ViewDistanceScale;

			if (BatchElement.InstancedLODIndex)
			{
				InstancingViewZConstant.X = -1.0f;
			}
			else
			{
				// this is the first LOD, so we don't have a fade-in region
				InstancingViewZConstant.X = 0.0f;
			}
			InstancingViewZConstant.Y = 0.0f;
			InstancingViewZConstant.Z = 1.0f;

			// now we subtract off the lower segments, since they will be incorporated 
			InstancingViewZConstant.Y -= InstancingViewZConstant.X;
			InstancingViewZConstant.Z -= InstancingViewZConstant.X + InstancingViewZConstant.Y;
			//not using W InstancingViewZConstant.W -= InstancingViewZConstant.X + InstancingViewZConstant.Y + InstancingViewZConstant.Z;

			for (int32 SampleIndex = 0; SampleIndex < 2; SampleIndex++)
			{
				FVector4f& InstancingViewZCompare(SampleIndex ? InstancingViewZCompareOne : InstancingViewZCompareZero);

				float FinalCull = MAX_flt;
				if (MinSize > 0.0)
				{
					FinalCull = ComputeBoundsDrawDistance(MinSize, SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
				}
				if (InstancingUserData->EndCullDistance > 0.0f)
				{
					FinalCull = FMath::Min(FinalCull, InstancingUserData->EndCullDistance * MaxDrawDistanceScale);
				}

				InstancingViewZCompare.Z = FinalCull;
				if (int(BatchElement.InstancedLODIndex) < InstancingUserData->MeshRenderData->LODResources.Num() - 1)
				{
					float NextCut = ComputeBoundsDrawDistance(InstancingUserData->MeshRenderData->ScreenSize[BatchElement.InstancedLODIndex + 1].GetValue(), SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
					InstancingViewZCompare.Z = FMath::Min(NextCut, FinalCull);
				}

				InstancingViewZCompare.X = MIN_flt;
				if (int(BatchElement.InstancedLODIndex) > FirstLOD)
				{
					float CurCut = ComputeBoundsDrawDistance(InstancingUserData->MeshRenderData->ScreenSize[BatchElement.InstancedLODIndex].GetValue(), SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
					if (CurCut < FinalCull)
					{
						InstancingViewZCompare.Y = CurCut;
					}
					else
					{
						// this LOD is completely removed by one of the other two factors
						InstancingViewZCompare.Y = MIN_flt;
						InstancingViewZCompare.Z = MIN_flt;
					}
				}
				else
				{
					// this is the first LOD, so we don't have a fade-in region
					InstancingViewZCompare.Y = MIN_flt;
				}
			}

			InstancingOffset = InstancingUserData->InstancingOffset;

			const FVector PreViewTranslation = View->ViewMatrices.GetPreViewTranslation();
			InstancingTranslatedWorldViewOriginZero = View->GetTemporalLODOrigin(0) + PreViewTranslation;
			InstancingTranslatedWorldViewOriginOne = View->GetTemporalLODOrigin(1) + PreViewTranslation;

			float Alpha = View->GetTemporalLODTransition();
			InstancingTranslatedWorldViewOriginZero.W = 1.0f - Alpha;
			InstancingTranslatedWorldViewOriginOne.W = Alpha;

			InstancingViewZCompareZero.W = LODRandom;
		}

		ShaderBindings.Add(InstancingViewZCompareZeroParameter, InstancingViewZCompareZero);
		ShaderBindings.Add(InstancingViewZCompareOneParameter, InstancingViewZCompareOne);
		ShaderBindings.Add(InstancingViewZConstantParameter, InstancingViewZConstant);
		ShaderBindings.Add(InstancingOffsetParameter, InstancingOffset);
		ShaderBindings.Add(InstancingWorldViewOriginZeroParameter, InstancingTranslatedWorldViewOriginZero);
		ShaderBindings.Add(InstancingWorldViewOriginOneParameter, InstancingTranslatedWorldViewOriginOne);
	}

	if( InstancingFadeOutParamsParameter.IsBound() )
	{
		FVector4f InstancingFadeOutParams(MAX_flt,0.f,1.f,1.f);
		if (InstancingUserData)
		{
			const float MaxDrawDistanceScale = GetCachedScalabilityCVars().ViewDistanceScale;
			const float StartDistance = InstancingUserData->StartCullDistance * MaxDrawDistanceScale;
			const float EndDistance = InstancingUserData->EndCullDistance * MaxDrawDistanceScale;

			InstancingFadeOutParams.X = StartDistance;
			if( EndDistance > 0 )
			{
				if( EndDistance > StartDistance )
				{
					InstancingFadeOutParams.Y = 1.f / (float)(EndDistance - StartDistance);
				}
				else
				{
					InstancingFadeOutParams.Y = 1.f;
				}
			}
			else
			{
				InstancingFadeOutParams.Y = 0.f;
			}
			if (CVarCullAllInVertexShader.GetValueOnRenderThread() > 0)
			{
				InstancingFadeOutParams.Z = 0.0f;
				InstancingFadeOutParams.W = 0.0f;
			}
			else
			{
				InstancingFadeOutParams.Z = InstancingUserData->bRenderSelected ? 1.f : 0.f;
				InstancingFadeOutParams.W = InstancingUserData->bRenderUnselected ? 1.f : 0.f;
			}
		}

		ShaderBindings.Add(InstancingFadeOutParamsParameter, InstancingFadeOutParams);

	}
}