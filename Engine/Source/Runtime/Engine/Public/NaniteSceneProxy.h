// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "Rendering/NaniteResources.h"
#include "RayTracingInstance.h"

namespace Nanite
{

class FSceneProxyBase : public FPrimitiveSceneProxy
{
public:
	struct FMaterialSection
	{
		UMaterialInterface* Material = nullptr;
	#if WITH_EDITOR
		HHitProxy* HitProxy = nullptr;
	#endif
		int32 MaterialIndex = INDEX_NONE;
		uint8 bHasAnyError : 1;
		uint8 bHasNullMaterial : 1;
		uint8 bHasInvalidRelevance : 1;
		uint8 bHasInvalidStaticLighting : 1;
		uint8 bHasNonOpaqueBlendMode : 1;
		uint8 bHasVertexInterpolator : 1;
		uint8 bHasPerInstanceRandomID : 1;
		uint8 bHasPerInstanceCustomData : 1;
	};

public:
	ENGINE_API SIZE_T GetTypeHash() const override;

	ENGINE_API FSceneProxyBase(UPrimitiveComponent* Component)
	: FPrimitiveSceneProxy(Component)
	{
		bIsNaniteMesh  = true;
		bAlwaysVisible = true;
	}

	ENGINE_API virtual ~FSceneProxyBase() = default;

	static bool IsNaniteRenderable(FMaterialRelevance MaterialRelevance)
	{
		return MaterialRelevance.bOpaque &&
			!MaterialRelevance.bDecal &&
			!MaterialRelevance.bMasked &&
			!MaterialRelevance.bNormalTranslucency &&
			!MaterialRelevance.bSeparateTranslucency &&
			!MaterialRelevance.bPostMotionBlurTranslucency;
	}

	virtual bool CanBeOccluded() const override
	{
		// Disable slow occlusion paths(Nanite does its own occlusion culling)
		return false;
	}

	inline const TArray<FMaterialSection>& GetMaterialSections() const
	{
		return MaterialSections;
	}

	inline int32 GetMaterialMaxIndex() const
	{
		return MaterialMaxIndex;
	}

	inline void GetMaterialDynamicDataUsage(bool& bOutCustomData, bool& bOutRandomID) const
	{
		bOutCustomData	= false;
		bOutRandomID	= false;

		// Checks if any material assigned to the mesh uses custom data and/or random ID
		for (const FMaterialSection& MaterialSection : MaterialSections)
		{
			bOutCustomData	|= MaterialSection.bHasPerInstanceCustomData;
			bOutRandomID	|= MaterialSection.bHasPerInstanceRandomID;

			if (bOutCustomData && bOutRandomID)
			{
				break;
			}
		}
	}

	// Nanite always uses LOD 0, and performs custom LOD streaming.
	virtual uint8 GetCurrentFirstLODIdx_RenderThread() const override { return 0; }

protected:
	ENGINE_API void DrawStaticElementsInternal(FStaticPrimitiveDrawInterface* PDI, const FLightCacheInterface* LCI);

protected:
	TArray<FMaterialSection> MaterialSections;
	int32 MaterialMaxIndex = INDEX_NONE;
};

class FSceneProxy : public FSceneProxyBase
{
public:
	FSceneProxy(UStaticMeshComponent* Component);
	FSceneProxy(UInstancedStaticMeshComponent* Component);
	FSceneProxy(UHierarchicalInstancedStaticMeshComponent* Component);

	virtual ~FSceneProxy();

public:
	// FPrimitiveSceneProxy interface.
	virtual FPrimitiveViewRelevance	GetViewRelevance(const FSceneView* View) const override;
	virtual void GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const override;

#if WITH_EDITOR
	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy> >& OutHitProxies) override;
#endif
	virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

#if RHI_RAYTRACING
	virtual bool IsRayTracingRelevant() const { return true; }
	virtual bool IsRayTracingStaticRelevant() const { return true; }
	virtual void GetDynamicRayTracingInstances(FRayTracingMaterialGatheringContext& Context, TArray<struct FRayTracingInstance>& OutRayTracingInstances) override;
	virtual ERayTracingPrimitiveFlags GetCachedRayTracingInstance(FRayTracingInstance& RayTracingInstance) override;
	virtual Nanite::CoarseMeshStreamingHandle GetCoarseMeshStreamingHandle() const override { return CoarseMeshStreamingHandle; }
#endif

	virtual uint32 GetMemoryFootprint() const override;

	virtual void GetLCIs(FLCIArray& LCIs) override
	{
		FLightCacheInterface* LCI = &MeshInfo;
		LCIs.Add(LCI);
	}

	virtual void GetDistanceFieldAtlasData(const FDistanceFieldVolumeData*& OutDistanceFieldData, float& SelfShadowBias) const override;
	virtual void GetDistanceFieldInstanceData(TArray<FRenderTransform>& ObjectLocalToWorldTransforms) const override;
	virtual bool HasDistanceFieldRepresentation() const override;

	virtual const FCardRepresentationData* GetMeshCardRepresentation() const override;

	virtual int32 GetLightMapCoordinateIndex() const override;

	virtual void OnTransformChanged() override;

	virtual void GetNaniteResourceInfo(uint32& ResourceID, uint32& HierarchyOffset, bool& bHasImposterData) const override
	{
		ResourceID = Resources->RuntimeResourceID;
		HierarchyOffset = Resources->HierarchyOffset;
		bHasImposterData = Resources->ImposterAtlas.Num() > 0;
	}

	const UStaticMesh* GetStaticMesh() const
	{
		return StaticMesh;
	}

protected:
	virtual void CreateRenderThreadResources() override;

	class FMeshInfo : public FLightCacheInterface
	{
	public:
		FMeshInfo(const UStaticMeshComponent* InComponent);

		// FLightCacheInterface.
		virtual FLightInteraction GetInteraction(const FLightSceneProxy* LightSceneProxy) const override;

	private:
		TArray<FGuid> IrrelevantLights;
	};

	bool IsCollisionView(const FEngineShowFlags& EngineShowFlags, bool& bDrawSimpleCollision, bool& bDrawComplexCollision) const;

#if RHI_RAYTRACING
	int32 GetFirstValidRaytracingGeometryLODIndex() const;
	void SetupRayTracingMaterials(int32 LODIndex, TArray<FMeshBatch>& Materials) const;
#endif // RHI_RAYTRACING

protected:
	FMeshInfo MeshInfo;

	FResources* Resources = nullptr;

	const FStaticMeshRenderData* RenderData;
	const FDistanceFieldVolumeData* DistanceFieldData;
	const FCardRepresentationData* CardRepresentationData;

	// TODO: Should probably calculate this on the materials array above instead of on the component
	//       Null and !Opaque are assigned default material unlike the component material relevance.
	FMaterialRelevance MaterialRelevance;

	uint32 bReverseCulling : 1;
	uint32 bHasMaterialErrors : 1;

	const UStaticMesh* StaticMesh = nullptr;

#if RHI_RAYTRACING
	bool bHasRayTracingInstances = false;
	bool bCachedRayTracingInstanceTransformsValid = false;
	Nanite::CoarseMeshStreamingHandle CoarseMeshStreamingHandle = INDEX_NONE;
	int16 CachedRayTracingMaterialsLODIndex = INDEX_NONE;
	TArray<FMatrix> CachedRayTracingInstanceTransforms;
	TArray<FMeshBatch> CachedRayTracingMaterials;	
	FRayTracingMaskAndFlags CachedRayTracingInstanceMaskAndFlags;
#endif

#if NANITE_ENABLE_DEBUG_RENDERING
	AActor* Owner;

	/** LightMap resolution used for VMI_LightmapDensity */
	int32 LightMapResolution;

	/** Body setup for collision debug rendering */
	UBodySetup* BodySetup;

	/** Collision trace flags */
	ECollisionTraceFlag CollisionTraceFlag;

	/** Collision Response of this component */
	FCollisionResponseContainer CollisionResponse;

	/** LOD used for collision */
	int32 LODForCollision;

	/** Draw mesh collision if used for complex collision */
	uint32 bDrawMeshCollisionIfComplex : 1;

	/** Draw mesh collision if used for simple collision */
	uint32 bDrawMeshCollisionIfSimple : 1;
#endif
};

} // namespace Nanite

