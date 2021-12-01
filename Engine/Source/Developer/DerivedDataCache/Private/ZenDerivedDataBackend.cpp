// Copyright Epic Games, Inc. All Rights Reserved.
#include "ZenDerivedDataBackend.h"

#if WITH_ZEN_DDC_BACKEND

#include "Algo/Accumulate.h"
#include "Containers/StringFwd.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataChunk.h"
#include "DerivedDataPayload.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryPackage.h"
#include "Serialization/CompactBinaryValidation.h"
#include "Serialization/CompactBinaryWriter.h"
#include "Serialization/CompactBinarySerialization.h"
#include "Serialization/BufferArchive.h"
#include "ZenBackendUtils.h"
#include "ZenSerialization.h"
#include "ZenServerHttp.h"

TRACE_DECLARE_INT_COUNTER(ZenDDC_Exist,			TEXT("ZenDDC Exist"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_ExistHit,		TEXT("ZenDDC Exist Hit"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_Get,			TEXT("ZenDDC Get"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_GetHit,		TEXT("ZenDDC Get Hit"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_Put,			TEXT("ZenDDC Put"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_PutHit,		TEXT("ZenDDC Put Hit"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_BytesReceived, TEXT("ZenDDC Bytes Received"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_BytesSent,		TEXT("ZenDDC Bytes Sent"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_CacheRecordRequestCount, TEXT("ZenDDC CacheRecord Request Count"));
TRACE_DECLARE_INT_COUNTER(ZenDDC_ChunkRequestCount, TEXT("ZenDDC Chunk Request Count"));

namespace UE::DerivedData::Backends {

template<typename T>
void ForEachBatch(const int32 BatchSize, const int32 TotalCount, T&& Fn)
{
	check(BatchSize > 0);

	if (TotalCount > 0)
	{
		const int32 BatchCount = FMath::DivideAndRoundUp(TotalCount, BatchSize);
		const int32 Last = TotalCount - 1;

		for (int32 BatchIndex = 0; BatchIndex < BatchCount; BatchIndex++)
		{
			const int32 BatchFirstIndex	= BatchIndex * BatchSize;
			const int32 BatchLastIndex	= FMath::Min(BatchFirstIndex + BatchSize - 1, Last);

			Fn(BatchFirstIndex, BatchLastIndex);
		}
	}
}

//----------------------------------------------------------------------------------------------------------
// FZenDerivedDataBackend
//----------------------------------------------------------------------------------------------------------

FZenDerivedDataBackend::FZenDerivedDataBackend(
	const TCHAR* InServiceUrl,
	const TCHAR* InNamespace)
	: Namespace(InNamespace)
	, ZenService(InServiceUrl)
{
	if (IsServiceReady())
	{
		RequestPool = MakeUnique<Zen::FZenHttpRequestPool>(ZenService.GetInstance().GetURL(), 32);
		bIsUsable = true;
		bIsRemote = false;

		Zen::FZenStats ZenStats;

		if (ZenService.GetInstance().GetStats(ZenStats) == true)
		{
			 bIsRemote = ZenStats.UpstreamStats.EndPointStats.IsEmpty() == false;
		}
	}

	GConfig->GetInt(TEXT("Zen"), TEXT("CacheRecordBatchSize"), CacheRecordBatchSize, GEngineIni);
	GConfig->GetInt(TEXT("Zen"), TEXT("CacheChunksBatchSize"), CacheChunksBatchSize, GEngineIni);
}

FZenDerivedDataBackend::~FZenDerivedDataBackend()
{
}

FString FZenDerivedDataBackend::GetDisplayName() const
{
	return FString(TEXT("Zen"));
}

FString FZenDerivedDataBackend::GetName() const
{
	return ZenService.GetInstance().GetURL();
}

bool FZenDerivedDataBackend::IsRemote() const
{
	return bIsRemote;
}

bool FZenDerivedDataBackend::IsServiceReady()
{
	return ZenService.GetInstance().IsServiceReady();
}

bool FZenDerivedDataBackend::ShouldRetryOnError(int64 ResponseCode)
{
	// Access token might have expired, request a new token and try again.
	if (ResponseCode == 401)
	{
		return true;
	}

	// Too many requests, make a new attempt
	if (ResponseCode == 429)
	{
		return true;
	}

	return false;
}

bool FZenDerivedDataBackend::CachedDataProbablyExists(const TCHAR* CacheKey)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::Exist);
	TRACE_COUNTER_ADD(ZenDDC_Exist, int64(1));
	COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());

	if (ShouldSimulateMiss(CacheKey))
	{
		return false;
	}
	
	FString Uri = MakeLegacyZenKey(CacheKey);
	int64 ResponseCode = 0; 
	uint32 Attempts = 0;

	// Retry request until we get an accepted response or exhaust allowed number of attempts.
	while (ResponseCode == 0 && ++Attempts < MaxAttempts)
	{
		Zen::FZenScopedRequestPtr Request(RequestPool.Get());
		Zen::FZenHttpRequest::Result Result = Request->PerformBlockingHead(*Uri, Zen::EContentType::Binary);
		ResponseCode = Request->GetResponseCode();

		if (Zen::IsSuccessCode(ResponseCode) || ResponseCode == 404)
		{
			const bool bIsHit = (Result == Zen::FZenHttpRequest::Result::Success && Zen::IsSuccessCode(ResponseCode));

			if (bIsHit)
			{
				TRACE_COUNTER_ADD(ZenDDC_ExistHit, int64(1));
			}
			return bIsHit;
		}

		if (!ShouldRetryOnError(ResponseCode))
		{
			return false;
		}

		ResponseCode = 0;
	}

	return false;
}

bool FZenDerivedDataBackend::GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::GetCachedData);
	TRACE_COUNTER_ADD(ZenDDC_Get, int64(1));
	COOK_STAT(auto Timer = UsageStats.TimeGet());

	if (ShouldSimulateMiss(CacheKey))
	{
		return false;
	}

	double StartTime = FPlatformTime::Seconds();

	TArray64<uint8> ArrayBuffer;
	EGetResult Result = GetZenData(MakeLegacyZenKey(CacheKey), &ArrayBuffer, Zen::EContentType::Binary);
	check(ArrayBuffer.Num() <= UINT32_MAX);
	OutData = MoveTemp(ArrayBuffer);
	if (Result != EGetResult::Success)
	{
		switch (Result)
		{
		default:
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache miss on %s"), *GetName(), CacheKey);
			break;
		case EGetResult::Corrupted:
			UE_LOG(LogDerivedDataCache, Warning,
				TEXT("Checksum from server on %s did not match recieved data. Discarding cached result."), CacheKey);
			break;
		}
		return false;
	}

	TRACE_COUNTER_ADD(ZenDDC_GetHit, int64(1));
	TRACE_COUNTER_ADD(ZenDDC_BytesReceived, int64(OutData.Num()));
	COOK_STAT(Timer.AddHit(OutData.Num()));
	double ReadDuration = FPlatformTime::Seconds() - StartTime;
	double ReadSpeed = (OutData.Num() / ReadDuration) / (1024.0 * 1024.0);
	UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache hit on %s (%d bytes, %.02f secs, %.2fMB/s)"),
		*GetName(), CacheKey, OutData.Num(), ReadDuration, ReadSpeed);
	return true;
}

FZenDerivedDataBackend::EGetResult
FZenDerivedDataBackend::GetZenData(FStringView Uri, TArray64<uint8>* OutData, Zen::EContentType ContentType) const
{
	// Retry request until we get an accepted response or exhaust allowed number of attempts.
	EGetResult GetResult = EGetResult::NotFound;
	for (uint32 Attempts = 0; Attempts < MaxAttempts; ++Attempts)
	{
		Zen::FZenScopedRequestPtr Request(RequestPool.Get());
		if (Request.IsValid())
		{
			Zen::FZenHttpRequest::Result Result;
			if (OutData)
			{
				Result = Request->PerformBlockingDownload(Uri, OutData, ContentType);
			}
			else
			{
				Result = Request->PerformBlockingHead(Uri, ContentType);
			}
			int64 ResponseCode = Request->GetResponseCode();

			// Request was successful, make sure we got all the expected data.
			if (Zen::IsSuccessCode(ResponseCode))
			{
				return EGetResult::Success;
			}

			if (!ShouldRetryOnError(ResponseCode))
			{
				break;
			}
		}
	}

	if (OutData)
	{
		OutData->Reset();
	}

	return GetResult;
}

FZenDerivedDataBackend::EGetResult
FZenDerivedDataBackend::GetZenData(const FCacheKey& CacheKey, ECachePolicy CachePolicy, FCbPackage& OutPackage) const
{
	TStringBuilder<256> QueryUri;
	AppendZenUri(CacheKey, QueryUri);
	AppendPolicyQueryString(CachePolicy, QueryUri);

	// Retry request until we get an accepted response or exhaust allowed number of attempts.
	EGetResult GetResult = EGetResult::NotFound;
	for (uint32 Attempts = 0; Attempts < MaxAttempts; ++Attempts)
	{
		Zen::FZenScopedRequestPtr Request(RequestPool.Get());
		if (Request.IsValid())
		{
			Zen::FZenHttpRequest::Result Result = Request->PerformBlockingDownload(QueryUri.ToString(), OutPackage);
			int64 ResponseCode = Request->GetResponseCode();
			bool bPackageValid = Request->GetResponseFormatValid();

			// Request was successful, make sure we got all the expected data.
			if (Zen::IsSuccessCode(ResponseCode))
			{
				if (bPackageValid)
				{
					GetResult = EGetResult::Success;
				}
				else
				{
					GetResult = EGetResult::Corrupted;
				}
				break;
			}

			if (!ShouldRetryOnError(ResponseCode))
			{
				break;
			}
		}
	}

	if (GetResult != EGetResult::Success)
	{
		OutPackage.Reset();
	}
	return GetResult;
}

FDerivedDataBackendInterface::EPutStatus
FZenDerivedDataBackend::PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::PutCachedData);

	if (ShouldSimulateMiss(CacheKey))
	{
		return EPutStatus::NotCached;
	}

	FSharedBuffer DataBuffer = FSharedBuffer::MakeView(InData.GetData(), InData.Num());
	return PutZenData(*MakeLegacyZenKey(CacheKey), FCompositeBuffer(DataBuffer), Zen::EContentType::Binary);
}

FDerivedDataBackendInterface::EPutStatus
FZenDerivedDataBackend::PutZenData(const TCHAR* Uri, const FCompositeBuffer& InData, Zen::EContentType ContentType)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC_Put);
	COOK_STAT(auto Timer = UsageStats.TimePut());

	int64 ResponseCode = 0; 
	uint32 Attempts = 0;

	// Retry request until we get an accepted response or exhaust allowed number of attempts.
	while (ResponseCode == 0 && ++Attempts < MaxAttempts)
	{
		Zen::FZenScopedRequestPtr Request(RequestPool.Get());
		if (Request.IsValid())
		{
			Zen::FZenHttpRequest::Result Result = Request->PerformBlockingPut(Uri, InData, ContentType);
			ResponseCode = Request->GetResponseCode();

			if (Zen::IsSuccessCode(ResponseCode))
			{
				TRACE_COUNTER_ADD(ZenDDC_BytesSent, int64(Request->GetBytesSent()));
				COOK_STAT(Timer.AddHit(Request->GetBytesSent()));
				return EPutStatus::Cached;
			}

			if (!ShouldRetryOnError(ResponseCode))
			{
				return EPutStatus::NotCached;
			}

			ResponseCode = 0;
		}
	}

	return EPutStatus::NotCached;
}

FString FZenDerivedDataBackend::MakeLegacyZenKey(const TCHAR* CacheKey)
{
	FIoHash KeyHash = FIoHash::HashBuffer(CacheKey, FCString::Strlen(CacheKey) * sizeof(TCHAR));
	return FString::Printf(TEXT("/z$/legacy/%s"), *LexToString(KeyHash));
}

void FZenDerivedDataBackend::AppendZenUri(const FCacheKey& CacheKey, FStringBuilderBase& Out)
{
	Out << TEXT("/z$/") << CacheKey.Bucket << TEXT('/') << CacheKey.Hash;
}

void FZenDerivedDataBackend::AppendZenUri(const FCacheKey& CacheKey, const FPayloadId& PayloadId, FStringBuilderBase& Out)
{
	AppendZenUri(CacheKey, Out);
	Out << TEXT('/') << PayloadId;
}

void FZenDerivedDataBackend::AppendPolicyQueryString(ECachePolicy Policy, FStringBuilderBase& Uri)
{
	bool bQueryEmpty = true;
	bool bValueEmpty = true;
	auto AppendKey = [&Uri, &bQueryEmpty, &bValueEmpty](const TCHAR* Key)
	{
		if (bQueryEmpty)
		{
			TCHAR LastChar = Uri.Len() == 0 ? '\0' : Uri.LastChar();
			if (LastChar != '?' && LastChar != '&')
			{
				Uri << '?';
			}
			bQueryEmpty = false;
		}
		else
		{
			Uri << '&';
		}
		bValueEmpty = true;
		Uri << Key;
	};
	auto AppendValue = [&Uri, &bValueEmpty](const TCHAR* Value)
	{
		if (bValueEmpty)
		{
			bValueEmpty = false;
		}
		else
		{
			Uri << ',';
		}
		Uri << Value;
	};

	if (!EnumHasAllFlags(Policy, ECachePolicy::Query))
	{
		AppendKey(TEXT("query="));
		if (EnumHasAnyFlags(Policy, ECachePolicy::QueryLocal)) { AppendValue(TEXT("local")); }
		if (EnumHasAnyFlags(Policy, ECachePolicy::QueryRemote)) { AppendValue(TEXT("remote")); }
		if (!EnumHasAnyFlags(Policy, ECachePolicy::Query)) { AppendValue(TEXT("none")); }
	}
	if (!EnumHasAllFlags(Policy, ECachePolicy::Store))
	{
		AppendKey(TEXT("store="));
		if (EnumHasAnyFlags(Policy, ECachePolicy::StoreLocal)) { AppendValue(TEXT("local")); }
		if (EnumHasAnyFlags(Policy, ECachePolicy::StoreRemote)) { AppendValue(TEXT("remote")); }
		if (!EnumHasAnyFlags(Policy, ECachePolicy::Store)) { AppendValue(TEXT("none")); }
	}
	if (EnumHasAnyFlags(Policy, ECachePolicy::SkipData))
	{
		AppendKey(TEXT("skip="));
		if (EnumHasAllFlags(Policy, ECachePolicy::SkipData)) { AppendValue(TEXT("data")); }
		else
		{
			if (EnumHasAnyFlags(Policy, ECachePolicy::SkipMeta)) { AppendValue(TEXT("meta")); }
			if (EnumHasAnyFlags(Policy, ECachePolicy::SkipValue)) { AppendValue(TEXT("value")); }
			if (EnumHasAnyFlags(Policy, ECachePolicy::SkipAttachments)) { AppendValue(TEXT("attachments")); }
		}
	}
}

uint64 FZenDerivedDataBackend::MeasureCacheRecord(const FCacheRecord& Record)
{
	return Record.GetMeta().GetSize() +
		Record.GetValuePayload().GetRawSize() +
		Algo::TransformAccumulate(Record.GetAttachmentPayloads(), &FPayload::GetRawSize, uint64(0));
}

void FZenDerivedDataBackend::RemoveCachedData(const TCHAR* CacheKey, bool bTransient)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC_Remove);
	FString Uri = MakeLegacyZenKey(CacheKey);

	int64 ResponseCode = 0; 
	uint32 Attempts = 0;

	// Retry request until we get an accepted response or exhaust allowed number of attempts.
	while (ResponseCode == 0 && ++Attempts < MaxAttempts)
	{
		Zen::FZenScopedRequestPtr Request(RequestPool.Get());
		if (Request)
		{
			Zen::FZenHttpRequest::Result Result = Request->PerformBlockingDelete(*Uri);
			ResponseCode = Request->GetResponseCode();

			if (Zen::IsSuccessCode(ResponseCode))
			{
				return;
			}

			if (!ShouldRetryOnError(ResponseCode))
			{
				return;
			}

			ResponseCode = 0;
		}
	}
}

bool FZenDerivedDataBackend::IsWritable() const 
{
	return true;
}

FDerivedDataBackendInterface::ESpeedClass 
FZenDerivedDataBackend::GetSpeedClass() const
{
	return ESpeedClass::Fast;
}

TSharedRef<FDerivedDataCacheStatsNode> FZenDerivedDataBackend::GatherUsageStats() const
{
	TSharedRef<FDerivedDataCacheStatsNode> Usage = MakeShared<FDerivedDataCacheStatsNode>(this, FString::Printf(TEXT("%s.%s"), TEXT("ZenDDC"), *GetName()));
	Usage->Stats.Add(TEXT(""), UsageStats);

	return Usage;
}

bool FZenDerivedDataBackend::TryToPrefetch(TConstArrayView<FString> CacheKeys)
{
	return CachedDataProbablyExistsBatch(CacheKeys).CountSetBits() == CacheKeys.Num();
}

bool FZenDerivedDataBackend::WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData)
{
	return true;
}

bool FZenDerivedDataBackend::ApplyDebugOptions(FBackendDebugOptions& InOptions)
{
	DebugOptions = InOptions;
	return true;
}

bool FZenDerivedDataBackend::ShouldSimulateMiss(const TCHAR* InKey)
{
	if (DebugOptions.RandomMissRate == 0 && DebugOptions.SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	const FName Key(InKey);
	const uint32 Hash = GetTypeHash(Key);

	if (FScopeLock Lock(&MissedKeysCS); DebugMissedKeys.ContainsByHash(Hash, Key))
	{
		return true;
	}

	if (DebugOptions.ShouldSimulateMiss(InKey))
	{
		FScopeLock Lock(&MissedKeysCS);
		UE_LOG(LogDerivedDataCache, Verbose, TEXT("Simulating miss in %s for %s"), *GetName(), InKey);
		DebugMissedKeys.AddByHash(Hash, Key);
		return true;
	}

	return false;
}

bool FZenDerivedDataBackend::ShouldSimulateMiss(const FCacheKey& Key)
{
	if (DebugOptions.RandomMissRate == 0 && DebugOptions.SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	const uint32 Hash = GetTypeHash(Key);

	if (FScopeLock Lock(&MissedKeysCS); DebugMissedCacheKeys.ContainsByHash(Hash, Key))
	{
		return true;
	}

	if (DebugOptions.ShouldSimulateMiss(Key))
	{
		FScopeLock Lock(&MissedKeysCS);
		DebugMissedCacheKeys.AddByHash(Hash, Key);
		return true;
	}

	return false;
}

void FZenDerivedDataBackend::Put(
	TConstArrayView<FCacheRecord> Records,
	FStringView Context,
	ECachePolicy Policy,
	IRequestOwner& Owner,
	FOnCachePutComplete&& OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::PutCachedRecord);

	for (const FCacheRecord& Record : Records)
	{
		COOK_STAT(auto Timer = UsageStats.TimePut());
		bool bResult;
		if (ShouldSimulateMiss(Record.GetKey()))
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for put of %s from '%.*s'"),
				*GetName(), *WriteToString<96>(Record.GetKey()), Context.Len(), Context.GetData());
			bResult = false;
		}

		bResult = PutCacheRecord(Record, Context, Policy);

		if (bResult)
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache put complete for %s from '%.*s'"),
				*GetName(), *WriteToString<96>(Record.GetKey()), Context.Len(), Context.GetData());
			COOK_STAT(Timer.AddHit(MeasureCacheRecord(Record)));
			if (OnComplete)
			{
				OnComplete({ Record.GetKey(), EStatus::Ok });
			}
		}
		else
		{
			COOK_STAT(Timer.AddMiss(MeasureCacheRecord(Record)));
			if (OnComplete)
			{
				OnComplete({ Record.GetKey(), EStatus::Error });
			}
		}
	}
}

void FZenDerivedDataBackend::Get(
	TConstArrayView<FCacheKey> Keys,
	FStringView Context,
	FCacheRecordPolicy Policy,
	IRequestOwner& Owner,
	FOnCacheGetComplete&& OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::GetCacheRecord);
	TRACE_COUNTER_ADD(ZenDDC_CacheRecordRequestCount, int64(Keys.Num()));

	int32 TotalCompleted = 0;

	ForEachBatch(CacheRecordBatchSize, Keys.Num(),
		[this, &Keys, &Context, &Policy, &Owner, &OnComplete, &TotalCompleted](int32 BatchFirst, int32 BatchLast)
	{
		TRACE_COUNTER_ADD(ZenDDC_Get, int64(BatchLast) - int64(BatchFirst) + 1);
		COOK_STAT(auto Timer = UsageStats.TimeGet());
		
		FCbWriter BatchRequest;
		BatchRequest.BeginObject();
		{
			BatchRequest << "Method"_ASV << "GetCacheRecords";
			BatchRequest.BeginObject("Params"_ASV);
			{
				BatchRequest.BeginArray("CacheKeys"_ASV);
				for (int32 KeyIndex = BatchFirst; KeyIndex <= BatchLast; KeyIndex++)
				{
					const FCacheKey& Key = Keys[KeyIndex];

					BatchRequest.BeginObject();
					BatchRequest << "Bucket"_ASV << Key.Bucket.ToString();
					BatchRequest << "Hash"_ASV << Key.Hash;
					BatchRequest.EndObject();
				}
				BatchRequest.EndArray();

				BatchRequest.BeginObject("Policy"_ASV);
				{
					BatchRequest << "RecordPolicy"_ASV << static_cast<uint32>(Policy.GetRecordPolicy());
					BatchRequest << "DefaultPayloadPolicy"_ASV << static_cast<uint32>(Policy.GetDefaultPayloadPolicy());
					
					TConstArrayView<FCachePayloadPolicy> PayloadPolicies = Policy.GetPayloadPolicies();
					if (PayloadPolicies.Num())
					{
						BatchRequest.BeginArray("PayloadPolicies"_ASV);
						for (const FCachePayloadPolicy& PayloadPolicy : PayloadPolicies)
						{
							BatchRequest.BeginObject();
							BatchRequest.AddObjectId("Id"_ASV, PayloadPolicy.Id);
							BatchRequest << "Policy"_ASV << static_cast<uint32>(PayloadPolicy.Policy);
							BatchRequest.EndObject();
						}
						BatchRequest.EndArray();
					}
				}
				BatchRequest.EndObject();
			}
			BatchRequest.EndObject();
		}
		BatchRequest.EndObject();

		FCbPackage BatchResponse;
		Zen::FZenHttpRequest::Result HttpResult = Zen::FZenHttpRequest::Result::Failed;

		{
			Zen::FZenScopedRequestPtr Request(RequestPool.Get());
			HttpResult = Request->PerformRpc(TEXT("/z$/$rpc"_SV), BatchRequest.Save().AsObject(), BatchResponse);
		}

		if (HttpResult == Zen::FZenHttpRequest::Result::Success)
		{
			const FCbObject& ResponseObj = BatchResponse.GetObject();
			
			int32 KeyIndex = BatchFirst;
			for (FCbFieldView RecordView : ResponseObj["Result"_ASV])
			{
				const FCacheKey& Key = Keys[KeyIndex++];
				
				FOptionalCacheRecord Record;

				if (!RecordView.IsNull())
				{
					if (ShouldSimulateMiss(Key))
					{
						UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for put of '%s' from '%.*s'"),
							*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
					}
					else
					{
						Record = FCacheRecord::Load(BatchResponse, RecordView.AsObjectView()); 
					}
				}

				if (Record)
				{
					UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache hit for '%s' from '%.*s'"),
						*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
					
					TRACE_COUNTER_ADD(ZenDDC_GetHit, int64(1));
					int64 ReceivedSize = MeasureCacheRecord(Record.Get());
					TRACE_COUNTER_ADD(ZenDDC_BytesReceived, ReceivedSize);
					COOK_STAT(Timer.AddHit(ReceivedSize));

					if (OnComplete)
					{
						OnComplete({ MoveTemp(Record).Get(), EStatus::Ok });
					}
				}
				else
				{
					UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache miss for '%s' from '%.*s'"),
						*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
					
					if (OnComplete)
					{
						OnComplete({ FCacheRecordBuilder(Key).Build(), EStatus::Error });
					}
				}
				
				TotalCompleted++;
			}
		}
		else
		{
			for (int32 KeyIndex = BatchFirst; KeyIndex <= BatchLast; KeyIndex++)
			{
				const FCacheKey& Key = Keys[KeyIndex];

				UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache miss for '%s' from '%.*s'"),
					*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
					
				if (OnComplete)
				{
					OnComplete({ FCacheRecordBuilder(Key).Build(), EStatus::Error });
				}
				
				TotalCompleted++;
			}
		}
	});
	
	UE_CLOG(TotalCompleted != Keys.Num(), LogDerivedDataCache, Warning, TEXT("Only '%d/%d' cache record request(s) completed"), TotalCompleted, Keys.Num());
	TRACE_COUNTER_SUBTRACT(ZenDDC_CacheRecordRequestCount, int64(Keys.Num()));
}

void FZenDerivedDataBackend::GetChunks(
	TConstArrayView<FCacheChunkRequest> Chunks,
	FStringView Context,
	IRequestOwner& Owner,
	FOnCacheGetChunkComplete&& OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ZenDDC::GetChunks);
	TRACE_COUNTER_ADD(ZenDDC_ChunkRequestCount, int64(Chunks.Num()));

	TArray<FCacheChunkRequest, TInlineAllocator<16>> SortedChunks(Chunks);
	SortedChunks.StableSort(TChunkLess());

	int32 TotalCompleted = 0;

	ForEachBatch(CacheChunksBatchSize, SortedChunks.Num(),
		[this, &SortedChunks, &Context, &Owner, &OnComplete, &TotalCompleted](int32 BatchFirst, int32 BatchLast)
	{
		TRACE_COUNTER_ADD(ZenDDC_Get, int64(BatchLast) - int64(BatchFirst) + 1);
		COOK_STAT(auto Timer = UsageStats.TimeGet());

		FCbWriter BatchRequest;
		BatchRequest.BeginObject();
		{
			BatchRequest << "Method"_ASV << "GetCachePayloads";
			BatchRequest.BeginObject("Params"_ASV);
			{
				BatchRequest.BeginArray("ChunkRequests"_ASV);
				for (int32 ChunkIndex = BatchFirst; ChunkIndex <= BatchLast; ChunkIndex++)
				{
					const FCacheChunkRequest& Chunk = SortedChunks[ChunkIndex];
					
					BatchRequest.BeginObject();
					
					BatchRequest.BeginObject("Key"_ASV);
					BatchRequest << "Bucket"_ASV << Chunk.Key.Bucket.ToString();
					BatchRequest << "Hash"_ASV << Chunk.Key.Hash;
					BatchRequest.EndObject();

					BatchRequest.AddObjectId("PayloadId"_ASV, Chunk.Id);
					BatchRequest << "RawOffset"_ASV << Chunk.RawOffset;
					BatchRequest << "RawSize"_ASV << Chunk.RawSize;
					BatchRequest << "Policy"_ASV << static_cast<uint32>(Chunk.Policy);

					BatchRequest.EndObject();
				}
				BatchRequest.EndArray();
			}
			BatchRequest.EndObject();
		}
		BatchRequest.EndObject();

		FCbPackage BatchResponse;
		Zen::FZenHttpRequest::Result HttpResult = Zen::FZenHttpRequest::Result::Failed;

		{
			Zen::FZenScopedRequestPtr Request(RequestPool.Get());
			HttpResult = Request->PerformRpc(TEXT("/z$/$rpc"_SV), BatchRequest.Save().AsObject(), BatchResponse);
		}

		if (HttpResult == Zen::FZenHttpRequest::Result::Success)
		{
			const FCbObject& ResponseObj = BatchResponse.GetObject();

			int32 ChunkIndex = BatchFirst;
			for (FCbFieldView HashView : ResponseObj["Result"_ASV])
			{
				const FCacheChunkRequest& Chunk = SortedChunks[ChunkIndex++];

				if (ShouldSimulateMiss(Chunk.Key))
				{
					UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for get of '%s' from '%.*s'"),
						*GetName(), *WriteToString<96>(Chunk.Key ,'/', Chunk.Id), Context.Len(), Context.GetData());
					
					if (OnComplete)
					{
						OnComplete({ Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Error });
					}
				}
				else if (const FCbAttachment* Attachment = BatchResponse.FindAttachment(HashView.AsHash()))
				{
					const FCompressedBuffer& CompressedBuffer = Attachment->AsCompressedBinary();
					FSharedBuffer Buffer = CompressedBuffer.Decompress(Chunk.RawOffset, Chunk.RawSize);
					
					UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Cache hit for '%s' from '%.*s'"),
						*GetName(), *WriteToString<96>(Chunk.Key, '/', Chunk.Id), Context.Len(), Context.GetData());

					const uint64 RawSize = Buffer.GetSize();
					TRACE_COUNTER_ADD(ZenDDC_GetHit, int64(1));
					TRACE_COUNTER_ADD(ZenDDC_BytesReceived, RawSize);
					COOK_STAT(Timer.AddHit(RawSize));
					
					if (OnComplete)
					{
						OnComplete({Chunk.Key, Chunk.Id, Chunk.RawOffset, RawSize, CompressedBuffer.GetRawHash(), MoveTemp(Buffer), EStatus::Ok});
					}
				}
				else
				{
					UE_LOG(LogDerivedDataCache, Display, TEXT("%s: Cache miss with missing payload '%s' for '%s' from '%.*s'"),
						*GetName(), *WriteToString<16>(Chunk.Id), *WriteToString<96>(Chunk.Key), Context.Len(), Context.GetData());
					
					if (OnComplete)
					{
						OnComplete({ Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Error });
					}
				}

				TotalCompleted++;
			}
		}
		else
		{
			for (int32 ChunkIndex = BatchFirst; ChunkIndex <= BatchLast; ChunkIndex++)
			{
				const FCacheChunkRequest& Chunk = SortedChunks[ChunkIndex];
				
				UE_LOG(LogDerivedDataCache, Display, TEXT("%s: Cache miss with missing payload '%s' for '%s' from '%.*s'"),
					*GetName(), *WriteToString<16>(Chunk.Id), *WriteToString<96>(Chunk.Key), Context.Len(), Context.GetData());
				
				if (OnComplete)
				{
					OnComplete({ Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Error });
				}
				
				TotalCompleted++;
			}
		}
	});

	UE_CLOG(TotalCompleted != SortedChunks.Num(), LogDerivedDataCache, Warning, TEXT("Only '%d/%d' cache chunk request(s) completed"), TotalCompleted, SortedChunks.Num());
	TRACE_COUNTER_SUBTRACT(ZenDDC_ChunkRequestCount, int64(Chunks.Num()));
}

bool FZenDerivedDataBackend::PutCacheRecord(const FCacheRecord& Record, FStringView Context, ECachePolicy Policy)
{
	const FCacheKey& Key = Record.GetKey();
	FCbPackage Package = Record.Save();
	FBufferArchive Ar;
	Package.Save(Ar);
	FCompositeBuffer Buffer = FCompositeBuffer(FSharedBuffer::MakeView(Ar.GetData(), Ar.Num()));
	TStringBuilder<256> Uri;
	AppendZenUri(Record.GetKey(), Uri);
	AppendPolicyQueryString(Policy, Uri);
	if (PutZenData(Uri.ToString(), Buffer, Zen::EContentType::CbPackage)
		!= FDerivedDataBackendInterface::EPutStatus::Cached)
	{
		return false;
	}

	return true;
}

} // namespace UE::DerivedData::Backends

#endif //WITH_HTTP_DDC_BACKEND
