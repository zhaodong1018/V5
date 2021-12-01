// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/NaniteResources.h"
#include "Rendering/NaniteStreamingManager.h"
#include "PrimitiveSceneInfo.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/InstancedStaticMesh.h"
#include "Materials/Material.h"
#include "RenderingThread.h"
#include "UnifiedBuffer.h"
#include "CommonRenderResources.h"
#include "StaticMeshResources.h"
#include "DistanceFieldAtlas.h"
#include "RenderGraphUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "AI/Navigation/NavCollisionBase.h"
#include "Misc/Compression.h"
#include "HAL/LowLevelMemStats.h"
#include "Interfaces/ITargetPlatform.h"
#include "NaniteSceneProxy.h"
#include "Rendering/NaniteCoarseMeshStreamingManager.h"

#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif

DEFINE_GPU_STAT(NaniteStreaming);
DEFINE_GPU_STAT(NaniteReadback);

DECLARE_LLM_MEMORY_STAT(TEXT("Nanite"), STAT_NaniteLLM, STATGROUP_LLMFULL);
DECLARE_LLM_MEMORY_STAT(TEXT("Nanite"), STAT_NaniteSummaryLLM, STATGROUP_LLM);
LLM_DEFINE_TAG(Nanite, NAME_None, NAME_None, GET_STATFNAME(STAT_NaniteLLM), GET_STATFNAME(STAT_NaniteSummaryLLM));

DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Total Instances"), STAT_NaniteInstanceCount, STATGROUP_Nanite);
DECLARE_MEMORY_STAT(TEXT("Nanite Proxy Instance Memory"), STAT_ProxyInstanceMemory, STATGROUP_Nanite);

#define MAX_CLUSTERS	(16 * 1024 * 1024)

int32 GNaniteOptimizedRelevance = 1;
FAutoConsoleVariableRef CVarNaniteOptimizedRelevance(
	TEXT("r.Nanite.OptimizedRelevance"),
	GNaniteOptimizedRelevance,
	TEXT("Whether to optimize Nanite relevance (outside of editor)."),
	ECVF_RenderThreadSafe
);

int32 GNaniteMaxNodes = 2 * 1048576;
FAutoConsoleVariableRef CVarNaniteMaxNodes(
	TEXT("r.Nanite.MaxNodes"),
	GNaniteMaxNodes,
	TEXT("Maximum number of Nanite nodes traversed during a culling pass."),
	ECVF_ReadOnly
);

int32 GNaniteMaxCandidateClusters = 16 * 1048576;
FAutoConsoleVariableRef CVarNaniteMaxCandidateClusters(
	TEXT("r.Nanite.MaxCandidateClusters"),
	GNaniteMaxCandidateClusters,
	TEXT("Maximum number of Nanite clusters before cluster culling."),
	ECVF_ReadOnly
);

int32 GNaniteMaxVisibleClusters = 2 * 1048576;
FAutoConsoleVariableRef CVarNaniteMaxVisibleClusters(
	TEXT("r.Nanite.MaxVisibleClusters"),
	GNaniteMaxVisibleClusters,
	TEXT("Maximum number of visible Nanite clusters."),
	ECVF_ReadOnly
);

int32 GRayTracingNaniteProxyMeshes = 1;
FAutoConsoleVariableRef CVarRayTracingNaniteProxyMeshes(
	TEXT("r.RayTracing.Geometry.NaniteProxies"),
	GRayTracingNaniteProxyMeshes,
	TEXT("Include Nanite proxy meshes in ray tracing effects (default = 1 (Nanite proxy meshes enabled in ray tracing))"),
	ECVF_RenderThreadSafe
);

int32 GNaniteErrorOnVertexInterpolator = 0;
FAutoConsoleVariableRef CVarNaniteErrorOnVertexInterpolator(
	TEXT("r.Nanite.ErrorOnVertexInterpolator"),
	GNaniteErrorOnVertexInterpolator,
	TEXT("Whether to error and use default material if vertex interpolator is present on a Nanite material."),
	ECVF_RenderThreadSafe
);

namespace Nanite
{

static_assert(sizeof(FPackedCluster) == NUM_PACKED_CLUSTER_FLOAT4S * 16, "NUM_PACKED_CLUSTER_FLOAT4S out of sync with sizeof(FPackedCluster)");

FArchive& operator<<(FArchive& Ar, FPackedHierarchyNode& Node)
{
	for (uint32 i = 0; i < MAX_BVH_NODE_FANOUT; i++)
	{
		Ar << Node.LODBounds[ i ];
		Ar << Node.Misc0[ i ].BoxBoundsCenter;
		Ar << Node.Misc0[ i ].MinLODError_MaxParentLODError;
		Ar << Node.Misc1[ i ].BoxBoundsExtent;
		Ar << Node.Misc1[ i ].ChildStartReference;
		Ar << Node.Misc2[ i ].ResourcePageIndex_NumPages_GroupPartSize;
	}
	
	return Ar;
}

FArchive& operator<<( FArchive& Ar, FPageStreamingState& PageStreamingState )
{
	Ar << PageStreamingState.BulkOffset;
	Ar << PageStreamingState.BulkSize;
	Ar << PageStreamingState.PageSize;
	Ar << PageStreamingState.DependenciesStart;
	Ar << PageStreamingState.DependenciesNum;
	Ar << PageStreamingState.Flags;
	return Ar;
}

void FResources::InitResources()
{
	// TODO: Should remove bulk data from built data if platform cannot run Nanite in any capacity
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	if (PageStreamingStates.Num() == 0)
	{
		// Skip resources that have their render data stripped
		return;
	}
	
	// Root pages should be available here. If they aren't, this resource has probably already been initialized and added to the streamer. Investigate!
	check(RootClusterPage.Num() > 0);

	ENQUEUE_RENDER_COMMAND(InitNaniteResources)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			GStreamingManager.Add(this);
		}
	);
}

bool FResources::ReleaseResources()
{
	// TODO: Should remove bulk data from built data if platform cannot run Nanite in any capacity
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return false;
	}

	if (PageStreamingStates.Num() == 0)
	{
		return false;
	}

	ENQUEUE_RENDER_COMMAND(ReleaseNaniteResources)(
		[this]( FRHICommandListImmediate& RHICmdList)
		{
			GStreamingManager.Remove(this);
		}
	);
	return true;
}

void FResources::Serialize(FArchive& Ar, UObject* Owner)
{
	LLM_SCOPE_BYTAG(Nanite);

	// Note: this is all derived data, native versioning is not needed, but be sure to bump NANITE_DERIVEDDATA_VER when modifying!
	FStripDataFlags StripFlags( Ar, 0 );
	if( !StripFlags.IsDataStrippedForServer() )
	{
		Ar << ResourceFlags;
		Ar << RootClusterPage;
		StreamableClusterPages.Serialize(Ar, Owner, 0);
		Ar << PageStreamingStates;
	
		Ar << HierarchyNodes;
		Ar << HierarchyRootOffsets;
		Ar << PageDependencies;
		Ar << ImposterAtlas;
		Ar << PositionPrecision;
		Ar << NumInputTriangles;
		Ar << NumInputVertices;
		Ar << NumInputMeshes;
		Ar << NumInputTexCoords;
	}
}

void FResources::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) const
{
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(sizeof(*this));
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(RootClusterPage.GetAllocatedSize());
	if (StreamableClusterPages.IsBulkDataLoaded())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(StreamableClusterPages.GetBulkDataSize());
	}
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(ImposterAtlas.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(HierarchyNodes.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(HierarchyRootOffsets.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PageStreamingStates.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PageDependencies.GetAllocatedSize());
}

class FVertexFactory final : public ::FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FVertexFactory);

public:
	FVertexFactory(ERHIFeatureLevel::Type FeatureLevel) : ::FVertexFactory(FeatureLevel)
	{
	}

	~FVertexFactory()
	{
		ReleaseResource();
	}

	virtual void InitRHI() override final
	{
		LLM_SCOPE_BYTAG(Nanite);

		FVertexStream VertexStream;
		VertexStream.VertexBuffer = &GScreenRectangleVertexBuffer;
		VertexStream.Offset = 0;

		Streams.Add(VertexStream);

		SetDeclaration(GFilterVertexDeclaration.VertexDeclarationRHI);
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
	{
		bool bShouldCompile = 
			(Parameters.MaterialParameters.bIsUsedWithNanite || Parameters.MaterialParameters.bIsSpecialEngineMaterial) &&
			Parameters.MaterialParameters.MaterialDomain == MD_Surface &&
			Parameters.MaterialParameters.BlendMode == BLEND_Opaque &&
			Parameters.ShaderType->GetFrequency() == SF_Pixel &&
			RHISupportsComputeShaders(Parameters.Platform) &&
			DoesPlatformSupportNanite(Parameters.Platform);

		return bShouldCompile;
	}

	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		::FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("IS_NANITE_FACTORY"), 1);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("NANITE_USE_UNIFORM_BUFFER"), 1);
		OutEnvironment.SetDefine(TEXT("NANITE_USE_VIEW_UNIFORM_BUFFER"), 1);
	}
};
IMPLEMENT_VERTEX_FACTORY_TYPE(Nanite::FVertexFactory, "/Engine/Private/Nanite/NaniteVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
	| EVertexFactoryFlags::SupportsNaniteRendering
);

SIZE_T FSceneProxyBase::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FSceneProxyBase::DrawStaticElementsInternal(FStaticPrimitiveDrawInterface* PDI, const FLightCacheInterface* LCI)
{
	LLM_SCOPE_BYTAG(Nanite);

	FMeshBatch MeshBatch;
	MeshBatch.VertexFactory = GGlobalResources.GetVertexFactory();
	MeshBatch.Type = GRHISupportsRectTopology ? PT_RectList : PT_TriangleList;
	MeshBatch.ReverseCulling = false;
	MeshBatch.bDisableBackfaceCulling = true;
	MeshBatch.DepthPriorityGroup = SDPG_World;
	MeshBatch.LODIndex = INDEX_NONE;
	MeshBatch.bWireframe = false;
	MeshBatch.bCanApplyViewModeOverrides = false;
	MeshBatch.LCI = LCI;
	MeshBatch.Elements[0].IndexBuffer = &GScreenRectangleIndexBuffer;
	MeshBatch.Elements[0].NumInstances = 1;
	MeshBatch.Elements[0].PrimitiveIdMode = PrimID_ForceZero;
	MeshBatch.Elements[0].PrimitiveUniformBufferResource = &GIdentityPrimitiveUniformBuffer;
	if (GRHISupportsRectTopology)
	{
		MeshBatch.Elements[0].FirstIndex = 9;
		MeshBatch.Elements[0].NumPrimitives = 1;
		MeshBatch.Elements[0].MinVertexIndex = 1;
		MeshBatch.Elements[0].MaxVertexIndex = 3;
	}
	else
	{
		MeshBatch.Elements[0].FirstIndex = 0;
		MeshBatch.Elements[0].NumPrimitives = 2;
		MeshBatch.Elements[0].MinVertexIndex = 0;
		MeshBatch.Elements[0].MaxVertexIndex = 3;
	}

	for (int32 SectionIndex = 0; SectionIndex < MaterialSections.Num(); ++SectionIndex)
	{
		const FMaterialSection& Section = MaterialSections[SectionIndex];
		const UMaterialInterface* Material = Section.Material;
		if (!Material)
		{
			continue;
		}

		MeshBatch.SegmentIndex = SectionIndex;
		MeshBatch.MaterialRenderProxy = Material->GetRenderProxy();

	#if WITH_EDITOR
		HHitProxy* HitProxy = Section.HitProxy;
		PDI->SetHitProxy(HitProxy);
	#endif
		PDI->DrawMesh(MeshBatch, FLT_MAX);
	}
}

FSceneProxy::FSceneProxy(UStaticMeshComponent* Component)
: FSceneProxyBase(Component)
, MeshInfo(Component)
, Resources(&Component->GetStaticMesh()->GetRenderData()->NaniteResources)
, RenderData(Component->GetStaticMesh()->GetRenderData())
, StaticMesh(Component->GetStaticMesh())
#if NANITE_ENABLE_DEBUG_RENDERING
, Owner(Component->GetOwner())
, LightMapResolution(Component->GetStaticLightMapResolution())
, BodySetup(Component->GetBodySetup())
, CollisionTraceFlag(ECollisionTraceFlag::CTF_UseSimpleAndComplex)
, CollisionResponse(Component->GetCollisionResponseToChannels())
, LODForCollision(Component->GetStaticMesh()->LODForCollision)
, bDrawMeshCollisionIfComplex(Component->bDrawMeshCollisionIfComplex)
, bDrawMeshCollisionIfSimple(Component->bDrawMeshCollisionIfSimple)
#endif
{
	LLM_SCOPE_BYTAG(Nanite);

	// Nanite requires GPUScene
	checkSlow(UseGPUScene(GMaxRHIShaderPlatform, GetScene().GetFeatureLevel()));
	checkSlow(DoesPlatformSupportNanite(GMaxRHIShaderPlatform));
	
	// This should always be valid.
	check(Resources);

	MaterialRelevance = Component->GetMaterialRelevance(Component->GetScene()->GetFeatureLevel());

	// Nanite supports the GPUScene instance data buffer.
	bSupportsInstanceDataBuffer = true;

	// Nanite supports distance field representation.
	bSupportsDistanceFieldRepresentation = MaterialRelevance.bOpaque;

	// Nanite supports mesh card representation.
	bSupportsMeshCardRepresentation = true;

	// Use fast path that does not update static draw lists.
	bStaticElementsAlwaysUseProxyPrimitiveUniformBuffer = true;

	// We always use local vertex factory, which gets its primitive data from
	// GPUScene, so we can skip expensive primitive uniform buffer updates.
	bVFRequiresPrimitiveUniformBuffer = false;

	// Indicates if 1 or more materials contain settings not supported by Nanite.
	bHasMaterialErrors = false;

	const bool bHasSurfaceStaticLighting = MeshInfo.GetLightMap() != nullptr || MeshInfo.GetShadowMap() != nullptr;

	const uint32 FirstLODIndex = 0; // Only data from LOD0 is used.
	const FStaticMeshLODResources& MeshResources = RenderData->LODResources[FirstLODIndex];
	const FStaticMeshSectionArray& MeshSections = MeshResources.Sections;

	// Copy the pointer to the volume data, async building of the data may modify the one on FStaticMeshLODResources while we are rendering
	DistanceFieldData = MeshResources.DistanceFieldData;
	CardRepresentationData = MeshResources.CardRepresentationData;
	
	MaterialSections.SetNumZeroed(MeshSections.Num());

	for (int32 SectionIndex = 0; SectionIndex < MeshSections.Num(); ++SectionIndex)
	{
		FMaterialSection& MaterialSection = MaterialSections[SectionIndex];
		const FStaticMeshSection& MeshSection = MeshSections[SectionIndex];
		const bool bValidMeshSection = MeshSection.MaterialIndex != INDEX_NONE;

		MaterialSection.MaterialIndex = MeshSection.MaterialIndex;

		// Keep track of highest observed material index.
		MaterialMaxIndex = FMath::Max(MaterialSection.MaterialIndex, MaterialMaxIndex);

		MaterialSection.Material = bValidMeshSection ? Component->GetMaterial(MaterialSection.MaterialIndex) : nullptr;

		if (MaterialSection.Material == nullptr)
		{
			MaterialSection.bHasNullMaterial = true;
			MaterialSection.Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		else if (!MaterialSection.Material->CheckMaterialUsage_Concurrent(MATUSAGE_Nanite))
		{
			MaterialSection.Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		else if (!Nanite::FSceneProxy::IsNaniteRenderable(MaterialRelevance))
		{
			MaterialSection.bHasInvalidRelevance = true;
		}
		else if (MaterialSection.Material->GetBlendMode() != BLEND_Opaque)
		{
			MaterialSection.bHasNonOpaqueBlendMode = true;
		}
		else if (bHasSurfaceStaticLighting && !MaterialSection.Material->CheckMaterialUsage_Concurrent(MATUSAGE_StaticLighting))
		{
			MaterialSection.bHasInvalidStaticLighting = true;
		}

		const UMaterial* Material = MaterialSection.Material->GetMaterial_Concurrent();
		check(Material != nullptr); // Should always be valid here

		const FMaterialCachedExpressionData& CachedMaterialData = MaterialSection.Material->GetCachedExpressionData();
		MaterialSection.bHasVertexInterpolator		= CachedMaterialData.bHasVertexInterpolator;
		MaterialSection.bHasPerInstanceRandomID		= CachedMaterialData.bHasPerInstanceRandom;
		MaterialSection.bHasPerInstanceCustomData	= CachedMaterialData.bHasPerInstanceCustomData;

		MaterialSection.bHasAnyError =
			MaterialSection.bHasNullMaterial ||
			MaterialSection.bHasInvalidRelevance ||
			MaterialSection.bHasNonOpaqueBlendMode ||
			//MaterialSection.bHasVertexInterpolator || 
			MaterialSection.bHasInvalidStaticLighting;

		if (GNaniteErrorOnVertexInterpolator != 0)
		{
			MaterialSection.bHasAnyError |= MaterialSection.bHasVertexInterpolator;
		}

		if (MaterialSection.bHasAnyError)
		{
			bHasMaterialErrors = true;

			const FString StaticMeshName = StaticMesh->GetName();
			const FString MaterialName = MaterialSection.Material->GetName();

			if (MaterialSection.bHasNullMaterial)
			{
				UE_LOG(LogStaticMesh, Warning, TEXT("Invalid material [null] used on Nanite static mesh [%s] - forcing default material instead."), *StaticMeshName);
			}
			else
			{
				// Replace invalid materials with default material
				MaterialSection.Material = UMaterial::GetDefaultMaterial(MD_Surface);

				if (MaterialSection.bHasInvalidRelevance)
				{
					UE_LOG(LogStaticMesh, Warning, TEXT("Invalid material relevance for Nanite static mesh [%s] - forcing default material instead."), *StaticMeshName);
				}
				else if (MaterialSection.bHasNonOpaqueBlendMode)
				{
					const FString BlendModeName = GetBlendModeString(MaterialSection.Material->GetBlendMode());
					UE_LOG
					(
						LogStaticMesh, Warning,
						TEXT("Invalid material [%s] used on Nanite static mesh [%s] - forcing default material instead. Only opaque blend mode is currently supported, [%s] blend mode was specified."),
						*MaterialName,
						*StaticMeshName,
						*BlendModeName
					);
				}
				else if (MaterialSection.bHasVertexInterpolator)
				{
					UE_LOG
					(
						LogStaticMesh, Warning,
						TEXT("Invalid material [%s] used on Nanite static mesh [%s] - forcing default material instead. Vertex interpolator nodes are not supported by Nanite."),
						*MaterialName,
						*StaticMeshName
					);
				}
				else
				{
					// Unimplemented error condition
					checkNoEntry();
				}
			}
		}
	}

#if RHI_RAYTRACING
	CachedRayTracingMaterials.SetNum(MaterialSections.Num());

	if (IsRayTracingEnabled())
	{
		CoarseMeshStreamingHandle = (Nanite::CoarseMeshStreamingHandle)Component->GetStaticMesh()->GetStreamingIndex();
		if (MeshResources.GetNumVertices())
		{
			bHasRayTracingInstances = true;
		}

		// This will be filled later (on the render thread) and cached.
		CachedRayTracingInstanceMaskAndFlags.Mask = 0;
	}
#endif

	FPrimitiveInstance& Instance = InstanceSceneData.Emplace_GetRef();
	Instance.LocalToPrimitive.SetIdentity();
	Instance.LocalBounds                = Component->GetStaticMesh()->GetBounds();
	Instance.NaniteHierarchyOffset      = 0u;
	Instance.Flags = 0u;
}

FSceneProxy::FSceneProxy(UInstancedStaticMeshComponent* Component)
: FSceneProxy(static_cast<UStaticMeshComponent*>(Component))
{
	LLM_SCOPE_BYTAG(Nanite);

	InstanceSceneData.SetNum(Component->GetInstanceCount());

	const bool bValidPreviousData = Component->PerInstancePrevTransform.Num() == Component->GetInstanceCount();
	InstanceDynamicData.SetNumUninitialized(bValidPreviousData ? Component->GetInstanceCount() : 0);

	InstanceRandomID.SetNumZeroed(Component->GetInstanceCount()); // TODO: Only allocate if material bound which uses this
	InstanceLightShadowUVBias.SetNumZeroed(Component->GetInstanceCount()); // TODO: Only allocate if static lighting is enabled for the project
	InstanceCustomData = Component->PerInstanceSMCustomData; // TODO: Only allocate if material bound which uses this
	check(Component->NumCustomDataFloats == 0 || (InstanceCustomData.Num() / Component->NumCustomDataFloats == Component->GetInstanceCount())); // Sanity check on the data packing

	bHasPerInstanceRandom = InstanceRandomID.Num() > 0; // TODO: Only allocate if material bound which uses this
	bHasPerInstanceCustomData = InstanceCustomData.Num() > 0; // TODO: Only allocate if material bound which uses this
	bHasPerInstanceDynamicData = InstanceDynamicData.Num() > 0;
	bHasPerInstanceLMSMUVBias = InstanceLightShadowUVBias.Num() > 0; // TODO: Only allocate if static lighting is enabled for the project

	uint32 InstanceDataFlags = 0;
	InstanceDataFlags |= bHasPerInstanceLMSMUVBias ? INSTANCE_SCENE_DATA_FLAG_HAS_LIGHTSHADOW_UV_BIAS : 0u;
	InstanceDataFlags |= bHasPerInstanceDynamicData ? INSTANCE_SCENE_DATA_FLAG_HAS_DYNAMIC_DATA : 0u;
	InstanceDataFlags |= bHasPerInstanceCustomData ? INSTANCE_SCENE_DATA_FLAG_HAS_CUSTOM_DATA : 0u;
	InstanceDataFlags |= bHasPerInstanceRandom ? INSTANCE_SCENE_DATA_FLAG_HAS_RANDOM : 0u;

	for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
	{
		FPrimitiveInstance& SceneData = InstanceSceneData[InstanceIndex];
		SceneData.LocalBounds = Component->GetStaticMesh()->GetBounds();
		SceneData.NaniteHierarchyOffset = 0U;
		SceneData.Flags = InstanceDataFlags;

		FTransform InstanceTransform;
		Component->GetInstanceTransform(InstanceIndex, InstanceTransform);
		SceneData.LocalToPrimitive = InstanceTransform.ToMatrixWithScale();

		if (bHasPerInstanceDynamicData)
		{
			FPrimitiveInstanceDynamicData& DynamicData = InstanceDynamicData[InstanceIndex];

			FTransform InstancePrevTransform;
			const bool bHasPrevTransform = Component->GetInstancePrevTransform(InstanceIndex, InstancePrevTransform);
			if (ensure(bHasPrevTransform)) // Should always be true here
			{
				DynamicData.PrevLocalToPrimitive = InstancePrevTransform.ToMatrixWithScale();
			}
			else
			{
				DynamicData.PrevLocalToPrimitive = SceneData.LocalToPrimitive;
			}
		}
	}

	ENQUEUE_RENDER_COMMAND(SetNanitePerInstanceData)(
		[this, PerInstanceRenderData = Component->PerInstanceRenderData](FRHICommandList& RHICmdList)
	{
		if (PerInstanceRenderData != nullptr &&
			PerInstanceRenderData->InstanceBuffer.GetNumInstances() == InstanceSceneData.Num())
		{
			if (bHasPerInstanceRandom || bHasPerInstanceLMSMUVBias)
			{
				for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
				{
					if (bHasPerInstanceRandom)
					{
						PerInstanceRenderData->InstanceBuffer.GetInstanceRandomID(InstanceIndex, InstanceRandomID[InstanceIndex]);
					}

					if (bHasPerInstanceLMSMUVBias)
					{
						PerInstanceRenderData->InstanceBuffer.GetInstanceLightMapData(InstanceIndex, InstanceLightShadowUVBias[InstanceIndex]);
					}
				}
			}
		}
	});

	// TODO: Should report much finer granularity than what this code is doing (i.e. dynamic vs static, per stream sizes, etc..)
	// TODO: Also should be reporting this for all proxies, not just the Nanite ones
	INC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceSceneData.GetAllocatedSize());
	INC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceDynamicData.GetAllocatedSize());
	INC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceCustomData.GetAllocatedSize());
	INC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceRandomID.GetAllocatedSize());
	INC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceLightShadowUVBias.GetAllocatedSize());
	INC_DWORD_STAT_BY(STAT_NaniteInstanceCount, InstanceSceneData.Num());

#if RHI_RAYTRACING
	if (InstanceSceneData.Num() == 0)
	{
		bHasRayTracingInstances = false;
	}
#endif
}

FSceneProxy::FSceneProxy(UHierarchicalInstancedStaticMeshComponent* Component)
: FSceneProxy(static_cast<UInstancedStaticMeshComponent*>(Component))
{
}

FSceneProxy::~FSceneProxy()
{
	// TODO: Should report much finer granularity than what this code is doing (i.e. dynamic vs static, per stream sizes, etc..)
	// TODO: Also should be reporting this for all proxies, not just the Nanite ones
	DEC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceSceneData.GetAllocatedSize());
	DEC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceDynamicData.GetAllocatedSize());
	DEC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceCustomData.GetAllocatedSize());
	DEC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceRandomID.GetAllocatedSize());
	DEC_MEMORY_STAT_BY(STAT_ProxyInstanceMemory, InstanceLightShadowUVBias.GetAllocatedSize());
	DEC_DWORD_STAT_BY(STAT_NaniteInstanceCount, InstanceSceneData.Num());
}

void FSceneProxy::CreateRenderThreadResources()
{
	// These couldn't be copied on the game thread because they are initialized
	// by the streaming manager on the render thread - initialize them now.
	check(Resources->RuntimeResourceID != NANITE_INVALID_RESOURCE_ID && Resources->HierarchyOffset != NANITE_INVALID_HIERARCHY_OFFSET);

	for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
	{
		// Regular static mesh instances only use hierarchy offset on primitive.
		InstanceSceneData[InstanceIndex].NaniteHierarchyOffset = 0;
	}
}

FPrimitiveViewRelevance FSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	LLM_SCOPE_BYTAG(Nanite);

#if WITH_EDITOR
	const bool bOptimizedRelevance = false;
#else
	const bool bOptimizedRelevance = GNaniteOptimizedRelevance != 0;
#endif

	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.NaniteMeshes;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();

	// Always render the Nanite mesh data with static relevance.
	Result.bStaticRelevance = true;

	// Should always be covered by constructor of Nanite scene proxy.
	Result.bRenderInMainPass = true;

	if (bOptimizedRelevance) // No dynamic relevance if optimized.
	{
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity();
	}
	else
	{
	#if WITH_EDITOR
		//only check these in the editor
		Result.bEditorVisualizeLevelInstanceRelevance = IsEditingLevelInstanceChild();
		Result.bEditorStaticSelectionRelevance = (IsSelected() || IsHovered());
	#endif

	#if NANITE_ENABLE_DEBUG_RENDERING
		bool bDrawSimpleCollision = false, bDrawComplexCollision = false;
		const bool bInCollisionView = IsCollisionView(View->Family->EngineShowFlags, bDrawSimpleCollision, bDrawComplexCollision);
	#else
		bool bInCollisionView = false;
	#endif

		// Set dynamic relevance for overlays like collision and bounds.
		bool bSetDynamicRelevance = false;
	#if !(UE_BUILD_SHIPPING) || WITH_EDITOR
		bSetDynamicRelevance |= (IsRichView(*View->Family) ||
			View->Family->EngineShowFlags.Collision ||
			bInCollisionView ||
			View->Family->EngineShowFlags.Bounds);
	#endif
	#if WITH_EDITOR
		bSetDynamicRelevance |= (IsSelected() && View->Family->EngineShowFlags.VertexColors);
	#endif
	#if NANITE_ENABLE_DEBUG_RENDERING
		bSetDynamicRelevance |= bDrawMeshCollisionIfComplex || bDrawMeshCollisionIfSimple;
	#endif

		if (bSetDynamicRelevance)
		{
			Result.bDynamicRelevance = true;

		#if NANITE_ENABLE_DEBUG_RENDERING
			// If we want to draw collision, needs to make sure we are considered relevant even if hidden
			if (View->Family->EngineShowFlags.Collision || bInCollisionView)
			{
				Result.bDrawRelevance = true;
			}
		#endif
		}

		if (!View->Family->EngineShowFlags.Materials
		#if NANITE_ENABLE_DEBUG_RENDERING
			|| bInCollisionView
		#endif
			)
		{
			Result.bOpaque = true;
		}

		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = Result.bOpaque && Result.bRenderInMainPass && DrawsVelocity();
	}

	return Result;
}

void FSceneProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	// Attach the light to the primitive's static meshes.
	const ELightInteractionType InteractionType = MeshInfo.GetInteraction(LightSceneProxy).GetType();
	bRelevant     = (InteractionType != LIT_CachedIrrelevant);
	bDynamic      = (InteractionType == LIT_Dynamic);
	bLightMapped  = (InteractionType == LIT_CachedLightMap || InteractionType == LIT_CachedIrrelevant);
	bShadowMapped = (InteractionType == LIT_CachedSignedDistanceFieldShadowMap2D);
}

#if WITH_EDITOR

HHitProxy* FSceneProxy::CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	LLM_SCOPE_BYTAG(Nanite);

	if (Component->GetOwner())
	{
		// Generate separate hit proxies for each material section, so that we can perform hit tests against each one.
		for (int32 SectionIndex = 0; SectionIndex < MaterialSections.Num(); ++SectionIndex)
		{
			FMaterialSection& Section = MaterialSections[SectionIndex];
			HHitProxy* ActorHitProxy = new HActor(Component->GetOwner(), Component, SectionIndex, SectionIndex);
			check(!Section.HitProxy);
			Section.HitProxy = ActorHitProxy;
			OutHitProxies.Add(ActorHitProxy);
		}
	}

	// We don't want a default hit proxy, or to output any hit proxies (avoid 2x registration).
	return nullptr;
}

#endif

FSceneProxy::FMeshInfo::FMeshInfo(const UStaticMeshComponent* InComponent)
{
	LLM_SCOPE_BYTAG(Nanite);

	if (InComponent->LightmapType == ELightmapType::ForceVolumetric)
	{
		SetGlobalVolumeLightmap(true);
	}
	else if (InComponent->LODData.Num() > 0)
	{
		const FStaticMeshComponentLODInfo& ComponentLODInfo = InComponent->LODData[0];

		const FMeshMapBuildData* MeshMapBuildData = InComponent->GetMeshMapBuildData(ComponentLODInfo);
		if (MeshMapBuildData)
		{
			SetLightMap(MeshMapBuildData->LightMap);
			SetShadowMap(MeshMapBuildData->ShadowMap);
			SetResourceCluster(MeshMapBuildData->ResourceCluster);
			IrrelevantLights = MeshMapBuildData->IrrelevantLights;
		}
	}
}

FLightInteraction FSceneProxy::FMeshInfo::GetInteraction(const FLightSceneProxy* LightSceneProxy) const
{
	// Ask base class
	ELightInteractionType LightInteraction = GetStaticInteraction(LightSceneProxy, IrrelevantLights);

	if (LightInteraction != LIT_MAX)
	{
		return FLightInteraction(LightInteraction);
	}

	// Use dynamic lighting if the light doesn't have static lighting.
	return FLightInteraction::Dynamic();
}

void FSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	const FLightCacheInterface* LCI = &MeshInfo;
	DrawStaticElementsInternal(PDI, LCI);
}

void FSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
#if !WITH_EDITOR
	if (GNaniteOptimizedRelevance != 0)
	{
		// No dynamic relevance.
		return;
	}
#endif

	LLM_SCOPE_BYTAG(Nanite);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_NaniteSceneProxy_GetMeshElements);
	checkSlow(IsInRenderingThread());

	const bool bIsLightmapSettingError = HasStaticLighting() && !HasValidSettingsForStaticLighting();
	const bool bProxyIsSelected = IsSelected();
	const FEngineShowFlags& EngineShowFlags = ViewFamily.EngineShowFlags;

	bool bDrawSimpleCollision = false, bDrawComplexCollision = false;
	const bool bInCollisionView = IsCollisionView(EngineShowFlags, bDrawSimpleCollision, bDrawComplexCollision);

#if NANITE_ENABLE_DEBUG_RENDERING
	// Collision and bounds drawing
	FColor SimpleCollisionColor = FColor(157, 149, 223, 255);
	FColor ComplexCollisionColor = FColor(0, 255, 255, 255);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			if (AllowDebugViewmodes())
			{
#if 0 // NANITE_TODO: Complex collision rendering
				// Should we draw the mesh wireframe to indicate we are using the mesh as collision
				bool bDrawComplexWireframeCollision = (EngineShowFlags.Collision && IsCollisionEnabled() && CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple);
				
				// Requested drawing complex in wireframe, but check that we are not using simple as complex
				bDrawComplexWireframeCollision |= (bDrawMeshCollisionIfComplex && CollisionTraceFlag != ECollisionTraceFlag::CTF_UseSimpleAsComplex);
				
				// Requested drawing simple in wireframe, and we are using complex as simple
				bDrawComplexWireframeCollision |= (bDrawMeshCollisionIfSimple && CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple);

				// If drawing complex collision as solid or wireframe
				if (bDrawComplexWireframeCollision || (bInCollisionView && bDrawComplexCollision))
				{
					// If we have at least one valid LOD to draw
					if (RenderData->LODResources.Num() > 0)
					{
						// Get LOD used for collision
						int32 DrawLOD = FMath::Clamp(LODForCollision, 0, RenderData->LODResources.Num() - 1);
						const FStaticMeshLODResources& LODModel = RenderData->LODResources[DrawLOD];

						UMaterial* MaterialToUse = UMaterial::GetDefaultMaterial(MD_Surface);
						FLinearColor DrawCollisionColor = GetWireframeColor();
						// Collision view modes draw collision mesh as solid
						if (bInCollisionView)
						{
							MaterialToUse = GEngine->ShadedLevelColorationUnlitMaterial;
						}
						// Wireframe, choose color based on complex or simple
						else
						{
							MaterialToUse = GEngine->WireframeMaterial;
							DrawCollisionColor = (CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple) ? SimpleCollisionColor : ComplexCollisionColor;
						}

						// Iterate over sections of that LOD
						for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
						{
							// If this section has collision enabled
							if (LODModel.Sections[SectionIndex].bEnableCollision)
							{
							#if WITH_EDITOR
								// See if we are selected
								const bool bSectionIsSelected = LODs[DrawLOD].Sections[SectionIndex].bSelected;
							#else
								const bool bSectionIsSelected = false;
							#endif

								// Create colored proxy
								FColoredMaterialRenderProxy* CollisionMaterialInstance = new FColoredMaterialRenderProxy(MaterialToUse->GetRenderProxy(), DrawCollisionColor);
								Collector.RegisterOneFrameMaterialProxy(CollisionMaterialInstance);

								// Iterate over batches
								for (int32 BatchIndex = 0; BatchIndex < GetNumMeshBatches(); BatchIndex++)
								{
									FMeshBatch& CollisionElement = Collector.AllocateMesh();
									if (GetCollisionMeshElement(DrawLOD, BatchIndex, SectionIndex, SDPG_World, CollisionMaterialInstance, CollisionElement))
									{
										Collector.AddMesh(ViewIndex, CollisionElement);
										INC_DWORD_STAT_BY(STAT_NaniteTriangles, CollisionElement.GetNumPrimitives());
									}
								}
							}
						}
					}
				}
#endif // NANITE_TODO
			}

			// Draw simple collision as wireframe if 'show collision', collision is enabled, and we are not using the complex as the simple
			// NANITE_TODO: const bool bDrawSimpleWireframeCollision = (EngineShowFlags.Collision && IsCollisionEnabled() && CollisionTraceFlag != ECollisionTraceFlag::CTF_UseComplexAsSimple); 
			const bool bDrawSimpleWireframeCollision = (EngineShowFlags.Collision && IsCollisionEnabled());

			if ((bDrawSimpleCollision || bDrawSimpleWireframeCollision) && BodySetup)
			{
				if (FMath::Abs(GetLocalToWorld().Determinant()) < SMALL_NUMBER)
				{
					// Catch this here or otherwise GeomTransform below will assert
					// This spams so commented out
					//UE_LOG(LogNanite, Log, TEXT("Zero scaling not supported (%s)"), *StaticMesh->GetPathName());
				}
				else
				{
					const bool bDrawSolid = !bDrawSimpleWireframeCollision;

					if (AllowDebugViewmodes() && bDrawSolid)
					{
						// Make a material for drawing solid collision stuff
						auto SolidMaterialInstance = new FColoredMaterialRenderProxy(
							GEngine->ShadedLevelColorationUnlitMaterial->GetRenderProxy(),
							GetWireframeColor()
							);

						Collector.RegisterOneFrameMaterialProxy(SolidMaterialInstance);

						FTransform GeomTransform(GetLocalToWorld());
						BodySetup->AggGeom.GetAggGeom(GeomTransform, GetWireframeColor().ToFColor(true), SolidMaterialInstance, false, true, DrawsVelocity(), ViewIndex, Collector);
					}
					// wireframe
					else
					{
						FTransform GeomTransform(GetLocalToWorld());
						BodySetup->AggGeom.GetAggGeom(GeomTransform, GetSelectionColor(SimpleCollisionColor, bProxyIsSelected, IsHovered()).ToFColor(true), nullptr, (Owner == nullptr), false, DrawsVelocity(), ViewIndex, Collector);
					}


					// The simple nav geometry is only used by dynamic obstacles for now
					if (StaticMesh->GetNavCollision() && StaticMesh->GetNavCollision()->IsDynamicObstacle())
					{
						// Draw the static mesh's body setup (simple collision)
						FTransform GeomTransform(GetLocalToWorld());
						FColor NavCollisionColor = FColor(118,84,255,255);
						StaticMesh->GetNavCollision()->DrawSimpleGeom(Collector.GetPDI(ViewIndex), GeomTransform, GetSelectionColor(NavCollisionColor, bProxyIsSelected, IsHovered()).ToFColor(true));
					}
				}
			}

			if (EngineShowFlags.MassProperties && DebugMassData.Num() > 0)
			{
				DebugMassData[0].DrawDebugMass(Collector.GetPDI(ViewIndex), FTransform(GetLocalToWorld()));
			}
	
			if (EngineShowFlags.StaticMeshes)
			{
				RenderBounds(Collector.GetPDI(ViewIndex), EngineShowFlags, GetBounds(), !Owner || IsSelected());
			}
		}
	}
#endif // NANITE_ENABLE_DEBUG_RENDERING
}

void FSceneProxy::OnTransformChanged()
{
#if RHI_RAYTRACING
	bCachedRayTracingInstanceTransformsValid = false;
#endif
}

#if RHI_RAYTRACING

int32 FSceneProxy::GetFirstValidRaytracingGeometryLODIndex() const
{
	int32 NumLODs = RenderData->LODResources.Num();
	int LODIndex = 0;

#if WITH_EDITOR
	// If coarse mesh streaming mode is set to 2 then we force use the lowest LOD to visualize streamed out coarse meshes
	if (Nanite::FCoarseMeshStreamingManager::GetStreamingMode() == 2)
	{
		LODIndex = NumLODs - 1;
	}
#endif // WITH_EDITOR

	// find the first valid RT geometry index
	for (; LODIndex < NumLODs; ++LODIndex)
	{
		if (RenderData->LODResources[LODIndex].RayTracingGeometry.Initializer.TotalPrimitiveCount > 0 &&
			RenderData->LODResources[LODIndex].RayTracingGeometry.RayTracingGeometryRHI != nullptr)
		{
			return LODIndex;
		}
	}

	return INDEX_NONE;
}

void FSceneProxy::SetupRayTracingMaterials(int32 LODIndex, TArray<FMeshBatch>& Materials) const
{
	check(Materials.Num() == MaterialSections.Num());
	for (int32 SectionIndex = 0; SectionIndex < MaterialSections.Num(); ++SectionIndex)
	{
		const FMaterialSection& MaterialSection = MaterialSections[SectionIndex];
		FMeshBatch& MeshBatch = Materials[SectionIndex];
		MeshBatch.VertexFactory = &RenderData->LODVertexFactories[LODIndex].VertexFactory;
		MeshBatch.MaterialRenderProxy = MaterialSection.Material->GetRenderProxy();
		MeshBatch.bWireframe = false;
		MeshBatch.SegmentIndex = SectionIndex;
		MeshBatch.LODIndex = 0;
	}
}

void FSceneProxy::GetDynamicRayTracingInstances(FRayTracingMaterialGatheringContext& Context, TArray<FRayTracingInstance>& OutRayTracingInstances)
{
	if (GRayTracingNaniteProxyMeshes == 0 || !bHasRayTracingInstances)
	{
		return;
	}

	// try and find the first valid RT geometry index
	int32 ValidLODIndex = GetFirstValidRaytracingGeometryLODIndex();
	if (ValidLODIndex == INDEX_NONE)
	{
		return;
	}

	// Setup a new instance
	FRayTracingInstance& RayTracingInstance = OutRayTracingInstances.Emplace_GetRef();
	RayTracingInstance.Geometry = &RenderData->LODResources[ValidLODIndex].RayTracingGeometry;;

	const int32 InstanceCount = InstanceSceneData.Num();
	if (CachedRayTracingInstanceTransforms.Num() != InstanceCount || !bCachedRayTracingInstanceTransformsValid)
	{
		const FRenderTransform PrimitiveToWorld = (FMatrix44f)GetLocalToWorld();

		CachedRayTracingInstanceTransforms.SetNumUninitialized(InstanceCount);
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
		{
			const FPrimitiveInstance& Instance = InstanceSceneData[InstanceIndex];
			const FRenderTransform InstanceLocalToWorld = Instance.ComputeLocalToWorld(PrimitiveToWorld);
			CachedRayTracingInstanceTransforms[InstanceIndex] = InstanceLocalToWorld.ToMatrix();
		}
		bCachedRayTracingInstanceTransformsValid = true;
	}

	// Transforms are persistently allocated, so we can just return them by pointer.
	RayTracingInstance.InstanceTransformsView = CachedRayTracingInstanceTransforms;
	RayTracingInstance.NumTransforms = CachedRayTracingInstanceTransforms.Num();

	// Setup the cached materials again when the LOD changes
	if (ValidLODIndex != CachedRayTracingMaterialsLODIndex)
	{
		SetupRayTracingMaterials(ValidLODIndex, CachedRayTracingMaterials);		
		CachedRayTracingMaterialsLODIndex = ValidLODIndex;

		// Request rebuild
		CachedRayTracingInstanceMaskAndFlags.Mask = 0;
	}
	RayTracingInstance.MaterialsView = CachedRayTracingMaterials;

	if (CachedRayTracingInstanceMaskAndFlags.Mask == 0)
	{
		CachedRayTracingInstanceMaskAndFlags = BuildRayTracingInstanceMaskAndFlags(CachedRayTracingMaterials, GetScene().GetFeatureLevel());
	}
	RayTracingInstance.Mask = CachedRayTracingInstanceMaskAndFlags.Mask;
	RayTracingInstance.bForceOpaque = CachedRayTracingInstanceMaskAndFlags.bForceOpaque;
	RayTracingInstance.bDoubleSided = CachedRayTracingInstanceMaskAndFlags.bDoubleSided;
}

ERayTracingPrimitiveFlags FSceneProxy::GetCachedRayTracingInstance(FRayTracingInstance& RayTracingInstance)
{
	const bool bShouldRender = (IsVisibleInRayTracing() && ShouldRenderInMainPass() && IsDrawnInGame()) || IsRayTracingFarField();
	if (GRayTracingNaniteProxyMeshes == 0 || !bHasRayTracingInstances || !bShouldRender)
	{
		return ERayTracingPrimitiveFlags::Excluded;
	}

	// try and find the first valid RT geometry index
	int32 ValidLODIndex = GetFirstValidRaytracingGeometryLODIndex();
	if (ValidLODIndex == INDEX_NONE)
	{
		// If there is a streaming handle (but no valid LOD available(, then give the streaming flag to make sure it's not excluded
		// It's still needs to be processed during TLAS build because this will drive the streaming of these resources.
		return (CoarseMeshStreamingHandle != INDEX_NONE) ? ERayTracingPrimitiveFlags::Streaming : ERayTracingPrimitiveFlags::Excluded;
	}

	RayTracingInstance.Geometry = &RenderData->LODResources[ValidLODIndex].RayTracingGeometry;

	const int32 InstanceCount = InstanceSceneData.Num();
	RayTracingInstance.InstanceTransforms.SetNumUninitialized(InstanceCount);
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
	{
		const FPrimitiveInstance& Instance = InstanceSceneData[InstanceIndex];
		// LocalToWorld multiplication will be done when added to FScene, and re-done when doing UpdatePrimitiveTransform
		RayTracingInstance.InstanceTransforms[InstanceIndex] = Instance.LocalToPrimitive.ToMatrix();
	}
	RayTracingInstance.NumTransforms = InstanceCount;

	RayTracingInstance.Materials.SetNum(MaterialSections.Num());
	SetupRayTracingMaterials(ValidLODIndex, RayTracingInstance.Materials);

	FRayTracingMaskAndFlags MaskAndFlags = BuildRayTracingInstanceMaskAndFlags(RayTracingInstance.Materials, GetScene().GetFeatureLevel());

	RayTracingInstance.Mask = MaskAndFlags.Mask;
	RayTracingInstance.bForceOpaque = MaskAndFlags.bForceOpaque;
	RayTracingInstance.bDoubleSided = MaskAndFlags.bDoubleSided;

	// setup the flags
	ERayTracingPrimitiveFlags ResultFlags = ERayTracingPrimitiveFlags::CacheMeshCommands | ERayTracingPrimitiveFlags::CacheInstances;
	if (CoarseMeshStreamingHandle != INDEX_NONE)
	{
		ResultFlags = ResultFlags | ERayTracingPrimitiveFlags::Streaming;
	}
	return ResultFlags;
}

#endif // RHI_RAYTRACING

const FCardRepresentationData* FSceneProxy::GetMeshCardRepresentation() const
{
	return CardRepresentationData;
}

void FSceneProxy::GetDistanceFieldAtlasData(const FDistanceFieldVolumeData*& OutDistanceFieldData, float& SelfShadowBias) const
{
	OutDistanceFieldData = DistanceFieldData;
	SelfShadowBias = DistanceFieldSelfShadowBias;
}

void FSceneProxy::GetDistanceFieldInstanceData(TArray<FRenderTransform>& ObjectLocalToWorldTransforms) const
{
	if (DistanceFieldData)
	{
		const FRenderTransform PrimitiveToWorld = (FMatrix44f)GetLocalToWorld();
		for (const FPrimitiveInstance& Instance : InstanceSceneData)
		{
			FRenderTransform& InstanceToWorld = ObjectLocalToWorldTransforms.Emplace_GetRef();
			InstanceToWorld = Instance.ComputeLocalToWorld(PrimitiveToWorld);
		}
	}
}

bool FSceneProxy::HasDistanceFieldRepresentation() const
{
	return CastsDynamicShadow() && AffectsDistanceFieldLighting() && DistanceFieldData;
}

int32 FSceneProxy::GetLightMapCoordinateIndex() const
{
	const int32 LightMapCoordinateIndex = StaticMesh != nullptr ? StaticMesh->GetLightMapCoordinateIndex() : INDEX_NONE;
	return LightMapCoordinateIndex;
}

bool FSceneProxy::IsCollisionView(const FEngineShowFlags& EngineShowFlags, bool& bDrawSimpleCollision, bool& bDrawComplexCollision) const
{
	bDrawSimpleCollision = bDrawComplexCollision = false;

	const bool bInCollisionView = EngineShowFlags.CollisionVisibility || EngineShowFlags.CollisionPawn;

#if NANITE_ENABLE_DEBUG_RENDERING
	// If in a 'collision view' and collision is enabled
	if (bInCollisionView && IsCollisionEnabled())
	{
		// See if we have a response to the interested channel
		bool bHasResponse = EngineShowFlags.CollisionPawn && CollisionResponse.GetResponse(ECC_Pawn) != ECR_Ignore;
		bHasResponse |= EngineShowFlags.CollisionVisibility && CollisionResponse.GetResponse(ECC_Visibility) != ECR_Ignore;

		if (bHasResponse)
		{
			// Visibility uses complex and pawn uses simple. However, if UseSimpleAsComplex or UseComplexAsSimple is used we need to adjust accordingly
			bDrawComplexCollision = (EngineShowFlags.CollisionVisibility && CollisionTraceFlag != ECollisionTraceFlag::CTF_UseSimpleAsComplex) || (EngineShowFlags.CollisionPawn && CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple);
			bDrawSimpleCollision = (EngineShowFlags.CollisionPawn && CollisionTraceFlag != ECollisionTraceFlag::CTF_UseComplexAsSimple) || (EngineShowFlags.CollisionVisibility && CollisionTraceFlag == ECollisionTraceFlag::CTF_UseSimpleAsComplex);
		}
	}
#endif
	return bInCollisionView;
}

uint32 FSceneProxy::GetMemoryFootprint() const
{
	return sizeof( *this ) + GetAllocatedSize();
}

void FGlobalResources::InitRHI()
{
	if (DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		LLM_SCOPE_BYTAG(Nanite);
		VertexFactory = new FVertexFactory(ERHIFeatureLevel::SM5);
		VertexFactory->InitResource();
	}
}

void FGlobalResources::ReleaseRHI()
{
	if (DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		LLM_SCOPE_BYTAG(Nanite);

		MainPassBuffers.StatsRasterizeArgsSWHWBuffer.SafeRelease();
		PostPassBuffers.StatsRasterizeArgsSWHWBuffer.SafeRelease();

		MainAndPostNodesAndClusterBatchesBuffer.SafeRelease();

		StatsBuffer.SafeRelease();

		StructureBufferStride8.SafeRelease();

		delete VertexFactory;
		VertexFactory = nullptr;
	}
}

void FGlobalResources::Update(FRDGBuilder& GraphBuilder)
{
	check(DoesPlatformSupportNanite(GMaxRHIShaderPlatform));

	if (!StructureBufferStride8.IsValid())
	{
		FRDGBufferDesc StructureBufferStride8Desc = FRDGBufferDesc::CreateStructuredDesc(8, 1);
		GetPooledFreeBuffer(GraphBuilder.RHICmdList, StructureBufferStride8Desc, StructureBufferStride8, TEXT("Nanite.StructureBufferStride8"));
		check(StructureBufferStride8.IsValid());
	}
}

uint32 FGlobalResources::GetMaxCandidateClusters()
{
	checkf(GNaniteMaxCandidateClusters <= MAX_CLUSTERS, TEXT("r.Nanite.MaxCandidateClusters must be <= MAX_CLUSTERS"));
	const uint32 MaxCandidateClusters = GNaniteMaxCandidateClusters & -PERSISTENT_CLUSTER_CULLING_GROUP_SIZE;
	return MaxCandidateClusters;
}

uint32 FGlobalResources::GetMaxClusterBatches()
{
	const uint32 MaxCandidateClusters = GetMaxCandidateClusters();
	check(MaxCandidateClusters % PERSISTENT_CLUSTER_CULLING_GROUP_SIZE == 0);
	return MaxCandidateClusters / PERSISTENT_CLUSTER_CULLING_GROUP_SIZE;
}

uint32 FGlobalResources::GetMaxVisibleClusters()
{
	checkf(GNaniteMaxVisibleClusters <= MAX_CLUSTERS, TEXT("r.Nanite.MaxVisibleClusters must be <= MAX_CLUSTERS"));
	return GNaniteMaxVisibleClusters;
}

uint32 FGlobalResources::GetMaxNodes()
{
	return GNaniteMaxNodes & -MAX_BVH_NODES_PER_GROUP;
}

TGlobalResource< FGlobalResources > GGlobalResources;

} // namespace Nanite
