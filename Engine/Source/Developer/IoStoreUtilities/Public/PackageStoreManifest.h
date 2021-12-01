// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IO/IoDispatcher.h"
#include "ZenServerInterface.h"

class FPackageStoreManifest
{
public:
	struct FFileInfo
	{
		FString FileName;
		FIoChunkId ChunkId;
	};

	struct FPackageInfo
	{
		FName PackageName;
		FIoChunkId PackageChunkId;
		TArray<FIoChunkId> BulkDataChunkIds;
	};
	
	struct FZenServerInfo
	{
		UE::Zen::FServiceSettings Settings;
		FString ProjectId;
		FString OplogId;
	};

	IOSTOREUTILITIES_API FPackageStoreManifest(const FString& CookedOutputPath);
	IOSTOREUTILITIES_API ~FPackageStoreManifest() = default;

	IOSTOREUTILITIES_API void BeginPackage(FName PackageName);
	IOSTOREUTILITIES_API void AddPackageData(FName PackageName, const FString& FileName, const FIoChunkId& ChunkId);
	IOSTOREUTILITIES_API void AddBulkData(FName PackageName, const FString& FileName, const FIoChunkId& ChunkId);

	IOSTOREUTILITIES_API FIoStatus Save(const TCHAR* Filename) const;
	IOSTOREUTILITIES_API FIoStatus Load(const TCHAR* Filename);

	IOSTOREUTILITIES_API TArray<FFileInfo> GetFiles() const;
	IOSTOREUTILITIES_API TArray<FPackageInfo> GetPackages() const;

	IOSTOREUTILITIES_API FZenServerInfo& EditZenServerInfo();
	IOSTOREUTILITIES_API const FZenServerInfo* ReadZenServerInfo() const;

private:
	mutable FCriticalSection CriticalSection;
	FString CookedOutputPath;
	TMap<FName, FPackageInfo> PackageInfoByNameMap;
	TMap<FIoChunkId, FString> FileNameByChunkIdMap;
	TUniquePtr<FZenServerInfo> ZenServerInfo;
};
