// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusNodeActions.h"

#include "IOptimusNodeGraphCollectionOwner.h"
#include "OptimusHelpers.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"


FOptimusNodeAction_RenameNode::FOptimusNodeAction_RenameNode(
	UOptimusNode* InNode, 
	FString InNewName
	)
{
	NodePath = InNode->GetNodePath();
	NewName = FText::FromString(InNewName);
	OldName = InNode->GetDisplayName();

	SetTitlef(TEXT("Rename %s"), *InNode->GetDisplayName().ToString());
}


bool FOptimusNodeAction_RenameNode::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNode *Node = InRoot->ResolveNodePath(NodePath);
	if (!Node)
	{
		return false;
	}

	Node->SetDisplayName(NewName);

	return true;
}


bool FOptimusNodeAction_RenameNode::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNode* Node = InRoot->ResolveNodePath(NodePath);
	if (!Node)
	{
		return false;
	}

	Node->SetDisplayName(OldName);

	return true;
}


FOptimusNodeAction_MoveNode::FOptimusNodeAction_MoveNode(
	UOptimusNode* InNode,
	const FVector2D& InPosition
)
{
	NodePath = InNode->GetNodePath();
	NewPosition = InPosition;
	OldPosition = InNode->GetGraphPosition();
}

bool FOptimusNodeAction_MoveNode::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNode* Node = InRoot->ResolveNodePath(NodePath);
	if (!Node)
	{
		return false;
	}

	return Node->SetGraphPositionDirect(NewPosition);
}

bool FOptimusNodeAction_MoveNode::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNode* Node = InRoot->ResolveNodePath(NodePath);
	if (!Node)
	{
		return false;
	}

	return Node->SetGraphPositionDirect(OldPosition);
}


FOptimusNodeAction_SetPinValue::FOptimusNodeAction_SetPinValue(
	UOptimusNodePin* InPin, 
	const FString& InNewValue
	)
{
	if (ensure(InPin) && InPin->GetSubPins().IsEmpty())
	{
		PinPath = InPin->GetPinPath();
		OldValue = InPin->GetValueAsString();
		NewValue = InNewValue;

		SetTitlef(TEXT("Set Value %s"), *InPin->GetPinPath());
	}
}


bool FOptimusNodeAction_SetPinValue::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNodePin* Pin = InRoot->ResolvePinPath(PinPath);
	if (!Pin)
	{
		return false;
	}

	return Pin->SetValueFromStringDirect(NewValue);
}


bool FOptimusNodeAction_SetPinValue::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNodePin* Pin = InRoot->ResolvePinPath(PinPath);
	if (!Pin)
	{
		return false;
	}

	return Pin->SetValueFromStringDirect(OldValue);
}


FOptimusNodeAction_SetPinName::FOptimusNodeAction_SetPinName(
	UOptimusNodePin* InPin,
	FName InPinName
	)
{
	if (ensure(InPin))
	{
		PinPath = InPin->GetPinPath();
		NewPinName = InPinName;
		OldPinName = InPin->GetFName();
	}
}


bool FOptimusNodeAction_SetPinName::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinName(InRoot, NewPinName);
}


bool FOptimusNodeAction_SetPinName::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinName(InRoot, OldPinName);
}


bool FOptimusNodeAction_SetPinName::SetPinName(
	IOptimusNodeGraphCollectionOwner* InRoot, 
	FName InName
	) const
{
	UOptimusNodePin *Pin = InRoot->ResolvePinPath(PinPath);

	if (!Pin)
	{
		return false;
	}
	
	return Pin->GetNode()->SetPinNameDirect(Pin, InName);
}


FOptimusNodeAction_SetPinType::FOptimusNodeAction_SetPinType(
	UOptimusNodePin* InPin,
	FOptimusDataTypeRef InDataType
	)
{
	if (ensure(InPin))
	{
		PinPath = InPin->GetPinPath();
		NewDataTypeName = InDataType.TypeName;
		OldDataTypeName = InPin->GetDataType()->TypeName;
	}
}


bool FOptimusNodeAction_SetPinType::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinType(InRoot, NewDataTypeName);
}


bool FOptimusNodeAction_SetPinType::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinType(InRoot, OldDataTypeName);
}


bool FOptimusNodeAction_SetPinType::SetPinType(
	IOptimusNodeGraphCollectionOwner* InRoot,
	FName InDataType
	) const
{
	UOptimusNodePin *Pin = InRoot->ResolvePinPath(PinPath);

	if (!Pin)
	{
		return false;
	}

	FOptimusDataTypeRef DataType;
	DataType.TypeName = InDataType;

	return Pin->GetNode()->SetPinDataTypeDirect(Pin, DataType);
}


FOptimusNodeAction_SetPinDataDomain::FOptimusNodeAction_SetPinDataDomain(
	UOptimusNodePin* InPin,
	const TArray<FName>& InContextNames
	)
{
	if (ensure(InPin) && ensure(!InContextNames.IsEmpty()) && ensure(InPin->GetStorageType() == EOptimusNodePinStorageType::Resource))
	{
		PinPath = InPin->GetPinPath();
		NewContextNames = InContextNames;
		OldContextNames = InPin->GetDataDomainLevelNames();
	}
}


bool FOptimusNodeAction_SetPinDataDomain::Do(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinDataDomain(InRoot, NewContextNames);
}


bool FOptimusNodeAction_SetPinDataDomain::Undo(IOptimusNodeGraphCollectionOwner* InRoot)
{
	return SetPinDataDomain(InRoot, OldContextNames);
}


bool FOptimusNodeAction_SetPinDataDomain::SetPinDataDomain(
	IOptimusNodeGraphCollectionOwner* InRoot,
	const TArray<FName>& InContextNames
	) const
{
	UOptimusNodePin *Pin = InRoot->ResolvePinPath(PinPath);

	if (!Pin)
	{
		return false;
	}

	return Pin->GetNode()->SetPinDataDomainDirect(Pin, InContextNames);
}


FOptimusNodeAction_AddRemovePin::FOptimusNodeAction_AddRemovePin(
	UOptimusNode* InNode,
	FName InName,
	EOptimusNodePinDirection InDirection,
	FOptimusNodePinStorageConfig InStorageConfig,
	FOptimusDataTypeRef InDataType,
	UOptimusNodePin* InBeforePin
	)
{
	if (ensure(InNode) &&
		ensure(!InBeforePin || InBeforePin->GetNode() == InNode) &&
		ensure(!InBeforePin || InBeforePin->GetParentPin() == nullptr))
	{
		NodePath = InNode->GetNodePath();
		PinName = InName;
		Direction = InDirection;
		StorageConfig = InStorageConfig;
		DataType = InDataType.TypeName;

		// New pins are always created in a non-expanded state.
		bExpanded = false;

		if (InBeforePin)
		{
			BeforePinPath = InBeforePin->GetPinPath();
		}
	}
}


FOptimusNodeAction_AddRemovePin::FOptimusNodeAction_AddRemovePin(UOptimusNodePin* InPin)
{
	if (ensure(InPin))
	{
		NodePath = InPin->GetNode()->GetNodePath();
		PinPath = InPin->GetPinPath();
		PinName = InPin->GetFName();
		Direction = InPin->GetDirection();
		if (InPin->GetStorageType() == EOptimusNodePinStorageType::Resource)
		{
			StorageConfig = FOptimusNodePinStorageConfig(InPin->GetDataDomainLevelNames());
		}
		else
		{
			StorageConfig = FOptimusNodePinStorageConfig();
		}
		DataType = InPin->GetDataType()->TypeName;

		// Store the expansion info.
		bExpanded = InPin->GetIsExpanded();

		// Capture the before pin.
		const TArray<UOptimusNodePin*>& Pins = InPin->GetNode()->GetPins();
		const int32 PinIndex = Pins.IndexOfByKey(InPin);
		if (ensure(Pins.IsValidIndex(PinIndex)))
		{
			// If it's not the last, then grab the path of the next pin.
			if (PinIndex < (Pins.Num() - 1))
			{
				BeforePinPath = Pins[PinIndex + 1]->GetPinPath();
			}
		}
	}
}


bool FOptimusNodeAction_AddRemovePin::AddPin(IOptimusNodeGraphCollectionOwner* InRoot)
{
	UOptimusNode* Node = InRoot->ResolveNodePath(NodePath);
	if (!Node)
	{
		return false;
	}

	FOptimusDataTypeRef TypeRef;
	TypeRef.TypeName = DataType;

	UOptimusNodePin *BeforePin = nullptr;
	if (!BeforePinPath.IsEmpty())
	{
		BeforePin = InRoot->ResolvePinPath(BeforePinPath);
		if (!BeforePin)
		{
			return false;
		}
	}
	
	UOptimusNodePin *Pin = Node->AddPinDirect(PinName, Direction, StorageConfig, TypeRef, BeforePin);
	if (!Pin)
	{
		return false;
	}

	Pin->SetIsExpanded(bExpanded);

	PinName = Pin->GetFName();
	PinPath = Pin->GetPinPath();

	return true;
}


bool FOptimusNodeAction_AddRemovePin::RemovePin(IOptimusNodeGraphCollectionOwner* InRoot) const
{
	UOptimusNodePin *Pin = InRoot->ResolvePinPath(PinPath);
	if (!Pin)
	{
		return false;
	}
	UOptimusNode *Node = Pin->GetNode();

	return Node->RemovePinDirect(Pin);
}


UOptimusNodePin* FOptimusNodeAction_AddPin::GetPin(IOptimusNodeGraphCollectionOwner* InRoot) const
{
	return InRoot->ResolvePinPath(PinPath);
}
