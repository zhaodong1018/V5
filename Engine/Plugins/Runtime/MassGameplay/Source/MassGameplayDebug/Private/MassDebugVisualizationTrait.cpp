// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassDebugVisualizationTrait.h"
#include "MassDebuggerSubsystem.h"
#include "MassDebugVisualizationComponent.h"
#include "MassEntityTemplateRegistry.h"
#include "MassCommonFragments.h"
#include "Engine/World.h"

void UMassDebugVisualizationTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
#if WITH_EDITORONLY_DATA
	const UStaticMesh* const DebugMesh = DebugShape.Mesh;
#else
	const UStaticMesh* const DebugMesh = nullptr;
#endif
	
	if (DebugMesh)
	{
#if WITH_EDITORONLY_DATA
		FSimDebugVisComponent& DebugVisFragment = BuildContext.AddFragmentWithDefaultInitializer_GetRef<FSimDebugVisComponent>();
		UMassDebuggerSubsystem* Debugger = World.GetSubsystem<UMassDebuggerSubsystem>();
		if (ensure(Debugger))
		{
			UMassDebugVisualizationComponent* DebugVisComponent = Debugger->GetVisualizationComponent();
			if (ensure(DebugVisComponent))
			{
				DebugVisFragment.VisualType = DebugVisComponent->AddDebugVisType(DebugShape);
			}
			// @todo this path requires a fragment destructor that will remove the mesh from the debugger.
		}

		BuildContext.AddDefaultInitializer<FSimDebugVisComponent>();
#endif // WITH_EDITORONLY_DATA
	}
	// add fragments needed whenever we have debugging capabilities
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	BuildContext.AddTag<FMassDebuggableTag>();
#if WITH_EDITORONLY_DATA
	BuildContext.AddFragmentWithDefaultInitializer_GetRef<FDataFragment_DebugVis>().Shape = DebugShape.WireShape;
#else
	// DebugShape unavailable, will used default instead
	BuildContext.AddFragmentWithDefaultInitializer<FDataFragment_DebugVis>();
#endif // WITH_EDITORONLY_DATA
	BuildContext.AddFragmentWithDefaultInitializer<FDataFragment_AgentRadius>();

	BuildContext.AddFragmentWithDefaultInitializer<FDataFragment_Transform>();
#endif // if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

}