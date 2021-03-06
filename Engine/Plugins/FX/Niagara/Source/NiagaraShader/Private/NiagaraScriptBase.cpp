// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraScriptBase.h"

UNiagaraScriptBase::UNiagaraScriptBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FSimulationStageMetaData::FSimulationStageMetaData()
	: bWritesParticles(0)
	, bPartialParticleUpdate(0)
{
}
