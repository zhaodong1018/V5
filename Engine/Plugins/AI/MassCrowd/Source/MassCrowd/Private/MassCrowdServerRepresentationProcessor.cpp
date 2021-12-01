// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassCrowdServerRepresentationProcessor.h"
#include "MassCrowdFragments.h"
#include "MassActorSubsystem.h"

UMassCrowdServerRepresentationProcessor::UMassCrowdServerRepresentationProcessor()
{
	ExecutionFlags = (int32)EProcessorExecutionFlags::Server;

	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::LOD);

	LODRepresentation[EMassLOD::High] = ERepresentationType::HighResSpawnedActor;
	LODRepresentation[EMassLOD::Medium] = ERepresentationType::None;
	LODRepresentation[EMassLOD::Low] = ERepresentationType::None;
	LODRepresentation[EMassLOD::Off] = ERepresentationType::None;
}

void UMassCrowdServerRepresentationProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
	{
		UpdateRepresentation(Context);
	});

	// @todo, we should use the new translators to do that initialization
	InitializeVelocity(EntitySubsystem, Context);
}