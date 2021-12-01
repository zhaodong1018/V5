// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "DerivedDataBackendInterface.h"
#include "ProfilingDebugging/CookStats.h"
#include "DerivedDataCacheUsageStats.h"
#include "Misc/ScopeLock.h"
#include "MemoryDerivedDataBackend.h"
#include "Async/AsyncWork.h"

namespace UE::DerivedData::Backends
{

/** 
 * Thread safe set helper
**/
struct FThreadSet
{
	FCriticalSection	SynchronizationObject;
	TSet<FString>		FilesInFlight;

	void Add(const FString& Key)
	{
		FScopeLock ScopeLock(&SynchronizationObject);
		check(Key.Len());
		FilesInFlight.Add(Key);
	}
	void Remove(const FString& Key)
	{
		FScopeLock ScopeLock(&SynchronizationObject);
		FilesInFlight.Remove(Key);
	}
	bool Exists(const FString& Key)
	{
		FScopeLock ScopeLock(&SynchronizationObject);
		return FilesInFlight.Contains(Key);
	}
	bool AddIfNotExists(const FString& Key)
	{
		FScopeLock ScopeLock(&SynchronizationObject);
		check(Key.Len());
		if (!FilesInFlight.Contains(Key))
		{
			FilesInFlight.Add(Key);
			return true;
		}
		return false;
	}
};

/** 
 * A backend wrapper that coordinates async puts. This means that a Get will hit an in-memory cache while the async put is still in flight.
**/
class FDerivedDataBackendAsyncPutWrapper : public FDerivedDataBackendInterface
{
public:

	/**
	 * Constructor
	 *
	 * @param	InInnerBackend		Backend to use for storage, my responsibilities are about async puts
	 * @param	bCacheInFlightPuts	if true, cache in-flight puts in a memory cache so that they hit immediately
	 */
	FDerivedDataBackendAsyncPutWrapper(FDerivedDataBackendInterface* InInnerBackend, bool bCacheInFlightPuts);

	/** Return a name for this interface */
	virtual FString GetDisplayName() const override { return FString(TEXT("AsyncPutWrapper")); }

	/** Return a name for this interface */
	virtual FString GetName() const override
	{
		return FString::Printf(TEXT("AsyncPutWrapper (%s)"), *InnerBackend->GetName());
	}

	/** return true if this cache is writable **/
	virtual bool IsWritable() const override;

	/** This is a wrapepr type **/
	virtual bool IsWrapper() const override
	{
		return true;
	}

	/** Returns a class of speed for this interface **/
	virtual ESpeedClass GetSpeedClass() const override;

	/** Return true if hits on this cache should propagate to lower cache level. */
	virtual bool BackfillLowerCacheLevels() const override;

	/**
	 * Synchronous test for the existence of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @return				true if the data probably will be found, this can't be guaranteed because of concurrency in the backends, corruption, etc
	 */
	virtual bool CachedDataProbablyExists(const TCHAR* CacheKey) override;

	/**
	 * Synchronous test for the existence of multiple cache items
	 *
	 * @param	CacheKeys	Alphanumeric+underscore key of the cache items
	 * @return				A bit array with bits indicating whether the data for the corresponding key will probably be found
	 */
	virtual TBitArray<> CachedDataProbablyExistsBatch(TConstArrayView<FString> CacheKeys) override;

	/**
	 * Attempt to make sure the cached data will be available as optimally as possible.
	 *
	 * @param	CacheKeys	Alphanumeric+underscore keys of the cache items
	 * @return				true if the data will probably be found in a fast backend on a future request.
	 */
	virtual bool TryToPrefetch(TConstArrayView<FString> CacheKeys) override;

	/**
	 * Allows the DDC backend to determine if it wants to cache the provided data. Reasons for returning false could be a slow connection,
	 * a file size limit, etc.
	 */
	virtual bool WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData) override;

	/**
	 * Synchronous retrieve of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	OutData		Buffer to receive the results, if any were found
	 * @return				true if any data was found, and in this case OutData is non-empty
	 */
	virtual bool GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData) override;

	/**
	 * Asynchronous, fire-and-forget placement of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	InData		Buffer containing the data to cache, can be destroyed after the call returns, immediately
	 * @param	bPutEvenIfExists	If true, then do not attempt skip the put even if CachedDataProbablyExists returns true
	 */
	virtual EPutStatus PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists) override;

	virtual void RemoveCachedData(const TCHAR* CacheKey, bool bTransient) override;

	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const override;

	virtual bool ApplyDebugOptions(FBackendDebugOptions& InOptions) override;

	virtual void Put(
		TConstArrayView<FCacheRecord> Records,
		FStringView Context,
		ECachePolicy Policy,
		IRequestOwner& Owner,
		FOnCachePutComplete&& OnComplete) override;

	virtual void Get(
		TConstArrayView<FCacheKey> Keys,
		FStringView Context,
		FCacheRecordPolicy Policy,
		IRequestOwner& Owner,
		FOnCacheGetComplete&& OnComplete) override;

	virtual void GetChunks(
		TConstArrayView<FCacheChunkRequest> Chunks,
		FStringView Context,
		IRequestOwner& Owner,
		FOnCacheGetChunkComplete&& OnComplete) override;

private:
	FDerivedDataCacheUsageStats UsageStats;
	FDerivedDataCacheUsageStats PutSyncUsageStats;

	/** Backend to use for storage, my responsibilities are about async puts **/
	FDerivedDataBackendInterface*					InnerBackend;
	/** Memory based cache to deal with gets that happen while an async put is still in flight **/
	TUniquePtr<FDerivedDataBackendInterface>		InflightCache;
	/** We remember outstanding puts so that we don't do them redundantly **/
	FThreadSet										FilesInFlight;
};

} // UE::DerivedData::Backends
