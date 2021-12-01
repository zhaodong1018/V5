// Copyright Epic Games, Inc. All Rights Reserved.

#include "DerivedDataBackendAsyncPutWrapper.h"

#include "DerivedDataPayload.h"
#include "DerivedDataRequest.h"
#include "DerivedDataRequestOwner.h"
#include "Experimental/Async/LazyEvent.h"
#include "MemoryDerivedDataBackend.h"
#include "Tasks/Task.h"

namespace UE::DerivedData::Backends
{

/** 
 * Async task to handle the fire and forget async put
 */
class FCachePutAsyncWorker
{
public:
	/** Cache Key for the put to InnerBackend **/
	FString								CacheKey;
	/** Data for the put to InnerBackend **/
	TArray<uint8>						Data;
	/** Backend to use for storage, my responsibilities are about async puts **/
	FDerivedDataBackendInterface*		InnerBackend;
	/** Memory based cache to clear once the put is finished **/
	FDerivedDataBackendInterface*		InflightCache;
	/** We remember outstanding puts so that we don't do them redundantly **/
	FThreadSet*							FilesInFlight;
	/**If true, then do not attempt skip the put even if CachedDataProbablyExists returns true **/
	bool								bPutEvenIfExists;
	/** Usage stats to track thread times. */
	FDerivedDataCacheUsageStats&        UsageStats;

	/** Constructor
	*/
	FCachePutAsyncWorker(const TCHAR* InCacheKey, TArrayView<const uint8> InData, FDerivedDataBackendInterface* InInnerBackend, bool InbPutEvenIfExists, FDerivedDataBackendInterface* InInflightCache, FThreadSet* InInFilesInFlight, FDerivedDataCacheUsageStats& InUsageStats)
		: CacheKey(InCacheKey)
		, InnerBackend(InInnerBackend)
		, InflightCache(InInflightCache)
		, FilesInFlight(InInFilesInFlight)
		, bPutEvenIfExists(InbPutEvenIfExists)
		, UsageStats(InUsageStats)
	{
		// Only make a copy if it's not going to be available from the Inflight cache
		if (!InInflightCache || !InInflightCache->CachedDataProbablyExists(InCacheKey))
		{
			Data = InData;
		}
		check(InnerBackend);
	}

	bool ShouldAbortForShutdown()
	{
		using ESpeedClass = FDerivedDataBackendInterface::ESpeedClass;
		ESpeedClass SpeedClass = InnerBackend->GetSpeedClass();
		if (SpeedClass == ESpeedClass::Local)
		{
			return false;
		}
		return !GIsBuildMachine && FDerivedDataBackend::Get().IsShuttingDown();
	}
		
	/** Call the inner backend and when that completes, remove the memory cache */
	void DoWork()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DDCPut_DoWork);
		COOK_STAT(auto Timer = UsageStats.TimePut());

		if (ShouldAbortForShutdown())
		{
			Abandon();
			return;
		}

		using EPutStatus = FDerivedDataBackendInterface::EPutStatus;
		EPutStatus Status = EPutStatus::NotCached;

		if (!bPutEvenIfExists && InnerBackend->CachedDataProbablyExists(*CacheKey))
		{
			Status = EPutStatus::Cached;
		}
		else
		{
			if (InflightCache && Data.Num() == 0)
			{
				// We verified at construction time that we would be able to get the data from the Inflight cache
				verify(InflightCache->GetCachedData(*CacheKey, Data));
			}
			Status = InnerBackend->PutCachedData(*CacheKey, Data, bPutEvenIfExists);
			COOK_STAT(Timer.AddHit(Data.Num()));
		}

		if (InflightCache)
		{
			// if the data was not cached synchronously, retry
			if (Status != EPutStatus::Cached && Status != EPutStatus::Skipped)
			{
				// retry after a brief wait
				FPlatformProcess::SleepNoStats(0.2f);

				if (Status == EPutStatus::Executing && InnerBackend->CachedDataProbablyExists(*CacheKey))
				{
					Status = EPutStatus::Cached;
				}
				else
				{
					if (Data.Num() == 0)
					{
						verify(InflightCache->GetCachedData(*CacheKey, Data));
					}
					Status = InnerBackend->PutCachedData(*CacheKey, Data, /*bPutEvenIfExists*/ false);
				}
			}

			switch (Status)
			{
			case EPutStatus::Skipped:
			case EPutStatus::Cached:
				// remove this from the in-flight cache because the inner cache contains the data or it was intentionally skipped
				InflightCache->RemoveCachedData(*CacheKey, /*bTransient*/ false);
				break;
			case EPutStatus::NotCached:
				UE_LOG(LogDerivedDataCache, Display, TEXT("%s: Put failed, keeping in memory copy %s."), *InnerBackend->GetName(), *CacheKey);
				if (uint32 ErrorCode = FPlatformMisc::GetLastError())
				{
					TCHAR ErrorBuffer[1024];
					FPlatformMisc::GetSystemErrorMessage(ErrorBuffer, 1024, ErrorCode);
					UE_LOG(LogDerivedDataCache, Display, TEXT("Failed to write %s to %s. Error: %u (%s)"), *CacheKey, *InnerBackend->GetName(), ErrorCode, ErrorBuffer);
				}
				break;
			case EPutStatus::Executing:
				UE_LOG(LogDerivedDataCache, Display, TEXT("%s: Put not finished executing, keeping in memory copy %s."), *InnerBackend->GetName(), *CacheKey);
				break;
			default:
				break;
			}
		}

		FilesInFlight->Remove(CacheKey);
		FDerivedDataBackend::Get().AddToAsyncCompletionCounter(-1);
		UE_LOG(LogDerivedDataCache, VeryVerbose, TEXT("%s: Completed AsyncPut of %s."), *InnerBackend->GetName(), *CacheKey);
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FCachePutAsyncWorker, STATGROUP_ThreadPoolAsyncTasks);
	}

	/** Indicates to the thread pool that this task is abandonable */
	bool CanAbandon()
	{
		return true;
	}

	/** Abandon routine, we need to remove the item from the in flight cache because something might be waiting for that */
	void Abandon()
	{
		if (InflightCache)
		{
			InflightCache->RemoveCachedData(*CacheKey, /*bTransient=*/ false); // we can remove this from the temp cache, since the real cache will hit now
		}
		FilesInFlight->Remove(CacheKey);
		FDerivedDataBackend::Get().AddToAsyncCompletionCounter(-1);
		UE_LOG(LogDerivedDataCache, VeryVerbose, TEXT("%s: Abandoned AsyncPut of %s."), *InnerBackend->GetName(), *CacheKey);
	}
};

FDerivedDataBackendAsyncPutWrapper::FDerivedDataBackendAsyncPutWrapper(FDerivedDataBackendInterface* InInnerBackend, bool bCacheInFlightPuts)
	: InnerBackend(InInnerBackend)
	, InflightCache(bCacheInFlightPuts ? (new FMemoryDerivedDataBackend(TEXT("InflightMemoryCache"))) : NULL)
{
	check(InnerBackend);
}

/** return true if this cache is writable **/
bool FDerivedDataBackendAsyncPutWrapper::IsWritable() const
{
	return InnerBackend->IsWritable();
}

FDerivedDataBackendInterface::ESpeedClass FDerivedDataBackendAsyncPutWrapper::GetSpeedClass() const
{
	return InnerBackend->GetSpeedClass();
}

bool FDerivedDataBackendAsyncPutWrapper::BackfillLowerCacheLevels() const
{
	return InnerBackend->BackfillLowerCacheLevels();
}

bool FDerivedDataBackendAsyncPutWrapper::CachedDataProbablyExists(const TCHAR* CacheKey)
{
	COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());
	bool Result = (InflightCache && InflightCache->CachedDataProbablyExists(CacheKey)) || InnerBackend->CachedDataProbablyExists(CacheKey);
	COOK_STAT(if (Result) {	Timer.AddHit(0); });

	UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s CachedDataProbablyExists=%d for %s"), *GetName(), Result, CacheKey);
	return Result;
}

TBitArray<> FDerivedDataBackendAsyncPutWrapper::CachedDataProbablyExistsBatch(TConstArrayView<FString> CacheKeys)
{
	COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());

	TBitArray<> Result;
	if (InflightCache)
	{
		Result = InflightCache->CachedDataProbablyExistsBatch(CacheKeys);
		check(Result.Num() == CacheKeys.Num());
		if (Result.CountSetBits() < CacheKeys.Num())
		{
			TBitArray<> InnerResult = InnerBackend->CachedDataProbablyExistsBatch(CacheKeys);
			check(InnerResult.Num() == CacheKeys.Num());
			Result.CombineWithBitwiseOR(InnerResult, EBitwiseOperatorFlags::MaintainSize);
		}
	}
	else
	{
		Result = InnerBackend->CachedDataProbablyExistsBatch(CacheKeys);
		check(Result.Num() == CacheKeys.Num());
	}

	COOK_STAT(if (Result.CountSetBits() == CacheKeys.Num()) { Timer.AddHit(0); });
	UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s CachedDataProbablyExists found %d/%d keys"), *GetName(), Result.CountSetBits(), CacheKeys.Num());
	return Result;
}

bool FDerivedDataBackendAsyncPutWrapper::TryToPrefetch(TConstArrayView<FString> CacheKeys)
{
	COOK_STAT(auto Timer = UsageStats.TimePrefetch());

	if ((InflightCache && InflightCache->CachedDataProbablyExistsBatch(CacheKeys).CountSetBits() == CacheKeys.Num()) ||
		InnerBackend->TryToPrefetch(CacheKeys))
	{
		COOK_STAT(Timer.AddHit(0));
		return true;
	}

	return false;
}

/*
	Determine if we would cache this by asking all our inner layers
*/
bool FDerivedDataBackendAsyncPutWrapper::WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData)
{
	return InnerBackend->WouldCache(CacheKey, InData);
}

bool FDerivedDataBackendAsyncPutWrapper::ApplyDebugOptions(FBackendDebugOptions& InOptions)
{
	return InnerBackend->ApplyDebugOptions(InOptions);
}

bool FDerivedDataBackendAsyncPutWrapper::GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData)
{
	COOK_STAT(auto Timer = UsageStats.TimeGet());
	if (InflightCache && InflightCache->GetCachedData(CacheKey, OutData))
	{
		COOK_STAT(Timer.AddHit(OutData.Num()));
		UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s CacheHit from InFlightCache on %s"), *GetName(), CacheKey);
		return true;
	}

	bool bSuccess = InnerBackend->GetCachedData(CacheKey, OutData);
	if (bSuccess)
	{
		UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s Cache hit on %s"), *GetName(), CacheKey);
		COOK_STAT(Timer.AddHit(OutData.Num()));
	}
	else
	{
		UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s Cache miss on %s"), *GetName(), CacheKey);
	}
	return bSuccess;
}

FDerivedDataBackendInterface::EPutStatus FDerivedDataBackendAsyncPutWrapper::PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists)
{
	COOK_STAT(auto Timer = PutSyncUsageStats.TimePut());

	if (!InnerBackend->IsWritable())
	{
		return EPutStatus::NotCached; // no point in continuing down the chain
	}
	const bool bAdded = FilesInFlight.AddIfNotExists(CacheKey);
	if (!bAdded)
	{
		return EPutStatus::Executing; // if it is already on its way, we don't need to send it again
	}
	if (InflightCache)
	{
		if (InflightCache->CachedDataProbablyExists(CacheKey))
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s skipping out of key already in in-flight cache %s"), *GetName(), CacheKey);
			return EPutStatus::Executing; // if it is already on its way, we don't need to send it again
		}
		InflightCache->PutCachedData(CacheKey, InData, true); // temp copy stored in memory while the async task waits to complete
		COOK_STAT(Timer.AddHit(InData.Num()));
	}
	
	UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s queueing %s for put"), *GetName(), CacheKey);

	FDerivedDataBackend::Get().AddToAsyncCompletionCounter(1);
	(new FAutoDeleteAsyncTask<FCachePutAsyncWorker>(CacheKey, InData, InnerBackend, bPutEvenIfExists, InflightCache.Get(), &FilesInFlight, UsageStats))->StartBackgroundTask(GDDCIOThreadPool, EQueuedWorkPriority::Low);

	return EPutStatus::Executing;
}

void FDerivedDataBackendAsyncPutWrapper::RemoveCachedData(const TCHAR* CacheKey, bool bTransient)
{
	if (!InnerBackend->IsWritable())
	{
		return; // no point in continuing down the chain
	}
	while (FilesInFlight.Exists(CacheKey))
	{
		FPlatformProcess::Sleep(0.0f); // this is an exception condition (corruption), spin and wait for it to clear
	}
	if (InflightCache)
	{
		InflightCache->RemoveCachedData(CacheKey, bTransient);
	}
	InnerBackend->RemoveCachedData(CacheKey, bTransient);

	UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s removed %s"), *GetName(), CacheKey)
}

TSharedRef<FDerivedDataCacheStatsNode> FDerivedDataBackendAsyncPutWrapper::GatherUsageStats() const
{
	TSharedRef<FDerivedDataCacheStatsNode> Usage = MakeShared<FDerivedDataCacheStatsNode>(this, TEXT("AsyncPutWrapper"));
	Usage->Stats.Add(TEXT("AsyncPut"), UsageStats);
	Usage->Stats.Add(TEXT("AsyncPutSync"), PutSyncUsageStats);

	if (InnerBackend)
	{
		Usage->Children.Add(InnerBackend->GatherUsageStats());
	}
	if (InflightCache)
	{
		Usage->Children.Add(InflightCache->GatherUsageStats());
	}

	return Usage;
}

class FDerivedDataAsyncWrapperRequest final : public FRequestBase, private IQueuedWork
{
public:
	inline FDerivedDataAsyncWrapperRequest(IRequestOwner& InOwner, TUniqueFunction<void (bool bCancel)>&& InFunction)
		: Owner(InOwner)
		, Function(MoveTemp(InFunction))
	{
	}

	inline void Start(EPriority Priority)
	{
		FDerivedDataBackend::Get().AddToAsyncCompletionCounter(1);
		Owner.Begin(this);

		DoneEvent.Reset();
		GDDCIOThreadPool->AddQueuedWork(this, GetPriority(Priority));
	}

	inline void Execute(bool bCancel)
	{
		FScopeCycleCounter Scope(GetStatId(), /*bAlways*/ true);
		Owner.End(this, [this, bCancel]
		{
			Function(bCancel);
			DoneEvent.Trigger();
		});
		// DO NOT ACCESS ANY MEMBERS PAST THIS POINT!
		FDerivedDataBackend::Get().AddToAsyncCompletionCounter(-1);
	}

	// IRequest Interface

	inline void SetPriority(EPriority Priority) final
	{
		if (GDDCIOThreadPool->RetractQueuedWork(this))
		{
			GDDCIOThreadPool->AddQueuedWork(this, GetPriority(Priority));
		}
	}

	inline void Cancel() final
	{
		if (!DoneEvent.Wait(0))
		{
			if (GDDCIOThreadPool->RetractQueuedWork(this))
			{
				Abandon();
			}
			else
			{
				FScopeCycleCounter Scope(GetStatId());
				DoneEvent.Wait();
			}
		}
	}

	inline void Wait() final
	{
		if (!DoneEvent.Wait(0))
		{
			if (GDDCIOThreadPool->RetractQueuedWork(this))
			{
				DoThreadedWork();
			}
			else
			{
				FScopeCycleCounter Scope(GetStatId());
				DoneEvent.Wait();
			}
		}
	}

private:
	static EQueuedWorkPriority GetPriority(EPriority Priority)
	{
		switch (Priority)
		{
		case EPriority::Blocking: return EQueuedWorkPriority::Highest;
		case EPriority::Highest:  return EQueuedWorkPriority::Highest;
		case EPriority::High:     return EQueuedWorkPriority::High;
		case EPriority::Normal:   return EQueuedWorkPriority::Normal;
		case EPriority::Low:      return EQueuedWorkPriority::Low;
		case EPriority::Lowest:   return EQueuedWorkPriority::Lowest;
		default: checkNoEntry();  return EQueuedWorkPriority::Normal;
		}
	}

	// IQueuedWork Interface

	inline void DoThreadedWork() final
	{
		Execute(/*bCancel*/ false);
	}

	inline void Abandon() final
	{
		Execute(/*bCancel*/ true);
	}

	inline TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FDerivedDataAsyncWrapperRequest, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	IRequestOwner& Owner;
	TUniqueFunction<void (bool bCancel)> Function;
	FLazyEvent DoneEvent{EEventMode::ManualReset};
};

void FDerivedDataBackendAsyncPutWrapper::Put(
	TConstArrayView<FCacheRecord> Records,
	FStringView Context,
	ECachePolicy Policy,
	IRequestOwner& Owner,
	FOnCachePutComplete&& OnComplete)
{
	if (Owner.GetPriority() == EPriority::Blocking || !GDDCIOThreadPool)
	{
		InnerBackend->Put(Records, Context, Policy, Owner, MoveTemp(OnComplete));
	}
	else
	{
		FDerivedDataAsyncWrapperRequest* Request = new FDerivedDataAsyncWrapperRequest(Owner,
			[this, Records = TArray<FCacheRecord>(Records), Context = FString(Context), Policy, OnComplete = MoveTemp(OnComplete)](bool bCancel) mutable
			{
				if (!bCancel)
				{
					FRequestOwner BlockingOwner(EPriority::Blocking);
					InnerBackend->Put(Records, Context, Policy, BlockingOwner, MoveTemp(OnComplete));
					BlockingOwner.Wait();
				}
				else if (OnComplete)
				{
					for (const FCacheRecord& Record : Records)
					{
						OnComplete({Record.GetKey(), EStatus::Canceled});
					}
				}
			});
		Request->Start(Owner.GetPriority());
	}
}

void FDerivedDataBackendAsyncPutWrapper::Get(
	TConstArrayView<FCacheKey> Keys,
	FStringView Context,
	FCacheRecordPolicy Policy,
	IRequestOwner& Owner,
	FOnCacheGetComplete&& OnComplete)
{
	if (Owner.GetPriority() == EPriority::Blocking || !GDDCIOThreadPool)
	{
		InnerBackend->Get(Keys, Context, Policy, Owner, MoveTemp(OnComplete));
	}
	else
	{
		FDerivedDataAsyncWrapperRequest* Request = new FDerivedDataAsyncWrapperRequest(Owner,
			[this, Keys = TArray<FCacheKey>(Keys), Context = FString(Context), Policy, OnComplete = MoveTemp(OnComplete)](bool bCancel) mutable
			{
				if (!bCancel)
				{
					FRequestOwner BlockingOwner(EPriority::Blocking);
					InnerBackend->Get(Keys, Context, Policy, BlockingOwner, MoveTemp(OnComplete));
					BlockingOwner.Wait();
				}
				else if (OnComplete)
				{
					for (const FCacheKey& Key : Keys)
					{
						OnComplete({FCacheRecordBuilder(Key).Build(), EStatus::Canceled});
					}
				}
			});
		Request->Start(Owner.GetPriority());
	}
}

void FDerivedDataBackendAsyncPutWrapper::GetChunks(
	TConstArrayView<FCacheChunkRequest> Chunks,
	FStringView Context,
	IRequestOwner& Owner,
	FOnCacheGetChunkComplete&& OnComplete)
{
	if (Owner.GetPriority() == EPriority::Blocking || !GDDCIOThreadPool)
	{
		InnerBackend->GetChunks(Chunks, Context, Owner, MoveTemp(OnComplete));
	}
	else
	{
		FDerivedDataAsyncWrapperRequest* Request = new FDerivedDataAsyncWrapperRequest(Owner,
			[this, Chunks = TArray<FCacheChunkRequest>(Chunks), Context = FString(Context), OnComplete = MoveTemp(OnComplete)](bool bCancel) mutable
			{
				if (!bCancel)
				{
					FRequestOwner BlockingOwner(EPriority::Blocking);
					InnerBackend->GetChunks(Chunks, Context, BlockingOwner, MoveTemp(OnComplete));
					BlockingOwner.Wait();
				}
				else if (OnComplete)
				{
					for (const FCacheChunkRequest& Chunk : Chunks)
					{
						OnComplete({Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Canceled});
					}
				}
			});
		Request->Start(Owner.GetPriority());
	}
}

} // UE::DerivedData::Backends
