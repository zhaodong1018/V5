// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorDomain/EditorDomainUtils.h"

#include "Algo/IsSorted.h"
#include "Algo/Unique.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Array.h"
#include "DerivedDataBuildDefinition.h"
#include "DerivedDataCache.h"
#include "DerivedDataCacheKey.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataRequestOwner.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Hash/Blake3.h"
#include "Memory/SharedBuffer.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackagePath.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "Serialization/CompactBinaryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/PackageWriterToSharedBuffer.h"
#include "String/ParseTokens.h"
#include "TargetDomain/TargetDomainUtils.h"
#include "Templates/Function.h"
#include "UObject/CoreRedirects.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UE5MainStreamObjectVersion.h"

/** Modify the masked bits in the output: set them to A & B. */
template<typename Enum>
static void EnumSetFlagsAnd(Enum& Output, Enum Mask, Enum A, Enum B)
{
	Output = (Output & ~Mask) | (Mask & A & B);
}

template <typename KeyType, typename ValueType, typename SetAllocator, typename KeyFuncs>
static ValueType MapFindRef(const TMap<KeyType, ValueType, SetAllocator, KeyFuncs>& Map,
	typename TMap<KeyType, ValueType, SetAllocator, KeyFuncs>::KeyConstPointerType Key, ValueType DefaultValue)
{
	const ValueType* FoundValue = Map.Find(Key);
	return FoundValue ? *FoundValue : DefaultValue;
}

namespace UE::EditorDomain
{

TArray<FGuid> GetCustomVersions(UClass& Class);
TMap<FGuid, UObject*> FindCustomVersionCulprits(TConstArrayView<FGuid> UnknownGuids, UPackage* Package);
void InitializeGlobalAddedCustomVersions();

FClassDigestMap GClassDigests;
FClassDigestMap& GetClassDigests()
{
	return GClassDigests;
}

TMap<FName, EDomainUse> GClassBlockedUses;
TMap<FName, EDomainUse> GPackageBlockedUses;
TMultiMap<FName, FName> GConstructClasses;
TArray<FName> GGlobalConstructClasses;
TSet<FName> GTargetDomainClassBlockList;
bool GTargetDomainClassUseAllowList = true;
bool GTargetDomainClassEmptyAllowList = false;
TArray<int32> GGlobalAddedCustomVersions;
bool bGGlobalAddedCustomVersionsInitialized = false;

// Change to a new guid when EditorDomain needs to be invalidated
const TCHAR* EditorDomainVersion = TEXT("30E58214A4A84D638FAA8826B81338A1");

// Identifier of the CacheBuckets for EditorDomain tables
const TCHAR* EditorDomainPackageBucketName = TEXT("EditorDomainPackage");
const TCHAR* EditorDomainBulkDataListBucketName = TEXT("EditorDomainBulkDataList");
const TCHAR* EditorDomainBulkDataPayloadIdBucketName = TEXT("EditorDomainBulkDataPayloadId");

static bool GetEditorDomainSaveUnversioned()
{
	auto Initialize = []()
	{
		bool bParsedValue;
		bool bResult = GConfig->GetBool(TEXT("EditorDomain"), TEXT("SaveUnversioned"), bParsedValue, GEditorIni) ? bParsedValue : true;
		if (GConfig->GetBool(TEXT("CookSettings"), TEXT("EditorDomainSaveUnversioned"), bResult, GEditorIni))
		{
			UE_LOG(LogEditorDomain, Error, TEXT("Editor.ini:[CookSettings]:EditorDomainSaveUnversioned is deprecated, use Editor.ini:[EditorDomain]:SaveUnversioned instead."));
		}
		return bResult;
	};
	static bool bEditorDomainSaveUnversioned = Initialize();
	return bEditorDomainSaveUnversioned;
}

/**
 * Thread-safe cache to compress CustomVersion Guids into integer handles, to reduce the cost of removing duplicates
 * when lists of CustomVersion Guids are merged
*/
class FKnownCustomVersions
{
public:
	/** Find or if necessary add the handle for each Guid; append them to the output handles. */
	static void FindOrAddHandles(TArray<int32>& OutHandles, TConstArrayView<FGuid> InGuids);
	/** Find or if necessary add the handle for each Guid; append them to the output handles. */
	static void FindOrAddHandles(TArray<int32>& OutHandles, int32 NumGuids, TFunctionRef<const FGuid& (int32)> GetGuid);
	/** Find the guid for each handle. Handles must be values returned from a FindHandle function. */
	static void FindGuidsChecked(TArray<FGuid>& OutGuids, TConstArrayView<int32> Handles);

private:
	static FRWLock Lock;
	static TMap<FGuid, int32> GuidToHandle;
	static TArray<FGuid> Guids;
};

FRWLock FKnownCustomVersions::Lock;
TMap<FGuid, int32> FKnownCustomVersions::GuidToHandle;
TArray<FGuid> FKnownCustomVersions::Guids;

void FKnownCustomVersions::FindOrAddHandles(TArray<int32>& OutHandles, TConstArrayView<FGuid> InGuids)
{
	FindOrAddHandles(OutHandles, InGuids.Num(), [InGuids](int32 Index) -> const FGuid& { return InGuids[Index]; });
}

void FKnownCustomVersions::FindOrAddHandles(TArray<int32>& OutHandles, int32 NumGuids, TFunctionRef<const FGuid& (int32)> GetGuid)
{
	// Avoid a WriteLock in most cases by finding-only the incoming guids and writing their handle to the output
	// For any Guids that are not found, add a placeholder handle and store the missing guid and its index in
	// the output in a list to iterate over later.
	TArray<TPair<FGuid, int32>> UnknownGuids;
	{
		FReadScopeLock ReadLock(Lock);
		OutHandles.Reserve(OutHandles.Num() + NumGuids);
		for (int32 Index = 0; Index < NumGuids; ++Index)
		{
			const FGuid& Guid = GetGuid(Index);
			int32* CustomVersionHandle = GuidToHandle.Find(GetGuid(Index));
			if (CustomVersionHandle)
			{
				OutHandles.Add(*CustomVersionHandle);
			}
			else
			{
				UnknownGuids.Reserve(NumGuids);
				UnknownGuids.Add(TPair<FGuid, int32>{ Guid, OutHandles.Num() });
				OutHandles.Add(INDEX_NONE);
			}
		}
	}

	if (UnknownGuids.Num())
	{
		// Add the missing guids under the writelock and write their handle over the placeholders in the output.
		FWriteScopeLock WriteLock(Lock);
		int32 NumKnownGuids = Guids.Num();
		for (TPair<FGuid, int32>& Pair : UnknownGuids)
		{
			int32& ExistingIndex = GuidToHandle.FindOrAdd(Pair.Key, NumKnownGuids);
			if (ExistingIndex == NumKnownGuids)
			{
				Guids.Add(Pair.Key);
				++NumKnownGuids;
			}
			OutHandles[Pair.Value] = ExistingIndex;
		}
	}
}

void FKnownCustomVersions::FindGuidsChecked(TArray<FGuid>& OutGuids, TConstArrayView<int32> Handles)
{
	OutGuids.Reserve(OutGuids.Num() + Handles.Num());
	FReadScopeLock ReadLock(Lock);
	for (int32 Handle : Handles)
	{
		check(0 <= Handle && Handle < Guids.Num());
		OutGuids.Add(Guids[Handle]);
	}
}

EPackageDigestResult AppendPackageDigest(FBlake3& Writer, EDomainUse& OutEditorDomainUse, FString& OutErrorMessage,
	const FAssetPackageData& PackageData, FName PackageName, TArray<FGuid>* OutCustomVersions)
{
	OutEditorDomainUse = EDomainUse::LoadEnabled | EDomainUse::SaveEnabled;
	
	FPackageFileVersion CurrentFileVersionUE = GPackageFileUEVersion;
	int32 CurrentFileVersionLicenseeUE = GPackageFileLicenseeUEVersion;
	Writer.Update(EditorDomainVersion, FCString::Strlen(EditorDomainVersion)*sizeof(TCHAR));
	uint8 EditorDomainSaveUnversioned = GetEditorDomainSaveUnversioned() ? 1 : 0;
	Writer.Update(&EditorDomainSaveUnversioned, sizeof(EditorDomainSaveUnversioned));
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Writer.Update(&PackageData.PackageGuid, sizeof(PackageData.PackageGuid));
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
	Writer.Update(&CurrentFileVersionUE, sizeof(CurrentFileVersionUE));
	Writer.Update(&CurrentFileVersionLicenseeUE, sizeof(CurrentFileVersionLicenseeUE));
	TArray<int32> CustomVersionHandles;
	// Reserve 10 custom versions per class times 100 classes per package times twice (once in package, once in class)
	CustomVersionHandles.Reserve(10*100*2);
	TConstArrayView<UE::AssetRegistry::FPackageCustomVersion> PackageVersions = PackageData.GetCustomVersions();
	FKnownCustomVersions::FindOrAddHandles(CustomVersionHandles,
		PackageVersions.Num(), [PackageVersions](int32 Index) -> const FGuid& { return PackageVersions[Index].Key;});

	int32 NextClass = 0;
	FClassDigestMap& ClassDigests = GetClassDigests();
	for (int32 Attempt = 0; NextClass < PackageData.ImportedClasses.Num(); ++Attempt)
	{
		if (Attempt > 0)
		{
			// EDITORDOMAIN_TODO: Remove this !IsInGameThread check once FindObject no longer asserts if GIsSavingPackage
			if (Attempt > 1 || !IsInGameThread())
			{
				OutErrorMessage = FString::Printf(TEXT("Package %s uses Class %s but that class is not loaded"),
					*PackageName.ToString(), *PackageData.ImportedClasses[NextClass].ToString());
				return EPackageDigestResult::MissingClass;
			}
			TConstArrayView<FName> RemainingClasses(PackageData.ImportedClasses);
			RemainingClasses = RemainingClasses.Slice(NextClass, PackageData.ImportedClasses.Num() - NextClass);
			PrecacheClassDigests(RemainingClasses);
		}
		FReadScopeLock ClassDigestsScopeLock(ClassDigests.Lock);
		for (; NextClass < PackageData.ImportedClasses.Num(); ++NextClass)
		{
			FName ClassName = PackageData.ImportedClasses[NextClass];
			FClassDigestData* ExistingData = ClassDigests.Map.Find(ClassName);
			if (!ExistingData)
			{
				break;
			}
			if (ExistingData->bNative)
			{
				Writer.Update(&ExistingData->SchemaHash, sizeof(ExistingData->SchemaHash));
			}
			CustomVersionHandles.Append(ExistingData->CustomVersionHandles);
			EnumSetFlagsAnd(OutEditorDomainUse, EDomainUse::LoadEnabled | EDomainUse::SaveEnabled,
				OutEditorDomainUse, ExistingData->EditorDomainUse);
		}
	}

	InitializeGlobalAddedCustomVersions();
	CustomVersionHandles.Append(GGlobalAddedCustomVersions);
	CustomVersionHandles.Sort();
	CustomVersionHandles.SetNum(Algo::Unique(CustomVersionHandles));

	TArray<FGuid> CustomVersionGuidBuffer;
	TArray<FGuid>& CustomVersionGuids(OutCustomVersions ? *OutCustomVersions : CustomVersionGuidBuffer);
	FKnownCustomVersions::FindGuidsChecked(CustomVersionGuids, CustomVersionHandles);
	CustomVersionGuids.Sort();

	for (const FGuid& CustomVersionGuid : CustomVersionGuids)
	{
		Writer.Update(&CustomVersionGuid, sizeof(CustomVersionGuid));
		TOptional<FCustomVersion> CurrentVersion = FCurrentCustomVersions::Get(CustomVersionGuid);
		if (CurrentVersion.IsSet())
		{
			Writer.Update(&CurrentVersion->Version, sizeof(CurrentVersion->Version));
		}
		else
		{
			OutErrorMessage = FString::Printf(TEXT("Package %s uses CustomVersion guid %s but that guid is not available in FCurrentCustomVersions"),
				*PackageName.ToString(), *CustomVersionGuid.ToString());
			return EPackageDigestResult::MissingCustomVersion;
		}
	}

	return EPackageDigestResult::Success;
}

/**
 * Holds context data for a call to PrecacheClassDigests, which needs to recursively
 * traverse a graph of of class parents and construction classes
 */
class FPrecacheClassDigest
{
public:
	FPrecacheClassDigest()
		: ClassDigestsMap(GetClassDigests())
		, ClassDigests(ClassDigestsMap.Map)
		, AssetRegistry(*IAssetRegistry::Get())
	{
		ClassDigestsMap.Lock.WriteLock();
	}

	~FPrecacheClassDigest()
	{
		ClassDigestsMap.Lock.WriteUnlock();
	}

	FClassDigestData* GetRecursive(FName ClassName, bool bAllowRedirects);

	struct FUnlockScope
	{
		FUnlockScope(FRWLock& InLock)
			: Lock(InLock)
		{
			Lock.WriteUnlock();
		}
		~FUnlockScope()
		{
			Lock.WriteLock();
		}
		FRWLock& Lock;
	};

private:
	FClassDigestMap& ClassDigestsMap;
	TMap<FName, FClassDigestData>& ClassDigests;
	IAssetRegistry& AssetRegistry;

	// Scratch variables usable during GetRecursive; they are invalidated when a recursive call is made
	FString NameStringBuffer;
	TArray<FName> AncestorShortNames;
};

FClassDigestData* FPrecacheClassDigest::GetRecursive(FName ClassName, bool bAllowRedirects)
{
	// Called within ClassDigestsMap.Lock.WriteLock()
	FClassDigestData* DigestData = &ClassDigests.FindOrAdd(ClassName);
	if (DigestData->bConstructed)
	{
		return DigestData;
	}
	DigestData->bConstructed = true;

	FName LookupName = ClassName;
	ClassName.ToString(NameStringBuffer);
	if (bAllowRedirects)
	{
		FCoreRedirectObjectName ClassNameRedirect(NameStringBuffer);
		FCoreRedirectObjectName RedirectedClassNameRedirect = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, ClassNameRedirect);
		if (ClassNameRedirect != RedirectedClassNameRedirect)
		{
			NameStringBuffer = RedirectedClassNameRedirect.ToString();
			LookupName = FName(NameStringBuffer);
		}
	}

	UStruct* Struct = nullptr;
	if (FPackageName::IsScriptPackage(NameStringBuffer))
	{
		Struct = FindObject<UStruct>(nullptr, *NameStringBuffer);
		if (!Struct)
		{
			// If ClassName is native but is not yet loaded then abort and the caller gives an error or retries later
			ClassDigests.Remove(ClassName);
			return nullptr;
		}
	}

	// Fill in digest data config-driven flags
	DigestData->EditorDomainUse = EDomainUse::LoadEnabled | EDomainUse::SaveEnabled;
	DigestData->EditorDomainUse &= ~MapFindRef(GClassBlockedUses, ClassName, EDomainUse::None);
	if (LookupName != ClassName)
	{
		DigestData->EditorDomainUse &= ~MapFindRef(GClassBlockedUses, LookupName, EDomainUse::None);
	}
	if (!GTargetDomainClassUseAllowList)
	{
		DigestData->bTargetIterativeEnabled = !GTargetDomainClassBlockList.Contains(ClassName);
		if (LookupName != ClassName)
		{
			DigestData->bTargetIterativeEnabled &= !GTargetDomainClassBlockList.Contains(LookupName);
		}
	}

	// Fill in native-specific digest data, get the ParentName, and if non-native, get the native ancestor struct
	FName ParentName;
	if (Struct)
	{
		DigestData->bNative = true;
		DigestData->SchemaHash = Struct->GetSchemaHash(false /* bSkipEditorOnly */);
		UStruct* ParentStruct = Struct->GetSuperStruct();
		if (ParentStruct)
		{
			NameStringBuffer.Reset();
			ParentStruct->GetPathName(nullptr, NameStringBuffer);
			ParentName = FName(*NameStringBuffer);
		}
	}
	else
	{
		DigestData->bNative = false;
		DigestData->SchemaHash.Reset();
		DigestData->CustomVersionHandles.Empty();
		FStringView UnusedClassOfClassName;
		FStringView ClassPackageName;
		FStringView ClassObjectName;
		FStringView ClassSubObjectName;
		FPackageName::SplitFullObjectPath(NameStringBuffer, UnusedClassOfClassName, ClassPackageName, ClassObjectName, ClassSubObjectName);
		FName ClassObjectFName(ClassObjectName);
		// TODO_EDITORDOMAIN: If the class is not yet present in the assetregistry, or
		// if its parent classes are not, then we will not be able to propagate information from the parent classes; wait on the class to be parsed
		AncestorShortNames.Reset();
		IAssetRegistry::Get()->GetAncestorClassNames(ClassObjectFName, AncestorShortNames);
		for (FName ShortName : AncestorShortNames)
		{
			// TODO_EDITORDOMAIN: For robustness and performance, we need the AssetRegistry to return FullPathNames rather than ShortNames
			// For now, we lookup each shortname using FindObject, and do not handle propagating data from blueprint classes to child classes
			if (UStruct* CurrentStruct = FindObjectFast<UStruct>(nullptr, ShortName, false /* ExactClass */, true /* AnyPackage */))
			{
				NameStringBuffer.Reset();
				CurrentStruct->GetPathName(nullptr, NameStringBuffer);
				if (FPackageName::IsScriptPackage(NameStringBuffer))
				{
					ParentName = FName(*NameStringBuffer);
					Struct = CurrentStruct;
					break;
				}
			}
		}
	}

	// Get the CustomVersions used by the native class; GetCustomVersions already returns all custom versions used
	// by the parent class so we do not need to copy data from the parent
	UClass* StructAsClass = Cast<UClass>(Struct);
	if (StructAsClass)
	{
		// GetCustomVersions can create the ClassDefaultObject, which can trigger LoadPackage, which
		// can reenter this function recursively. We have to drop the lock to prevent a deadlock.
		FUnlockScope UnlockScope(ClassDigestsMap.Lock);
		FKnownCustomVersions::FindOrAddHandles(DigestData->CustomVersionHandles, GetCustomVersions(*StructAsClass));
	}
	else
	{
		DigestData->CustomVersionHandles.Reset();
	}

	// Propagate values from the parent
	if (!ParentName.IsNone())
	{
		// CoreRedirects are expected to act only on import classes from packages; they are not expected to act on the parent class pointer
		// of a native class, which is authoritative, so set bAllowRedirects = false
		FClassDigestData* ParentDigest = GetRecursive(ParentName, false /* bAllowRedirects */);
		// The map has possibly been modified so we need to recalculate the address of ClassName's DigestData
		DigestData = &ClassDigests.FindChecked(ClassName);
		if (!ParentDigest)
		{
			UE_LOG(LogEditorDomain, Display,
				TEXT("Parent class %s of class %s not found. Allow flags for editordomain and iterative cooking will be invalid."),
				*ParentName.ToString(), *ClassName.ToString());
		}
		else
		{
			if (!ParentDigest->bConstructionComplete)
			{
				// Suppress the warning for MulticastDelegateProperty, which has a redirector to its own child class
				// of MulticastInlineDelegateProperty
				// We could fix this case by adding bAllowRedirects to the ClassDigestsMap lookup key, but it's not a
				// problem for MuticastDelegateProperty and we don't have any other cases where it is a problem, so we
				// avoid the performance cost of doing so.
				if (ClassName != FName(TEXT("/Script/CoreUObject.MulticastDelegateProperty")))
				{
					UE_LOG(LogEditorDomain, Display,
						TEXT("Cycle detected in parents of class %s. Allow flags for editordomain and iterative cooking will be invalid."),
						*ClassName.ToString());
				}
			}
			EnumSetFlagsAnd(DigestData->EditorDomainUse, EDomainUse::LoadEnabled | EDomainUse::SaveEnabled,
				DigestData->EditorDomainUse, ParentDigest->EditorDomainUse);
			if (!GTargetDomainClassUseAllowList)
			{
				DigestData->bTargetIterativeEnabled &= ParentDigest->bTargetIterativeEnabled;
			}
		}
	}

	// Propagate values from the ConstructClasses
	TArray<FName> ConstructClasses; // Not a class scratch variable because we use it across recursive calls
	GConstructClasses.MultiFind(ClassName, ConstructClasses);
	if (LookupName != ClassName)
	{
		GConstructClasses.MultiFind(LookupName, ConstructClasses);
	}
	if (!ConstructClasses.IsEmpty())
	{
		TArray<int32> ConstructCustomVersions; // Not a class scratch variable because we use it across recursive calls
		for (FName ConstructClass : ConstructClasses)
		{
			FClassDigestData* ConstructClassDigest = GetRecursive(ConstructClass, true /* bAllowRedirects */);
			if (!ConstructClassDigest)
			{
				UE_LOG(LogEditorDomain, Warning,
					TEXT("Construct class %s specified by Editor.ini:[EditorDomain]:PostLoadConstructClasses for class %s is not found. ")
					TEXT("This is a class that can be constructed by postload upgrades of class %s. ")
					TEXT("Old packages with class %s will load more slowly."),
					*ConstructClass.ToString(), *ClassName.ToString(), *ClassName.ToString(), *ClassName.ToString());
			}
			else
			{
				if (!ConstructClassDigest->bConstructionComplete)
				{
					UE_LOG(LogEditorDomain, Verbose,
						TEXT("Cycle detected in Editor.ini:[EditorDomain]:PostLoadConstructClasses of class %s. This is unexpected, but not a problem."),
						*ClassName.ToString());
				}
				ConstructCustomVersions.Append(ConstructClassDigest->CustomVersionHandles);
			}
		}
		// The map has possibly been modified so we need to recalculate the address of ClassName's DigestData
		DigestData = &ClassDigests.FindChecked(ClassName);
		DigestData->CustomVersionHandles.Append(MoveTemp(ConstructCustomVersions));
		Algo::Sort(DigestData->CustomVersionHandles);
		DigestData->CustomVersionHandles.SetNum(Algo::Unique(DigestData->CustomVersionHandles));
	}

	DigestData->bConstructionComplete = true;
	return DigestData;
}

/** Try to add the FClassDigestData for each given class into the GetClassDigests map */
void PrecacheClassDigests(TConstArrayView<FName> ClassNames)
{
	FPrecacheClassDigest Digester;
	for (FName ClassName : ClassNames)
	{
		Digester.GetRecursive(ClassName, true /* bAllowRedirects */);
	}
}

/** Construct GGlobalAddedCustomVersions from the classes specified by config */
void InitializeGlobalAddedCustomVersions()
{
	if (bGGlobalAddedCustomVersionsInitialized)
	{
		return;
	}
	bGGlobalAddedCustomVersionsInitialized = true;
	TArray<FName> GlobalAddedClassNames;
	{
		TArray<FString> Lines;
		GConfig->GetArray(TEXT("EditorDomain"), TEXT("GlobalCanConstructClasses"), Lines, GEditorIni);
		GlobalAddedClassNames.Reserve(Lines.Num());
		for (const FString& Line : Lines)
		{
			GlobalAddedClassNames.Add(FName(FStringView(Line).TrimStartAndEnd()));
		}
	}

	PrecacheClassDigests(GlobalAddedClassNames);
	FClassDigestMap& ClassDigests = GetClassDigests();
	FReadScopeLock ClassDigestsScopeLock(ClassDigests.Lock);
	for (FName ClassName : GlobalAddedClassNames)
	{
		FClassDigestData* ExistingData = ClassDigests.Map.Find(ClassName);
		if (!ExistingData)
		{
			UE_LOG(LogEditorDomain, Display, TEXT("Construct class %s specified by Editor.ini:[EditorDomain]:GlobalCanConstructClasses is not found. ")
				TEXT("This is a class that can be constructed automatically by SavePackage when saving old packages. ")
				TEXT("Old packages that do not yet have this class will load more slowly."),
				*ClassName.ToString());
		}
		else
		{
			GGlobalAddedCustomVersions.Append(ExistingData->CustomVersionHandles);
		}
	}
	Algo::Sort(GGlobalAddedCustomVersions);
	GGlobalAddedCustomVersions.SetNum(Algo::Unique(GGlobalAddedCustomVersions));
}

/** An archive that just collects custom versions. */
class FCustomVersionCollectorArchive : public FArchiveUObject
{
public:
	FCustomVersionCollectorArchive()
	{
		// Use the same Archive properties that are used by FPackageHarvester, since that
		// is the authoritative way of collecting CustomVersions used in the save
		SetIsSaving(true);
		SetIsPersistent(true);
		ArIsObjectReferenceCollector = true;
		ArShouldSkipBulkData = true;
	}
	// The base class functionality does most of what we need:
	// ignore Serialize(void*,int), ignore Serialize(UObject*), collect customversions
	// Some classes expect Seek and Tell to work, though, so we simulate those
	virtual void Seek(int64 InPos) override
	{
		check(0 <= Pos && Pos <= Max);
		Pos = InPos;
	}
	virtual int64 Tell() override
	{
		return Pos;
	}
	virtual int64 TotalSize() override
	{
		return Max;
	}
	virtual void Serialize(void* V, int64 Length) override
	{
		Pos += Length;
		Max = FMath::Max(Pos, Max);
	}
	virtual FString GetArchiveName() const override
	{
		return TEXT("FCustomVersionCollectorArchive");
	}

private:
	int64 Pos = 0;
	int64 Max = 0;
};

/** Collect the CustomVersions that can be used by the given Class when it is saved */
TArray<FGuid> GetCustomVersions(UClass& Class)
{
	FCustomVersionCollectorArchive Ar;
	Class.GetDefaultObject()->DeclareCustomVersions(Ar);
	// Default objects of blueprint classes are serialized during SavePackage with a special call to
	// UBlueprintGeneratedClass::SerializeDefaultObject
	// All packages that include a BlueprintGeneratedClass import the UClass BlueprintGeneratedClass
	// (Note the UClass BlueprintGeneratedClass is not the same as the c++ UBlueprintGeneratedClass)
	// We therefore add on the CustomVersions used by UBlueprintGeneratedClass::SerializeDefaultObject into
	// the CustomVersions for the UClass named BlueprintGeneratedClass
	static const FName NAME_EnginePackage(TEXT("/Script/Engine"));
	static const FName NAME_BlueprintGeneratedClass(TEXT("BlueprintGeneratedClass"));
	if (Class.GetFName() == NAME_BlueprintGeneratedClass && Class.GetPackage()->GetFName() == NAME_EnginePackage)
	{
		Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);
	}
	TArray<FGuid> Guids;
	const FCustomVersionContainer& CustomVersions = Ar.GetCustomVersions();
	Guids.Reserve(CustomVersions.GetAllVersions().Num());
	for (const FCustomVersion& CustomVersion : CustomVersions.GetAllVersions())
	{
		Guids.Add(CustomVersion.Key);
	}
	Algo::Sort(Guids);
	Guids.SetNum(Algo::Unique(Guids));
	return Guids;
}

/** Serialize each object in the package to find the one using each of the given CustomVersions */
TMap<FGuid, UObject*> FindCustomVersionCulprits(TConstArrayView<FGuid> UnknownGuids, UPackage* Package)
{
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects);
	TMap<FGuid, UObject*> Culprits;
	for (UObject* Object : Objects)
	{
		FCustomVersionCollectorArchive Ar;
		Object->Serialize(Ar);
		for (const FCustomVersion& CustomVersion : Ar.GetCustomVersions().GetAllVersions())
		{
			UObject*& Existing = Culprits.FindOrAdd(CustomVersion.Key);
			if (!Existing)
			{
				Existing = Object;
			}
		}
	}
	return Culprits;
}

TMap<FName, EDomainUse> ConstructClassBlockedUses()
{
	TMap<FName, EDomainUse> Result;
	TArray<FString> BlockListArray;
	TArray<FString> LoadBlockListArray;
	TArray<FString> SaveBlockListArray;
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("ClassBlockList"), BlockListArray, GEditorIni);
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("ClassLoadBlockList"), LoadBlockListArray, GEditorIni);
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("ClassSaveBlockList"), SaveBlockListArray, GEditorIni);
	for (TArray<FString>* Array : { &BlockListArray, &LoadBlockListArray, &SaveBlockListArray })
	{
		EDomainUse BlockedUse = Array == &BlockListArray	?	(EDomainUse::LoadEnabled | EDomainUse::SaveEnabled) : (
			Array == &LoadBlockListArray					?	EDomainUse::LoadEnabled : (
																EDomainUse::SaveEnabled));
		for (const FString& ClassPathName : *Array)
		{
			Result.FindOrAdd(FName(*ClassPathName), EDomainUse::None) |= BlockedUse;
		}
	}
	return Result;
}

TMap<FName, EDomainUse> ConstructPackageNameBlockedUses()
{
	TMap<FName, EDomainUse> Result;
	TArray<FString> BlockListArray;
	TArray<FString> LoadBlockListArray;
	TArray<FString> SaveBlockListArray;
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("PackageBlockList"), BlockListArray, GEditorIni);
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("PackageLoadBlockList"), LoadBlockListArray, GEditorIni);
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("PackageSaveBlockList"), SaveBlockListArray, GEditorIni);
	FString PackageName;
	FString ErrorReason;
	for (TArray<FString>* Array : { &BlockListArray, &LoadBlockListArray, &SaveBlockListArray })
	{
		EDomainUse BlockedUse = Array == &BlockListArray	?	(EDomainUse::LoadEnabled | EDomainUse::SaveEnabled) : (
				Array == &LoadBlockListArray				?	EDomainUse::LoadEnabled : (
																EDomainUse::SaveEnabled));
		for (const FString& PackageNameOrFilename : *Array)
		{
			if (!FPackageName::TryConvertFilenameToLongPackageName(PackageNameOrFilename, PackageName, &ErrorReason))
			{
				UE_LOG(LogEditorDomain, Warning, TEXT("Editor.ini:[EditorDomain]:PackageBlocklist: Could not convert %s to a LongPackageName: %s"),
					*PackageNameOrFilename, *ErrorReason);
				continue;
			}
			Result.FindOrAdd(FName(*PackageName), EDomainUse::None) |= BlockedUse;
		}
	}
	return Result;
}

TSet<FName> ConstructTargetIterativeClassBlockList()
{
	TSet<FName> Result;
	TArray<FString> BlockListArray;
	GConfig->GetArray(TEXT("TargetDomain"), TEXT("IterativeClassBlockList"), BlockListArray, GEditorIni);
	for (const FString& ClassPathName : BlockListArray)
	{
		Result.Add(FName(*ClassPathName));
	}
	return Result;
}

void ConstructTargetIterativeClassAllowList()
{
	// We're using a allowlist with a blocklist override, so the blocklist is only needed when creating the allowlist
	TSet<FName> BlockListFNames = ConstructTargetIterativeClassBlockList();

	// AllowList elements implicitly allow all parent classes, so instead of consulting a list and propagating
	// from parent classes every time we read a new class, we have to iterate the list for all classes up front and
	// propagate _TO_ parent classes. Note that we only support allowlisting native classes, otherwise we would have
	// to wait for the AssetRegistry to finish loading to be sure we could find every specified allowed class.

	// Declare a recursive Visit function. Every class we visit is allowlisted, and we visit its superclasses.
	// To decide whether a visited class is enabled, we also have to get IsBlockListed recursively from the parent.
	TSet<FName> EnabledFNames;
	TStringBuilder<256> NameStringBuffer;
	TMap<FName, TOptional<bool>> Visited;
	auto EnableClassIfNotBlocked = [&Visited, &EnabledFNames, &BlockListFNames, &NameStringBuffer]
		(FName PathName, UStruct* Struct, bool& bOutIsBlocked, auto& EnableClassIfNotBlockedRef)
	{
		int32 KeyHash = GetTypeHash(PathName);
		TOptional<bool>& BlockedValue = Visited.FindOrAddByHash(KeyHash, PathName);
		if (BlockedValue.IsSet())
		{
			bOutIsBlocked = *BlockedValue;
			return;
		}
		BlockedValue = false; // If there is a cycle in the class graph, we will encounter PathName again, so initialize to false

		bool bParentBlocked = false;
		UStruct* ParentStruct = Struct->GetSuperStruct();
		if (ParentStruct)
		{
			NameStringBuffer.Reset();
			ParentStruct->GetPathName(nullptr, NameStringBuffer);
			EnableClassIfNotBlockedRef(FName(*NameStringBuffer), ParentStruct,
				bParentBlocked, EnableClassIfNotBlockedRef);
		}

		bOutIsBlocked = bParentBlocked || BlockListFNames.Contains(PathName);
		if (bOutIsBlocked)
		{
			// Call FindOrAdd again, since the recursive calls may have altered the map and invalidated the BlockedValue reference
			Visited.FindOrAddByHash(KeyHash, PathName) = bOutIsBlocked;
		}
		else
		{
			EnabledFNames.Add(PathName);
		}
	};

	TArray<FString> AllowListLeafNames;
	GConfig->GetArray(TEXT("TargetDomain"), TEXT("IterativeClassAllowList"), AllowListLeafNames, GEditorIni);
	for (const FString& ClassPathName : AllowListLeafNames)
	{
		if (!FPackageName::IsScriptPackage(ClassPathName))
		{
			continue;
		}
		UStruct* Struct = FindObject<UStruct>(nullptr, *ClassPathName);
		if (!Struct)
		{
			continue;
		}
		bool bUnusedIsBlocked;
		EnableClassIfNotBlocked(FName(*ClassPathName), Struct, bUnusedIsBlocked, EnableClassIfNotBlocked);
	}

	TArray<FName> EnabledFNamesArray = EnabledFNames.Array();
	PrecacheClassDigests(EnabledFNamesArray);
	FClassDigestMap& ClassDigests = GetClassDigests();
	{
		FWriteScopeLock ClassDigestsScopeLock(ClassDigests.Lock);
		for (FName ClassPathName : EnabledFNamesArray)
		{
			FClassDigestData* DigestData = ClassDigests.Map.Find(ClassPathName);
			if (DigestData)
			{
				DigestData->bTargetIterativeEnabled = true;
			}
		}
	}
}

/** Construct PostLoadCanConstructClasses multimap from config settings and return it */
TMultiMap<FName, FName> ConstructConstructClasses()
{
	TArray<FString> Lines;
	GConfig->GetArray(TEXT("EditorDomain"), TEXT("PostLoadCanConstructClasses"), Lines, GEditorIni);
	TMultiMap<FName, FName> ConstructClasses;
	for (const FString& Line : Lines)
	{
		int32 NumTokens = 0;
		FStringView PostLoadClass;
		FStringView ConstructedClass;
		UE::String::ParseTokens(Line, TEXT(','), [&NumTokens, &PostLoadClass, &ConstructedClass](FStringView Token)
			{
				*(NumTokens == 0 ? &PostLoadClass : &ConstructedClass) = Token;
				++NumTokens;
			});
		if (NumTokens != 2)
		{
			UE_LOG(LogEditorDomain, Warning, TEXT("Invalid value %s in config setting Editor.ini:[EditorDomain]:PostLoadCanConstructClasses"), *Line);
			continue;
		}
		PostLoadClass.TrimStartAndEndInline();
		ConstructedClass.TrimStartAndEndInline();
		ConstructClasses.Add(FName(PostLoadClass), FName(ConstructedClass));
	};
	return ConstructClasses;
}

FDelegateHandle GUtilsPostInitDelegate;
void UtilsPostEngineInit();

void UtilsInitialize()
{
	GClassBlockedUses = ConstructClassBlockedUses();
	GPackageBlockedUses = ConstructPackageNameBlockedUses();
	GConstructClasses = ConstructConstructClasses();

	bool bTargetDomainClassUseBlockList = true;
	if (FParse::Param(FCommandLine::Get(), TEXT("fullcook")))
	{
		// Allow list is marked as used, but is initialized empty
		bTargetDomainClassUseBlockList = false;
		GTargetDomainClassUseAllowList = true;
		GTargetDomainClassEmptyAllowList = true;
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("iterate")))
	{
		bTargetDomainClassUseBlockList = false;
		GTargetDomainClassUseAllowList = false;
	}
	else
	{
		GConfig->GetBool(TEXT("TargetDomain"), TEXT("IterativeClassAllowListEnabled"), GTargetDomainClassUseAllowList, GEditorIni);
		GTargetDomainClassEmptyAllowList = false;
	}

	if (!GTargetDomainClassUseAllowList && bTargetDomainClassUseBlockList)
	{
		GTargetDomainClassBlockList = ConstructTargetIterativeClassBlockList();
	}

	// Constructing allowlists requires use of UStructs, and the early SetPackageResourceManager
	// where UtilsInitialize is called is too early; trying to call UStruct->GetSchemaHash at that
	// time will break the UClass. Defer the construction of allowlist-based data until OnPostEngineInit
	GUtilsPostInitDelegate = FCoreDelegates::OnPostEngineInit.AddLambda([]() { UtilsPostEngineInit(); });
}

void UtilsPostEngineInit()
{
	FCoreDelegates::OnPostEngineInit.Remove(GUtilsPostInitDelegate);
	GUtilsPostInitDelegate.Reset();

	// Note that constructing AllowLists depends on all BlockLists having been parsed already
	if (GTargetDomainClassUseAllowList && !GTargetDomainClassEmptyAllowList)
	{
		ConstructTargetIterativeClassAllowList();
	}
}

EPackageDigestResult GetPackageDigest(IAssetRegistry& AssetRegistry, FName PackageName,
	FPackageDigest& OutPackageDigest, EDomainUse& OutEditorDomainUse, FString& OutErrorMessage,
	TArray<FGuid>* OutCustomVersions)
{
	FBlake3 Builder;
	EPackageDigestResult Result = AppendPackageDigest(AssetRegistry, PackageName, Builder, OutEditorDomainUse, 
		OutErrorMessage, OutCustomVersions);
	if (Result == EPackageDigestResult::Success)
	{
		OutPackageDigest = Builder.Finalize();
	}
	return Result;
}

EPackageDigestResult AppendPackageDigest(IAssetRegistry& AssetRegistry, FName PackageName,
	FBlake3& Builder, EDomainUse& OutEditorDomainUse, FString& OutErrorMessage, TArray<FGuid>* OutCustomVersions)
{
	AssetRegistry.WaitForPackage(PackageName.ToString());
	TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(PackageName);
	if (!PackageData)
	{
		OutErrorMessage = FString::Printf(TEXT("Package %s does not exist in the AssetRegistry"),
			*PackageName.ToString());
		OutEditorDomainUse = EDomainUse::LoadEnabled | EDomainUse::SaveEnabled;
		if (OutCustomVersions)
		{
			OutCustomVersions->Reset();
		}
		return EPackageDigestResult::FileDoesNotExist;
	}
	EPackageDigestResult Result = AppendPackageDigest(Builder, OutEditorDomainUse, OutErrorMessage, *PackageData,
		PackageName, OutCustomVersions);
	EnumSetFlagsAnd(OutEditorDomainUse, EDomainUse::LoadEnabled | EDomainUse::SaveEnabled,
		OutEditorDomainUse, ~MapFindRef(GPackageBlockedUses, PackageName, EDomainUse::None));
	return Result;
}

UE::DerivedData::FCacheKey GetEditorDomainPackageKey(const FPackageDigest& PackageDigest)
{
	static UE::DerivedData::FCacheBucket EditorDomainPackageCacheBucket(EditorDomainPackageBucketName);
	return UE::DerivedData::FCacheKey{EditorDomainPackageCacheBucket, PackageDigest};
}

UE::DerivedData::FCacheKey GetBulkDataListKey(const FPackageDigest& PackageDigest)
{
	static UE::DerivedData::FCacheBucket EditorDomainBulkDataListBucket(EditorDomainBulkDataListBucketName);
	return UE::DerivedData::FCacheKey{ EditorDomainBulkDataListBucket, PackageDigest };
}

UE::DerivedData::FCacheKey GetBulkDataPayloadIdKey(const FIoHash& PackageAndGuidDigest)
{
	static UE::DerivedData::FCacheBucket EditorDomainBulkDataPayloadIdBucket(EditorDomainBulkDataPayloadIdBucketName);
	return UE::DerivedData::FCacheKey{ EditorDomainBulkDataPayloadIdBucket, PackageAndGuidDigest };
}

void RequestEditorDomainPackage(const FPackagePath& PackagePath,
	const FPackageDigest& PackageDigest, UE::DerivedData::ECachePolicy SkipFlags, UE::DerivedData::IRequestOwner& Owner,
	UE::DerivedData::FOnCacheGetComplete&& Callback)
{
	using namespace UE::DerivedData;

	ICache& Cache = GetCache();
	checkf((SkipFlags & (~ECachePolicy::SkipData)) == ECachePolicy::None,
		TEXT("SkipFlags should only contain ECachePolicy::Skip* flags"));

	// Set the CachePolicy to only query from local; we do not want to wait for download from remote.
	// Downloading from remote is done in batch  see FRequestCluster::StartAsync.
	// But set the CachePolicy to store into remote. This will cause the CacheStore to push
	// any existing local value into upstream storage and refresh the last-used time in the upstream.
	ECachePolicy CachePolicy = SkipFlags | ECachePolicy::Local | ECachePolicy::StoreRemote;
	Cache.Get({ GetEditorDomainPackageKey(PackageDigest) }, PackagePath.GetDebugName(),
		CachePolicy, Owner, MoveTemp(Callback));
}

/** Stores data from SavePackage in accessible fields */
class FEditorDomainPackageWriter final : public TPackageWriterToSharedBuffer<IPackageWriter>
{
public:
	FEditorDomainPackageWriter(uint64& InFileSize)
		: FileSize(InFileSize)
	{
	}

	// IPackageWriter
	virtual FCapabilities GetCapabilities() const override
	{
		FCapabilities Result;
		Result.bDeclareRegionForEachAdditionalFile = true;
		return Result;
	}

	/** Deserialize the CustomVersions out of the PackageFileSummary that was serialized into the header */
	bool TryGetCustomVersions(FCustomVersionContainer& OutVersions)
	{
		FMemoryReaderView HeaderArchive(WritePackageRecord.Buffer.GetView());
		FPackageFileSummary Summary;
		HeaderArchive << Summary;
		if (HeaderArchive.IsError())
		{
			return false;
		}
		OutVersions = Summary.GetCustomVersionContainer();
		return true;
	}

	struct FAttachment
	{
		FSharedBuffer Buffer;
		UE::DerivedData::FPayloadId PayloadId;
	};
	/** The Buffer+Id for each section making up the EditorDomain's copy of the package */
	TConstArrayView<FAttachment> GetAttachments() const
	{
		return Attachments;
	}

protected:
	virtual TFuture<FMD5Hash> CommitPackageInternal(FPackageWriterRecords::FPackage&& Record,
		const FCommitPackageInfo& Info) override
	{
		// CommitPackage is called below with these options
		check(Info.Attachments.Num() == 0);
		check(Info.bSucceeded);
		check(Info.WriteOptions == IPackageWriter::EWriteOptions::Write);
		if (Record.AdditionalFiles.Num() > 0)
		{
			// WriteAdditionalFile is only used when saving cooked packages or for SidecarDataToAppend
			// We don't handle cooked, and SidecarDataToAppend is not yet used by anything.
			// To implement this we will need to
			// 1) Add a segment argument to IPackageWriter::FAdditionalFileInfo
			// 2) Create MetaData for the EditorDomain package
			// 3) Save the sidecar segment as a separate Attachment.
			// 4) List sidecar segment and appended-to-exportsarchive segments in the metadata.
			// 5) Change FEditorDomainPackageSegments to have a separate way to request the sidecar segment.
			// 6) Handle EPackageSegment::PayloadSidecar in FEditorDomain::OpenReadPackage by returning an archive configured to deserialize the sidecar segment.
			unimplemented();
		}
		WritePackageRecord = *Record.Package;

		TArray<FSharedBuffer> AttachmentBuffers;

		for (const FFileRegion& FileRegion : Record.Package->Regions)
		{
			checkf(FileRegion.Type == EFileRegionType::None, TEXT("Does not support FileRegion types other than None."));
		}
		check(Record.Package->Buffer.GetSize() > 0); // Header+Exports segment is non-zero in length
		AttachmentBuffers.Add(Record.Package->Buffer);

		for (const FBulkDataRecord& BulkRecord : Record.BulkDatas)
		{
			checkf(BulkRecord.Info.BulkDataType == IPackageWriter::FBulkDataInfo::AppendToExports,
				TEXT("Does not support BulkData types other than AppendToExports."));

			const uint8* BufferStart = reinterpret_cast<const uint8*>(BulkRecord.Buffer.GetData());
			uint64 SizeFromRegions = 0;
			for (const FFileRegion& FileRegion : BulkRecord.Regions)
			{
				checkf(FileRegion.Type == EFileRegionType::None,
					TEXT("Does not support FileRegion types other than None."));
				checkf(FileRegion.Offset + FileRegion.Length <= BulkRecord.Buffer.GetSize(),
					TEXT("FileRegions in WriteBulkData were outside of the range of the BulkData's size."));
				check(FileRegion.Length > 0); // SavePackage must not call WriteBulkData with empty bulkdatas

				AttachmentBuffers.Add(FSharedBuffer::MakeView(BufferStart + FileRegion.Offset,
					FileRegion.Length, BulkRecord.Buffer));
				SizeFromRegions += FileRegion.Length;
			}
			checkf(SizeFromRegions == BulkRecord.Buffer.GetSize(), TEXT("Expects all BulkData to be in a region."))
		}
		for (const FLinkerAdditionalDataRecord& AdditionalRecord : Record.LinkerAdditionalDatas)
		{
			const uint8* BufferStart = reinterpret_cast<const uint8*>(AdditionalRecord.Buffer.GetData());
			uint64 SizeFromRegions = 0;
			for (const FFileRegion& FileRegion : AdditionalRecord.Regions)
			{
				checkf(FileRegion.Type == EFileRegionType::None,
					TEXT("Does not support FileRegion types other than None."));
				checkf(FileRegion.Offset + FileRegion.Length <= AdditionalRecord.Buffer.GetSize(),
					TEXT("FileRegions in WriteLinkerAdditionalData were outside of the range of the Data's size."));
				check(FileRegion.Length > 0); // SavePackage must not call WriteLinkerAdditionalData with empty regions

				AttachmentBuffers.Add(FSharedBuffer::MakeView(BufferStart + FileRegion.Offset,
					FileRegion.Length, AdditionalRecord.Buffer));
				SizeFromRegions += FileRegion.Length;
			}
			checkf(SizeFromRegions == AdditionalRecord.Buffer.GetSize(),
				TEXT("Expects all LinkerAdditionalData to be in a region."))
		}

		// We use a counter for PayloadIds rather than hashes of the Attachments. We do this because
		// some attachments may be identical, and Attachments are not allowed to have identical PayloadIds.
		// We need to keep the duplicate copies of identical payloads because BulkDatas were written into
		// the exports with offsets that expect all attachment segments to exist in the segmented archive.
		auto IntToPayloadId = [](uint32 Value)
		{
			alignas(decltype(Value)) UE::DerivedData::FPayloadId::ByteArray Bytes{};
			static_assert(sizeof(Bytes) >= sizeof(Value), "We are storing an integer counter in the Bytes array");
			// The PayloadIds are sorted as an array of bytes, so the bytes of the integer must be written big-endian
			for (int ByteIndex = 0; ByteIndex < sizeof(Value); ++ByteIndex)
			{
				Bytes[sizeof(Bytes) - 1 - ByteIndex] = static_cast<uint8>(Value & 0xff);
				Value >>= 8;
			}
			return UE::DerivedData::FPayloadId(Bytes);
		};

		uint32 AttachmentIndex = 1; // 0 is not a valid value for IntToPayloadId
		Attachments.Reserve(AttachmentBuffers.Num());
		FileSize = 0;
		for (const FSharedBuffer& Buffer : AttachmentBuffers)
		{
			Attachments.Add(FAttachment{ Buffer, IntToPayloadId(AttachmentIndex++) });
			FileSize += Buffer.GetSize();
		}
		WritePackageRecord = MoveTemp(*Record.Package);

		return TFuture<FMD5Hash>();
	}

private:
	TArray<FAttachment> Attachments;
	FPackageWriterRecords::FWritePackage WritePackageRecord;
	uint64& FileSize;
};


bool TrySavePackage(UPackage* Package)
{
	using namespace UE::DerivedData;

	FString ErrorMessage;
	FPackageDigest PackageDigest;
	EDomainUse EditorDomainUse;
	TArray<FGuid> CustomVersionGuids;
	EPackageDigestResult FindHashResult = GetPackageDigest(*IAssetRegistry::Get(), Package->GetFName(), PackageDigest,
		EditorDomainUse, ErrorMessage, &CustomVersionGuids);
	switch (FindHashResult)
	{
	case EPackageDigestResult::Success:
		break;
	default:
		UE_LOG(LogEditorDomain, Warning, TEXT("Could not save package to EditorDomain: %s."), *ErrorMessage)
		return false;
	}
	if (!EnumHasAnyFlags(EditorDomainUse, EDomainUse::SaveEnabled))
	{
		UE_LOG(LogEditorDomain, Verbose, TEXT("Skipping save of blocked package to EditorDomain: %s."), *Package->GetName())
		return false;
	}
	UE_LOG(LogEditorDomain, Verbose, TEXT("Saving to EditorDomain: %s."), *Package->GetName())

	uint32 SaveFlags = SAVE_NoError // Do not crash the SaveServer on an error
		| SAVE_BulkDataByReference	// EditorDomain saves reference bulkdata from the WorkspaceDomain rather than duplicating it
		| SAVE_Async				// SavePackage support for PackageWriter is only implemented with SAVE_Async
		// EDITOR_DOMAIN_TODO: Add a a save flag that specifies the creation of a deterministic guid
		// | SAVE_KeepGUID;			// Prevent indeterminism by keeping the Guid
		;

	if (GetEditorDomainSaveUnversioned())
	{
		// With some exceptions, EditorDomain packages are saved unversioned; 
		// editors request the appropriate version of the EditorDomain package matching their serialization version
		bool bSaveUnversioned = true;
		TArray<UObject*> PackageObjects;
		GetObjectsWithPackage(Package, PackageObjects);
		for (UObject* Object : PackageObjects)
		{
			UClass* Class = Object ? Object->GetClass() : nullptr;
			if (Class && Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
			{
				// EDITOR_DOMAIN_TODO: Revisit this once we track package schemas
				// Packages with Blueprint class instances can not be saved unversioned,
				// as the Blueprint class's layout can change during the editor's lifetime,
				// and we don't currently have a way to keep track of the changing package schema
				bSaveUnversioned = false;
			}
		}
		SaveFlags |= bSaveUnversioned ? SAVE_Unversioned_Properties : 0;
	}

	uint64 FileSize = 0;
	FEditorDomainPackageWriter* PackageWriter = new FEditorDomainPackageWriter(FileSize);
	IPackageWriter::FBeginPackageInfo BeginInfo;
	BeginInfo.PackageName = Package->GetFName();
	PackageWriter->BeginPackage(BeginInfo);
	FSavePackageContext SavePackageContext(nullptr /* TargetPlatform */, PackageWriter);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	SaveArgs.SaveFlags = SaveFlags;
	SaveArgs.bSlowTask = false;
	SaveArgs.SavePackageContext = &SavePackageContext;
	FSavePackageResultStruct Result = GEditor->Save(Package, nullptr, TEXT("EditorDomainPackageWriter"), SaveArgs);
	if (Result.Result != ESavePackageResult::Success)
	{
		UE_LOG(LogEditorDomain, Warning, TEXT("Could not save %s to EditorDomain: SavePackage returned %d."),
			*Package->GetName(), Result.Result);
		return false;
	}

	ICookedPackageWriter::FCommitPackageInfo Info;
	Info.bSucceeded = true;
	Info.PackageName = Package->GetFName();
	Info.WriteOptions = IPackageWriter::EWriteOptions::Write;
	PackageWriter->CommitPackage(MoveTemp(Info));

	FCustomVersionContainer SavedCustomVersions;
	if (!PackageWriter->TryGetCustomVersions(SavedCustomVersions))
	{
		UE_LOG(LogEditorDomain, Warning, TEXT("Could not save %s to EditorDomain: Could not read the PackageFileSummary from the saved bytes."),
			*Package->GetName());
		return false;
	}
	TSet<FGuid> KnownGuids;
	KnownGuids.Reserve(CustomVersionGuids.Num());
	for (const FGuid& CustomVersionGuid : CustomVersionGuids)
	{
		KnownGuids.Add(CustomVersionGuid);
	}
	TArray<FGuid> UnknownGuids;
	for (const FCustomVersion& CustomVersion : SavedCustomVersions.GetAllVersions())
	{
		if (!KnownGuids.Contains(CustomVersion.Key))
		{
			UnknownGuids.Add(CustomVersion.Key);
		}
	}
	if (!UnknownGuids.IsEmpty())
	{
		TMap<FGuid, UObject*> Culprits = FindCustomVersionCulprits(UnknownGuids, Package);

		// First check whether the culprit for (one of) the missing CustomVersion is an instance
		// that was added during PostLoad. If so, advise adding an entry to PostLoadCanConstructClasses.
		UObject* ConstructedCulprit = nullptr;
		TOptional<FAssetPackageData> PackageData = IAssetRegistry::Get()->GetAssetPackageDataCopy(Package->GetFName());
		for (const FGuid& CustomVersionGuid : UnknownGuids)
		{
			UObject* Culprit = Culprits.FindOrAdd(CustomVersionGuid);
			FName CulpritClassName = Culprit ? FName(*Culprit->GetClass()->GetPathName()) : NAME_None;
			if (CulpritClassName.IsNone() || PackageData->ImportedClasses.Contains(CulpritClassName))
			{
				continue;
			}
			// If the culprit class does not declare the version either, then we still need to give the message
			// advising adding an entry in DeclareCustomVersions
			bool bConstructedClassDeclaresTheVersion = true;
			{
				PrecacheClassDigests({ CulpritClassName });
				FClassDigestMap& ClassDigests = GetClassDigests();
				FReadScopeLock ClassDigestsScopeLock(ClassDigests.Lock);
				FClassDigestData* ExistingData = ClassDigests.Map.Find(CulpritClassName);
				if (!ExistingData)
				{
					bConstructedClassDeclaresTheVersion = false;
				}
				else
				{
					TArray<FGuid> ClassCustomVersionGuids;
					FKnownCustomVersions::FindGuidsChecked(ClassCustomVersionGuids, ExistingData->CustomVersionHandles);
					bConstructedClassDeclaresTheVersion = ClassCustomVersionGuids.Contains(CustomVersionGuid);
				}
			}
			if (bConstructedClassDeclaresTheVersion)
			{
				ConstructedCulprit = Culprit;
				break;
			}
		}
		TStringBuilder<128> FixupSuggestion;
		if (ConstructedCulprit)
		{
			// Suggested debugging technique for this message: Add a conditional breakpoint on the packagename
			// at the start of LoadPackageInternal. After it gets hit, add a breakpoint in the constructor
			// of the ConstructedCulprit class.
			FixupSuggestion << TEXT("The custom version is used by a class which was created after load of the package. ")
				<< TEXT("Find the class that added ") << ConstructedCulprit->GetFullName() << TEXT(" and add ")
				<< TEXT("Editor.ini:[EditorDomain]:+PostLoadCanConstructClasses=<ConstructingClass>,")
				<< ConstructedCulprit->GetClass()->GetPathName();
		}
		else
		{
			// Suggested debugging technique for this message: SetNextStatement back to beginning of the function,
			// add a conditional breakpoint in FArchive::UsingCustomVersion with Key.A == 0x<FirstHexWordFromGuid>
			FixupSuggestion << TEXT("Modify the classes or structs used in the package to call Ar.UsingCustomVersion(Guid) in Serialize or DeclareCustomVersions.");
			for (const FGuid& CustomVersionGuid : UnknownGuids)
			{
				TOptional<FCustomVersion> CustomVersion = FCurrentCustomVersions::Get(CustomVersionGuid);
				UObject* Culprit = Culprits.FindOrAdd(CustomVersionGuid);
				FixupSuggestion << TEXT("\n\tCustomVersion(Guid=") << CustomVersionGuid << TEXT(", Name=")
					<< (CustomVersion ? *CustomVersion->GetFriendlyName().ToString() : TEXT("<Unknown>"))
					<< TEXT("): Used by ")
					<< (Culprit ? *Culprit->GetClass()->GetPathName() : TEXT("<CulpritUnknown>"));
			}

		}
		UE_LOG(LogEditorDomain, Display, TEXT("Could not save %s to EditorDomain: It uses an unexpected custom version. ")
			TEXT("Optimized loading and iterative cooking will be disabled for this package.\n\t%s"),
			*Package->GetName(), FixupSuggestion.ToString());
		return false;
	}

	ICache& Cache = GetCache();

	TCbWriter<16> MetaData;
	MetaData.BeginObject();
	MetaData << "FileSize" << FileSize;
	MetaData.EndObject();

	FCacheRecordBuilder RecordBuilder(GetEditorDomainPackageKey(PackageDigest));
	for (const FEditorDomainPackageWriter::FAttachment& Attachment : PackageWriter->GetAttachments())
	{
		RecordBuilder.AddAttachment(Attachment.Buffer, Attachment.PayloadId);
	}
	RecordBuilder.SetMeta(MetaData.Save().AsObject());
	FRequestOwner Owner(EPriority::Normal);
	Cache.Put({RecordBuilder.Build()}, Package->GetName(), ECachePolicy::Default, Owner);
	Owner.KeepAlive();

	// TODO_BuildDefinitionList: Calculate and store BuildDefinitionList on the PackageData, or collect it here from some other source.
	TArray<UE::DerivedData::FBuildDefinition> BuildDefinitions;
	FCbObject BuildDefinitionList = UE::TargetDomain::BuildDefinitionListToObject(BuildDefinitions);
	FCbObject TargetDomainDependencies = UE::TargetDomain::CollectDependenciesObject(Package, nullptr, nullptr);
	if (TargetDomainDependencies)
	{
		TArray<IPackageWriter::FCommitAttachmentInfo, TInlineAllocator<2>> Attachments;
		Attachments.Add({ "Dependencies", TargetDomainDependencies });
		// TODO: Reenable BuildDefinitionList once FCbPackage support for empty FCbObjects is in
		//Attachments.Add({ "BuildDefinitionList", BuildDefinitionList });
		UE::TargetDomain::CommitEditorDomainCookAttachments(Package->GetFName(), Attachments);
	}
	return true;
}

void GetBulkDataList(FName PackageName, UE::DerivedData::IRequestOwner& Owner, TUniqueFunction<void(FSharedBuffer Buffer)>&& Callback)
{
	FString ErrorMessage;
	FPackageDigest PackageDigest;
	EDomainUse EditorDomainUse;
	EPackageDigestResult FindHashResult = GetPackageDigest(*IAssetRegistry::Get(), PackageName, PackageDigest,
		EditorDomainUse, ErrorMessage);
	switch (FindHashResult)
	{
	case EPackageDigestResult::Success:
		break;
	default:
	{
		Callback(FSharedBuffer());
		return;
	}
	}
	if (!EnumHasAnyFlags(EditorDomainUse, EDomainUse::LoadEnabled))
	{
		Callback(FSharedBuffer());
		return;
	}

	using namespace UE::DerivedData;
	ICache& Cache = GetCache();
	Cache.Get({ GetBulkDataListKey(PackageDigest) },
		WriteToString<128>(PackageName), ECachePolicy::Default, Owner,
		[InnerCallback = MoveTemp(Callback)](FCacheGetCompleteParams&& Params)
		{
			bool bOk = Params.Status == EStatus::Ok;
			InnerCallback(bOk ? Params.Record.GetValue() : FSharedBuffer());
		});
}

void PutBulkDataList(FName PackageName, FSharedBuffer Buffer)
{
	FString ErrorMessage;
	FPackageDigest PackageDigest;
	EDomainUse EditorDomainUse;
	EPackageDigestResult FindHashResult = GetPackageDigest(*IAssetRegistry::Get(), PackageName, PackageDigest,
		EditorDomainUse, ErrorMessage);
	switch (FindHashResult)
	{
	case EPackageDigestResult::Success:
		break;
	default:
	{
		return;
	}
	}
	if (!EnumHasAnyFlags(EditorDomainUse, EDomainUse::SaveEnabled))
	{
		return;
	}

	using namespace UE::DerivedData;
	ICache& Cache = GetCache();
	FRequestOwner Owner(EPriority::Normal);
	FCacheRecordBuilder RecordBuilder(GetBulkDataListKey(PackageDigest));
	RecordBuilder.SetValue(Buffer);
	Cache.Put({RecordBuilder.Build()}, WriteToString<128>(PackageName), ECachePolicy::Default, Owner);
	Owner.KeepAlive();
}

FIoHash GetPackageAndGuidDigest(FBlake3& Builder, const FGuid& BulkDataId)
{
	Builder.Update(&BulkDataId, sizeof(BulkDataId));
	return Builder.Finalize();
}

void GetBulkDataPayloadId(FName PackageName, const FGuid& BulkDataId, UE::DerivedData::IRequestOwner& Owner,
	TUniqueFunction<void(FSharedBuffer Buffer)>&& Callback)
{
	FString ErrorMessage;
	FBlake3 Builder;
	EDomainUse EditorDomainUse;
	EPackageDigestResult FindHashResult = AppendPackageDigest(*IAssetRegistry::Get(), PackageName, Builder, EditorDomainUse, ErrorMessage);
	switch (FindHashResult)
	{
	case EPackageDigestResult::Success:
		break;
	default:
	{
		Callback(FSharedBuffer());
		return;
	}
	}
	if (!EnumHasAnyFlags(EditorDomainUse, EDomainUse::LoadEnabled))
	{
		Callback(FSharedBuffer());
		return;
	}
	FIoHash PackageAndGuidDigest = GetPackageAndGuidDigest(Builder, BulkDataId);

	using namespace UE::DerivedData;
	ICache& Cache = GetCache();
	Cache.Get({ GetBulkDataPayloadIdKey(PackageAndGuidDigest) },
		WriteToString<192>(PackageName, TEXT("/"), BulkDataId), ECachePolicy::Default, Owner,
		[InnerCallback = MoveTemp(Callback)](FCacheGetCompleteParams&& Params)
	{
		bool bOk = Params.Status == EStatus::Ok;
		InnerCallback(bOk ? Params.Record.GetValue() : FSharedBuffer());
	});
}

void PutBulkDataPayloadId(FName PackageName, const FGuid& BulkDataId, FSharedBuffer Buffer)
{
	FString ErrorMessage;
	FBlake3 Builder;
	EDomainUse EditorDomainUse;
	EPackageDigestResult FindHashResult = AppendPackageDigest(*IAssetRegistry::Get(), PackageName, Builder, EditorDomainUse, ErrorMessage);
	switch (FindHashResult)
	{
	case EPackageDigestResult::Success:
		break;
	default:
	{
		return;
	}
	}
	if (!EnumHasAnyFlags(EditorDomainUse, EDomainUse::SaveEnabled))
	{
		return;
	}
	FIoHash PackageAndGuidDigest = GetPackageAndGuidDigest(Builder, BulkDataId);

	using namespace UE::DerivedData;
	ICache& Cache = GetCache();
	FRequestOwner Owner(EPriority::Normal);
	FCacheRecordBuilder RecordBuilder(GetBulkDataPayloadIdKey(PackageAndGuidDigest));
	RecordBuilder.SetValue(Buffer);
	Cache.Put({RecordBuilder.Build()}, WriteToString<128>(PackageName), ECachePolicy::Default, Owner);
	Owner.KeepAlive();
}

}