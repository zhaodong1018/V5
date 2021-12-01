// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/NaniteStreamingManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "RenderingThread.h"
#include "UnifiedBuffer.h"
#include "CommonRenderResources.h"
#include "FileCache/FileCache.h"
#include "DistanceFieldAtlas.h"
#include "ClearQuad.h"
#include "RenderGraphUtils.h"
#include "Logging/LogMacros.h"
#include "Async/ParallelFor.h"
#include "Misc/Compression.h"

#define MAX_LEGACY_REQUESTS_PER_UPDATE		32u		// Legacy IO requests are slow and cause lots of bubbles, so we NEED to limit them.

#define MAX_REQUESTS_HASH_TABLE_SIZE		(MAX_STREAMING_REQUESTS << 1)
#define MAX_REQUESTS_HASH_TABLE_MASK		(MAX_REQUESTS_HASH_TABLE_SIZE - 1)
#define INVALID_HASH_ENTRY					0xFFFFFFFFu

#define INVALID_RUNTIME_RESOURCE_ID			0xFFFFFFFFu
#define INVALID_PAGE_INDEX					0xFFFFFFFFu

int32 GNaniteStreamingAsync = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingAsync(
	TEXT("r.Nanite.Streaming.Async"),
	GNaniteStreamingAsync,
	TEXT("Perform most of the Nanite streaming on an asynchronous worker thread instead of the rendering thread.")
);

float GNaniteStreamingBandwidthLimit = -1.0f;
static FAutoConsoleVariableRef CVarNaniteStreamingBandwidthLimit(
	TEXT("r.Nanite.Streaming.BandwidthLimit" ),
	GNaniteStreamingBandwidthLimit,
	TEXT("Streaming bandwidth limit in megabytes per second. Negatives values are interpreted as unlimited. ")
);

int32 GNaniteStreamingPoolSize = 512;
static FAutoConsoleVariableRef CVarNaniteStreamingPoolSize(
	TEXT("r.Nanite.Streaming.StreamingPoolSize"),
	GNaniteStreamingPoolSize,
	TEXT("Size of streaming pool in MB. Does not include memory used for root pages."),
	ECVF_ReadOnly
);

int32 GNaniteStreamingNumInitialRootPages = 2048;
static FAutoConsoleVariableRef CVarNaniteStreamingNumInitialRootPages(
	TEXT("r.Nanite.Streaming.NumInitialRootPages"),
	GNaniteStreamingNumInitialRootPages,
	TEXT("Number of root pages in initial allocation. Allowed to grow on demand if r.Nanite.Streaming.DynamicRootPages is enabled."),
	ECVF_ReadOnly
);

int32 GNaniteStreamingDynamicRootPages = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingDynamicRootPages(
	TEXT("r.Nanite.Streaming.DynamicRootPages"),
	GNaniteStreamingDynamicRootPages,
	TEXT("Determines if root page allocation is allowed to grow dynamically initial allocation set by r.Nanite.Streaming.NumInitialRootPages."),
	ECVF_ReadOnly
);

int32 GNaniteStreamingMaxPendingPages = 128;
static FAutoConsoleVariableRef CVarNaniteStreamingMaxPendingPages(
	TEXT("r.Nanite.Streaming.MaxPendingPages"),
	GNaniteStreamingMaxPendingPages,
	TEXT("Maximum number of pages that can be pending for installation."),
	ECVF_ReadOnly
);

int32 GNaniteStreamingMaxPageInstallsPerFrame = 128;
static FAutoConsoleVariableRef CVarNaniteStreamingMaxPageInstallsPerFrame(
	TEXT("r.Nanite.Streaming.MaxPageInstallsPerFrame"),
	GNaniteStreamingMaxPageInstallsPerFrame,
	TEXT("Maximum number of pages that can be installed per frame. Limiting this can limit the overhead of streaming."),
	ECVF_ReadOnly
);


DECLARE_CYCLE_STAT( TEXT("StreamingManager_Update"),STAT_NaniteStreamingManagerUpdate,	STATGROUP_Nanite );


DECLARE_DWORD_COUNTER_STAT(		TEXT("PageInstalls"),				STAT_NanitePageInstalls,					STATGROUP_Nanite );
DECLARE_DWORD_COUNTER_STAT(		TEXT("StreamingRequests"),			STAT_NaniteStreamingRequests,				STATGROUP_Nanite );
DECLARE_DWORD_COUNTER_STAT(		TEXT("UniqueStreamingRequests"),	STAT_NaniteUniqueStreamingRequests,			STATGROUP_Nanite );
DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("TotalPages"),					STAT_NaniteTotalPages,						STATGROUP_Nanite );
DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("RegisteredStreamingPages"),	STAT_NaniteRegisteredStreamingPages,		STATGROUP_Nanite );
DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("InstalledPages"),				STAT_NaniteInstalledPages,					STATGROUP_Nanite );
DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("RootPages"),					STAT_NaniteRootPages,						STATGROUP_Nanite );

DECLARE_LOG_CATEGORY_EXTERN(LogNaniteStreaming, Log, All);
DEFINE_LOG_CATEGORY(LogNaniteStreaming);

namespace Nanite
{

// Round up to smallest value greater than or equal to x of the form k*2^s where k < 2^NumSignificantBits.
// This is the same as RoundUpToPowerOfTwo when NumSignificantBits=1.
// For larger values of NumSignificantBits each po2 bucket is subdivided into 2^(NumSignificantBits-1) linear steps.
// This gives more steps while still maintaining an overall exponential structure and keeps numbers nice and round (in the po2 sense).

// Example:
// Representable values for different values of NumSignificantBits.
// 1: ..., 16, 32, 64, 128, 256, 512, ...
// 2: ..., 16, 24, 32,  48,  64,  96, ...
// 3: ..., 16, 20, 24,  28,  32,  40, ...
static uint32 RoundUpToSignificantBits(uint32 x, uint32 NumSignificantBits)
{
	check(NumSignificantBits <= 32);

	const int32_t Shift = FMath::Max((int32)FMath::CeilLogTwo(x) - (int32)NumSignificantBits, 0);
	const uint32 Mask = (1u << Shift) - 1u;
	return (x + Mask) & ~Mask;
}

class FTranscodePageToGPU_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTranscodePageToGPU_CS);
	SHADER_USE_PARAMETER_STRUCT(FTranscodePageToGPU_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32,								StartPageIndex)
		SHADER_PARAMETER(FIntVector4,							PageConstants)
		SHADER_PARAMETER_SRV(StructuredBuffer<FPageInstallInfo>,InstallInfoBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>,			PageDependenciesBuffer)
		SHADER_PARAMETER_SRV(ByteAddressBuffer,					SrcPageBuffer)
		SHADER_PARAMETER_UAV(RWByteAddressBuffer,				DstPageBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FTranscodePageToGPU_CS, "/Engine/Private/Nanite/Transcode.usf", "TranscodePageToGPU", SF_Compute);

class FClearStreamingRequestCount_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearStreamingRequestCount_CS);
	SHADER_USE_PARAMETER_STRUCT(FClearStreamingRequestCount_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FStreamingRequest>, OutStreamingRequests)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FClearStreamingRequestCount_CS, "/Engine/Private/Nanite/ClusterCulling.usf", "ClearStreamingRequestCount", SF_Compute);


// Lean hash table for deduplicating requests.
// Linear probing hash table that only supports add and never grows.
// This is intended to be kept alive over the duration of the program, so allocation and clearing only has to happen once.
// TODO: Unify with VT?
class FRequestsHashTable
{
	FStreamingRequest*		HashTable;
	uint32*					ElementIndices;	// List of indices to unique elements of HashTable
	uint32					NumElements;	// Number of unique elements in HashTable
public:
	FRequestsHashTable()
	{
		check(FMath::IsPowerOfTwo(MAX_REQUESTS_HASH_TABLE_SIZE));
		HashTable = new FStreamingRequest[MAX_REQUESTS_HASH_TABLE_SIZE];
		ElementIndices = new uint32[MAX_REQUESTS_HASH_TABLE_SIZE];
		for(uint32 i = 0; i < MAX_REQUESTS_HASH_TABLE_SIZE; i++)
		{
			HashTable[i].Key.RuntimeResourceID = INVALID_RUNTIME_RESOURCE_ID;
		}
		NumElements = 0;
	}
	~FRequestsHashTable()
	{
		delete[] HashTable;
		delete[] ElementIndices;
		HashTable = nullptr;
		ElementIndices = nullptr;
	}

	FORCEINLINE void AddRequest(const FStreamingRequest& Request)
	{
		uint32 TableIndex = GetTypeHash(Request.Key) & MAX_REQUESTS_HASH_TABLE_MASK;

		while(true)
		{
			FStreamingRequest& TableEntry = HashTable[TableIndex];
			if(TableEntry.Key == Request.Key)
			{
				// Found it. Just update the key.
				TableEntry.Priority = FMath::Max( TableEntry.Priority, Request.Priority );
				return;
			}

			if(TableEntry.Key.RuntimeResourceID == INVALID_RUNTIME_RESOURCE_ID)
			{
				// Empty slot. Take it and add this to cell to the elements list.
				TableEntry = Request;
				ElementIndices[NumElements++] = TableIndex;
				return;
			}

			// Slot was taken by someone else. Move on to next slot.
			TableIndex = (TableIndex + 1) & MAX_REQUESTS_HASH_TABLE_MASK;
		}
	}

	uint32 GetNumElements() const
	{
		return NumElements;
	}

	const FStreamingRequest& GetElement(uint32 Index) const
	{
		check( Index < NumElements );
		return HashTable[ElementIndices[Index]];
	}

	// Clear by looping through unique elements. Cost is proportional to number of unique elements, not the whole table.
	void Clear()
	{
		for( uint32 i = 0; i < NumElements; i++ )
		{
			FStreamingRequest& Request		= HashTable[ ElementIndices[ i ] ];
			Request.Key.RuntimeResourceID	= INVALID_RUNTIME_RESOURCE_ID;
		}
		NumElements = 0;
	}
};

FORCEINLINE bool IsRootPage(uint32 PageIndex)	// Keep in sync with ClusterCulling.usf
{
	return PageIndex == 0;
}

struct FPageInstallInfo
{
	uint32 SrcPageOffset;
	uint32 DstPageOffset;
	uint32 PageDependenciesStart;
	uint32 PageDependenciesNum;
};

class FStreamingPageUploader
{
	struct FAddedPageInfo
	{
		FPageInstallInfo	InstallInfo;
		FPageKey			GPUPageKey;
		uint32				InstallPassIndex;
	};
public:
	FStreamingPageUploader()
	{
		ResetState();
	}

	void Init(uint32 InMaxPages, uint32 InMaxPageBytes, uint32 InMaxStreamingPages)
	{
		ResetState();
		MaxPages = InMaxPages;
		MaxPageBytes = InMaxPageBytes;
		MaxStreamingPages = InMaxStreamingPages;

		const uint32 PageAllocationSize	= FMath::RoundUpToPowerOfTwo(MaxPageBytes);
		if (PageAllocationSize > PageUploadBuffer.NumBytes)
		{
			PageUploadBuffer.Release();
			PageUploadBuffer.NumBytes = PageAllocationSize;

			FRHIResourceCreateInfo CreateInfo(TEXT("Nanite.PageUploadBuffer"));
			PageUploadBuffer.Buffer = RHICreateStructuredBuffer(sizeof(uint32), PageUploadBuffer.NumBytes, BUF_ShaderResource | BUF_Volatile | BUF_ByteAddressBuffer, CreateInfo);
			PageUploadBuffer.SRV = RHICreateShaderResourceView(PageUploadBuffer.Buffer);
		}
		
		PageDataPtr = (uint8*)RHILockBuffer(PageUploadBuffer.Buffer, 0, PageAllocationSize, RLM_WriteOnly);
	}

	uint8* Add_GetRef(uint32 PageSize, uint32 DstPageOffset, const FPageKey& GPUPageKey, const TArray<uint32>& PageDependencies)
	{
		check(IsAligned(PageSize, 4));
		check(IsAligned(DstPageOffset, 4));

		const uint32 PageIndex = AddedPageInfos.Num();

		check(PageIndex < MaxPages);
		check(NextPageByteOffset + PageSize <= MaxPageBytes);

		FAddedPageInfo& Info = AddedPageInfos.AddDefaulted_GetRef();
		Info.GPUPageKey = GPUPageKey;
		Info.InstallInfo.SrcPageOffset = NextPageByteOffset;
		Info.InstallInfo.DstPageOffset = DstPageOffset;
		Info.InstallInfo.PageDependenciesStart = FlattenedPageDependencies.Num();
		Info.InstallInfo.PageDependenciesNum = PageDependencies.Num();
		Info.InstallPassIndex = 0xFFFFFFFFu;
		FlattenedPageDependencies.Append(PageDependencies);
		GPUPageKeyToAddedIndex.Add(GPUPageKey, PageIndex);
		
		uint8* ResultPtr = PageDataPtr + NextPageByteOffset;
		NextPageByteOffset += PageSize;
		
		return ResultPtr;
	}

	void Release()
	{
		InstallInfoUploadBuffer.Release();
		PageUploadBuffer.Release();
		PageDependenciesBuffer.Release();
		ResetState();
	}

	void ResourceUploadTo(FRHICommandList& RHICmdList, FRWByteAddressBuffer& DstBuffer)
	{
		RHIUnlockBuffer(PageUploadBuffer.Buffer);

		const uint32 NumPages = AddedPageInfos.Num();
		if (NumPages == 0)	// This can end up getting called with NumPages = 0 when NumReadyPages > 0 and all pages early out.
		{
			ResetState();
			return;
		}

		uint32 InstallInfoAllocationSize = FMath::RoundUpToPowerOfTwo(NumPages * sizeof(FPageInstallInfo));
		if (InstallInfoAllocationSize > InstallInfoUploadBuffer.NumBytes)
		{
			InstallInfoUploadBuffer.Release();
			InstallInfoUploadBuffer.NumBytes = InstallInfoAllocationSize;

			FRHIResourceCreateInfo CreateInfo(TEXT("Nanite.InstallInfoUploadBuffer"));
			InstallInfoUploadBuffer.Buffer = RHICreateStructuredBuffer(sizeof(FPageInstallInfo), InstallInfoUploadBuffer.NumBytes, BUF_ShaderResource | BUF_Volatile, CreateInfo);
			InstallInfoUploadBuffer.SRV = RHICreateShaderResourceView(InstallInfoUploadBuffer.Buffer);
		}
		FPageInstallInfo* InstallInfoPtr = (FPageInstallInfo*)RHILockBuffer(InstallInfoUploadBuffer.Buffer, 0, InstallInfoAllocationSize, RLM_WriteOnly);
	
		uint32 PageDependenciesAllocationSize = FMath::RoundUpToPowerOfTwo(FMath::Max(FlattenedPageDependencies.Num(), 4096) * sizeof(uint32));
		if (PageDependenciesAllocationSize > PageDependenciesBuffer.NumBytes)
		{
			PageDependenciesBuffer.Release();
			PageDependenciesBuffer.NumBytes = PageDependenciesAllocationSize;

			FRHIResourceCreateInfo CreateInfo(TEXT("Nanite.PageDependenciesBuffer"));
			PageDependenciesBuffer.Buffer = RHICreateStructuredBuffer(sizeof(uint32), PageDependenciesBuffer.NumBytes, BUF_ShaderResource | BUF_Volatile, CreateInfo);
			PageDependenciesBuffer.SRV = RHICreateShaderResourceView(PageDependenciesBuffer.Buffer);
		}

		uint32* PageDependenciesPtr = (uint32*)RHILockBuffer(PageDependenciesBuffer.Buffer, 0, PageDependenciesAllocationSize, RLM_WriteOnly);
		FMemory::Memcpy(PageDependenciesPtr, FlattenedPageDependencies.GetData(), FlattenedPageDependencies.Num() * sizeof(uint32));
		RHIUnlockBuffer(PageDependenciesBuffer.Buffer);

		// Split page installs into passes.
		// Every pass adds the pages that no longer have any unresolved dependency.
		// Essentially a naive multi-pass topology sort, but with a low number of passes in practice.
		check(NumInstalledPagesPerPass.Num() == 0);
		uint32 NumRemainingPages = NumPages;
		while (NumRemainingPages > 0)
		{
			const uint32 CurrentPassIndex = NumInstalledPagesPerPass.Num();
			uint32 NumPassPages = 0;
			for(FAddedPageInfo& PageInfo : AddedPageInfos)
			{
				if (PageInfo.InstallPassIndex < CurrentPassIndex)
					continue;	// Page already installed in an earlier pass

				bool bMissingDependency = false;
				for (uint32 i = 0; i < PageInfo.InstallInfo.PageDependenciesNum; i++)
				{
					const uint32 GPUPageIndex = FlattenedPageDependencies[PageInfo.InstallInfo.PageDependenciesStart + i];
					const FPageKey DependencyGPUPageKey = { PageInfo.GPUPageKey.RuntimeResourceID, GPUPageIndex };
					const uint32* DependencyAddedIndexPtr = GPUPageKeyToAddedIndex.Find(DependencyGPUPageKey);
					
					// Check if a dependency has not yet been installed.
					// We only need to resolve dependencies in the current batch. Batches are already ordered.
					if (DependencyAddedIndexPtr && AddedPageInfos[*DependencyAddedIndexPtr].InstallPassIndex >= CurrentPassIndex)
					{
						bMissingDependency = true;
						break;
					}
				}

				if (!bMissingDependency)
				{
					*InstallInfoPtr++ = PageInfo.InstallInfo;
					PageInfo.InstallPassIndex = CurrentPassIndex;
					NumPassPages++;
				}
			}

			NumInstalledPagesPerPass.Add(NumPassPages);
			NumRemainingPages -= NumPassPages;
		}

		RHIUnlockBuffer(InstallInfoUploadBuffer.Buffer);

		// Dispatch passes
		const uint32 NumPasses = NumInstalledPagesPerPass.Num();
		uint32 StartPageIndex = 0;
		for (uint32 PassIndex = 0; PassIndex < NumPasses; PassIndex++)
		{
			FTranscodePageToGPU_CS::FParameters Parameters;
			Parameters.InstallInfoBuffer = InstallInfoUploadBuffer.SRV;
			Parameters.PageDependenciesBuffer = PageDependenciesBuffer.SRV;
			Parameters.SrcPageBuffer = PageUploadBuffer.SRV;
			Parameters.DstPageBuffer = DstBuffer.UAV;
			Parameters.StartPageIndex = StartPageIndex;
			Parameters.PageConstants = FIntVector4(0, MaxStreamingPages, 0, 0);
			
			const uint32 NumPagesInPass = NumInstalledPagesPerPass[PassIndex];

			if (PassIndex != 0)
			{
				RHICmdList.Transition(FRHITransitionInfo(DstBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
			}

			auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FTranscodePageToGPU_CS>();
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, Parameters, FIntVector(MAX_TRANSCODE_GROUPS_PER_PAGE, NumPagesInPass, 1));
			StartPageIndex += NumPagesInPass;
		}

		ResetState();
	}
private:
	FByteAddressBuffer		InstallInfoUploadBuffer;
	FByteAddressBuffer		PageUploadBuffer;
	FByteAddressBuffer		PageDependenciesBuffer;
	uint8*					PageDataPtr;

	uint32					MaxPages;
	uint32					MaxPageBytes;
	uint32					MaxStreamingPages;
	uint32					NextPageByteOffset;
	TArray<FAddedPageInfo>	AddedPageInfos;
	TMap<FPageKey, uint32>	GPUPageKeyToAddedIndex;
	TArray<uint32>			FlattenedPageDependencies;
	TArray<uint32>			NumInstalledPagesPerPass;
	
	void ResetState()
	{
		PageDataPtr = nullptr;
		MaxPages = 0;
		MaxPageBytes = 0;
		NextPageByteOffset = 0;
		AddedPageInfos.Reset();
		GPUPageKeyToAddedIndex.Reset();
		FlattenedPageDependencies.Reset();
		NumInstalledPagesPerPass.Reset();
	}
};

FStreamingManager::FStreamingManager() :
	MaxStreamingPages(0),
	MaxPendingPages(0),
	MaxPageInstallsPerUpdate(0),
	MaxStreamingReadbackBuffers(4u),
	ReadbackBuffersWriteIndex(0),
	ReadbackBuffersNumPending(0),
	NextUpdateIndex(0),
	NumRegisteredStreamingPages(0),
	NumPendingPages(0),
	NextPendingPageIndex(0)
#if !UE_BUILD_SHIPPING
	,PrevUpdateTick(0)
#endif
{
	NextRootPageVersion.SetNum(MAX_GPU_PAGES);
}

void FStreamingManager::InitRHI()
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);

	MaxStreamingPages = (uint32)((uint64)GNaniteStreamingPoolSize * 1024 * 1024 / STREAMING_PAGE_GPU_SIZE);
	check(MaxStreamingPages + GNaniteStreamingNumInitialRootPages <= MAX_GPU_PAGES);

	MaxPendingPages = GNaniteStreamingMaxPendingPages;
	MaxPageInstallsPerUpdate = (uint32)FMath::Min(GNaniteStreamingMaxPageInstallsPerFrame, GNaniteStreamingMaxPendingPages);

	StreamingRequestReadbackBuffers.AddZeroed( MaxStreamingReadbackBuffers );

	// Initialize pages
	StreamingPageInfos.AddUninitialized( MaxStreamingPages );
	for( uint32 i = 0; i < MaxStreamingPages; i++ )
	{
		FStreamingPageInfo& Page = StreamingPageInfos[ i ];
		Page.RegisteredKey = { INVALID_RUNTIME_RESOURCE_ID, INVALID_PAGE_INDEX };
		Page.ResidentKey = { INVALID_RUNTIME_RESOURCE_ID, INVALID_PAGE_INDEX };
		Page.GPUPageIndex = i;
	}

	// Add pages to free list
	StreamingPageInfoFreeList = &StreamingPageInfos[0];
	for( uint32 i = 1; i < MaxStreamingPages; i++ )
	{
		StreamingPageInfos[ i - 1 ].Next = &StreamingPageInfos[ i ];
	}
	StreamingPageInfos[ MaxStreamingPages - 1 ].Next = nullptr;

	// Initialize LRU sentinels
	StreamingPageLRU.RegisteredKey		= { INVALID_RUNTIME_RESOURCE_ID, INVALID_PAGE_INDEX };
	StreamingPageLRU.ResidentKey		= { INVALID_RUNTIME_RESOURCE_ID, INVALID_PAGE_INDEX };
	StreamingPageLRU.GPUPageIndex		= INVALID_PAGE_INDEX;
	StreamingPageLRU.LatestUpdateIndex	= 0xFFFFFFFFu;
	StreamingPageLRU.RefCount			= 0xFFFFFFFFu;
	StreamingPageLRU.Next				= &StreamingPageLRU;
	StreamingPageLRU.Prev				= &StreamingPageLRU;

	StreamingPageFixupChunks.SetNum( MaxStreamingPages );

	PendingPages.SetNum( MaxPendingPages );

#if !WITH_EDITOR
	PendingPageStagingMemory.SetNumUninitialized( MaxPendingPages * MAX_PAGE_DISK_SIZE );
	for (int32 i = 0; i < PendingPages.Num(); i++)
	{
		PendingPages[i].MemoryPtr = PendingPageStagingMemory.GetData() + i * MAX_PAGE_DISK_SIZE;
	}
#endif

	RequestsHashTable	= new FRequestsHashTable();
	PageUploader		= new FStreamingPageUploader();

	RootPages.DataBuffer.Initialize(TEXT("Nanite.StreamingManager.RootPagesInitial"), sizeof(uint32));
	ClusterPageData.DataBuffer.Initialize(TEXT("Nanite.StreamingManager.ClusterPageDataInitial"), sizeof(uint32));
	Hierarchy.DataBuffer.Initialize(TEXT("Nanite.StreamingManager.HierarchyInitial"), sizeof(uint32));	// Dummy allocation to make sure it is a valid resource
}

void FStreamingManager::ReleaseRHI()
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	for (uint32 BufferIndex = 0; BufferIndex < MaxStreamingReadbackBuffers; ++BufferIndex)
	{
		if (StreamingRequestReadbackBuffers[BufferIndex])
		{
			delete StreamingRequestReadbackBuffers[BufferIndex];
			StreamingRequestReadbackBuffers[BufferIndex] = nullptr;
		}
	}

	for (FFixupChunk* FixupChunk : StreamingPageFixupChunks)
	{
		FMemory::Free(FixupChunk);
	}

	RootPages.Release();
	ClusterPageData.Release();
	Hierarchy.Release();
	ClusterFixupUploadBuffer.Release();
	StreamingRequestsBuffer.SafeRelease();

	delete RequestsHashTable;
	delete PageUploader;
}

void FStreamingManager::Add( FResources* Resources )
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	if (Resources->RuntimeResourceID == INVALID_RUNTIME_RESOURCE_ID)
	{
		check(Resources->RootClusterPage.Num() > 0);
		Resources->HierarchyOffset = Hierarchy.Allocator.Allocate(Resources->HierarchyNodes.Num());
		Resources->NumHierarchyNodes = Resources->HierarchyNodes.Num();
		Hierarchy.TotalUpload += Resources->HierarchyNodes.Num();
		INC_DWORD_STAT_BY( STAT_NaniteTotalPages, Resources->PageStreamingStates.Num() );
		INC_DWORD_STAT_BY( STAT_NaniteRootPages, 1 );

		Resources->RootPageIndex = RootPages.Allocator.Allocate( 1 );
		if (GNaniteStreamingDynamicRootPages == 0 && RootPages.Allocator.GetMaxSize() > GNaniteStreamingNumInitialRootPages)
		{
			UE_LOG(LogNaniteStreaming, Fatal, TEXT("Out of root pages. Increase the initial root page allocation (r.Nanite.Streaming.NumInitialRootPages) or allow it to grow dynamically (r.Nanite.Streaming.DynamicRootPages)."));
		}
		
		RootPages.TotalUpload++;

		// Version root pages so we can disregard invalid streaming requests.
		// TODO: We only need enough versions to cover the frame delay from the GPU, so most of the version bits can be reclaimed.
		check(Resources->RootPageIndex < MAX_GPU_PAGES);
		Resources->RuntimeResourceID = (NextRootPageVersion[Resources->RootPageIndex]++ << MAX_GPU_PAGES_BITS) | Resources->RootPageIndex;
		RuntimeResourceMap.Add( Resources->RuntimeResourceID, Resources );
		
		PendingAdds.Add( Resources );
	}
}

void FStreamingManager::Remove( FResources* Resources )
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	if (Resources->RuntimeResourceID != INVALID_RUNTIME_RESOURCE_ID)
	{
		Hierarchy.Allocator.Free( Resources->HierarchyOffset, Resources->NumHierarchyNodes );
		Resources->HierarchyOffset = -1;

		RootPages.Allocator.Free( Resources->RootPageIndex, 1 );
		Resources->RootPageIndex = -1;

		const uint32 NumResourcePages = Resources->PageStreamingStates.Num();
		INC_DWORD_STAT_BY( STAT_NaniteTotalPages, NumResourcePages );
		DEC_DWORD_STAT_BY( STAT_NaniteRootPages, 1 );

		// Move all registered pages to the free list. No need to properly uninstall them as they are no longer referenced from the hierarchy.
		for( uint32 PageIndex = 0; PageIndex < NumResourcePages; PageIndex++ )
		{
			FPageKey Key = { Resources->RuntimeResourceID, PageIndex };
			FStreamingPageInfo* Page;
			if( RegisteredStreamingPagesMap.RemoveAndCopyValue(Key, Page) )
			{
				Page->RegisteredKey.RuntimeResourceID = INVALID_RUNTIME_RESOURCE_ID;	// Mark as free, so we won't try to uninstall it later
				MovePageToFreeList( Page );
			}
		}

		RuntimeResourceMap.Remove( Resources->RuntimeResourceID );
		Resources->RuntimeResourceID = INVALID_RUNTIME_RESOURCE_ID;
		PendingAdds.Remove( Resources );
	}
}

void FStreamingManager::CollectDependencyPages( FResources* Resources, TSet< FPageKey >& DependencyPages, const FPageKey& Key )
{
	LLM_SCOPE_BYTAG(Nanite);
	if( DependencyPages.Find( Key ) )
		return;

	DependencyPages.Add( Key );

	FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[ Key.PageIndex ];
	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		uint32 DependencyPageIndex = Resources->PageDependencies[ PageStreamingState.DependenciesStart + i ];

		if( IsRootPage( DependencyPageIndex ) )
			continue;

		FPageKey ChildKey = { Key.RuntimeResourceID, DependencyPageIndex };
		if( DependencyPages.Find( ChildKey ) == nullptr )
		{
			CollectDependencyPages( Resources, DependencyPages, ChildKey );
		}
	}
}

void FStreamingManager::SelectStreamingPages( FResources* Resources, TArray< FPageKey >& SelectedPages, TSet<FPageKey>& SelectedPagesSet, uint32 RuntimeResourceID, uint32 PageIndex, uint32 MaxSelectedPages )
{
	LLM_SCOPE_BYTAG(Nanite);
	FPageKey Key = { RuntimeResourceID, PageIndex };
	if( SelectedPagesSet.Find( Key ) || (uint32)SelectedPages.Num() >= MaxSelectedPages )
		return;

	SelectedPagesSet.Add( Key );

	const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[ PageIndex ];
	
	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		uint32 DependencyPageIndex = Resources->PageDependencies[ PageStreamingState.DependenciesStart + i ];
		if( IsRootPage( DependencyPageIndex ) )
			continue;

		FPageKey DependencyKey = { RuntimeResourceID, DependencyPageIndex };
		if( RegisteredStreamingPagesMap.Find( DependencyKey ) == nullptr )
		{
			SelectStreamingPages( Resources, SelectedPages, SelectedPagesSet, RuntimeResourceID, DependencyPageIndex, MaxSelectedPages );
		}
	}

	if( (uint32)SelectedPages.Num() < MaxSelectedPages )
	{
		SelectedPages.Push( { RuntimeResourceID, PageIndex } );	// We need to write ourselves after our dependencies
	}
}

void FStreamingManager::RegisterStreamingPage( FStreamingPageInfo* Page, const FPageKey& Key )
{
	LLM_SCOPE_BYTAG(Nanite);
	check( !IsRootPage( Key.PageIndex ) );

	FResources** Resources = RuntimeResourceMap.Find( Key.RuntimeResourceID );
	check( Resources != nullptr );
	
	TArray< FPageStreamingState >& PageStreamingStates = (*Resources)->PageStreamingStates;
	FPageStreamingState& PageStreamingState = PageStreamingStates[ Key.PageIndex ];
	
	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		uint32 DependencyPageIndex = ( *Resources )->PageDependencies[ PageStreamingState.DependenciesStart + i ];
		if( IsRootPage( DependencyPageIndex ) )
			continue;

		FPageKey DependencyKey = { Key.RuntimeResourceID, DependencyPageIndex };
		FStreamingPageInfo** DependencyPage = RegisteredStreamingPagesMap.Find( DependencyKey );
		check( DependencyPage != nullptr );
		(*DependencyPage)->RefCount++;
	}

	// Insert at the front of the LRU
	FStreamingPageInfo& LRUSentinel = StreamingPageLRU;

	Page->Prev = &LRUSentinel;
	Page->Next = LRUSentinel.Next;
	LRUSentinel.Next->Prev = Page;
	LRUSentinel.Next = Page;

	Page->RegisteredKey = Key;
	Page->LatestUpdateIndex = NextUpdateIndex;
	Page->RefCount = 0;

	// Register Page
	RegisteredStreamingPagesMap.Add(Key, Page);

	NumRegisteredStreamingPages++;
	INC_DWORD_STAT( STAT_NaniteRegisteredStreamingPages );
}

void FStreamingManager::UnregisterPage( const FPageKey& Key )
{
	LLM_SCOPE_BYTAG(Nanite);
	check( !IsRootPage( Key.PageIndex ) );

	FResources** Resources = RuntimeResourceMap.Find( Key.RuntimeResourceID );
	check( Resources != nullptr );

	FStreamingPageInfo** PagePtr = RegisteredStreamingPagesMap.Find( Key );
	check( PagePtr != nullptr );
	FStreamingPageInfo* Page = *PagePtr;
	
	// Decrement reference counts of dependencies.
	TArray< FPageStreamingState >& PageStreamingStates = ( *Resources )->PageStreamingStates;
	FPageStreamingState& PageStreamingState = PageStreamingStates[ Key.PageIndex ];
	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		uint32 DependencyPageIndex = ( *Resources )->PageDependencies[ PageStreamingState.DependenciesStart + i ];
		if( IsRootPage( DependencyPageIndex ) )
			continue;

		FPageKey DependencyKey = { Key.RuntimeResourceID, DependencyPageIndex };
		FStreamingPageInfo** DependencyPage = RegisteredStreamingPagesMap.Find( DependencyKey );
		check( DependencyPage != nullptr );
		( *DependencyPage )->RefCount--;
	}

	RegisteredStreamingPagesMap.Remove( Key );
	MovePageToFreeList( Page );
}

void FStreamingManager::MovePageToFreeList( FStreamingPageInfo* Page )
{
	// Unlink
	FStreamingPageInfo* OldNext = Page->Next;
	FStreamingPageInfo* OldPrev = Page->Prev;
	OldNext->Prev = OldPrev;
	OldPrev->Next = OldNext;

	// Add to free list
	Page->Next = StreamingPageInfoFreeList;
	StreamingPageInfoFreeList = Page;

	NumRegisteredStreamingPages--;
	DEC_DWORD_STAT( STAT_NaniteRegisteredStreamingPages );
}

bool FStreamingManager::ArePageDependenciesCommitted(uint32 RuntimeResourceID, uint32 PageIndex, uint32 DependencyPageStart, uint32 DependencyPageNum)
{
	bool bResult = true;

	if (DependencyPageNum == 1)
	{
		// If there is only one dependency, we don't have to check as it is the page we are about to install.
		check(DependencyPageStart == PageIndex);
	}
	else if (DependencyPageNum > 1)	
	{
		for (uint32 i = 0; i < DependencyPageNum; i++)
		{
			uint32 DependencyPage = DependencyPageStart + i;
			FPageKey DependencyKey = { RuntimeResourceID, DependencyPage };
			FStreamingPageInfo** DependencyPagePtr = CommittedStreamingPageMap.Find(DependencyKey);
			if (DependencyPagePtr == nullptr || (*DependencyPagePtr)->ResidentKey != DependencyKey)	// Is the page going to be committed after this batch and does it already have its fixupchunk loaded?
			{
				bResult = false;
				break;
			}
		}
	}

	return bResult;
}

uint32 FStreamingManager::GPUPageIndexToGPUOffset(uint32 PageIndex) const
{
	return (FMath::Min(PageIndex, MaxStreamingPages) << STREAMING_PAGE_GPU_SIZE_BITS) + ((uint32)FMath::Max((int32)PageIndex - (int32)MaxStreamingPages, 0) << ROOT_PAGE_GPU_SIZE_BITS);
}

// Applies the fixups required to install/uninstall a page.
// Hierarchy references are patched up and leaf flags of parent clusters are set accordingly.
// GPUPageIndex == INVALID_PAGE_INDEX signals that the page should be uninstalled.
void FStreamingManager::ApplyFixups( const FFixupChunk& FixupChunk, const FResources& Resources, uint32 PageIndex, uint32 GPUPageIndex )
{
	LLM_SCOPE_BYTAG(Nanite);

	const uint32 RuntimeResourceID = Resources.RuntimeResourceID;
	const uint32 HierarchyOffset = Resources.HierarchyOffset;
	bool bIsUninstall = ( GPUPageIndex == INVALID_PAGE_INDEX );
	uint32 Flags = bIsUninstall ? NANITE_CLUSTER_FLAG_LEAF : 0;

	// Fixup clusters
	for( uint32 i = 0; i < FixupChunk.Header.NumClusterFixups; i++ )
	{
		const FClusterFixup& Fixup = FixupChunk.GetClusterFixup( i );

		bool bPageDependenciesCommitted = bIsUninstall || ArePageDependenciesCommitted(RuntimeResourceID, PageIndex, Fixup.GetPageDependencyStart(), Fixup.GetPageDependencyNum());
		if (!bPageDependenciesCommitted)
			continue;
		
		uint32 TargetPageIndex = Fixup.GetPageIndex();
		uint32 TargetGPUPageIndex = INVALID_PAGE_INDEX;
		uint32 NumTargetPageClusters = 0;

		if( IsRootPage( TargetPageIndex ) )
		{
			TargetGPUPageIndex = MaxStreamingPages + Resources.RootPageIndex;
			NumTargetPageClusters = RootPageInfos[ Resources.RootPageIndex ].NumClusters;
		}
		else
		{
			FPageKey TargetKey = { RuntimeResourceID, TargetPageIndex };
			FStreamingPageInfo** TargetPagePtr = CommittedStreamingPageMap.Find( TargetKey );

			check( bIsUninstall || TargetPagePtr );
			if (TargetPagePtr)
			{
				FStreamingPageInfo* TargetPage = *TargetPagePtr;
				FFixupChunk& TargetFixupChunk = *StreamingPageFixupChunks[TargetPage->GPUPageIndex];
				check(StreamingPageInfos[TargetPage->GPUPageIndex].ResidentKey == TargetKey);

				NumTargetPageClusters = TargetFixupChunk.Header.NumClusters;
				check(Fixup.GetClusterIndex() < NumTargetPageClusters);

				TargetGPUPageIndex = TargetPage->GPUPageIndex;
			}
		}
		
		if(TargetGPUPageIndex != INVALID_PAGE_INDEX)
		{
			uint32 ClusterIndex = Fixup.GetClusterIndex();
			uint32 FlagsOffset = offsetof( FPackedCluster, Flags );
			uint32 Offset = GPUPageIndexToGPUOffset( TargetGPUPageIndex ) + GPU_PAGE_HEADER_SIZE + ( ( FlagsOffset >> 4 ) * NumTargetPageClusters + ClusterIndex ) * 16 + ( FlagsOffset & 15 );
			ClusterFixupUploadBuffer.Add( Offset / sizeof( uint32 ), &Flags, 1 );
		}
	}

	// Fixup hierarchy
	for( uint32 i = 0; i < FixupChunk.Header.NumHierachyFixups; i++ )
	{
		const FHierarchyFixup& Fixup = FixupChunk.GetHierarchyFixup( i );

		bool bPageDependenciesCommitted = bIsUninstall || ArePageDependenciesCommitted(RuntimeResourceID, PageIndex, Fixup.GetPageDependencyStart(), Fixup.GetPageDependencyNum());
		if (!bPageDependenciesCommitted)
			continue;

		FPageKey TargetKey = { RuntimeResourceID, Fixup.GetPageIndex() };
		uint32 TargetGPUPageIndex = INVALID_PAGE_INDEX;
		if (!bIsUninstall)
		{
			if (IsRootPage(TargetKey.PageIndex))
			{
				TargetGPUPageIndex = MaxStreamingPages + Resources.RootPageIndex;
			}
			else
			{
				FStreamingPageInfo** TargetPagePtr = CommittedStreamingPageMap.Find(TargetKey);
				check(TargetPagePtr);
				check((*TargetPagePtr)->ResidentKey == TargetKey);
				TargetGPUPageIndex = (*TargetPagePtr)->GPUPageIndex;
			}
		}
		
		// Uninstalls are unconditional. The same uninstall might happen more than once.
		// If this page is getting uninstalled it also means it wont be reinstalled and any split groups can't be satisfied, so we can safely uninstall them.	
		
		uint32 HierarchyNodeIndex = Fixup.GetNodeIndex();
		check( HierarchyNodeIndex < Resources.NumHierarchyNodes );
		uint32 ChildIndex = Fixup.GetChildIndex();
		uint32 ChildStartReference = bIsUninstall ? 0xFFFFFFFFu : ( ( TargetGPUPageIndex << MAX_CLUSTERS_PER_PAGE_BITS ) | Fixup.GetClusterGroupPartStartIndex() );
		uint32 Offset = ( size_t )&( ( (FPackedHierarchyNode*)0 )[ HierarchyOffset + HierarchyNodeIndex ].Misc1[ ChildIndex ].ChildStartReference );
		Hierarchy.UploadBuffer.Add( Offset / sizeof( uint32 ), &ChildStartReference );
	}
}

void FStreamingManager::InstallReadyPages( uint32 NumReadyPages )
{
	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::CopyReadyPages);

	if (NumReadyPages == 0)
		return;

	const uint32 StartPendingPageIndex = ( NextPendingPageIndex + MaxPendingPages - NumPendingPages ) % MaxPendingPages;

	struct FUploadTask
	{
		FPendingPage* PendingPage = nullptr;
		uint8* Dst = nullptr;
		const uint8* Src = nullptr;
		uint32 SrcSize = 0;
	};

#if WITH_EDITOR
	TMap<FResources*, const uint8*> ResourceToBulkPointer;
#endif

	TArray<FUploadTask> UploadTasks;
	UploadTasks.AddDefaulted(NumReadyPages);

	// Install ready pages
	{
		// Batched page install:
		// GPU uploads are unordered, so we need to make sure we have no overlapping writes.
		// For actual page uploads, we only upload the last page that ends up on a given GPU page.

		// Fixups are handled with set of UploadBuffers that are executed AFTER page upload.
		// To ensure we don't end up fixing up the same addresses more than once, we only perform the fixup associated with the first uninstall and the last install on a given GPU page.
		// If a page ends up being both installed and uninstalled in the same frame, we only install it to prevent a race.
		// Uninstall fixup depends on StreamingPageFixupChunks that is also updated by installs. To prevent races we perform all uninstalls before installs.
		
		// Calculate first and last Pending Page Index update for each GPU page.
		TMap<uint32, uint32> GPUPageToLastPendingPageIndex;
		for (uint32 i = 0; i < NumReadyPages; i++)
		{
			uint32 PendingPageIndex = (StartPendingPageIndex + i) % MaxPendingPages;
			FPendingPage& PendingPage = PendingPages[PendingPageIndex];
			
			// Update when the GPU page was touched for the last time.
			FResources** Resources = RuntimeResourceMap.Find(PendingPage.InstallKey.RuntimeResourceID);
			if(Resources)
			{
				GPUPageToLastPendingPageIndex.Add(PendingPage.GPUPageIndex, PendingPageIndex);
			}
		}

		TSet<FPageKey> BatchNewPageKeys;
		for (auto& Elem : GPUPageToLastPendingPageIndex)
		{
			uint32 GPUPageIndex = Elem.Key;

			// Remove uninstalled pages from streaming map, so we won't try to do uninstall fixup on them.
			FStreamingPageInfo& StreamingPageInfo = StreamingPageInfos[GPUPageIndex];
			if (StreamingPageInfo.ResidentKey.RuntimeResourceID != INVALID_RUNTIME_RESOURCE_ID)
			{
				CommittedStreamingPageMap.Remove(StreamingPageInfo.ResidentKey);
			}

			// Mark newly installed page
			FPendingPage& PendingPage = PendingPages[Elem.Value];
			BatchNewPageKeys.Add(PendingPage.InstallKey);
		}

		// Uninstall pages
		// We are uninstalling pages in a separate pass as installs will also overwrite the GPU page fixup information we need for uninstalls.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UninstallFixup);
			for (auto& Elem : GPUPageToLastPendingPageIndex)
			{
				uint32 GPUPageIndex = Elem.Key;
				FStreamingPageInfo& StreamingPageInfo = StreamingPageInfos[GPUPageIndex];

				// Uninstall GPU page
				if (StreamingPageInfo.ResidentKey.RuntimeResourceID != INVALID_RUNTIME_RESOURCE_ID)
				{
					// Apply fixups to uninstall page. No need to fix up anything if resource is gone.
					FResources** Resources = RuntimeResourceMap.Find(StreamingPageInfo.ResidentKey.RuntimeResourceID);
					if (Resources)
					{
						// Prevent race between installs and uninstalls of the same page. Only uninstall if the page is not going to be installed again.
						if (!BatchNewPageKeys.Contains(StreamingPageInfo.ResidentKey))
						{
							ApplyFixups(*StreamingPageFixupChunks[GPUPageIndex], **Resources, INVALID_PAGE_INDEX, INVALID_PAGE_INDEX);
						}
					}
				}

				StreamingPageInfo.ResidentKey.RuntimeResourceID = INVALID_RUNTIME_RESOURCE_ID;	// Only uninstall it the first time.
				DEC_DWORD_STAT(STAT_NaniteInstalledPages);
			}
		}

		// Commit to streaming map, so install fixups will happen on all pages
		for (auto& Elem : GPUPageToLastPendingPageIndex)
		{
			uint32 GPUPageIndex = Elem.Key;
			uint32 LastPendingPageIndex = Elem.Value;
			FPendingPage& PendingPage = PendingPages[LastPendingPageIndex];

			FResources** Resources = RuntimeResourceMap.Find(PendingPage.InstallKey.RuntimeResourceID);
			if (Resources)
			{
				CommittedStreamingPageMap.Add(PendingPage.InstallKey, &StreamingPageInfos[GPUPageIndex]);
			}
		}

		// Install pages
		// Must be processed in PendingPages order so FFixupChunks are loaded when we need them.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(InstallReadyPages);
			uint32 NumInstalledPages = 0;
			for (uint32 TaskIndex = 0; TaskIndex < NumReadyPages; TaskIndex++)
			{
				uint32 LastPendingPageIndex = (StartPendingPageIndex + TaskIndex) % MaxPendingPages;
				FPendingPage& PendingPage = PendingPages[LastPendingPageIndex];

				FUploadTask& UploadTask = UploadTasks[TaskIndex];
				UploadTask.PendingPage = &PendingPage;

				uint32* PagePtr = GPUPageToLastPendingPageIndex.Find(PendingPages[LastPendingPageIndex].GPUPageIndex);
				if (PagePtr == nullptr || *PagePtr != LastPendingPageIndex)
					continue;	// Skip resource install. Resource no longer exists or page has already been overwritten.

				FStreamingPageInfo& StreamingPageInfo = StreamingPageInfos[PendingPage.GPUPageIndex];
			
				FResources** Resources = RuntimeResourceMap.Find( PendingPage.InstallKey.RuntimeResourceID );
				check(Resources);

				TArray< FPageStreamingState >& PageStreamingStates = ( *Resources )->PageStreamingStates;
				const FPageStreamingState& PageStreamingState = PageStreamingStates[ PendingPage.InstallKey.PageIndex ];
				FStreamingPageInfo* StreamingPage = &StreamingPageInfos[ PendingPage.GPUPageIndex ];

				CommittedStreamingPageMap.Add(PendingPage.InstallKey, StreamingPage);

#if WITH_EDITOR
				// Make sure we only lock each resource BulkData once.
				const uint8** BulkDataPtrPtr = ResourceToBulkPointer.Find(*Resources);
				const uint8* BulkDataPtr;
				if (!BulkDataPtrPtr)
				{
					FByteBulkData& BulkData = (*Resources)->StreamableClusterPages;
					check(BulkData.IsBulkDataLoaded() && BulkData.GetBulkDataSize() > 0);
					BulkDataPtr = (const uint8*)BulkData.LockReadOnly();
					ResourceToBulkPointer.Add(*Resources, BulkDataPtr);
				}
				else
				{
					BulkDataPtr = *BulkDataPtrPtr;
				}
			
				const uint8* SrcPtr = BulkDataPtr + PageStreamingState.BulkOffset;
#else
				const uint8* SrcPtr = PendingPage.MemoryPtr;
#endif

				const uint32 FixupChunkSize = ((const FFixupChunk*)SrcPtr)->GetSize();
				FFixupChunk* FixupChunk = (FFixupChunk*)FMemory::Realloc(StreamingPageFixupChunks[PendingPage.GPUPageIndex], FixupChunkSize, sizeof(uint16));
				StreamingPageFixupChunks[PendingPage.GPUPageIndex] = FixupChunk;
				FMemory::Memcpy(FixupChunk, SrcPtr, FixupChunkSize);

				// Build list of GPU page dependencies
				GPUPageDependencies.Reset();
				if(PageStreamingState.Flags & NANITE_PAGE_FLAG_RELATIVE_ENCODING)
				{
					for (uint32 i = 0; i < PageStreamingState.DependenciesNum; i++)
					{
						const uint32 DependencyPageIndex = (*Resources)->PageDependencies[PageStreamingState.DependenciesStart + i];
						if (IsRootPage(DependencyPageIndex))
						{
							GPUPageDependencies.Add(MaxStreamingPages + (*Resources)->RootPageIndex);
						}
						else
						{
							FPageKey DependencyKey = { PendingPage.InstallKey.RuntimeResourceID, DependencyPageIndex };
							FStreamingPageInfo** DependencyPagePtr = CommittedStreamingPageMap.Find(DependencyKey);
							check(DependencyPagePtr != nullptr);
							GPUPageDependencies.Add((*DependencyPagePtr)->GPUPageIndex);
						}
					}
				}
			
				uint32 PageOffset = GPUPageIndexToGPUOffset( PendingPage.GPUPageIndex );
				uint32 DataSize = PageStreamingState.BulkSize - FixupChunkSize;
				check(NumInstalledPages < MaxPageInstallsPerUpdate);

				const FPageKey GPUPageKey = FPageKey{ PendingPage.InstallKey.RuntimeResourceID, PendingPage.GPUPageIndex };

				UploadTask.PendingPage = &PendingPage;
				UploadTask.Dst = PageUploader->Add_GetRef(DataSize, PageOffset, GPUPageKey, GPUPageDependencies);
				UploadTask.Src = SrcPtr + FixupChunkSize;
				UploadTask.SrcSize = DataSize;
				NumInstalledPages++;

				// Apply fixups to install page
				StreamingPage->ResidentKey = PendingPage.InstallKey;
				ApplyFixups( *FixupChunk, **Resources, PendingPage.InstallKey.PageIndex, PendingPage.GPUPageIndex );

				INC_DWORD_STAT( STAT_NaniteInstalledPages );
				INC_DWORD_STAT(STAT_NanitePageInstalls);
			}
		}
	}

	// Upload pages
	ParallelFor(UploadTasks.Num(), [&UploadTasks](int32 i)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CopyPageTask);
		const FUploadTask& Task = UploadTasks[i];
		
		if(Task.Dst)	// Dst can be 0 if we skipped install in InstallReadyPages.
		{
			FMemory::Memcpy(Task.Dst, Task.Src, Task.SrcSize);
		}

#if !WITH_EDITOR
		if (Task.PendingPage->AsyncRequest)
		{
			check(Task.PendingPage->AsyncRequest->PollCompletion());	
			delete Task.PendingPage->AsyncRequest;
			delete Task.PendingPage->AsyncHandle;
			Task.PendingPage->AsyncRequest = nullptr;
			Task.PendingPage->AsyncHandle = nullptr;
		}
		else
		{
			check(Task.PendingPage->Request.Status().IsCompleted());
		}
#endif
	});

#if WITH_EDITOR
	// Unlock BulkData
	for (auto it : ResourceToBulkPointer)
	{
		FResources* Resources = it.Key;
		FByteBulkData& BulkData = Resources->StreamableClusterPages;
		BulkData.Unlock();
	}
#endif
}

#if DO_CHECK
void FStreamingManager::VerifyPageLRU( FStreamingPageInfo& List, uint32 TargetListLength, bool bCheckUpdateIndex )
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::VerifyPageLRU);

	uint32 ListLength = 0u;
	uint32 PrevUpdateIndex = 0u;
	FStreamingPageInfo* Ptr = List.Prev;
	while( Ptr != &List )
	{
		if( bCheckUpdateIndex )
		{
			check( Ptr->LatestUpdateIndex >= PrevUpdateIndex );
			PrevUpdateIndex = Ptr->LatestUpdateIndex;
		}

		ListLength++;
		Ptr = Ptr->Prev;
	}

	check( ListLength == TargetListLength );
}
#endif

bool FStreamingManager::ProcessNewResources( FRDGBuilder& GraphBuilder)
{
	LLM_SCOPE_BYTAG(Nanite);

	if( PendingAdds.Num() == 0 )
		return false;

	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::ProcessNewResources);

	// Upload hierarchy for pending resources
	ResizeResourceIfNeeded( GraphBuilder.RHICmdList, Hierarchy.DataBuffer, FMath::RoundUpToPowerOfTwo( Hierarchy.Allocator.GetMaxSize() ) * sizeof( FPackedHierarchyNode ), TEXT("Nanite.StreamingManager.Hierarchy") );

	check( MaxStreamingPages <= MAX_GPU_PAGES );
	uint32 MaxRootPages = MAX_GPU_PAGES - MaxStreamingPages;
	
	uint32 NumAllocatedRootPages;	
	if(GNaniteStreamingDynamicRootPages)
		NumAllocatedRootPages = FMath::Clamp( RoundUpToSignificantBits( RootPages.Allocator.GetMaxSize(), 2 ), (uint32)GNaniteStreamingNumInitialRootPages, MaxRootPages );
	else
		NumAllocatedRootPages = GNaniteStreamingNumInitialRootPages;

	check( NumAllocatedRootPages >= (uint32)RootPages.Allocator.GetMaxSize() );	// Root pages just don't fit!
	
	uint32 WidthInTiles = 12;
	uint32 TileSize = 12;
	uint32 AtlasBytes = FMath::Square( WidthInTiles * TileSize ) * sizeof( uint16 );
	ResizeResourceIfNeeded( GraphBuilder.RHICmdList, RootPages.DataBuffer, NumAllocatedRootPages * AtlasBytes, TEXT("Nanite.StreamingManager.RootPages") );

	
	const uint32 NumAllocatedPages = MaxStreamingPages + NumAllocatedRootPages;
	const uint32 AllocatedPagesSize = GPUPageIndexToGPUOffset( NumAllocatedPages );
	check( NumAllocatedPages <= MAX_GPU_PAGES );
	ResizeResourceIfNeeded( GraphBuilder.RHICmdList, ClusterPageData.DataBuffer, AllocatedPagesSize, TEXT("Nanite.StreamingManager.ClusterPageData") );

	check( AllocatedPagesSize <= ( 1u << 31 ) );	// 2GB seems to be some sort of limit.
													// TODO: Is it a GPU/API limit or is it a signed integer bug on our end?
	RootPageInfos.SetNum( NumAllocatedRootPages );

	uint32 NumPendingAdds = PendingAdds.Num();

	// TODO: These uploads can end up being quite large.
	// We should try to change the high level logic so the proxy is not considered loaded until the root page has been loaded, so we can split this over multiple frames.
	
	Hierarchy.UploadBuffer.Init( Hierarchy.TotalUpload, sizeof( FPackedHierarchyNode ), false, TEXT("Nanite.StreamingManager.HierarchyUpload"));
	RootPages.UploadBuffer.Init( RootPages.TotalUpload, AtlasBytes, false, TEXT("Nanite.StreamingManager.RootPagesUpload"));
	
	// Calculate total requires size
	uint32 TotalPageSize = 0;
	for (uint32 i = 0; i < NumPendingAdds; i++)
	{
		TotalPageSize += PendingAdds[i]->PageStreamingStates[0].PageSize;
	}

	PageUploader->Init(NumPendingAdds, TotalPageSize, MaxStreamingPages);

	GPUPageDependencies.Reset();

	for( FResources* Resources : PendingAdds )
	{
		uint32 GPUPageIndex = MaxStreamingPages + Resources->RootPageIndex;
		uint8* Ptr = Resources->RootClusterPage.GetData();
		FFixupChunk& FixupChunk = *(FFixupChunk*)Ptr;
		uint32 FixupChunkSize = FixupChunk.GetSize();
		uint32 NumClusters = FixupChunk.Header.NumClusters;

		const FPageKey GPUPageKey = { Resources->RuntimeResourceID, GPUPageIndex };

		const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[0];
		uint32 PageDiskSize = PageStreamingState.BulkSize - FixupChunkSize;
		uint32 PageOffset = GPUPageIndexToGPUOffset(GPUPageIndex);
		uint8* Dst = PageUploader->Add_GetRef(PageDiskSize, PageOffset, GPUPageKey, GPUPageDependencies);
		FMemory::Memcpy(Dst, Ptr + FixupChunkSize, PageDiskSize);

		// Root node should only have fixups that depend on other pages and cannot be satisfied yet.

		// Fixup hierarchy
		for(uint32 i = 0; i < FixupChunk.Header.NumHierachyFixups; i++)
		{
			const FHierarchyFixup& Fixup = FixupChunk.GetHierarchyFixup( i );
			uint32 HierarchyNodeIndex = Fixup.GetNodeIndex();
			check( HierarchyNodeIndex < (uint32)Resources->HierarchyNodes.Num() );
			uint32 ChildIndex = Fixup.GetChildIndex();
			uint32 GroupStartIndex = Fixup.GetClusterGroupPartStartIndex();
			uint32 ChildStartReference = ( GPUPageIndex << MAX_CLUSTERS_PER_PAGE_BITS ) | Fixup.GetClusterGroupPartStartIndex();

			if(Fixup.GetPageDependencyNum() == 0)	// Only install part if it has no other dependencies
			{
				Resources->HierarchyNodes[HierarchyNodeIndex].Misc1[ChildIndex].ChildStartReference = ChildStartReference;
			}
		}
		
		Hierarchy.UploadBuffer.Add( Resources->HierarchyOffset, &Resources->HierarchyNodes[ 0 ], Resources->HierarchyNodes.Num() );
		if(Resources->ImposterAtlas.Num() > 0)
		{
			RootPages.UploadBuffer.Add( Resources->RootPageIndex, Resources->ImposterAtlas.GetData() );
		}

		FRootPageInfo& RootPageInfo = RootPageInfos[ Resources->RootPageIndex ];
		RootPageInfo.RuntimeResourceID = Resources->RuntimeResourceID;
		RootPageInfo.NumClusters = NumClusters;

#if !WITH_EDITOR
		// We can't free the CPU data in editor builds because the resource might be kept around and used for cooking later.
		Resources->RootClusterPage.Empty();
		Resources->HierarchyNodes.Empty();
		Resources->ImposterAtlas.Empty();
#endif
	}

	{
		FRHITransitionInfo UAVTransitions[] =
		{
			FRHITransitionInfo(ClusterPageData.DataBuffer.UAV,		ERHIAccess::Unknown, ERHIAccess::UAVCompute),
			FRHITransitionInfo(Hierarchy.DataBuffer.UAV,			ERHIAccess::Unknown, ERHIAccess::UAVCompute),
			FRHITransitionInfo(RootPages.DataBuffer.UAV,			ERHIAccess::Unknown, ERHIAccess::UAVCompute)
		};
		GraphBuilder.RHICmdList.Transition(UAVTransitions);
		
		Hierarchy.TotalUpload = 0;
		Hierarchy.UploadBuffer.ResourceUploadTo(GraphBuilder.RHICmdList, Hierarchy.DataBuffer, false);

		RootPages.TotalUpload = 0;
		RootPages.UploadBuffer.ResourceUploadTo(GraphBuilder.RHICmdList, RootPages.DataBuffer, false);

		PageUploader->ResourceUploadTo(GraphBuilder.RHICmdList, ClusterPageData.DataBuffer);

		// Transition root pages already since this one is not done while processing bBuffersTransitionedToWrite flag
		GraphBuilder.RHICmdList.Transition(
			{
				FRHITransitionInfo(RootPages.DataBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask)
			});
	}

	PendingAdds.Reset();
	if (NumPendingAdds > 1)
	{
		PageUploader->Release();
	}

	return true;
}

struct FStreamingUpdateParameters
{
	FStreamingManager* StreamingManager = nullptr;
};

class FStreamingUpdateTask
{
public:
	explicit FStreamingUpdateTask(const FStreamingUpdateParameters& InParams) : Parameters(InParams) {}

	FStreamingUpdateParameters Parameters;

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		Parameters.StreamingManager->AsyncUpdate();
	}

	static ESubsequentsMode::Type	GetSubsequentsMode()	{ return ESubsequentsMode::TrackSubsequents; }
	ENamedThreads::Type				GetDesiredThread()		{ return ENamedThreads::AnyNormalThreadNormalTask; }
	FORCEINLINE TStatId				GetStatId() const		{ return TStatId(); }
};

uint32 FStreamingManager::DetermineReadyPages()
{
	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::DetermineReadyPages);

	const uint32 StartPendingPageIndex = (NextPendingPageIndex + MaxPendingPages - NumPendingPages) % MaxPendingPages;
	uint32 NumReadyPages = 0;
	
#if !UE_BUILD_SHIPPING
	uint64 UpdateTick = FPlatformTime::Cycles64();
	uint64 DeltaTick = PrevUpdateTick ? UpdateTick - PrevUpdateTick : 0;
	PrevUpdateTick = UpdateTick;
#endif

	// Check how many pages are ready
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CheckReadyPages);

		for( uint32 i = 0; i < NumPendingPages && NumReadyPages < MaxPageInstallsPerUpdate; i++ )
		{
			uint32 PendingPageIndex = ( StartPendingPageIndex + i ) % MaxPendingPages;
			FPendingPage& PendingPage = PendingPages[ PendingPageIndex ];
			
#if WITH_EDITOR == 0
			if (PendingPage.AsyncRequest)
			{
				if (!PendingPage.AsyncRequest->PollCompletion())
					break;
			}
			else
			{
				if (!PendingPage.Request.Status().IsCompleted())
					break;
			}
#endif

#if !UE_BUILD_SHIPPING
			if( GNaniteStreamingBandwidthLimit >= 0.0 )
			{
				uint32 SimulatedBytesRemaining = FPlatformTime::ToSeconds64(DeltaTick) * GNaniteStreamingBandwidthLimit * 1048576.0;
				uint32 SimulatedBytesRead = FMath::Min( PendingPage.BytesLeftToStream, SimulatedBytesRemaining );
				PendingPage.BytesLeftToStream -= SimulatedBytesRead;
				SimulatedBytesRemaining -= SimulatedBytesRead;
				if( PendingPage.BytesLeftToStream > 0 )
					break;
			}
#endif

			NumReadyPages++;
		}
	}
	
	return NumReadyPages;
}

void FStreamingManager::BeginAsyncUpdate(FRDGBuilder& GraphBuilder)
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::BeginAsyncUpdate);
	RDG_EVENT_SCOPE(GraphBuilder, "Nanite::Streaming");
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteStreaming);
	
	check(!AsyncState.bUpdateActive);
	AsyncState = FAsyncState {};
	AsyncState.bUpdateActive = true;

	if (!StreamingRequestsBuffer.IsValid())
	{
		// Init and clear StreamingRequestsBuffer.
		// Can't do this in InitRHI as RHICmdList doesn't have a valid context yet.
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FStreamingRequest), MAX_STREAMING_REQUESTS);
		Desc.Usage = EBufferUsageFlags(Desc.Usage | BUF_SourceCopy);
		FRDGBufferRef StreamingRequestsBufferRef = GraphBuilder.CreateBuffer(Desc, TEXT("Nanite.StreamingRequests"));
		
		ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(StreamingRequestsBufferRef));

		StreamingRequestsBuffer = GraphBuilder.ConvertToExternalBuffer(StreamingRequestsBufferRef);
	}

	AsyncState.bBuffersTransitionedToWrite = ProcessNewResources(GraphBuilder);

	AsyncState.NumReadyPages = DetermineReadyPages();
	if (AsyncState.NumReadyPages > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AllocBuffers);
		// Prepare buffers for upload
		PageUploader->Init(MaxPageInstallsPerUpdate, MaxPageInstallsPerUpdate * MAX_PAGE_DISK_SIZE, MaxStreamingPages);
		ClusterFixupUploadBuffer.Init(MaxPageInstallsPerUpdate * MAX_CLUSTERS_PER_PAGE, sizeof(uint32), false, TEXT("Nanite.ClusterFixupUploadBuffer"));	// No more parents than children, so no more than MAX_CLUSTER_PER_PAGE parents need to be fixed
		Hierarchy.UploadBuffer.Init(2 * MaxPageInstallsPerUpdate * MAX_CLUSTERS_PER_PAGE, sizeof(uint32), false, TEXT("Nanite.HierarchyUploadBuffer"));	// Allocate enough to load all selected pages and evict old pages
	}

	// Find latest most recent ready readback buffer
	{
		// Find latest buffer that is ready
		uint32 Index = (ReadbackBuffersWriteIndex + MaxStreamingReadbackBuffers - ReadbackBuffersNumPending) % MaxStreamingReadbackBuffers;
		while (ReadbackBuffersNumPending > 0)
		{
			if (StreamingRequestReadbackBuffers[Index]->IsReady())	//TODO: process all buffers or just the latest?
			{
				ReadbackBuffersNumPending--;
				AsyncState.LatestReadbackBuffer = StreamingRequestReadbackBuffers[Index];
			}
			else
			{
				break;
			}
		}
	}

	// Lock buffer
	if (AsyncState.LatestReadbackBuffer)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LockBuffer);
		AsyncState.LatestReadbackBufferPtr = (const uint32*)AsyncState.LatestReadbackBuffer->Lock(MAX_STREAMING_REQUESTS * sizeof(uint32) * 3);
	}

	// Start async processing
	FStreamingUpdateParameters Parameters;
	Parameters.StreamingManager = this;

	check(AsyncTaskEvents.IsEmpty());
	if (GNaniteStreamingAsync)
	{
		AsyncTaskEvents.Add(TGraphTask<FStreamingUpdateTask>::CreateTask().ConstructAndDispatchWhenReady(Parameters));
	}
	else
	{
		AsyncUpdate();
	}
}

void FStreamingManager::AsyncUpdate()
{
	LLM_SCOPE_BYTAG(Nanite);
	SCOPED_NAMED_EVENT(FStreamingManager_AsyncUpdate, FColor::Cyan);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::AsyncUpdate);

	check(AsyncState.bUpdateActive);
	InstallReadyPages(AsyncState.NumReadyPages);

	if (!AsyncState.LatestReadbackBuffer)
		return;

	auto StreamingPriorityPredicate = []( const FStreamingRequest& A, const FStreamingRequest& B ) { return A.Priority > B.Priority; };

	PrioritizedRequestsHeap.Empty( MAX_STREAMING_REQUESTS );

	uint32 NumLegacyRequestsIssued = 0;

	struct FIORequestTask
	{
		FByteBulkData*	BulkData;
		FPendingPage*	PendingPage;
		uint32			BulkOffset;
		uint32			BulkSize;
	};
	TArray<FIORequestTask> RequestTasks;

	FIoDispatcher* IODispatcher = FBulkDataBase::GetIoDispatcher();

	
	TRACE_CPUPROFILER_EVENT_SCOPE(ProcessReadback);
	const uint32* BufferPtr = AsyncState.LatestReadbackBufferPtr;
	uint32 NumStreamingRequests = FMath::Min( BufferPtr[ 0 ], MAX_STREAMING_REQUESTS - 1u );	// First request is reserved for counter

	if( NumStreamingRequests > 0 )
	{
		// Update priorities
		FGPUStreamingRequest* StreamingRequestsPtr = ( ( FGPUStreamingRequest* ) BufferPtr + 1 );

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(DeduplicateRequests);
			RequestsHashTable->Clear();
			for( uint32 Index = 0; Index < NumStreamingRequests; Index++ )
			{
				const FGPUStreamingRequest& GPURequest = StreamingRequestsPtr[ Index ];
				uint32 NumPages = GPURequest.PageIndex_NumPages & MAX_GROUP_PARTS_MASK;
				uint32 PageStartIndex = GPURequest.PageIndex_NumPages >> MAX_GROUP_PARTS_BITS;
					
				FStreamingRequest Request;
				Request.Key.RuntimeResourceID = GPURequest.RuntimeResourceID;
				Request.Priority = GPURequest.Priority;
				for (uint32 i = 0; i < NumPages; i++)
				{
					Request.Key.PageIndex = PageStartIndex + i;
					check(!IsRootPage(Request.Key.PageIndex));
					RequestsHashTable->AddRequest(Request);
				}
			}
		}

		uint32 NumUniqueStreamingRequests = RequestsHashTable->GetNumElements();
		INC_DWORD_STAT_BY( STAT_NaniteStreamingRequests, NumStreamingRequests );
		INC_DWORD_STAT_BY( STAT_NaniteUniqueStreamingRequests, NumUniqueStreamingRequests );

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UpdatePriorities);

			struct FPrioritizedStreamingPage
			{
				FStreamingPageInfo* Page;
				uint32 Priority;
			};

			TArray< FPrioritizedStreamingPage > UpdatedPages;
			for(uint32 UniqueRequestIndex = 0; UniqueRequestIndex < NumUniqueStreamingRequests; UniqueRequestIndex++)
			{
				const FStreamingRequest& Request = RequestsHashTable->GetElement(UniqueRequestIndex);
				FStreamingPageInfo** StreamingPage = RegisteredStreamingPagesMap.Find( Request.Key );
				if( StreamingPage )
				{
					// Update index and move to front of LRU.
					(*StreamingPage)->LatestUpdateIndex = NextUpdateIndex;
					UpdatedPages.Push( { *StreamingPage, Request.Priority } );
				}
				else
				{
					// Page isn't there. Is the resource still here?
					FResources** Resources = RuntimeResourceMap.Find( Request.Key.RuntimeResourceID );
					if( Resources )
					{
						// ResourcesID is valid, so add request to the queue
						PrioritizedRequestsHeap.Push( Request );
					}
				}
			}

			PrioritizedRequestsHeap.Heapify( StreamingPriorityPredicate );

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(PrioritySort);
				UpdatedPages.Sort( []( const FPrioritizedStreamingPage& A, const FPrioritizedStreamingPage& B ) { return A.Priority < B.Priority; } );
			}

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UpdateLRU);

				for( const FPrioritizedStreamingPage& PrioritizedPage : UpdatedPages )
				{
					FStreamingPageInfo* Page = PrioritizedPage.Page;

					// Unlink
					FStreamingPageInfo* OldNext = Page->Next;
					FStreamingPageInfo* OldPrev = Page->Prev;
					OldNext->Prev = OldPrev;
					OldPrev->Next = OldNext;

					// Insert at the front of the LRU
					Page->Prev = &StreamingPageLRU;
					Page->Next = StreamingPageLRU.Next;
					StreamingPageLRU.Next->Prev = Page;
					StreamingPageLRU.Next = Page;
				}
			}
		}
	}

#if DO_CHECK
	VerifyPageLRU( StreamingPageLRU, NumRegisteredStreamingPages, true );
#endif
			
	uint32 MaxSelectedPages = MaxPendingPages - NumPendingPages;
	if( PrioritizedRequestsHeap.Num() > 0 )
	{
		TArray< FPageKey > SelectedPages;
		TSet< FPageKey > SelectedPagesSet;
			
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SelectStreamingPages);

			if(MaxSelectedPages > 0)
			{
				// Add low priority pages based on prioritized requests
				while( (uint32)SelectedPages.Num() < MaxSelectedPages && PrioritizedRequestsHeap.Num() > 0 )
				{
					FStreamingRequest SelectedRequest;
					PrioritizedRequestsHeap.HeapPop( SelectedRequest, StreamingPriorityPredicate, false );
					FResources** Resources = RuntimeResourceMap.Find( SelectedRequest.Key.RuntimeResourceID );
					check( Resources != nullptr );

					SelectStreamingPages( *Resources, SelectedPages, SelectedPagesSet, SelectedRequest.Key.RuntimeResourceID, SelectedRequest.Key.PageIndex, MaxSelectedPages );
				}
				check( (uint32)SelectedPages.Num() <= MaxSelectedPages );
			}
		}

		if( SelectedPages.Num() > 0 )
		{
			// Collect all pending registration dependencies so we are not going to remove them.
			TSet< FPageKey > RegistrationDependencyPages;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(CollectDependencyPages);
				for (const FPageKey& SelectedKey : SelectedPages)
				{
					FResources** Resources = RuntimeResourceMap.Find(SelectedKey.RuntimeResourceID);
					check(Resources != nullptr);

					CollectDependencyPages(*Resources, RegistrationDependencyPages, SelectedKey);	// Mark all dependencies as unremovable.
				}
			}

			FIoBatch Batch;
			FPendingPage* LastPendingPage = nullptr;
				
			// Register Pages
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(RegisterPages);

				for( const FPageKey& SelectedKey : SelectedPages )
				{
					FPendingPage& PendingPage = PendingPages[ NextPendingPageIndex ];

					FStreamingPageInfo** FreePage = nullptr;
						
					check(NumRegisteredStreamingPages <= MaxStreamingPages);
					if( NumRegisteredStreamingPages == MaxStreamingPages )
					{
						// No space. Free a page!
						FStreamingPageInfo* StreamingPage = StreamingPageLRU.Prev;
						while( StreamingPage != &StreamingPageLRU )
						{
							FStreamingPageInfo* PrevStreamingPage = StreamingPage->Prev;

							// Only remove leaf nodes. Make sure to never delete a node that was added this frame or is a dependency for a pending page registration.
							FPageKey FreeKey = PrevStreamingPage->RegisteredKey;
							if( PrevStreamingPage->RefCount == 0 && ( PrevStreamingPage->LatestUpdateIndex < NextUpdateIndex ) && RegistrationDependencyPages.Find( FreeKey ) == nullptr )
							{
								FreePage = RegisteredStreamingPagesMap.Find( FreeKey );
								check( FreePage != nullptr );
								check( (*FreePage)->RegisteredKey == FreeKey );
								break;
							}
							StreamingPage = PrevStreamingPage;
						}

						if (!FreePage)	// Couldn't free a page. Abort.
							break;
					}

					FResources** Resources = RuntimeResourceMap.Find(SelectedKey.RuntimeResourceID);
					check(Resources);
					FByteBulkData& BulkData = (*Resources)->StreamableClusterPages;

#if WITH_EDITOR
					bool bLegacyRequest = false;
#else
					bool bLegacyRequest = !BulkData.IsUsingIODispatcher();
					if (bLegacyRequest)
					{
						if (NumLegacyRequestsIssued == MAX_LEGACY_REQUESTS_PER_UPDATE)
							break;
					}
#endif

					if (FreePage)
						UnregisterPage((*FreePage)->RegisteredKey);

					const FPageStreamingState& PageStreamingState = ( *Resources )->PageStreamingStates[ SelectedKey.PageIndex ];
					check( !IsRootPage( SelectedKey.PageIndex ) );

#if !WITH_EDITOR
					if (!bLegacyRequest)
					{
						// Use IODispatcher when available
						LastPendingPage = &PendingPage;
						FIoChunkId ChunkID = BulkData.CreateChunkId();
						FIoReadOptions ReadOptions;
						ReadOptions.SetRange(BulkData.GetBulkDataOffsetInFile() + PageStreamingState.BulkOffset, PageStreamingState.BulkSize);
						ReadOptions.SetTargetVa(PendingPage.MemoryPtr);
						PendingPage.Request = Batch.Read(ChunkID, ReadOptions, IoDispatcherPriority_Low);
						PendingPage.AsyncHandle = nullptr;
						PendingPage.AsyncRequest = nullptr;
					}
					else
					{
						// Compatibility path without IODispatcher
						// Perform actual requests on workers to mitigate stalls
						FIORequestTask& Task = RequestTasks.AddDefaulted_GetRef();
						Task.BulkData = &BulkData;
						Task.PendingPage = &PendingPage;
						Task.BulkOffset = PageStreamingState.BulkOffset;
						Task.BulkSize = PageStreamingState.BulkSize;
					}
#endif

					// Grab a free page
					check(StreamingPageInfoFreeList != nullptr);
					FStreamingPageInfo* Page = StreamingPageInfoFreeList;
					StreamingPageInfoFreeList = StreamingPageInfoFreeList->Next;

					PendingPage.InstallKey = SelectedKey;
					PendingPage.GPUPageIndex = Page->GPUPageIndex;

					NextPendingPageIndex = ( NextPendingPageIndex + 1 ) % MaxPendingPages;
					NumPendingPages++;

#if !UE_BUILD_SHIPPING
					PendingPage.BytesLeftToStream = PageStreamingState.BulkSize;
#endif

					RegisterStreamingPage( Page, SelectedKey );

					if (bLegacyRequest)
						NumLegacyRequestsIssued++;

				}
			}

#if !WITH_EDITOR
			if (LastPendingPage)
			{
				// Issue batch
				TRACE_CPUPROFILER_EVENT_SCOPE(FIoBatch::Issue);
				Batch.Issue();
			}
#endif
		}
	}

#if !WITH_EDITOR
	// Legacy compatibility path
	// Delete this when we can rely on IOStore always being enabled
	if (!RequestTasks.IsEmpty())
	{
		ParallelFor(RequestTasks.Num(), [&RequestTasks](int32 i)
		{
			FIORequestTask& Task = RequestTasks[i];
			TRACE_CPUPROFILER_EVENT_SCOPE(Nanite_RequestTask);
			Task.PendingPage->AsyncHandle = Task.BulkData->OpenAsyncReadHandle();
			Task.PendingPage->AsyncRequest = Task.PendingPage->AsyncHandle->ReadRequest(Task.BulkData->GetBulkDataOffsetInFile() + Task.BulkOffset, Task.BulkSize, AIOP_Normal, nullptr, Task.PendingPage->MemoryPtr);
		});
	}
#endif

	// Issue warning if we end up taking the legacy path
	if (NumLegacyRequestsIssued > 0)
	{
		static bool bHasWarned = false;
		if(!bHasWarned)
		{
			UE_LOG(LogNaniteStreaming, Warning, TEXT(	"PERFORMANCE WARNING: Nanite is issuing IO requests using the legacy IO path. Expect slower streaming and higher CPU overhead. "
														"To avoid this penalty make sure iostore is enabled, it is supported by the platform, and that resources are built with -iostore."));
			bHasWarned = true;
		}
	}
}

void FStreamingManager::EndAsyncUpdate(FRDGBuilder& GraphBuilder)
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::EndAsyncUpdate);
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteStreaming);

	AddPass(GraphBuilder, RDG_EVENT_NAME("Nanite::Streaming"), [this](FRHICommandListImmediate& RHICmdList)
	{
		check(AsyncState.bUpdateActive);

		// Wait for async processing to finish
		if (GNaniteStreamingAsync)
		{
			check(!AsyncTaskEvents.IsEmpty());
			FTaskGraphInterface::Get().WaitUntilTasksComplete(AsyncTaskEvents, ENamedThreads::GetRenderThread_Local());
		}

		AsyncTaskEvents.Empty();

		// Unlock readback buffer
		if (AsyncState.LatestReadbackBuffer)
		{
			AsyncState.LatestReadbackBuffer->Unlock();
		}

		// Issue GPU copy operations
		if (AsyncState.NumReadyPages > 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UploadPages);

			if (AsyncState.bBuffersTransitionedToWrite)
			{
				// RHI validation fix: ClusterPageData decays to Unknown state after shader UAV access.
				RHICmdList.Transition(FRHITransitionInfo(ClusterPageData.DataBuffer.UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
			}
			else
			{
				RHICmdList.Transition(
				{
					FRHITransitionInfo(ClusterPageData.DataBuffer.UAV,		ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(Hierarchy.DataBuffer.UAV,			ERHIAccess::Unknown, ERHIAccess::UAVCompute)
				});
			}

			PageUploader->ResourceUploadTo(RHICmdList, ClusterPageData.DataBuffer);
			Hierarchy.UploadBuffer.ResourceUploadTo(RHICmdList, Hierarchy.DataBuffer, false);

			// NOTE: We need an additional barrier here to make sure pages are finished uploading before fixups can be applied.

			RHICmdList.Transition(
				{
					FRHITransitionInfo(ClusterPageData.DataBuffer.UAV,		ERHIAccess::Unknown, ERHIAccess::UAVCompute),
				});

			ClusterFixupUploadBuffer.ResourceUploadTo(RHICmdList, ClusterPageData.DataBuffer, false);

			NumPendingPages -= AsyncState.NumReadyPages;
			AsyncState.bBuffersTransitionedToWrite |= true;
		}

		// Transition resource back to read
		if (AsyncState.bBuffersTransitionedToWrite)
		{
			FRHICommandListExecutor::Transition(
			{
				FRHITransitionInfo(ClusterPageData.DataBuffer.UAV,		ERHIAccess::Unknown, ERHIAccess::SRVMask),
				FRHITransitionInfo(Hierarchy.DataBuffer.UAV,			ERHIAccess::UAVCompute, ERHIAccess::SRVMask)
			}, ERHIPipeline::Graphics, ERHIPipeline::All);

			AsyncState.bBuffersTransitionedToWrite = false;
		}

		NextUpdateIndex++;
		AsyncState.bUpdateActive = false;
	});
}

void FStreamingManager::SubmitFrameStreamingRequests(FRDGBuilder& GraphBuilder)
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteReadback);
	RDG_EVENT_SCOPE(GraphBuilder, "Nanite::Readback");

	if (ReadbackBuffersNumPending == MaxStreamingReadbackBuffers)
	{
		// Return when queue is full. It is NOT safe to EnqueueCopy on a buffer that already has a pending copy.
		return;
	}

	if (StreamingRequestReadbackBuffers[ReadbackBuffersWriteIndex] == nullptr)
	{
		FRHIGPUBufferReadback* GPUBufferReadback = new FRHIGPUBufferReadback(TEXT("Nanite.StreamingRequestReadBack"));
		StreamingRequestReadbackBuffers[ReadbackBuffersWriteIndex] = GPUBufferReadback;
	}

	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(StreamingRequestsBuffer);
	FRHIGPUBufferReadback* ReadbackBuffer = StreamingRequestReadbackBuffers[ReadbackBuffersWriteIndex];

	AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("Readback"), Buffer,
		[ReadbackBuffer, Buffer](FRHICommandList& RHICmdList)
	{
		ReadbackBuffer->EnqueueCopy(RHICmdList, Buffer->GetRHI(), 0u);
	});

	ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(Buffer));

	ReadbackBuffersWriteIndex = ( ReadbackBuffersWriteIndex + 1u ) % MaxStreamingReadbackBuffers;
	ReadbackBuffersNumPending = FMath::Min( ReadbackBuffersNumPending + 1u, MaxStreamingReadbackBuffers );
}

void FStreamingManager::ClearStreamingRequestCount(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAVRef)
{
	FClearStreamingRequestCount_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearStreamingRequestCount_CS::FParameters>();
	PassParameters->OutStreamingRequests = BufferUAVRef;

	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FClearStreamingRequestCount_CS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ClearStreamingRequestCount"),
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1)
	);
}


ENGINE_API bool FStreamingManager::IsAsyncUpdateInProgress()
{
	return AsyncState.bUpdateActive;
}

TGlobalResource< FStreamingManager > GStreamingManager;

} // namespace Nanite