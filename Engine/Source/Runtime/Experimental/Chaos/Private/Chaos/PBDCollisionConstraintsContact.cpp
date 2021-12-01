// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PBDCollisionConstraintsContact.h"
#include "Chaos/Collision/CollisionSolver.h"
#include "Chaos/Collision/PBDCollisionSolver.h"
#include "Chaos/CollisionResolution.h"
#include "Chaos/CollisionResolutionUtil.h"
#include "Chaos/Defines.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/Utilities.h"

#if INTEL_ISPC
#include "PBDCollisionConstraints.ispc.generated.h"
#endif

//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{
#if INTEL_ISPC
	extern bool bChaos_Collision_ISPC_Enabled;
#endif

	namespace CVars
	{
		int32 Chaos_Collision_EnergyClampEnabled = 1;
		FAutoConsoleVariableRef CVarChaosCollisionEnergyClampEnabled(TEXT("p.Chaos.Collision.EnergyClampEnabled"), Chaos_Collision_EnergyClampEnabled, TEXT("Whether to use energy clamping in collision apply step"));

		int32 Chaos_Collision_RelaxationEnabled = 0; // TODO remove this feature soon
		FAutoConsoleVariableRef CVarChaosCollisionRelaxationEnabled(TEXT("p.Chaos.Collision.RelaxationEnabled"), Chaos_Collision_RelaxationEnabled, TEXT("Whether to reduce applied impulses during iterations for improved solver stability but reduced convergence"));

		int32 Chaos_Collision_PrevVelocityRestitutionEnabled = 0;
		FAutoConsoleVariableRef CVarChaosCollisionPrevVelocityRestitutionEnabled(TEXT("p.Chaos.Collision.PrevVelocityRestitutionEnabled"), Chaos_Collision_PrevVelocityRestitutionEnabled, TEXT("If enabled restitution will be calculated on previous frame velocities instead of current frame velocities"));

		FRealSingle Chaos_Collision_ContactMovementAllowance = 0.05f;
		FAutoConsoleVariableRef CVarChaosCollisionContactMovementAllowance(TEXT("p.Chaos.Collision.AntiJitterContactMovementAllowance"), Chaos_Collision_ContactMovementAllowance,
			TEXT("If a contact is close to where it was during a previous iteration, we will assume it is the same contact that moved (to reduce jitter). Expressed as the fraction of movement distance and Centre of Mass distance to the contact point"));

		int32 Chaos_Collision_UseAccumulatedImpulseClipSolve = 0; // This requires multiple contact points per iteration per pair and contact points that don't move too much (in body space) to have an effect
		FAutoConsoleVariableRef CVarChaosCollisionImpulseClipSolve(TEXT("p.Chaos.Collision.UseAccumulatedImpulseClipSolve"), Chaos_Collision_UseAccumulatedImpulseClipSolve, TEXT("Use experimental Accumulated impulse clipped contact solve"));

		int32 Chaos_Collision_UseShockPropagation = 1;
		FAutoConsoleVariableRef CVarChaosCollisionUseShockPropagation(TEXT("p.Chaos.Collision.UseShockPropagation"), Chaos_Collision_UseShockPropagation, TEXT(""));

		FRealSingle Chaos_Collision_CollisionClipTolerance = 0.01f;
		FAutoConsoleVariableRef CVarChaosCollisionClipTolerance(TEXT("p.Chaos.Collision.ClipTolerance"), Chaos_Collision_CollisionClipTolerance, TEXT(""));

		bool Chaos_Collision_CheckManifoldComplete = false;
		FAutoConsoleVariableRef CVarChaosCollisionCheckManifoldComplete(TEXT("p.Chaos.Collision.CheckManifoldComplete"), Chaos_Collision_CheckManifoldComplete, TEXT(""));
	}
	using namespace CVars;

	namespace Collisions
	{
		void Update(FPBDCollisionConstraint& Constraint, const FReal Dt)
		{
			// NOTE: These are actor transforms, not CoM transforms
			// \todo(chaos): see if we can easily switch to CoM transforms now in collision loop (shapes are held in actor space)
			const FSolverBody& Body0 = *Constraint.GetSolverBody0();
			const FSolverBody& Body1 = *Constraint.GetSolverBody1();
			const FRigidTransform3 Transform0 = FRigidTransform3(Body0.ActorP(), Body0.ActorQ());
			const FRigidTransform3 Transform1 = FRigidTransform3(Body1.ActorP(), Body1.ActorQ());

			Constraint.ResetPhi(Constraint.GetCullDistance());
			UpdateConstraintFromGeometry<ECollisionUpdateType::Deepest>(Constraint, Transform0, Transform1, Dt);
		}

		void UpdateSwept(FPBDCollisionConstraint& Constraint, const FReal Dt)
		{
			// Note: This is unusual but we are using a mix of the previous and current transform
			// This is due to how CCD rewinds the position (not rotation) and then sweeps to find the first contact at the current orientation
			// NOTE: These are actor transforms, not CoM transforms
			// \todo(chaos): see if we can easily switch to CoM transforms now in collision loop (shapes are held in actor space)
			const FSolverBody& Body0 = *Constraint.GetSolverBody0();
			const FSolverBody& Body1 = *Constraint.GetSolverBody1();
			FGenericParticleHandle Particle0 = FGenericParticleHandle(Constraint.Particle[0]);
			const FRigidTransform3 TransformXQ0(Body0.X() - Body0.ActorQ().RotateVector(Particle0->CenterOfMass()), Body0.ActorQ());
			const FRigidTransform3 Transform1 = FRigidTransform3(Body1.ActorP(), Body1.ActorQ());

			Constraint.ResetPhi(Constraint.GetCullDistance());
			UpdateConstraintFromGeometrySwept<ECollisionUpdateType::Deepest>(Constraint, TransformXQ0, Transform1, Dt);
		}


		// A PBD collision penetration correction.
		// Currently only used by RBAN
		FVec3 ApplyContact2(FPBDCollisionConstraint& Constraint,
			const FContactIterationParameters& IterationParameters,
			const FContactParticleParameters& ParticleParameters)
		{
			FVec3 AccumulatedImpulse(0);

			FCollisionContact& Contact = Constraint.Manifold;
			FSolverBody& Body0 = *Constraint.GetSolverBody0();
			FSolverBody& Body1 = *Constraint.GetSolverBody1();

			FVec3 VectorToPoint0 = Contact.Location - Body0.P();
			FVec3 VectorToPoint1 = Contact.Location - Body1.P();

			if (Contact.Phi < 0)
			{
				*IterationParameters.NeedsAnotherIteration = true;

				bool bApplyResitution = (Contact.Restitution > 0.0f);
				bool bHaveRestitutionPadding = (Contact.RestitutionPadding > 0.0f);
				bool bApplyFriction = (Contact.Friction > 0) && (IterationParameters.Dt > SMALL_NUMBER);

				// If we have restitution, pad the constraint by an amount that enforces the outgoing velocity constraint
				// Really this should be per contact point, not per constraint.
				// NOTE: once we have calculated a padding, it is locked in for the rest of the iterations, and automatically
				// included in the Phi we get back from collision detection. The first time we calculate it, we must also
				// add the padding to the Phi (since it was from pre-padded collision detection).
				if (bApplyResitution && !bHaveRestitutionPadding)
				{
					FVec3 CV0 = Body0.V() + FVec3::CrossProduct(Body0.W(), VectorToPoint0);
					FVec3 CV1 = Body1.V() + FVec3::CrossProduct(Body1.W(), VectorToPoint1);
					FVec3 CV = CV0 - CV1;
					FReal CVNormal = FVec3::DotProduct(CV, Contact.Normal);

					// No restitution below threshold normal velocity (CVNormal is negative here)
					if (CVNormal < -ParticleParameters.RestitutionVelocityThreshold)
					{
						Contact.RestitutionPadding = -(1.0f + Contact.Restitution) * CVNormal * IterationParameters.Dt + Contact.Phi;
						Contact.Phi -= Contact.RestitutionPadding;
					}
				}
			
				FMatrix33 ContactInvI =
					(Body0.IsDynamic() ? ComputeFactorMatrix3(VectorToPoint0, Body0.InvI(), Body0.InvM()) : FMatrix33(0)) +
					(Body1.IsDynamic() ? ComputeFactorMatrix3(VectorToPoint1, Body1.InvI(), Body1.InvM()) : FMatrix33(0));

				// Calculate the normal correction
				FVec3 NormalError = Contact.Phi * Contact.Normal;
				FReal NormalImpulseDenominator = FVec3::DotProduct(Contact.Normal, ContactInvI * Contact.Normal);
				FVec3 NormalImpulseNumerator = -NormalError;
				FVec3 NormalCorrection = NormalImpulseNumerator / NormalImpulseDenominator;

				// Calculate lateral correction, clamped to the friction cone. Kinda.
				FVec3 LateralCorrection = FVec3(0);
				if (bApplyFriction)
				{
					// @todo(ccaulfield): use initial velocity (as for restitution) and accumulate friction force per contact point
					FVec3 V0 = FVec3::CalculateVelocity(Body0.X(), Body0.P(), IterationParameters.Dt);
					FVec3 W0 = FRotation3::CalculateAngularVelocity(Body0.R(), Body0.Q(), IterationParameters.Dt);
					FVec3 V1 = FVec3::CalculateVelocity(Body1.X(), Body1.P(), IterationParameters.Dt);
					FVec3 W1 = FRotation3::CalculateAngularVelocity(Body1.R(), Body1.Q(), IterationParameters.Dt);
					FVec3 CV0 = V0 + FVec3::CrossProduct(W0, VectorToPoint0);
					FVec3 CV1 = V1 + FVec3::CrossProduct(W1, VectorToPoint1);
					FVec3 CV = CV0 - CV1;
					FReal CVNormal = FVec3::DotProduct(CV, Contact.Normal);
					if (CVNormal < 0.0f)
					{
						FVec3 CVLateral = CV - CVNormal * Contact.Normal;
						FReal CVLateralMag = CVLateral.Size();
						if (CVLateralMag > KINDA_SMALL_NUMBER)
						{
							FVec3 DirLateral = CVLateral / CVLateralMag;
							FVec3 LateralImpulseNumerator = -CVLateral * IterationParameters.Dt;
							FReal LateralImpulseDenominator = FVec3::DotProduct(DirLateral, ContactInvI * DirLateral);
							LateralCorrection = LateralImpulseNumerator / LateralImpulseDenominator;
							FReal LateralImpulseMag = LateralCorrection.Size();
							FReal NormalImpulseMag = NormalCorrection.Size();
							if (LateralImpulseMag > Contact.Friction * NormalImpulseMag)
							{
								LateralCorrection *= Contact.Friction * NormalImpulseMag / LateralImpulseMag;
							}
						}
					}
				}

				// Net Correction
				FVec3 DX = NormalCorrection + LateralCorrection;
				
				if (Body0.IsDynamic())
				{
					FVec3 DP0 = Body0.InvM() * DX;
					FVec3 DR0 = Utilities::Multiply(Body0.InvI(), FVec3::CrossProduct(VectorToPoint0, DX));
					Body0.ApplyTransformDelta(DP0, DR0);
					Body0.UpdateRotationDependentState();
				}
				if (Body1.IsDynamic())
				{
					FVec3 DP1 = Body1.InvM() * -DX;
					FVec3 DR1 = Utilities::Multiply(Body1.InvI(), FVec3::CrossProduct(VectorToPoint1, -DX));
					Body1.ApplyTransformDelta(DP1, DR1);
					Body1.UpdateRotationDependentState();
				}
			}
			return AccumulatedImpulse;
		}


		template<typename T_CONSTRAINT>
		void ApplyImpl(T_CONSTRAINT& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			for (int32 PairIt = 0; PairIt < IterationParameters.NumPairIterations; ++PairIt)
			{
				// Update the contact information based on current particles' positions
				if (!Constraint.GetUseManifold() || (Constraint.GetManifoldPoints().Num() == 0))
				{
					Collisions::Update(Constraint, IterationParameters.Dt);
				}
				else
				{
					Constraint.UpdateManifoldContacts();
				}

				// Permanently disable a constraint that is beyond the cull distance
				if (Constraint.GetPhi() >= Constraint.GetCullDistance())
				{
					if (ParticleParameters.bCanDisableContacts)
					{
						Constraint.SetDisabled(true);
					}
					return;
				}

				// Do not early out here in the case of Accumulated impulse solve
				// @todo(chaos): remove this early out when we settle on manifolds
				const bool bIsAccumulatingImpulses = Constraint.GetUseManifold() || Chaos_Collision_UseAccumulatedImpulseClipSolve;
				if (Constraint.GetPhi() >= 0.0f && !bIsAccumulatingImpulses)
				{
					return;
				}

				// @todo(chaos): fix the collided flag - it will sometimes be set if clipping is enabled, even if there was no contact...
				// @todo(chaos): this doesn't seem to be being set or used...remove Collided and the Aux Collided array if so. If it is supposed 
				// to work, it should be set via the Scatter method of the SolverBody
				if (ParticleParameters.Collided)
				{
					FGenericParticleHandle Particle0 = FGenericParticleHandle(Constraint.Particle[0]);
					FGenericParticleHandle Particle1 = FGenericParticleHandle(Constraint.Particle[1]);
					Particle0->AuxilaryValue(*ParticleParameters.Collided) = true;
					Particle1->AuxilaryValue(*ParticleParameters.Collided) = true;
				}

				// What solver algorithm should we use?
				switch (IterationParameters.SolverType)
				{
				case EConstraintSolverType::GbfPbd:
					{
						ApplyContactManifold(Constraint, IterationParameters, ParticleParameters);
					}
					break;
				case EConstraintSolverType::StandardPbd:
					{
						Constraint.AccumulatedImpulse += ApplyContact2(Constraint, IterationParameters, ParticleParameters);
					}
					break;
				case EConstraintSolverType::QuasiPbd:
					{
						check(false);
					}
					break;
				default:
					break;
				}
			}
		}
		
		void ApplySweptImpl(FPBDCollisionConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			check(Constraint.GetCCDType() == ECollisionCCDType::Enabled);

			FSolverBody& Body0 = *Constraint.GetSolverBody0();
			FSolverBody& Body1 = *Constraint.GetSolverBody1();

			Collisions::UpdateSwept(Constraint, IterationParameters.Dt);
			
			const FContactParticleParameters CCDParticleParamaters{ ParticleParameters.RestitutionVelocityThreshold, true, ParticleParameters.Collided };
			if (Constraint.TimeOfImpact >= 1)
			{
				// If TOI = 1 (normal constraint) we don't want to split timestep at TOI.
				ApplyImpl(Constraint, IterationParameters, CCDParticleParamaters);
				return;
			}

			// Rebuild iteration params with partial dt, and non-zero iteration count to force update of constraint.
			// P may have changed due to other constraints, so at TOI our manifold needs updating.
			const FReal PartialDT = Constraint.TimeOfImpact * IterationParameters.Dt;
			const FReal RemainingDT = (1 - Constraint.TimeOfImpact) * IterationParameters.Dt;
			const int32 FakeIteration = IterationParameters.NumIterations / 2; // For iteration count dependent effects (like relaxation) // @todo: Do we still need this?
			const int32 PartialPairIterations = FMath::Max(IterationParameters.NumPairIterations, 2); // Do at least 2 pair iterations // @todo: Do we still need this?
			const FContactIterationParameters IterationParametersPartialDT{ PartialDT, FakeIteration, IterationParameters.NumIterations, PartialPairIterations, IterationParameters.SolverType, IterationParameters.NeedsAnotherIteration };
			const FContactIterationParameters IterationParametersRemainingDT{ RemainingDT, FakeIteration, IterationParameters.NumIterations, IterationParameters.NumPairIterations, IterationParameters.SolverType, IterationParameters.NeedsAnotherIteration };

			// Rewind P to TOI and Apply
			Body0.SetP(FMath::Lerp(Body0.X(), Body0.P(), Constraint.TimeOfImpact));
			ApplyImpl(Constraint, IterationParametersPartialDT, CCDParticleParamaters);

			// @todo(chaos): Make this work properly for real Stanard and QPBD implementations (they do not alter velocity)
			if (IterationParameters.SolverType == EConstraintSolverType::GbfPbd)
			{
				// Advance P to end of frame from TOI, and Apply
				if (CCDAlwaysSweepRemainingDT || IterationParameters.Iteration + 1 < IterationParameters.NumIterations)
				{
					Body0.SetP(Body0.P() + Body0.V() * RemainingDT); // If we are tunneling through something else due to this, it will be resolved in the next iteration
					ApplyImpl(Constraint, IterationParametersRemainingDT, CCDParticleParamaters);
				}
				else
				{
					// We get here if we cannot solve CCD collisions with the given number of iterations and restitution settings.
					// So don't do the remaining dt update. This will bleed the energy! (also: Ignore rotation)
					// To prevent this condition: increase number of iterations and/or reduce restitution and/or reduce velocities
					if (IterationParameters.Dt > SMALL_NUMBER)
					{
						// Update velocity to be consistent with PBD
						const FReal InvDt = FReal(1) / IterationParameters.Dt;
						Body0.SetV((Body0.P() - Body0.X()) * InvDt);
					}
					else
					{
						Body0.SetV(FVec3(0));
					}
				}
			}
		}


		void Apply(FPBDCollisionConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			if (Constraint.GetCCDType() == ECollisionCCDType::Disabled)
			{
				ApplyImpl(Constraint, IterationParameters, ParticleParameters);
			}
			else if (Constraint.GetCCDType() == ECollisionCCDType::Enabled)
			{
				ApplySweptImpl(Constraint, IterationParameters, ParticleParameters);
			}
		}


		void ApplyPushOutImpl(FPBDCollisionConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			for (int32 PairIt = 0; PairIt < IterationParameters.NumPairIterations; ++PairIt)
			{
				Update(Constraint, IterationParameters.Dt);

				// Ignore contacts where the closest point is greater than cull distance
				if (Constraint.GetPhi() >= Constraint.GetCullDistance())
				{
					// Optionally permanently disable the contact for the remaining iterations
					if (ParticleParameters.bCanDisableContacts)
					{
						Constraint.SetDisabled(true);
					}
					return;
				}

				// @todo(chaos): is this supposed to run for swept contacts as well?
				if (Constraint.GetCCDType() == ECollisionCCDType::Disabled)
				{
					switch (IterationParameters.SolverType)
					{
					case EConstraintSolverType::GbfPbd:
						{
							ApplyPushOutManifold(Constraint, IterationParameters, ParticleParameters);
						}
						break;
					case EConstraintSolverType::StandardPbd:
						{
							// Shouldn't have PushOut for Standard Pbd, but this is for experimentation (Collision PushOut Iterations should normally be set to 0 instead)
							ApplyPushOutManifold(Constraint, IterationParameters, ParticleParameters);
						}
						break;
					case EConstraintSolverType::QuasiPbd:
						{
							check(false);
						}
						break;
					default:
						break;
					}
				}
			}
		}

		void ApplyPushOut(FPBDCollisionConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			ApplyPushOutImpl(Constraint, IterationParameters, ParticleParameters);
		}

	} // Collisions

}// Chaos

