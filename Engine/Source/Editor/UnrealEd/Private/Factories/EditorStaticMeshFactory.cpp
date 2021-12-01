// Copyright Epic Games, Inc. All Rights Reserved.

#include "Factories/EditorStaticMeshFactory.h"

#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Elements/Interfaces/TypedElementAssetDataInterface.h"
#include "Elements/Interfaces/TypedElementObjectInterface.h"
#include "Instances/InstancedPlacementPartitionActor.h"

#include "ActorPartition/ActorPartitionSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "UObject/Class.h"

#include "Subsystems/PlacementSubsystem.h"

void UEditorStaticMeshFactoryPlacementSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	StaticMeshComponentDescriptor.ComputeHash();
}

TArray<FTypedElementHandle> UEditorStaticMeshFactory::PlaceAsset(const FAssetPlacementInfo& InPlacementInfo, const FPlacementOptions& InPlacementOptions)
{
	// If we're disallowing instanced placement, or creating preview elements, don't use the ISM placement.
	if (!ShouldPlaceInstancedStaticMeshes(InPlacementOptions))
	{
		return Super::PlaceAsset(InPlacementInfo, InPlacementOptions);
	}

	TArray<FTypedElementHandle> PlacedInstanceHandles;
	if (InPlacementInfo.PreferredLevel.IsValid())
	{
		if (!InPlacementInfo.SettingsObject)
		{
			return TArray<FTypedElementHandle>();
		}

		FISMComponentDescriptor ComponentDescriptor = CastChecked<UEditorStaticMeshFactoryPlacementSettings>(InPlacementInfo.SettingsObject)->StaticMeshComponentDescriptor;
		if (!ComponentDescriptor.StaticMesh)
		{
			return TArray<FTypedElementHandle>();
		}

		// Make sure the component descriptor's hash matches its current settings before we place.
		ComponentDescriptor.ComputeHash();

		if (UActorPartitionSubsystem* PartitionSubsystem = UWorld::GetSubsystem<UActorPartitionSubsystem>(InPlacementInfo.PreferredLevel->GetWorld()))
		{
			// Create or find the placement partition actor
			auto OnActorCreated = [InPlacementOptions](APartitionActor* CreatedPartitionActor)
			{
				if (AInstancedPlacementPartitionActor* ElementPartitionActor = Cast<AInstancedPlacementPartitionActor>(CreatedPartitionActor))
				{
					ElementPartitionActor->SetGridGuid(InPlacementOptions.InstancedPlacementGridGuid);
				}
			};

			// Make a good known client GUID out of the placed asset's package if one was not given to us.
			FGuid ItemGuidToUse = InPlacementInfo.ItemGuid;
			if (!ItemGuidToUse.IsValid())
			{
				ItemGuidToUse = InPlacementInfo.AssetToPlace.GetAsset()->GetPackage()->GetPersistentGuid();
			}

			constexpr bool bCreatePartitionActorIfMissing = true;
			FActorPartitionGetParams PartitionActorFindParams(
				AInstancedPlacementPartitionActor::StaticClass(),
				bCreatePartitionActorIfMissing, InPlacementInfo.PreferredLevel.Get(),
				InPlacementInfo.FinalizedTransform.GetLocation(),
				0,
				InPlacementOptions.InstancedPlacementGridGuid,
				true,
				OnActorCreated
			);
			AInstancedPlacementPartitionActor* PlacedElementsActor = Cast<AInstancedPlacementPartitionActor>(PartitionSubsystem->GetActor(PartitionActorFindParams));

			FISMClientHandle ClientHandle = PlacedElementsActor->RegisterClient(ItemGuidToUse);
			TSortedMap<int32, TArray<FTransform>> InstanceMap;
			InstanceMap.Emplace(PlacedElementsActor->RegisterISMComponentDescriptor(ComponentDescriptor), TArray({ FTransform() }));
			ModifiedPartitionActors.Add(PlacedElementsActor);
			PlacedElementsActor->BeginUpdate();

			TArray<FSMInstanceId> PlacedInstances = PlacedElementsActor->AddISMInstance(ClientHandle, InPlacementInfo.FinalizedTransform, InstanceMap);
			for (const FSMInstanceId PlacedInstanceID : PlacedInstances)
			{
				if (FTypedElementHandle PlacedHandle = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(PlacedInstanceID))
				{
					PlacedInstanceHandles.Emplace(PlacedHandle);
				}
			}
		}
	}

	return PlacedInstanceHandles;
}

FAssetData UEditorStaticMeshFactory::GetAssetDataFromElementHandle(const FTypedElementHandle& InHandle)
{
	FAssetData FoundAssetData;
	if (TTypedElement<ITypedElementAssetDataInterface> AssetDataInterface = UTypedElementRegistry::GetInstance()->GetElement<ITypedElementAssetDataInterface>(InHandle))
	{
		FoundAssetData = AssetDataInterface.GetAssetData();
	}

	if (!FoundAssetData.IsValid())
	{
		UInstancedStaticMeshComponent* ISMComponent = nullptr;
		if (TTypedElement<ITypedElementObjectInterface> ObjectInterface = UTypedElementRegistry::GetInstance()->GetElement<ITypedElementObjectInterface>(InHandle))
		{
			// Try to pull from component handle
			if (UInstancedStaticMeshComponent* RawComponentPtr = ObjectInterface.GetObjectAs<UInstancedStaticMeshComponent>())
			{
				ISMComponent = RawComponentPtr;
			}
			else if (AActor* RawActorPtr = ObjectInterface.GetObjectAs<AActor>())
			{
				ISMComponent = RawActorPtr->FindComponentByClass<UInstancedStaticMeshComponent>();
			}
		}

		if (ISMComponent)
		{
			FoundAssetData = FAssetData(ISMComponent->GetStaticMesh());
		}
	}

	if (CanPlaceElementsFromAssetData(FoundAssetData))
	{
		return FoundAssetData;
	}

	return Super::GetAssetDataFromElementHandle(InHandle);
}

UEditorFactorySettingsObject* UEditorStaticMeshFactory::FactorySettingsObjectForPlacement(const FAssetData& InAssetData, const FPlacementOptions& InPlacementOptions)
{
	if (!ShouldPlaceInstancedStaticMeshes(InPlacementOptions))
	{
		return Super::FactorySettingsObjectForPlacement(InAssetData, InPlacementOptions);
	}

	UEditorStaticMeshFactoryPlacementSettings* PlacementSettingsObject = NewObject<UEditorStaticMeshFactoryPlacementSettings>(this);
	if (PlacementSettingsObject)
	{
		UObject* AssetToPlaceAsObject = InAssetData.GetAsset();
		FISMComponentDescriptor& ComponentDescriptor = PlacementSettingsObject->StaticMeshComponentDescriptor;
		if (UStaticMesh* StaticMeshObject = Cast<UStaticMesh>(AssetToPlaceAsObject))
		{
			// If this is a Nanite mesh, prefer to use ISM over HISM, as HISM duplicates many features/bookkeeping that Nanite already handles for us.
			if (StaticMeshObject->HasValidNaniteData())
			{
				ComponentDescriptor.InitFrom(UInstancedStaticMeshComponent::StaticClass()->GetDefaultObject<UInstancedStaticMeshComponent>());
			}
			ComponentDescriptor.StaticMesh = StaticMeshObject;
		}
		else if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(AssetToPlaceAsObject))
		{
			if (UStaticMeshComponent* StaticMeshComponent = StaticMeshActor->GetStaticMeshComponent())
			{
				ComponentDescriptor.StaticMesh = StaticMeshComponent->GetStaticMesh();
			}
		}

		// Go ahead and compute the descriptor now, in case we do not go through a place cycle or edit any properties
		ComponentDescriptor.ComputeHash();
	}

	return PlacementSettingsObject;
}

bool UEditorStaticMeshFactory::ShouldPlaceInstancedStaticMeshes(const FPlacementOptions& InPlacementOptions) const
{
	return !InPlacementOptions.bIsCreatingPreviewElements && InPlacementOptions.InstancedPlacementGridGuid.IsValid();
}

void UEditorStaticMeshFactory::EndPlacement(TArrayView<const FTypedElementHandle> InPlacedElements, const FPlacementOptions& InPlacementOptions)
{
	for (TWeakObjectPtr<AISMPartitionActor> ISMPartitionActor : ModifiedPartitionActors)
	{
		if (ISMPartitionActor.IsValid())
		{
			ISMPartitionActor->EndUpdate();
		}
	}

	ModifiedPartitionActors.Empty();
}
