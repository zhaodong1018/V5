// Copyright Epic Games, Inc. All Rights Reserved.

#include "D3D12RHIPrivate.h"
#include "D3D12TransientResourceAllocator.h"
#include "D3D12Stats.h"

D3D12_RESOURCE_STATES GetInitialResourceState(const D3D12_RESOURCE_DESC& InDesc)
{
	// Validate the creation state
	D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
	if (EnumHasAnyFlags(InDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
	{
		State = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}
	else if (EnumHasAnyFlags(InDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
	{
		State = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}
	else if (EnumHasAnyFlags(InDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
	{
		State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	check(State != D3D12_RESOURCE_STATE_COMMON);
	return State;
}

FD3D12TransientHeap::FD3D12TransientHeap(const FRHITransientHeapInitializer& Initializer, FD3D12Adapter* Adapter, FD3D12Device* Device, FRHIGPUMask VisibleNodeMask)
	: FRHITransientHeap(Initializer)
{
	D3D12_HEAP_FLAGS HeapFlags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

	if (Initializer.Flags != ERHITransientHeapFlags::AllowAll)
	{
		switch (Initializer.Flags)
		{
		case ERHITransientHeapFlags::AllowBuffers:
			HeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
			break;

		case ERHITransientHeapFlags::AllowTextures:
			HeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
			break;

		case ERHITransientHeapFlags::AllowRenderTargets:
			HeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
			break;
		}
	}

	D3D12_HEAP_PROPERTIES HeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HeapProperties.CreationNodeMask = FRHIGPUMask::FromIndex(Device->GetGPUIndex()).GetNative();
	HeapProperties.VisibleNodeMask = VisibleNodeMask.GetNative();

	D3D12_HEAP_DESC Desc = {};
	Desc.SizeInBytes = Initializer.Size;
	Desc.Properties = HeapProperties;
	Desc.Alignment = Initializer.Alignment;
	Desc.Flags = HeapFlags;

	if (Adapter->IsHeapNotZeroedSupported())
	{
		Desc.Flags |= FD3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
	}

	ID3D12Heap* D3DHeap = nullptr;
	{
		ID3D12Device* D3DDevice = Device->GetDevice();

		LLM_PLATFORM_SCOPE(ELLMTag::GraphicsPlatform);

		VERIFYD3D12RESULT(D3DDevice->CreateHeap(&Desc, IID_PPV_ARGS(&D3DHeap)));

#if PLATFORM_WINDOWS
		// Boost priority to make sure it's not paged out
		TRefCountPtr<ID3D12Device5> D3DDevice5;
		if (SUCCEEDED(D3DDevice->QueryInterface(IID_PPV_ARGS(D3DDevice5.GetInitReference()))))
		{
			ID3D12Pageable* Pageable = D3DHeap;
			D3D12_RESIDENCY_PRIORITY HeapPriority = D3D12_RESIDENCY_PRIORITY_HIGH;
			D3DDevice5->SetResidencyPriority(1, &Pageable, &HeapPriority);
		}
#endif // PLATFORM_WINDOWS
	}

	Heap = new FD3D12Heap(Device, VisibleNodeMask);
	Heap->SetHeap(D3DHeap, TEXT("TransientResourceAllocator Backing Heap"), true, true);
	Heap->BeginTrackingResidency(Desc.SizeInBytes);

	BaseGPUVirtualAddress = Heap->GetGPUVirtualAddress();

	INC_MEMORY_STAT_BY(STAT_D3D12TransientHeaps, Desc.SizeInBytes);
}

FD3D12TransientHeap::~FD3D12TransientHeap()
{
	if (Heap)
	{
		D3D12_HEAP_DESC Desc = Heap->GetHeapDesc();
		DEC_MEMORY_STAT_BY(STAT_D3D12TransientHeaps, Desc.SizeInBytes);
	}
}

TUniquePtr<FD3D12TransientResourceSystem> FD3D12TransientResourceSystem::Create(FD3D12Adapter* ParentAdapter, FRHIGPUMask VisibleNodeMask)
{
	FRHITransientResourceSystemInitializer Initializer = FRHITransientResourceSystemInitializer::CreateDefault();
	Initializer.HeapAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

	// Tier2 hardware is able to mix resource types onto the same heap.
	Initializer.bSupportsAllHeapFlags = ParentAdapter->GetResourceHeapTier() == D3D12_RESOURCE_HEAP_TIER_2;

	return TUniquePtr<FD3D12TransientResourceSystem>(new FD3D12TransientResourceSystem(Initializer, ParentAdapter, VisibleNodeMask));
}

FD3D12TransientResourceSystem::FD3D12TransientResourceSystem(const FRHITransientResourceSystemInitializer& Initializer, FD3D12Adapter* ParentAdapter, FRHIGPUMask InVisibleNodeMask)
	: FRHITransientResourceSystem(Initializer)
	, FD3D12AdapterChild(ParentAdapter)
	, VisibleNodeMask(InVisibleNodeMask)
{}

FRHITransientHeap* FD3D12TransientResourceSystem::CreateHeap(const FRHITransientHeapInitializer& HeapInitializer)
{
	return GetParentAdapter()->CreateLinkedObject<FD3D12TransientHeap>(VisibleNodeMask, [&](FD3D12Device* Device)
	{
		return new FD3D12TransientHeap(HeapInitializer, GetParentAdapter(), Device, VisibleNodeMask);
	});
}

FD3D12TransientResourceAllocator::FD3D12TransientResourceAllocator(FD3D12TransientResourceSystem& InParentSystem)
	: FD3D12AdapterChild(InParentSystem.GetParentAdapter())
	, Allocator(InParentSystem)
	, AllocationInfoQueryDevice(GetParentAdapter()->GetDevice(0))
{}

FRHITransientTexture* FD3D12TransientResourceAllocator::CreateTexture(const FRHITextureCreateInfo& InCreateInfo, const TCHAR* InDebugName, uint32 InPassIndex)
{
	FD3D12DynamicRHI* DynamicRHI = FD3D12DynamicRHI::GetD3DRHI();

	const D3D12_RESOURCE_DESC Desc = DynamicRHI->GetResourceDesc(InCreateInfo);
	const D3D12_RESOURCE_ALLOCATION_INFO Info = AllocationInfoQueryDevice->GetResourceAllocationInfo(Desc);

	return Allocator.CreateTexture(InCreateInfo, InDebugName, InPassIndex, Info.SizeInBytes, Info.Alignment,
		[&](const FRHITransientResourceAllocator::FResourceInitializer& Initializer)
	{
		ERHIAccess InitialState = ERHIAccess::UAVMask;

		if (EnumHasAnyFlags(InCreateInfo.Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable))
		{
			InitialState = ERHIAccess::RTV;
		}
		else if (EnumHasAnyFlags(InCreateInfo.Flags, TexCreate_DepthStencilTargetable))
		{
			InitialState = ERHIAccess::DSVWrite;
		}

		ED3D12ResourceTransientMode TransientMode = ED3D12ResourceTransientMode::Transient;
		FResourceAllocatorAdapter ResourceAllocatorAdapter(GetParentAdapter(), static_cast<FD3D12TransientHeap&>(Initializer.Heap), Initializer.Allocation, Desc);
		
		FRHITexture* Texture = DynamicRHI->CreateTexture(InCreateInfo, InDebugName, InitialState, TransientMode, &ResourceAllocatorAdapter);
		return new FRHITransientTexture(Texture, Initializer.Hash, InCreateInfo);
	});
}

void FD3D12TransientResourceAllocator::FResourceAllocatorAdapter::AllocateResource(
	uint32 GPUIndex, D3D12_HEAP_TYPE, const FD3D12ResourceDesc& InDesc, uint64 InSize, uint32, ED3D12ResourceStateMode InResourceStateMode,
	D3D12_RESOURCE_STATES InCreateState, const D3D12_CLEAR_VALUE* InClearValue, const TCHAR* InName, FD3D12ResourceLocation& ResourceLocation)
{
	// The D3D12_RESOURCE_DESC's are built in two different functions right now. This checks that they actually match what we expect.
#if DO_CHECK
	{
		CD3DX12_RESOURCE_DESC CreatedDesc(InDesc);
		CD3DX12_RESOURCE_DESC DerivedDesc(Desc);
		check(CreatedDesc == DerivedDesc);
	}
#endif

	FD3D12Resource* NewResource = nullptr;
	FD3D12Adapter* Adapter = GetParentAdapter();
	VERIFYD3D12RESULT(Adapter->CreatePlacedResource(InDesc, Heap.GetLinkedObject(GPUIndex)->Get(), Allocation.Offset, InCreateState, InResourceStateMode, D3D12_RESOURCE_STATE_TBD, InClearValue, &NewResource, InName));

	check(!ResourceLocation.IsValid());
	ResourceLocation.AsHeapAliased(NewResource);
	ResourceLocation.SetSize(InSize);
	ResourceLocation.SetTransient(true);

#if TRACK_RESOURCE_ALLOCATIONS
	if (Adapter->IsTrackingAllAllocations())
	{
		bool bCollectCallstack = false;
		Adapter->TrackAllocationData(&ResourceLocation, Allocation.Size, bCollectCallstack);
	}
#endif
}

FRHITransientBuffer* FD3D12TransientResourceAllocator::CreateBuffer(const FRHIBufferCreateInfo& InCreateInfo, const TCHAR* InDebugName, uint32 InPassIndex)
{
	D3D12_RESOURCE_DESC Desc;
	uint32 Alignment;
	EBufferUsageFlags BufferUsage = InCreateInfo.Usage;
	FD3D12Buffer::GetResourceDescAndAlignment(InCreateInfo.Size, InCreateInfo.Stride, BufferUsage, Desc, Alignment);

	Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	uint64 Size = Align(Desc.Width, Alignment);

	return Allocator.CreateBuffer(InCreateInfo, InDebugName, InPassIndex, Size, Alignment,
		[&](const FRHITransientResourceAllocator::FResourceInitializer& Initializer)
	{
		ED3D12ResourceTransientMode TransientMode = ED3D12ResourceTransientMode::Transient;
		FResourceAllocatorAdapter ResourceAllocatorAdapter(GetParentAdapter(), static_cast<FD3D12TransientHeap&>(Initializer.Heap), Initializer.Allocation, Desc);
		FRHIBuffer* Buffer = FD3D12DynamicRHI::GetD3DRHI()->CreateBuffer(InCreateInfo, InDebugName, ERHIAccess::UAVMask, TransientMode, &ResourceAllocatorAdapter);
		return new FRHITransientBuffer(Buffer, Initializer.Hash, InCreateInfo);
	});
}

void FD3D12TransientResourceAllocator::DeallocateMemory(FRHITransientTexture* InTexture, uint32 InPassIndex)
{
	Allocator.DeallocateMemory(InTexture, InPassIndex);
}

void FD3D12TransientResourceAllocator::DeallocateMemory(FRHITransientBuffer* InBuffer, uint32 InPassIndex)
{
	Allocator.DeallocateMemory(InBuffer, InPassIndex);
}

void FD3D12TransientResourceAllocator::Freeze(FRHICommandListImmediate& RHICmdList, FRHITransientHeapStats& OutHeapStats)
{
	Allocator.Freeze(RHICmdList, OutHeapStats);
}
