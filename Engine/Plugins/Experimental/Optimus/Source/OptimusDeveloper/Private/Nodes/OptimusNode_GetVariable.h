// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "OptimusNode.h"

#include "OptimusNode_GetVariable.generated.h"


class UOptimusVariableDescription;


UCLASS(Hidden)
class UOptimusNode_GetVariable : 
	public UOptimusNode
{
	GENERATED_BODY()

public:
	void SetVariableDescription(UOptimusVariableDescription* InVariableDesc);

	UOptimusVariableDescription* GetVariableDescription() const;
	
	// UOptimusNode overrides
	FName GetNodeCategory() const override 
	{
		return CategoryName::Variables;
	}

protected:
	void ConstructNode() override;

private:
	UPROPERTY()
	TWeakObjectPtr<UOptimusVariableDescription> VariableDesc;
};
