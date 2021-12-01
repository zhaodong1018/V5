// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once 

#include "MassEntityTraitBase.h"
#include "MassSimulationLOD.h"

#include "MassLODTrait.generated.h"

UCLASS(meta = (DisplayName = "SimulationLOD"))
class MASSLOD_API UMassSimulationLODTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

	UPROPERTY(Category = "Config", EditAnywhere)
	FMassSimulationLODConfig Config;

protected:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const override;
};
