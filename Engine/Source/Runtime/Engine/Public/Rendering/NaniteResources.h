// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"
#include "GrowOnlySpanAllocator.h"
#include "UnifiedBuffer.h"
#include "RenderGraphDefinitions.h"
#include "SceneManagement.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/BulkData.h"
#include "Misc/MemoryReadStream.h"

/** Whether Nanite::FSceneProxy should store data and enable codepaths needed for debug rendering. */
#if PLATFORM_WINDOWS
#define NANITE_ENABLE_DEBUG_RENDERING (!(UE_BUILD_SHIPPING || UE_BUILD_TEST) || WITH_EDITOR)
#else
#define NANITE_ENABLE_DEBUG_RENDERING 0
#endif

#define MAX_STREAMING_REQUESTS					( 128u * 1024u )										// must match define in NaniteDataDecode.ush
#define MAX_CLUSTER_TRIANGLES					128
#define MAX_CLUSTER_VERTICES_BITS				8														// must match define in NaniteDataDecode.ush
#define MAX_CLUSTER_VERTICES_MASK				( ( 1 << MAX_CLUSTER_VERTICES_BITS ) - 1 )
#define MAX_CLUSTER_VERTICES					( 1 << MAX_CLUSTER_VERTICES_BITS )						// must match define in NaniteDataDecode.ush
#define MAX_CLUSTER_INDICES						( MAX_CLUSTER_TRIANGLES * 3 )
#define MAX_NANITE_UVS							4														// must match define in NaniteDataDecode.ush
#define NUM_ROOT_PAGES							1u														// Should probably be made a per-resource option

#define USE_STRIP_INDICES						1														// must match define in NaniteDataDecode.ush

#define ROOT_PAGE_GPU_SIZE_BITS					15														// must match define in NaniteDataDecode.ush
#define ROOT_PAGE_GPU_SIZE						( 1u << ROOT_PAGE_GPU_SIZE_BITS )						// must match define in NaniteDataDecode.ush
#define STREAMING_PAGE_GPU_SIZE_BITS			17														// must match define in NaniteDataDecode.ush
#define STREAMING_PAGE_GPU_SIZE					( 1u << STREAMING_PAGE_GPU_SIZE_BITS )					// must match define in NaniteDataDecode.ush
#define MAX_PAGE_DISK_SIZE						( STREAMING_PAGE_GPU_SIZE * 2 )							// must match define in NaniteDataDecode.ush

#define MAX_CLUSTERS_PER_PAGE_BITS				10														// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_PAGE_MASK				( ( 1 << MAX_CLUSTERS_PER_PAGE_BITS ) - 1 )				// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_PAGE					( 1 << MAX_CLUSTERS_PER_PAGE_BITS )						// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_GROUP_BITS				9														// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_GROUP_MASK				( ( 1 << MAX_CLUSTERS_PER_GROUP_BITS ) - 1 )			// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_GROUP					( ( 1 << MAX_CLUSTERS_PER_GROUP_BITS ) - 1 )			// must match define in NaniteDataDecode.ush
#define MAX_CLUSTERS_PER_GROUP_TARGET			128														// what we are targeting. MAX_CLUSTERS_PER_GROUP needs to be large 
																										// enough that it won't overflow after constraint-based splitting
#define MAX_HIERACHY_CHILDREN_BITS				6														// must match define in NaniteDataDecode.ush
#define MAX_HIERACHY_CHILDREN					( 1 << MAX_HIERACHY_CHILDREN_BITS )						// must match define in NaniteDataDecode.ush
#define MAX_GPU_PAGES_BITS						14														// must match define in NaniteDataDecode.ush
#define	MAX_GPU_PAGES							( 1 << MAX_GPU_PAGES_BITS )								// must match define in NaniteDataDecode.ush
#define MAX_INSTANCES_BITS						24														// must match define in NaniteDataDecode.ush
#define MAX_INSTANCES							( 1 << MAX_INSTANCES_BITS )								// must match define in NaniteDataDecode.ush
#define MAX_NODES_PER_PRIMITIVE_BITS			16														// must match define in NaniteDataDecode.ush
#define MAX_RESOURCE_PAGES_BITS					20
#define MAX_RESOURCE_PAGES						(1 << MAX_RESOURCE_PAGES_BITS)
#define MAX_GROUP_PARTS_BITS					3
#define MAX_GROUP_PARTS_MASK					((1 << MAX_GROUP_PARTS_BITS) - 1)
#define MAX_GROUP_PARTS							(1 << MAX_GROUP_PARTS_BITS)

#define PERSISTENT_CLUSTER_CULLING_GROUP_SIZE	64														// must match define in Culling.ush

#define MAX_BVH_NODE_FANOUT_BITS				2														// must match define in Culling.ush
#define MAX_BVH_NODE_FANOUT						(1 << MAX_BVH_NODE_FANOUT_BITS)							// must match define in Culling.ush

#define MAX_BVH_NODES_PER_GROUP					(PERSISTENT_CLUSTER_CULLING_GROUP_SIZE / MAX_BVH_NODE_FANOUT)

#define NUM_CULLING_FLAG_BITS					3														// must match define in NaniteDataDecode.ush

#define NUM_PACKED_CLUSTER_FLOAT4S				6														// must match define in NaniteDataDecode.ush
#define GPU_PAGE_HEADER_SIZE					16														// must match define in NaniteDataDecode.ush

#define MAX_POSITION_QUANTIZATION_BITS			21		// (21*3 = 63) < 64								// must match define in NaniteDataDecode.ush
#define MIN_POSITION_PRECISION					-8														// must match define in NaniteDataDecode.ush
#define MAX_POSITION_PRECISION					 23														// must match define in NaniteDataDecode.ush

#define NORMAL_QUANTIZATION_BITS				 9

#define MAX_TEXCOORD_QUANTIZATION_BITS			15														// must match define in NaniteDataDecode.ush
#define MAX_COLOR_QUANTIZATION_BITS				 8														// must match define in NaniteDataDecode.ush

#define NUM_STREAMING_PRIORITY_CATEGORY_BITS	 2														// must match define in NaniteDataDecode.ush
#define STREAMING_PRIORITY_CATEGORY_MASK		((1u << NUM_STREAMING_PRIORITY_CATEGORY_BITS) - 1u)		// must match define in NaniteDataDecode.ush

#define VIEW_FLAG_HZBTEST						0x1														// must match define in NaniteDataDecode.ush

#define MAX_TRANSCODE_GROUPS_PER_PAGE			128														// must match define in NaniteDataDecode.ush

#define VERTEX_COLOR_MODE_WHITE					0
#define VERTEX_COLOR_MODE_CONSTANT				1
#define VERTEX_COLOR_MODE_VARIABLE				2

#define NANITE_CLUSTER_FLAG_LEAF				0x1

#define NANITE_PAGE_FLAG_RELATIVE_ENCODING		0x1

DECLARE_STATS_GROUP( TEXT("Nanite"), STATGROUP_Nanite, STATCAT_Advanced );

DECLARE_GPU_STAT_NAMED_EXTERN(NaniteStreaming, TEXT("Nanite Streaming"));
DECLARE_GPU_STAT_NAMED_EXTERN(NaniteReadback, TEXT("Nanite Readback"));

LLM_DECLARE_TAG_API(Nanite, ENGINE_API);

class UStaticMesh;
class UBodySetup;
class FDistanceFieldVolumeData;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class FVertexFactory;

namespace Nanite
{

struct FUIntVector
{
	uint32 X, Y, Z;

	bool operator==(const FUIntVector& V) const
	{
		return X == V.X && Y == V.Y && Z == V.Z;
	}

	FORCEINLINE friend FArchive& operator<<(FArchive& Ar, FUIntVector& V)
	{
		Ar << V.X;
		Ar << V.Y;
		Ar << V.Z;
		return Ar;
	}
};

struct FPackedHierarchyNode
{
	FVector4f		LODBounds[MAX_BVH_NODE_FANOUT];
	
	struct
	{
		FVector3f	BoxBoundsCenter;
		uint32		MinLODError_MaxParentLODError;
	} Misc0[MAX_BVH_NODE_FANOUT];

	struct
	{
		FVector3f	BoxBoundsExtent;
		uint32		ChildStartReference;
	} Misc1[MAX_BVH_NODE_FANOUT];
	
	struct
	{
		uint32		ResourcePageIndex_NumPages_GroupPartSize;
	} Misc2[MAX_BVH_NODE_FANOUT];
};

struct FMaterialTriangle
{
	uint32 Index0;
	uint32 Index1;
	uint32 Index2;
	uint32 MaterialIndex;
	uint32 RangeCount;
};

FORCEINLINE uint32 GetBits(uint32 Value, uint32 NumBits, uint32 Offset)
{
	uint32 Mask = (1u << NumBits) - 1u;
	return (Value >> Offset) & Mask;
}

FORCEINLINE void SetBits(uint32& Value, uint32 Bits, uint32 NumBits, uint32 Offset)
{
	uint32 Mask = (1u << NumBits) - 1u;
	check(Bits <= Mask);
	Mask <<= Offset;
	Value = (Value & ~Mask) | (Bits << Offset);
}


// Packed Cluster as it is used by the GPU
struct FPackedCluster
{
	// Members needed for rasterization
	uint32		NumVerts_PositionOffset;					// NumVerts:9, PositionOffset:23
	uint32		NumTris_IndexOffset;						// NumTris:8, IndexOffset: 24
	uint32		ColorMin;
	uint32		ColorBits_GroupIndex;						// R:4, G:4, B:4, A:4. (GroupIndex&0xFFFF) is for debug visualization only.

	FIntVector	PosStart;
	uint32		BitsPerIndex_PosPrecision_PosBits;			// BitsPerIndex:4, PosPrecision: 5, PosBits:5.5.5
	
	// Members needed for culling
	FVector4f	LODBounds;									// LWC_TODO: Was FSphere, but that's now twice as big and won't work on GPU.

	FVector3f	BoxBoundsCenter;
	uint32		LODErrorAndEdgeLength;
	
	FVector3f	BoxBoundsExtent;
	uint32		Flags;

	// Members needed by materials
	uint32		AttributeOffset_BitsPerAttribute;			// AttributeOffset: 22, BitsPerAttribute: 10
	uint32		DecodeInfoOffset_NumUVs_ColorMode;			// DecodeInfoOffset: 22, NumUVs: 3, ColorMode: 2
	uint32		UV_Prec;									// U0:4, V0:4, U1:4, V1:4, U2:4, V2:4, U3:4, V3:4
	uint32		PackedMaterialInfo;

	uint32		GetNumVerts() const						{ return GetBits(NumVerts_PositionOffset, 9, 0); }
	uint32		GetPositionOffset() const				{ return GetBits(NumVerts_PositionOffset, 23, 9); }

	uint32		GetNumTris() const						{ return GetBits(NumTris_IndexOffset, 8, 0); }
	uint32		GetIndexOffset() const					{ return GetBits(NumTris_IndexOffset, 24, 8); }

	uint32		GetBitsPerIndex() const					{ return GetBits(BitsPerIndex_PosPrecision_PosBits, 4, 0); }
	int32		GetPosPrecision() const					{ return (int32)GetBits(BitsPerIndex_PosPrecision_PosBits, 5, 4) + MIN_POSITION_PRECISION; }
	uint32		GetPosBitsX() const						{ return GetBits(BitsPerIndex_PosPrecision_PosBits, 5, 9); }
	uint32		GetPosBitsY() const						{ return GetBits(BitsPerIndex_PosPrecision_PosBits, 5, 14); }
	uint32		GetPosBitsZ() const						{ return GetBits(BitsPerIndex_PosPrecision_PosBits, 5, 19); }

	uint32		GetAttributeOffset() const				{ return GetBits(AttributeOffset_BitsPerAttribute, 22, 0); }
	uint32		GetBitsPerAttribute() const				{ return GetBits(AttributeOffset_BitsPerAttribute, 10, 22); }
	
	void		SetNumVerts(uint32 NumVerts)			{ SetBits(NumVerts_PositionOffset, NumVerts, 9, 0); }
	void		SetPositionOffset(uint32 Offset)		{ SetBits(NumVerts_PositionOffset, Offset, 23, 9); }

	void		SetNumTris(uint32 NumTris)				{ SetBits(NumTris_IndexOffset, NumTris, 8, 0); }
	void		SetIndexOffset(uint32 Offset)			{ SetBits(NumTris_IndexOffset, Offset, 24, 8); }

	void		SetBitsPerIndex(uint32 BitsPerIndex)	{ SetBits(BitsPerIndex_PosPrecision_PosBits, BitsPerIndex, 4, 0); }
	void		SetPosPrecision(int32 Precision)		{ SetBits(BitsPerIndex_PosPrecision_PosBits, uint32(Precision - MIN_POSITION_PRECISION), 5, 4); }
	void		SetPosBitsX(uint32 NumBits)				{ SetBits(BitsPerIndex_PosPrecision_PosBits, NumBits, 5, 9); }
	void		SetPosBitsY(uint32 NumBits)				{ SetBits(BitsPerIndex_PosPrecision_PosBits, NumBits, 5, 14); }
	void		SetPosBitsZ(uint32 NumBits)				{ SetBits(BitsPerIndex_PosPrecision_PosBits, NumBits, 5, 19); }

	void		SetAttributeOffset(uint32 Offset)		{ SetBits(AttributeOffset_BitsPerAttribute, Offset, 22, 0); }
	void		SetBitsPerAttribute(uint32 Bits)		{ SetBits(AttributeOffset_BitsPerAttribute, Bits, 10, 22); }

	void		SetDecodeInfoOffset(uint32 Offset)		{ SetBits(DecodeInfoOffset_NumUVs_ColorMode, Offset, 22, 0); }
	void		SetNumUVs(uint32 Num)					{ SetBits(DecodeInfoOffset_NumUVs_ColorMode, Num, 3, 22); }
	void		SetColorMode(uint32 Mode)				{ SetBits(DecodeInfoOffset_NumUVs_ColorMode, Mode, 2, 22+3); }

	void		SetColorBitsR(uint32 NumBits)			{ SetBits(ColorBits_GroupIndex, NumBits, 4, 0); }
	void		SetColorBitsG(uint32 NumBits)			{ SetBits(ColorBits_GroupIndex, NumBits, 4, 4); }
	void		SetColorBitsB(uint32 NumBits)			{ SetBits(ColorBits_GroupIndex, NumBits, 4, 8); }
	void		SetColorBitsA(uint32 NumBits)			{ SetBits(ColorBits_GroupIndex, NumBits, 4, 12); }

	void		SetGroupIndex(uint32 GroupIndex)		{ SetBits(ColorBits_GroupIndex, GroupIndex & 0xFFFFu, 16, 16); }
};

struct FPageGPUHeader
{
	uint32 NumClusters = 0;
	uint32 Pad[3] = {0};
};

struct FPageStreamingState
{
	uint32			BulkOffset;
	uint32			BulkSize;
	uint32			PageSize;
	uint32			DependenciesStart;
	uint32			DependenciesNum;
	uint32			Flags;
};

class FHierarchyFixup
{
public:
	FHierarchyFixup() {}

	FHierarchyFixup( uint32 InPageIndex, uint32 NodeIndex, uint32 ChildIndex, uint32 InClusterGroupPartStartIndex, uint32 PageDependencyStart, uint32 PageDependencyNum )
	{
		check(InPageIndex < MAX_RESOURCE_PAGES);
		PageIndex = InPageIndex;

		check( NodeIndex < ( 1 << ( 32 - MAX_HIERACHY_CHILDREN_BITS ) ) );
		check( ChildIndex < MAX_HIERACHY_CHILDREN );
		check( InClusterGroupPartStartIndex < ( 1 << ( 32 - MAX_CLUSTERS_PER_GROUP_BITS ) ) );
		HierarchyNodeAndChildIndex = ( NodeIndex << MAX_HIERACHY_CHILDREN_BITS ) | ChildIndex;
		ClusterGroupPartStartIndex = InClusterGroupPartStartIndex;

		check(PageDependencyStart < MAX_RESOURCE_PAGES);
		check(PageDependencyNum <= MAX_GROUP_PARTS_MASK);
		PageDependencyStartAndNum = (PageDependencyStart << MAX_GROUP_PARTS_BITS) | PageDependencyNum;
	}

	uint32 GetPageIndex() const						{ return PageIndex; }
	uint32 GetNodeIndex() const						{ return HierarchyNodeAndChildIndex >> MAX_HIERACHY_CHILDREN_BITS; }
	uint32 GetChildIndex() const					{ return HierarchyNodeAndChildIndex & ( MAX_HIERACHY_CHILDREN - 1 ); }
	uint32 GetClusterGroupPartStartIndex() const	{ return ClusterGroupPartStartIndex; }
	uint32 GetPageDependencyStart() const			{ return PageDependencyStartAndNum >> MAX_GROUP_PARTS_BITS; }
	uint32 GetPageDependencyNum() const				{ return PageDependencyStartAndNum & MAX_GROUP_PARTS_MASK; }

	uint32 PageIndex;
	uint32 HierarchyNodeAndChildIndex;
	uint32 ClusterGroupPartStartIndex;
	uint32 PageDependencyStartAndNum;
};

class FClusterFixup
{
public:
	FClusterFixup() {}

	FClusterFixup( uint32 PageIndex, uint32 ClusterIndex, uint32 PageDependencyStart, uint32 PageDependencyNum )
	{
		check( PageIndex < ( 1 << ( 32 - MAX_CLUSTERS_PER_GROUP_BITS ) ) );
		check( ClusterIndex < MAX_CLUSTERS_PER_PAGE );
		PageAndClusterIndex = ( PageIndex << MAX_CLUSTERS_PER_PAGE_BITS ) | ClusterIndex;

		check(PageDependencyStart < MAX_RESOURCE_PAGES);
		check(PageDependencyNum <= MAX_GROUP_PARTS_MASK);
		PageDependencyStartAndNum = (PageDependencyStart << MAX_GROUP_PARTS_BITS) | PageDependencyNum;
	}
	
	uint32 GetPageIndex() const				{ return PageAndClusterIndex >> MAX_CLUSTERS_PER_PAGE_BITS; }
	uint32 GetClusterIndex() const			{ return PageAndClusterIndex & (MAX_CLUSTERS_PER_PAGE - 1u); }
	uint32 GetPageDependencyStart() const	{ return PageDependencyStartAndNum >> MAX_GROUP_PARTS_BITS; }
	uint32 GetPageDependencyNum() const		{ return PageDependencyStartAndNum & MAX_GROUP_PARTS_MASK; }

	uint32 PageAndClusterIndex;
	uint32 PageDependencyStartAndNum;
};

struct FPageDiskHeader
{
	uint32 GpuSize;
	uint32 NumClusters;
	uint32 NumRawFloat4s;
	uint32 NumTexCoords;
	uint32 NumVertexRefs;
	uint32 DecodeInfoOffset;
	uint32 StripBitmaskOffset;
	uint32 VertexRefBitmaskOffset;
};

struct FClusterDiskHeader
{
	uint32 IndexDataOffset;
	uint32 PageClusterMapOffset;
	uint32 VertexRefDataOffset;
	uint32 PositionDataOffset;
	uint32 AttributeDataOffset;
	uint32 NumVertexRefs;
	uint32 NumPrevRefVerticesBeforeDwords;
	uint32 NumPrevNewVerticesBeforeDwords;
};

class FFixupChunk	//TODO: rename to something else
{
public:
	struct FHeader
	{
		uint16 NumClusters = 0;
		uint16 NumHierachyFixups = 0;
		uint16 NumClusterFixups = 0;
		uint16 Pad = 0;
	} Header;
	
	uint8 Data[ sizeof(FHierarchyFixup) * MAX_CLUSTERS_PER_PAGE + sizeof( FClusterFixup ) * MAX_CLUSTERS_PER_PAGE ];	// One hierarchy fixup per cluster and at most one cluster fixup per cluster.

	FClusterFixup&		GetClusterFixup( uint32 Index ) const { check( Index < Header.NumClusterFixups );  return ( (FClusterFixup*)( Data + Header.NumHierachyFixups * sizeof( FHierarchyFixup ) ) )[ Index ]; }
	FHierarchyFixup&	GetHierarchyFixup( uint32 Index ) const { check( Index < Header.NumHierachyFixups ); return ((FHierarchyFixup*)Data)[ Index ]; }
	uint32				GetSize() const { return sizeof( Header ) + Header.NumHierachyFixups * sizeof( FHierarchyFixup ) + Header.NumClusterFixups * sizeof( FClusterFixup ); }
};

struct FInstanceDraw
{
	uint32 InstanceId;
	uint32 ViewId;
};

#define NANITE_RESOURCE_FLAG_HAS_VERTEX_COLOR 0x1
#define NANITE_RESOURCE_FLAG_HAS_IMPOSTER 0x2
#define NANITE_RESOURCE_FLAG_HAS_LZ_COMPRESSION 0x4

struct FResources
{
	// Persistent State
	TArray< uint8 >					RootClusterPage;		// Root page is loaded on resource load, so we always have something to draw.
	FByteBulkData					StreamableClusterPages;	// Remaining pages are streamed on demand.
	TArray< uint16 >				ImposterAtlas;
	TArray< FPackedHierarchyNode >	HierarchyNodes;
	TArray< uint32 >				HierarchyRootOffsets;
	TArray< FPageStreamingState >	PageStreamingStates;
	TArray< uint32 >				PageDependencies;
	int32							PositionPrecision	= 0;
	uint32							NumInputTriangles	= 0;
	uint32							NumInputVertices	= 0;
	uint16							NumInputMeshes		= 0;
	uint16							NumInputTexCoords	= 0;
	uint32							ResourceFlags		= 0;

	// Runtime State
	uint32	RuntimeResourceID		= 0xFFFFFFFFu;
	uint32	HierarchyOffset			= 0xFFFFFFFFu;
	int32	RootPageIndex			= INDEX_NONE;
	uint32	NumHierarchyNodes		= 0;

	ENGINE_API void InitResources();
	ENGINE_API bool ReleaseResources();

	ENGINE_API void Serialize(FArchive& Ar, UObject* Owner);

	void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) const;
};

/*
 * GPU side buffers containing Nanite resource data.
 */
class FGlobalResources : public FRenderResource
{
public:
	struct PassBuffers
	{
		// Used for statistics
		TRefCountPtr<FRDGPooledBuffer> StatsRasterizeArgsSWHWBuffer;
	};

	// Used for statistics
	uint32 StatsRenderFlags = 0;
	uint32 StatsDebugFlags  = 0;

public:
	virtual void InitRHI() override;
	virtual void ReleaseRHI() override;

	ENGINE_API void	Update(FRDGBuilder& GraphBuilder); // Called once per frame before any Nanite rendering has occurred.

	ENGINE_API static uint32 GetMaxCandidateClusters();
	ENGINE_API static uint32 GetMaxClusterBatches();
	ENGINE_API static uint32 GetMaxVisibleClusters();
	ENGINE_API static uint32 GetMaxNodes();

	inline PassBuffers& GetMainPassBuffers() { return MainPassBuffers; }
	inline PassBuffers& GetPostPassBuffers() { return PostPassBuffers; }

	TRefCountPtr<FRDGPooledBuffer>& GetMainAndPostNodesAndClusterBatchesBuffer() { return MainAndPostNodesAndClusterBatchesBuffer; };

	TRefCountPtr<FRDGPooledBuffer>& GetStatsBufferRef() { return StatsBuffer; }
	TRefCountPtr<FRDGPooledBuffer>& GetStructureBufferStride8() { return StructureBufferStride8; }

	FVertexFactory* GetVertexFactory() { return VertexFactory; }
	
private:
	PassBuffers MainPassBuffers;
	PassBuffers PostPassBuffers;

	class FVertexFactory* VertexFactory = nullptr;

	TRefCountPtr<FRDGPooledBuffer> MainAndPostNodesAndClusterBatchesBuffer;

	// Used for statistics
	TRefCountPtr<FRDGPooledBuffer> StatsBuffer;

	// Dummy structured buffer with stride8
	TRefCountPtr<FRDGPooledBuffer> StructureBufferStride8;
};

extern ENGINE_API TGlobalResource< FGlobalResources > GGlobalResources;

} // namespace Nanite
