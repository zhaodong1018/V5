// Copyright Epic Games, Inc. All Rights Reserved.
#include "ChaosCloth/ChaosClothingSimulationSolver.h"
#include "ChaosCloth/ChaosClothingSimulationCloth.h"
#include "ChaosCloth/ChaosClothingSimulationCollider.h"
#include "ChaosCloth/ChaosClothingSimulationMesh.h"
#include "ChaosCloth/ChaosClothingSimulation.h"
#include "ChaosCloth/ChaosClothPrivate.h"
#include "Chaos/PBDEvolution.h"
#if INTEL_ISPC
#include "ChaosClothingSimulationSolver.ispc.generated.h"
#endif

#if !UE_BUILD_SHIPPING
#include "FramePro/FramePro.h"
#include "HAL/IConsoleManager.h"
#else
#define FRAMEPRO_ENABLED 0
#endif

using namespace Chaos;

DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Update"), STAT_ChaosClothSolverUpdate, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Update Cloths"), STAT_ChaosClothSolverUpdateCloths, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Update Pre Solver Step"), STAT_ChaosClothSolverUpdatePreSolverStep, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Update Solver Step"), STAT_ChaosClothSolverUpdateSolverStep, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Update Post Solver Step"), STAT_ChaosClothSolverUpdatePostSolverStep, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Calculate Bounds"), STAT_ChaosClothSolverCalculateBounds, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Particle Pre Simulation Transforms"), STAT_ChaosClothParticlePreSimulationTransforms, STATGROUP_ChaosCloth);
DECLARE_CYCLE_STAT(TEXT("Chaos Cloth Solver Collision Pre Simulation Transforms"), STAT_ChaosClothCollisionPreSimulationTransforms, STATGROUP_ChaosCloth);

static int32 ChaosClothSolverMinParallelBatchSize = 1000;
static bool bChaosClothSolverParallelClothPreUpdate = false;  // TODO: Doesn't seem to improve much here. Review this after the ISPC implementation.
static bool bChaosClothSolverParallelClothUpdate = true;
static bool bChaosClothSolverParallelClothPostUpdate = true;
static bool bChaosClothSolverUseImprovedTimeStepSmoothing = true;

#if !UE_BUILD_SHIPPING
static int32 ChaosClothSolverDebugHitchLength = 0;
static int32 ChaosClothSolverDebugHitchInterval = 0;
static bool bChaosClothSolverDisableCollision = false;

FAutoConsoleVariableRef CVarChaosClothSolverMinParallelBatchSize(TEXT("p.ChaosCloth.Solver.MinParallelBatchSize"), ChaosClothSolverMinParallelBatchSize, TEXT("The minimum number of particle to process in parallel batch by the solver."));
FAutoConsoleVariableRef CVarChaosClothSolverParallelClothPreUpdate(TEXT("p.ChaosCloth.Solver.ParallelClothPreUpdate"), bChaosClothSolverParallelClothPreUpdate, TEXT("Pre-transform the cloth particles for each cloth in parallel."));
FAutoConsoleVariableRef CVarChaosClothSolverParallelClothUpdate(TEXT("p.ChaosCloth.Solver.ParallelClothUpdate"), bChaosClothSolverParallelClothUpdate, TEXT("Skin the physics mesh and do the other cloth update for each cloth in parallel."));
FAutoConsoleVariableRef CVarChaosClothSolverParallelClothPostUpdate(TEXT("p.ChaosCloth.Solver.ParallelClothPostUpdate"), bChaosClothSolverParallelClothPostUpdate, TEXT("Pre-transform the cloth particles for each cloth in parallel."));
FAutoConsoleVariableRef CVarChaosClothSolverDebugHitchLength(TEXT("p.ChaosCloth.Solver.DebugHitchLength"), ChaosClothSolverDebugHitchLength, TEXT("Hitch length in ms. Create artificial hitches to debug simulation jitter. 0 to disable"));
FAutoConsoleVariableRef CVarChaosClothSolverDebugHitchInterval(TEXT("p.ChaosCloth.Solver.DebugHitchInterval"), ChaosClothSolverDebugHitchInterval, TEXT("Hitch interval in frames. Create artificial hitches to debug simulation jitter. 0 to disable"));
FAutoConsoleVariableRef CVarChaosClothSolverDisableCollision(TEXT("p.ChaosCloth.Solver.DisableCollision"), bChaosClothSolverDisableCollision, TEXT("Disable all collision particles. Needs reset of the simulation (p.ChaosCloth.Reset)."));
#endif

#if INTEL_ISPC && !UE_BUILD_SHIPPING
bool bChaos_PreSimulationTransforms_ISPC_Enabled = true;
FAutoConsoleVariableRef CVarChaosPreSimulationTransformsISPCEnabled(TEXT("p.Chaos.PreSimulationTransforms.ISPC"), bChaos_PreSimulationTransforms_ISPC_Enabled, TEXT("Whether to use ISPC optimizations in ApplySimulationTransforms"));
bool bChaos_CalculateBounds_ISPC_Enabled = bChaos_CalculateBounds_ISPC_Enable;  // Disabled by default
FAutoConsoleVariableRef CVarChaosCalculateBoundsISPCEnabled(TEXT("p.Chaos.CalculateBounds.ISPC"), bChaos_CalculateBounds_ISPC_Enabled, TEXT("Whether to use ISPC optimizations in CalculateBounds"));
#endif

FAutoConsoleVariableRef CVarChaosClothSolverUseImprovedTimeStepSmoothing(TEXT("p.ChaosCloth.Solver.UseImprovedTimeStepSmoothing"), bChaosClothSolverUseImprovedTimeStepSmoothing, TEXT("Use the time step smoothing on input forces only rather than on the entire cloth solver, in order to avoid miscalculating velocities."));

namespace ChaosClothingSimulationSolverDefault
{
	static const FVec3 Gravity(0.f, 0.f, -980.665f);  // cm/s^2
	static const FVec3 WindVelocity(0.f);
	static const int32 NumIterations = 1;
	static const int32 NumSubsteps = 1;
	static const FRealSingle SelfCollisionThickness = 2.f;
	static const FRealSingle CollisionThickness = 1.2f;
	static const FRealSingle FrictionCoefficient = 0.2f;
	static const FRealSingle DampingCoefficient = 0.01f;
}

namespace ChaosClothingSimulationSolverConstant
{
	static const FReal WorldScale = 100.f;  // World is in cm, but values like wind speed and density are in SI unit and relates to m.
	static const FReal StartDeltaTime = 1.f / 30.f;  // Initialize filtered timestep at 30fps
}

FClothingSimulationSolver::FClothingSimulationSolver()
	: OldLocalSpaceLocation(0.f)
	, LocalSpaceLocation(0.f)
	, Time(0)
	, DeltaTime(ChaosClothingSimulationSolverConstant::StartDeltaTime)
	, NumIterations(ChaosClothingSimulationSolverDefault::NumIterations)
	, NumSubsteps(ChaosClothingSimulationSolverDefault::NumSubsteps)
	, CollisionParticlesOffset(0)
	, CollisionParticlesSize(0)
	, Gravity(ChaosClothingSimulationSolverDefault::Gravity)
	, WindVelocity(ChaosClothingSimulationSolverDefault::WindVelocity)
	, LegacyWindAdaption((FReal)0.)
	, bIsClothGravityOverrideEnabled(false)
{
	FPBDParticles LocalParticles;
	FKinematicGeometryClothParticles TRigidParticles;
	Evolution.Reset(
		new FPBDEvolution(
			MoveTemp(LocalParticles),
			MoveTemp(TRigidParticles),
			{}, // CollisionTriangles
			ChaosClothingSimulationSolverDefault::NumIterations,
			ChaosClothingSimulationSolverDefault::CollisionThickness,
			ChaosClothingSimulationSolverDefault::SelfCollisionThickness,
			ChaosClothingSimulationSolverDefault::FrictionCoefficient,
			ChaosClothingSimulationSolverDefault::DampingCoefficient));

	// Add simulation groups arrays
	Evolution->AddArray(&PreSimulationTransforms);
	Evolution->AddArray(&FictitiousAngularDisplacement);

	Evolution->Particles().AddArray(&Normals);
	Evolution->Particles().AddArray(&OldAnimationPositions);
	Evolution->Particles().AddArray(&AnimationPositions);
	Evolution->Particles().AddArray(&AnimationNormals);

	Evolution->CollisionParticles().AddArray(&CollisionBoneIndices);
	Evolution->CollisionParticles().AddArray(&CollisionBaseTransforms);
	Evolution->CollisionParticles().AddArray(&OldCollisionTransforms);
	Evolution->CollisionParticles().AddArray(&CollisionTransforms);

	Evolution->SetKinematicUpdateFunction(
		[this](FPBDParticles& ParticlesInput, const FReal Dt, const FReal LocalTime, const int32 Index)
		{
			const FReal Alpha = (LocalTime - Time) / DeltaTime;
			ParticlesInput.P(Index) = Alpha * AnimationPositions[Index] + (1.f - Alpha) * OldAnimationPositions[Index];  // X is the step initial condition, here it's P that needs to be updated so that constraints works with the correct step target
		});

	Evolution->SetCollisionKinematicUpdateFunction(
		[this](FKinematicGeometryClothParticles& ParticlesInput, const FReal Dt, const FReal LocalTime, const int32 Index)
		{
			checkSlow(Dt > SMALL_NUMBER && DeltaTime > SMALL_NUMBER);
			const FReal Alpha = (LocalTime - Time) / DeltaTime;
			const FVec3 NewX =
				Alpha * CollisionTransforms[Index].GetTranslation() + (1.f - Alpha) * OldCollisionTransforms[Index].GetTranslation();
			ParticlesInput.V(Index) = (NewX - ParticlesInput.X(Index)) / Dt;
			ParticlesInput.X(Index) = NewX;
			FRotation3 NewR = FQuat::Slerp(OldCollisionTransforms[Index].GetRotation(), CollisionTransforms[Index].GetRotation(), Alpha);
			FRotation3 Delta = NewR * ParticlesInput.R(Index).Inverse();
			FReal Angle = Delta.GetAngle();
			FVec3 Axis = Delta.GetRotationAxis();
			ParticlesInput.W(Index) = (FVec3)Axis * Angle / Dt;
			ParticlesInput.R(Index) = NewR;
		});
}

FClothingSimulationSolver::~FClothingSimulationSolver()
{
}

void FClothingSimulationSolver::SetLocalSpaceLocation(const FVec3& InLocalSpaceLocation, bool bReset)
{
	LocalSpaceLocation = InLocalSpaceLocation;
	if (bReset)
	{
		OldLocalSpaceLocation = InLocalSpaceLocation;
	}
}

void FClothingSimulationSolver::SetCloths(TArray<FClothingSimulationCloth*>&& InCloths)
{
	// Remove old cloths
	RemoveCloths();

	// Update array
	Cloths = MoveTemp(InCloths);

	// Add the new cloths' particles
	for (FClothingSimulationCloth* const Cloth : Cloths)
	{
		check(Cloth);

		// Add the cloth's particles
		Cloth->Add(this);

		// Set initial state
		Cloth->PreUpdate(this);
		Cloth->Update(this);
	}

	// Update external collision's offset
	CollisionParticlesOffset = Evolution->CollisionParticles().Size();
}

void FClothingSimulationSolver::AddCloth(FClothingSimulationCloth* InCloth)
{
	check(InCloth);

	if (Cloths.Find(InCloth) != INDEX_NONE)
	{
		return;
	}

	// Add the cloth to the solver update array
	Cloths.Emplace(InCloth);

	// Reset external collisions so that there is never any external collision particles below cloth's ones
	ResetCollisionParticles(CollisionParticlesOffset);

	// Add the cloth's particles
	InCloth->Add(this);

	// Set initial state
	InCloth->PreUpdate(this);
	InCloth->Update(this);

	// Update external collision's offset
	CollisionParticlesOffset = Evolution->CollisionParticles().Size();
}

void FClothingSimulationSolver::RemoveCloth(FClothingSimulationCloth* InCloth)
{
	if (Cloths.Find(InCloth) == INDEX_NONE)
	{
		return;
	}

	// Remove reference to this solver
	InCloth->Remove(this);

	// Remove collider from array
	Cloths.RemoveSwap(InCloth);

	// Reset collisions so that there is never any external collision particles below the cloth's ones
	ResetCollisionParticles();

	// Reset cloth particles and associated elements
	ResetParticles();

	// Re-add the remaining cloths' particles
	for (FClothingSimulationCloth* const Cloth : Cloths)
	{
		// Add the cloth's particles
		Cloth->Add(this);

		// Set initial state
		Cloth->PreUpdate(this);
		Cloth->Update(this);
	}

	// Update external collision's offset
	CollisionParticlesOffset = Evolution->CollisionParticles().Size();
}

void FClothingSimulationSolver::RemoveCloths()
{
	// Remove all cloths from array
	for (FClothingSimulationCloth* const Cloth : Cloths)
	{
		Cloth->Remove(this);
	}
	Cloths.Reset();

	// Reset solver collisions
	ResetCollisionParticles();

	// Reset cloth particles and associated elements
	ResetParticles();
}

void FClothingSimulationSolver::RefreshCloth(FClothingSimulationCloth* InCloth)
{
	if (Cloths.Find(InCloth) == INDEX_NONE)
	{
		return;
	}

	// TODO: Add different ways to refresh cloths without recreating everything (collisions, constraints, particles)
	RefreshCloths();
}

void FClothingSimulationSolver::RefreshCloths()
{
	// Remove the cloths' & collisions' particles
	for (FClothingSimulationCloth* const Cloth : Cloths)
	{
		// Remove any solver data held by the cloth 
		Cloth->Remove(this);
	}

	// Reset collision particles
	ResetCollisionParticles();

	// Reset cloth particles and associated elements
	ResetParticles();

	// Re-add the cloths' & collisions' particles
	for (FClothingSimulationCloth* const Cloth : Cloths)
	{
		// Re-Add the cloth's and collisions' particles
		Cloth->Add(this);

		// Set initial state
		Cloth->PreUpdate(this);
		Cloth->Update(this);
	}

	// Update solver collider's offset
	CollisionParticlesOffset = Evolution->CollisionParticles().Size();
}

void FClothingSimulationSolver::ResetParticles()
{
	Evolution->ResetParticles();
	Evolution->ResetConstraintRules();
	Evolution->ResetSelfCollision();
	ClothsConstraints.Reset();
}

int32 FClothingSimulationSolver::AddParticles(int32 NumParticles, uint32 GroupId)
{
	if (!NumParticles)
	{
		return INDEX_NONE;
	}
	const int32 Offset = Evolution->AddParticleRange(NumParticles, GroupId, /*bActivate =*/ false);

	// Add an empty constraints container for this range
	check(!ClothsConstraints.Find(Offset));  // We cannot already have this Offset in the map, particle ranges are always added, never removed (unless reset)

	ClothsConstraints.Emplace(Offset, MakeUnique<FClothConstraints>())
		->Initialize(Evolution.Get(), AnimationPositions, OldAnimationPositions, AnimationNormals, Offset, NumParticles);

	// Always starts with particles disabled
	EnableParticles(Offset, false);

	return Offset;
}

void FClothingSimulationSolver::EnableParticles(int32 Offset, bool bEnable)
{
	Evolution->ActivateParticleRange(Offset, bEnable);
	GetClothConstraints(Offset).Enable(bEnable);
}

const FVec3* FClothingSimulationSolver::GetParticlePs(int32 Offset) const
{
	return &Evolution->Particles().P(Offset);
}

FVec3* FClothingSimulationSolver::GetParticlePs(int32 Offset)
{
	return &Evolution->Particles().P(Offset);
}

const FVec3* FClothingSimulationSolver::GetParticleXs(int32 Offset) const
{
	return &Evolution->Particles().X(Offset);
}

FVec3* FClothingSimulationSolver::GetParticleXs(int32 Offset)
{
	return &Evolution->Particles().X(Offset);
}

const FVec3* FClothingSimulationSolver::GetParticleVs(int32 Offset) const
{
	return &Evolution->Particles().V(Offset);
}

FVec3* FClothingSimulationSolver::GetParticleVs(int32 Offset)
{
	return &Evolution->Particles().V(Offset);
}

const FReal* FClothingSimulationSolver::GetParticleInvMasses(int32 Offset) const
{
	return &Evolution->Particles().InvM(Offset);
}

void FClothingSimulationSolver::ResetCollisionParticles(int32 InCollisionParticlesOffset)
{
	Evolution->ResetCollisionParticles(InCollisionParticlesOffset);
	CollisionParticlesOffset = InCollisionParticlesOffset;
	CollisionParticlesSize = 0;
}

int32 FClothingSimulationSolver::AddCollisionParticles(int32 NumCollisionParticles, uint32 GroupId, int32 RecycledOffset)
{
	// Try reusing the particle range
	// This is used by external collisions so that they can be added/removed between every solver update.
	// If it doesn't match then remove all ranges above the given offset to start again.
	// This rely on the assumption that these ranges are added again in the same update order.
	if (RecycledOffset == CollisionParticlesOffset + CollisionParticlesSize)
	{
		CollisionParticlesSize += NumCollisionParticles;

		// Check that the range still exists
		if (CollisionParticlesOffset + CollisionParticlesSize <= (int32)Evolution->CollisionParticles().Size() &&  // Check first that the range hasn't been reset
			NumCollisionParticles == Evolution->GetCollisionParticleRangeSize(RecycledOffset))  // This will assert if range has been reset
		{
			return RecycledOffset;
		}
		// Size has changed. must reset this collision range (and all of those following up) and reallocate some new particles
		Evolution->ResetCollisionParticles(RecycledOffset);
	}

	if (!NumCollisionParticles)
	{
		return INDEX_NONE;
	}

	const int32 Offset = Evolution->AddCollisionParticleRange(NumCollisionParticles, GroupId, /*bActivate =*/ false);

	// Always initialize the collision particle's transforms as otherwise setting the geometry will get NaNs detected during the bounding box updates
	FRotation3* const Rs = GetCollisionParticleRs(Offset);
	FVec3* const Xs = GetCollisionParticleXs(Offset);

	for (int32 Index = 0; Index < NumCollisionParticles; ++Index)
	{
		Xs[Index] = FVec3(0.f);
		Rs[Index] = FRotation3::FromIdentity();
	}

	// Always starts with particles disabled
	EnableCollisionParticles(Offset, false);

	return Offset;
}

void FClothingSimulationSolver::EnableCollisionParticles(int32 Offset, bool bEnable)
{
#if !UE_BUILD_SHIPPING
	if (bChaosClothSolverDisableCollision)
	{
		Evolution->ActivateCollisionParticleRange(Offset, false);
	}
	else
#endif  // #if !UE_BUILD_SHIPPING
	{
		Evolution->ActivateCollisionParticleRange(Offset, bEnable);
	}
}

const FVec3* FClothingSimulationSolver::GetCollisionParticleXs(int32 Offset) const
{
	return &Evolution->CollisionParticles().X(Offset);
}

FVec3* FClothingSimulationSolver::GetCollisionParticleXs(int32 Offset)
{
	return &Evolution->CollisionParticles().X(Offset);
}

const FRotation3* FClothingSimulationSolver::GetCollisionParticleRs(int32 Offset) const
{
	return &Evolution->CollisionParticles().R(Offset);
}

FRotation3* FClothingSimulationSolver::GetCollisionParticleRs(int32 Offset)
{
	return &Evolution->CollisionParticles().R(Offset);
}

void FClothingSimulationSolver::SetCollisionGeometry(int32 Offset, int32 Index, TUniquePtr<FImplicitObject>&& Geometry)
{
	Evolution->CollisionParticles().SetDynamicGeometry(Offset + Index, MoveTemp(Geometry));
}

const TUniquePtr<FImplicitObject>* FClothingSimulationSolver::GetCollisionGeometries(int32 Offset) const
{
	return &Evolution->CollisionParticles().DynamicGeometry(Offset);
}

const bool* FClothingSimulationSolver::GetCollisionStatus(int32 Offset) const
{
	return Evolution->GetCollisionStatus().GetData() + Offset;
}

const TArray<FVec3>& FClothingSimulationSolver::GetCollisionContacts() const
{
	return Evolution->GetCollisionContacts();
}

const TArray<FVec3>& FClothingSimulationSolver::GetCollisionNormals() const
{
	return Evolution->GetCollisionNormals();
}

void FClothingSimulationSolver::SetParticleMassUniform(int32 Offset, FReal UniformMass, FReal MinPerParticleMass, const FTriangleMesh& Mesh, const TFunctionRef<bool(int32)>& KinematicPredicate)
{
	// Retrieve the particle block size
	const int32 Size = Evolution->GetParticleRangeSize(Offset);

	// Set mass from uniform mass
	const TSet<int32> Vertices = Mesh.GetVertices();
	FPBDParticles& Particles = Evolution->Particles();
	for (int32 Index = Offset; Index < Offset + Size; ++Index)
	{
		Particles.M(Index) = Vertices.Contains(Index) ? UniformMass : 0.f;
	}

	// Clamp and enslave
	ParticleMassClampAndEnslave(Offset, Size, MinPerParticleMass, KinematicPredicate);
}

void FClothingSimulationSolver::SetParticleMassFromTotalMass(int32 Offset, FReal TotalMass, FReal MinPerParticleMass, const FTriangleMesh& Mesh, const TFunctionRef<bool(int32)>& KinematicPredicate)
{
	// Retrieve the particle block size
	const int32 Size = Evolution->GetParticleRangeSize(Offset);

	// Set mass per area
	const FReal TotalArea = SetParticleMassPerArea(Offset, Size, Mesh);

	// Find density
	const FReal Density = TotalArea > 0.f ? TotalMass / TotalArea : 1.f;

	// Update mass from mesh and density
	ParticleMassUpdateDensity(Mesh, Density);

	// Clamp and enslave
	ParticleMassClampAndEnslave(Offset, Size, MinPerParticleMass, KinematicPredicate);
}

void FClothingSimulationSolver::SetParticleMassFromDensity(int32 Offset, FReal Density, FReal MinPerParticleMass, const FTriangleMesh& Mesh, const TFunctionRef<bool(int32)>& KinematicPredicate)
{
	// Retrieve the particle block size
	const int32 Size = Evolution->GetParticleRangeSize(Offset);

	// Set mass per area
	const FReal TotalArea = SetParticleMassPerArea(Offset, Size, Mesh);

	// Set density from cm2 to m2
	Density /= FMath::Square(ChaosClothingSimulationSolverConstant::WorldScale);

	// Update mass from mesh and density
	ParticleMassUpdateDensity(Mesh, Density);

	// Clamp and enslave
	ParticleMassClampAndEnslave(Offset, Size, MinPerParticleMass, KinematicPredicate);
}

void FClothingSimulationSolver::SetReferenceVelocityScale(
	uint32 GroupId,
	const FRigidTransform3& OldReferenceSpaceTransform,
	const FRigidTransform3& ReferenceSpaceTransform,
	const FVec3& LinearVelocityScale,
	FReal AngularVelocityScale,
	FReal FictitiousAngularScale)
{
	FRigidTransform3 OldRootBoneLocalTransform = OldReferenceSpaceTransform;
	OldRootBoneLocalTransform.AddToTranslation(-OldLocalSpaceLocation);

	// Calculate deltas
	const FRigidTransform3 DeltaTransform = ReferenceSpaceTransform.GetRelativeTransform(OldReferenceSpaceTransform);

	// Apply linear velocity scale
	const FVec3 LinearRatio = FVec3(1.f) - LinearVelocityScale.BoundToBox(FVec3(0.f), FVec3(1.f));
	const FVec3 DeltaPosition = LinearRatio * DeltaTransform.GetTranslation();

	// Apply angular velocity scale
	FRotation3 DeltaRotation = DeltaTransform.GetRotation();
	FReal DeltaAngle = DeltaRotation.GetAngle();
	FVec3 Axis = DeltaRotation.GetRotationAxis();
	if (DeltaAngle > PI)
	{
		DeltaAngle -= 2.f * PI;
	}

	const FReal PartialDeltaAngle = DeltaAngle * FMath::Clamp(1.f - AngularVelocityScale, 0.f, 1.f);
	DeltaRotation = UE::Math::TQuat<FReal>(Axis, PartialDeltaAngle);

	// Transform points back into the previous frame of reference before applying the adjusted deltas 
	PreSimulationTransforms[GroupId] = OldRootBoneLocalTransform.Inverse() * FRigidTransform3(DeltaPosition, DeltaRotation) * OldRootBoneLocalTransform;

	// Save the reference bone relative angular velocity for calculating the fictitious forces
	FictitiousAngularDisplacement[GroupId] = ReferenceSpaceTransform.TransformVector(Axis * PartialDeltaAngle * FMath::Min(2.f, FictitiousAngularScale));  // Clamp to 2x the delta angle
}

FReal FClothingSimulationSolver::SetParticleMassPerArea(int32 Offset, int32 Size, const FTriangleMesh& Mesh)
{
	// Zero out masses
	FPBDParticles& Particles = Evolution->Particles();
	for (int32 Index = Offset; Index < Offset + Size; ++Index)
	{
		Particles.M(Index) = 0.f;
	}

	// Assign per particle mass proportional to connected area.
	const TArray<TVec3<int32>>& SurfaceElements = Mesh.GetSurfaceElements();
	FReal TotalArea = (FReal)0.f;
	for (const TVec3<int32>& Tri : SurfaceElements)
	{
		const FReal TriArea = 0.5f * FVec3::CrossProduct(
			Particles.X(Tri[1]) - Particles.X(Tri[0]),
			Particles.X(Tri[2]) - Particles.X(Tri[0])).Size();
		TotalArea += TriArea;
		const FReal ThirdTriArea = TriArea / 3.f;
		Particles.M(Tri[0]) += ThirdTriArea;
		Particles.M(Tri[1]) += ThirdTriArea;
		Particles.M(Tri[2]) += ThirdTriArea;
	}

	UE_LOG(LogChaosCloth, Verbose, TEXT("Total area: %f, SI total area: %f"), TotalArea, TotalArea / FMath::Square(ChaosClothingSimulationSolverConstant::WorldScale));
	return TotalArea;
}

void FClothingSimulationSolver::ParticleMassUpdateDensity(const FTriangleMesh& Mesh, FReal Density)
{
	const TSet<int32> Vertices = Mesh.GetVertices();
	FPBDParticles& Particles = Evolution->Particles();
	FReal TotalMass = 0.f;
	for (const int32 Vertex : Vertices)
	{
		Particles.M(Vertex) *= Density;
		TotalMass += Particles.M(Vertex);
	}

	UE_LOG(LogChaosCloth, Verbose, TEXT("Total mass: %f, "), TotalMass);
}

void FClothingSimulationSolver::ParticleMassClampAndEnslave(int32 Offset, int32 Size, FReal MinPerParticleMass, const TFunctionRef<bool(int32)>& KinematicPredicate)
{
	FPBDParticles& Particles = Evolution->Particles();
	for (int32 Index = Offset; Index < Offset + Size; ++Index)
	{
		Particles.M(Index) = FMath::Max(Particles.M(Index), (FReal)MinPerParticleMass);
		Particles.InvM(Index) = KinematicPredicate(Index - Offset) ? 0.f : 1.f / Particles.M(Index);
	}
}

void FClothingSimulationSolver::SetProperties(uint32 GroupId, FRealSingle DampingCoefficient, FRealSingle CollisionThickness, FRealSingle FrictionCoefficient)
{
	Evolution->SetDamping(DampingCoefficient, GroupId);
	Evolution->SetCollisionThickness(CollisionThickness, GroupId);
	Evolution->SetCoefficientOfFriction(FrictionCoefficient, GroupId);
}

void FClothingSimulationSolver::SetUseCCD(uint32 GroupId, bool bUseCCD)
{
	Evolution->SetUseCCD(bUseCCD, GroupId);
}

void FClothingSimulationSolver::SetGravity(uint32 GroupId, const FVec3& InGravity)
{
	Evolution->GetGravityForces(GroupId).SetAcceleration(InGravity);
}

void FClothingSimulationSolver::SetWindVelocity(const FVec3& InWindVelocity, FRealSingle InLegacyWindAdaption)
{
	WindVelocity = InWindVelocity * ChaosClothingSimulationSolverConstant::WorldScale;
	LegacyWindAdaption = (FReal)InLegacyWindAdaption;
}

void FClothingSimulationSolver::SetWindVelocity(uint32 GroupId, const FVec3& InWindVelocity)
{
	FVelocityField& VelocityField = Evolution->GetVelocityField(GroupId);
	VelocityField.SetVelocity(InWindVelocity);
}

void FClothingSimulationSolver::SetWindGeometry(uint32 GroupId, const FTriangleMesh& TriangleMesh, const TConstArrayView<FRealSingle>& DragMultipliers, const TConstArrayView<FRealSingle>& LiftMultipliers)
{
	FVelocityField& VelocityField = Evolution->GetVelocityField(GroupId);
	VelocityField.SetGeometry(&TriangleMesh, DragMultipliers, LiftMultipliers);
}

void FClothingSimulationSolver::SetWindProperties(uint32 GroupId, const FVec2& Drag, const FVec2& Lift, FReal AirDensity)
{
	FVelocityField& VelocityField = Evolution->GetVelocityField(GroupId);
	VelocityField.SetProperties(Drag, Lift, AirDensity);
}

const FVelocityField& FClothingSimulationSolver::GetWindVelocityField(uint32 GroupId)
{
	return Evolution->GetVelocityField(GroupId);
}

void FClothingSimulationSolver::AddExternalForces(uint32 GroupId, bool bUseLegacyWind)
{
	if (Evolution)
	{
		const bool bHasVelocityField = !PerSolverField.GetOutputResults(EFieldCommandOutputType::LinearVelocity).IsEmpty();
		const bool bHasForceField = !PerSolverField.GetOutputResults(EFieldCommandOutputType::LinearForce).IsEmpty();

		const FVec3& AngularDisplacement = FictitiousAngularDisplacement[GroupId];
		const bool bHasFictitiousForces = !AngularDisplacement.IsNearlyZero();

		static const FReal LegacyWindMultiplier = (FReal)25.;
		const FVec3 LegacyWindVelocity = WindVelocity * LegacyWindMultiplier;

		Evolution->GetForceFunction(GroupId) =
			[this, bHasVelocityField, bHasForceField, bHasFictitiousForces, bUseLegacyWind, LegacyWindVelocity, AngularDisplacement](FPBDParticles& Particles, const FReal Dt, const int32 Index)
			{
				FVec3 Forces((FReal)0.);

				if (bHasVelocityField)
				{
					const TArray<FVector>& LinearVelocities = PerSolverField.GetOutputResults(EFieldCommandOutputType::LinearVelocity);
					Forces += LinearVelocities[Index] * Particles.M(Index) / Dt;
				}

				if (bHasForceField)
				{
					const TArray<FVector>& LinearForces = PerSolverField.GetOutputResults(EFieldCommandOutputType::LinearForce);
					Forces += LinearForces[Index];
				}

				if (bHasFictitiousForces)
				{
					const FVec3& X = Particles.X(Index);
					const FVec3 W = AngularDisplacement / Dt;
					const FReal& M = Particles.M(Index);
#if 0
					// Coriolis + Centrifugal seems a bit overkilled, but let's keep the code around in case it's ever required
					const FVec3& V = Particles.V(Index);
					Forces -= (FVec3::CrossProduct(W, V) * 2.f + FVec3::CrossProduct(W, FVec3::CrossProduct(W, X))) * M;
#else
					// Centrifugal force
					Forces -= FVec3::CrossProduct(W, FVec3::CrossProduct(W, X)) * M;
#endif
				}
				
				if (bUseLegacyWind)
				{
					// Calculate wind velocity delta
					const FVec3 VelocityDelta = LegacyWindVelocity - Particles.V(Index);

					FVec3 Direction = VelocityDelta;
					if (Direction.Normalize())
					{
						// Scale by angle
						const FReal DirectionDot = FVec3::DotProduct(Direction, Normals[Index]);
						const FReal ScaleFactor = FMath::Min(1.f, FMath::Abs(DirectionDot) * LegacyWindAdaption);
						Forces += VelocityDelta * ScaleFactor * Particles.M(Index);
					}
				}

				Particles.F(Index) += Forces;
			};
	}
}

void FClothingSimulationSolver::ApplyPreSimulationTransforms()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_ApplyPreSimulationTransforms);
	const FVec3 DeltaLocalSpaceLocation = LocalSpaceLocation - OldLocalSpaceLocation;

	const TPBDActiveView<FPBDParticles>& ParticlesActiveView = Evolution->ParticlesActiveView();
	const TArray<uint32>& ParticleGroupIds = Evolution->ParticleGroupIds();

	ParticlesActiveView.RangeFor(
		[this, &ParticleGroupIds, &DeltaLocalSpaceLocation](FPBDParticles& Particles, int32 Offset, int32 Range)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_ParticlePreSimulationTransforms);
			SCOPE_CYCLE_COUNTER(STAT_ChaosClothParticlePreSimulationTransforms);

			const int32 RangeSize = Range - Offset;

			if (bChaos_PreSimulationTransforms_ISPC_Enabled)
			{
#if INTEL_ISPC
				ispc::ApplyPreSimulationTransforms(
					(ispc::FVector*)Particles.GetP().GetData(),
					(ispc::FVector*)Particles.GetV().GetData(),
					(ispc::FVector*)Particles.XArray().GetData(),
					(ispc::FVector*)OldAnimationPositions.GetData(),
					ParticleGroupIds.GetData(),
					(ispc::FTransform*)PreSimulationTransforms.GetData(),
					(ispc::FVector&)DeltaLocalSpaceLocation,
					Offset,
					Range);
#endif
			}
			else
			{
			PhysicsParallelFor(RangeSize,
				[this, &ParticleGroupIds, &DeltaLocalSpaceLocation, &Particles, Offset](int32 i)
				{
					const int32 Index = Offset + i;
					const FRigidTransform3& GroupSpaceTransform = PreSimulationTransforms[ParticleGroupIds[Index]];

					// Update initial state for particles
					Particles.P(Index) = Particles.X(Index) = GroupSpaceTransform.TransformPositionNoScale(Particles.X(Index)) - DeltaLocalSpaceLocation;
					Particles.V(Index) = GroupSpaceTransform.TransformVector(Particles.V(Index));

					// Update anim initial state (target updated by skinning)
					OldAnimationPositions[Index] = GroupSpaceTransform.TransformPositionNoScale(OldAnimationPositions[Index]) - DeltaLocalSpaceLocation;
				}, RangeSize < ChaosClothSolverMinParallelBatchSize);
			}
		}, /*bForceSingleThreaded =*/ !bChaosClothSolverParallelClothPreUpdate);

#if FRAMEPRO_ENABLED
	FRAMEPRO_CUSTOM_STAT("ChaosClothSolverMinParallelBatchSize", ChaosClothSolverMinParallelBatchSize, "ChaosClothSolver", "Particles", FRAMEPRO_COLOUR(128,0,255));
	FRAMEPRO_CUSTOM_STAT("ChaosClothSolverParallelClothPreUpdate", bChaosClothSolverParallelClothPreUpdate, "ChaosClothSolver", "Enabled", FRAMEPRO_COLOUR(128, 128, 64));
#endif

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_CollisionPreSimulationTransforms);
		SCOPE_CYCLE_COUNTER(STAT_ChaosClothCollisionPreSimulationTransforms);

		const TPBDActiveView<FKinematicGeometryClothParticles>& CollisionParticlesActiveView = Evolution->CollisionParticlesActiveView();
		const TArray<uint32>& CollisionParticleGroupIds = Evolution->CollisionParticleGroupIds();

		CollisionParticlesActiveView.SequentialFor(  // There's unlikely to ever have enough collision particles for a parallel for
			[this, &CollisionParticleGroupIds, &DeltaLocalSpaceLocation](FKinematicGeometryClothParticles& CollisionParticles, int32 Index)
			{
				const FRigidTransform3& GroupSpaceTransform = PreSimulationTransforms[CollisionParticleGroupIds[Index]];

				// Update initial state for collisions
				OldCollisionTransforms[Index] = OldCollisionTransforms[Index] * GroupSpaceTransform;
				OldCollisionTransforms[Index].AddToTranslation(-DeltaLocalSpaceLocation);
				CollisionParticles.X(Index) = OldCollisionTransforms[Index].GetTranslation();
				CollisionParticles.R(Index) = OldCollisionTransforms[Index].GetRotation();
			});
	}
}

void FClothingSimulationSolver::UpdateSolverField()
{
	if (Evolution && !PerSolverField.IsEmpty())
	{
		TArray<FVector>& SamplePositions = PerSolverField.GetSamplePositions();
		TArray<FFieldContextIndex>& SampleIndices = PerSolverField.GetSampleIndices();

		const uint32 NumParticles = Evolution->Particles().Size();

		SamplePositions.SetNum(NumParticles, false);
		SampleIndices.SetNum(NumParticles, false);

		for (uint32 Index = 0; Index < NumParticles; ++Index)
		{
			SamplePositions[Index] = Evolution->Particles().X(Index) + LocalSpaceLocation;
			SampleIndices[Index] = FFieldContextIndex(Index, Index);
		}
		PerSolverField.ComputeFieldLinearImpulse(GetTime());
	}
}

void FClothingSimulationSolver::Update(FReal InDeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_Update);
	SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverUpdate);

	if (!bChaosClothSolverUseImprovedTimeStepSmoothing)
	{
		// Filter delta time to smoothen time variations and prevent unwanted vibrations
		// Note: This is now deprecated and replaced by in solver input force timestep smoothing
		static const FReal DeltaTimeDecay = 0.1f;
		const FReal PrevDeltaTime = DeltaTime;
		DeltaTime = DeltaTime + (InDeltaTime - DeltaTime) * DeltaTimeDecay;
	}
	else
	{
		// Update time step
		DeltaTime = InDeltaTime;
	}

#if !UE_BUILD_SHIPPING
	// Introduce artificial hitches for debugging any simulation jitter
	if (ChaosClothSolverDebugHitchLength && ChaosClothSolverDebugHitchInterval)
	{
		static int32 HitchCounter = 0;
		if (--HitchCounter < 0)
		{
			UE_LOG(LogChaosCloth, Warning, TEXT("Hitching for %dms"), ChaosClothSolverDebugHitchLength);
			FPlatformProcess::Sleep((float)ChaosClothSolverDebugHitchLength * 0.001f);
			HitchCounter = ChaosClothSolverDebugHitchInterval;
		}
	}
#endif  // #if !UE_BUILD_SHIPPING

	// Update Cloths and cloth colliders
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_UpdateCloths);
		SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverUpdateCloths);

		Swap(OldCollisionTransforms, CollisionTransforms);
		Swap(OldAnimationPositions, AnimationPositions);

		// Clear external collisions so that they can be re-added
		CollisionParticlesSize = 0;

		// Compute the solver field forces/velocities for future use in the AddExternalForces
		UpdateSolverField();

		// Run sequential pre-updates first
		for (FClothingSimulationCloth* const Cloth : Cloths)
		{
			Cloth->PreUpdate(this);
		}

		// Run parallel update
		PhysicsParallelFor(Cloths.Num(), [this](int32 ClothIndex)
		{
			FClothingSimulationCloth* const Cloth = Cloths[ClothIndex];
			const uint32 GroupId = Cloth->GetGroupId();

			// Pre-update overridable solver properties first
			Evolution->GetGravityForces(GroupId).SetAcceleration(Gravity);
			Evolution->GetVelocityField(GroupId).SetVelocity(WindVelocity);

			Cloth->Update(this);
		}, /*bForceSingleThreaded =*/ !bChaosClothSolverParallelClothUpdate);
	}

	// Pre solver step, apply group space transforms for teleport and linear/delta ratios, ...etc
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_UpdatePreSolverStep);
		SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverUpdatePreSolverStep);

		ApplyPreSimulationTransforms();
	}

	// Advance Sim
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_UpdateSolverStep);
		SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverUpdateSolverStep);
		SCOPE_CYCLE_COUNTER(STAT_ClothInternalSolve);

		Evolution->SetIterations(NumIterations);

		const FReal SubstepDeltaTime = DeltaTime / (FReal)NumSubsteps;
	
		for (int32 i = 0; i < NumSubsteps; ++i)
		{
			Evolution->AdvanceOneTimeStep(SubstepDeltaTime, bChaosClothSolverUseImprovedTimeStepSmoothing);
		}

		Time = Evolution->GetTime();
		UE_LOG(LogChaosCloth, VeryVerbose, TEXT("DeltaTime: %.6f, Time = %.6f"), DeltaTime, Time);
	}

	// Post solver step, update normals, ...etc
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_UpdatePostSolverStep);
		SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverUpdatePostSolverStep);
		SCOPE_CYCLE_COUNTER(STAT_ClothComputeNormals);

		PhysicsParallelFor(Cloths.Num(), [this](int32 ClothIndex)
		{
			FClothingSimulationCloth* const Cloth = Cloths[ClothIndex];
			Cloth->PostUpdate(this);
		}, /*bForceSingleThreaded =*/ !bChaosClothSolverParallelClothPostUpdate);
	}

	// Save old space location for next update
	OldLocalSpaceLocation = LocalSpaceLocation;
}

FBoxSphereBounds FClothingSimulationSolver::CalculateBounds() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FClothingSimulationSolver_CalculateBounds);
	SCOPE_CYCLE_COUNTER(STAT_ChaosClothSolverCalculateBounds);

	const TPBDActiveView<FPBDParticles>& ParticlesActiveView = Evolution->ParticlesActiveView();

	if (ParticlesActiveView.HasActiveRange())
	{
		// Calculate bounding box
		FAABB3 BoundingBox = FAABB3::EmptyAABB();

#if INTEL_ISPC
		if (bChaos_CalculateBounds_ISPC_Enabled && bRealTypeCompatibleWithISPC)
		{
			ParticlesActiveView.RangeFor(
				[&BoundingBox](FPBDParticles& Particles, int32 Offset, int32 Range)
				{
					FVec3 NewMin = BoundingBox.Min();
					FVec3 NewMax = BoundingBox.Max();

					ispc::CalculateBounds(
						(ispc::FVector&)NewMin,
						(ispc::FVector&)NewMax,
						(const ispc::FVector*)Particles.XArray().GetData(),
						Offset,
						Range);

					TAABB<float, 3> NewAABB(NewMin, NewMax);
					BoundingBox = NewAABB;
				});
		}
		else
#endif
		{
		ParticlesActiveView.SequentialFor(
			[&BoundingBox](FPBDParticles& Particles, int32 Index)
			{
				BoundingBox.GrowToInclude(Particles.X(Index));
			});
		}

		// Calculate (squared) radius
		const FVec3 Center = BoundingBox.Center();
		FReal SquaredRadius = 0.f;

		if (bChaos_CalculateBounds_ISPC_Enabled)
		{
#if INTEL_ISPC
			ParticlesActiveView.RangeFor(
				[&SquaredRadius, &Center](FPBDParticles& Particles, int32 Offset, int32 Range)
				{
					ispc::CalculateSquaredRadius(
						SquaredRadius,
						(const ispc::FVector&)Center,
						(const ispc::FVector*)Particles.XArray().GetData(),
						Offset,
						Range);
				});
#endif
		}
		else
		{
		ParticlesActiveView.SequentialFor(
			[&SquaredRadius, &Center](FPBDParticles& Particles, int32 Index)
			{
				SquaredRadius = FMath::Max(SquaredRadius, (Particles.X(Index) - Center).SizeSquared());
			});
		}

		// Update bounds with this cloth
		return FBoxSphereBounds(LocalSpaceLocation + BoundingBox.Center(), BoundingBox.Extents() * 0.5f, FMath::Sqrt(SquaredRadius));
	}

	return FBoxSphereBounds(LocalSpaceLocation, FVector(0.f), 0.f);
}
