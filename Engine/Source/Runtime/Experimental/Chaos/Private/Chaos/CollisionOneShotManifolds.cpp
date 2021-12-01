// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/CollisionOneShotManifolds.h"

#include "Chaos/Box.h"
#include "Chaos/CollisionResolution.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Convex.h"
#include "Chaos/Defines.h"
#include "Chaos/GJK.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Transform.h"
#include "Chaos/Triangle.h"
#include "Chaos/Utilities.h"
#include "ChaosStats.h"

#include "HAL/IConsoleManager.h"

//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{
	FRealSingle Chaos_Collision_Manifold_PlaneContactNormalEpsilon = 0.001f;
	FAutoConsoleVariableRef CVarChaos_Manifold_PlaneContactNormalEpsilon(TEXT("p.Chaos.Collision.Manifold.PlaneContactNormalEpsilon"), Chaos_Collision_Manifold_PlaneContactNormalEpsilon, TEXT("Normal tolerance used to distinguish face contacts from edge-edge contacts"));

	// @todo(chaos): TEMP - use convex-convex collisio detection for box-box until TBox::GetClosestEdgePosition is implemented for that path (without plane hint)
	bool bChaos_Collision_Manifold_BoxAsConvex = true;
	FAutoConsoleVariableRef CVarChaosCollisioConvexManifoldBoxAsConvex(TEXT("p.Chaos.Collision.Manifold.BoxAsConvex"), bChaos_Collision_Manifold_BoxAsConvex, TEXT(""));

	FRealSingle Chaos_Collision_Manifold_CullDistanceMarginMultiplier = 1.0f;
	FAutoConsoleVariableRef CVarChaosCollisioConvexManifoldCullDistanceMarginMultiplier(TEXT("p.Chaos.Collision.Manifold.CullDistanceMarginMultiplier"), Chaos_Collision_Manifold_CullDistanceMarginMultiplier, TEXT(""));

	FRealSingle Chaos_Collision_Manifold_MinFaceSearchDistance = 1.0f;
	FAutoConsoleVariableRef CVarChaosCollisioConvexManifoldMinFaceSearchDistance(TEXT("p.Chaos.Collision.Manifold.MinFaceSearchDistance"), Chaos_Collision_Manifold_MinFaceSearchDistance, TEXT(""));

	bool bChaos_Collision_Manifold_FixNormalsInWorldSpace = true;
	FAutoConsoleVariableRef CVarChaosCollisioFixNormalsInWorldSpace(TEXT("p.Chaos.Collision.Manifold.FixNormalsInWorldSpace"), bChaos_Collision_Manifold_FixNormalsInWorldSpace, TEXT("Fix normals in world space at beginning of frame"));	

	bool ForceOneShotManifoldEdgeEdgeCaseZeroCullDistance = false;
	FAutoConsoleVariableRef CVarForceOneShotManifoldEdgeEdgeCaseZeroCullDistance(TEXT("p.Chaos.Collision.Manifold.ForceOneShotManifoldEdgeEdgeCaseZeroCullDistance"), ForceOneShotManifoldEdgeEdgeCaseZeroCullDistance,
	TEXT("If enabled, if one shot manifold hits edge/edge case, we will force a cull distance of zero. That means edge/edge contacts will be thrown out if separated at all. Only applies to Convex/Convex oneshot impl."));

	bool bChaos_Collision_EnableManifoldInject = true;
	FAutoConsoleVariableRef CVarChaos_Collision_EnableManifoldInject(TEXT("p.Chaos.Collision.EnableManifolInject"), bChaos_Collision_EnableManifoldInject, TEXT(""));

	
	namespace Collisions
	{
		// Forward delarations we need from CollisionRestitution.cpp

		FContactPoint BoxBoxContactPoint(const FImplicitBox3& Box1, const FImplicitBox3& Box2, const FRigidTransform3& Box1TM, const FRigidTransform3& Box2TM, const FReal ShapePadding);
		FContactPoint GenericConvexConvexContactPoint(const FImplicitObject& A, const FRigidTransform3& ATM, const FImplicitObject& B, const FRigidTransform3& BTM, const FReal ShapePadding);

		//////////////////////////
		// Box Box
		//////////////////////////

		// This function will clip the input vertices by a reference shape's planes (Specified by ClippingAxis and Distance for an AABB)
		// more vertices may be added to outputVertexBuffer by this function
		// This is the core of the Sutherland-Hodgman algorithm
		uint32 BoxBoxClipVerticesAgainstPlane(const FVec3* InputVertexBuffer, FVec3* outputVertexBuffer, uint32 ClipPointCount, int32 ClippingAxis, FReal Distance)
		{

			auto CalculateIntersect = [=](const FVec3& Point1, const FVec3& Point2) -> FVec3
			{
				// Only needs to be valid if the line connecting Point1 with Point2 actually intersects
				FVec3 Result;

				FReal Denominator = Point2[ClippingAxis] - Point1[ClippingAxis];  // Can be negative
				if (FMath::Abs(Denominator) < SMALL_NUMBER)
				{
					Result = Point1;
				}
				else
				{
					FReal Alpha = (Distance - Point1[ClippingAxis]) / Denominator;
					Result = FMath::Lerp(Point1, Point2, Alpha);
				}
				Result[ClippingAxis] = Distance; // For Robustness
				return Result;
			};

			auto InsideClipFace = [=](const FVec3& Point) -> bool
			{
				// The sign of Distance encodes which plane we are using
				if (Distance >= 0)
				{
					return Point[ClippingAxis] <= Distance;
				}
				return Point[ClippingAxis] >= Distance;
			};

			uint32 NewClipPointCount = 0;
			const uint32 MaxNumberOfPoints = 8;

			for (uint32 ClipPointIndex = 0; ClipPointIndex < ClipPointCount; ClipPointIndex++)
			{
				FVec3 CurrentClipPoint = InputVertexBuffer[ClipPointIndex];
				FVec3 PrevClipPoint = InputVertexBuffer[(ClipPointIndex + ClipPointCount - 1) % ClipPointCount];
				FVec3 InterSect = CalculateIntersect(PrevClipPoint, CurrentClipPoint);

				if (InsideClipFace(CurrentClipPoint))
				{
					if (!InsideClipFace(PrevClipPoint))
					{
						outputVertexBuffer[NewClipPointCount++] = InterSect;
						if (NewClipPointCount >= MaxNumberOfPoints)
						{
							break;
						}
					}
					outputVertexBuffer[NewClipPointCount++] = CurrentClipPoint;
				}
				else if (InsideClipFace(PrevClipPoint))
				{
					outputVertexBuffer[NewClipPointCount++] = InterSect;
				}

				if (NewClipPointCount >= MaxNumberOfPoints)
				{
					break;
				}
			}

			return NewClipPointCount;
		}

		void ConstructBoxBoxOneShotManifold(
			const FImplicitBox3& Box1,
			const FRigidTransform3& Box1Transform, //world
			const FImplicitBox3& Box2,
			const FRigidTransform3& Box2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint)
		{
			if (bChaos_Collision_Manifold_BoxAsConvex)
			{
				ConstructConvexConvexOneShotManifold(Box1, Box1Transform, Box2, Box2Transform, Dt, Constraint);
				return;
			}

			const uint32 SpaceDimension = 3;

			// We only build one shot manifolds once
			// All boxes are prescaled
			check(Constraint.GetManifoldPoints().Num() == 0);
			check(Box1Transform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			check(Box2Transform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			const uint32 MaxContactPointCount = 8;
			uint32 ContactPointCount = 0;

			// Use GJK only once
			FContactPoint GJKContactPoint = BoxBoxContactPoint(Box1, Box2, Box1Transform, Box2Transform, Constraint.Manifold.RestitutionPadding);

			FRigidTransform3 Box1TransformCenter = Box1Transform;
			Box1TransformCenter.SetTranslation(Box1Transform.TransformPositionNoScale(Box1.GetCenter()));
			FRigidTransform3 Box2TransformCenter = Box2Transform;
			Box2TransformCenter.SetTranslation(Box2Transform.TransformPositionNoScale(Box2.GetCenter()));

			// GJK does not give us any face information yet, so find the best reference face here
			uint32 BestFaceNormalAxisBox1 = 0; // Note: Face normals are axis aligned due to coordinates. This is an element of {0 , 1, 2}
			int32 BestFaceNormalAxisDirectionBox1 = 1; // {-1, 1}
			FReal BestFaceNormalSizeInDirectionBox1 = -1.0f;
			const FVec3 SeparationDirectionLocalBox1 = Box1TransformCenter.InverseTransformVectorNoScale(GJKContactPoint.Normal);   // Todo: Check if we can have Non uniform scale here
			// Box1: Iterating through 2 faces at a time here
			for (uint32 FaceNormalAxis = 0; FaceNormalAxis < SpaceDimension; FaceNormalAxis++)
			{
				const FReal AbsSeparationDirectionDotFaceNormal = FMath::Abs(SeparationDirectionLocalBox1[FaceNormalAxis]);
				if (AbsSeparationDirectionDotFaceNormal > BestFaceNormalSizeInDirectionBox1)
				{
					BestFaceNormalAxisBox1 = FaceNormalAxis;
					BestFaceNormalSizeInDirectionBox1 = AbsSeparationDirectionDotFaceNormal;
					BestFaceNormalAxisDirectionBox1 = SeparationDirectionLocalBox1[FaceNormalAxis] >= 0.0f ? -1 : 1;
				}
			}

			// Now for Box2
			uint32 BestFaceNormalAxisBox2 = 0; // Note: Face normals are axis aligned due to coordinates. This is a n element of {0, 1, 2}
			int32 BestFaceNormalAxisDirectionBox2 = 1; // {-1, 1}
			FReal BestFaceNormalSizeInDirectionBox2 = -1.0f;
			const FVec3 SeparationDirectionLocalBox2 = Box2TransformCenter.InverseTransformVectorNoScale(GJKContactPoint.Normal);   // Todo: Check if we can have Non uniform scale here
			for (uint32 FaceNormalAxis = 0; FaceNormalAxis < SpaceDimension; FaceNormalAxis++)
			{
				FVec3 FaceNormal(FVec3::ZeroVector);
				FaceNormal[FaceNormalAxis] = 1.0f;
				const FReal SeparationDirectionDotFaceNormal = FMath::Abs(SeparationDirectionLocalBox2[FaceNormalAxis]);

				if (SeparationDirectionDotFaceNormal > BestFaceNormalSizeInDirectionBox2)
				{
					BestFaceNormalAxisBox2 = FaceNormalAxis;
					BestFaceNormalSizeInDirectionBox2 = SeparationDirectionDotFaceNormal;
					BestFaceNormalAxisDirectionBox2 = SeparationDirectionLocalBox2[FaceNormalAxis] >= 0.0f ? 1 : -1;  // Note opposite of box1
				}
			}

			const FReal SmallBiasToPreventFeatureFlipping = 0.002f; // This improves frame coherence by penalizing box 1 in favour of box 2
			bool ReferenceFaceBox1 = true; // Is the reference face on box1 or box2?
			if (BestFaceNormalSizeInDirectionBox2 + SmallBiasToPreventFeatureFlipping > BestFaceNormalSizeInDirectionBox1)
			{
				ReferenceFaceBox1 = false;
			}

			// Is this a vertex-plane or edge-edge contact? 
			const FReal PlaneContactNormalEpsilon = Chaos_Collision_Manifold_PlaneContactNormalEpsilon;
			const bool bIsPlaneContact = FMath::IsNearlyEqual(BestFaceNormalSizeInDirectionBox1, (FReal)1., PlaneContactNormalEpsilon) || FMath::IsNearlyEqual(BestFaceNormalSizeInDirectionBox2, (FReal)1., PlaneContactNormalEpsilon);

			// For edge-edge contacts we find the edges involved and project the contact onto the edges
			if (!bIsPlaneContact)
			{
				FVec3 ShapeEdgePos1 = Box1.GetClosestEdgePosition(INDEX_NONE, GJKContactPoint.ShapeContactPoints[0]);
				FVec3 ShapeEdgePos2 = Box2.GetClosestEdgePosition(INDEX_NONE, GJKContactPoint.ShapeContactPoints[1]);
				FVec3 EdgePos1 = Box1Transform.TransformPosition(ShapeEdgePos1);
				FVec3 EdgePos2 = Box2Transform.TransformPosition(ShapeEdgePos2);
				FReal EdgePhi = FVec3::DotProduct(EdgePos1 - EdgePos2, GJKContactPoint.Normal);

				GJKContactPoint.ShapeContactPoints[0] = EdgePos1;
				GJKContactPoint.ShapeContactPoints[1] = EdgePos2;
				GJKContactPoint.Phi = EdgePhi;
				GJKContactPoint.Location = 0.5f * (EdgePos1 + EdgePos2);

				Constraint.AddOneshotManifoldContact(GJKContactPoint);
				return;
			}


			// For vertex-plane contacts, we use a convex face as the manifold plane
			// Setup pointers to other box and reference box
			const FRigidTransform3* RefBoxTM;
			const FRigidTransform3* OtherBoxTM;
			const FImplicitBox3* RefBox;
			const FImplicitBox3* OtherBox;

			if (ReferenceFaceBox1)
			{
				RefBoxTM = &Box1TransformCenter;
				OtherBoxTM = &Box2TransformCenter;
				RefBox = &Box1;
				OtherBox = &Box2;
			}
			else
			{
				RefBoxTM = &Box2TransformCenter;
				OtherBoxTM = &Box1TransformCenter;
				RefBox = &Box2;
				OtherBox = &Box1;
			}

			// Populate the clipped vertices by the other face's vertices

			// Populate initial clipping vertices with a face from the other box
			FVec3 otherBoxHalfExtents = 0.5f * OtherBox->Extents();
			uint32 ConstantCoordinateIndex = ReferenceFaceBox1 ? BestFaceNormalAxisBox2 : BestFaceNormalAxisBox1;
			FReal ConstantCoordinate = otherBoxHalfExtents[ConstantCoordinateIndex] * (FReal)(ReferenceFaceBox1 ? BestFaceNormalAxisDirectionBox2 : BestFaceNormalAxisDirectionBox1);

			uint32 VariableCoordinateIndices[2];
			FReal VariableCoordinates[2];

			uint32 VariableCoordinateCount = 0;
			for (uint32 Coordinate = 0; Coordinate < 3; ++Coordinate)
			{
				if (Coordinate != ConstantCoordinateIndex)
				{
					VariableCoordinateIndices[VariableCoordinateCount] = Coordinate;
					VariableCoordinates[VariableCoordinateCount] = otherBoxHalfExtents[Coordinate];
					++VariableCoordinateCount;
				}
			}

			ContactPointCount = 4; // Number of face vertices
			const uint32 GreyCode[4] = { 0, 1, 3, 2 }; // Grey code to make sure we add vertices in correct order
			FVec3 ClippedVertices[MaxContactPointCount];
			// Add the vertices in an order that will form a closed loop
			const FRigidTransform3 BoxOtherToRef = OtherBoxTM->GetRelativeTransform(*RefBoxTM);
			for (uint32 Vertex = 0; Vertex < ContactPointCount; Vertex++)
			{
				ClippedVertices[Vertex][ConstantCoordinateIndex] = ConstantCoordinate;
				ClippedVertices[Vertex][VariableCoordinateIndices[0]] = (GreyCode[Vertex] & (1 << 0)) ? VariableCoordinates[0] : -VariableCoordinates[0];
				ClippedVertices[Vertex][VariableCoordinateIndices[1]] = (GreyCode[Vertex] & (1 << 1)) ? VariableCoordinates[1] : -VariableCoordinates[1];
				ClippedVertices[Vertex] = BoxOtherToRef.TransformPositionNoScale(ClippedVertices[Vertex]);
			}

			// Now clip against all planes that belong to the reference plane's, edges
			FVec3 ClippedVertices2[MaxContactPointCount]; // We will use a double buffer as an optimization
			FVec3* VertexBuffer1 = ClippedVertices;
			FVec3* VertexBuffer2 = ClippedVertices2;

			FVec3 refBoxHalfExtents = 0.5f * RefBox->Extents();
			uint32 RefPlaneCoordinateIndex = ReferenceFaceBox1 ? BestFaceNormalAxisBox1 : BestFaceNormalAxisBox2;
			for (uint32 Coordinate = 0; Coordinate < 3; ++Coordinate)
			{
				if (Coordinate != RefPlaneCoordinateIndex)
				{
					ContactPointCount = BoxBoxClipVerticesAgainstPlane(VertexBuffer1, VertexBuffer2, ContactPointCount, static_cast<int32>(Coordinate), refBoxHalfExtents[Coordinate]);
					ContactPointCount = BoxBoxClipVerticesAgainstPlane(VertexBuffer2, VertexBuffer1, ContactPointCount, static_cast<int32>(Coordinate), -refBoxHalfExtents[Coordinate]);
				}
			}

			// Reduce number of contacts to a maximum of 4
			if (ContactPointCount > 4)
			{
				FRotation3 RotateSeperationToZ = FRotation3::FromRotatedVector(ReferenceFaceBox1 ? SeparationDirectionLocalBox1 : SeparationDirectionLocalBox2, FVec3(0.0f, 0.0f, 1.0f));
				for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
				{
					ClippedVertices[ContactPointIndex] = RotateSeperationToZ * ClippedVertices[ContactPointIndex];
				}

				ContactPointCount = ReduceManifoldContactPoints(ClippedVertices, ContactPointCount);

				for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
				{
					ClippedVertices[ContactPointIndex] = RotateSeperationToZ.Inverse() * ClippedVertices[ContactPointIndex];
				}
			}

			// Generate the contact points from the clipped vertices
			for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
			{
				FContactPoint ContactPoint;
				const FVec3 VertexInReferenceCubeCoordinates = ClippedVertices[ContactPointIndex];
				FVec3 PointProjectedOntoReferenceFace = VertexInReferenceCubeCoordinates;
				PointProjectedOntoReferenceFace[RefPlaneCoordinateIndex] = refBoxHalfExtents[RefPlaneCoordinateIndex] * (FReal)(ReferenceFaceBox1 ? BestFaceNormalAxisDirectionBox1 : BestFaceNormalAxisDirectionBox2);
				FVec3 ClippedPointInOtherCubeCoordinates = BoxOtherToRef.InverseTransformPositionNoScale(VertexInReferenceCubeCoordinates);

				ContactPoint.ShapeContactPoints[0] = ReferenceFaceBox1 ? PointProjectedOntoReferenceFace + RefBox->GetCenter() : ClippedPointInOtherCubeCoordinates + OtherBox->GetCenter();
				ContactPoint.ShapeContactPoints[1] = ReferenceFaceBox1 ? ClippedPointInOtherCubeCoordinates + OtherBox->GetCenter() : PointProjectedOntoReferenceFace + RefBox->GetCenter();
				ContactPoint.ShapeContactNormal = SeparationDirectionLocalBox2;
				ContactPoint.Location = RefBoxTM->TransformPositionNoScale(PointProjectedOntoReferenceFace);
				ContactPoint.Normal = GJKContactPoint.Normal;
				ContactPoint.Phi = FVec3::DotProduct(PointProjectedOntoReferenceFace - VertexInReferenceCubeCoordinates, ReferenceFaceBox1 ? SeparationDirectionLocalBox1 : -SeparationDirectionLocalBox2);

				Constraint.AddOneshotManifoldContact(ContactPoint);
			}
		}

		/////////////////////////////
		/// General Convexes
		/////////////////////////////

		// Reduce the number of contact points (in place)
		// Prerequisites to calling this function:
		// The points should be in a reference frame such that the z-axis is the in the direction of the separation vector
		uint32 ReduceManifoldContactPoints(FVec3* Points, uint32 PointCount)
		{
			uint32 OutPointCount = 0;
			if (PointCount <= 4)
				return PointCount;

			// Point 1) Find the deepest contact point
			{
				uint32 DeepestPointIndex = 0;
				FReal DeepestPointPhi = FLT_MAX;
				for (uint32 PointIndex = 0; PointIndex < PointCount; PointIndex++)
				{
					if (Points[PointIndex].Z < DeepestPointPhi)
					{
						DeepestPointIndex = PointIndex;
						DeepestPointPhi = Points[PointIndex].Z;
					}
				}
				// Deepest point will be our first output point
				Swap(Points[0], Points[DeepestPointIndex]);
				++OutPointCount;
			}

			// Point 2) Find the point with the largest distance to the deepest contact point (projected onto the separation plane)
			{
				uint32 FarthestPointIndex = 1;
				FReal FarthestPointDistanceSQR = -1.0f;
				for (uint32 PointIndex = 1; PointIndex < PointCount; PointIndex++)
				{
					FReal PointAToPointBSizeSQR = (Points[PointIndex] - Points[0]).SizeSquared2D();
					if (PointAToPointBSizeSQR > FarthestPointDistanceSQR)
					{
						FarthestPointIndex = PointIndex;
						FarthestPointDistanceSQR = PointAToPointBSizeSQR;
					}
				}
				// Farthest point will be added now
				Swap(Points[1], Points[FarthestPointIndex]);
				++OutPointCount;
			}

			// Point 3) Largest triangle area
			{
				uint32 LargestTrianglePointIndex = 2;
				FReal LargestTrianglePointSignedArea = 0.0f; // This will actually be double the signed area
				FVec3 P0to1 = Points[1] - Points[0];
				for (uint32 PointIndex = 2; PointIndex < PointCount; PointIndex++)
				{
					FReal TriangleSignedArea = (FVec3::CrossProduct(P0to1, Points[PointIndex] - Points[0])).Z; // Dot in direction of separation vector
					if (FMath::Abs(TriangleSignedArea) > FMath::Abs(LargestTrianglePointSignedArea))
					{
						LargestTrianglePointIndex = PointIndex;
						LargestTrianglePointSignedArea = TriangleSignedArea;
					}
				}
				// Point causing the largest triangle will be added now
				Swap(Points[2], Points[LargestTrianglePointIndex]);
				++OutPointCount;
				// Ensure the winding order is consistent
				if (LargestTrianglePointSignedArea < 0)
				{
					Swap(Points[0], Points[1]);
				}
			}

			// Point 4) Find the largest triangle connecting with our current triangle
			{
				uint32 LargestTrianglePointIndex = 3;
				FReal LargestPositiveTrianglePointSignedArea = 0.0f;
				for (uint32 PointIndex = 3; PointIndex < PointCount; PointIndex++)
				{
					for (uint32 EdgeIndex = 0; EdgeIndex < 3; EdgeIndex++)
					{
						FReal TriangleSignedArea = (FVec3::CrossProduct(Points[PointIndex] - Points[EdgeIndex], Points[(EdgeIndex + 1) % 3] - Points[EdgeIndex])).Z; // Dot in direction of separation vector
						if (TriangleSignedArea > LargestPositiveTrianglePointSignedArea)
						{
							LargestTrianglePointIndex = PointIndex;
							LargestPositiveTrianglePointSignedArea = TriangleSignedArea;
						}
					}
				}
				// Point causing the largest positive triangle area will be added now
				Swap(Points[3], Points[LargestTrianglePointIndex]);
				++OutPointCount;
			}

			return OutPointCount; // This should always be 4
		}

		// Reduce the number of contact points (in place)
		// Prerequisites to calling this function:
		// ContactPoints are sorted on phi (ascending)
		void ReduceManifoldContactPointsTriangeMesh(TArray<FContactPoint>& ContactPoints)
		{
			if (ContactPoints.Num() <= 4)
			{
				return;
			}

			// Point 1) is the deepest contact point
			// It is already in position
			
			// Point 2) Find the point with the largest distance to the deepest contact point
			{
				uint32 FarthestPointIndex = 1;
				FReal FarthestPointDistanceSQR = -1.0f;
				for (int32 PointIndex = 1; PointIndex < ContactPoints.Num(); PointIndex++)
				{
					FReal PointAToPointBSizeSQR = (ContactPoints[PointIndex].ShapeContactPoints[1] - ContactPoints[0].ShapeContactPoints[1]).SizeSquared();
					if (PointAToPointBSizeSQR > FarthestPointDistanceSQR)
					{
						FarthestPointIndex = PointIndex;
						FarthestPointDistanceSQR = PointAToPointBSizeSQR;
					}
				}
				// Farthest point will be added now
				Swap(ContactPoints[1], ContactPoints[FarthestPointIndex]);
			}

			// Point 3) Largest triangle area
			{
				uint32 LargestTrianglePointIndex = 2;
				FReal LargestTrianglePointSignedArea = 0.0f; // This will actually be double the signed area
				FVec3 P0to1 = ContactPoints[1].ShapeContactPoints[1] - ContactPoints[0].ShapeContactPoints[1];
				for (int32 PointIndex = 2; PointIndex < ContactPoints.Num(); PointIndex++)
				{
					FReal TriangleSignedArea = FVec3::DotProduct(FVec3::CrossProduct(P0to1, ContactPoints[PointIndex].ShapeContactPoints[1] - ContactPoints[0].ShapeContactPoints[1]), ContactPoints[0].ShapeContactNormal);
					if (FMath::Abs(TriangleSignedArea) > FMath::Abs(LargestTrianglePointSignedArea))
					{
						LargestTrianglePointIndex = PointIndex;
						LargestTrianglePointSignedArea = TriangleSignedArea;
					}
				}
				// Point causing the largest triangle will be added now
				Swap(ContactPoints[2], ContactPoints[LargestTrianglePointIndex]);
				// Ensure the winding order is consistent
				if (LargestTrianglePointSignedArea < 0)
				{
					Swap(ContactPoints[0], ContactPoints[1]);
				}
			}

			// Point 4) Find the largest triangle connecting with our current triangle
			{
				uint32 LargestTrianglePointIndex = 3;
				FReal LargestPositiveTrianglePointSignedArea = 0.0f;
				for (int32 PointIndex = 3; PointIndex < ContactPoints.Num(); PointIndex++)
				{
					for (uint32 EdgeIndex = 0; EdgeIndex < 3; EdgeIndex++)
					{
						FReal TriangleSignedArea = FVec3::DotProduct(FVec3::CrossProduct(ContactPoints[PointIndex].ShapeContactPoints[1] - ContactPoints[EdgeIndex].ShapeContactPoints[1], ContactPoints[(EdgeIndex + 1) % 3].ShapeContactPoints[1] - ContactPoints[EdgeIndex].ShapeContactPoints[1]), ContactPoints[0].ShapeContactNormal);
						if (TriangleSignedArea > LargestPositiveTrianglePointSignedArea)
						{
							LargestTrianglePointIndex = PointIndex;
							LargestPositiveTrianglePointSignedArea = TriangleSignedArea;
						}
					}
				}
				// Point causing the largest positive triangle area will be added now
				Swap(ContactPoints[3], ContactPoints[LargestTrianglePointIndex]);
			}

			ContactPoints.SetNum(4); // Will end up with 4 points
		}

		// This function will clip the input vertices by a reference shape's planes
		// more vertices may be added to outputVertexBuffer by this function
		// This is the core of the Sutherland-Hodgman algorithm
		// Plane Normals face outwards 
		uint32 ClipVerticesAgainstPlane(const FVec3* InputVertexBuffer, FVec3* outputVertexBuffer, uint32 ClipPointCount, uint32 MaxNumberOfOutputPoints, FVec3 ClippingPlaneNormal, FReal PlaneDistance)
		{

			auto CalculateIntersect = [=](const FVec3& Point1, const FVec3& Point2) -> FVec3
			{
				// Only needs to be valid if the line connecting Point1 with Point2 actually intersects
				FVec3 Result;

				FReal Denominator = FVec3::DotProduct(Point2 - Point1, ClippingPlaneNormal); // Can be negative
				if (FMath::Abs(Denominator) < SMALL_NUMBER)
				{
					Result = Point1;
				}
				else
				{
					FReal Alpha = (PlaneDistance - FVec3::DotProduct(Point1, ClippingPlaneNormal)) / Denominator;
					Result = FMath::Lerp(Point1, Point2, Alpha);
				}
				return Result;
			};

			auto InsideClipFace = [=](const FVec3& Point) -> bool
			{
				// Epsilon is there so that previously clipped points will still be inside the plane
				return FVec3::DotProduct(Point, ClippingPlaneNormal) <= PlaneDistance + PlaneDistance * SMALL_NUMBER; 
			};

			uint32 NewClipPointCount = 0;

			for (uint32 ClipPointIndex = 0; ClipPointIndex < ClipPointCount; ClipPointIndex++)
			{
				FVec3 CurrentClipPoint = InputVertexBuffer[ClipPointIndex];
				FVec3 PrevClipPoint = InputVertexBuffer[(ClipPointIndex + ClipPointCount - 1) % ClipPointCount];
				FVec3 InterSect = CalculateIntersect(PrevClipPoint, CurrentClipPoint);

				if (InsideClipFace(CurrentClipPoint))
				{
					if (!InsideClipFace(PrevClipPoint))
					{
						outputVertexBuffer[NewClipPointCount++] = InterSect;
						if (NewClipPointCount >= MaxNumberOfOutputPoints)
						{
							break;
						}
					}
					outputVertexBuffer[NewClipPointCount++] = CurrentClipPoint;
				}
				else if (InsideClipFace(PrevClipPoint))
				{
					outputVertexBuffer[NewClipPointCount++] = InterSect;
				}

				if (NewClipPointCount >= MaxNumberOfOutputPoints)
				{
					break;
				}
			}

			return NewClipPointCount;
		}

		template <typename ConvexImplicitType1, typename ConvexImplicitType2>
		FVec3* GenerateConvexManifoldClippedVertices(
			const ConvexImplicitType1& RefConvex,
			const ConvexImplicitType2& OtherConvex,
			const FRigidTransform3& OtherToRefTransform,
			const int32 RefPlaneIndex,
			const int32 OtherPlaneIndex,
			const FVec3& RefPlaneNormal,
			const FVec3& RefPlanePos,
			FVec3* VertexBuffer1,
			FVec3* VertexBuffer2,
			uint32& ContactPointCount,	// InOut
			const uint32 MaxContactPointCount
		)
		{
			// Populate the clipped vertices by the other face's vertices
			const int32 OtherConvexFaceVerticesNum = OtherConvex.NumPlaneVertices(OtherPlaneIndex);
			ContactPointCount = FMath::Min(OtherConvexFaceVerticesNum, (int32)MaxContactPointCount); // Number of face vertices
			for (int32 VertexIndex = 0; VertexIndex < (int32)ContactPointCount; ++VertexIndex)
			{
				// Todo Check for Grey code
				const FVec3 OtherVertex = OtherConvex.GetVertex(OtherConvex.GetPlaneVertex(OtherPlaneIndex, VertexIndex));
				VertexBuffer1[VertexIndex] = OtherToRefTransform.TransformPositionNoScale(OtherVertex);
			}

			// Now clip against all planes that belong to the reference plane's, edges
			// Note winding order matters here, and we have to handle negative scales
			const FReal RefWindingOrder = RefConvex.GetWindingOrder();
			const int32 RefConvexFaceVerticesNum = RefConvex.NumPlaneVertices(RefPlaneIndex);
			int32 ClippingPlaneCount = RefConvexFaceVerticesNum;
			FVec3 PrevPoint = RefConvex.GetVertex(RefConvex.GetPlaneVertex(RefPlaneIndex, ClippingPlaneCount - 1));
			for (int32 ClippingPlaneIndex = 0; ClippingPlaneIndex < ClippingPlaneCount; ++ClippingPlaneIndex)
			{
				FVec3 CurrentPoint = RefConvex.GetVertex(RefConvex.GetPlaneVertex(RefPlaneIndex, ClippingPlaneIndex));
				FVec3 ClippingPlaneNormal = RefWindingOrder * FVec3::CrossProduct(RefPlaneNormal, PrevPoint - CurrentPoint);
				ClippingPlaneNormal.SafeNormalize();
				ContactPointCount = ClipVerticesAgainstPlane(VertexBuffer1, VertexBuffer2, ContactPointCount, MaxContactPointCount, ClippingPlaneNormal, FVec3::DotProduct(CurrentPoint, ClippingPlaneNormal));
				Swap(VertexBuffer1, VertexBuffer2); // VertexBuffer1 will now point to the latest
				PrevPoint = CurrentPoint;
			}

			return VertexBuffer1;
		}

		// Use GJK to find the closest points (or shallowest penetrating points) on two convex shapes usingthe specified margin
		// @todo(chaos): dedupe from GJKContactPoint in CollisionResolution.cpp
		template <typename GeometryA, typename GeometryB>
		FContactPoint GJKContactPointMargin(const GeometryA& A, const GeometryB& B, const FRigidTransform3& ATM, const FRigidTransform3& BTM, FReal MarginA, FReal MarginB, FGJKSimplexData& InOutGjkWarmStartData, FReal& OutMaxMarginDelta, int32& VertexIndexA, int32& VertexIndexB)
		{
			SCOPE_CYCLE_COUNTER_MANIFOLD_GJK();

			FContactPoint Contact;

			FReal Penetration;
			FVec3 ClosestA, ClosestB, NormalA, NormalB;

			// Slightly increased epsilon to reduce error in normal for almost touching objects.
			const FReal Epsilon = 3.e-3f;

			const TGJKCoreShape<GeometryA> AWithMargin(A, MarginA);
			const TGJKCoreShape<GeometryB> BWithMargin(B, MarginB);
			const FRigidTransform3 BToATM = BTM.GetRelativeTransformNoScale(ATM);

			if (GJKPenetrationWarmStartable(AWithMargin, BWithMargin, BToATM, Penetration, ClosestA, ClosestB, NormalA, NormalB, VertexIndexA, VertexIndexB, InOutGjkWarmStartData, OutMaxMarginDelta, Epsilon))
			{
				Contact.ShapeContactPoints[0] = ClosestA;
				Contact.ShapeContactPoints[1] = ClosestB;
				Contact.ShapeContactNormal = -NormalB;	// We want normal pointing from B to A
				const FVec3 WorldLocationA = ATM.TransformPositionNoScale(ClosestA);
				const FVec3 WorldLocationB = BTM.TransformPositionNoScale(ClosestB);
				Contact.Location = FReal(0.5) * (WorldLocationA + WorldLocationB);
				Contact.Normal = -ATM.TransformVectorNoScale(NormalA);
				Contact.Phi = -Penetration;
			}

			return Contact;
		}

		// Find the the most opposing plane given a position and a direction
		template <typename ConvexImplicitType>
		void FindBestPlane(
			const ConvexImplicitType& Convex,
			const FVec3& X,
			const FVec3& N,
			const FReal MaxDistance,
			const int32 PlaneIndex,
			int32& BestPlaneIndex,
			FReal& BestPlaneDot)
		{
			const TPlaneConcrete<FReal, 3> Plane = Convex.GetPlane(PlaneIndex);
			const FReal PlaneNormalDotN = FVec3::DotProduct(N, Plane.Normal());
				
			// Ignore planes that do not oppose N
			if (PlaneNormalDotN <= -SMALL_NUMBER)
			{
				// Reject planes farther than MaxDistance
				const FReal PlaneDistance = Plane.SignedDistance(X);
				if (FMath::Abs(PlaneDistance) <= MaxDistance)
				{
					// Keep the most opposing plane
					if (PlaneNormalDotN < BestPlaneDot)
					{
						BestPlaneDot = PlaneNormalDotN;
						BestPlaneIndex = PlaneIndex;
					}
				}
			}
		}

		// Select one of the planes on the convex to use as the contact plane, given an estimated contact position and opposing 
		// normal from GJK with margins (which gives the shapes rounded corners/edges).
		template <typename ConvexImplicitType>
		int32 SelectContactPlane(
			const ConvexImplicitType& Convex,
			const FVec3 X,
			const FVec3 N,
			const FReal InMaxDistance,
			const int32 VertexIndex)
		{
			// Handle InMaxDistance = 0. We expect that the X is actually on the surface in this case, so the search distance just needs to be some reasonable tolerance.
			// @todo(chaos): this should probable be dependent on the size of the objects...
			const FReal MinFaceSearchDistance = Chaos_Collision_Manifold_MinFaceSearchDistance;
			const FReal MaxDistance = FMath::Max(InMaxDistance, MinFaceSearchDistance);

			int32 BestPlaneIndex = INDEX_NONE;
			FReal BestPlaneDot = 1.0f;
			{
				int32 PlaneIndices[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
				int32 NumPlanes = Convex.GetVertexPlanes3(VertexIndex, PlaneIndices[0], PlaneIndices[1], PlaneIndices[2]);

				// If we have more than 3 planes we iterate over the full set of planes since it is faster than using the half edge structure
				if(NumPlanes > 3)
				{
					NumPlanes = Convex.NumPlanes();
					for (int32 PlaneIndex = 0; PlaneIndex < NumPlanes; ++PlaneIndex)
					{
						FindBestPlane(Convex, X, N, MaxDistance, PlaneIndex, BestPlaneIndex, BestPlaneDot);
					}
				}
				// Otherwise we iterate over the cached planes
				else
				{
					for (int32 PlaneIndex = 0; PlaneIndex < NumPlanes; ++PlaneIndex)
					{
						FindBestPlane(Convex, X, N, MaxDistance, PlaneIndices[PlaneIndex], BestPlaneIndex, BestPlaneDot);
					}
				}
			}
			
			// Malformed convexes or half-spaces or capsules could have all planes rejected above.
			// If that happens, select the most opposing plane including those that
			// may point the same direction as N. 
			if (BestPlaneIndex == INDEX_NONE)
			{
				// This always returns a valid plane.
				BestPlaneIndex = Convex.GetMostOpposingPlane(N);
			}

			check(BestPlaneIndex != INDEX_NONE);
			return BestPlaneIndex;
		}


		template <typename ConvexImplicitType1, typename ConvexImplicitType2>
		void ConstructConvexConvexOneShotManifold(
			const ConvexImplicitType1& Convex1,
			const FRigidTransform3& Convex1Transform, //world
			const ConvexImplicitType2& Convex2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint)
		{
			SCOPE_CYCLE_COUNTER_MANIFOLD();

			const uint32 SpaceDimension = 3;
			const bool bConvex1IsCapsule = (Convex1.GetType() & ~(ImplicitObjectType::IsInstanced | ImplicitObjectType::IsScaled)) == ImplicitObjectType::Capsule;

			// We only build one shot manifolds once
			// All convexes are pre-scaled, or wrapped in TImplicitObjectScaled
			//ensure(Constraint.GetManifoldPoints().Num() == 0);
			ensure(Convex1Transform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));
			ensure(Convex2Transform.GetScale3D() == FVec3(1.0f, 1.0f, 1.0f));

			// Get the adjusted margins for each convex
			const FReal Margin1 = Constraint.GetCollisionMargin0();
			const FReal Margin2 = Constraint.GetCollisionMargin1();

			// Find the deepest penetration. This is used to determine the planes and points to use for the manifold
			// MaxMarginDelta is an upper bound on the distance from the contact on the rounded core shape to the actual shape surface. 
			FReal MaxMarginDelta = FReal(0);
			int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;
			FContactPoint GJKContactPoint = GJKContactPointMargin(Convex1, Convex2, Convex1Transform, Convex2Transform, Margin1, Margin2, Constraint.GetGJKWarmStartData(), MaxMarginDelta, VertexIndexA, VertexIndexB);
			PHYSICS_CSV_CUSTOM_EXPENSIVE(PhysicsCounters, NumManifoldsGJKCalled, 1, ECsvCustomStatOp::Accumulate);

			const bool bCanUpdateManifold = bChaos_Collision_EnableManifoldInject;
			if (bCanUpdateManifold && Constraint.TryAddManifoldContact(GJKContactPoint, Convex1Transform, Convex2Transform))
			{
				PHYSICS_CSV_CUSTOM_EXPENSIVE(PhysicsCounters, NumManifoldsMaintained, 1, ECsvCustomStatOp::Accumulate);
				return;
			}

			Constraint.ResetActiveManifoldContacts();

			// GJK is using margins and rounded corner, so if we have a corner-to-corner contact it will under-report the actual distance by an amount that depends on how
			// "pointy" the edge/corner is - this error is bounded by MaxMarginDelta.
			const FReal GJKCullDistance = Constraint.GetCullDistance() + MaxMarginDelta;
			if (GJKContactPoint.Phi > GJKCullDistance)
			{
				PHYSICS_CSV_CUSTOM_EXPENSIVE(PhysicsCounters, NumManifoldsGJKCulled, 1, ECsvCustomStatOp::Accumulate);
				return;
			}

			PHYSICS_CSV_CUSTOM_EXPENSIVE(PhysicsCounters, NumManifoldsCreated, 1, ECsvCustomStatOp::Accumulate);

			// @todo(chaos): get the vertex index from GJK and use to to get the plane
			const FVec3 SeparationDirectionLocalConvex1 = Convex1Transform.InverseTransformVectorNoScale(GJKContactPoint.Normal);
			const int32 MostOpposingPlaneIndexConvex1 = SelectContactPlane(Convex1, GJKContactPoint.ShapeContactPoints[0], SeparationDirectionLocalConvex1, Margin1, VertexIndexA);
			const TPlaneConcrete<FReal, 3> BestPlaneConvex1 = Convex1.GetPlane(MostOpposingPlaneIndexConvex1);
			const FReal BestPlaneDotNormalConvex1 = !bConvex1IsCapsule ? FVec3::DotProduct(-SeparationDirectionLocalConvex1, BestPlaneConvex1.Normal()) : -FLT_MAX;

			// Now for Convex2
			const FVec3 SeparationDirectionLocalConvex2 = Convex2Transform.InverseTransformVectorNoScale(GJKContactPoint.Normal);
			const int32 MostOpposingPlaneIndexConvex2 = SelectContactPlane(Convex2, GJKContactPoint.ShapeContactPoints[1], -SeparationDirectionLocalConvex2, Margin2, VertexIndexB);
			const TPlaneConcrete<FReal, 3> BestPlaneConvex2 = Convex2.GetPlane(MostOpposingPlaneIndexConvex2);
			const FReal BestPlaneDotNormalConvex2 = FVec3::DotProduct(SeparationDirectionLocalConvex2, BestPlaneConvex2.Normal());

			const FReal SmallBiasToPreventFeatureFlipping = 0.002f; // This improves frame coherence by penalizing convex 1 in favour of convex 2
			bool ReferenceFaceConvex1 = true; // Is the reference face on convex1 or convex2?
			if (BestPlaneDotNormalConvex2 + SmallBiasToPreventFeatureFlipping > BestPlaneDotNormalConvex1)
			{
				ReferenceFaceConvex1 = false;
			}

			// Is this a vertex-plane or edge-edge contact? 
			const FReal PlaneContactNormalEpsilon = Chaos_Collision_Manifold_PlaneContactNormalEpsilon;
			const bool bIsPlaneContact = FMath::IsNearlyEqual(BestPlaneDotNormalConvex1, (FReal)1., PlaneContactNormalEpsilon) || FMath::IsNearlyEqual(BestPlaneDotNormalConvex2, (FReal)1., PlaneContactNormalEpsilon);

			// For edge-edge contacts, we find the edges involved and project the contact onto the edges
			if (!bIsPlaneContact)
			{
				SCOPE_CYCLE_COUNTER_MANIFOLD_ADDEDGEEDGE();

				if (ForceOneShotManifoldEdgeEdgeCaseZeroCullDistance && GJKContactPoint.Phi > 0)
				{
					return;
				}

				// @todo(chaos): this does not work well when the edges are parallel. We should always have points with zero
				// position delta perpendicular to the normal, but that is not the case for parallel edges
				FVec3 ShapeEdgePos1 = Convex1.GetClosestEdgePosition(MostOpposingPlaneIndexConvex1, GJKContactPoint.ShapeContactPoints[0]);
				FVec3 ShapeEdgePos2 = Convex2.GetClosestEdgePosition(MostOpposingPlaneIndexConvex2, GJKContactPoint.ShapeContactPoints[1]);
				if (bConvex1IsCapsule)
				{
					ShapeEdgePos1 -= Margin1 * SeparationDirectionLocalConvex1;
				}

				const FVec3 EdgePos1 = Convex1Transform.TransformPosition(ShapeEdgePos1);
				const FVec3 EdgePos2 = Convex2Transform.TransformPosition(ShapeEdgePos2);
				const FReal EdgePhi = FVec3::DotProduct(EdgePos1 - EdgePos2, GJKContactPoint.Normal);
				const FVec3 WorldPos = FReal(0.5) * (EdgePos1 + EdgePos2);
				const FVec3& WorldNormal = GJKContactPoint.Normal;

				GJKContactPoint.ShapeContactPoints[0] = Convex1Transform.InverseTransformPositionNoScale(WorldPos + FReal(0.5) * EdgePhi * WorldNormal);
				GJKContactPoint.ShapeContactPoints[1] = Convex2Transform.InverseTransformPositionNoScale(WorldPos - FReal(0.5) * EdgePhi * WorldNormal);
				GJKContactPoint.Phi = EdgePhi;
				GJKContactPoint.Location = WorldPos;
				// Normal unchanged from GJK result

				Constraint.AddOneshotManifoldContact(GJKContactPoint);
				return;
			}

			// For vertex-plane contacts, we use a convex face as the manifold plane
			const FVec3 RefSeparationDirection = ReferenceFaceConvex1 ? SeparationDirectionLocalConvex1 : SeparationDirectionLocalConvex2;
			const FVec3 RefPlaneNormal = ReferenceFaceConvex1 ? BestPlaneConvex1.Normal() : BestPlaneConvex2.Normal();
			const FVec3 RefPlanePosition = ReferenceFaceConvex1 ? BestPlaneConvex1.X() : BestPlaneConvex2.X();

			// @todo(chaos): fix use of hard-coded max array size
			// We will use a double buffer as an optimization
			const uint32 MaxContactPointCount = 32; // This should be tuned
			uint32 ContactPointCount = 0;
			FVec3 ClippedVertices1[MaxContactPointCount];
			FVec3 ClippedVertices2[MaxContactPointCount];
			FVec3* ClippedVertices = nullptr;
			const FRigidTransform3* RefConvexTM;
			FRigidTransform3 ConvexOtherToRef;

			{
				SCOPE_CYCLE_COUNTER_MANIFOLD_CLIP();
				if (ReferenceFaceConvex1)
				{
					RefConvexTM = &Convex1Transform;
					ConvexOtherToRef = Convex2Transform.GetRelativeTransform(Convex1Transform);

					ClippedVertices = GenerateConvexManifoldClippedVertices(
						Convex1,
						Convex2,
						ConvexOtherToRef, 
						MostOpposingPlaneIndexConvex1,
						MostOpposingPlaneIndexConvex2,
						RefPlaneNormal,
						RefPlanePosition,
						ClippedVertices1,
						ClippedVertices2,
						ContactPointCount,
						MaxContactPointCount);
				}
				else
				{
					RefConvexTM = &Convex2Transform;
					ConvexOtherToRef = Convex1Transform.GetRelativeTransform(Convex2Transform);

					ClippedVertices = GenerateConvexManifoldClippedVertices(
						Convex2,
						Convex1,
						ConvexOtherToRef,
						MostOpposingPlaneIndexConvex2,
						MostOpposingPlaneIndexConvex1,
						RefPlaneNormal,
						RefPlanePosition,
						ClippedVertices1,
						ClippedVertices2,
						ContactPointCount,
						MaxContactPointCount);
				}
			}

			// If we have the max number of contact points already, they will be in cyclic order. Stability is better
			// if we solve points non-sequentially (e.g., on a box, solve one point, then it's opposite corner).
			// If we have more than 4 contacts, the contact reduction step already effectively does something similar.
			if (ContactPointCount == 4)
			{
				Swap(ClippedVertices[1], ClippedVertices[2]);
			}

			// Reduce number of contacts to the maximum allowed
			if (ContactPointCount > 4)
			{
				SCOPE_CYCLE_COUNTER_MANIFOLD_REDUCE();

				FRotation3 RotateSeperationToZ = FRotation3::FromRotatedVector(RefPlaneNormal, FVec3(0.0f, 0.0f, 1.0f));
				for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
				{
					ClippedVertices[ContactPointIndex] = RotateSeperationToZ * ClippedVertices[ContactPointIndex];
				}

				ContactPointCount = ReduceManifoldContactPoints(ClippedVertices, ContactPointCount);

				for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
				{
					ClippedVertices[ContactPointIndex] = RotateSeperationToZ.Inverse() * ClippedVertices[ContactPointIndex];
				}
			}

			// Generate the contact points from the clipped vertices
			{
				SCOPE_CYCLE_COUNTER_MANIFOLD_ADDFACEVERTEX();
				for (uint32 ContactPointIndex = 0; ContactPointIndex < ContactPointCount; ++ContactPointIndex)
				{
					FContactPoint ContactPoint;
					FVec3 VertexInReferenceCoordinates = ClippedVertices[ContactPointIndex];
					if (bConvex1IsCapsule)
					{
						VertexInReferenceCoordinates -= Margin1 * RefSeparationDirection;
					}
					FVec3 PointProjectedOntoReferenceFace = VertexInReferenceCoordinates - FVec3::DotProduct(VertexInReferenceCoordinates - RefPlanePosition, RefPlaneNormal) * RefPlaneNormal;
					FVec3 ClippedPointInOtherCoordinates = ConvexOtherToRef.InverseTransformPositionNoScale(VertexInReferenceCoordinates);

					ContactPoint.ShapeContactPoints[0] = ReferenceFaceConvex1 ? PointProjectedOntoReferenceFace : ClippedPointInOtherCoordinates;
					ContactPoint.ShapeContactPoints[1] = ReferenceFaceConvex1 ? ClippedPointInOtherCoordinates : PointProjectedOntoReferenceFace;
					ContactPoint.ShapeContactNormal = SeparationDirectionLocalConvex2;
					ContactPoint.Location = ReferenceFaceConvex1 ? RefConvexTM->TransformPositionNoScale(VertexInReferenceCoordinates) : RefConvexTM->TransformPositionNoScale(PointProjectedOntoReferenceFace);
					ContactPoint.Normal = GJKContactPoint.Normal;
					ContactPoint.Phi = FVec3::DotProduct(PointProjectedOntoReferenceFace - VertexInReferenceCoordinates, ReferenceFaceConvex1 ? SeparationDirectionLocalConvex1 : -SeparationDirectionLocalConvex2);				

					Constraint.AddOneshotManifoldContact(ContactPoint);
				}
			}
		}



		//
		// Explicit instantiations of all convex-convex manifold combinations we support
		// Box, Convex, Scaled-Convex
		//

		template 
		void ConstructConvexConvexOneShotManifold<FImplicitBox3, FImplicitBox3>(
			const FImplicitBox3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitBox3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitBox3, FImplicitConvex3>(
			const FImplicitBox3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitConvex3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitConvex3, FImplicitBox3>(
			const FImplicitConvex3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitBox3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitBox3, TImplicitObjectInstanced<FImplicitConvex3>>(
			const FImplicitBox3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectInstanced<FImplicitConvex3>, FImplicitBox3>(
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitBox3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitBox3, TImplicitObjectScaled<FImplicitConvex3>>(
			const FImplicitBox3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<FImplicitConvex3>, FImplicitBox3>(
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitBox3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitConvex3, FImplicitConvex3>(
			const FImplicitConvex3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitConvex3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectInstanced<FImplicitConvex3>, FImplicitConvex3>(
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitConvex3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<FImplicitConvex3>, FImplicitConvex3>(
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FImplicitConvex3& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitConvex3, TImplicitObjectInstanced<FImplicitConvex3>>(
			const FImplicitConvex3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitConvex3, TImplicitObjectScaled<FImplicitConvex3>>(
			const FImplicitConvex3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectInstanced<FImplicitConvex3>, TImplicitObjectInstanced<FImplicitConvex3>>(
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<FImplicitConvex3>, TImplicitObjectInstanced<FImplicitConvex3>>(
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectInstanced<FImplicitConvex3>, TImplicitObjectScaled<FImplicitConvex3>>(
			const TImplicitObjectInstanced<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<FImplicitConvex3>, TImplicitObjectScaled<FImplicitConvex3>>(
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<FImplicitConvex3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitBox3, FTriangle>(
			const FImplicitBox3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FTriangle& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<FImplicitConvex3, FTriangle>(
			const FImplicitConvex3& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FTriangle& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<class TBox<FReal, 3>, 1>, FTriangle>(
			const TImplicitObjectScaled<class Chaos::TBox<FReal, 3>, 1>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FTriangle& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<class FConvex, 1>, FTriangle>(
			const Chaos::TImplicitObjectScaled<class FConvex, 1>& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FTriangle& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, class FConvex>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const FConvex& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, TImplicitObjectScaled<class FConvex, 1>>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<class FConvex, 1>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, class TImplicitObjectInstanced<class FConvex>>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<class FConvex>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, class TBox<FReal, 3>>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TBox<FReal, 3>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, TImplicitObjectScaled<class TBox<FReal, 3>, 1>>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectScaled<class TBox<FReal, 3>, 1>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
		void ConstructConvexConvexOneShotManifold<class FCapsule, class TImplicitObjectInstanced<class TBox<FReal, 3>>>(
			const FCapsule& Implicit1,
			const FRigidTransform3& Convex1Transform, //world
			const TImplicitObjectInstanced<class TBox<FReal, 3>>& Implicit2,
			const FRigidTransform3& Convex2Transform, //world
			const FReal Dt,
			FPBDCollisionConstraint& Constraint);

		template
			void ConstructConvexConvexOneShotManifold<class FCapsule, FTriangle>(
				const FCapsule& Implicit1,
				const FRigidTransform3& Convex1Transform, //world
				const FTriangle& Implicit2,
				const FRigidTransform3& Convex2Transform, //world
				const FReal Dt,
				FPBDCollisionConstraint& Constraint);

		template
			void ConstructConvexConvexOneShotManifold<TImplicitObjectScaled<class FCapsule, 1>, class FTriangle>(
				const TImplicitObjectScaled<class FCapsule, 1>& Implicit1,
				const FRigidTransform3& Convex1Transform, //world
				const FTriangle& Implicit2,
				const FRigidTransform3& Convex2Transform, //world
				const FReal Dt,
				FPBDCollisionConstraint& Constraint);
		
	}
}

