// Copyright Epic Games, Inc. All Rights Reserved.

#include "ComputeFramework/ComputeGraphComponent.h"

#include "ComputeFramework/ComputeDataInterface.h"
#include "ComputeFramework/ComputeDataProvider.h"
#include "ComputeFramework/ComputeFrameworkModule.h"
#include "ComputeFramework/ComputeGraph.h"
#include "ComputeFramework/ComputeGraphWorker.h"
#include "ComputeFramework/ComputeSystem.h"

UComputeGraphComponent::UComputeGraphComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// By default don't tick and allow any queuing of work to be handled by blueprint.
	// Ticking can be turned on by some systems that need it (such as editor window).
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UComputeGraphComponent::CreateDataProviders(bool bSetDefaultBindings)
{
	DataProviders.Reset();
	if (ComputeGraph != nullptr)
	{
		ComputeGraph->CreateDataProviders(this, bSetDefaultBindings, DataProviders);
	}

	// We only want to queue work after validating the new providers.
	bValidProviders = false;
}

void UComputeGraphComponent::QueueExecute()
{
	if (ComputeGraph == nullptr)
	{
		return;
	}

	if (GetScene() == nullptr || FComputeFrameworkModule::GetComputeSystem() == nullptr || FComputeFrameworkModule::GetComputeSystem()->GetComputeWorker(GetScene()) == nullptr)
	{
		return;
	}

	// Don't submit work if we don't have all of the expected bindings.
	bValidProviders = bValidProviders || ComputeGraph->ValidateProviders(DataProviders);
	if (!bValidProviders)
	{
		// todo[CF]: We should have a default fallback for all cases where we can't submit work.
		return;
	}

	MarkRenderDynamicDataDirty();
}

void UComputeGraphComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	QueueExecute();
}

void UComputeGraphComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
	
	// If we hit the ensure then something invalidated providers without calling CreateDataProviders().
	// Those paths DO need fixing. We can remove the ensure() if we ever feel safe enough!
	if (!bValidProviders || !ensure(ComputeGraph->ValidateProviders(DataProviders)))
	{
		// Probably we marked for update just before invalidating providers.
		return;
	}

	// Lookup the compute worker associated with this scene.
	FComputeGraphTaskWorker* ComputeGraphWorker = FComputeFrameworkModule::GetComputeSystem()->GetComputeWorker(GetScene());
	if (!ensure(ComputeGraphWorker))
	{
		return;
	}

	TArray<FComputeDataProviderRenderProxy*> ComputeDataProviderProxies;
	for (UComputeDataProvider* DataProvider : DataProviders)
	{
		// Be sure to add null provider slots because we want to maintain consistent array indices.
		// Note that we expect GetRenderProxy() to return a pointer that we can own and call delete on.
		FComputeDataProviderRenderProxy* ProviderProxy = DataProvider != nullptr ? DataProvider->GetRenderProxy() : nullptr;
		ComputeDataProviderProxies.Add(ProviderProxy);
	}

	FComputeGraphProxy* ComputeGraphProxy = new FComputeGraphProxy();
	ComputeGraphProxy->Initialize(ComputeGraph);

	ENQUEUE_RENDER_COMMAND(ComputeFrameworkEnqueueExecutionCommand)(
		[ComputeGraphWorker, ComputeGraphProxy, DataProviderProxies = MoveTemp(ComputeDataProviderProxies)](FRHICommandListImmediate& RHICmdList)
		{
			// Compute graph scheduler will take ownership of the provider proxies.
			ComputeGraphWorker->Enqueue(ComputeGraphProxy, DataProviderProxies);
			delete ComputeGraphProxy;
		});
}
