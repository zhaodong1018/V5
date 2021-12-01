// Copyright Epic Games, Inc. All Rights Reserved.

#include "EOSVoiceChat.h" 

#if WITH_EOS_RTC

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Stats/Stats.h"

#include "EOSShared.h"
#include "EOSVoiceChatErrors.h"
#include "EOSVoiceChatModule.h"
#include "EOSVoiceChatUser.h"
#include "IEOSSDKManager.h"
#include "VoiceChatErrors.h"
#include "VoiceChatResult.h"

#include "eos_sdk.h"
#include "eos_rtc.h"
#include "eos_rtc_audio.h"

#define CHECKPIN() FEOSVoiceChatPtr StrongThis = WeakThis.Pin(); if(!StrongThis) return

DEFINE_LOG_CATEGORY(LogEOSVoiceChat);

FVoiceChatResult ResultFromEOSResult(const EOS_EResult EosResult)
{
	FVoiceChatResult Result = FVoiceChatResult::CreateSuccess();
	if (EosResult != EOS_EResult::EOS_Success)
	{
		switch (EosResult)
		{
		case EOS_EResult::EOS_InvalidCredentials:
		case EOS_EResult::EOS_InvalidAuth:
		case EOS_EResult::EOS_Token_Not_Account:
			Result = VoiceChat::Errors::CredentialsInvalid();
			break;
		case EOS_EResult::EOS_InvalidUser:
		case EOS_EResult::EOS_InvalidParameters:
		case EOS_EResult::EOS_LimitExceeded:
			Result = VoiceChat::Errors::InvalidArgument();
			break;
		case EOS_EResult::EOS_AccessDenied:
		case EOS_EResult::EOS_MissingPermissions:
		case EOS_EResult::EOS_InvalidRequest:
			Result = VoiceChat::Errors::NotPermitted();
			break;
		case EOS_EResult::EOS_TooManyRequests:
			Result = VoiceChat::Errors::Throttled();
			break;
		case EOS_EResult::EOS_AlreadyPending:
			Result = VoiceChat::Errors::AlreadyInProgress();
			break;
		case EOS_EResult::EOS_NotConfigured:
			Result = VoiceChat::Errors::MissingConfig();
			break;
		case EOS_EResult::EOS_AlreadyConfigured:
			Result = VoiceChat::Errors::InvalidState();
			break;

			// TODO the rest
		case EOS_EResult::EOS_OperationWillRetry:
		case EOS_EResult::EOS_NoChange:
		case EOS_EResult::EOS_VersionMismatch:
		case EOS_EResult::EOS_Disabled:
		case EOS_EResult::EOS_DuplicateNotAllowed:

			// Auth/Presence/Friends/Ecom we're not expecting to encounter
		case EOS_EResult::EOS_Auth_AccountLocked:
		case EOS_EResult::EOS_Auth_AccountLockedForUpdate:
		case EOS_EResult::EOS_Auth_InvalidRefreshToken:
		case EOS_EResult::EOS_Auth_InvalidToken:
		case EOS_EResult::EOS_Auth_AuthenticationFailure:
		case EOS_EResult::EOS_Auth_InvalidPlatformToken:
		case EOS_EResult::EOS_Auth_WrongAccount:
		case EOS_EResult::EOS_Auth_WrongClient:
		case EOS_EResult::EOS_Auth_FullAccountRequired:
		case EOS_EResult::EOS_Auth_HeadlessAccountRequired:
		case EOS_EResult::EOS_Auth_PasswordResetRequired:
		case EOS_EResult::EOS_Auth_PasswordCannotBeReused:
		case EOS_EResult::EOS_Auth_Expired:
		case EOS_EResult::EOS_Auth_PinGrantCode:
		case EOS_EResult::EOS_Auth_PinGrantExpired:
		case EOS_EResult::EOS_Auth_PinGrantPending:
		case EOS_EResult::EOS_Auth_ExternalAuthNotLinked:
		case EOS_EResult::EOS_Auth_ExternalAuthRevoked:
		case EOS_EResult::EOS_Auth_ExternalAuthInvalid:
		case EOS_EResult::EOS_Auth_ExternalAuthRestricted:
		case EOS_EResult::EOS_Auth_ExternalAuthCannotLogin:
		case EOS_EResult::EOS_Auth_ExternalAuthExpired:
		case EOS_EResult::EOS_Auth_ExternalAuthIsLastLoginType:
		case EOS_EResult::EOS_Auth_ExchangeCodeNotFound:
		case EOS_EResult::EOS_Auth_OriginatingExchangeCodeSessionExpired:
		case EOS_EResult::EOS_Auth_PersistentAuth_AccountNotActive:
		case EOS_EResult::EOS_Auth_MFARequired:
		case EOS_EResult::EOS_Auth_ParentalControls:
		case EOS_EResult::EOS_Auth_NoRealId:
		case EOS_EResult::EOS_Friends_InviteAwaitingAcceptance:
		case EOS_EResult::EOS_Friends_NoInvitation:
		case EOS_EResult::EOS_Friends_AlreadyFriends:
		case EOS_EResult::EOS_Friends_NotFriends:
		case EOS_EResult::EOS_Presence_DataInvalid:
		case EOS_EResult::EOS_Presence_DataLengthInvalid:
		case EOS_EResult::EOS_Presence_DataKeyInvalid:
		case EOS_EResult::EOS_Presence_DataKeyLengthInvalid:
		case EOS_EResult::EOS_Presence_DataValueInvalid:
		case EOS_EResult::EOS_Presence_DataValueLengthInvalid:
		case EOS_EResult::EOS_Presence_RichTextInvalid:
		case EOS_EResult::EOS_Presence_RichTextLengthInvalid:
		case EOS_EResult::EOS_Presence_StatusInvalid:
		case EOS_EResult::EOS_Ecom_EntitlementStale:

			// Intentional fall-through cases
		case EOS_EResult::EOS_NoConnection:
		case EOS_EResult::EOS_Canceled:
		case EOS_EResult::EOS_IncompatibleVersion:
		case EOS_EResult::EOS_UnrecognizedResponse:
		case EOS_EResult::EOS_NotImplemented:
		case EOS_EResult::EOS_NotFound:
		case EOS_EResult::EOS_UnexpectedError:
		default:
			// TODO map more EOS statuses to text error codes
			Result = EOSVOICECHAT_ERROR(EVoiceChatResult::ImplementationError, *LexToString(EosResult));
			break;
		}

		Result.ErrorNum = static_cast<int>(EosResult);
		Result.ErrorDesc = FString::Printf(TEXT("EOS_EResult=%s"), *LexToString(EosResult));
	}

	return Result;
}

const TCHAR* LexToString(EOS_ERTCAudioInputStatus Status)
{
	switch (Status)
	{
	case EOS_ERTCAudioInputStatus::EOS_RTCAIS_Idle: return TEXT("EOS_RTCAIS_Idle");
	case EOS_ERTCAudioInputStatus::EOS_RTCAIS_Recording: return TEXT("EOS_RTCAIS_Recording");
	case EOS_ERTCAudioInputStatus::EOS_RTCAIS_RecordingSilent: return TEXT("EOS_RTCAIS_RecordingSilent");
	case EOS_ERTCAudioInputStatus::EOS_RTCAIS_RecordingDisconnected: return TEXT("EOS_RTCAIS_RecordingDisconnected");
	case EOS_ERTCAudioInputStatus::EOS_RTCAIS_Failed: return TEXT("EOS_RTCEOS_RTCAIS_Failed_AudioInputFailed");
	default: return TEXT("Unknown");
	}
}

FEOSVoiceChatDelegates::FOnAudioInputDeviceStatusChanged FEOSVoiceChatDelegates::OnAudioInputDeviceStatusChanged;
FEOSVoiceChatDelegates::FOnVoiceChatChannelConnectionStateDelegate FEOSVoiceChatDelegates::OnVoiceChatChannelConnectionStateChanged;
FEOSVoiceChatDelegates::FOnVoiceChatPlayerAddedMetadataDelegate FEOSVoiceChatDelegates::OnVoiceChatPlayerAddedMetadata;
FEOSVoiceChatDelegates::FOnAudioStatusChanged FEOSVoiceChatDelegates::OnAudioStatusChanged;

int64 FEOSVoiceChat::StaticInstanceIdCount = 0;

#define EOS_VOICE_TODO 0

FEOSVoiceChat::FEOSVoiceChat(IEOSSDKManager& InSDKManager, const IEOSPlatformHandlePtr& PlatformHandle)
	: SDKManager(InSDKManager)
	, ExternalPlatformHandle(PlatformHandle)
{
}

FEOSVoiceChat::~FEOSVoiceChat()
{
}

#pragma region IVoiceChat
bool FEOSVoiceChat::Initialize()
{
	if (!IsInitialized())
	{
		Initialize(FOnVoiceChatInitializeCompleteDelegate());
	}

	return IsInitialized();
}

bool FEOSVoiceChat::Uninitialize()
{
	bool bIsDone = false;
	Uninitialize(FOnVoiceChatUninitializeCompleteDelegate::CreateLambda([&bIsDone](const FVoiceChatResult& Result)
	{
		bIsDone = true;
	}));

	while (!bIsDone)
	{
		InitSession.EosPlatformHandle->Tick();
	}
	
	return !IsInitialized();
}

void FEOSVoiceChat::Initialize(const FOnVoiceChatInitializeCompleteDelegate& InitCompleteDelegate)
{
	FVoiceChatResult Result(FVoiceChatResult::CreateSuccess());

	switch (InitSession.State)
	{
	case EInitializationState::Uninitialized:
	{
		bool bEnabled = true;
		GConfig->GetBool(TEXT("EOSVoiceChat"), TEXT("bEnabled"), bEnabled, GEngineIni);
		if (bEnabled)
		{
			InitSession.State = EInitializationState::Initializing;

			EOS_EResult EosResult = SDKManager.Initialize();
			if (EosResult == EOS_EResult::EOS_Success)
			{
				if(ExternalPlatformHandle)
				{
					InitSession.EosPlatformHandle = ExternalPlatformHandle;
				}
				else
				{
					FString ConfigProductId;
					FString ConfigSandboxId;
					FString ConfigDeploymentId;
					FString ConfigClientId;
					FString ConfigClientSecret;
					FString ConfigEncryptionKey;
					FString ConfigOverrideCountryCode;
					FString ConfigOverrideLocaleCode;

					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("ProductId"), ConfigProductId, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("SandboxId"), ConfigSandboxId, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("DeploymentId"), ConfigDeploymentId, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("ClientId"), ConfigClientId, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("ClientSecret"), ConfigClientSecret, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("EncryptionKey"), ConfigEncryptionKey, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("OverrideCountryCode"), ConfigOverrideCountryCode, GEngineIni);
					GConfig->GetString(TEXT("EOSVoiceChat"), TEXT("OverrideLocaleCode"), ConfigOverrideLocaleCode, GEngineIni);

					const FTCHARToUTF8 Utf8ProductId(*ConfigProductId);
					const FTCHARToUTF8 Utf8SandboxId(*ConfigSandboxId);
					const FTCHARToUTF8 Utf8DeploymentId(*ConfigDeploymentId);
					const FTCHARToUTF8 Utf8ClientId(*ConfigClientId);
					const FTCHARToUTF8 Utf8ClientSecret(*ConfigClientSecret);
					const FTCHARToUTF8 Utf8EncryptionKey(*ConfigEncryptionKey);
					const FTCHARToUTF8 Utf8OverrideCountryCode(*ConfigOverrideCountryCode);
					const FTCHARToUTF8 Utf8OverrideLocaleCode(*ConfigOverrideLocaleCode);

					EOS_Platform_Options PlatformOptions = {};
					PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
					static_assert(EOS_PLATFORM_OPTIONS_API_LATEST == 11, "EOS_Platform_Options updated, check new fields");
					PlatformOptions.Reserved = nullptr;
					PlatformOptions.ProductId = ConfigProductId.IsEmpty() ? nullptr : Utf8ProductId.Get();
					PlatformOptions.SandboxId = ConfigSandboxId.IsEmpty() ? nullptr : Utf8SandboxId.Get();
					PlatformOptions.ClientCredentials.ClientId = ConfigClientId.IsEmpty() ? nullptr : Utf8ClientId.Get();
					PlatformOptions.ClientCredentials.ClientSecret = ConfigClientSecret.IsEmpty() ? nullptr : Utf8ClientSecret.Get();
					PlatformOptions.bIsServer = false;
					PlatformOptions.EncryptionKey = ConfigEncryptionKey.IsEmpty() ? nullptr : Utf8EncryptionKey.Get();
					PlatformOptions.OverrideCountryCode = ConfigOverrideCountryCode.IsEmpty() ? nullptr : Utf8OverrideCountryCode.Get();
					PlatformOptions.OverrideLocaleCode = ConfigOverrideLocaleCode.IsEmpty() ? nullptr : Utf8OverrideLocaleCode.Get();
					PlatformOptions.DeploymentId = ConfigDeploymentId.IsEmpty() ? nullptr : Utf8DeploymentId.Get();
					PlatformOptions.Flags = EOS_PF_DISABLE_OVERLAY;
					PlatformOptions.CacheDirectory = nullptr;
					PlatformOptions.TickBudgetInMilliseconds = 1;
#if UE_EDITOR
					//PlatformCreateOptions.Flags |= EOS_PF_LOADING_IN_EDITOR;
#endif

					EOS_Platform_RTCOptions PlatformRTCOptions = {};
					PlatformRTCOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;
					static_assert(EOS_PLATFORM_RTCOPTIONS_API_LATEST == 1, "EOS_Platform_RTCOptions updated, check new fields");
					PlatformOptions.RTCOptions = &PlatformRTCOptions;

					InitSession.EosPlatformHandle = EOSPlatformCreate(PlatformOptions);
					if (!InitSession.EosPlatformHandle)
					{
						UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Initialize CreatePlatform failed"));
						InitSession = FInitSession();
						Result = FVoiceChatResult(EVoiceChatResult::ImplementationError);
					}
				}

				if (Result.IsSuccess())
				{
					InitSession.EosRtcInterface = EOS_Platform_GetRTCInterface(*InitSession.EosPlatformHandle);
					InitSession.EosLobbyInterface = EOS_Platform_GetLobbyInterface(*InitSession.EosPlatformHandle);
					if (InitSession.EosRtcInterface)
					{
						BindInitCallbacks();
						InitSession.State = EInitializationState::Initialized;
						PostInitialize();
						Result = FVoiceChatResult::CreateSuccess();
					}
					else
					{
						UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Initialize failed to get RTC interface handle"));
						InitSession = FInitSession();
						Result = FVoiceChatResult(EVoiceChatResult::ImplementationError);
					}
				}
			}
			else
			{
				UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Initialize Initialize failed"));
				InitSession = FInitSession();
				Result = FVoiceChatResult(EVoiceChatResult::ImplementationError);
			}
		}
		else
		{
			Result = VoiceChat::Errors::NotEnabled();
		}
		break;
	}
	case EInitializationState::Uninitializing:
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Initialize call unexpected while State=Uninitializing"));
		Result = VoiceChat::Errors::InvalidState();
		break;
	case EInitializationState::Initializing:
		checkNoEntry(); // Should not be possible, Initialize is a synchronous call.
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Initialize call unexpected while State=Initializing"));
		Result = VoiceChat::Errors::InvalidState();
		break;
	case EInitializationState::Initialized:
		Result = FVoiceChatResult::CreateSuccess();
		break;
	}

	InitCompleteDelegate.ExecuteIfBound(Result);
}

void FEOSVoiceChat::Uninitialize(const FOnVoiceChatUninitializeCompleteDelegate& UninitCompleteDelegate)
{
	switch (InitSession.State)
	{
	case EInitializationState::Uninitialized:
		UninitCompleteDelegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
		break;
	case EInitializationState::Uninitializing:
		InitSession.UninitializeCompleteDelegates.Emplace(UninitCompleteDelegate);
		break;
	case EInitializationState::Initializing:
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Uninitialize call unexpected while State=Initializing"));
		UninitCompleteDelegate.ExecuteIfBound(VoiceChat::Errors::InvalidState());
		break;
	case EInitializationState::Initialized:
		InitSession.State = EInitializationState::Uninitializing;
		InitSession.UninitializeCompleteDelegates.Emplace(UninitCompleteDelegate);

		auto CompleteUninitialize = [this]()
		{
			PreUninitialize();
			UnbindInitCallbacks();

			const TArray<FOnVoiceChatUninitializeCompleteDelegate> UninitializeCompleteDelegates = MoveTemp(InitSession.UninitializeCompleteDelegates);
			InitSession = FInitSession();
			for (const FOnVoiceChatUninitializeCompleteDelegate& UninitializeCompleteDelegate : UninitializeCompleteDelegates)
			{
				UninitializeCompleteDelegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
			}
		};

		if (IsConnected())
		{
			Disconnect(FOnVoiceChatDisconnectCompleteDelegate::CreateLambda([this, CompleteUninitialize](const FVoiceChatResult& Result)
			{
				if (Result.IsSuccess())
				{
					CompleteUninitialize();
				}
				else
				{
					UE_LOG(LogEOSVoiceChat, Warning, TEXT("FEOSVoiceChat::Uninitialize failed %s"), *LexToString(Result));

					InitSession.State = EInitializationState::Initialized;

					const TArray<FOnVoiceChatUninitializeCompleteDelegate> Delegates = MoveTemp(InitSession.UninitializeCompleteDelegates);
					for (const FOnVoiceChatUninitializeCompleteDelegate& Delegate : Delegates)
					{
						Delegate.ExecuteIfBound(Result);
					}
				}
			}));
		}
		else
		{
			CompleteUninitialize();
		}
		break;
	}
}

bool FEOSVoiceChat::IsInitialized() const
{
	return InitSession.State == EInitializationState::Initialized;
}

IVoiceChatUser* FEOSVoiceChat::CreateUser()
{
	const FEOSVoiceChatUserRef& User = VoiceChatUsers.Emplace_GetRef(MakeShared<FEOSVoiceChatUser, ESPMode::ThreadSafe>(*this));

	return &User.Get();
}

void FEOSVoiceChat::ReleaseUser(IVoiceChatUser* User)
{
	if (User)
	{		
		if (IsInitialized()
			&& IsConnected()
			&& User->IsLoggedIn())
		{
			UE_LOG(LogEOSVoiceChat, Log, TEXT("ReleaseUser User=[%p] Logging out"), User);
			User->Logout(FOnVoiceChatLogoutCompleteDelegate::CreateLambda([this, WeakThis = CreateWeakThis(), User](const FString& PlayerName, const FVoiceChatResult& Result)
			{
				CHECKPIN();

				if (!Result.IsSuccess())
				{
					UE_LOG(LogEOSVoiceChat, Warning, TEXT("ReleaseUser User=[%p] Logout failed, Result=[%s]"), User, *LexToString(Result))
				}

				UE_LOG(LogEOSVoiceChat, Log, TEXT("ReleaseUser User=[%p] Removing"), User);
				VoiceChatUsers.RemoveAll([User](const FEOSVoiceChatUserRef& OtherUser)
				{
					return User == &OtherUser.Get();
				});
			}));
		}
		else
		{
			UE_LOG(LogEOSVoiceChat, Log, TEXT("ReleaseUser User=[%p] Removing"), User);
			VoiceChatUsers.RemoveAll([User](const FEOSVoiceChatUserRef& OtherUser)
			{
				return User == &OtherUser.Get();
			});
		}
	}
}
#pragma endregion IVoiceChat

#pragma region IVoiceChatUser
void FEOSVoiceChat::SetSetting(const FString& Name, const FString& Value)
{
	GetVoiceChatUser().SetSetting(Name, Value);
}

FString FEOSVoiceChat::GetSetting(const FString& Name)
{
	return GetVoiceChatUser().GetSetting(Name);
}

void FEOSVoiceChat::SetAudioInputVolume(float Volume)
{
	GetVoiceChatUser().SetAudioInputVolume(Volume);
}

void FEOSVoiceChat::SetAudioOutputVolume(float Volume)
{
	GetVoiceChatUser().SetAudioOutputVolume(Volume);
}

float FEOSVoiceChat::GetAudioInputVolume() const
{
	return GetVoiceChatUser().GetAudioInputVolume();
}

float FEOSVoiceChat::GetAudioOutputVolume() const
{
	return GetVoiceChatUser().GetAudioOutputVolume();
}

void FEOSVoiceChat::SetAudioInputDeviceMuted(bool bIsMuted)
{
	GetVoiceChatUser().SetAudioInputDeviceMuted(bIsMuted);
}

void FEOSVoiceChat::SetAudioOutputDeviceMuted(bool bIsMuted)
{
	GetVoiceChatUser().SetAudioOutputDeviceMuted(bIsMuted);
}

bool FEOSVoiceChat::GetAudioInputDeviceMuted() const
{
	return GetVoiceChatUser().GetAudioInputDeviceMuted();
}

bool FEOSVoiceChat::GetAudioOutputDeviceMuted() const
{
	return GetVoiceChatUser().GetAudioOutputDeviceMuted();
}

TArray<FVoiceChatDeviceInfo> FEOSVoiceChat::GetAvailableInputDeviceInfos() const
{
	return GetVoiceChatUser().GetAvailableOutputDeviceInfos();
}

TArray<FVoiceChatDeviceInfo> FEOSVoiceChat::GetAvailableOutputDeviceInfos() const
{
	return GetVoiceChatUser().GetAvailableOutputDeviceInfos();
}

FOnVoiceChatAvailableAudioDevicesChangedDelegate& FEOSVoiceChat::OnVoiceChatAvailableAudioDevicesChanged()
{
	return GetVoiceChatUser().OnVoiceChatAvailableAudioDevicesChanged();
}

void FEOSVoiceChat::SetInputDeviceId(const FString& InputDeviceId)
{
	GetVoiceChatUser().SetInputDeviceId(InputDeviceId);
}

void FEOSVoiceChat::SetOutputDeviceId(const FString& OutputDeviceId)
{
	GetVoiceChatUser().SetOutputDeviceId(OutputDeviceId);
}

FVoiceChatDeviceInfo FEOSVoiceChat::GetInputDeviceInfo() const
{
	return GetVoiceChatUser().GetInputDeviceInfo();
}

FVoiceChatDeviceInfo FEOSVoiceChat::GetOutputDeviceInfo() const
{
	return GetVoiceChatUser().GetOutputDeviceInfo();
}

FVoiceChatDeviceInfo FEOSVoiceChat::GetDefaultInputDeviceInfo() const
{
	return GetVoiceChatUser().GetDefaultInputDeviceInfo();
}

FVoiceChatDeviceInfo FEOSVoiceChat::GetDefaultOutputDeviceInfo() const
{
	return GetVoiceChatUser().GetDefaultOutputDeviceInfo();
}

void FEOSVoiceChat::Connect(const FOnVoiceChatConnectCompleteDelegate& Delegate)
{
	FVoiceChatResult Result = FVoiceChatResult::CreateSuccess();

	if (!IsInitialized())
	{
		Result = VoiceChat::Errors::NotInitialized();
	}
	else if (ConnectionState == EConnectionState::Disconnecting)
	{
		Result = VoiceChat::Errors::DisconnectInProgress();
	}

	if (!Result.IsSuccess())
	{
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("Connect %s"), *LexToString(Result));
		Delegate.ExecuteIfBound(Result);
	}
	else if (IsConnected())
	{
		Delegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
	}
	else
	{
		ConnectionState = EConnectionState::Connected;
		Delegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
		OnVoiceChatConnected().Broadcast();
	}
}

void FEOSVoiceChat::Disconnect(const FOnVoiceChatDisconnectCompleteDelegate& Delegate)
{
	// TODO Handle Disconnecting / Connecting states now this is async.
	if (IsConnected())
	{
		ConnectionState = EConnectionState::Disconnecting;

		TSet<FEOSVoiceChatUser*> UsersToLogout;

		if (SingleUserVoiceChatUser)
		{
			FEOSVoiceChatUser::ELoginState LoginState = SingleUserVoiceChatUser->LoginSession.State;
			if (LoginState == FEOSVoiceChatUser::ELoginState::LoggedIn || LoginState == FEOSVoiceChatUser::ELoginState::LoggingOut)
			{
				UsersToLogout.Emplace(SingleUserVoiceChatUser);
			}
		}
		else
		{
			for (const FEOSVoiceChatUserRef& VoiceChatUser : VoiceChatUsers)
			{
				FEOSVoiceChatUser::ELoginState LoginState = VoiceChatUser->LoginSession.State;
				if (LoginState == FEOSVoiceChatUser::ELoginState::LoggedIn || LoginState == FEOSVoiceChatUser::ELoginState::LoggingOut)
				{
					UsersToLogout.Emplace(&VoiceChatUser.Get());
				}
			}
		}

		if (UsersToLogout.Num() > 0)
		{
			struct FEOSVoiceChatDisconnectState
			{
				FVoiceChatResult Result = FVoiceChatResult::CreateSuccess();
				FOnVoiceChatDisconnectCompleteDelegate CompletionDelegate;
				int32 UsersToLogoutCount;
			};
			TSharedPtr<FEOSVoiceChatDisconnectState> DisconnectState = MakeShared<FEOSVoiceChatDisconnectState>();
			DisconnectState->UsersToLogoutCount = UsersToLogout.Num();
			DisconnectState->CompletionDelegate = Delegate;

			for (FEOSVoiceChatUser* User : UsersToLogout)
			{
				User->LogoutInternal(FOnVoiceChatLogoutCompleteDelegate::CreateLambda([this, User, DisconnectState](const FString& PlayerName, const FVoiceChatResult& PlayerResult)
				{
					if (!PlayerResult.IsSuccess())
					{
						UE_LOG(LogEOSVoiceChat, Warning, TEXT("Disconnect LogoutCompleteDelegate PlayerName=[%s] Result=%s"), *PlayerName, *LexToString(PlayerResult));
						DisconnectState->Result = PlayerResult;
					}

					DisconnectState->UsersToLogoutCount--;

					if (DisconnectState->UsersToLogoutCount == 0)
					{
						ConnectionState = DisconnectState->Result.IsSuccess() ? EConnectionState::Disconnected : EConnectionState::Connected;
						DisconnectState->CompletionDelegate.ExecuteIfBound(DisconnectState->Result);
						if (ConnectionState == EConnectionState::Disconnected)
						{
							OnVoiceChatDisconnected().Broadcast(DisconnectState->Result);
						}
					}
				}));
			}
		}
		else
		{
			ConnectionState = EConnectionState::Disconnected;
			Delegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
			OnVoiceChatDisconnected().Broadcast(FVoiceChatResult::CreateSuccess());
		}
	}
	else
	{
		Delegate.ExecuteIfBound(FVoiceChatResult::CreateSuccess());
	}
}

bool FEOSVoiceChat::IsConnecting() const
{
	return false;
}

bool FEOSVoiceChat::IsConnected() const
{
	return ConnectionState == EConnectionState::Connected;
}

void FEOSVoiceChat::Login(FPlatformUserId PlatformId, const FString& PlayerName, const FString& Credentials, const FOnVoiceChatLoginCompleteDelegate& Delegate)
{
	GetVoiceChatUser().Login(PlatformId, PlayerName, Credentials, Delegate);
}

void FEOSVoiceChat::Logout(const FOnVoiceChatLogoutCompleteDelegate& Delegate)
{
	GetVoiceChatUser().Logout(Delegate);
}

bool FEOSVoiceChat::IsLoggingIn() const
{
	return GetVoiceChatUser().IsLoggingIn();
}

bool FEOSVoiceChat::IsLoggedIn() const
{
	return GetVoiceChatUser().IsLoggedIn();
}

FOnVoiceChatLoggedInDelegate& FEOSVoiceChat::OnVoiceChatLoggedIn()
{
	return GetVoiceChatUser().OnVoiceChatLoggedIn();
}

FOnVoiceChatLoggedOutDelegate& FEOSVoiceChat::OnVoiceChatLoggedOut()
{
	return GetVoiceChatUser().OnVoiceChatLoggedOut();
}

FString FEOSVoiceChat::GetLoggedInPlayerName() const
{
	return GetVoiceChatUser().GetLoggedInPlayerName();
}

void FEOSVoiceChat::BlockPlayers(const TArray<FString>& PlayerNames)
{
	GetVoiceChatUser().BlockPlayers(PlayerNames);
}

void FEOSVoiceChat::UnblockPlayers(const TArray<FString>& PlayerNames)
{
	GetVoiceChatUser().UnblockPlayers(PlayerNames);
}

void FEOSVoiceChat::JoinChannel(const FString& ChannelName, const FString& ChannelCredentials, EVoiceChatChannelType ChannelType, const FOnVoiceChatChannelJoinCompleteDelegate& Delegate, TOptional<FVoiceChatChannel3dProperties> Channel3dProperties)
{
	GetVoiceChatUser().JoinChannel(ChannelName, ChannelCredentials, ChannelType, Delegate, Channel3dProperties);
}

void FEOSVoiceChat::LeaveChannel(const FString& Channel, const FOnVoiceChatChannelLeaveCompleteDelegate& Delegate)
{
	GetVoiceChatUser().LeaveChannel(Channel, Delegate);
}

FOnVoiceChatChannelJoinedDelegate& FEOSVoiceChat::OnVoiceChatChannelJoined()
{
	return GetVoiceChatUser().OnVoiceChatChannelJoined();
}

FOnVoiceChatChannelExitedDelegate& FEOSVoiceChat::OnVoiceChatChannelExited()
{
	return GetVoiceChatUser().OnVoiceChatChannelExited();
}

FOnVoiceChatCallStatsUpdatedDelegate& FEOSVoiceChat::OnVoiceChatCallStatsUpdated()
{
	return GetVoiceChatUser().OnVoiceChatCallStatsUpdated();
}

void FEOSVoiceChat::Set3DPosition(const FString& ChannelName, const FVector& SpeakerPosition, const FVector& ListenerPosition, const FVector& ListenerForwardDirection, const FVector& ListenerUpDirection)
{
	GetVoiceChatUser().Set3DPosition(ChannelName, SpeakerPosition, ListenerPosition, ListenerForwardDirection, ListenerUpDirection);
}

TArray<FString> FEOSVoiceChat::GetChannels() const
{
	return GetVoiceChatUser().GetChannels();
}

TArray<FString> FEOSVoiceChat::GetPlayersInChannel(const FString& ChannelName) const
{
	return GetVoiceChatUser().GetPlayersInChannel(ChannelName);
}

EVoiceChatChannelType FEOSVoiceChat::GetChannelType(const FString& ChannelName) const
{
	return GetVoiceChatUser().GetChannelType(ChannelName);
}

FOnVoiceChatPlayerAddedDelegate& FEOSVoiceChat::OnVoiceChatPlayerAdded()
{
	return GetVoiceChatUser().OnVoiceChatPlayerAdded();
}

FOnVoiceChatPlayerRemovedDelegate& FEOSVoiceChat::OnVoiceChatPlayerRemoved()
{
	return GetVoiceChatUser().OnVoiceChatPlayerRemoved();
}

bool FEOSVoiceChat::IsPlayerTalking(const FString& PlayerName) const
{
	return GetVoiceChatUser().IsPlayerTalking(PlayerName);
}

FOnVoiceChatPlayerTalkingUpdatedDelegate& FEOSVoiceChat::OnVoiceChatPlayerTalkingUpdated()
{
	return GetVoiceChatUser().OnVoiceChatPlayerTalkingUpdated();
}

void FEOSVoiceChat::SetPlayerMuted(const FString& PlayerName, bool bMuted)
{
	GetVoiceChatUser().SetPlayerMuted(PlayerName, bMuted);
}

bool FEOSVoiceChat::IsPlayerMuted(const FString& PlayerName) const
{
	return GetVoiceChatUser().IsPlayerMuted(PlayerName);
}

void FEOSVoiceChat::SetChannelPlayerMuted(const FString& ChannelName, const FString& PlayerName, bool bMuted)
{
	GetVoiceChatUser().SetChannelPlayerMuted(ChannelName, PlayerName, bMuted);
}

bool FEOSVoiceChat::IsChannelPlayerMuted(const FString& ChannelName, const FString& PlayerName) const
{
	return GetVoiceChatUser().IsChannelPlayerMuted(ChannelName, PlayerName);
}

FOnVoiceChatPlayerMuteUpdatedDelegate& FEOSVoiceChat::OnVoiceChatPlayerMuteUpdated()
{
	return GetVoiceChatUser().OnVoiceChatPlayerMuteUpdated();
}

void FEOSVoiceChat::SetPlayerVolume(const FString& PlayerName, float Volume)
{
	GetVoiceChatUser().SetPlayerVolume(PlayerName, Volume);
}

float FEOSVoiceChat::GetPlayerVolume(const FString& PlayerName) const
{
	return GetVoiceChatUser().GetPlayerVolume(PlayerName);
}

FOnVoiceChatPlayerVolumeUpdatedDelegate& FEOSVoiceChat::OnVoiceChatPlayerVolumeUpdated()
{
	return GetVoiceChatUser().OnVoiceChatPlayerVolumeUpdated();
}

void FEOSVoiceChat::TransmitToAllChannels()
{
	GetVoiceChatUser().TransmitToAllChannels();
}

void FEOSVoiceChat::TransmitToNoChannels()
{
	GetVoiceChatUser().TransmitToNoChannels();
}

void FEOSVoiceChat::TransmitToSpecificChannel(const FString& ChannelName)
{
	GetVoiceChatUser().TransmitToSpecificChannel(ChannelName);
}

EVoiceChatTransmitMode FEOSVoiceChat::GetTransmitMode() const
{
	return GetVoiceChatUser().GetTransmitMode();
}

FString FEOSVoiceChat::GetTransmitChannel() const
{
	return GetVoiceChatUser().GetTransmitChannel();
}

FDelegateHandle FEOSVoiceChat::StartRecording(const FOnVoiceChatRecordSamplesAvailableDelegate::FDelegate& Delegate)
{
	return GetVoiceChatUser().StartRecording(Delegate);
}

void FEOSVoiceChat::StopRecording(FDelegateHandle Handle)
{
	GetVoiceChatUser().StopRecording(Handle);
}

FDelegateHandle FEOSVoiceChat::RegisterOnVoiceChatAfterCaptureAudioReadDelegate(const FOnVoiceChatAfterCaptureAudioReadDelegate::FDelegate& Delegate)
{
	return GetVoiceChatUser().RegisterOnVoiceChatAfterCaptureAudioReadDelegate(Delegate);
}

void FEOSVoiceChat::UnregisterOnVoiceChatAfterCaptureAudioReadDelegate(FDelegateHandle Handle)
{
	GetVoiceChatUser().UnregisterOnVoiceChatAfterCaptureAudioReadDelegate(Handle);
}

FDelegateHandle FEOSVoiceChat::RegisterOnVoiceChatBeforeCaptureAudioSentDelegate(const FOnVoiceChatBeforeCaptureAudioSentDelegate::FDelegate& Delegate)
{
	return GetVoiceChatUser().RegisterOnVoiceChatBeforeCaptureAudioSentDelegate(Delegate);
}

void FEOSVoiceChat::UnregisterOnVoiceChatBeforeCaptureAudioSentDelegate(FDelegateHandle Handle)
{
	GetVoiceChatUser().UnregisterOnVoiceChatBeforeCaptureAudioSentDelegate(Handle);
}

FDelegateHandle FEOSVoiceChat::RegisterOnVoiceChatBeforeRecvAudioRenderedDelegate(const FOnVoiceChatBeforeRecvAudioRenderedDelegate::FDelegate& Delegate)
{
	return GetVoiceChatUser().RegisterOnVoiceChatBeforeRecvAudioRenderedDelegate(Delegate);
}

void FEOSVoiceChat::UnregisterOnVoiceChatBeforeRecvAudioRenderedDelegate(FDelegateHandle Handle)
{
	GetVoiceChatUser().UnregisterOnVoiceChatBeforeRecvAudioRenderedDelegate(Handle);
}

FDelegateHandle FEOSVoiceChat::RegisterOnVoiceChatDataReceivedDelegate(const FOnVoiceChatDataReceivedDelegate::FDelegate& Delegate)
{
	return GetVoiceChatUser().RegisterOnVoiceChatDataReceivedDelegate(Delegate);
}

void FEOSVoiceChat::UnregisterOnVoiceChatDataReceivedDelegate(FDelegateHandle Handle)
{
	GetVoiceChatUser().UnregisterOnVoiceChatDataReceivedDelegate(Handle);
}

FString FEOSVoiceChat::InsecureGetLoginToken(const FString& PlayerName)
{
	return GetVoiceChatUser().InsecureGetLoginToken(PlayerName);
}

FString FEOSVoiceChat::InsecureGetJoinToken(const FString& ChannelName, EVoiceChatChannelType ChannelType, TOptional<FVoiceChatChannel3dProperties> Channel3dProperties)
{
	return GetVoiceChatUser().InsecureGetJoinToken(ChannelName, ChannelType, Channel3dProperties);
}
#pragma endregion IVoiceChatUser

void FEOSVoiceChat::BindInitCallbacks()
{
	EOS_RTCAudio_AddNotifyAudioDevicesChangedOptions AudioDevicesChangedOptions = {};
	AudioDevicesChangedOptions.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYAUDIODEVICESCHANGED_API_LATEST;
	static_assert(EOS_RTCAUDIO_ADDNOTIFYAUDIODEVICESCHANGED_API_LATEST == 1, "EOS_RTC_AddNotifyAudioDevicesChangedOptions updated, check new fields");
	InitSession.OnAudioDevicesChangedNotificationId = EOS_RTCAudio_AddNotifyAudioDevicesChanged(EOS_RTC_GetAudioInterface(InitSession.EosRtcInterface), &AudioDevicesChangedOptions, this, &FEOSVoiceChat::OnAudioDevicesChangedStatic);
	if (InitSession.OnAudioDevicesChangedNotificationId == EOS_INVALID_NOTIFICATIONID)
	{
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("BindInitCallbacks EOS_RTC_AddNotifyAudioDevicesChanged failed"));
	}

	OnAudioDevicesChanged();
}

void FEOSVoiceChat::UnbindInitCallbacks()
{
	if (InitSession.OnAudioDevicesChangedNotificationId != EOS_INVALID_NOTIFICATIONID)
	{
		EOS_RTCAudio_RemoveNotifyAudioDevicesChanged(EOS_RTC_GetAudioInterface(InitSession.EosRtcInterface), InitSession.OnAudioDevicesChangedNotificationId);
		InitSession.OnAudioDevicesChangedNotificationId = EOS_INVALID_NOTIFICATIONID;
	}
}

void FEOSVoiceChat::OnAudioDevicesChangedStatic(const EOS_RTCAudio_AudioDevicesChangedCallbackInfo* CallbackInfo)
{
	if (CallbackInfo)
	{
		if (FEOSVoiceChat* EosVoiceChatPtr = static_cast<FEOSVoiceChat*>(CallbackInfo->ClientData))
		{
			EosVoiceChatPtr->OnAudioDevicesChanged();
		}
		else
		{
			UE_LOG(LogEOSVoiceChat, Warning, TEXT("OnAudioDevicesChangedStatic Error EosVoiceChatPtr=nullptr"));
		}
	}
	else
	{
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("OnAudioDevicesChangedStatic Error CallbackInfo=nullptr"));
	}
}

void FEOSVoiceChat::OnAudioDevicesChanged()
{
	InitSession.CachedInputDeviceInfos = GetRtcInputDeviceInfos(InitSession.DefaultInputDeviceInfoIdx);
	InitSession.CachedOutputDeviceInfos = GetRtcOutputDeviceInfos(InitSession.DefaultOutputDeviceInfoIdx);

	UE_LOG(LogEOSVoiceChat, Verbose, TEXT("OnAudioDevicesChanged InputDeviceInfos=[%s] DefaultInputDeviceInfoIdx=%d"), *FString::JoinBy(InitSession.CachedInputDeviceInfos, TEXT(", "), &FVoiceChatDeviceInfo::ToDebugString), InitSession.DefaultInputDeviceInfoIdx);
	UE_LOG(LogEOSVoiceChat, Verbose, TEXT("OnAudioDevicesChanged OutputDeviceInfos=[%s] DefaultOutputDeviceInfoIdx=%d"), *FString::JoinBy(InitSession.CachedOutputDeviceInfos, TEXT(", "), &FVoiceChatDeviceInfo::ToDebugString), InitSession.DefaultOutputDeviceInfoIdx);

	OnVoiceChatAvailableAudioDevicesChangedDelegate.Broadcast();
}

TArray<FVoiceChatDeviceInfo> FEOSVoiceChat::GetRtcInputDeviceInfos(int32& OutDefaultDeviceIdx) const
{
	OutDefaultDeviceIdx = -1;
	TArray<FVoiceChatDeviceInfo> InputDeviceInfos;
	EOS_HRTCAudio RTCAudioHandle = EOS_RTC_GetAudioInterface(InitSession.EosRtcInterface);

	EOS_RTCAudio_GetAudioInputDevicesCountOptions CountOptions = {};
	CountOptions.ApiVersion = EOS_RTCAUDIO_GETAUDIOINPUTDEVICESCOUNT_API_LATEST;
 	static_assert(EOS_RTCAUDIO_GETAUDIOINPUTDEVICESCOUNT_API_LATEST == 1, "EOS_RTCAudio_GetAudioInputDevicesCountOptions updated, check new fields");

	uint32_t Count = EOS_RTCAudio_GetAudioInputDevicesCount(RTCAudioHandle, &CountOptions);

	for (uint32_t Index = 0; Index < Count; Index++)
	{
		EOS_RTCAudio_GetAudioInputDeviceByIndexOptions GetByIndexOptions = {};
		GetByIndexOptions.ApiVersion = EOS_RTCAUDIO_GETAUDIOINPUTDEVICEBYINDEX_API_LATEST;
		GetByIndexOptions.DeviceInfoIndex = Index;
		if (const EOS_RTCAudio_AudioInputDeviceInfo* DeviceInfo = EOS_RTCAudio_GetAudioInputDeviceByIndex(RTCAudioHandle, &GetByIndexOptions))
		{
			FString DeviceName = UTF8_TO_TCHAR(DeviceInfo->DeviceName);
			if (DeviceName != TEXT("Default Device"))
			{
				FVoiceChatDeviceInfo& InputDeviceInfo = InputDeviceInfos.Emplace_GetRef();
				InputDeviceInfo.DisplayName = MoveTemp(DeviceName);
				InputDeviceInfo.Id = UTF8_TO_TCHAR(DeviceInfo->DeviceId);
				if (DeviceInfo->bDefaultDevice)
				{
					OutDefaultDeviceIdx = InputDeviceInfos.Num() - 1;
				}
			}
		}
		else
		{
			UE_LOG(LogEOSVoiceChat, Warning, TEXT("EOS_RTCAudio_GetAudioInputDeviceByIndex failed: DevicesInfo=nullptr"));
		}
	}

	if (Count == 0)
	{
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("EOS_RTCAudio_GetAudioInputDevicesCount failed: DevicesCount=0"));
	}

	return InputDeviceInfos;
}

TArray<FVoiceChatDeviceInfo> FEOSVoiceChat::GetRtcOutputDeviceInfos(int32& OutDefaultDeviceIdx) const
{
	OutDefaultDeviceIdx = -1;
	TArray<FVoiceChatDeviceInfo> OutputDeviceInfos;
	EOS_HRTCAudio RTCAudioHandle = EOS_RTC_GetAudioInterface(InitSession.EosRtcInterface);

	EOS_RTCAudio_GetAudioOutputDevicesCountOptions CountOptions = {};
	CountOptions.ApiVersion = EOS_RTCAUDIO_GETAUDIOOUTPUTDEVICESCOUNT_API_LATEST;
	static_assert(EOS_RTCAUDIO_GETAUDIOOUTPUTDEVICESCOUNT_API_LATEST == 1, "EOS_RTCAudio_GetAudioOutputDevicesCountOptions updated, check new fields");

	uint32_t Count = EOS_RTCAudio_GetAudioOutputDevicesCount(RTCAudioHandle, &CountOptions);

	for (uint32_t Index = 0; Index < Count; Index++)
	{
		EOS_RTCAudio_GetAudioOutputDeviceByIndexOptions GetByIndexOptions = {};
		GetByIndexOptions.ApiVersion = EOS_RTCAUDIO_GETAUDIOOUTPUTDEVICEBYINDEX_API_LATEST;
		GetByIndexOptions.DeviceInfoIndex = Index;
		if (const EOS_RTCAudio_AudioOutputDeviceInfo* DeviceInfo = EOS_RTCAudio_GetAudioOutputDeviceByIndex(RTCAudioHandle, &GetByIndexOptions))
		{
			FString DeviceName = UTF8_TO_TCHAR(DeviceInfo->DeviceName);
			if (DeviceName != TEXT("Default Device"))
			{
				FVoiceChatDeviceInfo& InputDeviceInfo = OutputDeviceInfos.Emplace_GetRef();
				InputDeviceInfo.DisplayName = MoveTemp(DeviceName);
				InputDeviceInfo.Id = UTF8_TO_TCHAR(DeviceInfo->DeviceId);
				if (DeviceInfo->bDefaultDevice)
				{
					OutDefaultDeviceIdx = OutputDeviceInfos.Num() - 1;
				}
			}
		}
		else
		{
			UE_LOG(LogEOSVoiceChat, Warning, TEXT("EOS_RTCAudio_GetAudioOutputDeviceByIndex failed: DevicesInfo=nullptr"));
		}
	}

	if (Count == 0)
	{
		UE_LOG(LogEOSVoiceChat, Warning, TEXT("EOS_RTCAudio_GetAudioOutputDevicesCount failed: DevicesCount=0"));
	}

	return OutputDeviceInfos;
}

FEOSVoiceChatUser& FEOSVoiceChat::GetVoiceChatUser()
{
	if (!SingleUserVoiceChatUser)
	{
		SingleUserVoiceChatUser = static_cast<FEOSVoiceChatUser*>(CreateUser());
		ensureMsgf(VoiceChatUsers.Num() == 1, TEXT("When using multiple users, all connections should be managed by an IVoiceChatUser"));
	}

	return *SingleUserVoiceChatUser;
}

FEOSVoiceChatUser& FEOSVoiceChat::GetVoiceChatUser() const
{
	return const_cast<FEOSVoiceChat*>(this)->GetVoiceChatUser();
}

bool FEOSVoiceChat::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
#if !NO_LOGGING
#define EOS_EXEC_LOG(Fmt, ...) Ar.CategorizedLogf(LogEOSVoiceChat.GetCategoryName(), ELogVerbosity::Log, Fmt, ##__VA_ARGS__)
#else
#define EOS_EXEC_LOG(Fmt, ...) 
#endif

	if (FParse::Command(&Cmd, TEXT("EOSVOICECHAT")))
	{
		const TCHAR* SubCmd = Cmd;
		if (FParse::Command(&Cmd, TEXT("LIST")))
		{
			EOS_EXEC_LOG(TEXT("InstanceId=%d Users=%d"), InstanceId, VoiceChatUsers.Num());
			if (VoiceChatUsers.Num() > 0)
			{
				for (int UserIndex = 0; UserIndex < VoiceChatUsers.Num(); ++UserIndex)
				{
					const FEOSVoiceChatUserRef& User = VoiceChatUsers[UserIndex];
					EOS_EXEC_LOG(TEXT("  EOSUser Index:%i PlayerName:%s"), UserIndex, *User->GetLoggedInPlayerName());
				}
			}
			return true;
		}

		int64 InstanceIdParam = 0;
		FParse::Value(Cmd, TEXT("InstanceId="), InstanceIdParam);
		if (InstanceIdParam == InstanceId)
		{
			if (FParse::Command(&Cmd, TEXT("INFO")))
			{
				EOS_EXEC_LOG(TEXT("Initialized: %s"), *LexToString(IsInitialized()));
				if (IsInitialized())
				{
					EOS_EXEC_LOG(TEXT("Connection Status: %s"), LexToString(ConnectionState));

					for (int UserIndex = 0; UserIndex < VoiceChatUsers.Num(); ++UserIndex)
					{
						const FEOSVoiceChatUserRef& User = VoiceChatUsers[UserIndex];
						EOS_EXEC_LOG(TEXT("  User Index:%i PlayerName:%s"), UserIndex, *User->GetLoggedInPlayerName());
						User->Exec(InWorld, SubCmd, Ar);
					}
				}
				return true;
			}
	#if !UE_BUILD_SHIPPING
			else if (FParse::Command(&Cmd, TEXT("INITIALIZE")))
			{
				Initialize(FOnVoiceChatInitializeCompleteDelegate::CreateLambda([](const FVoiceChatResult& Result)
				{
					UE_LOG(LogEOSVoiceChat, Display, TEXT("EOS INITIALIZE success:%s"), *LexToString(Result));
				}));
				return true;
			}
			else if (FParse::Command(&Cmd, TEXT("UNINITIALIZE")))
			{
				Uninitialize(FOnVoiceChatUninitializeCompleteDelegate::CreateLambda([](const FVoiceChatResult& Result)
				{
					UE_LOG(LogEOSVoiceChat, Display, TEXT("EOS UNINITIALIZE success:%s"), *LexToString(Result));
				}));
				return true;
			}
			else if (FParse::Command(&Cmd, TEXT("CONNECT")))
			{
				Connect(FOnVoiceChatConnectCompleteDelegate::CreateLambda([](const FVoiceChatResult& Result)
				{
					UE_LOG(LogEOSVoiceChat, Display, TEXT("EOS CONNECT result:%s"), *LexToString(Result));
				}));
				return true;
			}
			else if (FParse::Command(&Cmd, TEXT("DISCONNECT")))
			{
				Disconnect(FOnVoiceChatDisconnectCompleteDelegate::CreateLambda([](const FVoiceChatResult& Result)
				{
					UE_LOG(LogEOSVoiceChat, Display, TEXT("EOS DISCONNECT result:%s"), *LexToString(Result));
				}));
				return true;
			}
			else if (FParse::Command(&Cmd, TEXT("CREATEUSER")))
			{
				if (!SingleUserVoiceChatUser)
				{
					UsersCreatedByConsoleCommand.Add(CreateUser());
					EOS_EXEC_LOG(TEXT("EOS CREATEUSER success"));
					return true;
				}
				else
				{
					EOS_EXEC_LOG(TEXT("EOS CREATEUSER failed, single user set."));
					return true;
				}
			}
			else if (FParse::Command(&Cmd, TEXT("CREATESINGLEUSER")))
			{
				if (SingleUserVoiceChatUser)
				{
					EOS_EXEC_LOG(TEXT("EOS CREATESINGLEUSER already exists"));
					return true;
				}
				else if (VoiceChatUsers.Num() == 0)
				{
					GetVoiceChatUser();
					EOS_EXEC_LOG(TEXT("EOS CREATESINGLEUSER success"));
					return true;
				}
				else
				{
					EOS_EXEC_LOG(TEXT("EOS CREATESINGLEUSER failed, VoiceChatUsers not empty."));
					return true;
				}
			}
			else
			{	
				int UserIndex = 0;
				if (FParse::Value(Cmd, TEXT("UserIndex="), UserIndex))
				{
					if (UserIndex < VoiceChatUsers.Num())
					{
						const FEOSVoiceChatUserRef& UserRef = VoiceChatUsers[UserIndex];
						if (FParse::Command(&Cmd, TEXT("RELEASEUSER")))
						{
							IVoiceChatUser* User = &UserRef.Get();
							if (UsersCreatedByConsoleCommand.RemoveSwap(User))
							{
							EOS_EXEC_LOG(TEXT("EOS RELEASEUSER releasing UserIndex=%d..."), UserIndex);
								ReleaseUser(User);
							}
							else
							{
								EOS_EXEC_LOG(TEXT("EOS RELEASEUSER UserIndex=%d not created by CREATEUSER call."), UserIndex);
							}
							return true;
						}
						else
						{
							return UserRef->Exec(InWorld, Cmd, Ar);
						}
					}
					else
					{
						EOS_EXEC_LOG(TEXT("EOS RELEASEUSER UserIndex=%d not found, VoiceChatUsers.Num=%d"), UserIndex, VoiceChatUsers.Num());
						return true;
					}
				}
				else if (SingleUserVoiceChatUser)
				{
					return SingleUserVoiceChatUser->Exec(InWorld, SubCmd, Ar);
				}
				else
				{
					EOS_EXEC_LOG(TEXT("EOS User index not specified, and no single user created. Either CREATEUSER and specify UserIndex=n in subsequent commands, or CREATESINGLEUSER (no UserIndex=n necessary in subsequent commands)"));
					return true;
				}
			}
#endif // !UE_BUILD_SHIPPING
		}
	}

#undef EOS_EXEC_LOG

	return false;
}

IEOSPlatformHandlePtr FEOSVoiceChat::EOSPlatformCreate(EOS_Platform_Options& PlatformOptions)
{
	return SDKManager.CreatePlatform(PlatformOptions);
}

FEOSVoiceChatWeakPtr FEOSVoiceChat::CreateWeakThis()
{
	return FEOSVoiceChatWeakPtr(AsShared());
}

const TCHAR* LexToString(FEOSVoiceChat::EConnectionState State)
{
	switch (State)
	{
	case FEOSVoiceChat::EConnectionState::Disconnected:		return TEXT("Disconnected");
	case FEOSVoiceChat::EConnectionState::Disconnecting:	return TEXT("Disconnecting");
	case FEOSVoiceChat::EConnectionState::Connecting:		return TEXT("Connecting");
	case FEOSVoiceChat::EConnectionState::Connected:		return TEXT("Connected");
	default:												return TEXT("Unknown");
	}
}

#undef CHECKPIN

#endif // WITH_EOS_RTC