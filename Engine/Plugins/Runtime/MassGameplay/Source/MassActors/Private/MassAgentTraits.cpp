// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassAgentTraits.h"
#include "MassCommonTypes.h"
#include "MassMovementFragments.h"
#include "MassTranslatorRegistry.h"
#include "MassEntityTemplate.h"
#include "MassEntityTemplateRegistry.h"
#include "Translators/MassCapsuleComponentTranslators.h"
#include "Translators/MassCharacterMovementTranslators.h"
#include "Translators/MassSceneComponentLocationTranslator.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "VisualLogger/VisualLogger.h"
#include "MassEntityView.h"

namespace FMassAgentTraitsHelper 
{
	template<typename T>
	T* AsComponent(UObject& Owner)
	{
		T* Component = nullptr;
		if (AActor* AsActor = Cast<AActor>(&Owner))
		{
			Component = AsActor->FindComponentByClass<T>();
		}
		else
		{
			Component = Cast<T>(&Owner);
		}

		UE_CVLOG_UELOG(Component == nullptr, &Owner, LogMass, Error, TEXT("Trying to extract %s from %s failed")
			, *T::StaticClass()->GetName(), *Owner.GetName());

		return Component;
	}
}

//----------------------------------------------------------------------//
//  UMassAgentCapsuleCollisionSyncTrait
//----------------------------------------------------------------------//
void UMassAgentCapsuleCollisionSyncTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FDataFragment_CapsuleComponentWrapper>();
	BuildContext.AddFragment<FDataFragment_AgentRadius>();
	if (bSyncTransform)
	{
		BuildContext.AddFragment<FDataFragment_Transform>();
	}
	
	BuildContext.GetMutableObjectFragmentInitializers().Add([=](UObject& Owner, FMassEntityView& EntityView, const EMassTranslationDirection CurrentDirection)
		{
			if (UCapsuleComponent* CapsuleComponent = FMassAgentTraitsHelper::AsComponent<UCapsuleComponent>(Owner))
			{
				FDataFragment_CapsuleComponentWrapper& CapsuleFragment = EntityView.GetFragmentData<FDataFragment_CapsuleComponentWrapper>();
				CapsuleFragment.Component = CapsuleComponent;

				FDataFragment_AgentRadius& RadiusFragment = EntityView.GetFragmentData<FDataFragment_AgentRadius>();
				RadiusFragment.Radius = CapsuleComponent->GetScaledCapsuleRadius();

				if (bSyncTransform)
				{
					FDataFragment_Transform& TransformFragment = EntityView.GetFragmentData<FDataFragment_Transform>();
					TransformFragment.GetMutableTransform() = CapsuleComponent->GetComponentTransform();
				}
			}
		});

	if (bSyncTransform)
	{
		if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::ActorToMass))
		{
			BuildContext.AddTranslator<UMassCapsuleTransformToMassTranslator>();
		}

		if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::MassToActor))
		{
			BuildContext.AddTranslator<UMassTransformToActorCapsuleTranslator>();
		}
	}
}

//----------------------------------------------------------------------//
//  UMassAgentMovementSyncTrait
//----------------------------------------------------------------------//
void UMassAgentMovementSyncTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FDataFragment_CharacterMovementComponentWrapper>();
	BuildContext.AddFragment<FMassVelocityFragment>();
	
	BuildContext.GetMutableObjectFragmentInitializers().Add([=](UObject& Owner, FMassEntityView& EntityView, const EMassTranslationDirection CurrentDirection)
		{
			if (UCharacterMovementComponent* MovementComp = FMassAgentTraitsHelper::AsComponent<UCharacterMovementComponent>(Owner))
			{
				FDataFragment_CharacterMovementComponentWrapper& ComponentFragment = EntityView.GetFragmentData<FDataFragment_CharacterMovementComponentWrapper>();
				ComponentFragment.Component = MovementComp;

				FMassVelocityFragment& VelocityFragment = EntityView.GetFragmentData<FMassVelocityFragment>();

				// the entity is the authority
				if (CurrentDirection ==  EMassTranslationDirection::MassToActor)
				{
					MovementComp->bRunPhysicsWithNoController = true;
					MovementComp->SetMovementMode(EMovementMode::MOVE_Walking);
					MovementComp->Velocity = VelocityFragment.Value;
				}
				// actor is the authority
				else
				{
					VelocityFragment.Value = MovementComp->GetLastUpdateVelocity();
				}
			}
		});

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::ActorToMass))
	{
		BuildContext.AddTranslator<UMassCharacterMovementToMassTranslator>();
	}

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::MassToActor))
	{
		BuildContext.AddTranslator<UMassCharacterMovementToActorTranslator>();
	}
}

//----------------------------------------------------------------------//
//  UMassAgentOrientationSyncTrait
//----------------------------------------------------------------------//
void UMassAgentOrientationSyncTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FDataFragment_CharacterMovementComponentWrapper>();
	// @todo: Figure out how we can share init with UMassAgentMovementSyncTrait, or make this this trait to depend on UMassAgentMovementSyncTrait.

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::ActorToMass))
	{
		BuildContext.AddTranslator<UMassCharacterOrientationToMassTranslator>();
	}

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::MassToActor))
	{
		BuildContext.AddTranslator<UMassCharacterOrientationToActorTranslator>();
	}
}

//----------------------------------------------------------------------//
//  UMassAgentFeetLocationSyncTrait
//----------------------------------------------------------------------//
void UMassAgentFeetLocationSyncTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FMassSceneComponentWrapperFragment>();
	BuildContext.AddFragment<FDataFragment_Transform>();

	BuildContext.GetMutableObjectFragmentInitializers().Add([=](UObject& Owner, FMassEntityView& EntityView, const EMassTranslationDirection CurrentDirection)
		{
			AActor* AsActor = Cast<AActor>(&Owner);
			if (AsActor && AsActor->GetRootComponent())
			{
				USceneComponent* Component = AsActor->GetRootComponent();
				FMassSceneComponentWrapperFragment& ComponentFragment = EntityView.GetFragmentData<FMassSceneComponentWrapperFragment>();
				ComponentFragment.Component = Component;

				FDataFragment_Transform& TransformFragment = EntityView.GetFragmentData<FDataFragment_Transform>();

				REDIRECT_OBJECT_TO_VLOG(Component, &Owner);
				UE_VLOG_LOCATION(&Owner, LogMass, Log, Component->GetComponentLocation(), 30, FColor::Yellow, TEXT("Initial component location"));
				UE_VLOG_LOCATION(&Owner, LogMass, Log, TransformFragment.GetTransform().GetLocation(), 30, FColor::Red, TEXT("Initial entity location"));

				// the entity is the authority
				if (CurrentDirection == EMassTranslationDirection::MassToActor)
				{
					// Temporary disabling this as it is already done earlier in the MassRepresentation and we needed to do a sweep to find he floor
					//Component->SetWorldLocation(FeetLocation, /*bSweep*/true, nullptr, ETeleportType::TeleportPhysics);
				}
				// actor is the authority
				else
				{
					TransformFragment.GetMutableTransform().SetLocation(Component->GetComponentTransform().GetLocation() - FVector(0.f, 0.f, Component->Bounds.BoxExtent.Z));
				}
			}
		});

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::ActorToMass))
	{
		BuildContext.AddTranslator<UMassSceneComponentLocationToMassTranslator>();
	}

	if (EnumHasAnyFlags(SyncDirection, EMassTranslationDirection::MassToActor))
	{
		BuildContext.AddTranslator<UMassSceneComponentLocationToActorTranslator>();
	}
}

