// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Algo/BinarySearch.h"
#include "Containers/ArrayView.h"
#include "DerivedDataBackendAsyncPutWrapper.h"
#include "DerivedDataBackendInterface.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataCacheUsageStats.h"
#include "DerivedDataChunk.h"
#include "DerivedDataPayload.h"
#include "DerivedDataRequestOwner.h"
#include "HAL/PlatformMath.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "ProfilingDebugging/CookStats.h"
#include "Templates/UniquePtr.h"

extern bool GVerifyDDC;

namespace UE::DerivedData::Backends
{

/** 
 * A backend wrapper that implements a cache hierarchy of backends. 
**/
class FHierarchicalDerivedDataBackend : public FDerivedDataBackendInterface
{
public:

	/**
	 * Constructor
	 * @param	InInnerBackends Backends to call into for actual storage of the cache, first item is the "fastest cache"
	 */
	FHierarchicalDerivedDataBackend(const TArray<FDerivedDataBackendInterface*>& InInnerBackends)
		: InnerBackends(InInnerBackends)
		, bIsWritable(false)
		, bHasLocalBackends(false)
		, bHasRemoteBackends(false)
		, bHasMultipleLocalBackends(false)
		, bHasMultipleRemoteBackends(false)
		, bHasWritableLocalBackends(false)
		, bHasWritableRemoteBackends(false)
	{
		check(InnerBackends.Num() > 1); // if it is just one, then you don't need this wrapper
		UpdateAsyncInnerBackends();
	}

	/** Return a type for this interface */
	virtual FString GetDisplayName() const override
	{
		return TEXT("Hierarchical");
	}

	/** Return a name for this interface */
	virtual FString GetName() const override
	{
		return TEXT("HierarchicalDerivedDataBackend");
	}

	/** Are we a remote cache? */
	virtual ESpeedClass GetSpeedClass() const override
	{
		return ESpeedClass::Local;
	}

private:
	void UpdateAsyncInnerBackends()
	{
		bIsWritable = false;
		bHasLocalBackends = false;
		bHasRemoteBackends = false;
		bHasMultipleLocalBackends = false;
		bHasMultipleRemoteBackends = false;
		bHasWritableLocalBackends = false;
		bHasWritableRemoteBackends = false;
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			const bool bIsWritableBackend = InnerBackends[CacheIndex]->IsWritable();
			bIsWritable |= bIsWritableBackend;
			if (InnerBackends[CacheIndex]->GetSpeedClass() == ESpeedClass::Local)
			{
				bHasWritableLocalBackends |= bIsWritableBackend;
				bHasMultipleLocalBackends = bHasLocalBackends;
				bHasLocalBackends = true;
			}
			else
			{
				bHasWritableRemoteBackends |= bIsWritableBackend;
				bHasMultipleRemoteBackends = bHasRemoteBackends;
				bHasRemoteBackends = true;
			}
		}

		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			// async puts to allow us to fill all levels without holding up the engine
			// we need to cache inflight puts to avoid having inconsistent miss and redownload on lower cache levels while puts are still async
			const bool bCacheInFlightPuts = true;
			AsyncPutInnerBackends.Emplace(new FDerivedDataBackendAsyncPutWrapper(InnerBackends[CacheIndex], bCacheInFlightPuts));
		}
	}

public:
	/** Adds inner backend. */
	void AddInnerBackend(FDerivedDataBackendInterface* InInner) 
	{
		FWriteScopeLock LockScope(Lock);

		InnerBackends.Add(InInner);
		AsyncPutInnerBackends.Empty();
		UpdateAsyncInnerBackends();
	}

	/** Removes inner backend. */
	bool RemoveInnerBackend(FDerivedDataBackendInterface* InInner) 
	{
		FWriteScopeLock LockScope(Lock);

		int32 NumRemoved = InnerBackends.Remove(InInner);
		AsyncPutInnerBackends.Empty();
		UpdateAsyncInnerBackends();
		return NumRemoved != 0;
	}

	/** return true if this cache is writable **/
	virtual bool IsWritable() const override
	{
		return bIsWritable;
	}

	/**
	 * Synchronous test for the existence of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @return				true if the data probably will be found, this can't be guaranteed because of concurrency in the backends, corruption, etc
	 */
	virtual bool CachedDataProbablyExists(const TCHAR* CacheKey) override
	{
		COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());

		FReadScopeLock LockScope(Lock);

		for (int32 CacheIndex = 0; CacheIndex < AsyncPutInnerBackends.Num(); CacheIndex++)
		{
			if (AsyncPutInnerBackends[CacheIndex]->CachedDataProbablyExists(CacheKey))
			{
				COOK_STAT(Timer.AddHit(0));
				return true;
			}
		}
		return false;
	}

	/**
	 * Synchronous test for the existence of multiple cache items
	 *
	 * @param	CacheKeys	Alphanumeric+underscore key of the cache items
	 * @return				A bit array with bits indicating whether the data for the corresponding key will probably be found
	 */
	virtual TBitArray<> CachedDataProbablyExistsBatch(TConstArrayView<FString> CacheKeys) override
	{
		COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());

		TBitArray<> Result;
		{
			FReadScopeLock LockScope(Lock);

			check(InnerBackends.Num() > 0);
			for (int32 CacheIndex = 0; CacheIndex < AsyncPutInnerBackends.Num(); ++CacheIndex)
			{
				const int32 MissingKeys = CacheKeys.Num() - Result.CountSetBits();
				if (MissingKeys == 0)
				{
					break;
				}

				if (MissingKeys == CacheKeys.Num())
				{
					Result = AsyncPutInnerBackends[CacheIndex]->CachedDataProbablyExistsBatch(CacheKeys);
					check(Result.Num() == CacheKeys.Num());
				}
				else
				{
					TArray<FString> RemainingKeys;
					for (int32 KeyIndex = 0; KeyIndex < CacheKeys.Num(); ++KeyIndex)
					{
						if (!Result[KeyIndex])
						{
							RemainingKeys.Add(CacheKeys[KeyIndex]);
						}
					}

					TBitArray<>::FIterator ResultIt(Result);
					TBitArray<> NewResult = AsyncPutInnerBackends[CacheIndex]->CachedDataProbablyExistsBatch(RemainingKeys);
					check(NewResult.Num() == RemainingKeys.Num());
					for (TBitArray<>::FConstIterator NewIt(NewResult); NewIt; ++NewIt)
					{
						// Skip to the bit for the next remaining key.
						while (ResultIt.GetValue())
						{
							++ResultIt;
						}
						ResultIt.GetValue() = NewIt.GetValue();
						++ResultIt;
					}
				}
			}
		}

		if (Result.IsEmpty())
		{
			Result.Add(false, CacheKeys.Num());
		}

		if (Result.CountSetBits() == CacheKeys.Num())
		{
			COOK_STAT(Timer.AddHit(0));
		}
		return Result;
	}

	/**
	 * Attempt to make sure the cached data will be available as optimally as possible.
	 *
	 * @param	CacheKeys	Alphanumeric+underscore keys of the cache items
	 * @return				true if the data will probably be found in a fast backend on a future request.
	 */
	virtual bool TryToPrefetch(TConstArrayView<FString> CacheKeys) override
	{
		COOK_STAT(auto Timer = UsageStats.TimePrefetch());

		TArray<FString, TInlineAllocator<16>> SearchKeys(CacheKeys);

		bool bHasFastBackendToWrite = false;
		bool bHasSlowBackend = false;

		{
			FReadScopeLock LockScope(Lock);

			for (const TUniquePtr<FDerivedDataBackendInterface>& Interface : AsyncPutInnerBackends)
			{
				if (Interface->GetSpeedClass() < ESpeedClass::Fast)
				{
					bHasSlowBackend = true;
				}
				else
				{
					bHasFastBackendToWrite = bHasFastBackendToWrite || Interface->IsWritable();

					// Remove keys that exist in a fast backend because we already have fast access to them.
					TBitArray<> Hits = Interface->CachedDataProbablyExistsBatch(SearchKeys);
					TArray<FString, TInlineAllocator<16>>::TIterator It = SearchKeys.CreateIterator();
					It.SetToEnd();
					for (--It; It; )
					{
						if (Hits[It.GetIndex()])
						{
							// RemoveCurrent will decrement the iterator
							It.RemoveCurrent();
						}
						else
						{
							--It;
						}
					}

					// No fetch is necessary if every key already exists in a fast backend.
					if (SearchKeys.IsEmpty())
					{
						COOK_STAT(Timer.AddHit(0));
						return true;
					}
				}
			}
		}

		// Try to fetch remaining keys, which will fill them from slow backends into writable fast backends.
		bool bHit = true;
		int64 BytesFetched = 0;
		if (bHasSlowBackend && bHasFastBackendToWrite)
		{
			for (const FString& CacheKey : SearchKeys)
			{
				TArray<uint8> Ignored;
				bHit &= GetCachedData(*CacheKey, Ignored);
				BytesFetched += Ignored.Num();
			}
		}

		COOK_STAT(if (bHit) { Timer.AddHit(BytesFetched); });
		return bHit;
	}

	/*
		Determine if we would cache this by asking all our inner layers
	*/
	virtual bool WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData) override
	{
		FReadScopeLock LockScope(Lock);

		for (int32 CacheIndex = 0; CacheIndex < AsyncPutInnerBackends.Num(); CacheIndex++)
		{
			if (AsyncPutInnerBackends[CacheIndex]->WouldCache(CacheKey, InData))
			{
				return true;
			}
		}

		return false;
	}

	bool ApplyDebugOptions(FBackendDebugOptions& InOptions) override
	{
		FReadScopeLock LockScope(Lock);
		
		bool bSuccess = true;
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			if (!InnerBackends[CacheIndex]->ApplyDebugOptions(InOptions))
			{
				bSuccess = false;
			}
		}

		return bSuccess;
	}



	/**
	 * Synchronous retrieve of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	OutData		Buffer to receive the results, if any were found
	 * @return				true if any data was found, and in this case OutData is non-empty
	 */
	virtual bool GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData) override
	{
		COOK_STAT(auto Timer = UsageStats.TimeGet());

		FReadScopeLock LockScope(Lock);

		for (int32 CacheIndex = 0; CacheIndex < AsyncPutInnerBackends.Num(); CacheIndex++)
		{
			const TUniquePtr<FDerivedDataBackendInterface>& GetInterface = AsyncPutInnerBackends[CacheIndex];

			// just try and get the cached data. It's faster to try and fail than it is to check and succeed. 			
			if (GetInterface->GetCachedData(CacheKey, OutData))
			{
				// if this hierarchy is writable..
				if (bIsWritable)
				{
					// fill in the higher level caches (start with the highest level as that should be the biggest 
					// !/$ if any of our puts get interrupted or fail)
					for (int32 MissedCacheIndex = 0; MissedCacheIndex < CacheIndex; MissedCacheIndex++)
					{
						FDerivedDataBackendInterface* MissedCache = InnerBackends[MissedCacheIndex];

						if (MissedCache->IsWritable())
						{
							// use the async interface to perform the put
							AsyncPutInnerBackends[MissedCacheIndex]->PutCachedData(CacheKey, OutData, false);

							UE_LOG(LogDerivedDataCache, Verbose, TEXT("Forward-filling cache %s with: %s (%d bytes)"), *MissedCache->GetName(), CacheKey, OutData.Num());
						}
					}

					// cascade this data to any lower level back ends that may be missing the data
					if (GetInterface->BackfillLowerCacheLevels())
					{
						// fill in the lower level caches
						for (int32 PutCacheIndex = CacheIndex + 1; PutCacheIndex < AsyncPutInnerBackends.Num(); PutCacheIndex++)
						{
							const TUniquePtr<FDerivedDataBackendInterface>& PutBackend = AsyncPutInnerBackends[PutCacheIndex];

							// If the key is in a distributed cache (e.g. Pak or S3) then don't backfill any further. 
							bool IsInDistributedCache = !PutBackend->IsWritable() && !PutBackend->BackfillLowerCacheLevels() && PutBackend->CachedDataProbablyExists(CacheKey);

							if (!IsInDistributedCache)
							{
								// only backfill to fast caches (todo - need a way to put data that was created locally into the cache for other people)
								bool bFastCache = PutBackend->GetSpeedClass() >= ESpeedClass::Fast;

								// No need to validate that the cache data might exist since the check can be expensive, the async put will do the check and early out in that case
								if (bFastCache && PutBackend->IsWritable())
								{								
									AsyncPutInnerBackends[PutCacheIndex]->PutCachedData(CacheKey, OutData, false); // we do not need to force a put here
									UE_LOG(LogDerivedDataCache, Verbose, TEXT("Back-filling cache %s with: %s (%d bytes)"), *PutBackend->GetName(), CacheKey, OutData.Num());
								}
							}
							else
							{ 
								UE_LOG(LogDerivedDataCache, Verbose, TEXT("Item %s exists in distributed cache %s. Skipping any further backfills."), CacheKey, *PutBackend->GetName());
								break;
							}
						}
					}
				}
				COOK_STAT(Timer.AddHit(OutData.Num()));
				return true;
			}
		}
		return false;
	}
	/**
	 * Asynchronous, fire-and-forget placement of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	InData		Buffer containing the data to cache, can be destroyed after the call returns, immediately
	 * @param	bPutEvenIfExists	If true, then do not attempt skip the put even if CachedDataProbablyExists returns true
	 */
	virtual EPutStatus PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists) override
	{
		COOK_STAT(auto Timer = UsageStats.TimePut());
		EPutStatus Status = EPutStatus::NotCached;
		if (!bIsWritable)
		{
			return Status; // no point in continuing down the chain
		}

		FReadScopeLock LockScope(Lock);

		for (int32 PutCacheIndex = 0; PutCacheIndex < InnerBackends.Num(); PutCacheIndex++)
		{
			if (!InnerBackends[PutCacheIndex]->IsWritable() && !InnerBackends[PutCacheIndex]->BackfillLowerCacheLevels() && InnerBackends[PutCacheIndex]->CachedDataProbablyExists(CacheKey))
			{
				break; //do not write things that are already in the read only pak file
			}
			if (InnerBackends[PutCacheIndex]->IsWritable())
			{
				COOK_STAT(Timer.AddHit(InData.Num()));
				// we must do at least one synchronous put to a writable cache before we return
				if (Status != EPutStatus::Cached)
				{
					Status = InnerBackends[PutCacheIndex]->PutCachedData(CacheKey, InData, bPutEvenIfExists);
				}
				else
				{
					AsyncPutInnerBackends[PutCacheIndex]->PutCachedData(CacheKey, InData, bPutEvenIfExists);
				}
			}
		}
		return Status;
	}

	virtual void RemoveCachedData(const TCHAR* CacheKey, bool bTransient) override
	{
		if (!bIsWritable)
		{
			return; // no point in continuing down the chain
		}

		FReadScopeLock LockScope(Lock);

		for (int32 PutCacheIndex = 0; PutCacheIndex < AsyncPutInnerBackends.Num(); PutCacheIndex++)
		{
			AsyncPutInnerBackends[PutCacheIndex]->RemoveCachedData(CacheKey, bTransient);
		}
	}

	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const override
	{
		TSharedRef<FDerivedDataCacheStatsNode> Usage = MakeShared<FDerivedDataCacheStatsNode>(this, TEXT("Hierarchical"));
		Usage->Stats.Add(TEXT(""), UsageStats);

		FReadScopeLock LockScope(Lock);

		// All the inner backends are actually wrapped by AsyncPut backends
		for (const auto& InnerBackend : AsyncPutInnerBackends)
		{
			Usage->Children.Add(InnerBackend->GatherUsageStats());
		}

		return Usage;
	}

	virtual void Put(
		TConstArrayView<FCacheRecord> Records,
		FStringView Context,
		ECachePolicy Policy,
		IRequestOwner& Owner,
		FOnCachePutComplete&& OnComplete) override
	{
		FRequestOwner AsyncOwner(FPlatformMath::Min(Owner.GetPriority(), EPriority::Highest));
		FRequestBarrier AsyncBarrier(AsyncOwner);
		AsyncOwner.KeepAlive();

		TSet<FCacheKey> RecordsOk;

		{
			FReadScopeLock LockScope(Lock);

			for (int32 PutCacheIndex = 0; PutCacheIndex < InnerBackends.Num(); PutCacheIndex++)
			{
				if (InnerBackends[PutCacheIndex]->IsWritable())
				{
					// Every record must put synchronously before switching to async calls.
					if (RecordsOk.Num() < Records.Num())
					{
						FRequestOwner BlockingOwner(EPriority::Blocking);
						InnerBackends[PutCacheIndex]->Put(Records, Context, Policy, BlockingOwner,
							[&OnComplete, &RecordsOk](FCachePutCompleteParams&& Params)
							{
								if (Params.Status == EStatus::Ok)
								{
									bool bIsAlreadyInSet = false;
									RecordsOk.FindOrAdd(Params.Key, &bIsAlreadyInSet);
									if (OnComplete && !bIsAlreadyInSet)
									{
										OnComplete(MoveTemp(Params));
									}
								}
							});
						BlockingOwner.Wait();
					}
					else
					{
						AsyncPutInnerBackends[PutCacheIndex]->Put(Records, Context, Policy, AsyncOwner);
					}
				}
			}
		}

		if (OnComplete && RecordsOk.Num() < Records.Num())
		{
			for (const FCacheRecord& Record : Records)
			{
				if (!RecordsOk.Contains(Record.GetKey()))
				{
					OnComplete({Record.GetKey(), EStatus::Error});
				}
			}
		}
	}

	virtual void Get(
		TConstArrayView<FCacheKey> Keys,
		FStringView Context,
		FCacheRecordPolicy Policy,
		IRequestOwner& Owner,
		FOnCacheGetComplete&& OnComplete) override
	{
		const bool bQueryLocal = bHasLocalBackends && EnumHasAnyFlags(Policy.GetRecordPolicy(), ECachePolicy::QueryLocal);
		const bool bStoreLocal = bHasWritableLocalBackends && EnumHasAnyFlags(Policy.GetRecordPolicy(), ECachePolicy::StoreLocal);
		const bool bQueryRemote = bHasRemoteBackends && EnumHasAnyFlags(Policy.GetRecordPolicy(), ECachePolicy::QueryRemote);
		const bool bStoreRemote = bHasWritableRemoteBackends && EnumHasAnyFlags(Policy.GetRecordPolicy(), ECachePolicy::StoreRemote);
		const bool bStoreLocalCopy = bStoreLocal && bHasMultipleLocalBackends;

		TArray<FCacheKey, TInlineAllocator<16>> RemainingKeys(Keys);

		{
			FReadScopeLock LockScope(Lock);
			TSet<FCacheKey> KeysOk;

			bool bHadLocalMiss = false;
			bool bHadRemoteMiss = false;

			for (int32 GetCacheIndex = 0; GetCacheIndex < InnerBackends.Num() && !RemainingKeys.IsEmpty(); ++GetCacheIndex)
			{
				const bool bIsLocalGet = InnerBackends[GetCacheIndex]->GetSpeedClass() >= ESpeedClass::Fast;

				// Block on this because backends in this hierarchy are not expected to be asynchronous.
				FRequestOwner BlockingOwner(EPriority::Blocking);
				InnerBackends[GetCacheIndex]->Get(RemainingKeys, Context, Policy, BlockingOwner,
					[this, Context, GetCacheIndex, bStoreLocal, bStoreRemote, bStoreLocalCopy, bIsLocalGet, &Owner, &OnComplete, &KeysOk](FCacheGetCompleteParams&& Params)
					{
						if (Params.Status == EStatus::Ok)
						{
							FRequestOwner AsyncOwner(FPlatformMath::Min(Owner.GetPriority(), EPriority::Highest));
							FRequestBarrier AsyncBarrier(AsyncOwner);
							AsyncOwner.KeepAlive();
							for (int32 FillCacheIndex = 0; FillCacheIndex < InnerBackends.Num(); ++FillCacheIndex)
							{
								if (GetCacheIndex != FillCacheIndex)
								{
									const bool bIsLocalFill = InnerBackends[FillCacheIndex]->GetSpeedClass() >= ESpeedClass::Fast;
									if (bIsLocalFill ? bStoreLocal : bStoreRemote)
									{
										AsyncPutInnerBackends[FillCacheIndex]->Put({Params.Record}, Context, ECachePolicy::Default, AsyncOwner);
									}
								}
							}

							KeysOk.Add(Params.Record.GetKey());
							if (OnComplete)
							{
								OnComplete(MoveTemp(Params));
							}
						}
					});
				BlockingOwner.Wait();

				RemainingKeys.RemoveAll([&KeysOk](const FCacheKey& Key) { return KeysOk.Contains(Key); });

				bool& bHadMiss = bIsLocalGet ? bHadLocalMiss : bHadRemoteMiss;
				if (!bHadMiss && !RemainingKeys.IsEmpty() && InnerBackends[GetCacheIndex]->IsWritable())
				{
					bHadMiss = true;
					const auto ConvertPolicy = [bIsLocalGet](ECachePolicy P) -> ECachePolicy
					{
						if (EnumHasAnyFlags(P, bIsLocalGet ? ECachePolicy::StoreLocal : ECachePolicy::StoreRemote))
						{
							EnumRemoveFlags(P, ECachePolicy::SkipData);
						}
						return P;
					};
					FCacheRecordPolicyBuilder Builder(ConvertPolicy(Policy.GetDefaultPayloadPolicy()));
					for (const FCachePayloadPolicy& PayloadPolicy : Policy.GetPayloadPolicies())
					{
						Builder.AddPayloadPolicy(PayloadPolicy.Id, ConvertPolicy(PayloadPolicy.Policy));
					}
					Policy = Builder.Build();
				}
			}
		}

		if (OnComplete)
		{
			for (const FCacheKey& Key : RemainingKeys)
			{
				OnComplete({FCacheRecordBuilder(Key).Build(), EStatus::Error});
			}
		}
	}

	virtual void GetChunks(
		TConstArrayView<FCacheChunkRequest> Chunks,
		FStringView Context,
		IRequestOwner& Owner,
		FOnCacheGetChunkComplete&& OnComplete) override
	{
		TArray<FCacheChunkRequest, TInlineAllocator<16>> RemainingChunks(Chunks);

		{
			FReadScopeLock LockScope(Lock);

			for (FDerivedDataBackendInterface* InnerBackend : InnerBackends)
			{
				if (RemainingChunks.IsEmpty())
				{
					return;
				}
				RemainingChunks.StableSort(TChunkLess());
				TArray<FCacheChunkRequest, TInlineAllocator<16>> ErrorChunks;
				FRequestOwner BlockingOwner(EPriority::Blocking);
				InnerBackend->GetChunks(RemainingChunks, Context, BlockingOwner,
					[&OnComplete, &RemainingChunks, &ErrorChunks](FCacheGetChunkCompleteParams&& Params)
					{
						if (Params.Status == EStatus::Error)
						{
							const int32 ChunkIndex = Algo::BinarySearch(RemainingChunks, Params, TChunkLess());
							checkf(ChunkIndex != INDEX_NONE, TEXT("Failed to find remaining chunk %s ")
								TEXT(" with raw offset %") UINT64_FMT TEXT("."),
								*WriteToString<96>(Params.Key, '/', Params.Id), Params.RawOffset);
							ErrorChunks.Add(RemainingChunks[ChunkIndex]);
						}
						else if (OnComplete)
						{
							OnComplete(MoveTemp(Params));
						}
					});
				BlockingOwner.Wait();
				RemainingChunks = MoveTemp(ErrorChunks);
			}
		}

		if (OnComplete)
		{
			for (const FCacheChunkRequest& Chunk : RemainingChunks)
			{
				OnComplete({Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Error});
			}
		}
	}

private:
	FDerivedDataCacheUsageStats UsageStats;

	/** To avoid race while manipulating the arrays */
	mutable FRWLock Lock;

	/** Array of backends forming the hierarchical cache...the first element is the fastest cache. **/
	TArray<FDerivedDataBackendInterface*> InnerBackends;
	/** Each of the backends wrapped with an async put **/
	TArray<TUniquePtr<FDerivedDataBackendInterface> > AsyncPutInnerBackends;
	/** As an optimization, we check our writable status at contruction **/
	bool bIsWritable;
	bool bHasLocalBackends;
	bool bHasRemoteBackends;
	bool bHasMultipleLocalBackends;
	bool bHasMultipleRemoteBackends;
	bool bHasWritableLocalBackends;
	bool bHasWritableRemoteBackends;
};

} // UE::DerivedData::Backends
