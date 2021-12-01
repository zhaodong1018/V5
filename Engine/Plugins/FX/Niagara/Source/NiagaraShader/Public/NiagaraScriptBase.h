// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIDefinitions.h"

#include "NiagaraScriptBase.generated.h"

UENUM()
enum class ENiagaraSimStageExecuteBehavior : uint8
{
	/** The stage will run every frame. */
	Always,
	/** The stage will only run on the frame when the simulation is reset. */
	OnSimulationReset,
	/** The stage will not run on the frame where the simulation is reset. */
	NotOnSimulationReset,
};

USTRUCT()
struct NIAGARASHADER_API FSimulationStageMetaData
{
	GENERATED_USTRUCT_BODY()
public:
	FSimulationStageMetaData();

	/** User simulation stage name. */
	UPROPERTY()
	FName SimulationStageName;

	UPROPERTY()
	FName EnabledBinding;

	/** The Data Interface that we iterate over for this stage. If None, then use particles.*/
	UPROPERTY()
	FName IterationSource;

	/** Controls when the simulation stage will execute. */
	UPROPERTY()
	ENiagaraSimStageExecuteBehavior ExecuteBehavior = ENiagaraSimStageExecuteBehavior::Always;

	/** Do we write to particles this stage? */
	UPROPERTY()
	uint32 bWritesParticles : 1;

	/** When enabled the simulation stage does not write all variables out, so we are reading / writing to the same buffer. */
	UPROPERTY()
	uint32 bPartialParticleUpdate : 1;

	/** DataInterfaces that we write to in this stage.*/
	UPROPERTY()
	TArray<FName> OutputDestinations;

	/** The number of iterations for the stage. */
	UPROPERTY()
	int32 NumIterations = 1;

	/** Optional binding to gather num iterations from. */
	UPROPERTY()
	FName NumIterationsBinding;

	inline bool ShouldRunStage(bool bResetData) const
	{
		const bool bAlways = ExecuteBehavior == ENiagaraSimStageExecuteBehavior::Always;
		const bool bResetOnly = ExecuteBehavior == ENiagaraSimStageExecuteBehavior::OnSimulationReset;
		return bAlways || (bResetOnly == bResetData);
	}
};

UCLASS(MinimalAPI, abstract)
class UNiagaraScriptBase : public UObject
{
	GENERATED_UCLASS_BODY()

	virtual void ModifyCompilationEnvironment(EShaderPlatform Platform, struct FShaderCompilerEnvironment& OutEnvironment) const PURE_VIRTUAL(UNiagaraScriptBase::ModifyCompilationEnvironment, );
	virtual TConstArrayView<FSimulationStageMetaData> GetSimulationStageMetaData() const PURE_VIRTUAL(UNiagaraScriptBase::GetSimulationStageMetaData(), return MakeArrayView<FSimulationStageMetaData>(nullptr, 0); )
};
