// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosCloth/ChaosClothingSimulationInteractor.h"
#include "ChaosCloth/ChaosClothingSimulationCloth.h"
#include "ChaosCloth/ChaosClothingSimulation.h"

using namespace Chaos;

namespace ChaosClothingInteractor
{
	static const float InvStiffnessLogBase = 1.f / FMath::Loge(1.e3f);  // Log base for updating old linear stiffnesses to the new stiffness exponentiation
}

void UChaosClothingInteractor::Sync(IClothingSimulation* Simulation)
{
	check(Simulation);

	if (FClothingSimulationCloth* const Cloth = static_cast<FClothingSimulation*>(Simulation)->GetCloth(ClothingId))
	{
		for (FChaosClothingInteractorCommand& Command : Commands)
		{
			Command.Execute(Cloth);
		}
		Commands.Reset();
	}

	// Call to base class' sync
	UClothingInteractor::Sync(Simulation);
}

void UChaosClothingInteractor::SetMaterialLinear(float EdgeStiffnessLinear, float BendingStiffnessLinear, float AreaStiffnessLinear)
{
	const TVec2<FRealSingle> EdgeStiffness((FMath::Clamp(FMath::Loge(EdgeStiffnessLinear) * ChaosClothingInteractor::InvStiffnessLogBase + 1.f, 0.f, 1.f)), 1.f);
	const TVec2<FRealSingle> BendingStiffness((FMath::Clamp(FMath::Loge(BendingStiffnessLinear) * ChaosClothingInteractor::InvStiffnessLogBase + 1.f, 0.f, 1.f)), 1.f);
	const TVec2<FRealSingle> AreaStiffness((FMath::Clamp(FMath::Loge(AreaStiffnessLinear) * ChaosClothingInteractor::InvStiffnessLogBase + 1.f, 0.f, 1.f)), 1.f);

	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([EdgeStiffness, BendingStiffness, AreaStiffness](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetMaterialProperties(EdgeStiffness, BendingStiffness, AreaStiffness);
	}));
}

void UChaosClothingInteractor::SetMaterial(FVector2D EdgeStiffness, FVector2D BendingStiffness, FVector2D AreaStiffness)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([EdgeStiffness, BendingStiffness, AreaStiffness](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetMaterialProperties(TVec2<FRealSingle>(EdgeStiffness[0], EdgeStiffness[1]), TVec2<FRealSingle>(BendingStiffness[0], BendingStiffness[1]), TVec2<FRealSingle>(AreaStiffness[0], AreaStiffness[1]));
	}));
}

void UChaosClothingInteractor::SetLongRangeAttachmentLinear(float TetherStiffnessLinear, float TetherScale)
{
	// Deprecated
	const TVec2<FRealSingle> TetherStiffness((FMath::Clamp(FMath::Loge(TetherStiffnessLinear) * ChaosClothingInteractor::InvStiffnessLogBase + 1.f, 0.f, 1.f)), 1.f);
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([TetherStiffness, TetherScale](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetLongRangeAttachmentProperties(TetherStiffness, TVec2<FRealSingle>(TetherScale, TetherScale));
	}));
}

void UChaosClothingInteractor::SetLongRangeAttachment(FVector2D TetherStiffness, FVector2D TetherScale)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([TetherStiffness, TetherScale](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetLongRangeAttachmentProperties(
			TVec2<FRealSingle>(TetherStiffness[0], TetherStiffness[1]),
			TVec2<FRealSingle>(TetherScale[0], TetherScale[1]));
	}));
}

void UChaosClothingInteractor::SetCollision(float CollisionThickness, float FrictionCoefficient, bool bUseCCD, float SelfCollisionThickness)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([CollisionThickness, FrictionCoefficient, bUseCCD, SelfCollisionThickness](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetCollisionProperties(CollisionThickness, FrictionCoefficient, bUseCCD, SelfCollisionThickness);
	}));
}

void UChaosClothingInteractor::SetBackstop(bool bEnabled)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([bEnabled](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetBackstopProperties(bEnabled);
	}));
}
void UChaosClothingInteractor::SetDamping(float DampingCoefficient)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([DampingCoefficient](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetDampingProperties(DampingCoefficient);
	}));
}

void UChaosClothingInteractor::SetAerodynamics(float DragCoefficient, float LiftCoefficient, FVector WindVelocity)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([DragCoefficient, LiftCoefficient, WindVelocity](FClothingSimulationCloth* Cloth)
	{
		constexpr FRealSingle AirDensity = 1.225e-6f;
		Cloth->SetAerodynamicsProperties(TVec2<FRealSingle>(DragCoefficient, DragCoefficient), TVec2<FRealSingle>(LiftCoefficient, LiftCoefficient), AirDensity, WindVelocity);
	}));
}

void UChaosClothingInteractor::SetWind(FVector2D Drag, FVector2D Lift, float AirDensity, FVector WindVelocity)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([Drag, Lift, AirDensity, WindVelocity](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetAerodynamicsProperties(TVec2<FRealSingle>(Drag[0], Drag[1]), TVec2<FRealSingle>(Lift[0], Lift[1]), AirDensity, WindVelocity);
	}));
}

void UChaosClothingInteractor::SetGravity(float GravityScale, bool bIsGravityOverridden, FVector GravityOverride)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([GravityScale, bIsGravityOverridden, GravityOverride](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetGravityProperties(GravityScale, bIsGravityOverridden, GravityOverride);
	}));
}

void UChaosClothingInteractor::SetAnimDriveLinear(float AnimDriveStiffnessLinear)
{
	// Deprecated
	const TVec2<FRealSingle> AnimDriveStiffness(0.f, FMath::Clamp(FMath::Loge(AnimDriveStiffnessLinear) * ChaosClothingInteractor::InvStiffnessLogBase + 1.f, 0.f, 1.f));
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([AnimDriveStiffness](FClothingSimulationCloth* Cloth)
	{
		// The Anim Drive stiffness Low value needs to be 0 in order to keep backward compatibility with existing mask (this wouldn't be an issue if this property had no legacy mask)
		static const TVec2<FRealSingle> AnimDriveDamping(0.f, 1.f);
		Cloth->SetAnimDriveProperties(AnimDriveStiffness, AnimDriveDamping);
	}));
}

void UChaosClothingInteractor::SetAnimDrive(FVector2D AnimDriveStiffness, FVector2D AnimDriveDamping)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([AnimDriveStiffness, AnimDriveDamping](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetAnimDriveProperties(TVec2<FRealSingle>(AnimDriveStiffness.X, AnimDriveStiffness.Y), TVec2<FRealSingle>(AnimDriveDamping.X, AnimDriveDamping.Y));
	}));
}

void UChaosClothingInteractor::SetVelocityScale(FVector LinearVelocityScale, float AngularVelocityScale, float FictitiousAngularScale)
{
	Commands.Add(FChaosClothingInteractorCommand::CreateLambda([LinearVelocityScale, AngularVelocityScale, FictitiousAngularScale](FClothingSimulationCloth* Cloth)
	{
		Cloth->SetVelocityScaleProperties(LinearVelocityScale, AngularVelocityScale, FictitiousAngularScale);
	}));
}

void UChaosClothingInteractor::ResetAndTeleport(bool bReset, bool bTeleport)
{
	if (bReset)
	{
		Commands.Add(FChaosClothingInteractorCommand::CreateLambda([](FClothingSimulationCloth* Cloth)
		{
			Cloth->Reset();
		}));
	}
	if (bTeleport)
	{
		Commands.Add(FChaosClothingInteractorCommand::CreateLambda([](FClothingSimulationCloth* Cloth)
		{
			Cloth->Teleport();
		}));
	}
}

void UChaosClothingSimulationInteractor::Sync(IClothingSimulation* Simulation, IClothingSimulationContext* Context)
{
	check(Simulation);
	check(Context);

	for (FChaosClothingSimulationInteractorCommand& Command : Commands)
	{
		Command.Execute(static_cast<FClothingSimulation*>(Simulation), static_cast<FClothingSimulationContext*>(Context));
	}
	Commands.Reset();

	// Call base class' sync 
	UClothingSimulationInteractor::Sync(Simulation, Context);
}

void UChaosClothingSimulationInteractor::PhysicsAssetUpdated()
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([](FClothingSimulation* Simulation, FClothingSimulationContext* /*Context*/)
	{
		Simulation->RefreshPhysicsAsset();
	}));
}

void UChaosClothingSimulationInteractor::ClothConfigUpdated()
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([](FClothingSimulation* Simulation, FClothingSimulationContext* Context)
	{
		Simulation->RefreshClothConfig(Context);
	}));
}

void UChaosClothingSimulationInteractor::SetAnimDriveSpringStiffness(float Stiffness)
{
	// Set the anim drive stiffness through the ChaosClothInteractor to allow the value to be overridden by the cloth interactor if needed
	for (const auto& ClothingInteractor : UClothingSimulationInteractor::ClothingInteractors)
	{
		if (UChaosClothingInteractor* const ChaosClothingInteractor = Cast<UChaosClothingInteractor>(ClothingInteractor.Value))
		{
			ChaosClothingInteractor->SetAnimDriveLinear(Stiffness);
		}
	}
}

void UChaosClothingSimulationInteractor::EnableGravityOverride(const FVector& Gravity)
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([Gravity](FClothingSimulation* Simulation, FClothingSimulationContext* /*Context*/)
	{
		Simulation->SetGravityOverride(Gravity);
	}));
}

void UChaosClothingSimulationInteractor::DisableGravityOverride()
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([](FClothingSimulation* Simulation, FClothingSimulationContext* /*Context*/)
	{
		Simulation->DisableGravityOverride();
	}));
}

void UChaosClothingSimulationInteractor::SetNumIterations(int32 InNumIterations)
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([InNumIterations](FClothingSimulation* Simulation, FClothingSimulationContext* /*Context*/)
	{
		Simulation->SetNumIterations(InNumIterations);
	}));
}

void UChaosClothingSimulationInteractor::SetNumSubsteps(int32 InNumSubsteps)
{
	Commands.Add(FChaosClothingSimulationInteractorCommand::CreateLambda([InNumSubsteps](FClothingSimulation* Simulation, FClothingSimulationContext* /*Context*/)
	{
		Simulation->SetNumSubsteps(InNumSubsteps);
	}));
}

UClothingInteractor* UChaosClothingSimulationInteractor::CreateClothingInteractor()
{
	return NewObject<UChaosClothingInteractor>(this);
}
