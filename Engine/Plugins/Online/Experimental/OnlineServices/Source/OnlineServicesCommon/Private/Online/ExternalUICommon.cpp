// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/ExternalUICommon.h"

#include "Online/OnlineAsyncOp.h"
#include "Online/OnlineErrorDefinitions.h"
#include "Online/OnlineServicesCommon.h"

namespace UE::Online {

FExternalUICommon::FExternalUICommon(FOnlineServicesCommon& InServices)
	: TOnlineComponent(TEXT("ExternalUI"), InServices)
{
}

void FExternalUICommon::RegisterCommands()
{
	RegisterCommand(&FExternalUICommon::ShowFriendsUI);
}

TOnlineAsyncOpHandle<FExternalUIShowFriendsUI> FExternalUICommon::ShowFriendsUI(FExternalUIShowFriendsUI::Params&& Params)
{
	TOnlineAsyncOpRef<FExternalUIShowFriendsUI> Operation = GetOp<FExternalUIShowFriendsUI>(MoveTemp(Params));
	Operation->SetError(Errors::NotImplemented());
	return Operation->GetHandle();
}

/* UE::Online */ }
