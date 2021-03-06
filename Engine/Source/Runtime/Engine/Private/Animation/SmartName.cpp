// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/SmartName.h"
#include "UObject/FrameworkObjectVersion.h"
#include "Animation/Skeleton.h"
#include "UObject/AnimPhysObjectVersion.h"
#include "Misc/ScopeRWLock.h"

////////////////////////////////////////////////////////////////////////
//
// FSmartNameMapping
//
///////////////////////////////////////////////////////////////////////
FSmartNameMapping::FSmartNameMapping()
	: RWLock(nullptr)
{
}

FSmartNameMapping::FSmartNameMapping(FRWLock* Lock)
	: RWLock(Lock)
{
}

void FSmartNameMapping::Iterate(TFunction<void(const FSmartNameMapping* Mapping, SmartName::UID_Type ID)> Callback) const
{
	FReadScopeLock Lock(*RWLock);

	for (int32 NameIndex = 0; NameIndex < CurveNameList.Num(); ++NameIndex)
	{
		Callback(this, NameIndex);	
	}
}

FSmartName FSmartNameMapping::AddName(FName Name)
{
	FWriteScopeLock Lock(*RWLock);
	
	// Make sure we are not trying to do an invalid add
	check(Name.IsValid() && !CurveNameList.Contains(Name));

	// Make sure we didn't reach the UID limit
	check(CurveNameList.Num() < (SmartName::MaxUID-1));

	FSmartName NewSmartName(Name, CurveNameList.Add(Name));
	CurveMetaDataMap.Add(Name);
#if !WITH_EDITOR
	CurveMetaDataList.AddDefaulted();
#endif
	return NewSmartName;
}

FCurveMetaData* FSmartNameMapping::GetCurveMetaData(FName CurveName)
{
	checkSlow(Exists(CurveName));
	FWriteScopeLock Lock(*RWLock);
	return &CurveMetaDataMap.FindOrAdd(CurveName);
}

const FCurveMetaData* FSmartNameMapping::GetCurveMetaData(FName CurveName) const
{
	checkSlow(Exists(CurveName));
	FReadScopeLock Lock(*RWLock);
	return CurveMetaDataMap.Find(CurveName);
}

#if !WITH_EDITOR
const FCurveMetaData& FSmartNameMapping::GetCurveMetaData(SmartName::UID_Type CurveUID) const
{
	FReadScopeLock Lock(*RWLock);
	return CurveMetaDataList[CurveUID];
}
#endif

bool FSmartNameMapping::GetName(const SmartName::UID_Type& Uid, FName& OutName) const
{
	FReadScopeLock Lock(*RWLock);
	
	if (CurveNameList.IsValidIndex(Uid))
	{
		OutName = CurveNameList[Uid];
		return OutName != NAME_None; // Name may have been removed
	}
	return false;
}

#if WITH_EDITOR
bool FSmartNameMapping::Rename(const SmartName::UID_Type& Uid, FName NewName)
{
	FWriteScopeLock Lock(*RWLock);
	
	FName ExistingName;
	if(GetName(Uid, ExistingName))
	{
		// fix up meta data
		FCurveMetaData* MetaDataToCopy = CurveMetaDataMap.Find(ExistingName);
		if (MetaDataToCopy)
		{
			FCurveMetaData& NewMetaData = CurveMetaDataMap.Add(NewName);
			NewMetaData = *MetaDataToCopy;
			
			// remove old one
			CurveMetaDataMap.Remove(ExistingName);			
		}

		CurveNameList[Uid] = NewName;
		return true;
	}
	return false;
}

bool FSmartNameMapping::Remove(const SmartName::UID_Type& Uid)
{
	FWriteScopeLock Lock(*RWLock);
	
	FName ExistingName;
	if (GetName(Uid, ExistingName))
	{
		CurveMetaDataMap.Remove(ExistingName);
		CurveNameList[Uid] = NAME_None;

		return true;
	}
	return false;
}

bool FSmartNameMapping::Remove(const FName& Name)
{
	FWriteScopeLock Lock(*RWLock);
	
	const SmartName::UID_Type Uid = FindUID(Name);
	if (Uid != SmartName::MaxUID)
	{
		CurveMetaDataMap.Remove(Name);
		CurveNameList[Uid] = NAME_None;
		return true;
	}
	return false;
}
#endif

void FSmartNameMapping::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FFrameworkObjectVersion::GUID);
	Ar.UsingCustomVersion(FAnimPhysObjectVersion::GUID);
	if (Ar.CustomVer(FFrameworkObjectVersion::GUID) >= FFrameworkObjectVersion::SmartNameRefactor)
	{
		if (Ar.CustomVer(FAnimPhysObjectVersion::GUID) < FAnimPhysObjectVersion::SmartNameRefactorForDeterministicCooking)
		{
			TMap<FName, FGuid> TempGuidMap;
			Ar << TempGuidMap;
		}
	}
	else if(Ar.UEVer() >= VER_UE4_SKELETON_ADD_SMARTNAMES)
	{
		SmartName::UID_Type NextUidTemp;
		Ar << NextUidTemp;

		TMap<SmartName::UID_Type, FName> TempUidMap;
		Ar << TempUidMap;
	}

	if (Ar.CustomVer(FFrameworkObjectVersion::GUID) >= FFrameworkObjectVersion::MoveCurveTypesToSkeleton)
	{
		Ar << CurveMetaDataMap;
	}

	if (Ar.IsLoading())
	{
		CurveMetaDataMap.GenerateKeyArray(CurveNameList);
#if !WITH_EDITOR
		CurveMetaDataMap.GenerateValueArray(CurveMetaDataList);
#endif
	}
}

void FSmartNameMapping::FillUidArray(TArray<SmartName::UID_Type>& Array) const
{
	FReadScopeLock Lock(*RWLock);
	
	Array.Reset(CurveNameList.Num());
	
	for (int32 NameIndex = 0; NameIndex < CurveNameList.Num(); ++NameIndex)
	{
		//In editor names can be removed and so have to deal with empty slots
#if WITH_EDITOR
		if (CurveNameList[NameIndex] != NAME_None)
		{
			Array.Add(NameIndex);
		}
#else
		Array.Add(NameIndex);
#endif
	}
}

void FSmartNameMapping::FillNameArray(TArray<FName>& Array) const
{
	FReadScopeLock Lock(*RWLock);
	
	//In editor names can be removed and so have to deal with empty slots
#if WITH_EDITOR
	Array.Reset(CurveNameList.Num());
	for (const FName& Name : CurveNameList)
	{
		if (Name != NAME_None)
		{
			Array.Add(Name);
		}
	}
#else
	Array = CurveNameList;
#endif
}

void FSmartNameMapping::FillUIDToNameArray(TArray<FName>& Array) const
{
	Array = CurveNameList;
}

void FSmartNameMapping::FillCurveTypeArray(TArray<FAnimCurveType>& Array) const
{
	FReadScopeLock Lock(*RWLock);
	Array.Reset(CurveNameList.Num());

	for (const FName& Name : CurveNameList)
	{
		const FCurveMetaData* MetaData = CurveMetaDataMap.Find(Name);
		FAnimCurveType AnimCurveType = MetaData ? MetaData->Type : FAnimCurveType();

		//In editor names can be removed and so have to deal with empty slots
#if WITH_EDITOR
		if (Name != NAME_None)
		{
			Array.Add(AnimCurveType);
		}
#else
		Array.Add(AnimCurveType);
#endif
	}
}

void FSmartNameMapping::FillUIDToCurveTypeArray(TArray<FAnimCurveType>& Array) const
{
	FReadScopeLock Lock(*RWLock);
	
	Array.Reset(CurveNameList.Num());

	for (const FName& Name : CurveNameList)
	{
		const FCurveMetaData* MetaData = CurveMetaDataMap.Find(Name);
		FAnimCurveType AnimCurveType = MetaData ? MetaData->Type : FAnimCurveType();

		//In editor names can be removed and so have to deal with empty slots
#if WITH_EDITOR
		if (Name != NAME_None)
		{
			Array.Add(AnimCurveType);
		}
		else
		{
			Array.AddDefaulted();
		}
#else
		Array.Add(AnimCurveType);
#endif
	}
}


bool FSmartNameMapping::Exists(const SmartName::UID_Type& Uid) const
{
	FReadScopeLock Lock(*RWLock);
	return CurveNameList.IsValidIndex(Uid) && CurveNameList[Uid] != NAME_None;
}

bool FSmartNameMapping::Exists(const FName& Name) const
{
	FReadScopeLock Lock(*RWLock);
	return CurveNameList.Contains(Name);
}

SmartName::UID_Type FSmartNameMapping::FindUID(const FName& Name) const
{
	FReadScopeLock Lock(*RWLock);
	return CurveNameList.IndexOfByKey(Name);
}

FArchive& operator<<(FArchive& Ar, FSmartNameMapping& Elem)
{
	Elem.Serialize(Ar);

	return Ar;
}

bool FSmartNameMapping::FindSmartName(FName Name, FSmartName& OutName) const
{
	FReadScopeLock Lock(*RWLock);
	SmartName::UID_Type ExistingUID = FindUID(Name);
	if (ExistingUID != SmartName::MaxUID)
	{
		OutName = FSmartName(Name, ExistingUID);
		return true;
	}

	return false;
}

bool FSmartNameMapping::FindSmartNameByUID(SmartName::UID_Type UID, FSmartName& OutName) const
{
	FReadScopeLock Lock(*RWLock);
	FName ExistingName;
	if (GetName(UID, ExistingName))
	{
		OutName.DisplayName = ExistingName;
		OutName.UID = UID;
		return true;
	}

	return false;
}

/* initialize curve meta data for the container */
void FSmartNameMapping::InitializeCurveMetaData(class USkeleton* Skeleton)
{
	FWriteScopeLock Lock(*RWLock);
	
	// initialize bone indices for skeleton
	for (TPair<FName, FCurveMetaData>& Iter : CurveMetaDataMap)
	{
		FCurveMetaData& CurveMetaData = Iter.Value;
		for (int32 LinkedBoneIndex = 0; LinkedBoneIndex < CurveMetaData.LinkedBones.Num(); ++LinkedBoneIndex)
		{
			CurveMetaData.LinkedBones[LinkedBoneIndex].Initialize(Skeleton);
		}
	}
}
////////////////////////////////////////////////////////////////////////
//
// FSmartNameContainer
//
//////////////////////////////////////////////////////////////////////
FSmartNameMapping* FSmartNameContainer::AddContainer(FName NewContainerName)
{
	FWriteScopeLock Lock(RWLock);
	
	FSmartNameMapping* ExistingMapping = NameMappings.Find(NewContainerName);
	if(ExistingMapping == nullptr)
	{
		return &NameMappings.Add(NewContainerName, {&RWLock});
	}

	return ExistingMapping;
}

const FSmartNameMapping* FSmartNameContainer::GetContainer(FName ContainerName) const
{
	return NameMappings.Find(ContainerName);
}

void FSmartNameContainer::Serialize(FArchive& Ar, bool bIsTemplate)
{
#if WITH_EDITORONLY_DATA
	if (Ar.IsCooking() && !bIsTemplate)
	{
		Ar << LoadedNameMappings;
	}
	else
#endif
	{
		Ar << NameMappings;
	}

}

void FSmartNameContainer::PostLoad()
{
#if WITH_EDITORONLY_DATA
	// Preserve Load state for deterministic cooking
	LoadedNameMappings = NameMappings;
#endif
	
	for(auto& Mapping : NameMappings)
	{
		Mapping.Value.SetLock(&RWLock);
	}
}

FSmartNameMapping* FSmartNameContainer::GetContainerInternal(const FName& ContainerName)
{
	return NameMappings.Find(ContainerName);
}

const FSmartNameMapping* FSmartNameContainer::GetContainerInternal(const FName& ContainerName) const
{
	return NameMappings.Find(ContainerName);
}

////////////////////////////////////////////////////////////////////////
//
// FSmartName
//
///////////////////////////////////////////////////////////////////////
bool FSmartName::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimPhysObjectVersion::GUID);
	Ar << DisplayName;
	if (Ar.CustomVer(FAnimPhysObjectVersion::GUID) < FAnimPhysObjectVersion::RemoveUIDFromSmartNameSerialize)
	{
		SmartName::UID_Type TempUID;
		Ar << TempUID;
	}
#if WITH_EDITOR
	else if (Ar.IsTransacting() || Ar.HasAnyPortFlags(PPF_Duplicate))
	{
		Ar << UID;
	}
#endif

	// only save if it's editor build and not cooking
	if (Ar.CustomVer(FAnimPhysObjectVersion::GUID) < FAnimPhysObjectVersion::SmartNameRefactorForDeterministicCooking)
	{
		FGuid TempGUID;
		Ar << TempGUID;
	}

	return true;
}
