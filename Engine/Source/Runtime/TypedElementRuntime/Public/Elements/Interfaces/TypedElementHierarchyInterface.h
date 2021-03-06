// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Elements/Framework/TypedElementHandle.h"
#include "UObject/Interface.h"

#include "TypedElementHierarchyInterface.generated.h"

UINTERFACE(MinimalAPI, BlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class UTypedElementHierarchyInterface : public UInterface
{
	GENERATED_BODY()
};

class TYPEDELEMENTRUNTIME_API ITypedElementHierarchyInterface
{
	GENERATED_BODY()

public:
	/**
	 * Get the logical parent of this element, if any.
	 * eg) A component might return its actor, or a static mesh instance might return its ISM component.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|Hierarchy")
	virtual FTypedElementHandle GetParentElement(const FTypedElementHandle& InElementHandle, const bool bAllowCreate = true) { return FTypedElementHandle(); }

	/**
	 * Get the logical children of this element, if any.
	 * eg) An actor might return its component, or an ISM component might return its static mesh instances.
	 *
	 * @note Appends to OutElementHandles.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|Hierarchy")
	virtual void GetChildElements(const FTypedElementHandle& InElementHandle, TArray<FTypedElementHandle>& OutElementHandles, const bool bAllowCreate = true) {}
};

template <>
struct TTypedElement<ITypedElementHierarchyInterface> : public TTypedElementBase<ITypedElementHierarchyInterface>
{
	FTypedElementHandle GetParentElement(const bool bAllowCreate = true) const { return InterfacePtr->GetParentElement(*this, bAllowCreate); }
	void GetChildElements(TArray<FTypedElementHandle>& OutElementHandles, const bool bAllowCreate = true) const { return InterfacePtr->GetChildElements(*this, OutElementHandles, bAllowCreate); }
};
