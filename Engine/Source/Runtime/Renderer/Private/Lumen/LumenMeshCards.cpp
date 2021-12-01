// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenMeshCards.cpp
=============================================================================*/

#include "LumenMeshCards.h"
#include "RendererPrivate.h"
#include "MeshCardRepresentation.h"
#include "ComponentRecreateRenderStateContext.h"

float GLumenMeshCardsMinSize = 30.0f;
FAutoConsoleVariableRef CVarLumenMeshCardsMinSize(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMinSize"),
	GLumenMeshCardsMinSize,
	TEXT("Minimum mesh cards world space size to be included in Lumen Scene."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenMeshCardsMergeComponents = 1;
FAutoConsoleVariableRef CVarLumenMeshCardsMergeComponents(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergeComponents"),
	GLumenMeshCardsMergeComponents,
	TEXT("Whether to merge all components with the same RayTracingGroupId into a single MeshCards."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenMeshCardsMergeInstances = 1;
FAutoConsoleVariableRef CVarLumenMeshCardsMergeInstances(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergeInstances"),
	GLumenMeshCardsMergeInstances,
	TEXT("Whether to merge all instances of a Instanced Static Mesh Component into a single MeshCards."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenMeshCardsMergedCardMinSurfaceArea = 0.05f;
FAutoConsoleVariableRef CVarLumenMeshCardsMergedCardMinSurfaceArea(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergedCardMinSurfaceArea"),
	GLumenMeshCardsMergedCardMinSurfaceArea,
	TEXT("Minimum area to spawn a merged card."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenMeshCardsMaxLOD = 1;
FAutoConsoleVariableRef CVarLumenMeshCardsMaxLOD(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMaxLOD"),
	GLumenMeshCardsMaxLOD,
	TEXT("Max LOD level for the card representation. 0 - lowest quality."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
		{
			FGlobalComponentRecreateRenderStateContext Context;
		}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenMeshCardsMergeInstancesMaxSurfaceAreaRatio = 1.7f;
FAutoConsoleVariableRef CVarLumenMeshCardsMergeInstancesMaxSurfaceAreaRatio(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergeInstancesMaxSurfaceAreaRatio"),
	GLumenMeshCardsMergeInstancesMaxSurfaceAreaRatio,
	TEXT("Only merge if the (combined box surface area) / (summed instance box surface area) < MaxSurfaceAreaRatio"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenMeshCardsMergedResolutionScale = .3f;
FAutoConsoleVariableRef CVarLumenMeshCardsMergedResolutionScale(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergedResolutionScale"),
	GLumenMeshCardsMergedResolutionScale,
	TEXT("Scale on the resolution calculation for a merged MeshCards.  This compensates for the merged box getting a higher resolution assigned due to being closer to the viewer."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenMeshCardsMergedMaxWorldSize = 10000.0f;
FAutoConsoleVariableRef CVarLumenMeshCardsMergedMaxWorldSize(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsMergedMaxWorldSize"),
	GLumenMeshCardsMergedMaxWorldSize,
	TEXT("Only merged bounds less than this size on any axis are considered, since Lumen Scene streaming relies on object granularity."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenMeshCardsCullFaces = 1;
FAutoConsoleVariableRef CVarLumenMeshCardsCullFaces(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsCullFaces"),
	GLumenMeshCardsCullFaces,
	TEXT(""),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenMeshCardsDebugSingleCard = -1;
FAutoConsoleVariableRef CVarLumenMeshCardsDebugSingleCard(
	TEXT("r.LumenScene.SurfaceCache.MeshCardsDebugSingleCard"),
	GLumenMeshCardsDebugSingleCard,
	TEXT("Spawn only a specified card on mesh. Useful for debugging."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_RenderThreadSafe
);

extern int32 GLumenSceneUploadEveryFrame;

namespace LumenMeshCards
{
	FVector3f GetAxisAlignedDirection(uint32 AxisAlignedDirectionIndex);
};

FVector3f LumenMeshCards::GetAxisAlignedDirection(uint32 AxisAlignedDirectionIndex)
{
	const uint32 AxisIndex = AxisAlignedDirectionIndex / 2;

	FVector3f Direction(0.0f, 0.0f, 0.0f);
	Direction[AxisIndex] = AxisAlignedDirectionIndex & 1 ? 1.0f : -1.0f;
	return Direction;
}

class FLumenCardGPUData
{
public:
	// Must match usf
	enum { DataStrideInFloat4s = 7 };
	enum { DataStrideInBytes = DataStrideInFloat4s * sizeof(FVector4f) };

	static void PackSurfaceMipMap(const FLumenCard& Card, int32 ResLevel, uint32& PackedSizeInPages, uint32& PackedPageTableOffset)
	{
		PackedSizeInPages = 0;
		PackedPageTableOffset = 0;

		if (Card.IsAllocated())
		{
			const FLumenSurfaceMipMap& MipMap = Card.GetMipMap(ResLevel);

			if (MipMap.IsAllocated())
			{
				PackedSizeInPages = MipMap.SizeInPagesX | (MipMap.SizeInPagesY << 16);
				PackedPageTableOffset = MipMap.PageTableSpanOffset;
			}
		}
	}

	static void FillData(const class FLumenCard& RESTRICT Card, FVector4f* RESTRICT OutData)
	{
		// Note: layout must match GetLumenCardData in usf

		OutData[0] = FVector4f(Card.WorldOBB.AxisX[0], Card.WorldOBB.AxisY[0], Card.WorldOBB.AxisZ[0], Card.WorldOBB.Origin.X);
		OutData[1] = FVector4f(Card.WorldOBB.AxisX[1], Card.WorldOBB.AxisY[1], Card.WorldOBB.AxisZ[1], Card.WorldOBB.Origin.Y);
		OutData[2] = FVector4f(Card.WorldOBB.AxisX[2], Card.WorldOBB.AxisY[2], Card.WorldOBB.AxisZ[2], Card.WorldOBB.Origin.Z);

		const FIntPoint ResLevelBias = Card.ResLevelToResLevelXYBias();
		uint32 Packed3W = 0;
		Packed3W = uint8(ResLevelBias.X) & 0xFF;
		Packed3W |= (uint8(ResLevelBias.Y) & 0xFF) << 8;
		Packed3W |= Card.bVisible && Card.IsAllocated() ? (1 << 16) : 0;

		OutData[3] = FVector4f(Card.WorldOBB.Extent.X, Card.WorldOBB.Extent.Y, Card.WorldOBB.Extent.Z, 0.0f);
		OutData[3].W = *((float*)&Packed3W);

		// Map low-res level for diffuse
		uint32 PackedSizeInPages = 0;
		uint32 PackedPageTableOffset = 0;
		PackSurfaceMipMap(Card, Card.MinAllocatedResLevel, PackedSizeInPages, PackedPageTableOffset);

		// Map hi-res for specular
		uint32 PackedHiResSizeInPages = 0;
		uint32 PackedHiResPageTableOffset = 0;
		PackSurfaceMipMap(Card, Card.MaxAllocatedResLevel, PackedHiResSizeInPages, PackedHiResPageTableOffset);

		OutData[4].X = *((float*)&PackedSizeInPages);
		OutData[4].Y = *((float*)&PackedPageTableOffset);
		OutData[4].Z = *((float*)&PackedHiResSizeInPages);
		OutData[4].W = *((float*)&PackedHiResPageTableOffset);

		FVector3f MeshCardsBoundsCenter = Card.LocalOBB.Origin;
		FVector3f MeshCardsBoundsExtent = Card.LocalOBB.RotateCardToLocal(Card.LocalOBB.Extent).GetAbs();

		OutData[5] = FVector4f(MeshCardsBoundsCenter.X, MeshCardsBoundsCenter.Y, MeshCardsBoundsCenter.Z, 0.0f);
		OutData[6] = FVector4f(MeshCardsBoundsExtent.X, MeshCardsBoundsExtent.Y, MeshCardsBoundsExtent.Z, 0.0f);

		static_assert(DataStrideInFloat4s == 7, "Data stride doesn't match");
	}
};

struct FLumenMeshCardsGPUData
{
	// Must match LUMEN_MESH_CARDS_DATA_STRIDE in LumenCardCommon.ush
	enum { DataStrideInFloat4s = 8 };
	enum { DataStrideInBytes = DataStrideInFloat4s * 16 };

	static void FillData(const class FLumenMeshCards& RESTRICT MeshCards, FVector4f* RESTRICT OutData);
};

void FLumenMeshCardsGPUData::FillData(const FLumenMeshCards& RESTRICT MeshCards, FVector4f* RESTRICT OutData)
{
	// Note: layout must match GetLumenMeshCardsData in usf

	const FMatrix44f WorldToLocal = MeshCards.LocalToWorld.Inverse();
	const FMatrix44f TransposedLocalToWorld = MeshCards.LocalToWorld.GetTransposed();
	const FMatrix44f TransposedWorldToLocal = WorldToLocal.GetTransposed();

	OutData[0] = *(FVector4f*)&TransposedLocalToWorld.M[0];
	OutData[1] = *(FVector4f*)&TransposedLocalToWorld.M[1];
	OutData[2] = *(FVector4f*)&TransposedLocalToWorld.M[2];

	OutData[3] = *(FVector4f*)&TransposedWorldToLocal.M[0];
	OutData[4] = *(FVector4f*)&TransposedWorldToLocal.M[1];
	OutData[5] = *(FVector4f*)&TransposedWorldToLocal.M[2];

	uint32 PackedData[4];
	PackedData[0] = MeshCards.FirstCardIndex;
	PackedData[1] = MeshCards.NumCards;
	PackedData[2] = MeshCards.CardLookup[0];
	PackedData[3] = MeshCards.CardLookup[1];
	OutData[6] = *(FVector4f*)&PackedData;

	PackedData[0] = MeshCards.CardLookup[2];
	PackedData[1] = MeshCards.CardLookup[3];
	PackedData[2] = MeshCards.CardLookup[4];
	PackedData[3] = MeshCards.CardLookup[5];
	OutData[7] = *(FVector4f*)&PackedData;

	static_assert(DataStrideInFloat4s == 8, "Data stride doesn't match");
}

void Lumen::UpdateCardSceneBuffer(FRHICommandListImmediate& RHICmdList, const FSceneViewFamily& ViewFamily, FScene* Scene)
{
	LLM_SCOPE_BYTAG(Lumen);

	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCardSceneBuffer);
	QUICK_SCOPE_CYCLE_COUNTER(UpdateCardSceneBuffer);
	SCOPED_DRAW_EVENT(RHICmdList, UpdateCardSceneBuffer);

	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;

	// CardBuffer
	{
		bool bResourceResized = false;
		{
			const int32 NumCardEntries = LumenSceneData.Cards.Num();
			const uint32 CardSceneNumFloat4s = NumCardEntries * FLumenCardGPUData::DataStrideInFloat4s;
			const uint32 CardSceneNumBytes = FMath::DivideAndRoundUp(CardSceneNumFloat4s, 16384u) * 16384 * sizeof(FVector4f);
			bResourceResized = ResizeResourceIfNeeded(RHICmdList, LumenSceneData.CardBuffer, FMath::RoundUpToPowerOfTwo(CardSceneNumFloat4s) * sizeof(FVector4f), TEXT("Lumen.Cards"));
		}

		if (GLumenSceneUploadEveryFrame)
		{
			LumenSceneData.CardIndicesToUpdateInBuffer.Reset();

			for (int32 i = 0; i < LumenSceneData.Cards.Num(); i++)
			{
				LumenSceneData.CardIndicesToUpdateInBuffer.Add(i);
			}
		}

		const int32 NumCardDataUploads = LumenSceneData.CardIndicesToUpdateInBuffer.Num();

		if (NumCardDataUploads > 0)
		{
			FLumenCard NullCard;

			LumenSceneData.UploadBuffer.Init(NumCardDataUploads, FLumenCardGPUData::DataStrideInBytes, true, TEXT("Lumen.UploadBuffer"));

			for (int32 Index : LumenSceneData.CardIndicesToUpdateInBuffer)
			{
				if (Index < LumenSceneData.Cards.Num())
				{
					const FLumenCard& Card = LumenSceneData.Cards.IsAllocated(Index) ? LumenSceneData.Cards[Index] : NullCard;

					FVector4f* Data = (FVector4f*)LumenSceneData.UploadBuffer.Add_GetRef(Index);
					FLumenCardGPUData::FillData(Card, Data);
				}
			}

			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.CardBuffer.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
			LumenSceneData.UploadBuffer.ResourceUploadTo(RHICmdList, LumenSceneData.CardBuffer, false);
			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.CardBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		}
		else if (bResourceResized)
		{
			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.CardBuffer.UAV, ERHIAccess::UAVCompute | ERHIAccess::UAVGraphics, ERHIAccess::SRVMask));
		}
	}

	UpdateLumenMeshCards(*Scene, Scene->DistanceFieldSceneData, LumenSceneData, RHICmdList);

	const uint32 MaxUploadBufferSize = 64 * 1024;
	if (LumenSceneData.UploadBuffer.GetNumBytes() > MaxUploadBufferSize)
	{
		LumenSceneData.UploadBuffer.Release();
	}
}

int32 FLumenSceneData::GetMeshCardsIndex(const FPrimitiveSceneInfo* PrimitiveSceneInfo, int32 InstanceIndex) const
{
	if (PrimitiveSceneInfo->LumenPrimitiveGroupIndices.Num() > 0)
	{
		const int32 IndexInArray = FMath::Min(InstanceIndex, PrimitiveSceneInfo->LumenPrimitiveGroupIndices.Num() - 1);
		const int32 PrimitiveGroupIndex = PrimitiveSceneInfo->LumenPrimitiveGroupIndices[IndexInArray];
		const FLumenPrimitiveGroup& PrimitiveGroup = PrimitiveGroups[PrimitiveGroupIndex];

		return PrimitiveGroup.MeshCardsIndex;
	}

	return -1;
}

void UpdateLumenMeshCards(FScene& Scene, const FDistanceFieldSceneData& DistanceFieldSceneData, FLumenSceneData& LumenSceneData, FRHICommandListImmediate& RHICmdList)
{
	LLM_SCOPE_BYTAG(Lumen);
	QUICK_SCOPE_CYCLE_COUNTER(UpdateLumenMeshCards);

	extern int32 GLumenSceneUploadEveryFrame;
	if (GLumenSceneUploadEveryFrame)
	{
		LumenSceneData.MeshCardsIndicesToUpdateInBuffer.Reset();

		for (int32 i = 0; i < LumenSceneData.MeshCards.Num(); i++)
		{
			LumenSceneData.MeshCardsIndicesToUpdateInBuffer.Add(i);
		}
	}

	// Upload MeshCards
	{
		QUICK_SCOPE_CYCLE_COUNTER(UpdateMeshCards);

		const uint32 NumMeshCards = LumenSceneData.MeshCards.Num();
		const uint32 MeshCardsNumFloat4s = FMath::RoundUpToPowerOfTwo(NumMeshCards * FLumenMeshCardsGPUData::DataStrideInFloat4s);
		const uint32 MeshCardsNumBytes = MeshCardsNumFloat4s * sizeof(FVector4f);
		const bool bResourceResized = ResizeResourceIfNeeded(RHICmdList, LumenSceneData.MeshCardsBuffer, MeshCardsNumBytes, TEXT("Lumen.MeshCards"));

		const int32 NumMeshCardsUploads = LumenSceneData.MeshCardsIndicesToUpdateInBuffer.Num();

		if (NumMeshCardsUploads > 0)
		{
			FLumenMeshCards NullMeshCards;

			LumenSceneData.UploadBuffer.Init(NumMeshCardsUploads, FLumenMeshCardsGPUData::DataStrideInBytes, true, TEXT("Lumen.UploadBuffer"));

			for (int32 Index : LumenSceneData.MeshCardsIndicesToUpdateInBuffer)
			{
				if (Index < LumenSceneData.MeshCards.Num())
				{
					const FLumenMeshCards& MeshCards = LumenSceneData.MeshCards.IsAllocated(Index) ? LumenSceneData.MeshCards[Index] : NullMeshCards;

					FVector4f* Data = (FVector4f*) LumenSceneData.UploadBuffer.Add_GetRef(Index);
					FLumenMeshCardsGPUData::FillData(MeshCards, Data);
				}
			}

			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.MeshCardsBuffer.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
			LumenSceneData.UploadBuffer.ResourceUploadTo(RHICmdList, LumenSceneData.MeshCardsBuffer, false);
			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.MeshCardsBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		}
		else if (bResourceResized)
		{
			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.MeshCardsBuffer.UAV, ERHIAccess::UAVCompute | ERHIAccess::UAVGraphics, ERHIAccess::SRVMask));
		}
	}

	// Upload MeshCards
	{
		QUICK_SCOPE_CYCLE_COUNTER(UpdateSceneInstanceIndexToMeshCardsIndexBuffer);

		if (GLumenSceneUploadEveryFrame)
		{
			LumenSceneData.PrimitivesToUpdateMeshCards.Reset();

			for (int32 PrimitiveIndex = 0; PrimitiveIndex < Scene.Primitives.Num(); ++PrimitiveIndex)
			{
				LumenSceneData.PrimitivesToUpdateMeshCards.Add(PrimitiveIndex);
			}
		}

		const int32 NumIndices = FMath::Max(FMath::RoundUpToPowerOfTwo(Scene.GPUScene.InstanceSceneDataAllocator.GetMaxSize()), 1024u);
		const uint32 IndexSizeInBytes = GPixelFormats[PF_R32_UINT].BlockBytes;
		const uint32 IndicesSizeInBytes = NumIndices * IndexSizeInBytes;
		ResizeResourceIfNeeded(RHICmdList, LumenSceneData.SceneInstanceIndexToMeshCardsIndexBuffer, IndicesSizeInBytes, TEXT("SceneInstanceIndexToMeshCardsIndexBuffer"));

		uint32 NumIndexUploads = 0;

		for (int32 PrimitiveIndex : LumenSceneData.PrimitivesToUpdateMeshCards)
		{
			if (PrimitiveIndex < Scene.Primitives.Num())
			{
				const FPrimitiveSceneInfo* PrimitiveSceneInfo = Scene.Primitives[PrimitiveIndex];
				NumIndexUploads += PrimitiveSceneInfo->GetNumInstanceSceneDataEntries();
			}
		}

		if (NumIndexUploads > 0)
		{
			LumenSceneData.ByteBufferUploadBuffer.Init(NumIndexUploads, IndexSizeInBytes, false, TEXT("LumenUploadBuffer"));

			for (int32 PrimitiveIndex : LumenSceneData.PrimitivesToUpdateMeshCards)
			{
				if (PrimitiveIndex < Scene.Primitives.Num())
				{
					const FPrimitiveSceneInfo* PrimitiveSceneInfo = Scene.Primitives[PrimitiveIndex];
					const int32 NumInstances = PrimitiveSceneInfo->GetNumInstanceSceneDataEntries();

					for (int32 InstanceIndex = 0; InstanceIndex < NumInstances; ++InstanceIndex)
					{
						const int32 MeshCardsIndex = LumenSceneData.GetMeshCardsIndex(PrimitiveSceneInfo, InstanceIndex);

						LumenSceneData.ByteBufferUploadBuffer.Add(PrimitiveSceneInfo->GetInstanceSceneDataOffset() + InstanceIndex, &MeshCardsIndex);
					}
				}
			}

			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.SceneInstanceIndexToMeshCardsIndexBuffer.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
			LumenSceneData.ByteBufferUploadBuffer.ResourceUploadTo(RHICmdList, LumenSceneData.SceneInstanceIndexToMeshCardsIndexBuffer, false);
			RHICmdList.Transition(FRHITransitionInfo(LumenSceneData.SceneInstanceIndexToMeshCardsIndexBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		}
	}

	// Reset arrays, but keep allocated memory for 1024 elements
	LumenSceneData.MeshCardsIndicesToUpdateInBuffer.Empty(1024);
	LumenSceneData.PrimitivesToUpdateMeshCards.Empty(1024);
}

class FLumenMergedMeshCards
{
public:
	FLumenMergedMeshCards()
	{
		MergedBounds.Init();

		for (int32 AxisAlignedDirectionIndex = 0; AxisAlignedDirectionIndex < Lumen::NumAxisAlignedDirections; ++AxisAlignedDirectionIndex)
		{
			InstanceCardAreaPerDirection[AxisAlignedDirectionIndex] = 0;
		}
	}

	void AddInstance(FBox InstanceBox, FMatrix InstanceToMerged, const FMeshCardsBuildData& MeshCardsBuildData)
	{
		MergedBounds += InstanceBox.TransformBy(InstanceToMerged);

		const int32 LODLevel = FMath::Clamp(GLumenMeshCardsMaxLOD, 0, MeshCardsBuildData.MaxLODLevel);

		for (const FLumenCardBuildData& CardBuildData : MeshCardsBuildData.CardBuildData)
		{
			if (CardBuildData.LODLevel == LODLevel)
			{
				const FVector3f AxisX = FVector4f(InstanceToMerged.TransformVector(CardBuildData.OBB.AxisX));
				const FVector3f AxisY = FVector4f(InstanceToMerged.TransformVector(CardBuildData.OBB.AxisY));
				const FVector3f AxisZ = FVector4f(InstanceToMerged.TransformVector(CardBuildData.OBB.AxisZ));
				const FVector3f Extent = CardBuildData.OBB.Extent * FVector3f(AxisX.Length(), AxisY.Length(), AxisZ.Length());

				const float InstanceCardArea = Extent.X * Extent.Y;
				const FVector3f CardDirection = AxisZ.GetUnsafeNormal();

				for (int32 AxisAlignedDirectionIndex = 0; AxisAlignedDirectionIndex < Lumen::NumAxisAlignedDirections; ++AxisAlignedDirectionIndex)
				{
					const FVector3f AxisDirection = LumenMeshCards::GetAxisAlignedDirection(AxisAlignedDirectionIndex);
					const float AxisProjection = CardDirection.Dot(AxisDirection);

					if (AxisProjection > 0.0f)
					{
						InstanceCardAreaPerDirection[AxisAlignedDirectionIndex] += AxisProjection * InstanceCardArea;
					}
				}
			}
		}
	}

	FBox MergedBounds;
	float InstanceCardAreaPerDirection[Lumen::NumAxisAlignedDirections];
};

void BuildMeshCardsDataForMergedInstances(const FLumenPrimitiveGroup& PrimitiveGroup, FMeshCardsBuildData& MeshCardsBuildData, FMatrix& MeshCardsLocalToWorld)
{
	MeshCardsLocalToWorld.SetIdentity();

	// Pick first largest bbox as a reference frame
	float LargestInstanceArea = -1.0f;
	for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : PrimitiveGroup.Primitives)
	{
		const FMatrix& PrimitiveToWorld = PrimitiveSceneInfo->Proxy->GetLocalToWorld();
		const TConstArrayView<FPrimitiveInstance> InstanceSceneData = PrimitiveSceneInfo->Proxy->GetInstanceSceneData();

		float InstanceArea = BoxSurfaceArea(PrimitiveSceneInfo->Proxy->GetBounds().BoxExtent);
		FMatrix InstanceMeshCardsLocalToWorld = PrimitiveToWorld;

		for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
		{
			const FPrimitiveInstance& Instance = InstanceSceneData[InstanceIndex];
			InstanceArea = BoxSurfaceArea(Instance.LocalBounds.GetExtent());
			InstanceMeshCardsLocalToWorld = Instance.LocalToPrimitive.ToMatrix() * PrimitiveToWorld;
		}

		if (InstanceArea > LargestInstanceArea)
		{
			MeshCardsLocalToWorld = InstanceMeshCardsLocalToWorld;
			LargestInstanceArea = InstanceArea;
		}
	}

	const FMatrix WorldToMeshCardsLocal = MeshCardsLocalToWorld.Inverse();

	MeshCardsBuildData.MaxLODLevel = 0;
	MeshCardsBuildData.Bounds.Init();

	FLumenMergedMeshCards MergedMeshCards;

	for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : PrimitiveGroup.Primitives)
	{
		const FCardRepresentationData* CardRepresentationData = PrimitiveSceneInfo->Proxy->GetMeshCardRepresentation();

		if (CardRepresentationData)
		{
			const FMatrix& PrimitiveToWorld = PrimitiveSceneInfo->Proxy->GetLocalToWorld();
			const TConstArrayView<FPrimitiveInstance> InstanceSceneData = PrimitiveSceneInfo->Proxy->GetInstanceSceneData();
			const FMeshCardsBuildData& PrimitiveMeshCardsBuildData = CardRepresentationData->MeshCardsBuildData;
			const FMatrix PrimitiveLocalToMeshCardsLocal = PrimitiveToWorld * WorldToMeshCardsLocal;

			if (InstanceSceneData.Num() > 0)
			{
				for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
				{
					const FPrimitiveInstance& Instance = InstanceSceneData[InstanceIndex];
					MergedMeshCards.AddInstance(
						Instance.LocalBounds.ToBox(),
						Instance.LocalToPrimitive.ToMatrix() * PrimitiveLocalToMeshCardsLocal,
						PrimitiveMeshCardsBuildData);
				}
			}
			else
			{
				MergedMeshCards.AddInstance(
					PrimitiveSceneInfo->Proxy->GetLocalBounds().GetBox(),
					PrimitiveLocalToMeshCardsLocal,
					PrimitiveMeshCardsBuildData);
			}
		}
	}

	// Spawn cards only on faces passing min area threshold
	TArray<int32, TInlineAllocator<Lumen::NumAxisAlignedDirections>> AxisAlignedDirectionsToSpawnCards;
	for (int32 AxisAlignedDirectionIndex = 0; AxisAlignedDirectionIndex < Lumen::NumAxisAlignedDirections; ++AxisAlignedDirectionIndex)
	{
		FVector3f MergedExtent = MergedMeshCards.MergedBounds.GetExtent();
		MergedExtent[AxisAlignedDirectionIndex / 2] = 1.0f;
		const float MergedFaceArea = MergedExtent.X * MergedExtent.Y * MergedExtent.Z;

		if (MergedMeshCards.InstanceCardAreaPerDirection[AxisAlignedDirectionIndex] > GLumenMeshCardsMergedCardMinSurfaceArea * MergedFaceArea)
		{
			AxisAlignedDirectionsToSpawnCards.Add(AxisAlignedDirectionIndex);
		}
	}

	if (MergedMeshCards.MergedBounds.IsValid && AxisAlignedDirectionsToSpawnCards.Num() > 0)
	{
		// Make sure BBox isn't empty and we can generate card representation for it. This handles e.g. infinitely thin planes.
		const FVector SafeCenter = MergedMeshCards.MergedBounds.GetCenter();
		const FVector SafeExtent = FVector::Max(MergedMeshCards.MergedBounds.GetExtent() + 1.0f, FVector(5.0f));
		const FBox SafeMergedBounds = FBox(SafeCenter - SafeExtent, SafeCenter + SafeExtent);

		MeshCardsBuildData.MaxLODLevel = 0;
		MeshCardsBuildData.Bounds = SafeMergedBounds;

		MeshCardsBuildData.CardBuildData.SetNum(AxisAlignedDirectionsToSpawnCards.Num());
		uint32 CardBuildDataIndex = 0;

		for (int32 AxisAlignedDirectionIndex : AxisAlignedDirectionsToSpawnCards)
		{
			FLumenCardBuildData& CardBuildData = MeshCardsBuildData.CardBuildData[CardBuildDataIndex];
			++CardBuildDataIndex;

			// Set rotation
			CardBuildData.OBB.AxisZ = LumenMeshCards::GetAxisAlignedDirection(AxisAlignedDirectionIndex);
			CardBuildData.OBB.AxisZ.FindBestAxisVectors(CardBuildData.OBB.AxisX, CardBuildData.OBB.AxisY);
			CardBuildData.OBB.AxisX = FVector::CrossProduct(CardBuildData.OBB.AxisZ, CardBuildData.OBB.AxisY);
			CardBuildData.OBB.AxisX.Normalize();

			CardBuildData.OBB.Origin = SafeMergedBounds.GetCenter();
			CardBuildData.OBB.Extent = CardBuildData.OBB.RotateLocalToCard(SafeMergedBounds.GetExtent() + FVector(1.0f)).GetAbs();

			CardBuildData.AxisAlignedDirectionIndex = AxisAlignedDirectionIndex;
			CardBuildData.LODLevel = 0;
		}
	}
}

void FLumenSceneData::AddMeshCards(int32 PrimitiveGroupIndex)
{
	FLumenPrimitiveGroup& PrimitiveGroup = PrimitiveGroups[PrimitiveGroupIndex];

	if (PrimitiveGroup.MeshCardsIndex < 0)
	{
		const bool bMergeInstances = PrimitiveGroup.HasMergedInstances();

		if (bMergeInstances)
		{
			FMatrix LocalToWorld;
			FMeshCardsBuildData MeshCardsBuildData;
			BuildMeshCardsDataForMergedInstances(PrimitiveGroup, MeshCardsBuildData, LocalToWorld);

			PrimitiveGroup.MeshCardsIndex = AddMeshCardsFromBuildData(PrimitiveGroupIndex, LocalToWorld, MeshCardsBuildData, PrimitiveGroup.CardResolutionScale);
		}
		else
		{
			ensure(PrimitiveGroup.Primitives.Num() == 1);
			const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveGroup.Primitives[0];

			FMatrix LocalToWorld = PrimitiveSceneInfo->Proxy->GetLocalToWorld();
			const TConstArrayView<FPrimitiveInstance> InstanceSceneData = PrimitiveSceneInfo->Proxy->GetInstanceSceneData();

			if (InstanceSceneData.Num() > 0)
			{
				const int32 PrimitiveInstanceIndex = FMath::Clamp(PrimitiveGroup.PrimitiveInstanceIndex, 0, InstanceSceneData.Num() - 1);
				LocalToWorld = InstanceSceneData[PrimitiveInstanceIndex].LocalToPrimitive.ToMatrix() * LocalToWorld;
			}

			const FCardRepresentationData* CardRepresentationData = PrimitiveSceneInfo->Proxy->GetMeshCardRepresentation();
			if (CardRepresentationData)
			{
				const FMeshCardsBuildData& MeshCardsBuildData = CardRepresentationData->MeshCardsBuildData;
				PrimitiveGroup.MeshCardsIndex = AddMeshCardsFromBuildData(PrimitiveGroupIndex, LocalToWorld, MeshCardsBuildData, PrimitiveGroup.CardResolutionScale);
			}
		}

		// Update surface cache mapping
		for (const FPrimitiveSceneInfo* ScenePrimitive : PrimitiveGroup.Primitives)
		{
			PrimitivesToUpdateMeshCards.Add(ScenePrimitive->GetIndex());
		}

		if (PrimitiveGroup.MeshCardsIndex < 0)
		{
			PrimitiveGroup.bValidMeshCards = false;
		}
	}
}

bool IsMatrixOrthogonal(const FMatrix& Matrix)
{
	const FVector MatrixScale = Matrix.GetScaleVector();

	if (MatrixScale.GetAbsMin() >= KINDA_SMALL_NUMBER)
	{
		FVector AxisX;
		FVector AxisY;
		FVector AxisZ;
		Matrix.GetUnitAxes(AxisX, AxisY, AxisZ);

		return FMath::Abs(AxisX | AxisY) < KINDA_SMALL_NUMBER
			&& FMath::Abs(AxisX | AxisZ) < KINDA_SMALL_NUMBER
			&& FMath::Abs(AxisY | AxisZ) < KINDA_SMALL_NUMBER;
	}

	return false;
}

bool MeshCardCullTest(const FLumenCardBuildData& CardBuildData, const FVector3f LocalToWorldScale, const int32 LODLevel, float MinFaceSurfaceArea, int32 CardIndex)
{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	if (GLumenMeshCardsDebugSingleCard >= 0)
	{
		return GLumenMeshCardsDebugSingleCard == CardIndex;
	}
#endif

	const FVector3f ScaledBoundsSize = 2.0f * CardBuildData.OBB.Extent * LocalToWorldScale;
	const float SurfaceArea = ScaledBoundsSize.X * ScaledBoundsSize.Y;
	const bool bCardPassedCulling = (!GLumenMeshCardsCullFaces || SurfaceArea > MinFaceSurfaceArea);
	const bool bCardPassedLODTest = CardBuildData.LODLevel == LODLevel;

	return bCardPassedCulling && bCardPassedLODTest;
}

int32 FLumenSceneData::AddMeshCardsFromBuildData(int32 PrimitiveGroupIndex, const FMatrix& LocalToWorld, const FMeshCardsBuildData& MeshCardsBuildData, float ResolutionScale)
{
	const FLumenPrimitiveGroup& PrimitiveGroup = PrimitiveGroups[PrimitiveGroupIndex];

	const FVector3f LocalToWorldScale = LocalToWorld.GetScaleVector();
	const FVector3f ScaledBoundSize = MeshCardsBuildData.Bounds.GetSize() * LocalToWorldScale;
	const FVector3f FaceSurfaceArea(ScaledBoundSize.Y * ScaledBoundSize.Z, ScaledBoundSize.X * ScaledBoundSize.Z, ScaledBoundSize.Y * ScaledBoundSize.X);
	const float LargestFaceArea = FaceSurfaceArea.GetMax();
	const float MinFaceSurfaceArea = GLumenMeshCardsMinSize * GLumenMeshCardsMinSize;
	const int32 LODLevel = FMath::Clamp(GLumenMeshCardsMaxLOD, 0, MeshCardsBuildData.MaxLODLevel);

	if (LargestFaceArea > MinFaceSurfaceArea
		&& IsMatrixOrthogonal(LocalToWorld)) // #lumen_todo: implement card capture for non orthogonal local to world transforms
	{
		const int32 NumBuildDataCards = MeshCardsBuildData.CardBuildData.Num();

		uint32 NumCards = 0;

		for (int32 CardIndexInBuildData = 0; CardIndexInBuildData < NumBuildDataCards; ++CardIndexInBuildData)
		{
			const FLumenCardBuildData& CardBuildData = MeshCardsBuildData.CardBuildData[CardIndexInBuildData];

			if (MeshCardCullTest(CardBuildData, LocalToWorldScale, LODLevel, MinFaceSurfaceArea, CardIndexInBuildData))
			{
				++NumCards;
			}
		}

		if (NumCards > 0)
		{
			const int32 FirstCardIndex = Cards.AddSpan(NumCards);

			const int32 MeshCardsIndex = MeshCards.AddSpan(1);
			MeshCards[MeshCardsIndex].Initialize(
				LocalToWorld,
				MeshCardsBuildData.Bounds,
				PrimitiveGroupIndex,
				FirstCardIndex,
				NumCards);

			MeshCardsIndicesToUpdateInBuffer.Add(MeshCardsIndex);

			// Add cards
			int32 LocalCardIndex = 0;
			for (int32 CardIndexInBuildData = 0; CardIndexInBuildData < NumBuildDataCards; ++CardIndexInBuildData)
			{	
				const FLumenCardBuildData& CardBuildData = MeshCardsBuildData.CardBuildData[CardIndexInBuildData];

				if (MeshCardCullTest(CardBuildData, LocalToWorldScale, LODLevel, MinFaceSurfaceArea, CardIndexInBuildData))
				{
					const int32 CardInsertIndex = FirstCardIndex + LocalCardIndex;

					Cards[CardInsertIndex].Initialize(ResolutionScale, LocalToWorld, CardBuildData, LocalCardIndex, MeshCardsIndex, CardIndexInBuildData);
					CardIndicesToUpdateInBuffer.Add(CardInsertIndex);

					++LocalCardIndex;
				}
			}

			MeshCards[MeshCardsIndex].UpdateLookup(Cards);

			return MeshCardsIndex;
		}
	}

	return -1;
}

void FLumenSceneData::RemoveMeshCards(FLumenPrimitiveGroup& PrimitiveGroup)
{
	if (PrimitiveGroup.MeshCardsIndex >= 0)
	{
		const FLumenMeshCards& MeshCardsInstance = MeshCards[PrimitiveGroup.MeshCardsIndex];

		for (uint32 CardIndex = MeshCardsInstance.FirstCardIndex; CardIndex < MeshCardsInstance.FirstCardIndex + MeshCardsInstance.NumCards; ++CardIndex)
		{
			RemoveCardFromAtlas(CardIndex);
		}

		Cards.RemoveSpan(MeshCardsInstance.FirstCardIndex, MeshCardsInstance.NumCards);
		MeshCards.RemoveSpan(PrimitiveGroup.MeshCardsIndex, 1);

		MeshCardsIndicesToUpdateInBuffer.Add(PrimitiveGroup.MeshCardsIndex);

		PrimitiveGroup.MeshCardsIndex = -1;

		// Update surface cache mapping
		for (const FPrimitiveSceneInfo* ScenePrimitive : PrimitiveGroup.Primitives)
		{
			PrimitivesToUpdateMeshCards.Add(ScenePrimitive->GetIndex());
		}
	}
}

void FLumenSceneData::UpdateMeshCards(const FMatrix& LocalToWorld, int32 MeshCardsIndex, const FMeshCardsBuildData& MeshCardsBuildData)
{
	if (MeshCardsIndex >= 0 && IsMatrixOrthogonal(LocalToWorld))
	{
		FLumenMeshCards& MeshCardsInstance = MeshCards[MeshCardsIndex];
		MeshCardsInstance.SetTransform(LocalToWorld);
		MeshCardsIndicesToUpdateInBuffer.Add(MeshCardsIndex);

		for (uint32 LocalCardIndex = 0; LocalCardIndex < MeshCardsInstance.NumCards; ++LocalCardIndex)
		{
			const uint32 CardIndex = MeshCardsInstance.FirstCardIndex + LocalCardIndex;
			FLumenCard& Card = Cards[CardIndex];

			Card.SetTransform(LocalToWorld, Card.LocalOBB);

			CardIndicesToUpdateInBuffer.Add(CardIndex);
		}
	}
}

void FLumenSceneData::RemoveCardFromAtlas(int32 CardIndex)
{
	FLumenCard& Card = Cards[CardIndex];
	Card.DesiredLockedResLevel = 0;
	FreeVirtualSurface(Card, Card.MinAllocatedResLevel, Card.MaxAllocatedResLevel);
	CardIndicesToUpdateInBuffer.Add(CardIndex);
}

void FLumenMeshCards::UpdateLookup(const TSparseSpanArray<FLumenCard>& Cards)
{
	for (int32 AxisAlignedDirectionIndex = 0; AxisAlignedDirectionIndex < Lumen::NumAxisAlignedDirections; ++AxisAlignedDirectionIndex)
	{
		CardLookup[AxisAlignedDirectionIndex] = 0;
	}

	for (uint32 LocalCardIndex = 0; LocalCardIndex < NumCards; ++LocalCardIndex)
	{
		const uint32 CardIndex = FirstCardIndex + LocalCardIndex;
		const FLumenCard& Card = Cards[CardIndex];
		
		const uint32 BitMask = (1 << LocalCardIndex);
		CardLookup[Card.AxisAlignedDirectionIndex] |= BitMask;
	}
}