// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TraceServices/Model/Memory.h"

// Insights
#include "Insights/MemoryProfiler/ViewModels/MemAllocTable.h"
#include "Insights/MemoryProfiler/ViewModels/MemoryAlloc.h"
#include "Insights/MemoryProfiler/ViewModels/MemorySharedState.h"
#include "Insights/Table/ViewModels/TableTreeNode.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Insights
{

enum class EMemAllocNodeType
{
	/** The MemAllocNode is an allocation node. */
	MemAlloc,

	/** The MemAllocNode is a group node. */
	Group,

	/** Invalid enum type, may be used as a number of enumerations. */
	InvalidOrMax,
};

////////////////////////////////////////////////////////////////////////////////////////////////////

class FMemAllocNode;

/** Type definition for shared pointers to instances of FMemAllocNode. */
typedef TSharedPtr<class FMemAllocNode> FMemAllocNodePtr;

/** Type definition for shared references to instances of FMemAllocNode. */
typedef TSharedRef<class FMemAllocNode> FMemAllocNodeRef;

/** Type definition for shared references to const instances of FMemAllocNode. */
typedef TSharedRef<const class FMemAllocNode> FMemAllocNodeRefConst;

/** Type definition for weak references to instances of FMemAllocNode. */
typedef TWeakPtr<class FMemAllocNode> FMemAllocNodeWeak;

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Class used to store information about an allocation node (used in the SMemAllocTreeView).
 */
class FMemAllocNode : public FTableTreeNode
{
public:
	static const FName TypeName;

public:
	/** Initialization constructor for the MemAlloc node. */
	explicit FMemAllocNode(const FName InName, TWeakPtr<FMemAllocTable> InParentTable, int32 InRowIndex)
		: FTableTreeNode(InName, InParentTable, InRowIndex)
		, Type(EMemAllocNodeType::MemAlloc)
	{
	}

	/** Initialization constructor for the group node. */
	explicit FMemAllocNode(const FName InGroupName, TWeakPtr<FMemAllocTable> InParentTable)
		: FTableTreeNode(InGroupName, InParentTable)
		, Type(EMemAllocNodeType::Group)
	{
	}

	virtual const FName& GetTypeName() const override { return TypeName; }

	/**
	 * @return a type of this MemAlloc node or EMemAllocNodeType::Group for group nodes.
	 */
	EMemAllocNodeType GetType() const { return Type; }

	FMemAllocTable& GetMemTableChecked() const
	{
		const TSharedPtr<FTable>& TablePin = GetParentTable().Pin();
		check(TablePin.IsValid());
		return *StaticCastSharedPtr<FMemAllocTable>(TablePin);
	}

	bool IsValidMemAlloc() const { return GetMemTableChecked().IsValidRowIndex(GetRowIndex()); }
	const FMemoryAlloc* GetMemAlloc() const { return GetMemTableChecked().GetMemAlloc(GetRowIndex()); }
	const FMemoryAlloc& GetMemAllocChecked() const { return GetMemTableChecked().GetMemAllocChecked(GetRowIndex()); }
	FText GetFullCallstack() const;

private:
	const EMemAllocNodeType Type;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace Insights
