// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBackendInterface.h"
#include "DerivedDataCacheUsageStats.h"

// Macro for whether to enable the S3 backend. libcurl is not currently available on Mac.
#if !defined(WITH_HTTP_DDC_BACKEND)
	#define WITH_HTTP_DDC_BACKEND 0
#endif

#if WITH_HTTP_DDC_BACKEND

namespace UE::DerivedData::Backends
{

typedef TSharedPtr<class IHttpRequest> FHttpRequestPtr;
typedef TSharedPtr<class IHttpResponse, ESPMode::ThreadSafe> FHttpResponsePtr;


/**
 * Backend for a HTTP based caching service (Jupiter).
 **/
class FHttpDerivedDataBackend : public FDerivedDataBackendInterface
{
public:
	
	/**
	 * Creates the backend, checks health status and attempts to acquire an access token.
	 *
	 * @param ServiceUrl	Base url to the service including schema.
	 * @param Namespace		Namespace to use.
	 * @param OAuthProvider	Url to OAuth provider, for example "https://myprovider.com/oauth2/v1/token".
	 * @param OAuthClientId	OAuth client identifier.
	 * @param OAuthData		OAuth form data to send to login service. Can either be the raw form data or a Windows network file address (starting with "\\").
	 */
	FHttpDerivedDataBackend(
		const TCHAR* ServiceUrl, 
		const TCHAR* Namespace, 
		const TCHAR* OAuthProvider, 
		const TCHAR* OAuthClientId, 
		const TCHAR* OAuthData,
		bool bReadOnly);

	~FHttpDerivedDataBackend();

	/**
	 * Checks is backend is usable (reachable and accessible).
	 * @return true if usable
	 */
	bool IsUsable() const { return bIsUsable; }

	bool IsRemote() const { return true; }

	/** return true if this cache is writable **/
	virtual bool IsWritable() const override
	{
		return !bReadOnly && bIsUsable;
	}

	virtual bool CachedDataProbablyExists(const TCHAR* CacheKey) override;
	virtual TBitArray<> CachedDataProbablyExistsBatch(TConstArrayView<FString> CacheKeys) override;
	virtual bool GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData) override;
	virtual EPutStatus PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists) override;
	virtual void RemoveCachedData(const TCHAR* CacheKey, bool bTransient) override;
	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const override;
	
	virtual FString GetName() const override;
	virtual FString GetDisplayName() const override;
	virtual bool TryToPrefetch(TConstArrayView<FString> CacheKeys) override;
	virtual bool WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData) override;
	virtual ESpeedClass GetSpeedClass() const override;
	virtual bool ApplyDebugOptions(FBackendDebugOptions& InOptions) override;

	void SetSpeedClass(ESpeedClass InSpeedClass) { SpeedClass = InSpeedClass; }

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

	static FHttpDerivedDataBackend* GetAny()
	{
		return AnyInstance;
	}

private:
	FString Domain;
	FString Namespace;
	FString DefaultBucket;
	FString OAuthProvider;
	FString OAuthClientId;
	FString OAuthSecret;
	FCriticalSection AccessCs;
	FDerivedDataCacheUsageStats UsageStats;
	FBackendDebugOptions DebugOptions;
	FCriticalSection MissedKeysCS;
	TSet<FName> DebugMissedKeys;
	TUniquePtr<struct FRequestPool> GetRequestPools[2];
	TUniquePtr<struct FRequestPool> PutRequestPools[2];
	TUniquePtr<struct FHttpAccessToken> Access;
	bool bIsUsable;
	bool bReadOnly;
	uint32 FailedLoginAttempts;
	ESpeedClass SpeedClass;
	static inline FHttpDerivedDataBackend* AnyInstance = nullptr;

	bool IsServiceReady();
	bool AcquireAccessToken();
	bool ShouldRetryOnError(int64 ResponseCode);
	bool ShouldSimulateMiss(const TCHAR* InKey);
};

} // UE::DerivedData::Backends

#endif //WITH_HTTP_DDC_BACKEND
