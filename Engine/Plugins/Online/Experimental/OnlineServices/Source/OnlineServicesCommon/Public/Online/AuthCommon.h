// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/Auth.h"
#include "Online/OnlineComponent.h"

namespace UE::Online {

class FOnlineServicesCommon;

class ONLINESERVICESCOMMON_API FAuthCommon : public TOnlineComponent<IAuth>
{
public:
	using Super = IAuth;

	FAuthCommon(FOnlineServicesCommon& InServices);

	// TOnlineComponent
	virtual void RegisterCommands() override;

	// IAuth
	virtual TOnlineAsyncOpHandle<FAuthLogin> Login(FAuthLogin::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FAuthLogout> Logout(FAuthLogout::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FAuthGenerateAuth> GenerateAuth(FAuthGenerateAuth::Params&& Params) override;
	virtual TOnlineResult<FAuthGetAccountByLocalUserNum> GetAccountByLocalUserNum(FAuthGetAccountByLocalUserNum::Params&& Params) override;
	virtual TOnlineResult<FAuthGetAccountByAccountId> GetAccountByAccountId(FAuthGetAccountByAccountId::Params&& Params) override;
	virtual TOnlineEvent<void(const FLoginStatusChanged&)> OnLoginStatusChanged() override;

protected:
	TOnlineEventCallable<void(const FLoginStatusChanged&)> OnLoginStatusChangedEvent;
};

/* UE::Online */ }
