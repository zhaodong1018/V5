// Copyright Epic Games, Inc. All Rights Reserved.

#include "MemoryDerivedDataBackend.h"

#include "Algo/Accumulate.h"
#include "Algo/AllOf.h"
#include "DerivedDataPayload.h"
#include "Misc/ScopeExit.h"
#include "Serialization/CompactBinary.h"
#include "Templates/UniquePtr.h"
#include "Misc/ScopeRWLock.h"

namespace UE::DerivedData::Backends
{

FMemoryDerivedDataBackend::FMemoryDerivedDataBackend(const TCHAR* InName, int64 InMaxCacheSize, bool bInCanBeDisabled)
	: Name(InName)
	, MaxCacheSize(InMaxCacheSize)
	, bDisabled( false )
	, CurrentCacheSize( SerializationSpecificDataSize )
	, bMaxSizeExceeded(false)
	, bCanBeDisabled(bInCanBeDisabled)
{
}

FMemoryDerivedDataBackend::~FMemoryDerivedDataBackend()
{
	bShuttingDown = true;
	Disable();
}

bool FMemoryDerivedDataBackend::IsWritable() const
{
	return !bDisabled;
}

FDerivedDataBackendInterface::ESpeedClass FMemoryDerivedDataBackend::GetSpeedClass() const
{
	return ESpeedClass::Local;
}

bool FMemoryDerivedDataBackend::CachedDataProbablyExists(const TCHAR* CacheKey)
{
	// See comments on the declaration of bCanBeDisabled variable.
	if (bCanBeDisabled)
	{
		return false;
	}

	COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());

	if (ShouldSimulateMiss(CacheKey))
	{
		return false;
	}

	if (bDisabled)
	{
		return false;
	}

	FReadScopeLock ScopeLock(SynchronizationObject);
	bool Result = CacheItems.Contains(FString(CacheKey));
	if (Result)
	{
		COOK_STAT(Timer.AddHit(0));
	}
	return Result;
}

bool FMemoryDerivedDataBackend::GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData)
{
	COOK_STAT(auto Timer = UsageStats.TimeGet());

	if (ShouldSimulateMiss(CacheKey))
	{
		return false;
	}
	
	if (!bDisabled)
	{
		FReadScopeLock ScopeLock(SynchronizationObject);

		FCacheValue* Item = CacheItems.FindRef(FString(CacheKey));
		if (Item)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FMemoryDerivedDataBackend::GetCachedData);
			FReadScopeLock ItemLock(Item->DataLock);

			OutData = Item->Data;
			Item->Age = 0;
			check(OutData.Num());
			COOK_STAT(Timer.AddHit(OutData.Num()));
			return true;
		}
	}
	OutData.Empty();
	return false;
}

bool FMemoryDerivedDataBackend::TryToPrefetch(TConstArrayView<FString> CacheKeys)
{
	return CachedDataProbablyExistsBatch(CacheKeys).CountSetBits() == CacheKeys.Num();
}

bool FMemoryDerivedDataBackend::WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData)
{
	if (bDisabled || bMaxSizeExceeded)
	{
		return false;
	}

	return true;
}

FDerivedDataBackendInterface::EPutStatus FMemoryDerivedDataBackend::PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMemoryDerivedDataBackend::PutCachedData);
	COOK_STAT(auto Timer = UsageStats.TimePut());

	FString Key;
	{
		FReadScopeLock ReadScopeLock(SynchronizationObject);

		if (ShouldSimulateMiss(CacheKey))
		{
			return EPutStatus::Skipped;
		}

		// Should never hit this as higher level code should be checking..
		if (!WouldCache(CacheKey, InData))
		{
			//UE_LOG(LogDerivedDataCache, Warning, TEXT("WouldCache was not called prior to attempted Put!"));
			return EPutStatus::NotCached;
		}

		Key = CacheKey;
		FCacheValue* Item = CacheItems.FindRef(Key);
		if (Item)
		{
			//check(Item->Data == InData); // any second attempt to push data should be identical data
			return EPutStatus::Cached;
		}
	}
	
	// Compute the size of the cache before copying the data to avoid alloc/memcpy of something we might throw away
	// if we bust the cache size.
	int32 CacheValueSize = CalcSerializedCacheValueSize(Key, InData);

	FCacheValue* Val = nullptr;

	{
		FWriteScopeLock WriteScopeLock(SynchronizationObject);

		// check if we haven't exceeded the MaxCacheSize
		if (MaxCacheSize > 0 && (CurrentCacheSize + CacheValueSize) > MaxCacheSize)
		{
			UE_LOG(LogDerivedDataCache, Display, TEXT("Failed to cache data. Maximum cache size reached. CurrentSize %d kb / MaxSize: %d kb"), CurrentCacheSize / 1024, MaxCacheSize / 1024);
			bMaxSizeExceeded = true;
			return EPutStatus::NotCached;
		}

		if (CacheItems.Contains(Key))
		{
			// Another thread already beat us, just return.
			return EPutStatus::Cached;
		}

		COOK_STAT(Timer.AddHit(InData.Num()));
		Val = new FCacheValue(InData.Num());
		// Make sure no other thread can access the data until we finish copying it
		Val->DataLock.WriteLock();
		CacheItems.Add(Key, Val);

		CurrentCacheSize += CacheValueSize;
	}

	// Data is copied outside of the lock so that we only lock
	// for this specific key instead of always locking all keys by default.
	Val->Data = InData;
	// It is now safe for other thread to access this data, unlock
	Val->DataLock.WriteUnlock();

	return EPutStatus::Cached;
}

void FMemoryDerivedDataBackend::RemoveCachedData(const TCHAR* CacheKey, bool bTransient)
{
	if (bDisabled || bTransient)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FMemoryDerivedDataBackend::RemoveCachedData);
	FString Key(CacheKey);
	FCacheValue* Item = nullptr;
	{
		FWriteScopeLock WriteScopeLock(SynchronizationObject);
		
		if (CacheItems.RemoveAndCopyValue(Key, Item) && Item)
		{
			CurrentCacheSize -= CalcSerializedCacheValueSize(Key, *Item);
			bMaxSizeExceeded = false;
		}
	}

	if (Item)
	{
		// Just make sure any other thread has finished writing or reading the data before deleting.
		Item->DataLock.WriteLock();

		// Avoid deleting the item with a locked FRWLock, unlocking is safe because
		// the item is now unreachable from other threads
		Item->DataLock.WriteUnlock();
		delete Item;
	}
}

bool FMemoryDerivedDataBackend::SaveCache(const TCHAR* Filename)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMemoryDerivedDataBackend::SaveCache);

	double StartTime = FPlatformTime::Seconds();
	TUniquePtr<FArchive> SaverArchive(IFileManager::Get().CreateFileWriter(Filename, FILEWRITE_EvenIfReadOnly));
	if (!SaverArchive)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Could not save memory cache %s."), Filename);
		return false;
	}

	FArchive& Saver = *SaverArchive;
	uint32 Magic = MemCache_Magic64;
	Saver << Magic;
	const int64 DataStartOffset = Saver.Tell();
	{
		FWriteScopeLock ScopeLock(SynchronizationObject);
		check(!bDisabled);
		for (TMap<FString, FCacheValue*>::TIterator It(CacheItems); It; ++It )
		{
			Saver << It.Key();
			Saver << It.Value()->Age;
			FReadScopeLock ReadScopeLock(It.Value()->DataLock);
			Saver << It.Value()->Data;
		}
	}
	const int64 DataSize = Saver.Tell(); // Everything except the footer
	int64 Size = DataSize;
	uint32 Crc = MemCache_Magic64; // Crc takes more time than I want to spend  FCrc::MemCrc_DEPRECATED(&Buffer[0], Size);
	Saver << Size;
	Saver << Crc;

	check(SerializationSpecificDataSize + DataSize <= MaxCacheSize || MaxCacheSize <= 0);

	UE_LOG(LogDerivedDataCache, Log, TEXT("Saved boot cache %4.2fs %lldMB %s."), float(FPlatformTime::Seconds() - StartTime), DataSize / (1024 * 1024), Filename);
	return true;
}

bool FMemoryDerivedDataBackend::LoadCache(const TCHAR* Filename)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMemoryDerivedDataBackend::LoadCache);

	double StartTime = FPlatformTime::Seconds();
	const int64 FileSize = IFileManager::Get().FileSize(Filename);
	if (FileSize < 0)
	{
		UE_LOG(LogDerivedDataCache, Warning, TEXT("Could not find memory cache %s."), Filename);
		return false;
	}
	// We test 3 * uint32 which is the old format (< SerializationSpecificDataSize). We'll test
	// against SerializationSpecificDataSize later when we read the magic number from the cache.
	if (FileSize < sizeof(uint32) * 3)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Memory cache was corrputed (short) %s."), Filename);
		return false;
	}
	if (FileSize > MaxCacheSize*2 && MaxCacheSize > 0)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Refusing to load DDC cache %s. Size exceeds doubled MaxCacheSize."), Filename);
		return false;
	}

	TUniquePtr<FArchive> LoaderArchive(IFileManager::Get().CreateFileReader(Filename));
	if (!LoaderArchive)
	{
		UE_LOG(LogDerivedDataCache, Warning, TEXT("Could not read memory cache %s."), Filename);
		return false;
	}

	FArchive& Loader = *LoaderArchive;
	uint32 Magic = 0;
	Loader << Magic;
	if (Magic != MemCache_Magic && Magic != MemCache_Magic64)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Memory cache was corrputed (magic) %s."), Filename);
		return false;
	}
	// Check the file size again, this time against the correct minimum size.
	if (Magic == MemCache_Magic64 && FileSize < SerializationSpecificDataSize)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Memory cache was corrputed (short) %s."), Filename);
		return false;
	}
	// Calculate expected DataSize based on the magic number (footer size difference)
	const int64 DataSize = FileSize - (Magic == MemCache_Magic64 ? (SerializationSpecificDataSize - sizeof(uint32)) : (sizeof(uint32) * 2));		
	Loader.Seek(DataSize);
	int64 Size = 0;
	uint32 Crc = 0;
	if (Magic == MemCache_Magic64)
	{
		Loader << Size;
	}
	else
	{
		uint32 Size32 = 0;
		Loader << Size32;
		Size = (int64)Size32;
	}
	Loader << Crc;
	if (Size != DataSize)
	{
		UE_LOG(LogDerivedDataCache, Error, TEXT("Memory cache was corrputed (size) %s."), Filename);
		return false;
	}
	if ((Crc != MemCache_Magic && Crc != MemCache_Magic64) || Crc != Magic)
	{
		UE_LOG(LogDerivedDataCache, Warning, TEXT("Memory cache was corrputed (crc) %s."), Filename);
		return false;
	}
	// Seek to data start offset (skip magic number)
	Loader.Seek(sizeof(uint32));
	{
		TArray<uint8> Working;
		FWriteScopeLock ScopeLock(SynchronizationObject);
		check(!bDisabled);
		while (Loader.Tell() < DataSize)
		{
			FString Key;
			int32 Age;
			Loader << Key;
			Loader << Age;
			Age++;
			Loader << Working;
			if (Age < MaxAge)
			{
				CacheItems.Add(Key, new FCacheValue(Working.Num(), Age))->Data = MoveTemp(Working);
			}
			Working.Reset();
		}
		// these are just a double check on ending correctly
		if (Magic == MemCache_Magic64)
		{
			Loader << Size;
		}
		else
		{
			uint32 Size32 = 0;
			Loader << Size32;
			Size = (int64)Size32;
		}
		Loader << Crc;

		CurrentCacheSize = FileSize;
		CacheFilename = Filename;
	}

	UE_LOG(LogDerivedDataCache, Log, TEXT("Loaded boot cache %4.2fs %lldMB %s."), float(FPlatformTime::Seconds() - StartTime), DataSize / (1024 * 1024), Filename);
	return true;
}

void FMemoryDerivedDataBackend::Disable()
{
	check(bCanBeDisabled || bShuttingDown);
	FWriteScopeLock ScopeLock(SynchronizationObject);
	bDisabled = true;
	for (TMap<FString,FCacheValue*>::TIterator It(CacheItems); It; ++It )
	{
		delete It.Value();
	}
	CacheItems.Empty();

	CurrentCacheSize = SerializationSpecificDataSize;
}

TSharedRef<FDerivedDataCacheStatsNode> FMemoryDerivedDataBackend::GatherUsageStats() const
{
	TSharedRef<FDerivedDataCacheStatsNode> Usage = MakeShared<FDerivedDataCacheStatsNode>(this, FString::Printf(TEXT("%s.%s"), TEXT("MemoryBackend"), *CacheFilename));
	Usage->Stats.Add(TEXT(""), UsageStats);

	return Usage;
}

bool FMemoryDerivedDataBackend::ApplyDebugOptions(FBackendDebugOptions& InOptions)
{
	DebugOptions = InOptions;
	return true;
}

bool FMemoryDerivedDataBackend::ShouldSimulateMiss(const TCHAR* InKey)
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

bool FMemoryDerivedDataBackend::ShouldSimulateMiss(const FCacheKey& Key)
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

int64 FMemoryDerivedDataBackend::CalcRawCacheRecordSize(const FCacheRecord& Record) const
{
	const uint64 ValueSize = Record.GetValuePayload().GetRawSize();
	return int64(Algo::TransformAccumulate(Record.GetAttachmentPayloads(), &FPayload::GetRawSize, ValueSize));
}

int64 FMemoryDerivedDataBackend::CalcSerializedCacheRecordSize(const FCacheRecord& Record) const
{
	// Estimate the serialized size of the cache record.
	uint64 TotalSize = 20;
	TotalSize += Record.GetKey().Bucket.ToString().Len();
	TotalSize += Record.GetMeta().GetSize();
	const auto CalcCachePayloadSize = [](const FPayload& Payload)
	{
		return Payload ? Payload.GetData().GetCompressedSize() + 32 : 0;
	};
	TotalSize += CalcCachePayloadSize(Record.GetValuePayload());
	TotalSize += Algo::TransformAccumulate(Record.GetAttachmentPayloads(), CalcCachePayloadSize, uint64(0));
	return int64(TotalSize);
}

void FMemoryDerivedDataBackend::Put(
	TConstArrayView<FCacheRecord> Records,
	FStringView Context,
	ECachePolicy Policy,
	IRequestOwner& Owner,
	FOnCachePutComplete&& OnComplete)
{
	for (const FCacheRecord& Record : Records)
	{
		const FCacheKey& Key = Record.GetKey();
		EStatus Status = EStatus::Error;
		ON_SCOPE_EXIT
		{
			if (OnComplete)
			{
				OnComplete({Key, Status});
			}
		};

		if (ShouldSimulateMiss(Key))
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for put of %s from '%.*s'"),
				*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
			continue;
		}

		const FPayload& Value = Record.GetValuePayload();
		const TConstArrayView<FPayload> Attachments = Record.GetAttachmentPayloads();

		if ((Value && !Value.HasData()) || !Algo::AllOf(Attachments, &FPayload::HasData))
		{
			continue;
		}

		if (!Value && Attachments.IsEmpty())
		{
			FWriteScopeLock ScopeLock(SynchronizationObject);
			if (bDisabled)
			{
				continue;
			}
			if (const FCacheRecord* Existing = CacheRecords.Find(Key))
			{
				CurrentCacheSize -= CalcSerializedCacheRecordSize(*Existing);
				bMaxSizeExceeded = false;
			}
			Status = EStatus::Ok;
		}
		else
		{
			COOK_STAT(auto Timer = UsageStats.TimePut());
			const int64 RecordSize = CalcSerializedCacheRecordSize(Record);

			FWriteScopeLock ScopeLock(SynchronizationObject);
			Status = CacheRecords.Contains(Key) ? EStatus::Ok : EStatus::Error;
			if (bDisabled || Status == EStatus::Ok)
			{
				continue;
			}
			if (MaxCacheSize > 0 && (CurrentCacheSize + RecordSize) > MaxCacheSize)
			{
				UE_LOG(LogDerivedDataCache, Display, TEXT("Failed to cache data. Maximum cache size reached. CurrentSize %" INT64_FMT " KiB / MaxSize: %" INT64_FMT " KiB"), CurrentCacheSize / 1024, MaxCacheSize / 1024);
				bMaxSizeExceeded = true;
				continue;
			}

			CurrentCacheSize += RecordSize;
			CacheRecords.Add(Record);
			COOK_STAT(Timer.AddHit(RecordSize));
			Status = EStatus::Ok;
		}
	}
}

void FMemoryDerivedDataBackend::Get(
	TConstArrayView<FCacheKey> Keys,
	FStringView Context,
	FCacheRecordPolicy Policy,
	IRequestOwner& Owner,
	FOnCacheGetComplete&& OnComplete)
{
	for (const FCacheKey& Key : Keys)
	{
		const bool bExistsOnly = EnumHasAllFlags(Policy.GetRecordPolicy(), ECachePolicy::SkipData);
		COOK_STAT(auto Timer = bExistsOnly ? UsageStats.TimeProbablyExists() : UsageStats.TimeGet());
		FOptionalCacheRecord Record;
		EStatus Status = EStatus::Error;
		if (ShouldSimulateMiss(Key))
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for get of %s from '%.*s'"),
				*GetName(), *WriteToString<96>(Key), Context.Len(), Context.GetData());
		}
		else if (FWriteScopeLock ScopeLock(SynchronizationObject); const FCacheRecord* CacheRecord = CacheRecords.Find(Key))
		{
			Status = EStatus::Ok;
			Record = *CacheRecord;
		}
		if (Record)
		{
			if (const FPayload& Payload = Record.Get().GetValuePayload();
				!Payload.HasData() && !EnumHasAnyFlags(Policy.GetPayloadPolicy(Payload.GetId()), ECachePolicy::SkipValue))
			{
				Status = EStatus::Error;
				if (!EnumHasAllFlags(Policy.GetPayloadPolicy(Payload.GetId()), ECachePolicy::PartialOnError))
				{
					Record.Reset();
				}
			}
			if (Record)
			{
				for (const FPayload& Payload : Record.Get().GetAttachmentPayloads())
				{
					if (!Payload.HasData() && !EnumHasAnyFlags(Policy.GetPayloadPolicy(Payload.GetId()), ECachePolicy::SkipAttachments))
					{
						Status = EStatus::Error;
						if (!EnumHasAllFlags(Policy.GetPayloadPolicy(Payload.GetId()), ECachePolicy::PartialOnError))
						{
							Record.Reset();
							break;
						}
					}
				}
			}
		}
		if (Record)
		{
			COOK_STAT(Timer.AddHit(CalcRawCacheRecordSize(Record.Get())));
			if (OnComplete)
			{
				OnComplete({MoveTemp(Record).Get(), Status});
			}
		}
		else
		{
			if (OnComplete)
			{
				OnComplete({FCacheRecordBuilder(Key).Build(), EStatus::Error});
			}
		}
	}
}

void FMemoryDerivedDataBackend::GetChunks(
	TConstArrayView<FCacheChunkRequest> Chunks,
	FStringView Context,
	IRequestOwner& Owner,
	FOnCacheGetChunkComplete&& OnComplete)
{
	for (const FCacheChunkRequest& Chunk : Chunks)
	{
		const bool bExistsOnly = EnumHasAnyFlags(Chunk.Policy, ECachePolicy::SkipValue);
		COOK_STAT(auto Timer = bExistsOnly ? UsageStats.TimeProbablyExists() : UsageStats.TimeGet());
		FPayload Payload;
		if (ShouldSimulateMiss(Chunk.Key))
		{
			UE_LOG(LogDerivedDataCache, Verbose, TEXT("%s: Simulated miss for get of %s from '%.*s'"),
				*GetName(), *WriteToString<96>(Chunk.Key, '/', Chunk.Id), Context.Len(), Context.GetData());
		}
		else if (FWriteScopeLock ScopeLock(SynchronizationObject); const FCacheRecord* Record = CacheRecords.Find(Chunk.Key))
		{
			Payload = Record->GetAttachmentPayload(Chunk.Id);
		}
		if (Payload && Chunk.RawOffset <= Payload.GetRawSize())
		{
			const uint64 RawSize = FMath::Min(Payload.GetRawSize() - Chunk.RawOffset, Chunk.RawSize);
			COOK_STAT(Timer.AddHit(RawSize));
			if (OnComplete)
			{
				FUniqueBuffer Buffer;
				if (Payload.HasData() && !bExistsOnly)
				{
					Buffer = FUniqueBuffer::Alloc(RawSize);
					Payload.GetData().DecompressToComposite().CopyTo(Buffer, Chunk.RawOffset);
				}
				const EStatus Status = bExistsOnly || Buffer ? EStatus::Ok : EStatus::Error;
				OnComplete({Chunk.Key, Chunk.Id, Chunk.RawOffset,
					RawSize, Payload.GetRawHash(), Buffer.MoveToShared(), Status});
			}
		}
		else
		{
			if (OnComplete)
			{
				OnComplete({Chunk.Key, Chunk.Id, Chunk.RawOffset, 0, {}, {}, EStatus::Error});
			}
		}
	}
}

} // UE::DerivedData::Backends
