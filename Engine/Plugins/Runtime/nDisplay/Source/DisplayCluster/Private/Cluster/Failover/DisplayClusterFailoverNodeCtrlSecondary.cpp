// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cluster/Failover/DisplayClusterFailoverNodeCtrlSecondary.h"

#include "Misc/DisplayClusterAppExit.h"
#include "Misc/DisplayClusterLog.h"
#include "Misc/DisplayClusterTypesConverterPrivate.h"

#include "Network/DisplayClusterNetworkTypes.h"


////////////////////////////////////////////////////////////////////////////////////////////////////
// EDisplayClusterFailoverPolicy::Disabled
////////////////////////////////////////////////////////////////////////////////////////////////////
void FDisplayClusterFailoverNodeCtrlSecondary::HandleCommResult_Disabled(EDisplayClusterCommResult CommResult)
{
	// Any comm error on the primary node results in node termination
	if (CommResult != EDisplayClusterCommResult::Ok)
	{
		FDisplayClusterAppExit::ExitApplication(FString::Printf(TEXT("Failover: requesting application exit. CommError=%u (%s)"), static_cast<uint32>(CommResult), *DisplayClusterTypesConverter::ToString(CommResult)));
	}
}

void FDisplayClusterFailoverNodeCtrlSecondary::HandleNodeFailed_Disabled(const FString& NodeId, IDisplayClusterServer::ENodeFailType NodeFailType)
{
	// Currently, all comm sessions run on the primary node only. We don't have to do anything over here.
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// EDisplayClusterFailoverPolicy::Failover_v1_DropSecondaryNodesOnly
////////////////////////////////////////////////////////////////////////////////////////////////////
void FDisplayClusterFailoverNodeCtrlSecondary::HandleCommResult_Failover_v1(EDisplayClusterCommResult CommResult)
{
	// Any comm error on the primary node results in node termination
	if (CommResult != EDisplayClusterCommResult::Ok)
	{
		FDisplayClusterAppExit::ExitApplication(FString::Printf(TEXT("Failover: requesting application exit. CommError=%u (%s)"), static_cast<uint32>(CommResult), *DisplayClusterTypesConverter::ToString(CommResult)));
	}
}

void FDisplayClusterFailoverNodeCtrlSecondary::HandleNodeFailed_Failover_v1(const FString& NodeId, IDisplayClusterServer::ENodeFailType NodeFailType)
{
	// Currently, all comm sessions run on the primary node only. We don't have to do anything over here.
}
