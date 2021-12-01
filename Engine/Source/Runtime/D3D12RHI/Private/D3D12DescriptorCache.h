// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

class FD3D12DynamicRHI;
class FD3D12DescriptorCache;
struct FD3D12VertexBufferCache;
struct FD3D12IndexBufferCache;
struct FD3D12ConstantBufferCache;
struct FD3D12ShaderResourceViewCache;
struct FD3D12UnorderedAccessViewCache;
struct FD3D12SamplerStateCache;

// Like a TMap<KeyType, ValueType>
// Faster lookup performance, but possibly has false negatives
template<typename KeyType, typename ValueType>
class FD3D12ConservativeMap
{
public:
	FD3D12ConservativeMap(uint32 Size)
	{
		Table.AddUninitialized(Size);

		Reset();
	}

	void Add(const KeyType& Key, const ValueType& Value)
	{
		uint32 Index = GetIndex(Key);

		Entry& Pair = Table[Index];

		Pair.Valid = true;
		Pair.Key = Key;
		Pair.Value = Value;
	}

	ValueType* Find(const KeyType& Key)
	{
		uint32 Index = GetIndex(Key);

		Entry& Pair = Table[Index];

		if (Pair.Valid &&
			(Pair.Key == Key))
		{
			return &Pair.Value;
		}
		else
		{
			return nullptr;
		}
	}

	void Reset()
	{
		for (int32 i = 0; i < Table.Num(); i++)
		{
			Table[i].Valid = false;
		}
	}

private:
	uint32 GetIndex(const KeyType& Key)
	{
		uint32 Hash = GetTypeHash(Key);

		return Hash % static_cast<uint32>(Table.Num());
	}

	struct Entry
	{
		bool Valid;
		KeyType Key;
		ValueType Value;
	};

	TArray<Entry> Table;
};

uint32 GetTypeHash(const D3D12_SAMPLER_DESC& Desc);
struct FD3D12SamplerArrayDesc
{
	uint32 Count;
	uint16 SamplerID[16];
	inline bool operator==(const FD3D12SamplerArrayDesc& rhs) const
	{
		check(Count <= UE_ARRAY_COUNT(SamplerID));
		check(rhs.Count <= UE_ARRAY_COUNT(rhs.SamplerID));

		if (Count != rhs.Count)
		{
			return false;
		}
		else
		{
			// It is safe to compare pointers, because samplers are kept alive for the lifetime of the RHI
			return 0 == FMemory::Memcmp(SamplerID, rhs.SamplerID, sizeof(SamplerID[0]) * Count);
		}
	}
};
uint32 GetTypeHash(const FD3D12SamplerArrayDesc& Key);
typedef FD3D12ConservativeMap<FD3D12SamplerArrayDesc, D3D12_GPU_DESCRIPTOR_HANDLE> FD3D12SamplerMap;


template< uint32 CPUTableSize>
struct FD3D12UniqueDescriptorTable
{
	FD3D12UniqueDescriptorTable() = default;
	FD3D12UniqueDescriptorTable(FD3D12SamplerArrayDesc KeyIn, D3D12_CPU_DESCRIPTOR_HANDLE* Table)
	{
		FMemory::Memcpy(&Key, &KeyIn, sizeof(Key));//Memcpy to avoid alignement issues
		FMemory::Memcpy(CPUTable, Table, Key.Count * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
	}

	FORCEINLINE uint32 GetTypeHash(const FD3D12UniqueDescriptorTable& Table)
	{
		return FD3D12PipelineStateCache::HashData((void*)Table.Key.SamplerID, Table.Key.Count * sizeof(Table.Key.SamplerID[0]));
	}

	FD3D12SamplerArrayDesc Key{};
	D3D12_CPU_DESCRIPTOR_HANDLE CPUTable[MAX_SAMPLERS]{};

	// This will point to the table start in the global heap
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle{};
};

template<typename FD3D12UniqueDescriptorTable, bool bInAllowDuplicateKeys = false>
struct FD3D12UniqueDescriptorTableKeyFuncs : BaseKeyFuncs<FD3D12UniqueDescriptorTable, FD3D12UniqueDescriptorTable, bInAllowDuplicateKeys>
{
	typedef typename TCallTraits<FD3D12UniqueDescriptorTable>::ParamType KeyInitType;
	typedef typename TCallTraits<FD3D12UniqueDescriptorTable>::ParamType ElementInitType;

	/**
	* @return The key used to index the given element.
	*/
	static FORCEINLINE KeyInitType GetSetKey(ElementInitType Element)
	{
		return Element;
	}

	/**
	* @return True if the keys match.
	*/
	static FORCEINLINE bool Matches(KeyInitType A, KeyInitType B)
	{
		return A.Key == B.Key;
	}

	/** Calculates a hash index for a key. */
	static FORCEINLINE uint32 GetKeyHash(KeyInitType Key)
	{
		return GetTypeHash(Key.Key);
	}
};

typedef FD3D12UniqueDescriptorTable<MAX_SAMPLERS> FD3D12UniqueSamplerTable;

typedef TSet<FD3D12UniqueSamplerTable, FD3D12UniqueDescriptorTableKeyFuncs<FD3D12UniqueSamplerTable>> FD3D12SamplerSet;

/** Manages a D3D heap which is GPU visible - base class which can be used by the FD3D12DescriptorCache */
class FD3D12OnlineHeap : public FD3D12DeviceChild
{
public:
	FD3D12OnlineHeap(FD3D12Device* Device, bool CanLoopAround);
	virtual ~FD3D12OnlineHeap();

	ID3D12DescriptorHeap* GetHeap() { return Heap->GetHeap(); }

	FORCEINLINE D3D12_CPU_DESCRIPTOR_HANDLE GetCPUSlotHandle(uint32 Slot) const { return Heap->GetCPUSlotHandle(Slot); }
	FORCEINLINE D3D12_GPU_DESCRIPTOR_HANDLE GetGPUSlotHandle(uint32 Slot) const { return Heap->GetGPUSlotHandle(Slot); }

	// Call this to reserve descriptor heap slots for use by the command list you are currently recording. This will wait if
	// necessary until slots are free (if they are currently in use by another command list.) If the reservation can be
	// fulfilled, the index of the first reserved slot is returned (all reserved slots are consecutive.) If not, it will 
	// throw an exception.
	bool CanReserveSlots(uint32 NumSlots);
	uint32 ReserveSlots(uint32 NumSlotsRequested);

	void SetNextSlot(uint32 NextSlot);
	uint32 GetNextSlotIndex() const { return NextSlotIndex;  }

	// Function which can/should be implemented by the derived classes
	virtual bool RollOver() = 0;
	virtual void HeapLoopedAround() { }
	virtual void SetCurrentCommandList(const FD3D12CommandListHandle& CommandListHandle) { }
	virtual uint32 GetTotalSize() { return Heap->GetNumDescriptors(); }

	static const uint32 HeapExhaustedValue = uint32(-1);

protected:
	// Keeping this ptr around is basically just for lifetime management
	TRefCountPtr<FD3D12DescriptorHeap> Heap;

	// This index indicate where the next set of descriptors should be placed *if* there's room
	uint32 NextSlotIndex = 0;

	// Indicates the last free slot marked by the command list being finished
	uint32 FirstUsedSlot = 0;

	// Does the heap support loop around allocations
	const bool bCanLoopAround;
};

/** Global sampler heap managed by the device which stored a unique set of sampler sets */
class FD3D12GlobalOnlineSamplerHeap : public FD3D12OnlineHeap
{
public:
	FD3D12GlobalOnlineSamplerHeap(FD3D12Device* Device);
	~FD3D12GlobalOnlineSamplerHeap();

	void Init(uint32 TotalSize);

	void ToggleDescriptorTablesDirtyFlag(bool Value) { bUniqueDescriptorTablesAreDirty = Value; }
	bool DescriptorTablesDirty() { return bUniqueDescriptorTablesAreDirty; }
	FD3D12SamplerSet& GetUniqueDescriptorTables() { return UniqueDescriptorTables; }
	FCriticalSection& GetCriticalSection() { return CriticalSection; }

	// Override FD3D12OnlineHeap functions
	virtual bool RollOver() final override;

private:
	FD3D12SamplerSet UniqueDescriptorTables;
	FCriticalSection CriticalSection;
	bool bUniqueDescriptorTablesAreDirty = false;
};

/** Online heap which can be used by a FD3D12DescriptorCache to manage a block allocated from a GlobalHeap */
class FD3D12SubAllocatedOnlineHeap : public FD3D12OnlineHeap
{
public:
	FD3D12SubAllocatedOnlineHeap(FD3D12DescriptorCache* InDescriptorCache);
	~FD3D12SubAllocatedOnlineHeap();

	// Setup the online heap data
	void Init(FD3D12Device* InParent);

	// Override FD3D12OnlineHeap functions
	virtual bool RollOver() final override;
	virtual void SetCurrentCommandList(const FD3D12CommandListHandle& CommandListHandle) final override;
	virtual uint32 GetTotalSize() final override
	{
		return CurrentBlock ? CurrentBlock->Size : 0;
	}

private:
	// Allocate a new block from the global heap - return true if allocation succeeds
	bool AllocateBlock();

	FD3D12OnlineDescriptorBlock* CurrentBlock = nullptr;

	FD3D12DescriptorCache* DescriptorCache = nullptr;
	FD3D12CommandListHandle CurrentCommandList;
};


/** Online heap which is not shared between multiple FD3D12DescriptorCache.
 *  Used as overflow heap when the global heaps are full or don't contain the required data
 */
class FD3D12LocalOnlineHeap : public FD3D12OnlineHeap
{
public:
	FD3D12LocalOnlineHeap(FD3D12DescriptorCache* InDescriptorCache);
	~FD3D12LocalOnlineHeap();

	// Allocate the actual overflow heap
	void Init(FD3D12Device* InParent, uint32 InNumDescriptors, ERHIDescriptorHeapType InHeapType);

	// Override FD3D12OnlineHeap functions
	virtual bool RollOver() final override;
	virtual void HeapLoopedAround() final override;
	virtual void SetCurrentCommandList(const FD3D12CommandListHandle& CommandListHandle) final override;

private:
	struct SyncPointEntry
	{
		FD3D12CLSyncPoint SyncPoint;
		uint32 LastSlotInUse;

		SyncPointEntry() : LastSlotInUse(0)
		{}

		SyncPointEntry(const SyncPointEntry& InSyncPoint) : SyncPoint(InSyncPoint.SyncPoint), LastSlotInUse(InSyncPoint.LastSlotInUse)
		{}

		SyncPointEntry& operator = (const SyncPointEntry& InSyncPoint)
		{
			SyncPoint = InSyncPoint.SyncPoint;
			LastSlotInUse = InSyncPoint.LastSlotInUse;

			return *this;
		}
	};
	TQueue<SyncPointEntry> SyncPoints;

	struct PoolEntry
	{
		TRefCountPtr<FD3D12DescriptorHeap> Heap;
		FD3D12CLSyncPoint SyncPoint;

		PoolEntry() 
		{}

		PoolEntry(const PoolEntry& InPoolEntry) : Heap(InPoolEntry.Heap), SyncPoint(InPoolEntry.SyncPoint)
		{}

		PoolEntry& operator = (const PoolEntry& InPoolEntry)
		{
			Heap = InPoolEntry.Heap;
			SyncPoint = InPoolEntry.SyncPoint;
			return *this;
		}
	};
	PoolEntry Entry;
	TQueue<PoolEntry> ReclaimPool;

	FD3D12DescriptorCache* DescriptorCache;
	FD3D12CommandListHandle CurrentCommandList;
};

class FD3D12DescriptorCache : public FD3D12DeviceChild, public FD3D12SingleNodeGPUObject
{
protected:
	FD3D12CommandContext* CmdContext;

public:
	FD3D12OnlineHeap* GetCurrentViewHeap() { return CurrentViewHeap; }
	FD3D12OnlineHeap* GetCurrentSamplerHeap() { return CurrentSamplerHeap; }

	FD3D12DescriptorCache(FRHIGPUMask Node);

	~FD3D12DescriptorCache()
	{
		if (LocalViewHeap) { delete(LocalViewHeap); }
	}

	// Checks if the specified descriptor heap has been set on the current command list.
	bool IsHeapSet(ID3D12DescriptorHeap* const pHeap) const
	{
		return (pHeap == pPreviousViewHeap) || (pHeap == pPreviousSamplerHeap);
	}

	// Notify the descriptor cache every time you start recording a command list.
	// This sets descriptor heaps on the command list and indicates the current fence value which allows
	// us to avoid querying DX12 for that value thousands of times per frame, which can be costly.
	D3D12RHI_API void SetCurrentCommandList(const FD3D12CommandListHandle& CommandListHandle);

	// ------------------------------------------------------
	// end Descriptor Slot Reservation stuff

	// null views

	FD3D12ViewDescriptorHandle* NullSRV{};
	FD3D12ViewDescriptorHandle* NullRTV{};
	FD3D12ViewDescriptorHandle* NullUAV{};

#if USE_STATIC_ROOT_SIGNATURE
	FD3D12ConstantBufferView* NullCBV{};
#endif
	TRefCountPtr<FD3D12SamplerState> DefaultSampler;

	void SetVertexBuffers(FD3D12VertexBufferCache& Cache);
	void SetRenderTargets(FD3D12RenderTargetView** RenderTargetViewArray, uint32 Count, FD3D12DepthStencilView* DepthStencilTarget);

	template <EShaderFrequency ShaderStage>
	void SetUAVs(const FD3D12RootSignature* RootSignature, FD3D12UnorderedAccessViewCache& Cache, const UAVSlotMask& SlotsNeededMask, uint32 Count, uint32 &HeapSlot);

	template <EShaderFrequency ShaderStage>
	void SetSamplers(const FD3D12RootSignature* RootSignature, FD3D12SamplerStateCache& Cache, const SamplerSlotMask& SlotsNeededMask, uint32 Count, uint32& HeapSlot);

	template <EShaderFrequency ShaderStage>
	void SetSRVs(const FD3D12RootSignature* RootSignature, FD3D12ShaderResourceViewCache& Cache, const SRVSlotMask& SlotsNeededMask, uint32 Count, uint32& HeapSlot);

	template <EShaderFrequency ShaderStage> 
#if USE_STATIC_ROOT_SIGNATURE
	void SetConstantBuffers(const FD3D12RootSignature* RootSignature, FD3D12ConstantBufferCache& Cache, const CBVSlotMask& SlotsNeededMask, uint32 Count, uint32& HeapSlot);
#else
	void SetConstantBuffers(const FD3D12RootSignature* RootSignature, FD3D12ConstantBufferCache& Cache, const CBVSlotMask& SlotsNeededMask);
#endif

	void SetStreamOutTargets(FD3D12Resource **Buffers, uint32 Count, const uint32* Offsets);

	bool HeapRolledOver(ERHIDescriptorHeapType InHeapType);
	void HeapLoopedAround(ERHIDescriptorHeapType InHeapType);
	void Init(FD3D12Device* InParent, FD3D12CommandContext* InCmdContext, uint32 InNumLocalViewDescriptors, uint32 InNumSamplerDescriptors);
	void Clear();
	void BeginFrame();
	void EndFrame();
	void GatherUniqueSamplerTables();

	bool SwitchToContextLocalViewHeap(const FD3D12CommandListHandle& CommandListHandle);
	bool SwitchToContextLocalSamplerHeap();
	bool SwitchToGlobalSamplerHeap();

	TArray<FD3D12UniqueSamplerTable>& GetUniqueTables() { return UniqueTables; }

	inline bool UsingGlobalSamplerHeap() const { return bUsingGlobalSamplerHeap; }
	FD3D12SamplerSet& GetLocalSamplerSet() { return LocalSamplerSet; }

private:
	// Sets the current descriptor tables on the command list and marks any descriptor tables as dirty if necessary.
	// Returns true if one of the heaps actually changed, false otherwise.
	bool SetDescriptorHeaps();

	// The previous view and sampler heaps set on the current command list.
	ID3D12DescriptorHeap* pPreviousViewHeap;
	ID3D12DescriptorHeap* pPreviousSamplerHeap;

	FD3D12OnlineHeap* CurrentViewHeap;
	FD3D12OnlineHeap* CurrentSamplerHeap;

	FD3D12LocalOnlineHeap* LocalViewHeap;
	FD3D12LocalOnlineHeap LocalSamplerHeap;
	FD3D12SubAllocatedOnlineHeap SubAllocatedViewHeap;

	FD3D12SamplerMap SamplerMap;

	TArray<FD3D12UniqueSamplerTable> UniqueTables;

	FD3D12SamplerSet LocalSamplerSet;
	bool bUsingGlobalSamplerHeap;

	uint32 NumLocalViewDescriptors;
};