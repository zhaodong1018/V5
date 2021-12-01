// Copyright Epic Games, Inc. All Rights Reserved.

#include "VirtualizationDDCBackend.h"

#include "Misc/Parse.h"
#include "Virtualization/PayloadId.h"
#include "DerivedDataCache.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataPayload.h"
#include "DerivedDataRequestOwner.h"

namespace UE::Virtualization
{

/** Utility function to help convert from UE::Virtualization types to UE::DerivedData::FPayload */
UE::DerivedData::FPayload ToDDCPayload(const FPayloadId& Id, const FCompressedBuffer& Payload)
{
	UE::DerivedData::FPayloadId DDCPayloadId = UE::DerivedData::FPayloadId::FromHash(Id.GetIdentifier());

	return UE::DerivedData::FPayload(DDCPayloadId, Payload);
}

FDDCBackend::FDDCBackend(FStringView ConfigName, FStringView InDebugName)
: IVirtualizationBackend(ConfigName, InDebugName, EOperations::Both)
, BucketName(TEXT("BulkData"))
, TransferPolicy(UE::DerivedData::ECachePolicy::None)
, QueryPolicy(UE::DerivedData::ECachePolicy::None)
{
	
}

bool FDDCBackend::Initialize(const FString& ConfigEntry)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Initialize::Initialize);

	if (!FParse::Value(*ConfigEntry, TEXT("Bucket="), BucketName))
	{
		UE_LOG(LogVirtualization, Fatal, TEXT("[%s] 'Bucket=' not found in the config file"), *GetDebugName());
	}

	bool bAllowLocal = true;
	if (FParse::Bool(*ConfigEntry, TEXT("LocalStorage="), bAllowLocal))
	{
		UE_LOG(LogVirtualization, Log, TEXT("[%s] Use of local storage set to '%s"), *GetDebugName(), bAllowLocal ? TEXT("true") : TEXT("false"));
	}

	bool bAllowRemote = true;
	if (FParse::Bool(*ConfigEntry, TEXT("RemoteStorage="), bAllowRemote))
	{
		UE_LOG(LogVirtualization, Log, TEXT("[%s] Use of remote storage set to '%s"), *GetDebugName(), bAllowRemote ? TEXT("true") : TEXT("false"));
	}

	if (!bAllowLocal && !bAllowRemote)
	{
		UE_LOG(LogVirtualization, Fatal, TEXT("[%s] LocalStorage and RemoteStorage cannot both be disabled"), *GetDebugName());
		return false;
	}

	if (bAllowLocal)
	{
		TransferPolicy |= UE::DerivedData::ECachePolicy::Local;
		QueryPolicy |= UE::DerivedData::ECachePolicy::QueryLocal;
	}

	if (bAllowRemote)
	{
		TransferPolicy |= UE::DerivedData::ECachePolicy::Remote;
		QueryPolicy |= UE::DerivedData::ECachePolicy::QueryRemote;
	}

	Bucket = UE::DerivedData::FCacheBucket(BucketName);

	return true;	
}

EPushResult FDDCBackend::PushData(const FPayloadId& Id, const FCompressedBuffer& Payload, const FPackagePath& PackageContext)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::PushData);

	if (DoesExist(Id))
	{
		UE_LOG(LogVirtualization, Verbose, TEXT("[%s] Already has a copy of the payload '%s'."), *GetDebugName(), *Id.ToString());
		return EPushResult::PayloadAlreadyExisted;
	}

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();
	
	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket;
	Key.Hash = Id.GetIdentifier();

	UE::DerivedData::FPayload DDCPayload = ToDDCPayload(Id, Payload);
	check(DDCPayload.GetRawHash() == Id.GetIdentifier());

	UE::DerivedData::FCacheRecordBuilder RecordBuilder(Key);
	RecordBuilder.SetValue(DDCPayload);

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);

	UE::DerivedData::FCachePutCompleteParams Result;
	auto Callback = [&Result](UE::DerivedData::FCachePutCompleteParams&& Params)
	{
		Result = Params;
	};

	Cache.Put(	{RecordBuilder.Build()},
				TEXT("Mirage"), // TODO: Improve this when we start passing more context to push
				TransferPolicy,
				Owner, 
				MoveTemp(Callback));

	Owner.Wait();

	if (Result.Status == UE::DerivedData::EStatus::Ok)
	{
		return EPushResult::Success;
	}
	else
	{
		return EPushResult::Failed;
	}
}

FCompressedBuffer FDDCBackend::PullData(const FPayloadId& Id)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::PullData);

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();

	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket; 
	Key.Hash = Id.GetIdentifier();

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);

	FCompressedBuffer ResultData;
	UE::DerivedData::EStatus ResultStatus;

	auto Callback = [&ResultData, &ResultStatus](UE::DerivedData::FCacheGetCompleteParams&& Params)
	{
		ResultStatus = Params.Status;
		if (ResultStatus == UE::DerivedData::EStatus::Ok)
		{
			ResultData = Params.Record.GetValuePayload().GetData();
		}
	};

	Cache.Get(	{Key}, 
				TEXT("Mirage"), // TODO: Improve this when we start passing more context to push
				TransferPolicy,
				Owner,
				MoveTemp(Callback));

	Owner.Wait();

	return ResultData;
}

bool FDDCBackend::DoesExist(const FPayloadId& Id) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::DoesExist);

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();

	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket;
	Key.Hash = Id.GetIdentifier();

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);
	
	UE::DerivedData::EStatus ResultStatus;
	auto Callback = [&ResultStatus](UE::DerivedData::FCacheGetCompleteParams&& Params)
	{
		ResultStatus = Params.Status;
	};

	Cache.Get(	{ Key },
				TEXT("Mirage"), // TODO: Improve this when we start passing more context to push
				QueryPolicy | UE::DerivedData::ECachePolicy::SkipData,
				Owner,
				MoveTemp(Callback));

	Owner.Wait();

	return ResultStatus == UE::DerivedData::EStatus::Ok;
}

UE_REGISTER_VIRTUALIZATION_BACKEND_FACTORY(FDDCBackend, DDCBackend);
} // namespace UE::Virtualization