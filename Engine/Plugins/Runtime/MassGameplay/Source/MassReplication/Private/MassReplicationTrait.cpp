// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassReplicationTrait.h"
#include "MassEntityTemplateRegistry.h"
#include "Engine/World.h"
#include "MassSpawnerTypes.h"
#include "MassSimulationLOD.h"
#include "MassReplicationTypes.h"


void UMassReplicationTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	if (World.IsNetMode(NM_Standalone))
	{
		return;
	}

	FDataFragment_ReplicationTemplateID& TemplateIDFragment = BuildContext.AddFragmentWithDefaultInitializer_GetRef<FDataFragment_ReplicationTemplateID>();
	TemplateIDFragment.ID = BuildContext.GetTemplateID();

	BuildContext.AddFragmentWithDefaultInitializer<FMassNetworkIDFragment>();
	BuildContext.AddFragment<FMassReplicatedAgentFragment>();
	BuildContext.AddFragment<FMassReplicationViewerLODFragment>();
	BuildContext.AddFragment<FDataFragment_MassSimulationLODInfo>();
}