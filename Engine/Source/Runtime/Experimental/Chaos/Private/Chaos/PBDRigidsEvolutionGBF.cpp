// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/Defines.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/Framework/Parallel.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDCollisionSpringConstraints.h"
#include "Chaos/PerParticleEtherDrag.h"
#include "Chaos/PerParticleEulerStepVelocity.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/PerParticleInitForce.h"
#include "Chaos/PerParticlePBDEulerStep.h"
#include "Chaos/PerParticlePBDGroundConstraint.h"
#include "Chaos/PerParticlePBDUpdateFromDeltaPosition.h"
#include "ChaosStats.h"
#include "Chaos/EvolutionResimCache.h"

#include "ProfilingDebugging/ScopedTimers.h"
#include "Chaos/DebugDrawQueue.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{
#if !UE_BUILD_SHIPPING
	CHAOS_API bool bPendingHierarchyDump = false;
#else
	const bool bPendingHierarchyDump = false;
#endif

	namespace CVars
	{
		FRealSingle HackMaxAngularVelocity = 1000.f;
		FAutoConsoleVariableRef CVarHackMaxAngularVelocity(TEXT("p.HackMaxAngularVelocity"), HackMaxAngularVelocity, TEXT("Max cap on angular velocity: rad/s. This is only a temp solution and should not be relied on as a feature. -1.f to disable"));

		FRealSingle HackMaxVelocity = -1.f;
		FAutoConsoleVariableRef CVarHackMaxVelocity(TEXT("p.HackMaxVelocity2"), HackMaxVelocity, TEXT("Max cap on velocity: cm/s. This is only a temp solution and should not be relied on as a feature. -1.f to disable"));


		int DisableThreshold = 5;
		FAutoConsoleVariableRef CVarDisableThreshold(TEXT("p.DisableThreshold2"), DisableThreshold, TEXT("Disable threshold frames to transition to sleeping"));

		int CollisionDisableCulledContacts = 0;
		FAutoConsoleVariableRef CVarDisableCulledContacts(TEXT("p.CollisionDisableCulledContacts"), CollisionDisableCulledContacts, TEXT("Allow the PBDRigidsEvolutionGBF collision constraints to throw out contacts mid solve if they are culled."));

		// @todo(chaos): this should be 0 but we need it for CCD atm
		FRealSingle BoundsThicknessVelocityMultiplier = 0.0f;
		FAutoConsoleVariableRef CVarBoundsThicknessVelocityMultiplier(TEXT("p.CollisionBoundsVelocityInflation"), BoundsThicknessVelocityMultiplier, TEXT("Collision velocity inflation for speculatibe contact generation.[def:2.0]"));

		FRealSingle SmoothedPositionLerpRate = 0.1f;
		FAutoConsoleVariableRef CVarSmoothedPositionLerpRate(TEXT("p.Chaos.SmoothedPositionLerpRate"), SmoothedPositionLerpRate, TEXT("The interpolation rate for the smoothed position calculation. Used for sleeping."));

		int DisableParticleUpdateVelocityParallelFor = 0;
		FAutoConsoleVariableRef CVarDisableParticleUpdateVelocityParallelFor(TEXT("p.DisableParticleUpdateVelocityParallelFor"), DisableParticleUpdateVelocityParallelFor, TEXT("Disable Particle Update Velocity ParallelFor and run the update on a single thread"));

		bool bChaosUseCCD = true;
		FAutoConsoleVariableRef  CVarChaosUseCCD(TEXT("p.Chaos.CCD.UseCCD"), bChaosUseCCD , TEXT("Global flag to turn CCD on or off. Default is true"));

		FRealSingle CCDEnableThresholdBoundsScale = 0.4f;
		FAutoConsoleVariableRef  CVarCCDEnableThresholdBoundsScale(TEXT("p.Chaos.CCD.EnableThresholdBoundsScale"), CCDEnableThresholdBoundsScale , TEXT("CCD is used when object position is changing > smallest bound's extent * BoundsScale. 0 will always Use CCD. Values < 0 disables CCD."));

		bool bChaosCollisionCCDUseTightBoundingBox = true; 
		FAutoConsoleVariableRef  CVarChaosCollisionCCDUseTightBoundingBox(TEXT("p.Chaos.Collision.CCD.UseTightBoundingBox"), bChaosCollisionCCDUseTightBoundingBox , TEXT(""));


		int32 ChaosSolverCollisionPriority = 0;
		FAutoConsoleVariableRef CVarChaosSolverCollisionPriority(TEXT("p.Chaos.Solver.Collision.Priority"), ChaosSolverCollisionPriority, TEXT("Set constraint priority. Larger values are evaluated later [def:0]"));

		int32 ChaosSolverJointPriority = 0;
		FAutoConsoleVariableRef CVarChaosSolverJointPriority(TEXT("p.Chaos.Solver.Joint.Priority"), ChaosSolverJointPriority, TEXT("Set constraint priority. Larger values are evaluated later [def:0]"));

		int32 ChaosSolverSuspensionPriority = 0;
		FAutoConsoleVariableRef CVarChaosSolverSuspensionPriority(TEXT("p.Chaos.Solver.Suspension.Priority"), ChaosSolverSuspensionPriority, TEXT("Set constraint priority. Larger values are evaluated later [def:0]"));

		bool DoTransferJointConstraintCollisions = true;
		FAutoConsoleVariableRef CVarDoTransferJointConstraintCollisions(TEXT("p.Chaos.Solver.Joint.TransferCollisions"), DoTransferJointConstraintCollisions, TEXT("Allows joints to apply collisions to the parent from the child when the Joints TransferCollisionScale is not 0 [def:true]"));

		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::AdvanceOneTimeStep"), STAT_Evolution_AdvanceOneTimeStep, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::UnclusterUnions"), STAT_Evolution_UnclusterUnions, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::Integrate"), STAT_Evolution_Integrate, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::KinematicTargets"), STAT_Evolution_KinematicTargets, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::PostIntegrateCallback"), STAT_Evolution_PostIntegrateCallback, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::CollisionModifierCallback"), STAT_Evolution_CollisionModifierCallback, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::CCD"), STAT_Evolution_CCD, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::GraphColor"), STAT_Evolution_GraphColor, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::Gather"), STAT_Evolution_Gather, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::Scatter"), STAT_Evolution_Scatter, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::ApplyConstraintsPhase1"), STAT_Evolution_ApplyConstraintsPhase1, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::UpdateVelocities"), STAT_Evolution_UpdateVelocites, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::ApplyConstraintsPhase2"), STAT_Evolution_ApplyConstraintsPhase2, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::DetectCollisions"), STAT_Evolution_DetectCollisions, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::TransferJointCollisions"), STAT_Evolution_TransferJointCollisions, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::PostDetectCollisionsCallback"), STAT_Evolution_PostDetectCollisionsCallback, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::UpdateConstraintPositionBasedState"), STAT_Evolution_UpdateConstraintPositionBasedState, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::ComputeIntermediateSpatialAcceleration"), STAT_Evolution_ComputeIntermediateSpatialAcceleration, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::CreateConstraintGraph"), STAT_Evolution_CreateConstraintGraph, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::CreateIslands"), STAT_Evolution_CreateIslands, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::UpdateAccelerationStructures"), STAT_Evolution_UpdateAccelerationStructures, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::AddSleepingContacts"), STAT_Evolution_AddSleepingContacts, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::PreApplyCallback"), STAT_Evolution_PreApplyCallback, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::ParallelSolve"), STAT_Evolution_ParallelSolve, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::SaveParticlePostSolve"), STAT_Evolution_SavePostSolve, STATGROUP_Chaos);
		DECLARE_CYCLE_STAT(TEXT("FPBDRigidsEvolutionGBF::DeactivateSleep"), STAT_Evolution_DeactivateSleep, STATGROUP_Chaos);

		int32 SerializeEvolution = 0;
		FAutoConsoleVariableRef CVarSerializeEvolution(TEXT("p.SerializeEvolution"), SerializeEvolution, TEXT(""));

		bool bChaos_CollisionStore_Enabled = true;
		FAutoConsoleVariableRef CVarCollisionStoreEnabled(TEXT("p.Chaos.CollisionStore.Enabled"), bChaos_CollisionStore_Enabled, TEXT(""));
	}

	using namespace CVars;

#if !UE_BUILD_SHIPPING
template <typename TEvolution>
void SerializeToDisk(TEvolution& Evolution)
{
	const TCHAR* FilePrefix = TEXT("ChaosEvolution");
	const FString FullPathPrefix = FPaths::ProfilingDir() / FilePrefix;

	static FCriticalSection CS;	//many evolutions could be running in parallel, serialize one at a time to avoid file conflicts
	FScopeLock Lock(&CS);

	int32 Tries = 0;
	FString UseFileName;
	do
	{
		UseFileName = FString::Printf(TEXT("%s_%d.bin"), *FullPathPrefix, Tries++);
	} while (IFileManager::Get().FileExists(*UseFileName));

	//this is not actually file safe but oh well, very unlikely someone else is trying to create this file at the same time
	TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*UseFileName));
	if (File)
	{
		FChaosArchive Ar(*File);
		UE_LOG(LogChaos, Log, TEXT("SerializeToDisk File: %s"), *UseFileName);
		Evolution.Serialize(Ar);
	}
	else
	{
		UE_LOG(LogChaos, Warning, TEXT("Could not create file(%s)"), *UseFileName);
	}
}
#endif

void FPBDRigidsEvolutionGBF::Advance(const FReal Dt,const FReal MaxStepDt,const int32 MaxSteps)
{
	// Determine how many steps we would like to take
	int32 NumSteps = FMath::CeilToInt(Dt / MaxStepDt);
	if (NumSteps > 0)
	{
		PrepareTick();

		// Determine the step time
		const FReal StepDt = Dt / (FReal)NumSteps;

		// Limit the number of steps
		// NOTE: This is after step time calculation so simulation will appear to slow down for large Dt
		// but that is preferable to blowing up from a large timestep.
		NumSteps = FMath::Clamp(NumSteps, 1, MaxSteps);

		for (int32 Step = 0; Step < NumSteps; ++Step)
		{
			// StepFraction: how much of the remaining time this step represents, used to interpolate kinematic targets
			// E.g., for 4 steps this will be: 1/4, 1/2, 3/4, 1
			const FReal StepFraction = (FReal)(Step + 1) / (FReal)(NumSteps);
		
			UE_LOG(LogChaos, Verbose, TEXT("Advance dt = %f [%d/%d]"), StepDt, Step + 1, NumSteps);

			AdvanceOneTimeStepImpl(StepDt, FSubStepInfo{ StepFraction, Step, MaxSteps });
		}

		UnprepareTick();
	}
}

void FPBDRigidsEvolutionGBF::AdvanceOneTimeStep(const FReal Dt,const FSubStepInfo& SubStepInfo)
{
	PrepareTick();

	AdvanceOneTimeStepImpl(Dt, SubStepInfo);

	UnprepareTick();
}

int32 DrawAwake = 0;
FAutoConsoleVariableRef CVarDrawAwake(TEXT("p.chaos.DebugDrawAwake"),DrawAwake,TEXT("Draw particles that are awake"));

void FPBDRigidsEvolutionGBF::AdvanceOneTimeStepImpl(const FReal Dt, const FSubStepInfo& SubStepInfo)
{
	SCOPE_CYCLE_COUNTER(STAT_Evolution_AdvanceOneTimeStep);

	//for now we never allow solver to schedule more than two tasks back to back
	//this means we only need to keep indices alive for one additional frame
	//the code that pushes indices to pending happens after this check which ensures we won't delete until next frame
	//if sub-stepping is used, the index free will only happen on the first sub-step. However, since we are sub-stepping we would end up releasing half way through interval
	//by checking the step and only releasing on step 0, we ensure the entire interval will see the indices
	if(SubStepInfo.Step == 0)
	{
		Base::ReleasePendingIndices();
	}

#if !UE_BUILD_SHIPPING
	if (SerializeEvolution)
	{
		SerializeToDisk(*this);
	}
#endif

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_UnclusterUnions);
		Clustering.UnionClusterGroups();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_Integrate);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_Integrate);
		Integrate(Particles.GetActiveParticlesView(), Dt);
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_KinematicTargets);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_KinematicTargets);
		ApplyKinematicTargets(Dt, SubStepInfo.PseudoFraction);
	}

	if (PostIntegrateCallback != nullptr)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_PostIntegrateCallback);
		PostIntegrateCallback();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_UpdateConstraintPositionBasedState);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_UpdateConstraintPositionBasedState);
		UpdateConstraintPositionBasedState(Dt);
	}
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_ComputeIntermediateSpatialAcceleration);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_ComputeIntermediateSpatialAcceleration);
		Base::ComputeIntermediateSpatialAcceleration();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_DetectCollisions);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_DetectCollisions);
		CollisionDetector.GetBroadPhase().SetSpatialAcceleration(InternalAcceleration);

		CollisionDetector.DetectCollisions(Dt, GetCurrentStepResimCache());
	}

	if (PostDetectCollisionsCallback != nullptr)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_PostDetectCollisionsCallback);
		PostDetectCollisionsCallback();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_TransferJointCollisions);
		TransferJointConstraintCollisions();
	}

	if(CollisionModifiers)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_CollisionModifierCallback);
		CollisionConstraints.ApplyCollisionModifier(*CollisionModifiers, Dt);
	}

	if (bChaosUseCCD)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_CCD);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, CCD);
		CCDManager.ApplyConstraintsPhaseCCD(Dt, &CollisionConstraints.GetConstraintAllocator(), Particles.GetActiveParticlesView().Num());
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_CreateConstraintGraph);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_CreateConstraintGraph);
		CreateConstraintGraph();
	}
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_CreateIslands);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_CreateIslands);
		CreateIslands();
	}
	{
		// We keep the graph color stat name for now to compare with previous implementation
		// @todo : change the name to sort constraints
		SCOPE_CYCLE_COUNTER(STAT_Evolution_GraphColor);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_GraphColor);
		SortConstraints();
	}
	if (PreApplyCallback != nullptr)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_PreApplyCallback);
		PreApplyCallback();
	}
	CollisionConstraints.SetGravity(GetGravityForces().GetAcceleration());

	TArray<bool> SleepedIslands;
	SleepedIslands.SetNum(GetConstraintGraph().NumIslands());
	TArray<TArray<FPBDRigidParticleHandle*>> DisabledParticles;
	DisabledParticles.SetNum(GetConstraintGraph().NumIslands());
	SleepedIslands.SetNum(GetConstraintGraph().NumIslands());
	if(Dt > 0)
	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_ParallelSolve);
		PhysicsParallelFor(GetConstraintGraph().NumIslands(), [&](int32 Island) {

			bool bHasCachedData = false;
			FEvolutionResimCache* ResimCache = GetCurrentStepResimCache();
			if (ResimCache && ResimCache->IsResimming() && GetConstraintGraph().IslandNeedsResim(Island) == false)
			{
				bHasCachedData = true;
			}

			if(GetConstraintGraph().GetSolverIsland(Island)->IsSleeping())
			{
				return;
			}
			
			const TArray<FGeometryParticleHandle*>& IslandParticles = GetConstraintGraph().GetIslandParticles(Island);

			if (bHasCachedData)
			{
				for (auto Particle : IslandParticles)
				{
					if (auto Rigid = Particle->CastToRigidParticle())
					{
						ResimCache->ReloadParticlePostSolve(*Rigid);
					}
				}
			}
			else
			{
				// Update constraint graphs, coloring etc as required by the different constraint types in this island
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_UpdateAccelerationStructures);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_UpdateAccelerationStructures);
					UpdateAccelerationStructures(Dt, Island);
				}

				// Collect all the data that the constraint solvers operate on
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_Gather);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_Gather);
					GatherSolverInput(Dt, Island);
				}

				// Run the first phase of the constraint solvers
				// For GBF this is the hybrid velocity solving step (which also moves the bodies to make the implicit velocity be what it should be)
				// For PBD/QPBD this is the position solve step
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_ApplyConstraintsPhase1);
					CSV_SCOPED_TIMING_STAT(Chaos, ApplyConstraints);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_Apply);
					ApplyConstraintsPhase1(Dt, Island);
				}

				if (PostApplyCallback != nullptr)
				{
					PostApplyCallback(Island);
				}

				// Update implicit velocities from results of constraint solver phase 1
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_UpdateVelocites);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_UpdateVelocities);
					SetImplicitVelocities(Dt, Island);
				}

				// Run the second phase of the constraint solvers
				// For GBF this is the pushout step
				// For PBD this does nothing
				// For QPBD this is the velocity solve step
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_ApplyConstraintsPhase2);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_ApplyPushOut);
					ApplyConstraintsPhase2(Dt, Island);
				}

				// Update the particles with the results of the constraint solvers, and also update constraint data
				// that is accessed externally (net impusles, break info, etc)
				{
					SCOPE_CYCLE_COUNTER(STAT_Evolution_Scatter);
					CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_Scatter);
					ScatterSolverOutput(Dt, Island);
				}

				if (PostApplyPushOutCallback != nullptr)
				{
					PostApplyPushOutCallback(Island);
				}
			}

			for (auto Particle : IslandParticles)
			{
				// If a dynamic particle is moving slowly enough for long enough, disable it.
				// @todo(mlentine): Find a good way of not doing this when we aren't using this functionality

				// increment the disable count for the particle
				auto PBDRigid = Particle->CastToRigidParticle();
				if (PBDRigid && PBDRigid->ObjectState() == EObjectStateType::Dynamic)
				{
					if (PBDRigid->AuxilaryValue(PhysicsMaterials) && PBDRigid->V().SizeSquared() < PBDRigid->AuxilaryValue(PhysicsMaterials)->DisabledLinearThreshold &&
						PBDRigid->W().SizeSquared() < PBDRigid->AuxilaryValue(PhysicsMaterials)->DisabledAngularThreshold)
					{
						++PBDRigid->AuxilaryValue(ParticleDisableCount);
					}

					// check if we're over the disable count threshold
					if (PBDRigid->AuxilaryValue(ParticleDisableCount) > DisableThreshold)
					{
						PBDRigid->AuxilaryValue(ParticleDisableCount) = 0;
						//Particles.Disabled(Index) = true;
						DisabledParticles[Island].Add(PBDRigid);
						//Particles.V(Index) = TVector<T, d>(0);
						//Particles.W(Index) = TVector<T, d>(0);
					}

					if (!(ensure(!FMath::IsNaN(PBDRigid->P()[0])) && ensure(!FMath::IsNaN(PBDRigid->P()[1])) && ensure(!FMath::IsNaN(PBDRigid->P()[2]))))
					{
						//Particles.Disabled(Index) = true;
						DisabledParticles[Island].Add(PBDRigid);
					}
				}
			}

			// Turn off if not moving
			SleepedIslands[Island] = GetConstraintGraph().SleepInactive(Island, PhysicsMaterials, SolverPhysicsMaterials);
		});
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_SavePostSolve);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_SavePostSolve);
		FEvolutionResimCache* ResimCache = GetCurrentStepResimCache();
		if (ResimCache)
		{
			for (const auto& Particle : Particles.GetActiveKinematicParticlesView())
			{
				if (const auto* Rigid = Particle.CastToRigidParticle())
				{
					//NOTE: this assumes the cached values have not changed after the solve (V, W, P, Q should be untouched, otherwise we'll use the wrong values when resim happens)
					ResimCache->SaveParticlePostSolve(*Rigid->Handle());
				}
			}
			for (const auto& Particle : Particles.GetNonDisabledDynamicView())
			{
				//NOTE: this assumes the cached values have not changed after the solve (V, W, P, Q should be untouched, otherwise we'll use the wrong values when resim happens)
				ResimCache->SaveParticlePostSolve(*Particle.Handle());
			}
		}
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Evolution_DeactivateSleep);
		CSV_SCOPED_TIMING_STAT(PhysicsVerbose, StepSolver_DeactivateSleep);
		for (int32 Island = 0; Island < GetConstraintGraph().NumIslands(); ++Island)
		{
			if (SleepedIslands[Island])
			{
				GetConstraintGraph().SleepIsland(Particles, Island);
			}
			
			for (const auto Particle : DisabledParticles[Island])
			{
				DisableParticle(Particle);
			}
		}
	}

	Clustering.AdvanceClustering(Dt, GetCollisionConstraints());

	if(CaptureRewindData)
	{
		CaptureRewindData(Particles.GetDirtyParticlesView());
	}

	ParticleUpdatePosition(Particles.GetDirtyParticlesView(), Dt);

#if !UE_BUILD_SHIPPING
	if(SerializeEvolution)
	{
		SerializeToDisk(*this);
	}

#if CHAOS_DEBUG_DRAW
	if(FDebugDrawQueue::IsDebugDrawingEnabled())
	{
		if(!!DrawAwake)
		{
			static const FColor IslandColors[] = {FColor::Green,FColor::Red,FColor::Yellow,
				FColor::Blue,FColor::Orange,FColor::Black,FColor::Cyan,
				FColor::Magenta,FColor::Purple,FColor::Turquoise};

			static const int32 NumColors = sizeof(IslandColors) / sizeof(IslandColors[0]);
			
			for(const auto& Active : Particles.GetActiveParticlesView())
			{
				if(const auto* Geom = Active.Geometry().Get())
				{
					if(Geom->HasBoundingBox())
					{
						const int32 Island = Active.IslandIndex();
						ensure(Island >= 0);
						const int32 ColorIdx = Island % NumColors;
						const FAABB3 LocalBounds = Geom->BoundingBox();
						FDebugDrawQueue::GetInstance().DrawDebugBox(Active.X(),LocalBounds.Extents()*0.5f,Active.R(),IslandColors[ColorIdx],false,-1.f,0,0.f);
					}
				}
			}
		}
	}
#endif
#endif
}

void FPBDRigidsEvolutionGBF::GatherSolverInput(FReal Dt, int32 Island)
{
	// We must initialize the solver body container to be large enough to hold all particles in the
	// island so that the pointers remain valid (the array should not grow and relocate)
	ConstraintGraph.GetSolverIsland(Island)->GetBodyContainer().Reset(ConstraintGraph.GetIslandParticles(Island).Num());

	// NOTE: SolverBodies are gathered as part of the constraint gather, in the order that they are first seen 
	for (FPBDConstraintGraphRule* ConstraintRule : PrioritizedConstraintRules)
	{
		ConstraintRule->GatherSolverInput(Dt, Island);
	}
}

void FPBDRigidsEvolutionGBF::ScatterSolverOutput(FReal Dt, int32 Island)
{
	// Scatter solver results for constraints (impulses, break events, etc)
	for (FPBDConstraintGraphRule* ConstraintRule : PrioritizedConstraintRules)
	{
		ConstraintRule->ScatterSolverOutput(Dt, Island);
	}

	// Scatter body results back to particles (position, rotation, etc)
	ConstraintGraph.GetSolverIsland(Island)->GetBodyContainer().ScatterOutput();
}

void FPBDRigidsEvolutionGBF::ApplyConstraintsPhase1(const FReal Dt, int32 Island)
{
	int32 LocalNumIterations = ChaosNumContactIterationsOverride >= 0 ? ChaosNumContactIterationsOverride : NumIterations;
	// @todo(ccaulfield): track whether we are sufficiently solved and can early-out
	for (int i = 0; i < LocalNumIterations; ++i)
	{
		bool bNeedsAnotherIteration = false;
		for (FPBDConstraintGraphRule* ConstraintRule : PrioritizedConstraintRules)
		{
			bNeedsAnotherIteration |= ConstraintRule->ApplyConstraints(Dt, Island, i, LocalNumIterations);
		}

		if (ChaosRigidsEvolutionApplyAllowEarlyOutCVar && !bNeedsAnotherIteration)
		{
			break;
		}
	}
}

void FPBDRigidsEvolutionGBF::SetImplicitVelocities(const FReal Dt, int32 Island)
{
	ConstraintGraph.GetSolverIsland(Island)->GetBodyContainer().SetImplicitVelocities(Dt);
}

void FPBDRigidsEvolutionGBF::ApplyConstraintsPhase2(const FReal Dt, int32 Island)
{
	int32 LocalNumPushOutIterations = ChaosNumPushOutIterationsOverride >= 0 ? ChaosNumPushOutIterationsOverride : NumPushOutIterations;
	bool bNeedsAnotherIteration = true;
	for (int32 It = 0; It < LocalNumPushOutIterations; ++It)
	{
		bNeedsAnotherIteration = false;
		for (FPBDConstraintGraphRule* ConstraintRule : PrioritizedConstraintRules)
		{
			bNeedsAnotherIteration |= ConstraintRule->ApplyPushOut(Dt, Island, It, LocalNumPushOutIterations);
		}

		if (ChaosRigidsEvolutionApplyPushoutAllowEarlyOutCVar && !bNeedsAnotherIteration)
		{
			break;
		}
	}
}


FPBDRigidsEvolutionGBF::FPBDRigidsEvolutionGBF(FPBDRigidsSOAs& InParticles,THandleArray<FChaosPhysicsMaterial>& SolverPhysicsMaterials, const TArray<ISimCallbackObject*>* InCollisionModifiers, bool InIsSingleThreaded)
	: Base(InParticles, SolverPhysicsMaterials, DefaultNumIterations, DefaultNumPushOutIterations, InIsSingleThreaded)
	, Clustering(*this, Particles.GetClusteredParticles())
	, JointConstraintRule(JointConstraints, ChaosSolverJointPriority)
	, SuspensionConstraintRule(SuspensionConstraints, ChaosSolverSuspensionPriority)
	, CollisionConstraints(InParticles, Collided, PhysicsMaterials, PerParticlePhysicsMaterials, DefaultNumCollisionPairIterations, DefaultNumCollisionPushOutPairIterations, DefaultRestitutionThreshold)
	, CollisionRule(CollisionConstraints, ChaosSolverCollisionPriority)
	, BroadPhase(InParticles)
	, NarrowPhase(DefaultCollisionCullDistance, BoundsThicknessVelocityMultiplier, CollisionConstraints.GetConstraintAllocator())
	, CollisionDetector(BroadPhase, NarrowPhase, CollisionConstraints)
	, PostIntegrateCallback(nullptr)
	, PreApplyCallback(nullptr)
	, PostApplyCallback(nullptr)
	, PostApplyPushOutCallback(nullptr)
	, CurrentStepResimCacheImp(nullptr)
	, CollisionModifiers(InCollisionModifiers)
	, CCDManager()
{
	CollisionConstraints.SetCanDisableContacts(!!CollisionDisableCulledContacts);

	SetParticleUpdatePositionFunction([this](const TParticleView<FPBDRigidParticles>& ParticlesInput, const FReal Dt)
	{
		ParticlesInput.ParallelFor([&](auto& Particle, int32 Index)
		{
			if (Dt > SMALL_NUMBER)
			{
				const FReal SmoothRate = FMath::Clamp(SmoothedPositionLerpRate, 0.0f, 1.0f);
				const FVec3 VImp = FVec3::CalculateVelocity(Particle.X(), Particle.P(), Dt);
				const FVec3 WImp = FRotation3::CalculateAngularVelocity(Particle.R(), Particle.Q(), Dt);
				Particle.VSmooth() = FMath::Lerp(Particle.VSmooth(), VImp, SmoothRate);
				Particle.WSmooth() = FMath::Lerp(Particle.WSmooth(), WImp, SmoothRate);
			}

			Particle.X() = Particle.P();
			Particle.R() = Particle.Q();

			//TODO: rename this function since it's not just updating position
			Particle.SetPreObjectStateLowLevel(Particle.ObjectState());
		});
	});

	AddForceFunction([this](TTransientPBDRigidParticleHandle<FReal, 3>& HandleIn, const FReal Dt)
	{
		GravityForces.Apply(HandleIn, Dt);
	});

	AddConstraintRule(&SuspensionConstraintRule);
	AddConstraintRule(&CollisionRule);
	AddConstraintRule(&JointConstraintRule);

	SetInternalParticleInitilizationFunction([](const FGeometryParticleHandle*, const FGeometryParticleHandle*) {});

	NarrowPhase.GetContext().bFilteringEnabled = true;
	NarrowPhase.GetContext().bDeferUpdate = false;
	NarrowPhase.GetContext().bAllowManifolds = false;
}

FPBDRigidsEvolutionGBF::~FPBDRigidsEvolutionGBF()
{
}


void FPBDRigidsEvolutionGBF::Serialize(FChaosArchive& Ar)
{
	Base::Serialize(Ar);
}

TUniquePtr<IResimCacheBase> FPBDRigidsEvolutionGBF::CreateExternalResimCache() const
{
	return TUniquePtr<IResimCacheBase>(new FEvolutionResimCache());
}

void FPBDRigidsEvolutionGBF::SetCurrentStepResimCache(IResimCacheBase* InCurrentStepResimCache)
{
	CurrentStepResimCacheImp = static_cast<FEvolutionResimCache*>(InCurrentStepResimCache);
}

void FPBDRigidsEvolutionGBF::TransferJointConstraintCollisions()
{
	// Transfer collisions from the child of a joint to the parent.
	// E.g., if body A and B are connected by a joint, with A the parent and B the child...
	// then a third body C collides with B...
	// we create a new collision between A and C at the same world position.
	// E.g., This can be used to forward collision impulses from a vehicle bumper to its
	// chassis without having to worry about making the joint connecting them very stiff
	// which is quite difficult for large mass ratios and would require many iterations.
	if (DoTransferJointConstraintCollisions)
	{
		FCollisionConstraintAllocator& CollisionAllocator = CollisionConstraints.GetConstraintAllocator();

		// @todo(chaos): we should only visit the joints that have ContactTransferScale > 0 
		for (int32 JointConstraintIndex = 0; JointConstraintIndex < GetJointConstraints().NumConstraints(); ++JointConstraintIndex)
		{
			FPBDJointConstraintHandle* JointConstraint = GetJointConstraints().GetConstraintHandle(JointConstraintIndex);
			const FPBDJointSettings& JointSettings = JointConstraint->GetSettings();
			if (JointSettings.ContactTransferScale > FReal(0))
			{
				FGenericParticleHandle ParentParticle = JointConstraint->GetConstrainedParticles()[1];
				FGenericParticleHandle ChildParticle = JointConstraint->GetConstrainedParticles()[0];

				const FRigidTransform3 ParentTransform = FParticleUtilities::GetActorWorldTransform(ParentParticle);
				const FRigidTransform3 ChildTransform = FParticleUtilities::GetActorWorldTransform(ChildParticle);
				const FRigidTransform3 ChildToParentTransform = ChildTransform.GetRelativeTransform(ParentTransform);

				ChildParticle->Handle()->ParticleCollisions().VisitCollisions(
					[&](const FPBDCollisionConstraint* ChildCollisionConstraint)
					{
						if (ChildCollisionConstraint->GetCCDType() != ECollisionCCDType::Disabled)
						{
							return;
						}

						// @todo(chaos): implemeent this
						// Note: the defined out version has a couple issues we will need to address in the new version
						//	-	it passes Implicit pointers from one body to a constraint whose lifetime is not controlled by that body
						//		which could cause problems if the first body is destroyed.
						//	-	we need to properly support collisions constraints without one (or both) Implicit Objects. Collisions are 
						//		managed per shape pair, and found by a key that depends on them, so we'd need to rethink that a bit. 
						//		Here it's useful to be able to use the child implicit to generate the unique key, but we don't want the 
						//		constraint to hold the pointer (see previous issue).
						//	-	we should check to see if there is already an active constraint between the bodies because we don't want
						//		to replace a legit collision with our fake one...probably
						ensure(false);

						const FGeometryParticleHandle* NewParentParticleConst = (ChildCollisionConstraint->GetParticle0() == ChildParticle->Handle()) ? ChildCollisionConstraint->GetParticle1() : ChildCollisionConstraint->GetParticle0();
						
						FGeometryParticleHandle* NewParticleA = const_cast<FGeometryParticleHandle*>(NewParentParticleConst);
						FGeometryParticleHandle* NewParticleB = ParentParticle->Handle();

						// Set up NewCollision - this should duplicate what happens in collision detection, except the
						// contact points are just read from the source constraint rather than via the narrow phase
						//FPBDCollisionConstraint NewCollision = FPBDCollisionConstraint(...);
						// NewCollision.AddOneshotManifoldContact(...);
						// ...
						// NewCollision.SetStiffness(JointSettings.ContactTransferScale);

						// Add collision to the system
						//FParticlePairMidPhase* MidPhase = CollisionAllocator.GetParticlePairMidPhase(NewParticleA, NewParticleB);
						//MidPhase->InjectCollision(NewCollision);
					});
			}
		}

		CollisionAllocator.ProcessInjectedConstraints();
	}
#if 0
	//
	// Append transfer constraints. 
	//
	// @todo(chaos): this should be implementable with a collision modifier system
	using FRigidHandle = TPBDRigidParticleHandleImp<FReal, 3, true>;
	if (DoTransferJointConstraintCollisions)
	{
		FCollisionConstraintAllocator& CollisionAllocator = CollisionConstraints.GetConstraintAllocator();

		TMap<TGeometryParticleHandle<FReal, 3>*, TArray<FPBDCollisionConstraintHandle*> >& CollisionMap = CollisionConstraints.GetParticleCollisionsMap();
		for (auto& CollisionPair : CollisionMap)
		{
			if (FRigidHandle* ChildParticle = CollisionPair.Key->CastToRigidParticle())
			{
				const FRigidTransform3 ChildTransform = FParticleUtilities::GetActorWorldTransform(FConstGenericParticleHandle(ChildParticle));

				for (FConstraintHandle* Constraint : ChildParticle->ParticleConstraints())
				{
					if (FPBDJointConstraintHandle* JointConstraint = Constraint->As<FPBDJointConstraintHandle>())
					{
						if (!FMath::IsNearlyZero(JointConstraint->GetSettings().ContactTransferScale))
						{
							if (FRigidHandle* ParentParticle = JointConstraint->GetConstrainedParticles()[1]->CastToRigidParticle()) // Parent
							{
								const FRigidTransform3 ParentTransform = FParticleUtilities::GetActorWorldTransform(FConstGenericParticleHandle(ParentParticle));
								const FTransform ChildToParentTransform = ChildTransform.GetRelativeTransform(ParentTransform);

								for (FPBDCollisionConstraintHandle* ContactHandle : CollisionPair.Value)
								{
									if (ContactHandle->GetCCDType() == ECollisionCCDType::Disabled)
									{
										const FPBDCollisionConstraint& CurrConstraint = ContactHandle->GetContact();

										FPBDCollisionConstraint* TransferedConstraint;
										if (ContactHandle->GetConstrainedParticles()[0] == ChildParticle)
										{
											TransferedConstraint = FPBDCollisionConstraint::Make(
												ParentParticle,
												CurrConstraint.GetManifold().Implicit[0],
												CurrConstraint.GetManifold().Simplicial[0],
												ChildTransform,
												ChildToParentTransform.GetRelativeTransform(CurrConstraint.ImplicitTransform[0]),
												CurrConstraint.Particle[1],
												CurrConstraint.GetManifold().Implicit[1],
												CurrConstraint.GetManifold().Simplicial[1],
												ParentTransform,
												CurrConstraint.ImplicitTransform[1],
												CurrConstraint.GetCullDistance(),
												CurrConstraint.GetManifold().ShapesType,
												CurrConstraint.GetUseManifold(),
												CollisionAllocator
											);
										}
										else
										{
											TransferedConstraint = FPBDCollisionConstraint::Make(
												CurrConstraint.Particle[0],
												CurrConstraint.GetManifold().Implicit[0],
												CurrConstraint.GetManifold().Simplicial[0],
												ParentTransform,
												CurrConstraint.ImplicitTransform[0],
												ParentParticle,
												CurrConstraint.GetManifold().Implicit[1],
												CurrConstraint.GetManifold().Simplicial[1],
												ChildTransform,
												ChildToParentTransform.GetRelativeTransform(CurrConstraint.ImplicitTransform[1]),
												CurrConstraint.GetCullDistance(),
												CurrConstraint.GetManifold().ShapesType,
												CurrConstraint.GetUseManifold(),
												CollisionAllocator
											);
										}
										TArray<FManifoldPoint> TransferManifolds;
										for (const FManifoldPoint& Manifold : CurrConstraint.GetManifoldPoints())
										{
											TransferManifolds.Add(Manifold);
										}
										TransferedConstraint->SetManifoldPoints(TransferManifolds);
										for (FManifoldPoint& Manifold : TransferedConstraint->GetManifoldPoints())
										{
											TransferedConstraint->UpdateManifoldPointFromContact(Manifold);
										}
										TransferedConstraint->SetStiffness(JointConstraint->GetSettings().ContactTransferScale);
									}
								}
							}
						}
					}
				}
			}
		}

		CollisionAllocator.ProcessInjectedConstraints();
	}
#endif
}

}

