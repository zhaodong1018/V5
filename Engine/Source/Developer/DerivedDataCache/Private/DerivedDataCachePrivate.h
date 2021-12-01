// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/StringFwd.h"
#include "Misc/AsciiSet.h"

class FDerivedDataCacheInterface;

namespace UE::DerivedData { class FCacheRecord; }
namespace UE::DerivedData { class ICache; }
namespace UE::DerivedData { struct FCacheKey; }

namespace UE::DerivedData::Private
{

// Implemented in DerivedDataCache.cpp
ICache* CreateCache(FDerivedDataCacheInterface** OutLegacyCache);

// Implemented in DerivedDataCacheRecord.cpp
uint64 GetCacheRecordCompressedSize(const FCacheRecord& Record);
uint64 GetCacheRecordTotalRawSize(const FCacheRecord& Record);
uint64 GetCacheRecordRawSize(const FCacheRecord& Record);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename CharType>
inline bool IsValidCacheBucketName(TStringView<CharType> Name)
{
	constexpr FAsciiSet Valid("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
	return !Name.IsEmpty() && Name.Len() < 256 && FAsciiSet::HasOnly(Name, Valid);
}

} // UE::DerivedData::Private
