// Copyright Epic Games, Inc. All Rights Reserved.

#include "AudioGameplayVolume.h"
#include "AudioGameplayVolumeLogs.h"
#include "AudioGameplayVolumeProxy.h"
#include "AudioGameplayVolumeSubsystem.h"
#include "AudioGameplayVolumeComponent.h"
#include "AudioDevice.h"
#include "Components/BrushComponent.h"
#include "Engine/CollisionProfile.h"
#include "Net/UnrealNetwork.h"

AAudioGameplayVolume::AAudioGameplayVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (UBrushComponent* BrushComp = GetBrushComponent())
	{
		BrushComp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		BrushComp->SetGenerateOverlapEvents(false);
		BrushComp->bAlwaysCreatePhysicsState = true;
	}

	AGVComponent = CreateDefaultSubobject<UAudioGameplayVolumeProxyComponent>(TEXT("AGVComponent"));

#if WITH_EDITOR
	bColored = true;
	BrushColor = FColor(255, 255, 0, 255);
#endif // WITH_EDITOR
}

void AAudioGameplayVolume::SetEnabled(bool bNewEnabled)
{
	if (bNewEnabled != bEnabled)
	{
		bEnabled = bNewEnabled;
		if (CanSupportProxy())
		{
			AddProxy();
		}
		else
		{
			RemoveProxy();
		}
	}
}

#if WITH_EDITOR
void AAudioGameplayVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property && PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(AAudioGameplayVolume, bEnabled))
	{
		if (CanSupportProxy())
		{
			AddProxy();
		}
		else
		{
			RemoveProxy();
		}
	}
}
#endif // WITH_EDITOR

void AAudioGameplayVolume::GetLifetimeReplicatedProps(TArray< FLifetimeProperty > & OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAudioGameplayVolume, bEnabled);
}

void AAudioGameplayVolume::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	if (USceneComponent* SC = GetRootComponent())
	{
		SC->TransformUpdated.AddUObject(this, &AAudioGameplayVolume::TransformUpdated);
	}

	if (AGVComponent)
	{
		UAGVPrimitiveComponentProxy* PrimitiveComponentProxy = NewObject<UAGVPrimitiveComponentProxy>(AGVComponent);
		AGVComponent->SetProxy(PrimitiveComponentProxy);
	}

	if (CanSupportProxy())
	{
		AddProxy();
	}
}

void AAudioGameplayVolume::PostUnregisterAllComponents()
{
	RemoveProxy();

	// Component can be nulled due to GC at this point
	if (USceneComponent* SC = GetRootComponent())
	{
		SC->TransformUpdated.RemoveAll(this);
	}

	Super::PostUnregisterAllComponents();
}

void AAudioGameplayVolume::OnComponentDataChanged()
{
	if (CanSupportProxy())
	{
		UpdateProxy();
	}
}

bool AAudioGameplayVolume::CanSupportProxy() const
{
	if (!bEnabled || !AGVComponent || !AGVComponent->GetProxy())
	{
		return false;
	}

	return true;
}

void AAudioGameplayVolume::OnRep_bEnabled()
{
	if (CanSupportProxy())
	{
		AddProxy();
	}
	else
	{
		RemoveProxy();
	}
}

void AAudioGameplayVolume::TransformUpdated(USceneComponent* InRootComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	UpdateProxy();
}

void AAudioGameplayVolume::AddProxy() const
{
	if (UAudioGameplayVolumeSubsystem* VolumeSubsystem = GetSubsystem())
	{
		VolumeSubsystem->AddVolumeComponent(AGVComponent);
	}
}

void AAudioGameplayVolume::RemoveProxy() const
{
	if (UAudioGameplayVolumeSubsystem* VolumeSubsystem = GetSubsystem())
	{
		VolumeSubsystem->RemoveVolumeComponent(AGVComponent);
	}
}

void AAudioGameplayVolume::UpdateProxy() const
{
	if (UAudioGameplayVolumeSubsystem* VolumeSubsystem = GetSubsystem())
	{
		VolumeSubsystem->UpdateVolumeComponent(AGVComponent);
	}
}

UAudioGameplayVolumeSubsystem* AAudioGameplayVolume::GetSubsystem() const
{
	if (UWorld* World = GetWorld())
	{
		return FAudioDevice::GetSubsystem<UAudioGameplayVolumeSubsystem>(World->GetAudioDevice());
	}

	return nullptr;
}