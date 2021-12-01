// Copyright Epic Games, Inc. All Rights Reserved.

#include "AuthEOS.h"

#include "OnlineIdEOS.h"
#include "OnlineServicesEOS.h"
#include "OnlineServicesEOSTypes.h"
#include "Algo/Transform.h"
#include "Online/AuthErrors.h"
#include "Online/OnlineAsyncOp.h"
#include "Online/OnlineErrorDefinitions.h"

#include "eos_auth.h"
#include "eos_common.h"
#include "eos_connect.h"
#include "eos_types.h"
#include "eos_init.h"
#include "eos_sdk.h"
#include "eos_logging.h"

namespace UE::Online {

static inline ELoginStatus ToELoginStatus(EOS_ELoginStatus InStatus)
{
	switch (InStatus)
	{
	case EOS_ELoginStatus::EOS_LS_NotLoggedIn:
	{
		return ELoginStatus::NotLoggedIn;
	}
	case EOS_ELoginStatus::EOS_LS_UsingLocalProfile:
	{
		return ELoginStatus::UsingLocalProfile;
	}
	case EOS_ELoginStatus::EOS_LS_LoggedIn:
	{
		return ELoginStatus::LoggedIn;
	}
	}
	return ELoginStatus::NotLoggedIn;
}

// Copied from OSS EOS

#define EOS_OSS_STRING_BUFFER_LENGTH 256
// Chose arbitrarily since the SDK doesn't define it
#define EOS_MAX_TOKEN_SIZE 4096

struct FEOSAuthCredentials :
	public EOS_Auth_Credentials
{
	FEOSAuthCredentials() :
		EOS_Auth_Credentials()
	{
		ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Id = IdAnsi;
		Token = TokenAnsi;
	}

	FEOSAuthCredentials(const FEOSAuthCredentials& Other)
	{
		ApiVersion = Other.ApiVersion;
		Id = IdAnsi;
		Token = TokenAnsi;
		Type = Other.Type;
		SystemAuthCredentialsOptions = Other.SystemAuthCredentialsOptions;
		ExternalType = Other.ExternalType;

		FCStringAnsi::Strncpy(IdAnsi, Other.IdAnsi, EOS_OSS_STRING_BUFFER_LENGTH);
		FCStringAnsi::Strncpy(TokenAnsi, Other.TokenAnsi, EOS_MAX_TOKEN_SIZE);
	}

	FEOSAuthCredentials(EOS_EExternalCredentialType InExternalType, const TArray<uint8>& InToken) :
		EOS_Auth_Credentials()
	{
		ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Type = EOS_ELoginCredentialType::EOS_LCT_ExternalAuth;
		ExternalType = InExternalType;
		Id = IdAnsi;
		Token = TokenAnsi;

		uint32_t InOutBufferLength = EOS_OSS_STRING_BUFFER_LENGTH;
		EOS_ByteArray_ToString(InToken.GetData(), InToken.Num(), TokenAnsi, &InOutBufferLength);
	}

	FEOSAuthCredentials& operator=(FEOSAuthCredentials& Other)
	{
		ApiVersion = Other.ApiVersion;
		Type = Other.Type;
		SystemAuthCredentialsOptions = Other.SystemAuthCredentialsOptions;
		ExternalType = Other.ExternalType;

		FCStringAnsi::Strncpy(IdAnsi, Other.IdAnsi, EOS_OSS_STRING_BUFFER_LENGTH);
		FCStringAnsi::Strncpy(TokenAnsi, Other.TokenAnsi, EOS_MAX_TOKEN_SIZE);

		return *this;
	}

	char IdAnsi[EOS_OSS_STRING_BUFFER_LENGTH];
	char TokenAnsi[EOS_MAX_TOKEN_SIZE];
};

FAuthEOS::FAuthEOS(FOnlineServicesEOS& InServices)
	: FAuthCommon(InServices)
{
}

void FAuthEOS::Initialize()
{
	FAuthCommon::Initialize();

	AuthHandle = EOS_Platform_GetAuthInterface(static_cast<FOnlineServicesEOS&>(GetServices()).GetEOSPlatformHandle());
	check(AuthHandle != nullptr);

	ConnectHandle = EOS_Platform_GetConnectInterface(static_cast<FOnlineServicesEOS&>(GetServices()).GetEOSPlatformHandle());
	check(ConnectHandle != nullptr);

	// Register for login status changes
	EOS_Auth_AddNotifyLoginStatusChangedOptions Options = { };
	Options.ApiVersion = EOS_AUTH_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;
	NotifyLoginStatusChangedNotificationId = EOS_Auth_AddNotifyLoginStatusChanged(AuthHandle, &Options, this, [](const EOS_Auth_LoginStatusChangedCallbackInfo* Data)
	{
		FAuthEOS* This = reinterpret_cast<FAuthEOS*>(Data->ClientData);

		FOnlineAccountIdHandle LocalUserId = FindAccountId(Data->LocalUserId);
		// invalid handle is expected for players logging in because this callback is called _before_ the login complete callback
		if (LocalUserId.IsValid())
		{
			ELoginStatus PreviousStatus = ToELoginStatus(Data->PrevStatus);
			ELoginStatus CurrentStatus = ToELoginStatus(Data->CurrentStatus);
			This->OnEOSLoginStatusChanged(LocalUserId, PreviousStatus, CurrentStatus);
		}
	});
}

void FAuthEOS::PreShutdown()
{
}

bool LexFromString(EOS_EAuthScopeFlags& OutEnum, const TCHAR* InString)
{
	if (FCString::Stricmp(InString, TEXT("BasicProfile")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;
	}
	else if (FCString::Stricmp(InString, TEXT("FriendsList")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_FriendsList;
	}
	else if (FCString::Stricmp(InString, TEXT("Presence")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_Presence;
	}
	else if (FCString::Stricmp(InString, TEXT("FriendsManagement")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_FriendsManagement;
	}
	else if (FCString::Stricmp(InString, TEXT("Email")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_Email;
	}
	else if (FCString::Stricmp(InString, TEXT("NoFlags")) == 0 || FCString::Stricmp(InString, TEXT("None")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_NoFlags;
	}
	else
	{
		return false;
	}
	return true;
}

bool LexFromString(EOS_ELoginCredentialType& OutEnum, const TCHAR* const InString)
{
	if (FCString::Stricmp(InString, TEXT("ExchangeCode")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_ExchangeCode;
	}
	else if (FCString::Stricmp(InString, TEXT("PersistentAuth")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
	}
	//else if (FCString::Stricmp(InString, TEXT("DeviceCode")) == 0) // DeviceCode is deprecated
	//{
	//	OutEnum = EOS_ELoginCredentialType::EOS_LCT_DeviceCode;
	//}
	else if (FCString::Stricmp(InString, TEXT("Password")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_Password;
	}
	else if (FCString::Stricmp(InString, TEXT("Developer")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_Developer;
	}
	else if (FCString::Stricmp(InString, TEXT("RefreshToken")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_RefreshToken;
	}
	else if (FCString::Stricmp(InString, TEXT("AccountPortal")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
	}
	else if (FCString::Stricmp(InString, TEXT("ExternalAuth")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_ExternalAuth;
	}
	else
	{
		return false;
	}
	return true;
}

TOnlineAsyncOpHandle<FAuthLogin> FAuthEOS::Login(FAuthLogin::Params&& Params)
{
	TOnlineAsyncOpRef<FAuthLogin> Op = GetOp<FAuthLogin>(MoveTemp(Params));

	EOS_Auth_LoginOptions LoginOptions = { };
	LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
	bool bContainsFlagsNone = false;
	for (const FString& Scope : Op->GetParams().Scopes)
	{
		EOS_EAuthScopeFlags ScopeFlag;
		if (LexFromString(ScopeFlag, *Scope))
		{
			if (ScopeFlag == EOS_EAuthScopeFlags::EOS_AS_NoFlags)
			{
				bContainsFlagsNone = true;
			}
			LoginOptions.ScopeFlags |= ScopeFlag;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Invalid ScopeFlag=[%s]"), *Scope);
			Op->SetError(Errors::Unknown());
			return Op->GetHandle();
		}
	}
	// TODO:  Where to put default scopes?
	if (!bContainsFlagsNone && LoginOptions.ScopeFlags == EOS_EAuthScopeFlags::EOS_AS_NoFlags)
	{
		LoginOptions.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile | EOS_EAuthScopeFlags::EOS_AS_FriendsList | EOS_EAuthScopeFlags::EOS_AS_Presence;
	}	

	FEOSAuthCredentials Credentials;
	if (LexFromString(Credentials.Type, *Op->GetParams().CredentialsType))
	{
		switch (Credentials.Type)
		{
		case EOS_ELoginCredentialType::EOS_LCT_ExchangeCode:
			// This is how the Epic launcher will pass credentials to you
			Credentials.IdAnsi[0] = '\0';
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op->GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_Password:
			FCStringAnsi::Strncpy(Credentials.IdAnsi, TCHAR_TO_UTF8(*Op->GetParams().CredentialsId), EOS_OSS_STRING_BUFFER_LENGTH);
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op->GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_Developer:
			// This is auth via the EOS auth tool
			FCStringAnsi::Strncpy(Credentials.IdAnsi, TCHAR_TO_UTF8(*Op->GetParams().CredentialsId), EOS_OSS_STRING_BUFFER_LENGTH);
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op->GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_AccountPortal:
			// This is auth via the EOS Account Portal
			Credentials.IdAnsi[0] = '\0';
			Credentials.TokenAnsi[0] = '\0';
			break;
		case EOS_ELoginCredentialType::EOS_LCT_PersistentAuth:
			// This is auth via stored credentials in EOS
			Credentials.Id = nullptr;
			Credentials.Token = nullptr;
			break;
		default:
			UE_LOG(LogTemp, Warning, TEXT("Unsupported CredentialsType=[%s]"), *Op->GetParams().CredentialsType);
			Op->SetError(Errors::Unknown()); // TODO
			return Op->GetHandle();
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid CredentialsType=[%s]"), *Op->GetParams().CredentialsType);
		Op->SetError(Errors::Unknown()); // TODO
		return Op->GetHandle();
	}

	Op->Then([this, LoginOptions, Credentials](TOnlineAsyncOp<FAuthLogin>& InAsyncOp) mutable
	{
		LoginOptions.Credentials = &Credentials;
		return EOS_Async<EOS_Auth_LoginCallbackInfo>(EOS_Auth_Login, AuthHandle, LoginOptions);
	})
	.Then([this](TOnlineAsyncOp<FAuthLogin>& InAsyncOp, const EOS_Auth_LoginCallbackInfo* Data)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[FAuthEOS::Login] EOS_Auth_Login Result: [%s]"), *LexToString(Data->ResultCode));

		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			// We cache the Epic Account Id to use it in later stages of the login process
			InAsyncOp.Data.Set(TEXT("EpicAccountId"), Data->LocalUserId);

			// On success, attempt Connect Login
			EOS_Auth_Token* AuthToken = nullptr;
			EOS_Auth_CopyUserAuthTokenOptions CopyOptions = { };
			CopyOptions.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;

			EOS_EResult CopyResult = EOS_Auth_CopyUserAuthToken(AuthHandle, &CopyOptions, Data->LocalUserId, &AuthToken);

			UE_LOG(LogTemp, Verbose, TEXT("[FAuthEOS::Login] EOS_Auth_CopyUserAuthToken Result: [%s]"), *LexToString(CopyResult));

			if (CopyResult == EOS_EResult::EOS_Success)
			{
				EOS_Connect_Credentials ConnectLoginCredentials = { };
				ConnectLoginCredentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
				ConnectLoginCredentials.Type = EOS_EExternalCredentialType::EOS_ECT_EPIC;
				ConnectLoginCredentials.Token = AuthToken->AccessToken;

				EOS_Connect_LoginOptions ConnectLoginOptions = { };
				ConnectLoginOptions.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
				ConnectLoginOptions.Credentials = &ConnectLoginCredentials;

				return EOS_Async<EOS_Connect_LoginCallbackInfo>(EOS_Connect_Login, ConnectHandle, ConnectLoginOptions);
			}
			else
			{
				// TODO: EAS Logout

				InAsyncOp.SetError(Errors::Unknown()); // TODO
			}
		}
		else if (Data->ResultCode == EOS_EResult::EOS_InvalidUser && Data->ContinuanceToken != nullptr)
		{
			// Link Account
		}
		else
		{
			FOnlineError Error = Errors::Unknown();
			if (Data->ResultCode == EOS_EResult::EOS_InvalidAuth)
			{
				Error = Errors::InvalidCreds();
			}

			InAsyncOp.SetError(MoveTemp(Error));
		}

		return MakeFulfilledPromise<const EOS_Connect_LoginCallbackInfo*>().GetFuture();
	})
	.Then([this](TOnlineAsyncOp<FAuthLogin>& InAsyncOp, const EOS_Connect_LoginCallbackInfo* Data)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[FAuthEOS::Login] EOS_Connect_Login Result: [%s]"), *LexToString(Data->ResultCode));

		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			// We cache the Product User Id to use it in later stages of the login process
			InAsyncOp.Data.Set(TEXT("ProductUserId"), Data->LocalUserId);

			ProcessSuccessfulLogin(InAsyncOp);
		}
		else if (Data->ResultCode == EOS_EResult::EOS_InvalidUser && Data->ContinuanceToken != nullptr)
		{
			EOS_Connect_CreateUserOptions ConnectCreateUserOptions = { };
			ConnectCreateUserOptions.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
			ConnectCreateUserOptions.ContinuanceToken = Data->ContinuanceToken;

			return EOS_Async<EOS_Connect_CreateUserCallbackInfo>(EOS_Connect_CreateUser, ConnectHandle, ConnectCreateUserOptions);
		}
		else
		{
			// TODO: EAS Logout

			InAsyncOp.SetError(Errors::Unknown()); // TODO
		}

		return MakeFulfilledPromise<const EOS_Connect_CreateUserCallbackInfo*>().GetFuture();
	})
	.Then([this](TOnlineAsyncOp<FAuthLogin>& InAsyncOp, const EOS_Connect_CreateUserCallbackInfo* Data)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[FAuthEOS::Login] EOS_Connect_CreateUser Result: [%s]"), *LexToString(Data->ResultCode));

		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			// We cache the Product User Id to use it in later stages of the login process
			InAsyncOp.Data.Set(TEXT("ProductUserId"), Data->LocalUserId);

			ProcessSuccessfulLogin(InAsyncOp);
		}
		else
		{
			// TODO: EAS Logout

			InAsyncOp.SetError(Errors::Unknown()); // TODO
		}
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

void FAuthEOS::ProcessSuccessfulLogin(TOnlineAsyncOp<FAuthLogin>& InAsyncOp)
{
	const EOS_EpicAccountId EpicAccountId = *InAsyncOp.Data.Get<EOS_EpicAccountId>(TEXT("EpicAccountId"));
	const EOS_ProductUserId ProductUserId = *InAsyncOp.Data.Get<EOS_ProductUserId>(TEXT("ProductUserId"));
	const FOnlineAccountIdHandle LocalUserId = CreateAccountId(EpicAccountId, ProductUserId);

	UE_LOG(LogTemp, Verbose, TEXT("[FAuthEOS::Login] Successfully logged in as [%s]"), *ToLogString(LocalUserId));

	TSharedRef<FAccountInfoEOS> AccountInfo = MakeShared<FAccountInfoEOS>();
	AccountInfo->LocalUserNum = InAsyncOp.GetParams().LocalUserNum;
	AccountInfo->UserId = LocalUserId;
	AccountInfo->LoginStatus = ELoginStatus::LoggedIn;

	check(!AccountInfos.Contains(LocalUserId));
	AccountInfos.Emplace(LocalUserId, AccountInfo);

	FAuthLogin::Result Result = { AccountInfo };
	InAsyncOp.SetResult(MoveTemp(Result));

	// When a user logs in, OnEOSLoginStatusChanged can not trigger (if it's that user's first login) or trigger before we add relevant information to AccountInfos, so we trigger the status change event here 
	FLoginStatusChanged EventParameters;
	EventParameters.LocalUserId = LocalUserId;
	EventParameters.PreviousStatus = ELoginStatus::NotLoggedIn;
	EventParameters.CurrentStatus = ELoginStatus::LoggedIn;
	OnLoginStatusChangedEvent.Broadcast(EventParameters);
}

TOnlineAsyncOpHandle<FAuthLogout> FAuthEOS::Logout(FAuthLogout::Params&& Params)
{
	const FOnlineAccountIdHandle LocalUserId = Params.LocalUserId;
	TOnlineAsyncOpRef<FAuthLogout> Op = GetOp<FAuthLogout>(MoveTemp(Params));

	if (!ValidateOnlineId(LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	const EOS_EpicAccountId LocalUserEasId = GetEpicAccountId(Params.LocalUserId);
	if (!EOS_EpicAccountId_IsValid(LocalUserEasId) || !AccountInfos.Contains(LocalUserId))
	{
		// TODO: Error codes
		Op->SetError(Errors::Unknown());
		return Op->GetHandle();
	}

	// Should we destroy persistent auth first?
	TOnlineChainableAsyncOp<FAuthLogout, void> NextOp = *Op;
	if (Params.bDestroyAuth)
	{
		EOS_Auth_DeletePersistentAuthOptions DeletePersistentAuthOptions = {0};
		DeletePersistentAuthOptions.ApiVersion = EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST;
		DeletePersistentAuthOptions.RefreshToken = nullptr; // Is this needed?  Docs say it's needed for consoles
		NextOp = NextOp.Then([this, DeletePersistentAuthOptions](TOnlineAsyncOp<FAuthLogout>& InAsyncOp)
		{
			return EOS_Async<EOS_Auth_DeletePersistentAuthCallbackInfo>(EOS_Auth_DeletePersistentAuth, AuthHandle, DeletePersistentAuthOptions);
		})
		.Then([](TOnlineAsyncOp<FAuthLogout>& InAsyncOp, const EOS_Auth_DeletePersistentAuthCallbackInfo* Data)
		{
			UE_LOG(LogTemp, Warning, TEXT("DeletePersistentAuthResult: [%s]"), ANSI_TO_TCHAR(EOS_EResult_ToString(Data->ResultCode)));
			// Regardless of success/failure, continue
		});
	}

	// Logout
	NextOp.Then([this, LocalUserEasId](TOnlineAsyncOp<FAuthLogout>& InAsyncOp)
	{
		EOS_Auth_LogoutOptions LogoutOptions = { };
		LogoutOptions.ApiVersion = EOS_AUTH_LOGOUT_API_LATEST;
		LogoutOptions.LocalUserId = LocalUserEasId;
		return EOS_Async<EOS_Auth_LogoutCallbackInfo>(EOS_Auth_Logout, AuthHandle, LogoutOptions);
	})
	.Then([](TOnlineAsyncOp<FAuthLogout>& InAsyncOp, const EOS_Auth_LogoutCallbackInfo* Data)
	{
		UE_LOG(LogTemp, Warning, TEXT("LogoutResult: [%s]"), *LexToString(Data->ResultCode));

		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			// Success
			InAsyncOp.SetResult(FAuthLogout::Result());

			// OnLoginStatusChanged will be triggered by OnEOSLoginStatusChanged
		}
		else
		{
			// TODO: Error codes
			FOnlineError Error = Errors::Unknown();
			InAsyncOp.SetError(MoveTemp(Error));
		}
	}).Enqueue(GetSerialQueue());
	
	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FAuthGenerateAuth> FAuthEOS::GenerateAuth(FAuthGenerateAuth::Params&& Params)
{
	TSharedRef<TOnlineAsyncOp<FAuthGenerateAuth>> AsyncOperation = MakeShared<TOnlineAsyncOp<FAuthGenerateAuth>>(Services, MoveTemp(Params));
	return AsyncOperation->GetHandle();
}

TOnlineResult<FAuthGetAccountByLocalUserNum> FAuthEOS::GetAccountByLocalUserNum(FAuthGetAccountByLocalUserNum::Params&& Params)
{
	TResult<FOnlineAccountIdHandle, FOnlineError> LocalUserIdResult = GetAccountIdByLocalUserNum(Params.LocalUserNum);
	if (LocalUserIdResult.IsOk())
	{
		FAuthGetAccountByLocalUserNum::Result Result = { AccountInfos.FindChecked(LocalUserIdResult.GetOkValue()) };
		return TOnlineResult<FAuthGetAccountByLocalUserNum>(Result);
	}
	else
	{
		return TOnlineResult<FAuthGetAccountByLocalUserNum>(LocalUserIdResult.GetErrorValue());
	}
}

TOnlineResult<FAuthGetAccountByAccountId> FAuthEOS::GetAccountByAccountId(FAuthGetAccountByAccountId::Params&& Params)
{
	if (TSharedRef<FAccountInfoEOS>* const FoundAccount = AccountInfos.Find(Params.LocalUserId))
	{
		FAuthGetAccountByAccountId::Result Result;
		Result.AccountInfo = *FoundAccount;
		return TOnlineResult<FAuthGetAccountByAccountId>(Result);
	}
	else
	{
		// TODO: proper error
		return TOnlineResult<FAuthGetAccountByAccountId>(Errors::Unknown());
	}
}

bool FAuthEOS::IsLoggedIn(const FOnlineAccountIdHandle& AccountId) const
{
	// TODO:  More logic?
	return AccountInfos.Contains(AccountId);
}

TResult<FOnlineAccountIdHandle, FOnlineError> FAuthEOS::GetAccountIdByLocalUserNum(int32 LocalUserNum) const
{
	for (const TPair<FOnlineAccountIdHandle, TSharedRef<FAccountInfoEOS>>& AccountPair : AccountInfos)
	{
		if (AccountPair.Value->LocalUserNum == LocalUserNum)
		{
			TResult<FOnlineAccountIdHandle, FOnlineError> Result(AccountPair.Key);
			return Result;
		}
	}
	TResult<FOnlineAccountIdHandle, FOnlineError> Result(Errors::Unknown()); // TODO: error code
	return Result;
}

void FAuthEOS::OnEOSLoginStatusChanged(FOnlineAccountIdHandle LocalUserId, ELoginStatus PreviousStatus, ELoginStatus CurrentStatus)
{
	UE_LOG(LogTemp, Warning, TEXT("OnEOSLoginStatusChanged: [%s] [%s]->[%s]"), *ToLogString(LocalUserId), LexToString(PreviousStatus), LexToString(CurrentStatus));
	if (TSharedRef<FAccountInfoEOS>* AccountInfoPtr = AccountInfos.Find(LocalUserId))
	{
		FAccountInfoEOS& AccountInfo = **AccountInfoPtr;
		if (AccountInfo.LoginStatus != CurrentStatus)
		{
			FLoginStatusChanged EventParameters;
			EventParameters.LocalUserId = LocalUserId;
			EventParameters.PreviousStatus = AccountInfo.LoginStatus;
			EventParameters.CurrentStatus = CurrentStatus;

			AccountInfo.LoginStatus = CurrentStatus;

			if (CurrentStatus == ELoginStatus::NotLoggedIn)
			{
				// Remove user
				AccountInfos.Remove(LocalUserId); // Invalidates AccountInfo
			}

			OnLoginStatusChangedEvent.Broadcast(EventParameters);
		}
	}
}

template <typename IdType>
TFuture<FOnlineAccountIdHandle> ResolveAccountIdImpl(FAuthEOS& AuthEOS, const FOnlineAccountIdHandle& LocalUserId, const IdType InId)
{
	TPromise<FOnlineAccountIdHandle> Promise;
	TFuture<FOnlineAccountIdHandle> Future = Promise.GetFuture();

	AuthEOS.ResolveAccountIds(LocalUserId, { InId }).Next([Promise = MoveTemp(Promise)](const TArray<FOnlineAccountIdHandle>& AccountIds) mutable
	{
		FOnlineAccountIdHandle Result;
		if (AccountIds.Num() == 1)
		{
			Result = AccountIds[0];
		}
		Promise.SetValue(Result);
	});

	return Future;
}

TFuture<FOnlineAccountIdHandle> FAuthEOS::ResolveAccountId(const FOnlineAccountIdHandle& LocalUserId, const EOS_EpicAccountId EpicAccountId)
{
	return ResolveAccountIdImpl(*this, LocalUserId, EpicAccountId);
}

TFuture<FOnlineAccountIdHandle> FAuthEOS::ResolveAccountId(const FOnlineAccountIdHandle& LocalUserId, const EOS_ProductUserId ProductUserId)
{
	return ResolveAccountIdImpl(*this, LocalUserId, ProductUserId);
}

using FEpicAccountIdStrBuffer = char[EOS_EPICACCOUNTID_MAX_LENGTH + 1];

TFuture<TArray<FOnlineAccountIdHandle>> FAuthEOS::ResolveAccountIds(const FOnlineAccountIdHandle& LocalUserId, const TArray<EOS_EpicAccountId>& InEpicAccountIds)
{
	// Search for all the account id's
	TArray<FOnlineAccountIdHandle> AccountIdHandles;
	AccountIdHandles.Reserve(InEpicAccountIds.Num());
	TArray<EOS_EpicAccountId> MissingEpicAccountIds;
	MissingEpicAccountIds.Reserve(InEpicAccountIds.Num());
	for (const EOS_EpicAccountId EpicAccountId : InEpicAccountIds)
	{
		if (!EOS_EpicAccountId_IsValid(EpicAccountId))
		{
			return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
		}

		FOnlineAccountIdHandle Found = FindAccountId(EpicAccountId);
		if (!Found.IsValid())
		{
			MissingEpicAccountIds.Emplace(EpicAccountId);
		}
		AccountIdHandles.Emplace(MoveTemp(Found));
	}
	if (MissingEpicAccountIds.IsEmpty())
	{
		// We have them all, so we can just return
		return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>(MoveTemp(AccountIdHandles)).GetFuture();
	}

	// If we failed to find all the handles, we need to query, which requires a valid LocalUserId
	if (!ValidateOnlineId(LocalUserId))
	{
		checkNoEntry();
		return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
	}

	TPromise<TArray<FOnlineAccountIdHandle>> Promise;
	TFuture<TArray<FOnlineAccountIdHandle>> Future = Promise.GetFuture();

	TArray<FEpicAccountIdStrBuffer> EpicAccountIdStrsToQuery;
	EpicAccountIdStrsToQuery.Reserve(MissingEpicAccountIds.Num());
	for (const EOS_EpicAccountId EpicAccountId : MissingEpicAccountIds)
	{
		FEpicAccountIdStrBuffer& EpicAccountIdStr = EpicAccountIdStrsToQuery.Emplace_GetRef();
		int32_t BufferSize = sizeof(EpicAccountIdStr);
		if (!EOS_EpicAccountId_IsValid(EpicAccountId) ||
			EOS_EpicAccountId_ToString(EpicAccountId, EpicAccountIdStr, &BufferSize) != EOS_EResult::EOS_Success)
		{
			checkNoEntry();
			return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
		}
	}

	TArray<const char*> EpicAccountIdStrPtrs;
	Algo::Transform(EpicAccountIdStrsToQuery, EpicAccountIdStrPtrs, [](const FEpicAccountIdStrBuffer& Str) { return &Str[0]; });

	EOS_Connect_QueryExternalAccountMappingsOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_QUERYEXTERNALACCOUNTMAPPINGS_API_LATEST;
	Options.LocalUserId = GetProductUserIdChecked(LocalUserId);
	Options.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;
	Options.ExternalAccountIds = (const char**)EpicAccountIdStrPtrs.GetData();
	Options.ExternalAccountIdCount = 1;

	EOS_Async<EOS_Connect_QueryExternalAccountMappingsCallbackInfo>(EOS_Connect_QueryExternalAccountMappings, ConnectHandle, Options)
		.Next([this, InEpicAccountIds, Promise = MoveTemp(Promise)](const EOS_Connect_QueryExternalAccountMappingsCallbackInfo* Data) mutable
	{
		TArray<FOnlineAccountIdHandle> AccountIds;
		AccountIds.Reserve(InEpicAccountIds.Num());
		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			EOS_Connect_GetExternalAccountMappingsOptions Options = {};
			Options.ApiVersion = EOS_CONNECT_GETEXTERNALACCOUNTMAPPING_API_LATEST;
			Options.LocalUserId = Data->LocalUserId;
			Options.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;

			for (const EOS_EpicAccountId EpicAccountId : InEpicAccountIds)
			{
				FOnlineAccountIdHandle AccountId = FindAccountId(EpicAccountId);
				if (!AccountId.IsValid())
				{
					FEpicAccountIdStrBuffer EpicAccountIdStr;
					int32_t BufferSize = sizeof(EpicAccountIdStr);
					verify(EOS_EpicAccountId_ToString(EpicAccountId, EpicAccountIdStr, &BufferSize) == EOS_EResult::EOS_Success);
					Options.TargetExternalUserId = &EpicAccountIdStr[0];
					const EOS_ProductUserId ProductUserId = EOS_Connect_GetExternalAccountMapping(ConnectHandle, &Options);
					AccountId = CreateAccountId(EpicAccountId, ProductUserId);
				}
				AccountIds.Emplace(MoveTemp(AccountId));
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ResolveAccountId failed to query external mapping Result=[%s]"), *LexToString(Data->ResultCode));
		}
		Promise.SetValue(MoveTemp(AccountIds));
	});

	return Future;
}

TFuture<TArray<FOnlineAccountIdHandle>> FAuthEOS::ResolveAccountIds(const FOnlineAccountIdHandle& LocalUserId, const TArray<EOS_ProductUserId>& InProductUserIds)
{
	// Search for all the account id's
	TArray<FOnlineAccountIdHandle> AccountIdHandles;
	AccountIdHandles.Reserve(InProductUserIds.Num());
	TArray<EOS_ProductUserId> MissingProductUserIds;
	MissingProductUserIds.Reserve(InProductUserIds.Num());
	for (const EOS_ProductUserId ProductUserId : InProductUserIds)
	{
		if (!EOS_ProductUserId_IsValid(ProductUserId))
		{
			return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
		}

		FOnlineAccountIdHandle Found = FindAccountId(ProductUserId);
		if (!Found.IsValid())
		{
			MissingProductUserIds.Emplace(ProductUserId);
		}
		AccountIdHandles.Emplace(MoveTemp(Found));
	}
	if (MissingProductUserIds.IsEmpty())
	{
		// We have them all, so we can just return
		return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>(MoveTemp(AccountIdHandles)).GetFuture();
	}

	// If we failed to find all the handles, we need to query, which requires a valid LocalUserId
	if (!ValidateOnlineId(LocalUserId))
	{
		checkNoEntry();
		return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
	}

	TPromise<TArray<FOnlineAccountIdHandle>> Promise;
	TFuture<TArray<FOnlineAccountIdHandle>> Future = Promise.GetFuture();

	EOS_Connect_QueryProductUserIdMappingsOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_QUERYPRODUCTUSERIDMAPPINGS_API_LATEST;
	Options.LocalUserId = GetProductUserIdChecked(LocalUserId);
	Options.ProductUserIds = MissingProductUserIds.GetData();
	Options.ProductUserIdCount = MissingProductUserIds.Num();
	EOS_Async<EOS_Connect_QueryProductUserIdMappingsCallbackInfo>(EOS_Connect_QueryProductUserIdMappings, ConnectHandle, Options)
		.Next([this, InProductUserIds, Promise = MoveTemp(Promise)](const EOS_Connect_QueryProductUserIdMappingsCallbackInfo* Data) mutable
	{
		TArray<FOnlineAccountIdHandle> AccountIds;
		if (Data->ResultCode == EOS_EResult::EOS_Success)
		{
			EOS_Connect_GetProductUserIdMappingOptions Options = {};
			Options.ApiVersion = EOS_CONNECT_GETPRODUCTUSERIDMAPPING_API_LATEST;
			Options.LocalUserId = Data->LocalUserId;
			Options.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;

			for (const EOS_ProductUserId ProductUserId : InProductUserIds)
			{
				FOnlineAccountIdHandle AccountId = FindAccountId(ProductUserId);
				if (!AccountId.IsValid())
				{
					Options.TargetProductUserId = ProductUserId;
					FEpicAccountIdStrBuffer EpicAccountIdStr;
					int32_t BufferLength = sizeof(EpicAccountIdStr);
					verify(EOS_Connect_GetProductUserIdMapping(ConnectHandle, &Options, EpicAccountIdStr, &BufferLength) == EOS_EResult::EOS_Success);
					const EOS_EpicAccountId EpicAccountId = EOS_EpicAccountId_FromString(EpicAccountIdStr);
					check(EOS_EpicAccountId_IsValid(EpicAccountId));
					AccountId = CreateAccountId(EpicAccountId, ProductUserId);
				}
				AccountIds.Emplace(MoveTemp(AccountId));
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ResolveAccountId failed to query external mapping Result=[%s]"), *LexToString(Data->ResultCode));
		}
		Promise.SetValue(MoveTemp(AccountIds));
	});

	return Future;
}

template<typename ParamType>
TFunction<TFuture<FOnlineAccountIdHandle>(FOnlineAsyncOp& InAsyncOp, const ParamType& Param)> ResolveIdFnImpl(FAuthEOS* AuthEOS)
{
	return [AuthEOS](FOnlineAsyncOp& InAsyncOp, const ParamType& Param)
	{
		const FOnlineAccountIdHandle* LocalUserIdPtr = InAsyncOp.Data.Get<FOnlineAccountIdHandle>(TEXT("LocalUserId"));
		if (!ensure(LocalUserIdPtr))
		{
			return MakeFulfilledPromise<FOnlineAccountIdHandle>().GetFuture();
		}
		return AuthEOS->ResolveAccountId(*LocalUserIdPtr, Param);
	};
}
TFunction<TFuture<FOnlineAccountIdHandle>(FOnlineAsyncOp& InAsyncOp, const EOS_EpicAccountId& ProductUserId)> FAuthEOS::ResolveEpicIdFn()
{
	return ResolveIdFnImpl<EOS_EpicAccountId>(this);
}
TFunction<TFuture<FOnlineAccountIdHandle>(FOnlineAsyncOp& InAsyncOp, const EOS_ProductUserId& ProductUserId)> FAuthEOS::ResolveProductIdFn()
{
	return ResolveIdFnImpl<EOS_ProductUserId>(this);
}

template<typename ParamType>
TFunction<TFuture<TArray<FOnlineAccountIdHandle>>(FOnlineAsyncOp& InAsyncOp, const TArray<ParamType>& Param)> ResolveIdsFnImpl(FAuthEOS* AuthEOS)
{
	return [AuthEOS](FOnlineAsyncOp& InAsyncOp, const TArray<ParamType>& Param)
	{
		const FOnlineAccountIdHandle* LocalUserIdPtr = InAsyncOp.Data.Get<FOnlineAccountIdHandle>(TEXT("LocalUserId"));
		if (!ensure(LocalUserIdPtr))
		{
			return MakeFulfilledPromise<TArray<FOnlineAccountIdHandle>>().GetFuture();
		}
		return AuthEOS->ResolveAccountIds(*LocalUserIdPtr, Param);
	};
}
TFunction<TFuture<TArray<FOnlineAccountIdHandle>>(FOnlineAsyncOp& InAsyncOp, const TArray<EOS_EpicAccountId>& EpicAccountIds)> FAuthEOS::ResolveEpicIdsFn()
{
	return ResolveIdsFnImpl<EOS_EpicAccountId>(this);
}
TFunction<TFuture<TArray<FOnlineAccountIdHandle>>(FOnlineAsyncOp& InAsyncOp, const TArray<EOS_ProductUserId>& ProductUserIds)> FAuthEOS::ResolveProductIdsFn()
{
	return ResolveIdsFnImpl<EOS_ProductUserId>(this);
}

/* UE::Online */ }
