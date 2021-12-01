// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/ActorDescContainer.h"

#if WITH_EDITOR
#include "Editor.h"
#include "AssetRegistryModule.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "Misc/Base64.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/CoreRedirects.h"
#endif

UActorDescContainer::UActorDescContainer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, World(nullptr)
#if WITH_EDITOR
	, bContainerInitialized(false)
#endif
{}

void UActorDescContainer::Initialize(UWorld* InWorld, FName InPackageName, TFunctionRef<void(FWorldPartitionActorDesc*)> PreRegister)
{
	check(!World || World == InWorld);
	World = InWorld;
#if WITH_EDITOR
	check(!bContainerInitialized);
	ContainerPackageName = InPackageName;
	TArray<FAssetData> Assets;
	
	if (!ContainerPackageName.IsNone())
	{
		const FString LevelPathStr = ContainerPackageName.ToString();
		const FString LevelExternalActorsPath = ULevel::GetExternalActorsPath(LevelPathStr);

		// Do a synchronous scan of the level external actors path.			
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		AssetRegistry.ScanPathsSynchronous({ LevelExternalActorsPath }, /*bForceRescan*/false, /*bIgnoreDenyListScanFilters*/false);

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.bIncludeOnlyOnDiskAssets = true;
		Filter.PackagePaths.Add(*LevelExternalActorsPath);

		AssetRegistry.GetAssets(Filter, Assets);
	}

	auto GetActorDescriptor = [this](const FAssetData& InAssetData) -> TUniquePtr<FWorldPartitionActorDesc>
	{
		FString ActorMetaDataClass;
		static FName NAME_ActorMetaDataClass(TEXT("ActorMetaDataClass"));
		if (InAssetData.GetTagValue(NAME_ActorMetaDataClass, ActorMetaDataClass))
		{
			FString ActorMetaDataStr;
			static FName NAME_ActorMetaData(TEXT("ActorMetaData"));
			if (InAssetData.GetTagValue(NAME_ActorMetaData, ActorMetaDataStr))
			{
				FString ActorClassName;
				FString ActorPackageName;
				if (!ActorMetaDataClass.Split(TEXT("."), &ActorPackageName, &ActorClassName))
				{
					ActorClassName = *ActorMetaDataClass;
				}

				// Look for a class redirectors
				FCoreRedirectObjectName OldClassName = FCoreRedirectObjectName(*ActorClassName, NAME_None, *ActorPackageName);
				FCoreRedirectObjectName NewClassName = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, OldClassName);

				bool bIsValidClass = true;
				UClass* ActorClass = FindObject<UClass>(ANY_PACKAGE, *NewClassName.ToString(), true);

				if (!ActorClass)
				{
					ActorClass = AActor::StaticClass();
					bIsValidClass = false;
				}

				FWorldPartitionActorDescInitData ActorDescInitData;
				ActorDescInitData.NativeClass = ActorClass;
				ActorDescInitData.PackageName = InAssetData.PackageName;
				ActorDescInitData.ActorPath = InAssetData.ObjectPath;
				FBase64::Decode(ActorMetaDataStr, ActorDescInitData.SerializedData);

				TUniquePtr<FWorldPartitionActorDesc> NewActorDesc(AActor::CreateClassActorDesc(ActorDescInitData.NativeClass));

				NewActorDesc->Init(this, ActorDescInitData);
			
				//check(bIsValidClass || (NewActorDesc->GetActorIsEditorOnly() && IsRunningGame()));

				if (!bIsValidClass)
				{
					return nullptr;
				}

				return NewActorDesc;
			}
		}

		return nullptr;
	};

	for (const FAssetData& Asset : Assets)
	{
		TUniquePtr<FWorldPartitionActorDesc> ActorDesc = GetActorDescriptor(Asset);

		if (ActorDesc.IsValid())
		{
			AddActorDescriptor(ActorDesc.Release());
		}
	}

	for (FActorDescList::TIterator<> ActorDescIterator(this); ActorDescIterator; ++ActorDescIterator)
	{
		PreRegister(*ActorDescIterator);
		ActorDescIterator->OnRegister(World);
	}

	RegisterEditorDelegates();

	bContainerInitialized = true;
#endif
}

void UActorDescContainer::Uninitialize()
{
#if WITH_EDITOR
	PinnedActors.Empty();
	PinnedActorRefs.Empty();

	if (bContainerInitialized)
	{
		UnregisterEditorDelegates();
		bContainerInitialized = false;
	}

	for (TUniquePtr<FWorldPartitionActorDesc>& ActorDescPtr : ActorDescList)
	{
		if (FWorldPartitionActorDesc* ActorDesc = ActorDescPtr.Get())
		{
			ActorDesc->OnUnregister();
		}
		ActorDescPtr.Reset();
	}
#endif
	World = nullptr;
}

UWorld* UActorDescContainer::GetWorld() const
{
	if (World)
	{
		return World;
	}
	return Super::GetWorld();
}

void UActorDescContainer::BeginDestroy()
{
	Super::BeginDestroy();

	Uninitialize();
}

#if WITH_EDITOR
bool UActorDescContainer::ShouldHandleActorEvent(const AActor* Actor)
{
	return Actor && Actor->IsMainPackageActor() && (Actor->GetLevel() != nullptr) && (Actor->GetLevel()->GetPackage()->GetFName() == ContainerPackageName);
}

void UActorDescContainer::OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
	if (!SaveContext.IsProceduralSave() && !(SaveContext.GetSaveFlags() & SAVE_FromAutosave))
	{
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			if (ShouldHandleActorEvent(Actor))
			{
				check(IsValidChecked(Actor));
				if (TUniquePtr<FWorldPartitionActorDesc>* ExistingActorDesc = GetActorDescriptor(Actor->GetActorGuid()))
				{
					// Pin the actor handle on the actor to prevent unloading it when unhashing
					FWorldPartitionHandle ExistingActorHandle(ExistingActorDesc);
					FWorldPartitionHandlePinRefScope ExistingActorHandlePin(ExistingActorHandle);

					TUniquePtr<FWorldPartitionActorDesc> NewActorDesc(Actor->CreateActorDesc());

					OnActorDescUpdating(ExistingActorDesc->Get());

					// Transfer any internal values not coming from the actor
					NewActorDesc->TransferFrom(ExistingActorDesc->Get());

					*ExistingActorDesc = MoveTemp(NewActorDesc);

					OnActorDescUpdated(ExistingActorDesc->Get());
				}
				// New actor
				else
				{
					FWorldPartitionActorDesc* const AddedActorDesc = AddActor(Actor);
					OnActorDescAdded(AddedActorDesc);
				}
			}
		}
	}
}

void UActorDescContainer::OnPackageDeleted(UPackage* Package)
{
	AActor* Actor = AActor::FindActorInPackage(Package);

	if (ShouldHandleActorEvent(Actor))
	{
		RemoveActor(Actor->GetActorGuid());
	}
}

void UActorDescContainer::OnObjectsReplaced(const TMap<UObject*, UObject*>& OldToNewObjectMap)
{
	// Patch up Actor pointers in ActorDescs
	for (auto Iter = OldToNewObjectMap.CreateConstIterator(); Iter; ++Iter)
	{
		if (AActor* OldActor = Cast<AActor>(Iter->Key))
		{
			if (FWorldPartitionActorDesc* ActorDesc = GetActorDesc(OldActor->GetActorGuid()))
			{
				if (ActorDesc->GetActor() == OldActor)
				{
					ActorDesc->ActorPtr = Cast<AActor>(Iter->Value);
				}
			}
		}
	}
}

void UActorDescContainer::RemoveActor(const FGuid& ActorGuid)
{
	if (TUniquePtr<FWorldPartitionActorDesc>* ExistingActorDesc = GetActorDescriptor(ActorGuid))
	{
		OnActorDescRemoved(ExistingActorDesc->Get());
		RemoveActorDescriptor(ExistingActorDesc->Get());
		ExistingActorDesc->Reset();
	}
}

AActor* UActorDescContainer::PinActor(const FGuid& ActorGuid)
{
	if (FWorldPartitionReference* PinnedActor = PinnedActors.Find(ActorGuid))
	{
		return (*PinnedActor).IsValid() ? (*PinnedActor)->GetActor() : nullptr;
	}

	TFunction<void(const FGuid&, TMap<FGuid, FWorldPartitionReference>&)> AddReferences = [this, &AddReferences](const FGuid& ActorGuid, TMap<FGuid, FWorldPartitionReference>& ReferenceMap)
	{
		if (ReferenceMap.Contains(ActorGuid))
		{
			return;
		}

		if (TUniquePtr<FWorldPartitionActorDesc>* const ActorDescPtr = GetActorDescriptor(ActorGuid); ActorDescPtr != nullptr && ActorDescPtr->IsValid())
		{
			ReferenceMap.Emplace(ActorGuid, ActorDescPtr);
			
			const FWorldPartitionActorDesc* const ActorDesc = ActorDescPtr->Get(); 
			for (const FGuid& ReferencedActor : ActorDesc->GetReferences())
			{
				AddReferences(ReferencedActor, ReferenceMap);
			}
		}
	};
	
	if (TUniquePtr<FWorldPartitionActorDesc>* const ActorDescPtr = GetActorDescriptor(ActorGuid))
	{
		if (const FWorldPartitionActorDesc* const ActorDesc = ActorDescPtr->Get())
		{
			FWorldPartitionReference& PinnedActor = PinnedActors.Emplace(ActorGuid, ActorDescPtr);

			// If the pinned actor has references, we must also create references to those to ensure they are added
			TMap<FGuid, FWorldPartitionReference>& References = PinnedActorRefs.Emplace(ActorGuid);

			for (const FGuid& ReferencedActor : ActorDesc->GetReferences())
			{
				AddReferences(ReferencedActor, References);
			}

			return PinnedActor.IsValid() ? PinnedActor->GetActor() : nullptr;
		}
	}

	return nullptr;
}

void UActorDescContainer::UnpinActor(const FGuid& ActorGuid)
{
	PinnedActors.Remove(ActorGuid);
	PinnedActorRefs.Remove(ActorGuid);
}

void UActorDescContainer::RegisterEditorDelegates()
{
	if (GEditor && !IsTemplate() && !World->IsGameWorld())
	{
		FCoreUObjectDelegates::OnObjectPreSave.AddUObject(this, &UActorDescContainer::OnObjectPreSave);
		FEditorDelegates::OnPackageDeleted.AddUObject(this, &UActorDescContainer::OnPackageDeleted);
		FCoreUObjectDelegates::OnObjectsReplaced.AddUObject(this, &UActorDescContainer::OnObjectsReplaced);
	}
}

void UActorDescContainer::UnregisterEditorDelegates()
{
	if (GEditor && !IsTemplate() && !World->IsGameWorld())
	{
		FCoreUObjectDelegates::OnObjectPreSave.RemoveAll(this);
		FEditorDelegates::OnPackageDeleted.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectsReplaced.RemoveAll(this);
	}
}

void UActorDescContainer::OnActorDescAdded(FWorldPartitionActorDesc* NewActorDesc)
{
	NewActorDesc->OnRegister(World);

	OnActorDescAddedEvent.Broadcast(NewActorDesc);
}

void UActorDescContainer::OnActorDescRemoved(FWorldPartitionActorDesc* ActorDesc)
{
	OnActorDescRemovedEvent.Broadcast(ActorDesc);

	ActorDesc->OnUnregister();
}
#endif
