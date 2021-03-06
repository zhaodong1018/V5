// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassSimpleMovementTrait.h"
#include "MassEntityTemplateRegistry.h"
#include "Engine/World.h"
#include "MassMovementTypes.h"
#include "MassMovementFragments.h"
#include "MassCommonFragments.h"
#include "MassSimulationLOD.h"


//----------------------------------------------------------------------//
//  UMassSimpleMovementTrait
//----------------------------------------------------------------------//
void UMassSimpleMovementTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FDataFragment_Transform>();
	BuildContext.AddFragment<FMassVelocityFragment>();
	BuildContext.AddTag<FMassSimpleMovementTag>();	
}

//----------------------------------------------------------------------//
//  UMassSimpleMovementProcessor
//----------------------------------------------------------------------//
UMassSimpleMovementProcessor::UMassSimpleMovementProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ExecutionFlags = (int32)EProcessorExecutionFlags::All;
	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Avoidance;
}

void UMassSimpleMovementProcessor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FMassVelocityFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FDataFragment_Transform>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddTagRequirement<FMassSimpleMovementTag>(EMassFragmentPresence::All);

	EntityQuery.AddRequirement<FMassSimulationLODFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	EntityQuery.AddChunkRequirement<FMassSimulationVariableTickChunkFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	EntityQuery.SetChunkFilter(&FMassSimulationVariableTickChunkFragment::ShouldTickChunkThisFrame);
}

void UMassSimpleMovementProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, ([this](FMassExecutionContext& Context)
		{
			const TConstArrayView<FMassVelocityFragment> VelocitiesList = Context.GetFragmentView<FMassVelocityFragment>();
			const TArrayView<FDataFragment_Transform> TransformsList = Context.GetMutableFragmentView<FDataFragment_Transform>();
			const TConstArrayView<FMassSimulationLODFragment> SimLODList = Context.GetFragmentView<FMassSimulationLODFragment>();
			const bool bHasLOD = (SimLODList.Num() > 0);
			const float WorldDeltaTime = Context.GetDeltaTimeSeconds();
		
			for (int32 EntityIndex = 0; EntityIndex < Context.GetNumEntities(); ++EntityIndex)
			{
				const FMassVelocityFragment& Velocity = VelocitiesList[EntityIndex];
				FTransform& Transform = TransformsList[EntityIndex].GetMutableTransform();
				const float DeltaTime = bHasLOD ? SimLODList[EntityIndex].DeltaTime : WorldDeltaTime; 
				Transform.SetTranslation(Transform.GetTranslation() + Velocity.Value * DeltaTime);
			}
		}));
}
