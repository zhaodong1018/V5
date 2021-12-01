// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/OnlineId.h"
#include "Templates/SharedPointer.h"

class FString;

namespace UE::Online {

// Interfaces
using IAuthPtr = TSharedPtr<class IAuth>;
using IFriendsPtr = TSharedPtr<class IFriends>;
using IPresencePtr = TSharedPtr<class IPresence>;
using IExternalUIPtr = TSharedPtr<class IExternalUI>;

class ONLINESERVICESINTERFACE_API IOnlineServices
{
public:
	/**
	 *
	 */
	virtual void Init() = 0;

	/**
	 *
	 */
	virtual void Destroy() = 0;
	
	/**
	 *
	 */
	virtual IAuthPtr GetAuthInterface() = 0;

	/**
	 *
	 */
	virtual IFriendsPtr GetFriendsInterface() = 0;

	/**
	 *
	 */
	virtual IPresencePtr GetPresenceInterface() = 0;

	/**
	 *
	 */
	virtual IExternalUIPtr GetExternalUIInterface() = 0;

	/**
	 * 
	 */
	virtual FString ToLogString(const FOnlineAccountIdHandle& Handle) = 0;
};

/**
 * Get an instance of the online subsystem
 *
 * @param OnlineServices Type of online services to retrieve
 * @param InstanceName Name of the services instance to retrieve
 * @return The services instance or an invalid pointer if the services is unavailable
 */
ONLINESERVICESINTERFACE_API TSharedPtr<IOnlineServices> GetServices(EOnlineServices OnlineServices = EOnlineServices::Default, FName InstanceName = NAME_None);

/**
 * Get a specific services type and cast to the specific services type
 *
 * @param InstanceName Name of the services instance to retrieve
 * @return The services instance or an invalid pointer if the services is unavailable
 */
template <typename ServicesClass>
TSharedPtr<ServicesClass> GetServices(FName InstanceName = NAME_None)
{
	return StaticCastSharedPtr<ServicesClass>(GetServices(ServicesClass::GetServicesProvider(), InstanceName));
}

/**
 * Destroy an instance of the online subsystem
 *
 * @param OnlineServices Type of online services to destroy
 * @param InstanceName Name of the services instance to destroy
 */
ONLINESERVICESINTERFACE_API void DestroyServices(EOnlineServices OnlineServices = EOnlineServices::Default, FName InstanceName = NAME_None);

template<typename IdType>
inline FString ToLogString(const TOnlineIdHandle<IdType>& Id)
{
	FString Result;
	if (TSharedPtr<IOnlineServices> Services = GetServices(Id.GetOnlineServicesType()))
	{
		Result = Services->ToLogString(Id);
	}
	return Result;
}

/* UE::Online */ }
