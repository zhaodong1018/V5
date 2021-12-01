// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Algo/Find.h"
#include "Containers/StringView.h"
#include "IAssetRegistry.h"
#include "Misc/PackageName.h"

/* This class has marked all of its functions const and its variables mutable so that CookOnTheFlyServer can use its functions from const functions */
struct FPackageNameCache
{
	struct FCachedPackageFilename
	{
		FCachedPackageFilename(FString&& InStandardFilename, FName InStandardFileFName)
			: StandardFileNameString(MoveTemp(InStandardFilename))
			, StandardFileName(InStandardFileFName)
		{
		}

		FCachedPackageFilename(const FCachedPackageFilename& In) = default;
		FCachedPackageFilename& operator=(const FCachedPackageFilename& Other) = default;
		FCachedPackageFilename() = default;

		FCachedPackageFilename(FCachedPackageFilename&& In)
			: StandardFileNameString(MoveTemp(In.StandardFileNameString))
			, StandardFileName(In.StandardFileName)
		{
		}

		FString		StandardFileNameString;
		FName		StandardFileName;
	};

	bool			HasCacheForPackageName(const FName& PackageName) const;

	FString			GetCachedStandardFileNameString(const UPackage* Package) const;

	FName			GetCachedStandardFileName(const FName& PackageName, bool bRequireExists=true, bool bCreateAsMap=false) const;
	FName			GetCachedStandardFileName(const UPackage* Package) const;

	
	const FName*	FindExistingCachedPackageNameFromStandardFileName(const FName& NormalizedFileName, FName* FoundFileName = nullptr) const;
	const FName*	GetCachedPackageNameFromStandardFileName(const FName& NormalizedFileName, bool bExactMatchRequired=true, FName* FoundFileName = nullptr) const;

	void			ClearPackageFileNameCache(IAssetRegistry* InAssetRegistry) const;
	bool			ClearPackageFileNameCacheForPackage(const UPackage* Package) const;
	bool			ClearPackageFileNameCacheForPackage(const FName& PackageName) const;

	void			AppendCacheResults(TArray<TTuple<FName, FCachedPackageFilename>>&& PackageToCachedFileNames) const;

	bool			TryCalculateCacheData(FName PackageName, FString& OutStandardFilename, FName& OutStandardFileFName,
										bool bRequireExists = true, bool bCreateAsMap = false) const;

	bool			ContainsPackageName(FName PackageName) const;
	
	void			SetAssetRegistry(IAssetRegistry* InAssetRegistry) const;
	IAssetRegistry* GetAssetRegistry() const;

	/** Normalize the given FileName for use in looking up the cached data associated with the FileName. This normalization is equivalent to FPaths::MakeStandardFilename */
	static FName	GetStandardFileName(const FName& FileName);
	static FName	GetStandardFileName(const FStringView& FileName);

private:

	bool DoesPackageExist(const FName& PackageName, FString* OutFilename) const;
	const FCachedPackageFilename& Cache(const FName& PackageName, bool bRequireExists, bool bCreateAsMap) const;

	mutable IAssetRegistry* AssetRegistry = nullptr;

	mutable TMap<FName, FCachedPackageFilename> PackageFilenameCache; // filename cache (only process the string operations once)
	mutable TMap<FName, FName>					PackageFilenameToPackageFNameCache;
};

inline FName FPackageNameCache::GetCachedStandardFileName(const FName& PackageName, bool bRequireExists, bool bCreateAsMap) const
{
	return Cache(PackageName, bRequireExists, bCreateAsMap).StandardFileName;
}

inline bool FPackageNameCache::HasCacheForPackageName(const FName& PackageName) const
{
	return PackageFilenameCache.Find(PackageName) != nullptr;
}

inline FString FPackageNameCache::GetCachedStandardFileNameString(const UPackage* Package) const
{
	// check( Package->GetName() == Package->GetFName().ToString() );
	return Cache(Package->GetFName(), true /* bRequireExists */, false /* bCreateAsMap */).StandardFileNameString;
}

inline FName FPackageNameCache::GetCachedStandardFileName(const UPackage* Package) const
{
	// check( Package->GetName() == Package->GetFName().ToString() );
	return Cache(Package->GetFName(), true /* bRequireExists */, false /* bCreateAsMap */).StandardFileName;
}

inline bool FPackageNameCache::ClearPackageFileNameCacheForPackage(const UPackage* Package) const
{
	return ClearPackageFileNameCacheForPackage(Package->GetFName());
}

inline void FPackageNameCache::AppendCacheResults(TArray<TTuple<FName, FCachedPackageFilename>>&& PackageToStandardFileNames) const
{
	check(IsInGameThread());
	for (auto& Entry : PackageToStandardFileNames)
	{
		FName PackageName = Entry.Get<0>();
		FCachedPackageFilename& CachedPackageFilename = Entry.Get<1>();

		PackageFilenameToPackageFNameCache.Add(CachedPackageFilename.StandardFileName, PackageName);
		PackageFilenameCache.Emplace(PackageName, MoveTemp(CachedPackageFilename));
	}
}

inline bool FPackageNameCache::ClearPackageFileNameCacheForPackage(const FName& PackageName) const
{
	check(IsInGameThread());

	return PackageFilenameCache.Remove(PackageName) >= 1;
}

inline bool FPackageNameCache::DoesPackageExist(const FName& PackageName, FString* OutFilename) const
{
	FString PackageNameStr = PackageName.ToString();

	// Verse packages are editor-generated in-memory packages which don't have a corresponding 
	// asset file (yet). However, we still want to cook these packages out, producing cooked 
	// asset files for packaged projects.
	if (FPackageName::IsVersePackage(PackageNameStr))
	{
		if (FindPackage(/*Outer =*/nullptr, *PackageNameStr))
		{
			if (OutFilename)
			{
				*OutFilename = FPackageName::LongPackageNameToFilename(PackageNameStr, FPackageName::GetAssetPackageExtension());
			}
			return true;
		}
		// else, the cooker could be responding to a NotifyUObjectCreated() event, and the object hasn't
		// been fully constructed yet (missing from the FindObject() list) -- in this case, we've found 
		// that the linker loader is creating a dummy object to fill a referencing import slot, not loading
		// the proper object (which means we want to ignore it).
	}

	if (!AssetRegistry)
	{
		return FPackageName::DoesPackageExist(PackageNameStr, OutFilename, false);
	}

	TArray<FAssetData> Assets;
	AssetRegistry->GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyDiskAssets =*/true);

	if (Assets.Num() <= 0)
	{
		return false;
	}

	FString Dummy;
	if (OutFilename == nullptr)
	{
		OutFilename = &Dummy;
	}

	FName ClassRedirector = UObjectRedirector::StaticClass()->GetFName();
	bool bContainsMap = false;
	bool bContainsRedirector = false;
	for (const FAssetData& Asset : Assets)
	{
		bContainsMap = bContainsMap | ((Asset.PackageFlags & PKG_ContainsMap) != 0);
		bContainsRedirector = bContainsRedirector | (Asset.AssetClass == ClassRedirector);
	}
	if (!bContainsMap && bContainsRedirector)
	{
		// presence of map -> .umap
		// But we can only assume lack of map -> .uasset if we know the type of every object in the package.
		// If we don't, because there was a redirector, we have to check the package on disk
		// TODO: Have the AssetRegistry store the extension of the package so that we don't have to look it up
		// Guessing the extension based on map vs non-map also does not support text assets and maps which have a different extension
		return FPackageName::DoesPackageExist(PackageNameStr, OutFilename, false);
	}
	const FString& PackageExtension = bContainsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
	return FPackageName::TryConvertLongPackageNameToFilename(PackageNameStr, *OutFilename, PackageExtension);
}

inline bool FPackageNameCache::TryCalculateCacheData(FName PackageName, FString& OutStandardFilename, FName& OutStandardFileFName,
	bool bRequireExists, bool bCreateAsMap) const
{
	FString FilenameOnDisk;
	bool bFound = false;
	if (DoesPackageExist(PackageName, &FilenameOnDisk))
	{
		bFound = true;
	}
	else if (!bRequireExists)
	{
		FString Extension = bCreateAsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName.ToString(), FilenameOnDisk, Extension))
		{
			bFound = true;
		}
	}
	if (bFound)
	{
		OutStandardFilename = FPaths::ConvertRelativePathToFull(FilenameOnDisk);
		FPaths::MakeStandardFilename(OutStandardFilename);
		OutStandardFileFName = FName(*OutStandardFilename);
		return true;
	}
	else
	{
		return false;
	}
}

inline bool FPackageNameCache::ContainsPackageName(FName PackageName) const
{
	return PackageFilenameCache.Contains(PackageName);
}
inline IAssetRegistry* FPackageNameCache::GetAssetRegistry() const
{
	return AssetRegistry;
}

inline const FPackageNameCache::FCachedPackageFilename& FPackageNameCache::Cache(const FName& PackageName,
	bool bRequireExists, bool bCreateAsMap) const
{
	check(IsInGameThread());

	FCachedPackageFilename *Cached = PackageFilenameCache.Find(PackageName);

	if (Cached != nullptr)
	{
		if (!Cached->StandardFileName.IsNone() || bRequireExists)
		{
			return *Cached;
		}
	}

	// cache all the things, like it's your birthday!

	FString FileNameString;
	FName FileName = NAME_None;

	if (TryCalculateCacheData(PackageName, FileNameString, FileName, bRequireExists, bCreateAsMap))
	{
		PackageFilenameToPackageFNameCache.Add(FileName, PackageName);
	}

	return PackageFilenameCache.Emplace(PackageName, FCachedPackageFilename(MoveTemp(FileNameString), FileName));
}

inline const FName* FPackageNameCache::FindExistingCachedPackageNameFromStandardFileName(const FName& NormalizedFileName, FName* FoundFileName) const
{
	const FName* Result = PackageFilenameToPackageFNameCache.Find(NormalizedFileName);
	if (Result)
	{
		if (FoundFileName)
		{
			*FoundFileName = NormalizedFileName;
		}
		return Result;
	}

	return nullptr;
}

inline const FName* FPackageNameCache::GetCachedPackageNameFromStandardFileName(const FName& NormalizedFileName, bool bExactMatchRequired, FName* FoundFileName) const
{
	check(IsInGameThread());
	const FName* Result = FindExistingCachedPackageNameFromStandardFileName(NormalizedFileName, FoundFileName);
	if (Result)
	{
		return Result;
	}

	FName PackageName = NormalizedFileName;
	FString PotentialLongPackageName = NormalizedFileName.ToString();
	if (!FPackageName::IsValidLongPackageName(PotentialLongPackageName))
	{
		if (!FPackageName::TryConvertFilenameToLongPackageName(PotentialLongPackageName, PotentialLongPackageName))
		{
			return nullptr;
		}
		PackageName = FName(*PotentialLongPackageName);
	}

	const FCachedPackageFilename& CachedFilename = Cache(PackageName, true /* bRequireExists */, false /* bCreateAsMap */);

	if (bExactMatchRequired)
	{
		if (FoundFileName)
		{
			*FoundFileName = NormalizedFileName;
		}
		return PackageFilenameToPackageFNameCache.Find(NormalizedFileName);
	}
	else
	{
		check(FoundFileName != nullptr);
		*FoundFileName = CachedFilename.StandardFileName;
		return PackageFilenameToPackageFNameCache.Find(CachedFilename.StandardFileName);
	}
}

inline void FPackageNameCache::ClearPackageFileNameCache(IAssetRegistry* InAssetRegistry) const
{
	check(IsInGameThread());
	PackageFilenameCache.Empty();
	PackageFilenameToPackageFNameCache.Empty();
	AssetRegistry = InAssetRegistry;
}

inline void FPackageNameCache::SetAssetRegistry(IAssetRegistry* InAssetRegistry) const
{
	AssetRegistry = InAssetRegistry;
}

inline FName FPackageNameCache::GetStandardFileName(const FName& FileName)
{
	return GetStandardFileName(FileName.ToString());
}

inline FName FPackageNameCache::GetStandardFileName(const FStringView& InFileName)
{
	FString FileName(InFileName);
	FPaths::MakeStandardFilename(FileName);
	return FName(FileName);
}

