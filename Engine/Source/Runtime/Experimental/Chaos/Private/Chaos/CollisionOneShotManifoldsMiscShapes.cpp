// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/CollisionOneShotManifoldsMiscShapes.h"

#include "Chaos/CollisionOneShotManifolds.h"
#include "Chaos/CollisionResolution.h"
#include "Chaos/Collision/CapsuleConvexContactPoint.h"
#include "Chaos/Collision/ContactPointsMiscShapes.h"
#include "Chaos/Collision/SphereConvexContactPoint.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Convex.h"
#include "Chaos/Defines.h"
#include "Chaos/GJK.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Transform.h"
#include "Chaos/Utilities.h"
#include "ChaosStats.h"

#include "HAL/IConsoleManager.h"

//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{

	namespace Collisions
	{
		void ConstructSphereSphereOneShotManifold(
			const TSphere<FReal, 3>& SphereA,
			const FRigidTransform3& SphereATransform, //world
			const TSphere<FReal, 3>& SphereB,
			const FRigidTransform3& SphereBTransform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint)
		{
			SCOPE_CYCLE_COUNTER_MANIFOLD();
			
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereATransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(SphereBTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SphereSphereContactPoint(SphereA, SphereATransform, SphereB, SphereBTransform, Constraint.Manifold.RestitutionPadding);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		void ConstructSpherePlaneOneShotManifold(
			const TSphere<FReal, 3>& Sphere, 
			const FRigidTransform3& SphereTransform, 
			const TPlane<FReal, 3>& Plane, 
			const FRigidTransform3& PlaneTransform, 
			const FReal Dt, 
			FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(PlaneTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SpherePlaneContactPoint(Sphere, SphereTransform, Plane, PlaneTransform, Constraint.Manifold.RestitutionPadding);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}	

		void ConstructSphereBoxOneShotManifold(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const FImplicitBox3& Box, const FRigidTransform3& BoxTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(BoxTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SphereBoxContactPoint(Sphere, SphereTransform, Box, BoxTransform, Constraint.Manifold.RestitutionPadding);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		void ConstructSphereCapsuleOneShotManifold(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const FCapsule& Capsule, const FRigidTransform3& CapsuleTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(CapsuleTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SphereCapsuleContactPoint(Sphere, SphereTransform, Capsule, CapsuleTransform, Constraint.Manifold.RestitutionPadding);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		void ConstructSphereConvexManifold(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const FImplicitObject3& Convex, const FRigidTransform3& ConvexTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(ConvexTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SphereConvexContactPoint(Sphere, SphereTransform, Convex, ConvexTransform);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		template <typename TriMeshType>
		void ConstructSphereTriangleMeshOneShotManifold(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereWorldTransform, const TriMeshType& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereWorldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(TriMeshWorldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FContactPoint ContactPoint = SphereTriangleMeshContactPoint(Sphere, SphereWorldTransform, TriangleMesh, TriMeshWorldTransform, Constraint.GetCullDistance(), 0.0f);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		void ConstructSphereHeightFieldOneShotManifold(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const FHeightField& Heightfield, const FRigidTransform3& HeightfieldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(SphereTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(HeightfieldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			FContactPoint ContactPoint = SphereHeightFieldContactPoint(Sphere, SphereTransform, Heightfield, HeightfieldTransform, Constraint.GetCullDistance(), 0.0f);

			Constraint.AddOneshotManifoldContact(ContactPoint);
		}

		void ConstructCapsuleCapsuleOneShotManifold(const FCapsule& CapsuleA, const FRigidTransform3& CapsuleATransform, const FCapsule& CapsuleB, const FRigidTransform3& CapsuleBTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			const FReal AxisDotMinimum = 0.707f; // If the axes are off by more than this, just a single manifold point will be generated

			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(CapsuleATransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(CapsuleBTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			FVec3 CapsuleADirection(CapsuleATransform.TransformVector(CapsuleA.GetSegment().GetAxis()));
			const FVec3 CapsuleBDirection(CapsuleBTransform.TransformVector(CapsuleB.GetSegment().GetAxis()));

			FReal ADotB = FVec3::DotProduct(CapsuleADirection, CapsuleBDirection);

			const FReal AHalfLen = CapsuleA.GetHeight() / 2.0f;
			const FReal BHalfLen = CapsuleB.GetHeight() / 2.0f;

			if (FMath::Abs(ADotB) < AxisDotMinimum || AHalfLen < KINDA_SMALL_NUMBER || BHalfLen < KINDA_SMALL_NUMBER)
			{
				FContactPoint ContactPoint = CapsuleCapsuleContactPoint(CapsuleA, CapsuleATransform, CapsuleB, CapsuleBTransform, Constraint.GetCullDistance());
				Constraint.AddOneshotManifoldContact(ContactPoint);
				return;
			}
			
			FVector P1, P2;
			const FVector ACenter = CapsuleATransform.TransformPosition(CapsuleA.GetCenter());
			const FVector BCenter = CapsuleBTransform.TransformPosition(CapsuleB.GetCenter());
			FMath::SegmentDistToSegmentSafe(
				ACenter + AHalfLen * CapsuleADirection, 
				ACenter - AHalfLen * CapsuleADirection, 
				BCenter + BHalfLen * CapsuleBDirection, 
				BCenter - BHalfLen * CapsuleBDirection, 
				P1, 
				P2);

			FVec3 Delta = P2 - P1;
			FReal DeltaLen = Delta.Size();

			if (DeltaLen < KINDA_SMALL_NUMBER)
			{
				FContactPoint ContactPoint = CapsuleCapsuleContactPoint(CapsuleA, CapsuleATransform, CapsuleB, CapsuleBTransform, Constraint.GetCullDistance());
				Constraint.AddOneshotManifoldContact(ContactPoint);
				return;
			}
			
			// Make both capsules point in the same general direction
			if (ADotB < 0)
			{
				ADotB = -ADotB;
				CapsuleADirection = -CapsuleADirection;
			}

			// Now project A points onto B segment
			const FReal ProjA1OntoB = FVec3::DotProduct(ACenter - BCenter - AHalfLen * CapsuleADirection, CapsuleBDirection);
			const FReal ProjA2OntoB = FVec3::DotProduct(ACenter - BCenter + AHalfLen * CapsuleADirection, CapsuleBDirection);

			const FReal Clipped1Coord = FMath::Max(ProjA1OntoB, -BHalfLen); // 1D coordinates
			const FReal Clipped2Coord = FMath::Min(ProjA2OntoB, BHalfLen);
			if (Clipped1Coord > Clipped2Coord) // No overlap
			{
				FContactPoint ContactPoint = CapsuleCapsuleContactPoint(CapsuleA, CapsuleATransform, CapsuleB, CapsuleBTransform, Constraint.GetCullDistance());
				Constraint.AddOneshotManifoldContact(ContactPoint);
				return;
			}

			//FReal NewPhi = DeltaLen - (CapsuleA.GetRadius() + CapsuleB.GetRadius());
			FVec3 Dir = Delta / DeltaLen;
			FVec3 Normal = -Dir;

			FContactPoint ContactPoint;
			ContactPoint.ShapeContactNormal = CapsuleBTransform.InverseTransformVector(Normal);
			ContactPoint.Normal = Normal;

			auto AddManifoldPoint = [&](FReal ClippedCoord)
			{
				FVec3 LocationB = ClippedCoord * CapsuleBDirection + BCenter + Normal * (CapsuleB.GetRadius());
				const FReal ProjCentreAOntoB = FVec3::DotProduct(ACenter - BCenter, CapsuleBDirection);
				// Note location A is calculated by rotation (effectively) instead of the usual plane clipping
				FVec3 LocationA = (ClippedCoord - ProjCentreAOntoB) * CapsuleADirection + ACenter - Normal * (CapsuleA.GetRadius());

				ContactPoint.ShapeContactPoints[0] = CapsuleATransform.InverseTransformPosition(LocationA);
				ContactPoint.ShapeContactPoints[1] = CapsuleBTransform.InverseTransformPosition(LocationB);
				ContactPoint.Location = 0.5f * (LocationA + LocationB);
				ContactPoint.Phi = FVec3::DotProduct(LocationA - LocationB, Normal);

				Constraint.AddOneshotManifoldContact(ContactPoint);
			};

			AddManifoldPoint(Clipped1Coord);
			AddManifoldPoint(Clipped2Coord);
		}

		template <typename TriMeshType>
		void ConstructCapsuleTriMeshOneShotManifold(const FCapsule& Capsule, const FRigidTransform3& CapsuleWorldTransform, const TriMeshType& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(CapsuleWorldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(TriMeshWorldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			TArray<FContactPoint> ContactPoints;
			GJKImplicitManifold<FCapsule, TriMeshType>(Capsule, CapsuleWorldTransform, TriangleMesh, TriMeshWorldTransform, Constraint.GetCullDistance(), 0.0f, ContactPoints);
			for (FContactPoint& ContactPoint : ContactPoints)
			{
				Constraint.AddOneshotManifoldContact(ContactPoint);
			}
		}

		void ConstructCapsuleHeightFieldOneShotManifold(const FCapsule& Capsule, const FRigidTransform3& CapsuleTransform, const FHeightField& HeightField, const FRigidTransform3& HeightFieldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(CapsuleTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(HeightFieldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			TArray<FContactPoint> ContactPoints;
			GJKImplicitManifold<FCapsule>(Capsule, CapsuleTransform, HeightField, HeightFieldTransform, Constraint.GetCullDistance(), 0.0f, ContactPoints);
			for (FContactPoint& ContactPoint : ContactPoints)
			{
				Constraint.AddOneshotManifoldContact(ContactPoint);
			}
		}

		template<typename ConvexType>
		void ConstructConvexHeightFieldOneShotManifold(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const FHeightField& HeightField, const FRigidTransform3& HeightFieldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(ConvexTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(HeightFieldTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			//FContactPoint ContactPoint = ConvexHeightFieldContactPoint(Convex, ConvexTransform, HeightField, HeightFieldTransform, Constraint.GetCullDistance(), 0.0f);

			TArray<FContactPoint> ContactPoints;
			GJKImplicitManifold<ConvexType>(Convex, ConvexTransform, HeightField, HeightFieldTransform, Constraint.GetCullDistance(), 0.0f, ContactPoints);
			for (FContactPoint& ContactPoint : ContactPoints)
			{
				Constraint.AddOneshotManifoldContact(ContactPoint);
			}
		}

		template <typename ConvexType, typename TriMeshType>
		void ConstructConvexTriMeshOneShotManifold(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const TriMeshType& TriangleMesh, const FRigidTransform3& TriMeshTransform, const FReal Dt, FPBDCollisionConstraint& Constraint)
		{
			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			ensure(ConvexTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(TriMeshTransform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// @todo(chaos): support manifold maintenance
			Constraint.ResetManifold();

			TArray<FContactPoint> ContactPoints;
			GJKImplicitManifold<ConvexType, TriMeshType>(Convex, ConvexTransform, TriangleMesh, TriMeshTransform, Constraint.GetCullDistance(), 0.0f, ContactPoints);
			for (FContactPoint& ContactPoint : ContactPoints)
			{
				Constraint.AddOneshotManifoldContact(ContactPoint);
			}
		}

		template void ConstructSphereTriangleMeshOneShotManifold<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereWorldTransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructSphereTriangleMeshOneShotManifold<FTriangleMeshImplicitObject>(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereWorldTransform, const FTriangleMeshImplicitObject& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);

		template void ConstructCapsuleTriMeshOneShotManifold<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FCapsule& Capsule, const FRigidTransform3& CapsuleWorldTransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructCapsuleTriMeshOneShotManifold<FTriangleMeshImplicitObject>(const FCapsule& Capsule, const FRigidTransform3& CapsuleWorldTransform, const FTriangleMeshImplicitObject& TriangleMesh, const FRigidTransform3& TriMeshWorldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);

		template void ConstructConvexTriMeshOneShotManifold<FConvex, TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& TriangleMesh, const FRigidTransform3& TriMeshTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructConvexTriMeshOneShotManifold<FConvex, FTriangleMeshImplicitObject>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const FTriangleMeshImplicitObject& TriangleMesh, const FRigidTransform3& TriMeshTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructConvexTriMeshOneShotManifold<FImplicitBox3, TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& TriangleMesh, const FRigidTransform3& TriMeshTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructConvexTriMeshOneShotManifold<FImplicitBox3, FTriangleMeshImplicitObject>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const FTriangleMeshImplicitObject& TriangleMesh, const FRigidTransform3& TriMeshTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);

		template void ConstructConvexHeightFieldOneShotManifold<FConvex>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const FHeightField& HeightField, const FRigidTransform3& HeightFieldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
		template void ConstructConvexHeightFieldOneShotManifold<FImplicitBox3>(const FImplicitObject& Convex, const FRigidTransform3& ConvexTransform, const FHeightField& HeightField, const FRigidTransform3& HeightFieldTransform, const FReal Dt, FPBDCollisionConstraint& Constraint);
	}
}

