// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "D3D12CommandList.h"

struct FD3D12OfflineHeapEntry;

struct FD3D12DescriptorHeap : public FD3D12DeviceChild, public FD3D12RefCount
{
public:
	FD3D12DescriptorHeap() = delete;

	// Heap created with its own D3D heap object.
	FD3D12DescriptorHeap(FD3D12Device* InDevice, ID3D12DescriptorHeap* InHeap, uint32 InNumDescriptors, ERHIDescriptorHeapType InType, D3D12_DESCRIPTOR_HEAP_FLAGS InFlags, bool bInIsGlobal);

	// Heap created as a suballocation of another heap.
	FD3D12DescriptorHeap(FD3D12DescriptorHeap* SubAllocateSourceHeap, uint32 InOffset, uint32 InNumDescriptors);

	~FD3D12DescriptorHeap();

	inline ID3D12DescriptorHeap*       GetHeap()  const { return Heap; }
	inline ERHIDescriptorHeapType      GetType()  const { return Type; }
	inline D3D12_DESCRIPTOR_HEAP_FLAGS GetFlags() const { return Flags; }

	inline uint32 GetOffset()         const { return Offset; }
	inline uint32 GetNumDescriptors() const { return NumDescriptors; }
	inline uint32 GetDescriptorSize() const { return DescriptorSize; }
	inline bool   IsGlobal()          const { return bIsGlobal; }
	inline bool   IsSuballocation()   const { return bIsSuballocation; }

	inline uint32 GetMemorySize() const { return DescriptorSize * NumDescriptors; }

	inline D3D12_CPU_DESCRIPTOR_HANDLE GetCPUSlotHandle(uint32 Slot) const { return CD3DX12_CPU_DESCRIPTOR_HANDLE(CpuBase, Slot, DescriptorSize); }
	inline D3D12_GPU_DESCRIPTOR_HANDLE GetGPUSlotHandle(uint32 Slot) const { return CD3DX12_GPU_DESCRIPTOR_HANDLE(GpuBase, Slot, DescriptorSize); }

private:
	TRefCountPtr<ID3D12DescriptorHeap> Heap;

	const CD3DX12_CPU_DESCRIPTOR_HANDLE CpuBase;
	const CD3DX12_GPU_DESCRIPTOR_HANDLE GpuBase;

	// Offset in descriptors into the heap, only used when heap is suballocated.
	const uint32 Offset = 0;

	// Total number of descriptors in this heap.
	const uint32 NumDescriptors;

	// Device provided size of each descriptor in this heap.
	const uint32 DescriptorSize;

	const ERHIDescriptorHeapType Type;
	const D3D12_DESCRIPTOR_HEAP_FLAGS Flags;

	// Enabled if this heap is the "global" heap.
	const bool bIsGlobal;

	// Enabled if this heap was allocated inside another heap.
	const bool bIsSuballocation;
};
using FD3D12DescriptorHeapPtr = TRefCountPtr<FD3D12DescriptorHeap>;

struct FDescriptorAllocatorRange;

class FDescriptorAllocator
{
public:
	FDescriptorAllocator();
	~FDescriptorAllocator();

	void Init(uint32 InNumDescriptors);

	bool Allocate(uint32 NumDescriptors, uint32& OutSlot);
	void Free(uint32 Slot, uint32 NumDescriptors);

private:
	TArray<FDescriptorAllocatorRange> Ranges;
	uint32 Capacity = 0;

	FCriticalSection CriticalSection;
};

/** Manager for resource descriptor allocations. */
class FD3D12DescriptorManager : public FD3D12DeviceChild
{
public:
	FD3D12DescriptorManager() = delete;
	FD3D12DescriptorManager(FD3D12Device* Device, FD3D12DescriptorHeap* InHeap);
	~FD3D12DescriptorManager();

	void Init(TCHAR* InName, ERHIDescriptorHeapType InType, uint32 InNumDescriptors);
	void Destroy();

	FRHIDescriptorHandle AllocateDescriptor();
	bool AllocateDescriptors(uint32 NumDescriptors, uint32& OutSlot);

	void FreeDescriptor(FRHIDescriptorHandle InHandle);
	void FreeDescriptors(uint32 Slot, uint32 NumDescriptors);

	inline FD3D12DescriptorHeap* GetHeap() { return Heap.GetReference(); }
	inline D3D12_CPU_DESCRIPTOR_HANDLE GetCpuDescriptorHandle(uint32 InSlot) const { return Heap->GetCPUSlotHandle(InSlot); }

	inline bool HandlesAllocation(ERHIDescriptorHeapType InHeapType) const
	{
		return Heap && Heap->GetType() == InHeapType;
	}

	inline bool HandlesAllocation(ERHIDescriptorHeapType InHeapType, D3D12_DESCRIPTOR_HEAP_FLAGS InHeapFlags) const
	{
		return HandlesAllocation(InHeapType) && Heap->GetFlags() == InHeapFlags;
	}

	inline bool IsHeapAChild(const FD3D12DescriptorHeap* InHeap)
	{
		return InHeap->GetHeap() == Heap->GetHeap();
	}

private:
	FD3D12DescriptorHeapPtr Heap;
	FDescriptorAllocator Allocator;
};

/** Manager for resource descriptors used in bindless rendering. */
class FD3D12BindlessDescriptorManager : public FD3D12DeviceChild
{
public:
	FD3D12BindlessDescriptorManager(FD3D12Device* Device);

	void Init(uint32 InNumResourceDescriptors, uint32 InNumSamplerDescriptors);

	FRHIDescriptorHandle AllocateDescriptor(ERHIDescriptorHeapType InType);
	void FreeDescriptor(FRHIDescriptorHandle InHandle);

	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuDescriptorHandle(FRHIDescriptorHandle InHandle) const;

private:
	TArray<FD3D12DescriptorManager> Managers;
};

/** Heap sub block of an online heap */
struct FD3D12OnlineDescriptorBlock
{
	FD3D12OnlineDescriptorBlock() = delete;
	FD3D12OnlineDescriptorBlock(uint32 InBaseSlot, uint32 InSize)
		: BaseSlot(InBaseSlot)
		, Size(InSize)
	{
	}

	uint32 BaseSlot;
	uint32 Size;
	uint32 SizeUsed = 0;
	FD3D12CLSyncPoint SyncPoint;
};

/** Primary online heap from which sub blocks can be allocated and freed. Used when allocating blocks of descriptors for tables. */
class FD3D12OnlineDescriptorManager : public FD3D12DeviceChild
{
public:
	FD3D12OnlineDescriptorManager(FD3D12Device* Device);
	~FD3D12OnlineDescriptorManager();

	// Setup the actual heap
	void Init(uint32 InTotalSize, uint32 InBlockSize);

	// Allocate an available sub heap block from the global heap
	FD3D12OnlineDescriptorBlock* AllocateHeapBlock();
	void FreeHeapBlock(FD3D12OnlineDescriptorBlock* InHeapBlock);

	ID3D12DescriptorHeap* GetHeap() { return Heap->GetHeap(); }
	FD3D12DescriptorHeap* GetDescriptorHeap() { return Heap.GetReference(); }

	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUSlotHandle(FD3D12OnlineDescriptorBlock* InBlock) const { return Heap->GetCPUSlotHandle(InBlock->BaseSlot); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUSlotHandle(FD3D12OnlineDescriptorBlock* InBlock) const { return Heap->GetGPUSlotHandle(InBlock->BaseSlot); }

private:
	// Check all released blocks and check which ones are not used by the GPU anymore
	void UpdateFreeBlocks();

	FD3D12DescriptorHeapPtr Heap;

	TQueue<FD3D12OnlineDescriptorBlock*> FreeBlocks;
	TArray<FD3D12OnlineDescriptorBlock*> ReleasedBlocks;

	FCriticalSection CriticalSection;
};

/** Manages and allows allocations of CPU descriptors only. Creates small heaps on demand to satisfy allocations. */
class FD3D12OfflineDescriptorManager : public FD3D12DeviceChild
{
public:
	FD3D12OfflineDescriptorManager() = delete;
	FD3D12OfflineDescriptorManager(FD3D12Device* InDevice);
	~FD3D12OfflineDescriptorManager();

	inline ERHIDescriptorHeapType GetHeapType() const { return HeapType; }

	void Init(ERHIDescriptorHeapType InHeapType);

	D3D12_CPU_DESCRIPTOR_HANDLE AllocateHeapSlot(uint32& outIndex);
	void FreeHeapSlot(D3D12_CPU_DESCRIPTOR_HANDLE Offset, uint32 index);

private:
	void AllocateHeap();

	TArray<FD3D12OfflineHeapEntry> Heaps;
	TDoubleLinkedList<uint32> FreeHeaps;

	ERHIDescriptorHeapType HeapType;
	uint32 NumDescriptorsPerHeap{};
	uint32 DescriptorSize{};

	FCriticalSection CriticalSection;
};

/** Primary descriptor heap and descriptor manager. All heap allocations come from here.
	All GPU visible resource heap allocations will be sub-allocated from a single heap in this manager. */
class FD3D12DescriptorHeapManager : public FD3D12DeviceChild
{
public:
	FD3D12DescriptorHeapManager(FD3D12Device* InDevice);
	~FD3D12DescriptorHeapManager();

	void Init(uint32 InNumGlobalResourceDescriptors, uint32 InNumGlobalSamplerDescriptors);
	void Destroy();

	FD3D12DescriptorHeap* AllocateHeap(const TCHAR* DebugName, ERHIDescriptorHeapType InHeapType, uint32 InNumDescriptors, D3D12_DESCRIPTOR_HEAP_FLAGS InFlags);
	void FreeHeap(FD3D12DescriptorHeap* InHeap);

	//inline FD3D12DescriptorHeap* GetGlobalHeap() const { return GlobalHeap; }

private:
	TArray<FD3D12DescriptorManager> GlobalHeaps;
};

