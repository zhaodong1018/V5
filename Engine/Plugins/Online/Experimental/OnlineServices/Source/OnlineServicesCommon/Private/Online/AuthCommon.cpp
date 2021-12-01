// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/AuthCommon.h"

#include "Online/OnlineAsyncOp.h"
#include "Online/OnlineErrorDefinitions.h"
#include "Online/OnlineServicesCommon.h"

namespace UE::Online {

FAuthCommon::FAuthCommon(FOnlineServicesCommon& InServices)
	: TOnlineComponent(TEXT("Auth"), InServices)
{
}

void FAuthCommon::RegisterCommands()
{
	RegisterCommand(&FAuthCommon::Login);
	RegisterCommand(&FAuthCommon::Logout);
	RegisterCommand(&FAuthCommon::GenerateAuth);
	RegisterCommand(&FAuthCommon::GetAccountByLocalUserNum);
	RegisterCommand(&FAuthCommon::GetAccountByAccountId);
}

TOnlineAsyncOpHandle<FAuthLogin> FAuthCommon::Login(FAuthLogin::Params&& Params)
{
	TOnlineAsyncOpRef<FAuthLogin> Operation = GetOp<FAuthLogin>(MoveTemp(Params));
	Operation->SetError(Errors::NotImplemented());  
	return Operation->GetHandle();
}

TOnlineAsyncOpHandle<FAuthLogout> FAuthCommon::Logout(FAuthLogout::Params&& Params)
{
	TOnlineAsyncOpRef<FAuthLogout> Operation = GetOp<FAuthLogout>(MoveTemp(Params));
	Operation->SetError(Errors::NotImplemented());
	return Operation->GetHandle();
}

TOnlineAsyncOpHandle<FAuthGenerateAuth> FAuthCommon::GenerateAuth(FAuthGenerateAuth::Params&& Params)
{
	TOnlineAsyncOpRef<FAuthGenerateAuth> Operation = GetOp<FAuthGenerateAuth>(MoveTemp(Params));
	Operation->SetError(Errors::NotImplemented());
	return Operation->GetHandle();
}

TOnlineResult<FAuthGetAccountByLocalUserNum> FAuthCommon::GetAccountByLocalUserNum(FAuthGetAccountByLocalUserNum::Params&& Params)
{
	return TOnlineResult<FAuthGetAccountByLocalUserNum>(Errors::NotImplemented());
}

TOnlineResult<FAuthGetAccountByAccountId> FAuthCommon::GetAccountByAccountId(FAuthGetAccountByAccountId::Params&& Params)
{
	return TOnlineResult<FAuthGetAccountByAccountId>(Errors::NotImplemented());
}

TOnlineEvent<void(const FLoginStatusChanged&)> FAuthCommon::OnLoginStatusChanged()
{
	return OnLoginStatusChangedEvent;
}

/* UE::Online */ }
