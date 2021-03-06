// Copyright Epic Games, Inc. All Rights Reserved.

#include "Elements/Component/ComponentElementEditorWorldInterface.h"
#include "Elements/Component/ComponentElementData.h"
#include "Components/ActorComponent.h"

#include "Elements/Framework/EngineElementsLibrary.h"

#include "Editor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Kismet2/ComponentEditorUtils.h"

void UComponentElementEditorWorldInterface::NotifyMovementStarted(const FTypedElementHandle& InElementHandle)
{
	if (UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(InElementHandle))
	{
		GEditor->BroadcastBeginObjectMovement(*Component);
	}
}

void UComponentElementEditorWorldInterface::NotifyMovementOngoing(const FTypedElementHandle& InElementHandle)
{
	if (UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(InElementHandle))
	{
		if (AActor* Actor = Component->GetOwner())
		{
			Actor->PostEditMove(false);
		}
	}
}

void UComponentElementEditorWorldInterface::NotifyMovementEnded(const FTypedElementHandle& InElementHandle)
{
	if (UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(InElementHandle))
	{
		GEditor->BroadcastEndObjectMovement(*Component);
		if (AActor* Actor = Component->GetOwner())
		{
			Actor->PostEditMove(true);
			Actor->InvalidateLightingCache();
		}

		Component->MarkPackageDirty();
	}
}

bool UComponentElementEditorWorldInterface::CanDeleteElement(const FTypedElementHandle& InElementHandle)
{
	UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(InElementHandle);
	return Component && GUnrealEd->CanDeleteComponent(Component);
}

bool UComponentElementEditorWorldInterface::DeleteElements(TArrayView<const FTypedElementHandle> InElementHandles, UWorld* InWorld, UTypedElementSelectionSet* InSelectionSet, const FTypedElementDeletionOptions& InDeletionOptions)
{
	const TArray<UActorComponent*> ComponentsToDelete = ComponentElementDataUtil::GetComponentsFromHandles(InElementHandles);
	return ComponentsToDelete.Num() > 0
		&& GUnrealEd->DeleteComponents(ComponentsToDelete, InSelectionSet, InDeletionOptions.VerifyDeletionCanHappen());
}

bool UComponentElementEditorWorldInterface::CanDuplicateElement(const FTypedElementHandle& InElementHandle)
{
	UActorComponent* Component = ComponentElementDataUtil::GetComponentFromHandle(InElementHandle);
	return Component && FComponentEditorUtils::CanCopyComponent(Component); // If we can copy, we can duplicate
}

void UComponentElementEditorWorldInterface::DuplicateElements(TArrayView<const FTypedElementHandle> InElementHandles, UWorld* InWorld, const FVector& InLocationOffset, TArray<FTypedElementHandle>& OutNewElements)
{
	const TArray<UActorComponent*> ComponentsToDuplicate = ComponentElementDataUtil::GetComponentsFromHandles(InElementHandles);

	if (ComponentsToDuplicate.Num() > 0)
	{
		TArray<UActorComponent*> NewComponents;
		GUnrealEd->DuplicateComponents(ComponentsToDuplicate, NewComponents);

		OutNewElements.Reserve(OutNewElements.Num() + NewComponents.Num());
		for (UActorComponent* NewComponent : NewComponents)
		{
			OutNewElements.Add(UEngineElementsLibrary::AcquireEditorComponentElementHandle(NewComponent));
		}
	}
}
