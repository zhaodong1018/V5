// Copyright Epic Games, Inc. All Rights Reserved.

#include "Policy/Camera/DisplayClusterProjectionCameraPolicy.h"

#include "DisplayClusterProjectionLog.h"
#include "DisplayClusterProjectionStrings.h"

#include "Game/IDisplayClusterGameManager.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "ComposurePostMoves.h"

#include "Render/Viewport/IDisplayClusterViewport.h"
#include "Render/Viewport/IDisplayClusterViewportManager.h"

FDisplayClusterProjectionCameraPolicy::FDisplayClusterProjectionCameraPolicy(const FString& ProjectionPolicyId, const struct FDisplayClusterConfigurationProjection* InConfigurationProjectionPolicy)
	: FDisplayClusterProjectionPolicyBase(ProjectionPolicyId, InConfigurationProjectionPolicy)
{
}

FDisplayClusterProjectionCameraPolicy::~FDisplayClusterProjectionCameraPolicy()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////
// IDisplayClusterProjectionPolicy
//////////////////////////////////////////////////////////////////////////////////////////////
bool FDisplayClusterProjectionCameraPolicy::HandleStartScene(class IDisplayClusterViewport* InViewport)
{
	check(IsInGameThread());

	return true;
}

void FDisplayClusterProjectionCameraPolicy::HandleEndScene(class IDisplayClusterViewport* InViewport)
{
	check(IsInGameThread());

	CameraRef.ResetSceneComponent();
}

APlayerCameraManager* const GetCurPlayerCameraManager(IDisplayClusterViewport* InViewport)
{
	if (InViewport)
	{
	UWorld* World = InViewport->GetOwner().GetCurrentWorld();
	if (World)
	{
		APlayerController* const CurPlayerController = World->GetFirstPlayerController();
		if (CurPlayerController)
		{
			return CurPlayerController->PlayerCameraManager;
		}
	}
	}

	return nullptr;
}

UCameraComponent* FDisplayClusterProjectionCameraPolicy::GetCameraComponent()
{
		USceneComponent* SceneComponent = CameraRef.GetOrFindSceneComponent();
		if (SceneComponent)
		{
			UCameraComponent* CameraComponent = Cast<UCameraComponent>(SceneComponent);
			if (CameraComponent)
			{
				return CameraComponent;
			}
		}
	
	return  nullptr;
}

bool FDisplayClusterProjectionCameraPolicy::CalculateView(class IDisplayClusterViewport* InViewport, const uint32 InContextNum, FVector& InOutViewLocation, FRotator& InOutViewRotation, const FVector& ViewOffset, const float WorldToMeters, const float NCP, const float FCP)
{
	check(IsInGameThread());

	InOutViewLocation = FVector::ZeroVector;
	InOutViewRotation = FRotator::ZeroRotator;

	// Save Z values
	ZNear = NCP;
	ZFar  = FCP;
	
	// Use transform of an assigned camera
	if (UCameraComponent* CameraComponent = GetCameraComponent())
	{
		InOutViewLocation = CameraComponent->GetComponentLocation();
		InOutViewRotation = CameraComponent->GetComponentRotation();
	}
	// Otherwise default UE camera is used
	else
	{
		APlayerCameraManager* const CurPlayerCameraManager = GetCurPlayerCameraManager(InViewport);
				if (CurPlayerCameraManager)
				{
					InOutViewLocation = CurPlayerCameraManager->GetCameraLocation();
					InOutViewRotation = CurPlayerCameraManager->GetCameraRotation();
				}
			}

	// Fix camera lens deffects (prototype)
	InOutViewLocation += CameraSettings.FrustumOffset;
	InOutViewRotation += CameraSettings.FrustumRotation;

	return true;
}

bool FDisplayClusterProjectionCameraPolicy::ImplGetProjectionMatrix(const float InCameraFOV, const float InCameraAspectRatio, IDisplayClusterViewport* InViewport, const uint32 InContextNum, FMatrix& OutPrjMatrix)
{
	// The horizontal field of view (in degrees)
	const float ScaledCameraFOV = InCameraFOV * CameraSettings.FOVMultiplier;

	// Clamp camera fov to valid range [1.f, 178.f]
	const float ClampedCameraFOV = FMath::Clamp(ScaledCameraFOV, 1.f, 178.f);
	if (ClampedCameraFOV != ScaledCameraFOV && !IsEditorOperationMode())
	{
		UE_LOG(LogDisplayClusterProjectionCamera, Warning, TEXT("CameraFOV clamped: '%d' -> '%d'. (FieldOfView='%d', FOVMultiplier='%d'"), ScaledCameraFOV, ClampedCameraFOV, InCameraFOV, CameraSettings.FOVMultiplier);
	}

	if (InViewport)
	{
		// Support inner camera custom frustum
		const float HalfFOVH = 0.5 * ClampedCameraFOV;
		const float HalfFOVV = HalfFOVH / InCameraAspectRatio;

		InViewport->CalculateProjectionMatrix(InContextNum, -HalfFOVH, HalfFOVH, HalfFOVV, -HalfFOVV, ZNear, ZFar, true);
		OutPrjMatrix = InViewport->GetContexts()[InContextNum].ProjectionMatrix;
	}
	else
	{
		FComposurePostMoveSettings ComposureSettings;
		OutPrjMatrix = ComposureSettings.GetProjectionMatrix(ClampedCameraFOV, InCameraAspectRatio);
	}

	return true;
}

bool FDisplayClusterProjectionCameraPolicy::GetProjectionMatrix(IDisplayClusterViewport* InViewport, const uint32 InContextNum, FMatrix& OutPrjMatrix)
{
	check(IsInGameThread());

	if (UCameraComponent* CameraComponent = GetCameraComponent())
	{
		return ImplGetProjectionMatrix(CameraComponent->FieldOfView, CameraComponent->AspectRatio, InViewport, InContextNum, OutPrjMatrix);
	}
	else
	{
		APlayerCameraManager* const CurPlayerCameraManager = GetCurPlayerCameraManager(InViewport);
		if (CurPlayerCameraManager)
		{
			return ImplGetProjectionMatrix(CurPlayerCameraManager->GetFOVAngle(), CurPlayerCameraManager->DefaultAspectRatio, InViewport, InContextNum, OutPrjMatrix);
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// FDisplayClusterProjectionCameraPolicy
//////////////////////////////////////////////////////////////////////////////////////////////
void FDisplayClusterProjectionCameraPolicy::SetCamera(UCameraComponent* NewCamera, const FDisplayClusterProjectionCameraPolicySettings& InCameraSettings)
{
	if (NewCamera)
	{
		UE_LOG(LogDisplayClusterProjectionCamera, Verbose, TEXT("New camera set: %s"), *NewCamera->GetFullName());
		CameraRef.SetSceneComponent(NewCamera);
	}
	else
	{
		CameraRef.ResetSceneComponent();
		if (!IsEditorOperationMode())
		{
			UE_LOG(LogDisplayClusterProjectionCamera, Warning, TEXT("Trying to set nullptr camera pointer"));
		}
	}

	CameraSettings = InCameraSettings;
}
