// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Collision/CollisionConstraintAllocator.h"
#include "Chaos/Collision/PBDCollisionConstraintHandle.h"
#include "Chaos/Collision/SolverCollisionContainer.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Evolution/SolverBody.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDCollisionConstraints.h"

// Private includes
#include "PBDCollisionSolver.h"


//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{
	FRealSingle Chaos_Manifold_MatchPositionTolerance = 0.3f;		// Fraction of object size position tolerance
	FRealSingle Chaos_Manifold_MatchNormalTolerance = 0.02f;		// Dot product tolerance
	FAutoConsoleVariableRef CVarChaos_Manifold_MatchPositionTolerance(TEXT("p.Chaos.Collision.Manifold.MatchPositionTolerance"), Chaos_Manifold_MatchPositionTolerance, TEXT("A tolerance as a fraction of object size used to determine if two contact points are the same"));
	FAutoConsoleVariableRef CVarChaos_Manifold_MatchNormalTolerance(TEXT("p.Chaos.Collision.Manifold.MatchNormalTolerance"), Chaos_Manifold_MatchNormalTolerance, TEXT("A tolerance on the normal dot product used to determine if two contact points are the same"));

	FRealSingle Chaos_Manifold_FrictionPositionTolerance = 1.0f;	// Distance a shape-relative contact point can move and still be considered the same point
	FAutoConsoleVariableRef CVarChaos_Manifold_FrictionPositionTolerance(TEXT("p.Chaos.Collision.Manifold.FrictionPositionTolerance"), Chaos_Manifold_FrictionPositionTolerance, TEXT(""));

	FRealSingle Chaos_GBFCharacteristicTimeRatio = 1.0f;
	FAutoConsoleVariableRef CVarChaos_GBFCharacteristicTimeRatio(TEXT("p.Chaos.Collision.GBFCharacteristicTimeRatio"), Chaos_GBFCharacteristicTimeRatio, TEXT("The ratio between characteristic time and Dt"));

	bool bChaos_Manifold_EnabledWithJoints = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnabledWithJoints(TEXT("p.Chaos.Collision.Manifold.EnabledWithJoints"), bChaos_Manifold_EnabledWithJoints, TEXT(""));

	bool bChaos_Manifold_EnableGjkWarmStart = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnableGjkWarmStart(TEXT("p.Chaos.Collision.Manifold.EnableGjkWarmStart"), bChaos_Manifold_EnableGjkWarmStart, TEXT(""));

	bool bChaos_Manifold_EnableFrictionRestore = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnableFrictionRestore(TEXT("p.Chaos.Collision.Manifold.EnableFrictionRestore"), bChaos_Manifold_EnableFrictionRestore, TEXT(""));

	extern bool bChaos_Collision_Manifold_FixNormalsInWorldSpace;

	FString FPBDCollisionConstraint::ToString() const
	{
		return FString::Printf(TEXT("Particle:%s, Levelset:%s, AccumulatedImpulse:%s"), *Particle[0]->ToString(), *Particle[1]->ToString(), *AccumulatedImpulse.ToString());
	}

	bool ContactConstraintSortPredicate(const FPBDCollisionConstraint& L, const FPBDCollisionConstraint& R)
	{
		//sort constraints by the smallest particle idx in them first
		//if the smallest particle idx is the same for both, use the other idx

		if (L.GetCCDType() != R.GetCCDType())
		{
			return L.GetCCDType() < R.GetCCDType();
		}

		const FParticleID ParticleIdxs[] = { L.Particle[0]->ParticleID(), L.Particle[1]->ParticleID() };
		const FParticleID OtherParticleIdxs[] = { R.Particle[0]->ParticleID(), R.Particle[1]->ParticleID() };

		const int32 MinIdx = ParticleIdxs[0] < ParticleIdxs[1] ? 0 : 1;
		const int32 OtherMinIdx = OtherParticleIdxs[0] < OtherParticleIdxs[1] ? 0 : 1;

		if(ParticleIdxs[MinIdx] < OtherParticleIdxs[OtherMinIdx])
		{
			return true;
		} 
		else if(ParticleIdxs[MinIdx] == OtherParticleIdxs[OtherMinIdx])
		{
			return ParticleIdxs[!MinIdx] < OtherParticleIdxs[!OtherMinIdx];
		}

		return false;
	}

	TUniquePtr<FPBDCollisionConstraint> FPBDCollisionConstraint::Make(
		FGeometryParticleHandle* Particle0,
		const FImplicitObject* Implicit0,
		const FBVHParticles* Simplicial0,
		const FRigidTransform3& ImplicitLocalTransform0,
		FGeometryParticleHandle* Particle1,
		const FImplicitObject* Implicit1,
		const FBVHParticles* Simplicial1,
		const FRigidTransform3& ImplicitLocalTransform1,
		const FReal InCullDistance,
		const bool bInUseManifold,
		const EContactShapesType ShapesType)
	{
		FPBDCollisionConstraint* Constraint = new FPBDCollisionConstraint(Particle0, Implicit0, Simplicial0, Particle1, Implicit1, Simplicial1);
		
		Constraint->Setup(ECollisionCCDType::Disabled, ShapesType, ImplicitLocalTransform0, ImplicitLocalTransform1, InCullDistance, bInUseManifold);

		return TUniquePtr<FPBDCollisionConstraint>(Constraint);
	}

	FPBDCollisionConstraint FPBDCollisionConstraint::MakeTriangle(const FImplicitObject* Implicit0)
	{
		FPBDCollisionConstraint Constraint;
		Constraint.InitMargins(Implicit0->GetCollisionType(), ImplicitObjectType::Triangle, Implicit0->GetMargin(), FReal(0));
		return Constraint;
	}

	FPBDCollisionConstraint FPBDCollisionConstraint::MakeCopy(
		const FPBDCollisionConstraint& Source)
	{
		// @todo(chaos): The resim cache version probably doesn't need all the data, so maybe try to cut this down?
		FPBDCollisionConstraint Constraint = Source;

		// Invalidate the data that maps the constraint to its container (we are no longer in the container)
		// @todo(chaos): this should probably be handled by the copy constructor
		Constraint.GetContainerCookie().ClearContainerData();

		return Constraint;
	}

	FPBDCollisionConstraint::FPBDCollisionConstraint()
		: ImplicitTransform{ FRigidTransform3(), FRigidTransform3() }
		, Particle{ nullptr, nullptr }
		, AccumulatedImpulse(0)
		, Manifold()
		, TimeOfImpact(0)
		, CCDType(ECollisionCCDType::Disabled)
		, Stiffness(FReal(1))
		, CullDistance(TNumericLimits<FReal>::Max())
		, CollisionMargins{ 0, 0 }
		, CollisionTolerance(0)
		, bUseManifold(false)
		, bUseIncrementalManifold(false)
		, SolverBodies{ nullptr, nullptr }
		, GJKWarmStartData()
		, ManifoldPointSavedData{}
		, NumSavedManifoldPoints(0)
		, LastShapeWorldTransform0()
		, LastShapeWorldTransform1()
		, ExpectedNumManifoldPoints(0)
		, bWasManifoldRestored(false)
		, NumActivePositionIterations(0)
	{
		Manifold.Implicit[0] = nullptr;
		Manifold.Implicit[1] = nullptr;
		Manifold.Simplicial[0] = nullptr;
		Manifold.Simplicial[1] = nullptr;
		Manifold.ShapesType = EContactShapesType::Unknown;
	}

	FPBDCollisionConstraint::FPBDCollisionConstraint(
		FGeometryParticleHandle* Particle0,
		const FImplicitObject* Implicit0,
		const FBVHParticles* Simplicial0,
		FGeometryParticleHandle* Particle1,
		const FImplicitObject* Implicit1,
		const FBVHParticles* Simplicial1)
		: ImplicitTransform{ FRigidTransform3(), FRigidTransform3() }
		, Particle{ Particle0, Particle1 }
		, AccumulatedImpulse(0)
		, Manifold()
		, TimeOfImpact(0)
		, CCDType(ECollisionCCDType::Disabled)
		, Stiffness(FReal(1))
		, CullDistance(TNumericLimits<FReal>::Max())
		, CollisionMargins{ 0, 0 }
		, CollisionTolerance(0)
		, bUseManifold(false)
		, bUseIncrementalManifold(false)
		, SolverBodies{ nullptr, nullptr }
		, GJKWarmStartData()
		, ManifoldPointSavedData{}
		, NumSavedManifoldPoints(0)
		, LastShapeWorldTransform0()
		, LastShapeWorldTransform1()
		, ExpectedNumManifoldPoints(0)
		, bWasManifoldRestored(false)
	{
		Manifold.Implicit[0] = Implicit0;
		Manifold.Implicit[1] = Implicit1;
		Manifold.Simplicial[0] = Simplicial0;
		Manifold.Simplicial[1] = Simplicial1;
		Manifold.ShapesType = EContactShapesType::Unknown;
	}

	void FPBDCollisionConstraint::Setup(
		const ECollisionCCDType InCCDType,
		const EContactShapesType InShapesType,
		const FRigidTransform3& InImplicitLocalTransform0,
		const FRigidTransform3& InImplicitLocalTransform1,
		const FReal InCullDistance,
		const bool bInUseManifold)
	{
		CCDType = InCCDType;

		Manifold.ShapesType = InShapesType;

		ImplicitTransform[0] = InImplicitLocalTransform0;
		ImplicitTransform[1] = InImplicitLocalTransform1;

		CullDistance = InCullDistance;

		bUseManifold = bInUseManifold && CanUseManifold(Particle[0], Particle[1]);
		bUseIncrementalManifold = true;	// This will get changed later if we call AddOneShotManifoldContact

		const FReal Margin0 = GetImplicit0()->GetMargin();
		const FReal Margin1 = GetImplicit1()->GetMargin();
		const EImplicitObjectType ImplicitType0 = GetInnerType(GetImplicit0()->GetCollisionType());
		const EImplicitObjectType ImplicitType1 = GetInnerType(GetImplicit1()->GetCollisionType());
		InitMargins(ImplicitType0, ImplicitType1, Margin0, Margin1);
	}

	void FPBDCollisionConstraint::InitMargins(const EImplicitObjectType ImplicitType0, const EImplicitObjectType ImplicitType1, const FReal Margin0, const FReal Margin1)
	{
		// Set up the margins and tolerances to be used during the narrow phase.
		// One shape in a collision will always have a margin. Only triangles have zero margin and we don't 
		// collide two triangles. If we have a triangle, it is always the second shape.
		// The collision tolerance is used for knowing whether a new contact matches an existing one.
		// If we have two non-quadratic shapes, we use the smallest margin on both shapes.
		// If we have a quadratic shape versus a non-quadratic, we don't need a margin on the non-quadratic.
		// For non-quadratics the collision tolerance is the smallest non-zero margin. 
		// For quadratic shapes we want a collision tolerance much smaller than the radius.
		const bool bIsQuadratic0 = ((ImplicitType0 == ImplicitObjectType::Sphere) || (ImplicitType0 == ImplicitObjectType::Capsule));
		const bool bIsQuadratic1 = ((ImplicitType1 == ImplicitObjectType::Sphere) || (ImplicitType1 == ImplicitObjectType::Capsule));
		const FReal QuadraticToleranceScale = 0.05f;
		if (!bIsQuadratic0 && !bIsQuadratic1)
		{
			CollisionMargins[0] = FMath::Min(Margin0, Margin1);
			CollisionMargins[1] = CollisionMargins[0];
			CollisionTolerance = ((Margin0 < Margin1) || (Margin1 == 0)) ? Margin0 : Margin1;
		}
		else if (bIsQuadratic0 && bIsQuadratic1)
		{
			CollisionMargins[0] = Margin0;
			CollisionMargins[1] = Margin1;
			CollisionTolerance = QuadraticToleranceScale * FMath::Min(Margin0, Margin1);
		}
		else if (bIsQuadratic0 && !bIsQuadratic1)
		{
			CollisionMargins[0] = Margin0;
			CollisionMargins[1] = 0;
			CollisionTolerance = QuadraticToleranceScale * Margin0;
		}
		else if (!bIsQuadratic0 && bIsQuadratic1)
		{
			CollisionMargins[0] = 0;
			CollisionMargins[1] = Margin1;
			CollisionTolerance = QuadraticToleranceScale * Margin1;
		}
	}

	void FPBDCollisionConstraint::SetIsSleeping(const bool bInIsSleeping)
	{
		// This actually sets the sleeping state on all constraints between the same particle pair so calling this with multiple
		// constraints on the same particle pair is a little wasteful. It early-outs on subsequent calls, but still not ideal.
		// @todo(chaos): we only need to set sleeping on particle pairs or particles, not constraints (See UpdateSleepState in IslandManager.cpp)
		check(ContainerCookie.MidPhase != nullptr);
		ContainerCookie.MidPhase->SetIsSleeping(bInIsSleeping);
	}

	// Are the two manifold points the same point?
	// Ideally a contact is considered the same as one from the previous iteration if
	//		The contact is Vertex - Face and there was a prior iteration collision on the same Vertex
	//		The contact is Edge - Edge and a prior iteration collision contained both edges
	//		The contact is Face - Face and a prior iteration contained both faces
	//
	// But we don’t have feature IDs. So in the meantime contact points will be considered the "same" if
	//		Vertex - Face - the local space contact position on either body is within some tolerance
	//		Edge - Edge - ?? hard...
	//		Face - Face - ?? hard...
	//
	bool FPBDCollisionConstraint::AreMatchingContactPoints(const FContactPoint& A, const FContactPoint& B, FReal& OutScore) const
	{
		OutScore = 0.0f;

		// @todo(chaos): cache tolerances?
		FReal DistanceTolerance = 0.0f;
		if (Particle[0]->Geometry()->HasBoundingBox() && Particle[1]->Geometry()->HasBoundingBox())
		{
			const FReal Size0 = Particle[0]->Geometry()->BoundingBox().Extents().Max();
			const FReal Size1 = Particle[1]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = FMath::Min(Size0, Size1) * Chaos_Manifold_MatchPositionTolerance;
		}
		else if (Particle[0]->Geometry()->HasBoundingBox())
		{
			const FReal Size0 = Particle[0]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = Size0 * Chaos_Manifold_MatchPositionTolerance;
		}
		else if (Particle[1]->Geometry()->HasBoundingBox())
		{
			const FReal Size1 = Particle[1]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = Size1 * Chaos_Manifold_MatchPositionTolerance;
		}
		else
		{
			return false;
		}
		const FReal NormalTolerance = Chaos_Manifold_MatchNormalTolerance;

		// If normal has changed a lot, it is a different contact
		// (This was only here to detect bad normals - it is not right for edge-edge contact tracking, but we don't do a good job of that yet anyway!)
		FReal NormalDot = FVec3::DotProduct(A.ShapeContactNormal, B.ShapeContactNormal);
		if (NormalDot < 1.0f - NormalTolerance)
		{
			return false;
		}

		// If either point in local space is the same, it is the same contact
		if (DistanceTolerance > 0.0f)
		{
			const FReal DistanceTolerance2 = DistanceTolerance * DistanceTolerance;
			for (int32 BodyIndex = 0; BodyIndex < 2; ++BodyIndex)
			{
				FVec3 DR = A.ShapeContactPoints[BodyIndex] - B.ShapeContactPoints[BodyIndex];
				FReal DRLen2 = DR.SizeSquared();
				if (DRLen2 < DistanceTolerance2)
				{
					OutScore = FMath::Clamp(1.f - DRLen2 / DistanceTolerance2, 0.f, 1.f);
					return true;
				}
			}
		}

		return false;
	}

	int32 FPBDCollisionConstraint::FindManifoldPoint(const FContactPoint& ContactPoint) const
	{
		const int32 NumManifoldPoints = ManifoldPoints.Num();
		int32 BestMatchIndex = INDEX_NONE;
		FReal BestMatchScore = 0.0f;
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			FReal Score = 0.0f;
			if (AreMatchingContactPoints(ContactPoint, ManifoldPoints[ManifoldPointIndex].ContactPoint, Score))
			{
				if (Score > BestMatchScore)
				{
					BestMatchIndex = ManifoldPointIndex;
					BestMatchScore = Score;

					// Just take the first one that meets the tolerances
					break;
				}
			}
		}
		return BestMatchIndex;
	}

	void FPBDCollisionConstraint::UpdateManifoldContacts()
	{
		FVec3 P0, P1;
		FRotation3 Q0, Q1;

		// @todo(chaos): Remove this when we don't need to support incremental manifolds (this will only be called on creation/restore)
		if ((GetSolverBody0() != nullptr) && (GetSolverBody1() != nullptr))
		{
			P0 = GetSolverBody0()->P();
			Q0 = GetSolverBody0()->Q();
			P1 = GetSolverBody1()->P();
			Q1 = GetSolverBody1()->Q();
		}
		else
		{
			// @todo(chaos): we should not need to regenerate the CoM transform
			P0 = FParticleUtilities::GetCoMWorldPosition(FConstGenericParticleHandle(Particle[0]));
			Q0 = FParticleUtilities::GetCoMWorldRotation(FConstGenericParticleHandle(Particle[0]));
			P1 = FParticleUtilities::GetCoMWorldPosition(FConstGenericParticleHandle(Particle[1]));
			Q1 = FParticleUtilities::GetCoMWorldRotation(FConstGenericParticleHandle(Particle[1]));
		}

		Manifold.Reset();

		for (int32 Index = 0; Index < ManifoldPoints.Num(); Index++)
		{
			FManifoldPoint& ManifoldPoint = ManifoldPoints[Index];
			GetWorldSpaceManifoldPoint(ManifoldPoint, P0, Q0, P1, Q1, ManifoldPoint.ContactPoint.Location, ManifoldPoint.ContactPoint.Phi);

			ManifoldPoint.bInsideStaticFrictionCone = bUseManifold;

			// Copy currently active point
			if (ManifoldPoint.ContactPoint.Phi < Manifold.Phi)
			{
				SetActiveContactPoint(ManifoldPoint.ContactPoint);
			}
		}
	}

	void FPBDCollisionConstraint::AddOneshotManifoldContact(const FContactPoint& ContactPoint)
	{
		if (ManifoldPoints.Num() == MaxManifoldPoints)
		{
			return;
		}

		int32 ManifoldPointIndex = AddManifoldPoint(ContactPoint);

		// Copy currently active point
		if (ManifoldPoints[ManifoldPointIndex].ContactPoint.Phi < Manifold.Phi)
		{
			SetActiveContactPoint(ManifoldPoints[ManifoldPointIndex].ContactPoint);
		}

		bUseIncrementalManifold = false;
	}

	void FPBDCollisionConstraint::AddIncrementalManifoldContact(const FContactPoint& ContactPoint)
	{
		if (ManifoldPoints.Num() == MaxManifoldPoints)
		{
			return;
		}

		if (bUseManifold)
		{
			// See if the manifold point already exists
			int32 ManifoldPointIndex = FindManifoldPoint(ContactPoint);
			if (ManifoldPointIndex >= 0)
			{
				// This contact point is already in the manifold - update the state
				UpdateManifoldPoint(ManifoldPointIndex, ContactPoint);
			}
			else
			{
				// This is a new manifold point - capture the state and generate initial properties
				ManifoldPointIndex = AddManifoldPoint(ContactPoint);
			}

			// Copy currently active point
			if (ManifoldPoints[ManifoldPointIndex].ContactPoint.Phi < Manifold.Phi)
			{
				SetActiveContactPoint(ManifoldPoints[ManifoldPointIndex].ContactPoint);
			}
		}
		else 
		{
			// We are not using manifolds - reuse the first and only point
			if (ManifoldPoints.Num() == 0)
			{
				ManifoldPoints.Add(ContactPoint);
			}
			else
			{
				ManifoldPoints[0].ContactPoint = ContactPoint;
			}

			InitManifoldPoint(0);

			SetActiveContactPoint(ManifoldPoints[0].ContactPoint);
		}

		bUseIncrementalManifold = true;
	}

	void FPBDCollisionConstraint::ClearManifold()
	{
		ManifoldPoints.Reset();
	}

	void FPBDCollisionConstraint::InitManifoldPoint(const int32 ManifoldPointIndex)
	{
		FConstGenericParticleHandle Particle0 = Particle[0];
		FConstGenericParticleHandle Particle1 = Particle[1];
		if (!Particle0.IsValid() || !Particle1.IsValid())
		{
			// @todo(chaos): This is just for unit tests testing one-shot manifolds - remove it somehow... 
			// maybe ConstructConvexConvexOneShotManifold should not take a Constraint
			return;
		}

		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
		ManifoldPoint.InitialShapeContactPoints[0] = ManifoldPoint.ContactPoint.ShapeContactPoints[0];
		ManifoldPoint.InitialShapeContactPoints[1] = ManifoldPoint.ContactPoint.ShapeContactPoints[1];

		// Update the derived contact state (CoM relative data)
		UpdateManifoldPointFromContact(ManifoldPointIndex);

		// Initialize the previous contact transforms if the data is available, otherwise reset them to current
		TryRestoreFrictionData(ManifoldPointIndex);
	}

	int32 FPBDCollisionConstraint::AddManifoldPoint(const FContactPoint& ContactPoint)
	{
		check(ManifoldPoints.Num() < MaxManifoldPoints);

		int32 ManifoldPointIndex = ManifoldPoints.Add(ContactPoint);

		InitManifoldPoint(ManifoldPointIndex);

		return ManifoldPointIndex;
	}

	void FPBDCollisionConstraint::UpdateManifoldPoint(int32 ManifoldPointIndex, const FContactPoint& ContactPoint)
	{
		// We really need to know that it's exactly the same contact and not just a close one to update it here
		// otherwise the PrevLocalContactPoint1 we calculated is not longer for the correct point.
		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
		ManifoldPoint.ContactPoint = ContactPoint;
		UpdateManifoldPointFromContact(ManifoldPointIndex);
	}

	// Update the derived contact state (CoM relative data)
	void FPBDCollisionConstraint::UpdateManifoldPointFromContact(const int32 ManifoldPointIndex)
	{
		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
		FConstGenericParticleHandle Particle0 = Particle[0];
		FConstGenericParticleHandle Particle1 = Particle[1];

		const FVec3 LocalContactPoint0 = ImplicitTransform[0].TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[0]);	// Particle Space on body 0
		const FVec3 LocalContactPoint1 = ImplicitTransform[1].TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[1]);	// Particle Space on body 1

		const FVec3 CoMContactPoint0 = Particle0->RotationOfMass().Inverse() * (LocalContactPoint0 - Particle0->CenterOfMass());	// CoM Space on Body 0
		const FVec3 CoMContactPoint1 = Particle1->RotationOfMass().Inverse() * (LocalContactPoint1 - Particle1->CenterOfMass());	// CoM Space on Body 1
		ManifoldPoint.CoMContactPoints[0] = CoMContactPoint0;
		ManifoldPoint.CoMContactPoints[1] = CoMContactPoint1;

		// We now assume that the low-level collision detection functions initialize the world-space contact in the way we want, which is as below...
		//const FVec3 WorldContactPoint0 = Particle0->P() + Particle0->Q().RotateVector(LocalContactPoint0);
		//const FVec3 WorldContactPoint1 = Particle1->P() + Particle1->Q().RotateVector(LocalContactPoint1);
		//ManifoldPoint.ContactPoint.Location = FReal(0.5) * (WorldContactPoint0 + WorldContactPoint1);
		//ManifoldPoint.ContactPoint.Phi = FVec3::DotProduct(WorldContactPoint0 - WorldContactPoint1, ManifoldPoint.ContactPoint.Normal);
	}

	void FPBDCollisionConstraint::SetActiveContactPoint(const FContactPoint& ContactPoint)
	{
		// @todo(chaos): once we settle on manifolds we should just store the index
		Manifold.Location = ContactPoint.Location;
		Manifold.Normal = ContactPoint.Normal;
		Manifold.Phi = ContactPoint.Phi;
	}

	void FPBDCollisionConstraint::GetWorldSpaceContactPositions(
		const FManifoldPoint& ManifoldPoint,
		const FVec3& P0,			// World-Space CoM
		const FRotation3& Q0,		// World-Space CoM
		const FVec3& P1,			// World-Space CoM
		const FRotation3& Q1,		// World-Space CoM
		FVec3& OutWorldPosition0,
		FVec3& OutWorldPosition1)
	{
		OutWorldPosition0 = P0 + Q0.RotateVector(ManifoldPoint.CoMContactPoints[0]);
		OutWorldPosition1 = P1 + Q1.RotateVector(ManifoldPoint.CoMContactPoints[1]);
	}

	void FPBDCollisionConstraint::GetCoMContactPositionsFromWorld(
		const FManifoldPoint& ManifoldPoint,
		const FVec3& PCoM0,
		const FRotation3& QCoM0,
		const FVec3& PCoM1,
		const FRotation3& QCoM1,
		const FVec3& WorldPoint0,
		const FVec3& WorldPoint1,
		FVec3& OutCoMPoint0,
		FVec3& OutCoMPoint1)
	{
		// Invert the transformation in GetWorldSpaceContactPositions() and return CoM space contact locations.
		OutCoMPoint0 = QCoM0.UnrotateVector(WorldPoint0 - PCoM0);
		OutCoMPoint1 = QCoM1.UnrotateVector(WorldPoint1 - PCoM1);
	}

	void FPBDCollisionConstraint::GetWorldSpaceManifoldPoint(
		const FManifoldPoint& ManifoldPoint,
		const FVec3& P0,			// World-Space CoM
		const FRotation3& Q0,		// World-Space CoM
		const FVec3& P1,			// World-Space CoM
		const FRotation3& Q1,		// World-Space CoM
		FVec3& OutContactLocation,
		FReal& OutContactPhi)
	{
		FVec3 ContactPos0;
		FVec3 ContactPos1;
		FPBDCollisionConstraint::GetWorldSpaceContactPositions(ManifoldPoint, P0, Q0, P1, Q1, ContactPos0, ContactPos1);

		OutContactLocation = 0.5f * (ContactPos0 + ContactPos1);
		OutContactPhi = FVec3::DotProduct(ContactPos0 - ContactPos1, ManifoldPoint.ContactPoint.Normal);
	}

	bool FPBDCollisionConstraint::CanUseManifold(FGeometryParticleHandle* Particle0, FGeometryParticleHandle* Particle1) const
	{
		// Do not use manifolds when a body is connected by a joint to another. Manifolds do not work when the bodies may be moved
		// and rotated by significant amounts and joints can do that.
		return bChaos_Manifold_EnabledWithJoints || ((Particle0->ParticleConstraints().Num() == 0) && (Particle1->ParticleConstraints().Num() == 0));
	}

	void FPBDCollisionConstraint::ResetManifold()
	{
		NumSavedManifoldPoints = 0;
		ResetActiveManifoldContacts();
	}

	void FPBDCollisionConstraint::ResetActiveManifoldContacts()
	{
		ManifoldPoints.Reset();
		Manifold.Reset();
		ExpectedNumManifoldPoints = 0;
		bWasManifoldRestored = false;
	}

	void FPBDCollisionConstraint::SaveManifold()
	{
		check(ManifoldPoints.Num() <= MaxManifoldPoints);

		// Save off the previous data for use by static friction
		NumSavedManifoldPoints = 0;
		const int32 NumManifoldPoints = ManifoldPoints.Num();
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			// Only save points that actually produced a response and are holding static friction
			const FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
			if (ManifoldPoint.bInsideStaticFrictionCone && !ManifoldPoint.NetPushOut.IsNearlyZero())
			{
				ManifoldPointSavedData[NumSavedManifoldPoints++].Save(ManifoldPoint);
			}
		}

		bWasManifoldRestored = false;
	}

	void FPBDCollisionConstraint::RestoreManifold()
	{
		// We want to restore the manifold as-is and will skip the narrow phase, which means we leave the manifold in place, 
		// but we still have some cleanup to do to account for slight movement of the bodies. E.g., we need to update the 
		// world-space state for the contact modifiers
		UpdateManifoldContacts();

		bWasManifoldRestored = true;
	}

	void FPBDCollisionConstraint::UpdateLastShapeWorldTransforms(const FRigidTransform3& ShapeWorldTransform0, const FRigidTransform3& ShapeWorldTransform1)
	{
		LastShapeWorldTransform0 = ShapeWorldTransform0;
		LastShapeWorldTransform1 = ShapeWorldTransform1;
	}

	bool FPBDCollisionConstraint::UpdateAndTryRestoreManifold(const FRigidTransform3& ShapeWorldTransform0, const FRigidTransform3& ShapeWorldTransform1)
	{
		check(ManifoldPoints.Num() <= MaxManifoldPoints);

		// @todo(chaos): tune the multipliers
		const FReal ContactPositionTolerance = FReal(0.8) * CollisionTolerance;
		const FReal ShapePositionTolerance = (ManifoldPoints.Num() > 0) ? FReal(0.2) * CollisionTolerance : FReal(0.5) * CollisionTolerance;
		const FReal ShapeRotationThreshold = (ManifoldPoints.Num() > 0) ? FReal(0.9999) : FReal(0.9998);
		const FReal ContactPositionToleranceSq = FMath::Square(ContactPositionTolerance);

		// Reset current closest point
		Manifold.Reset();

		// How many manifold points we expect. E.g., for Box-box this will be 4 or 1 depending on whether
		// we have a face or edge contact. We don't reuse the manifold if we lose points after culling
		// here and potentially adding the new narrow phase result (See TryAddManifoldContact).
		ExpectedNumManifoldPoints = ManifoldPoints.Num();
		bWasManifoldRestored = false;

		const FRigidTransform3 Shape0ToShape1Transform = ShapeWorldTransform0.GetRelativeTransformNoScale(ShapeWorldTransform1);

		// Update and prune manifold points.
		TArray<int32, TInlineAllocator<4>> ManifoldPointsToRemove;
		const int32 NumManifoldPoints = ManifoldPoints.Num();
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

			// Calculate the world-space contact location and separation at the current shape transforms
			// @todo(chaos): this should use the normal owner. Currently we assume body 1 is the owner
			const FVec3 Contact0In1 = Shape0ToShape1Transform.TransformPositionNoScale(ManifoldPoint.InitialShapeContactPoints[0]);
			const FVec3& Contact1In1 = ManifoldPoint.InitialShapeContactPoints[1];
			const FVec3 ContactNormalIn1 = ShapeWorldTransform1.InverseTransformVectorNoScale(ManifoldPoint.ContactPoint.Normal);

			const FVec3 ContactDeltaIn1 = Contact0In1 - Contact1In1;
			const FReal ContactPhi = FVec3::DotProduct(ContactDeltaIn1, ContactNormalIn1);
			const FVec3 ContactLateralDeltaIn1 = ContactDeltaIn1 - ContactPhi * ContactNormalIn1;
			const FReal ContactLateralDistanceSq = ContactLateralDeltaIn1.SizeSquared();

			// Either update the point or flag it for removal
			if (ContactLateralDistanceSq < ContactPositionToleranceSq)
			{
				// Recalculate the contact points at the new location
				// @todo(chaos): we should reproject the contact on the plane owner
				FConstGenericParticleHandle Particle1 = Particle[1];
				const FVec3 ShapeContactPoint1 = Contact0In1 - ContactPhi * ContactNormalIn1;
				const FVec3 ActorContactPoint = ImplicitTransform[1].TransformPositionNoScale(ShapeContactPoint1);
				ManifoldPoint.ContactPoint.ShapeContactPoints[1] = ShapeContactPoint1;
				ManifoldPoint.CoMContactPoints[1] = Particle1->RotationOfMass().Inverse() * (ActorContactPoint - Particle1->CenterOfMass());
				ManifoldPoint.ContactPoint.Location = ShapeWorldTransform1.TransformPositionNoScale(FReal(0.5) * (ShapeContactPoint1 + Contact0In1));
				ManifoldPoint.ContactPoint.Phi = ContactPhi;
				ManifoldPoint.bWasRestored = true;
				TryRestoreFrictionData(ManifoldPointIndex);
				if (ContactPhi < Manifold.Phi)
				{
					// Update closest point
					SetActiveContactPoint(ManifoldPoint.ContactPoint);
				}
			}
			else
			{
				ManifoldPointsToRemove.Add(ManifoldPointIndex);
			}
		}

		if ((ManifoldPointsToRemove.Num() == 0) && (ShapePositionTolerance > 0) && (ShapeRotationThreshold > 0))
		{
			// If we did not remove any contact points and we have not moved or rotated much we can reuse the manifold as-is.
			// The transform check is necessary regardless of how many points we have left in the manifold because
			// as a body moves/rotates we may have to change which faces/edges are colliding. We can't know if the face/edge
			// will change until we run the closest-point checks (GJK) in the narrow phase.
			const FVec3 Shape1ToShape0Translation = ShapeWorldTransform0.GetTranslation() - ShapeWorldTransform1.GetTranslation();
			const FVec3 OriginalShape1ToShape0Translation = LastShapeWorldTransform0.GetTranslation() - LastShapeWorldTransform1.GetTranslation();
			const FVec3 TranslationDelta = Shape1ToShape0Translation - OriginalShape1ToShape0Translation;
			if (TranslationDelta.IsNearlyZero(ShapePositionTolerance))
			{
				const FRotation3 Shape1toShape0Rotation = ShapeWorldTransform0.GetRotation().Inverse() * ShapeWorldTransform1.GetRotation();
				const FRotation3 OriginalShape1toShape0Rotation = LastShapeWorldTransform0.GetRotation().Inverse() * LastShapeWorldTransform1.GetRotation();
				const FReal RotationOverlap = FRotation3::DotProduct(Shape1toShape0Rotation, OriginalShape1toShape0Rotation);
				if (RotationOverlap > ShapeRotationThreshold)
				{
					return true;
				}
			}
		}

		// We removed some points - process in reverse order for simpler index handling and faster removal of multiple elements
		for (int32 RemoveArrayIndex = ManifoldPointsToRemove.Num() - 1; RemoveArrayIndex >= 0; --RemoveArrayIndex)
		{
			ManifoldPoints.RemoveAt(ManifoldPointsToRemove[RemoveArrayIndex]);
		}

		return false;
	}

	bool FPBDCollisionConstraint::TryAddManifoldContact(const FContactPoint& NewContactPoint, const FRigidTransform3& ShapeWorldTransform0, const FRigidTransform3& ShapeWorldTransform1)
	{
		check(ManifoldPoints.Num() <= MaxManifoldPoints);

		const FReal PositionTolerance = FReal(1.0) * CollisionTolerance;
		const FReal NormalThreshold = FReal(0.7);

		// We must end up with a full manifold after this if we want to reuse it
		if ((ManifoldPoints.Num() < ExpectedNumManifoldPoints - 1) || (ExpectedNumManifoldPoints == 0))
		{
			// We need to add more than 1 point to restore the manifold so we must rebuild it from scratch
			return false;
		}

		// Find the matching manifold point if it exists and replace it
		// Also check to see if the normal has changed significantly and if it has force manifold regeneration
		// NOTE: the normal rejection check assumes all contacts have the same normal - this may not always be true. The worst
		// case here is that we will regenerate the manifold too often so it will work but could be bad for perf
		const FReal PositionToleranceSq = FMath::Square(PositionTolerance);
		int32 MatchedManifoldPointIndex = INDEX_NONE;
		const int32 NumManifoldPoints = ManifoldPoints.Num();
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

			const FReal NormalOverlap = FVec3::DotProduct(ManifoldPoint.ContactPoint.Normal, NewContactPoint.Normal);
			if (NormalOverlap < NormalThreshold)
			{
				return false;
			}

			const FVec3 DR0 = ManifoldPoint.ContactPoint.ShapeContactPoints[0] - NewContactPoint.ShapeContactPoints[0];
			const FVec3 DR1 = ManifoldPoint.ContactPoint.ShapeContactPoints[1] - NewContactPoint.ShapeContactPoints[1];
			if ((DR0.SizeSquared() < PositionToleranceSq) && (DR1.SizeSquared() < PositionToleranceSq))
			{
				// If we should replace a point but will then have too few points we abort
				if (ManifoldPoints.Num() < ExpectedNumManifoldPoints)
				{
					return false;
				}

				// If the existing point has a deeper penetration, just re-use it. This is common when we have a GJK
				// result on an edge or corner - the contact created when generating the manifold is on the
				// surface shape rather than the rounded (margin-reduced) shape.
				if (ManifoldPoint.ContactPoint.Phi > NewContactPoint.Phi)
				{
					ManifoldPoint.ContactPoint = NewContactPoint;
					ManifoldPoint.InitialShapeContactPoints[0] = NewContactPoint.ShapeContactPoints[0];
					ManifoldPoint.InitialShapeContactPoints[1] = NewContactPoint.ShapeContactPoints[1];
					ManifoldPoint.bWasRestored = false;
					UpdateManifoldPointFromContact(ManifoldPointIndex);
					TryRestoreFrictionData(ManifoldPointIndex);
					if (NewContactPoint.Phi < GetPhi())
					{
						SetActiveContactPoint(ManifoldPoint.ContactPoint);
					}
				}

				return true;
			}
		}

		// If we have a full manifold, see if we can use or reject the GJK point
		if (ManifoldPoints.Num() == 4)
		{
			return TryInsertManifoldContact(NewContactPoint, ShapeWorldTransform0, ShapeWorldTransform1);
		}
		
		return false;
	}

	bool FPBDCollisionConstraint::TryInsertManifoldContact(const FContactPoint& NewContactPoint, const FRigidTransform3& ShapeWorldTransform0, const FRigidTransform3& ShapeWorldTransform1)
	{
		check(ManifoldPoints.Num() == 4);

		const int32 NormalBodyIndex = 1;
		constexpr int32 NumContactPoints = 5;
		constexpr int32 NumManifoldPoints = 4;

		// We want to select 4 points from the 5 we have
		// Create a working set of points, and keep track which points have been selected
		FVec3 ContactPoints[NumContactPoints];
		FReal ContactPhis[NumContactPoints];
		bool bContactSelected[NumContactPoints];
		int32 SelectedContactIndices[NumManifoldPoints];
		for (int32 ContactIndex = 0; ContactIndex < NumManifoldPoints; ++ContactIndex)
		{
			const FManifoldPoint& ManifoldPoint = ManifoldPoints[ContactIndex];
			ContactPoints[ContactIndex] = ManifoldPoint.ContactPoint.ShapeContactPoints[NormalBodyIndex];
			ContactPhis[ContactIndex] = ManifoldPoint.ContactPoint.Phi;
			bContactSelected[ContactIndex] = false;
		}
		ContactPoints[4] = NewContactPoint.ShapeContactPoints[NormalBodyIndex];
		ContactPhis[4] = NewContactPoint.Phi;
		bContactSelected[4] = false;

		// We are projecting points into a plane perpendicular to the contact normal, which we assume is the new point's normal
		const FVec3 ContactNormal = NewContactPoint.ShapeContactNormal;

		// Start with the deepest point. This may not be point 4 despite that being the result of
		// collision detection because for some shape types we use margin-reduced core shapes which
		// are effectively rounded at the corners. But...when building a one-shot manifold we 
		// use the outer shape to get sharp corners. So, if we have a GJK result from a "corner"
		// the real corner (if it is in the manifold) may actually be deeper than the GJK result.
		SelectedContactIndices[0] = 0;
		for (int32 ContactIndex = 1; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (ContactPhis[ContactIndex] < ContactPhis[SelectedContactIndices[0]])
			{
				SelectedContactIndices[0] = ContactIndex;
			}
		}
		bContactSelected[SelectedContactIndices[0]] = true;

		// The second point will be the one farthest from the first
		SelectedContactIndices[1] = INDEX_NONE;
		FReal MaxDistanceSq = TNumericLimits<FReal>::Lowest();
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				const FReal DistanceSq = (ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[0]]).SizeSquared();
				if (DistanceSq > MaxDistanceSq)
				{
					SelectedContactIndices[1] = ContactIndex;
					MaxDistanceSq = DistanceSq;
				}
			}
		}
		check(SelectedContactIndices[1] != INDEX_NONE);
		bContactSelected[SelectedContactIndices[1]] = true;

		// The third point is the one which gives us the largest triangle (projected onto a plane perpendicular to the normal)
		SelectedContactIndices[2] = INDEX_NONE;
		FReal MaxTriangleArea = 0;
		FReal WindingOrder = FReal(1.0);
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				const FVec3 Cross = FVec3::CrossProduct(ContactPoints[SelectedContactIndices[1]] - ContactPoints[SelectedContactIndices[0]], ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[1]]);
				const FReal SignedArea = FVec3::DotProduct(Cross, ContactNormal);
				if (FMath::Abs(SignedArea) > MaxTriangleArea)
				{
					SelectedContactIndices[2] = ContactIndex;
					MaxTriangleArea = FMath::Abs(SignedArea);
					WindingOrder = FMath::Sign(SignedArea);
				}
			}
		}
		if (SelectedContactIndices[2] == INDEX_NONE)
		{
			// Degenerate points - all 4 exactly in a line
			return false;
		}
		bContactSelected[SelectedContactIndices[2]] = true;

		// The fourth point is the one which adds the most area to the 3 points we already have
		SelectedContactIndices[3] = INDEX_NONE;
		FReal MaxQuadArea = 0;	// Additional area to MaxTriangleArea
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				// Calculate the area that is added by inserting the point into each edge of the selected triangle
				// The signed area will be negative for interior points, positive for points that extend the triangle into a quad.
				const FVec3 Cross0 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[0]], ContactPoints[SelectedContactIndices[1]] - ContactPoints[ContactIndex]);
				const FReal SignedArea0 = WindingOrder * FVec3::DotProduct(Cross0, ContactNormal);
				const FVec3 Cross1 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[1]], ContactPoints[SelectedContactIndices[2]] - ContactPoints[ContactIndex]);
				const FReal SignedArea1 = WindingOrder * FVec3::DotProduct(Cross1, ContactNormal);
				const FVec3 Cross2 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[2]], ContactPoints[SelectedContactIndices[0]] - ContactPoints[ContactIndex]);
				const FReal SignedArea2 = WindingOrder * FVec3::DotProduct(Cross2, ContactNormal);
				const FReal SignedArea = FMath::Max3(SignedArea0, SignedArea1, SignedArea2);
				if (SignedArea > MaxQuadArea)
				{
					SelectedContactIndices[3] = ContactIndex;
					MaxQuadArea = SignedArea;
				}
			}
		}
		if (SelectedContactIndices[3] == INDEX_NONE)
		{
			// No point is outside the triangle we already have
			return false;
		}
		bContactSelected[SelectedContactIndices[3]] = true;

		// Now we should have exactly 4 selected contacts. If we find that one of the existing points is not
		// selected, it must be because it is being replaced by the new contact. Otherwise the new contact
		// is interior to the existing manifiold and is rejected.
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			if (!bContactSelected[ManifoldPointIndex])
			{
				ManifoldPoints[ManifoldPointIndex].ContactPoint = NewContactPoint;
				ManifoldPoints[ManifoldPointIndex].InitialShapeContactPoints[0] = NewContactPoint.ShapeContactPoints[0];
				ManifoldPoints[ManifoldPointIndex].InitialShapeContactPoints[1] = NewContactPoint.ShapeContactPoints[1];
				ManifoldPoints[ManifoldPointIndex].bWasRestored = false;
				UpdateManifoldPointFromContact(ManifoldPointIndex);
				if (NewContactPoint.Phi < Manifold.Phi)
				{
					SetActiveContactPoint(NewContactPoint);
				}
			}
		}

		return true;
	}

	const FManifoldPointSavedData* FPBDCollisionConstraint::FindManifoldPointSavedData(const FManifoldPoint& ManifoldPoint) const
	{
		if (bChaos_Manifold_EnableFrictionRestore)
		{
			const FReal DistanceToleranceSq = FMath::Square(Chaos_Manifold_FrictionPositionTolerance);
			for (int32 PrevManifoldPointIndex = 0; PrevManifoldPointIndex < NumSavedManifoldPoints; ++PrevManifoldPointIndex)
			{
				const FManifoldPointSavedData& PrevManifoldPoint = ManifoldPointSavedData[PrevManifoldPointIndex];
				if (PrevManifoldPoint.bInsideStaticFrictionCone && PrevManifoldPoint.IsMatch(ManifoldPoint, DistanceToleranceSq))
				{
					return &PrevManifoldPoint;
				}
			}
		}
		return nullptr;
	}

	void FPBDCollisionConstraint::TryRestoreFrictionData(const int32 ManifoldPointIndex)
	{
		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

		// Assume we have no matching point from the previous tick, but that we can retain friction from now on
		// Not supported for non-manifolds yet (hopefully we don't need to)
		ManifoldPoint.bInsideStaticFrictionCone = bUseManifold;
		ManifoldPoint.StaticFrictionMax = FReal(0);

		// Find the previous manifold point that matches if there is one
		const FManifoldPointSavedData* PrevManifoldPoint = FindManifoldPointSavedData(ManifoldPoint);
		if (PrevManifoldPoint != nullptr)
		{
			// We have data from the previous tick and static friction was enabled - restore the data
			PrevManifoldPoint->Restore(ManifoldPoint);
		}
	}

	ECollisionConstraintDirection FPBDCollisionConstraint::GetConstraintDirection(const FReal Dt) const
	{
		if (GetDisabled())
		{
			return NoRestingDependency;
		}
		// D\tau is the chacteristic time (as in GBF paper Sec 8.1)
		const FReal Dtau = Dt * Chaos_GBFCharacteristicTimeRatio; 

		const FVec3 Normal = GetNormal();
		const FReal Phi = GetPhi();
		if (GetPhi() >= GetCullDistance())
		{
			return NoRestingDependency;
		}

		FVec3 GravityDirection = ConcreteContainer()->GetGravityDirection();
		FReal GravitySize = ConcreteContainer()->GetGravitySize();
		// When gravity is zero, we still want to sort the constraints instead of having a random order. In this case, set gravity to default gravity.
		if (GravitySize < SMALL_NUMBER)
		{
			GravityDirection = FVec3(0, 0, -1);
			GravitySize = 980.f;
		}

		// How far an object travels in gravity direction within time Dtau starting with zero velocity (as in GBF paper Sec 8.1). 
		// Theoretically this should be 0.5 * GravityMagnitude * Dtau * Dtau.
		// Omitting 0.5 to be more consistent with our integration scheme.
		// Multiplying 0.5 can alternatively be achieved by setting Chaos_GBFCharacteristicTimeRatio=sqrt(0.5)
		const FReal StepSize = GravitySize * Dtau * Dtau; 
		const FReal NormalDotG = FVec3::DotProduct(Normal, GravityDirection);
		const FReal NormalDirectionThreshold = 0.1f; // Hack
		if (NormalDotG < -NormalDirectionThreshold) // Object 0 rests on object 1
		{
			if (Phi + NormalDotG * StepSize < 0) // Hack to simulate object 0 falling (as in GBF paper Sec 8.1)
			{
				return Particle1ToParticle0;
			}
			else
			{
				return NoRestingDependency;
			}
		}
		else if (NormalDotG > NormalDirectionThreshold) // Object 1 rests on object 0
		{
			if (Phi - NormalDotG * StepSize < 0) // Hack to simulate object 1 falling (as in GBF paper Sec 8.1)
			{
				return Particle0ToParticle1;
			}
			else
			{
				return NoRestingDependency;
			}
		}
		else // Horizontal contact
		{
			return NoRestingDependency;
		}
	}
}