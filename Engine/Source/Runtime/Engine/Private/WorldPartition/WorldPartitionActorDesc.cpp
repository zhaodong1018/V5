// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/WorldPartitionActorDesc.h"

#if WITH_EDITOR
#include "Misc/HashBuilder.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/UObjectHash.h"
#include "UObject/MetaData.h"
#include "Algo/Transform.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Editor/GroupActor.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "UObject/UE5ReleaseStreamObjectVersion.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/ActorDescContainer.h"
#include "WorldPartition/DataLayer/DataLayer.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "Engine/Public/ActorReferencesUtils.h"
#endif

#if WITH_EDITOR
uint32 FWorldPartitionActorDesc::GlobalTag = 0;

FWorldPartitionActorDesc::FWorldPartitionActorDesc()
	: SoftRefCount(0)
	, HardRefCount(0)
	, Container(nullptr)
	, Tag(0)
{}

void FWorldPartitionActorDesc::Init(const AActor* InActor)
{	
	check(InActor->IsPackageExternal());

	Guid = InActor->GetActorGuid();
	check(Guid.IsValid());

	// Get the first native class in the hierarchy
	ActorClass = GetParentNativeClass(InActor->GetClass());
	Class = ActorClass->GetFName();

	const FBox StreamingBounds = InActor->GetStreamingBounds();
	StreamingBounds.GetCenterAndExtents(BoundsLocation, BoundsExtent);

	const EActorGridPlacement DefaultGridPlacement = InActor->GetDefaultGridPlacement();
	if (DefaultGridPlacement != EActorGridPlacement::None)
	{
		GridPlacement = DefaultGridPlacement;
	}
	else
	{
		GridPlacement = InActor->GetGridPlacement();
	}

	RuntimeGrid = InActor->GetRuntimeGrid();
	bActorIsEditorOnly = InActor->IsEditorOnly();
	bLevelBoundsRelevant = InActor->IsLevelBoundsRelevant();
	bActorIsHLODRelevant = InActor->IsHLODRelevant();
	HLODLayer = InActor->GetHLODLayer() ? FName(InActor->GetHLODLayer()->GetPathName()) : FName();
	DataLayers = InActor->GetDataLayerNames();
	ActorPackage = InActor->GetPackage()->GetFName();
	ActorPath = *InActor->GetPathName();
	FolderPath = InActor->GetFolderPath();
	
	TArray<AActor*> ActorReferences = ActorsReferencesUtils::GetExternalActorReferences((AActor*)InActor);

	if (ActorReferences.Num())
	{
		References.Empty(ActorReferences.Num());
		for(AActor* ActorReference: ActorReferences)
		{
			References.Add(ActorReference->GetActorGuid());
		}
	}

	ActorLabel = *InActor->GetActorLabel(false);

	// Only set ActorPtr/Container on WorldPartition ActorDescs
	if (UWorldPartition* WorldPartition = InActor->GetLevel()->GetWorldPartition())
	{
		ActorPtr = (AActor*)InActor;
		Container = WorldPartition;
	}
}

void FWorldPartitionActorDesc::Init(UActorDescContainer* InContainer, const FWorldPartitionActorDescInitData& DescData)
{
	ActorPackage = DescData.PackageName;
	ActorPath = DescData.ActorPath;
	ActorClass = DescData.NativeClass;
	Class = DescData.NativeClass->GetFName();

	// Serialize actor metadata
	FMemoryReader MetadataAr(DescData.SerializedData, true);

	// Serialize metadata custom versions
	FCustomVersionContainer CustomVersions;
	CustomVersions.Serialize(MetadataAr);
	MetadataAr.SetCustomVersions(CustomVersions);
	
	// Serialize metadata payload
	Serialize(MetadataAr);

	// Override grid placement by default class value
	const EActorGridPlacement DefaultGridPlacement = ActorClass->GetDefaultObject<AActor>()->GetDefaultGridPlacement();
	if (DefaultGridPlacement != EActorGridPlacement::None)
	{
		GridPlacement = DefaultGridPlacement;
	}

	// Only set ActorPtr/Container on WorldPartition ActorDescs
	if (UWorldPartition* WorldPartition = Cast<UWorldPartition>(InContainer))
	{
		Container = InContainer;
		ActorPtr = FindObject<AActor>(nullptr, *ActorPath.ToString());
	}
}

bool FWorldPartitionActorDesc::Equals(const FWorldPartitionActorDesc* Other) const
{
	if (Guid == Other->Guid && 
		Class == Other->Class && 
		ActorPackage == Other->ActorPackage && 
		ActorPath == Other->ActorPath && 
		ActorLabel == Other->ActorLabel && 
		BoundsLocation.Equals(Other->BoundsLocation, 0.1f) &&
		BoundsExtent.Equals(Other->BoundsExtent, 0.1f) && 
		RuntimeGrid == Other->RuntimeGrid && 
		bActorIsEditorOnly == Other->bActorIsEditorOnly && 
		bLevelBoundsRelevant == Other->bLevelBoundsRelevant && 
		bActorIsHLODRelevant == Other->bActorIsHLODRelevant && 
		HLODLayer == Other->HLODLayer && 
		FolderPath == Other->FolderPath &&
		DataLayers.Num() == Other->DataLayers.Num() &&
		References.Num() == Other->References.Num())
	{
		TArray<FName> SortedDataLayers(DataLayers);
		TArray<FName> SortedDataLayersOther(Other->DataLayers);
		SortedDataLayers.Sort([](const FName& LHS, const FName& RHS) { return LHS.LexicalLess(RHS); });
		SortedDataLayersOther.Sort([](const FName& LHS, const FName& RHS) { return LHS.LexicalLess(RHS); });

		TArray<FGuid> SortedReferences(References);
		TArray<FGuid> SortedReferencesOther(Other->References);
		SortedReferences.Sort();
		SortedReferencesOther.Sort();

		return SortedDataLayers == SortedDataLayersOther && SortedReferences == SortedReferencesOther;
	}

	return false;
}

void FWorldPartitionActorDesc::SerializeTo(TArray<uint8>& OutData)
{
	// Serialize to archive and gather custom versions
	TArray<uint8> PayloadData;
	FMemoryWriter PayloadAr(PayloadData, true);
	Serialize(PayloadAr);

	// Serialize custom versions
	TArray<uint8> HeaderData;
	FMemoryWriter HeaderAr(HeaderData);
	FCustomVersionContainer CustomVersions = PayloadAr.GetCustomVersions();
	CustomVersions.Serialize(HeaderAr);

	// Append data
	OutData = MoveTemp(HeaderData);
	OutData.Append(PayloadData);
}

void FWorldPartitionActorDesc::TransformInstance(const FString& From, const FString& To)
{
	check(!HardRefCount);

	ActorPath = *ActorPath.ToString().Replace(*From, *To);
}

FString FWorldPartitionActorDesc::ToString() const
{
	return FString::Printf(TEXT("Guid:%s Class:%s Name:%s"), *Guid.ToString(), *Class.ToString(), *FPaths::GetExtension(ActorPath.ToString()));
}

void FWorldPartitionActorDesc::Serialize(FArchive& Ar)
{
	check(Ar.IsPersistent());

	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5ReleaseStreamObjectVersion::GUID);

	Ar << Class << Guid << BoundsLocation << BoundsExtent << GridPlacement << RuntimeGrid << bActorIsEditorOnly << bLevelBoundsRelevant;
	
	if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::WorldPartitionActorDescSerializeDataLayers)
	{
		TArray<FName> Deprecated_Layers;
		Ar << Deprecated_Layers;
	}

	Ar << References;

	if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::WorldPartitionActorDescSerializeArchivePersistent)
	{
		Ar << ActorPackage << ActorPath;
	}

	if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::WorldPartitionActorDescSerializeDataLayers)
	{
		Ar << DataLayers;
	}

	if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::WorldPartitionActorDescSerializeActorLabel)
	{
		Ar << ActorLabel;
	}

	if ((Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::WorldPartitionActorDescSerializeHLODInfo) ||
		(Ar.CustomVer(FUE5ReleaseStreamObjectVersion::GUID) >= FUE5ReleaseStreamObjectVersion::WorldPartitionActorDescSerializeHLODInfo))
	{
		Ar << bActorIsHLODRelevant;
		Ar << HLODLayer;
	}
	else
	{
		bActorIsHLODRelevant = true;
		HLODLayer = FName();
	}

	if (Ar.CustomVer(FUE5ReleaseStreamObjectVersion::GUID) >= FUE5ReleaseStreamObjectVersion::WorldPartitionActorDescSerializeActorFolderPath)
	{
		Ar << FolderPath;
	}
}

FBox FWorldPartitionActorDesc::GetBounds() const
{
	return FBox(BoundsLocation - BoundsExtent, BoundsLocation + BoundsExtent);
}

FName FWorldPartitionActorDesc::GetActorName() const
{
	FString ActorName;
	FString ActorContext;
	verify(GetActorPath().ToString().Split(TEXT("."), &ActorContext, &ActorName, ESearchCase::CaseSensitive, ESearchDir::FromEnd));
	return *ActorName;
}

TArray<const UDataLayer*> FWorldPartitionActorDesc::GetDataLayerObjects() const
{
	if (Container)
	{
		if (AWorldDataLayers* WorldDataLayers = Container->GetWorld()->GetWorldDataLayers())
		{
			TArray<const UDataLayer*> DataLayerObjects;
			DataLayerObjects.Reserve(DataLayers.Num());
			for (const FName& DataLayerName : DataLayers)
			{
				if (const UDataLayer* DataLayer = WorldDataLayers->GetDataLayerFromName(DataLayerName))
				{
					DataLayerObjects.Add(DataLayer);
				}
			}

			return DataLayerObjects;
		}
	}

	return TArray<const UDataLayer*>();
}

UHLODLayer* FWorldPartitionActorDesc::GetHLODLayer() const
{
	return HLODLayer.IsNone() ? nullptr : Cast<UHLODLayer>(FSoftObjectPath(HLODLayer).TryLoad());
}

bool FWorldPartitionActorDesc::IsLoaded(bool bEvenIfPendingKill) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GIsAutomationTesting)
	{
		return HardRefCount > 0;
	}
#endif

	return ActorPtr.IsValid(bEvenIfPendingKill);
}

AActor* FWorldPartitionActorDesc::GetActor(bool bEvenIfPendingKill, bool bEvenIfUnreachable) const
{
	return bEvenIfUnreachable ? ActorPtr.GetEvenIfUnreachable() : ActorPtr.Get(bEvenIfPendingKill);
}

AActor* FWorldPartitionActorDesc::Load() const
{
	if (ActorPtr.IsExplicitlyNull())
	{
		// First, try to find the existing actor which could have been loaded by another actor (through standard serialization)
		ActorPtr = FindObject<AActor>(nullptr, *ActorPath.ToString());
	}

	// The, if the actor isn't loaded, load it
	if (ActorPtr.IsExplicitlyNull())
	{
		// For now we assume that an ActorDesc that gets loaded is owned by an actual WorldPartition
		UWorldPartition* WorldPartition = Container ? CastChecked<UWorldPartition>(Container) : nullptr;
		check(WorldPartition || GIsAutomationTesting);

		const FLinkerInstancingContext* InstancingContext = nullptr;
		FSoftObjectPathFixupArchive* SoftObjectPathFixupArchive = nullptr;
		if (WorldPartition && WorldPartition->InstancingContext.IsInstanced())
		{
			InstancingContext = &WorldPartition->InstancingContext;
			SoftObjectPathFixupArchive = WorldPartition->InstancingSoftObjectPathFixupArchive.Get();
		}

		UPackage* Package = nullptr;

		if (InstancingContext)
		{
			FName RemappedPackageName = InstancingContext->Remap(ActorPackage);
			check(RemappedPackageName != ActorPath);

			Package = CreatePackage(*RemappedPackageName.ToString());
		}

		Package = LoadPackage(Package, *ActorPackage.ToString(), LOAD_None, nullptr, InstancingContext);
		check(Package || GIsAutomationTesting);

		if (Package)
		{
			ActorPtr = FindObject<AActor>(nullptr, *ActorPath.ToString());
			if (AActor* Actor = ActorPtr.Get())
			{
				if (SoftObjectPathFixupArchive)
				{
					SoftObjectPathFixupArchive->Fixup(Actor);
				}
			}
			else
			{
				UE_LOG(LogWorldPartition, Warning, TEXT("Can't load actor %s"), *GetActorName().ToString());
			}
		}
	}

	return ActorPtr.Get();
}

void FWorldPartitionActorDesc::Unload()
{
	if (AActor* Actor = GetActor())
	{
		// @todo_ow: FWorldPartitionCookPackageSplitter should mark each FWorldPartitionActorDesc as moved, which we would assert on here rather than asserting IsRunningCookCommandlet
		// and the splitter should take responsbility for calling ClearFlags on every object in the package when it does the move
		check(Actor->IsPackageExternal() || IsRunningCookCommandlet());
		if (Actor->IsPackageExternal())
		{
			ForEachObjectWithPackage(Actor->GetPackage(), [](UObject* Object)
			{
				if (Object->HasAnyFlags(RF_Public | RF_Standalone))
				{
					CastChecked<UMetaData>(Object)->ClearFlags(RF_Public | RF_Standalone);
				}
				return true;
			}, false);
		}

		ActorPtr = nullptr;
	}
}

void FWorldPartitionActorDesc::RegisterActor()
{
	if (AActor* Actor = GetActor())
	{
		check(Container);
		Container->OnActorDescRegistered(*this);
	}
}

void FWorldPartitionActorDesc::UnregisterActor()
{
	if (AActor* Actor = GetActor())
	{
		check(Container);
		Container->OnActorDescUnregistered(*this);
	}
}
#endif