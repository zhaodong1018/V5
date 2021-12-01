// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusNode_GetVariable.h"

#include "OptimusDeveloperModule.h"
#include "OptimusNodePin.h"
#include "OptimusVariableDescription.h"

void UOptimusNode_GetVariable::SetVariableDescription(UOptimusVariableDescription* InVariableDesc)
{
	if (!ensure(InVariableDesc))
	{
		return;
	}

	if (!EnumHasAnyFlags(InVariableDesc->DataType->UsageFlags, EOptimusDataTypeUsageFlags::Variable))
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Data type '%s' is not usable in a resource"),
		    *InVariableDesc->DataType->TypeName.ToString());
		return;
	}

	VariableDesc = InVariableDesc;
}


UOptimusVariableDescription* UOptimusNode_GetVariable::GetVariableDescription() const
{
	return VariableDesc.Get();
}


void UOptimusNode_GetVariable::ConstructNode()
{
	UOptimusVariableDescription *Var = VariableDesc.Get();
	if (Var)
	{
		AddPinDirect(
			Var->VariableName, 
			EOptimusNodePinDirection::Output,
			{},
			Var->DataType);
	}
}
