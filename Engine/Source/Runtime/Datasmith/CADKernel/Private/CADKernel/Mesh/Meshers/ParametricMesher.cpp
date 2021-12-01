// Copyright Epic Games, Inc. All Rights Reserved.

#include "CADKernel/Mesh/Meshers/ParametricMesher.h"

#include "CADKernel/Core/KernelParameters.h"
#include "CADKernel/Mesh/Criteria/CriteriaGrid.h"
#include "CADKernel/Mesh/Criteria/Criterion.h"
#include "CADKernel/Mesh/Meshers/IsoTriangulator.h"
#include "CADKernel/Mesh/Meshers/MesherTools.h"
#include "CADKernel/Mesh/Structure/EdgeMesh.h"
#include "CADKernel/Mesh/Structure/FaceMesh.h"
#include "CADKernel/Mesh/Structure/ModelMesh.h"
#include "CADKernel/Mesh/Structure/VertexMesh.h"
#include "CADKernel/Topo/Body.h"
#include "CADKernel/Topo/Model.h"
#include "CADKernel/Topo/Shell.h"
#include "CADKernel/Topo/TopologicalFace.h"
#include "CADKernel/Utils/Util.h"

//#define DEBUG_MESH_EDGE
//#define DEBUG_GETPREFERREDUVCOORDINATESFROMNEIGHBOURS
namespace CADKernel
{

FMesherParameters::FMesherParameters()
	: InconsistencyAngle(TEXT("inconsistencyAngle"), 20., *this)
{}

FParametricMesher::FParametricMesher(TSharedRef<FModelMesh> InMeshModel)
	: MeshModel(InMeshModel)
	, Parameters(MakeShared<FMesherParameters>())
{
}

void FParametricMesher::MeshEntities(TArray<TSharedPtr<FEntity>>& InEntities)
{
	int32 FaceCount = 0;

	for (TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		Face->SetMarker1();
	}

	// count faces
	for (TSharedPtr<FEntity>& Entity : InEntities)
	{
		TSharedPtr<FTopologicalEntity> TopologicalEntity = StaticCastSharedPtr<FTopologicalEntity>(Entity);
		if (!TopologicalEntity.IsValid())
		{
			continue;
		}
		FaceCount += TopologicalEntity->FaceCount();
	}
	Faces.Reserve(Faces.Num() + FaceCount);

	for (TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		Face->ResetMarkers();
	}

	// Get independent Faces and spread body's shells orientation
	for (TSharedPtr<FEntity>& Entity : InEntities)
	{
		TSharedPtr<FTopologicalEntity> TopologicalEntity = StaticCastSharedPtr<FTopologicalEntity>(Entity);
		if (!TopologicalEntity.IsValid())
		{
			continue;
		}

		TopologicalEntity->SpreadBodyOrientation();
		TopologicalEntity->GetFaces(Faces);
	}

	for (TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		Face->ResetMarkers();
	}

	// Get independent elementary entities (Edge, Vertex)
	for (TSharedPtr<FEntity>& Entity : InEntities)
	{
		if (!Entity.IsValid())
		{
			continue;
		}

		switch (Entity->GetEntityType())
		{
		case EEntity::TopologicalEdge:
			Edges.Add(StaticCastSharedPtr<FTopologicalEdge>(Entity));
			break;

		case EEntity::TopologicalVertex:
			Vertices.Add(StaticCastSharedPtr<FTopologicalVertex>(Entity));
			break;

		default:
			break;
		}
	}

	MeshEntities();
}

void FParametricMesher::MeshEntities()
{
	FTimePoint StartTime = FChrono::Now();
	FTimePoint ApplyCriteriaStartTime = FChrono::Now();

	FProgress ProgressBar(Faces.Num() * 2, TEXT("Meshing Entities : Apply Surface Criteria"));

	// ============================================================================================================
	//      Apply Surface Criteria
	// ============================================================================================================

	for (TSharedPtr<FTopologicalFace> Face : Faces)
	{
		FProgress _(1, TEXT("Meshing Entities : Apply Surface Criteria"));

		ensureCADKernel(Face.IsValid());
		ensureCADKernel(!Face->IsDeleted());

		ApplyFaceCriteria(Face.ToSharedRef());
	}

	Chronos.ApplyCriteriaDuration = FChrono::Elapse(ApplyCriteriaStartTime);

	FTimePoint MeshingStartTime = FChrono::Now();

	// ============================================================================================================
	//      Find quad surfaces 
	// ============================================================================================================

	TArray<FCostToFace> QuadTrimmedSurfaceSet;

	if (Faces.Num() > 1)
	{
		TArray<TSharedPtr<FTopologicalFace>> OtherEntities;

		FMessage::Printf(Log, TEXT("  Isolate QuadPatch\n"));
		FTimePoint IsolateQuadPatchStartTime = FChrono::Now();

		IsolateQuadFace(QuadTrimmedSurfaceSet, OtherEntities);

		Chronos.IsolateQuadPatchDuration = FChrono::Elapse(IsolateQuadPatchStartTime);
		FMessage::Printf(Log, TEXT("  %d Quad Surfaces found\n"), QuadTrimmedSurfaceSet.Num());
	}

	// ============================================================================================================
	//      Mesh surfaces 
	// ============================================================================================================

	FMessage::Printf(Log, TEXT("  Mesh Surfaces\n"));

	FTimePoint MeshStartTime = FChrono::Now();
	MeshSurfaceByFront(QuadTrimmedSurfaceSet);
	Chronos.GlobalMeshDuration = FChrono::Elapse(MeshStartTime);
	Chronos.GlobalDuration = FChrono::Elapse(StartTime);

	Chronos.PrintTimeElapse();
}

void FParametricMesher::ApplyFaceCriteria(TSharedRef<FTopologicalFace> Face)
{
	if (Face->IsApplyCriteria())
	{
		return;
	}

	FCriteriaGrid Grid(Face);
	Grid.ApplyCriteria(GetMeshModel()->GetCriteria());

	Face->ChooseFinalDeltaUs();
	Face->SetApplyCriteria();
}

void FParametricMesher::ApplyEdgeCriteria(FTopologicalEdge& Edge)
{
	FTopologicalEdge& ActiveEdge = *Edge.GetLinkActiveEdge();
	ensureCADKernel(!ActiveEdge.IsApplyCriteria());

	Edge.ComputeCrossingPointCoordinates();
	Edge.InitDeltaUs();
	const TArray<double>& CrossingPointUs = Edge.GetCrossingPointUs();

	TArray<double> Coordinates;
	Coordinates.SetNum(CrossingPointUs.Num() * 2 - 1);
	Coordinates[0] = CrossingPointUs[0];
	for (int32 ICuttingPoint = 1; ICuttingPoint < Edge.GetCrossingPointUs().Num(); ICuttingPoint++)
	{
		Coordinates[2 * ICuttingPoint - 1] = (CrossingPointUs[ICuttingPoint - 1] + CrossingPointUs[ICuttingPoint]) * 0.5;
		Coordinates[2 * ICuttingPoint] = CrossingPointUs[ICuttingPoint];
	}

	TArray<FCurvePoint> Points3D;
	Edge.EvaluatePoints(Coordinates, 0, Points3D);

	const TArray<TSharedPtr<FCriterion>>& Criteria = GetMeshModel()->GetCriteria();
	for (const TSharedPtr<FCriterion>& Criterion : Criteria)
	{
		Criterion->ApplyOnEdgeParameters(Edge, CrossingPointUs, Points3D);
	}

	Edge.ChooseFinalDeltaUs();
	Edge.SetApplyCriteria();
	ActiveEdge.SetApplyCriteria();
}

void FParametricMesher::Mesh(TSharedRef<FTopologicalFace> Face)
{
	ensureCADKernel(!Face->IsDeleted());
	ensureCADKernel(!Face->IsMeshed());

	FMessage::Printf(EVerboseLevel::Debug, TEXT("Meshing of surface %d\n"), Face->GetId());

	FProgress _(1, TEXT("Meshing Entities : Mesh Surface"));

	if (BoolDisplayDebugMeshStep)
	{
		Open3DDebugSession(FString::Printf(TEXT("Mesh of surface %d"), Face->GetId()));
	}

	FTimePoint StartTime = FChrono::Now();

	FTimePoint GenerateCloudStartTime = FChrono::Now();

	FGrid Grid(Face, MeshModel);
	GenerateCloud(Grid);

	if (Grid.IsDegenerated())
	{
		if (BoolDisplayDebugMeshStep)
		{
			Close3DDebugSession();
		}
		FMessage::Printf(EVerboseLevel::Log, TEXT("The meshing of the surface %d failed due to a degenerated grid\n"), Face->GetId());
		Face->SetMeshed();
		return;
	}

	FDuration GenerateCloudDuration = FChrono::Elapse(GenerateCloudStartTime);

	FTimePoint IsoTriangulerStartTime = FChrono::Now();

	TSharedRef<FFaceMesh> SurfaceMesh = StaticCastSharedRef<FFaceMesh>(Face->GetOrCreateMesh(MeshModel));

	FIsoTriangulator IsoTrianguler(Grid, SurfaceMesh);
	if (IsoTrianguler.Triangulate())
	{
		if (Face->IsBackOriented())
		{
			SurfaceMesh->InverseOrientation();
		}
		MeshModel->AddMesh(SurfaceMesh);
	}
	Face->SetMeshed();

	FDuration TriangulateDuration = FChrono::Elapse(IsoTriangulerStartTime);
	FDuration Duration = FChrono::Elapse(StartTime);

#ifdef CADKERNEL_DEV
	Chronos.GlobalPointCloudDuration += Grid.Chronos.GeneratePointCloudDuration;
	Chronos.GlobalGeneratePointCloudDuration += GenerateCloudDuration;
	Chronos.GlobalTriangulateDuration += TriangulateDuration;
	Chronos.GlobalDelaunayDuration += IsoTrianguler.Chronos.FindSegmentToLinkLoopToLoopByDelaunayDuration;
	Chronos.GlobalMeshDuration += Duration;
#endif

	if (BoolDisplayDebugMeshStep)
	{
		Close3DDebugSession();
	}
}

void FParametricMesher::GenerateCloud(FGrid& Grid)
{
	Grid.DefineCuttingParameters();
	if (!Grid.GeneratePointCloud())
	{
		return;
	}

	bool bFindThinZone = false;
	if (bFindThinZone)
	{
		FTimePoint StartTime = FChrono::Now();
		Grid.SearchThinZones();

		if (Grid.GetFace()->HasThinZone())
		{
#ifdef DEBUG_THIN_ZONES
			{
				F3DDebugSession _(TEXT("Thin Surface"));
				Display(Grid.GetSurface());
			}
#endif
			FTimePoint MeshThinZonesTime = FChrono::Now();
			MeshThinZoneEdges(Grid);
			Chronos.GlobalMeshThinZones += FChrono::Elapse(MeshThinZonesTime);
		}
		Chronos.GlobalThinZones += FChrono::Elapse(StartTime);
	}

	Grid.DisplayInnerDomainPoints(TEXT("FGrid::PointCloud 2D"), Grid.GetInner2DPoints(EGridSpace::Default2D));
	//Wait(Grid.bDisplay);

	FTimePoint StartTime = FChrono::Now();
	MeshFaceLoops(Grid);

	Grid.ProcessPointCloud();

	Chronos.GlobalMeshAndGetLoopNodes += FChrono::Elapse(StartTime);
}

void FParametricMesher::MeshFaceLoops(FGrid& Grid)
{
	TSharedRef<FTopologicalFace> Face = Grid.GetFace();

	FTimePoint StartTime = FChrono::Now();

	for (const TSharedPtr<FTopologicalLoop>& Loop : Face->GetLoops())
	{
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			Mesh(*Edge.Entity, *Face);
		}
	}

	Chronos.GlobalMeshEdges += FChrono::Elapse(StartTime);
}

static void FillImposedIsoCuttingPoints(TArray<double>& UEdgeSetOfIntersectionWithIso, ECoordinateType CoordinateType, double EdgeToleranceGeo, const FTopologicalEdge& Edge, TArray<FCuttingPoint>& OutImposedIsoVertexSet)
{
	FLinearBoundary EdgeBoundary = Edge.GetBoundary();

	int32 StartIndex = OutImposedIsoVertexSet.Num();
	Algo::Sort(UEdgeSetOfIntersectionWithIso);
	double PreviousU = -HUGE_VALUE;
	for (double InterU : UEdgeSetOfIntersectionWithIso)
	{
		// Remove coordinate nearly equal to boundary
		if ((InterU - EdgeToleranceGeo) < EdgeBoundary.GetMin() || (InterU + EdgeToleranceGeo) > EdgeBoundary.GetMax())
		{
			continue;
		}

		// Remove nearly duplicate 
		if (InterU - PreviousU < EdgeToleranceGeo)
		{
			continue;
		}

		OutImposedIsoVertexSet.Emplace(InterU, CoordinateType);
		PreviousU = InterU;
	}

	int32 Index;
	int32 NewCoordinateCount = OutImposedIsoVertexSet.Num() - StartIndex;
	switch (NewCoordinateCount)
	{
	case 0:
		return;

	case 1:
	{
		int32 CuttingPointIndex = 0;
		while (CuttingPointIndex < Edge.GetCrossingPointUs().Num() && Edge.GetCrossingPointUs()[CuttingPointIndex] + SMALL_NUMBER <= OutImposedIsoVertexSet[StartIndex].Coordinate)
		{
			++CuttingPointIndex;
		};
		if (CuttingPointIndex > 0)
		{
			--CuttingPointIndex;
		}
		OutImposedIsoVertexSet[StartIndex].IsoDeltaU = Edge.GetDeltaUMaxs()[CuttingPointIndex] * AQuarter;
		break;
	}

	default:
	{
		OutImposedIsoVertexSet[StartIndex].IsoDeltaU = (OutImposedIsoVertexSet[StartIndex + 1].Coordinate - OutImposedIsoVertexSet[StartIndex].Coordinate) * AQuarter;
		for (Index = StartIndex + 1; Index < OutImposedIsoVertexSet.Num() - 1; ++Index)
		{
			OutImposedIsoVertexSet[Index].IsoDeltaU = (OutImposedIsoVertexSet[Index + 1].Coordinate - OutImposedIsoVertexSet[Index - 1].Coordinate) * AEighth;
		}
		OutImposedIsoVertexSet[Index].IsoDeltaU = (OutImposedIsoVertexSet[Index].Coordinate - OutImposedIsoVertexSet[Index - 1].Coordinate) * AQuarter;
		break;
	}

	}
}

void FParametricMesher::Mesh(TSharedRef<FTopologicalVertex> InVertex)
{
	InVertex->GetOrCreateMesh(GetMeshModel());
}

//#define DEBUG_INTERSECTEDGEISOS
#ifdef DEBUG_INTERSECTEDGEISOS
void DebugIntersectEdgeIsos(const FTopologicalFace& Face, const TArray<double>& IsoCoordinates, EIso TypeIso);
#endif

void FParametricMesher::Mesh(FTopologicalEdge& InEdge, FTopologicalFace& Face)
{
	{
		FTopologicalEdge& ActiveEdge = *InEdge.GetLinkActiveEntity();
		if (ActiveEdge.IsMeshed())
		{
			return;
		}

		if (ActiveEdge.IsThinPeak())
		{
			TArray<FCuttingPoint>& FinalEdgeCuttingPointCoordinates = ActiveEdge.GetCuttingPoints();
			FinalEdgeCuttingPointCoordinates.Emplace(ActiveEdge.GetStartCurvilinearCoordinates(), ECoordinateType::VertexCoordinate);
			FinalEdgeCuttingPointCoordinates.Emplace(ActiveEdge.GetEndCurvilinearCoordinates(), ECoordinateType::VertexCoordinate);
			GenerateEdgeElements(ActiveEdge);
			return;
		}
	}

	const FSurfacicTolerance& ToleranceIso = Face.GetIsoTolerances();

	// Get Edge intersection with inner surface mesh grid
	TArray<double> EdgeIntersectionWithIsoU_Coordinates;
	TArray<double> EdgeIntersectionWithIsoV_Coordinates;

	const TArray<double>& SurfaceTabU = Face.GetCuttingCoordinatesAlongIso(EIso::IsoU);
	const TArray<double>& SurfaceTabV = Face.GetCuttingCoordinatesAlongIso(EIso::IsoV);

	ApplyEdgeCriteria(InEdge);

#ifdef DEBUG_MESH_EDGE
	{
		F3DDebugSession _(FString::Printf(TEXT("EdgePointsOnDomain %d"), ActiveEdge.GetId()));
		for (const FPoint& Point : EdgeCrossingPoints2D)
		{
			DisplayPoint(Point);
		}
		Wait();
	}
#endif

#ifdef DEBUG_INTERSECTEDGEISOS
	DebugIntersectEdgeIsos(Face, SurfaceTabU, EIso::IsoU);
	DebugIntersectEdgeIsos(Face, SurfaceTabV, EIso::IsoV);
	{
		F3DDebugSession _(FString::Printf(TEXT("Edge 2D %d"), InEdge.GetId()));
		CADKernel::Display2D(InEdge);
	}
#endif

	InEdge.ComputeIntersectionsWithIsos(SurfaceTabU, EIso::IsoU, ToleranceIso, EdgeIntersectionWithIsoU_Coordinates);
	InEdge.ComputeIntersectionsWithIsos(SurfaceTabV, EIso::IsoV, ToleranceIso, EdgeIntersectionWithIsoV_Coordinates);

#ifdef DEBUG_INTERSECTEDGEISOS
	{
		F3DDebugSession _(FString::Printf(TEXT("Edge %d Intersect with iso"), InEdge.GetId()));
		TArray<FPoint2D> Intersections;
		InEdge.Approximate2DPoints(EdgeIntersectionWithIsoU_Coordinates, Intersections);
		for (const FPoint2D& Point : Intersections)
		{
			DisplayPoint(Point);
		}
		Intersections.Empty();
		InEdge.Approximate2DPoints(EdgeIntersectionWithIsoV_Coordinates, Intersections);
		for (const FPoint2D& Point : Intersections)
		{
			DisplayPoint(Point);
		}
		Wait();
	}
#endif

	FLinearBoundary EdgeBounds = InEdge.GetBoundary();

	TArray<double>& DeltaUs = InEdge.GetDeltaUMaxs();

	// build a edge mesh compiling inner surface cutting (based on criteria applied on the surface) and edge cutting (based on criteria applied on the curve)
	TArray<FCuttingPoint> ImposedIsoCuttingPoints;
	{
		int32 NbImposedCuttingPoints = InEdge.GetImposedCuttingPoints().Num() + EdgeIntersectionWithIsoU_Coordinates.Num() + EdgeIntersectionWithIsoV_Coordinates.Num() + 2;
		ImposedIsoCuttingPoints.Reserve(NbImposedCuttingPoints);
	}

	FPoint2D ExtremityTolerances = InEdge.GetCurve()->GetExtremityTolerances(EdgeBounds);

	ImposedIsoCuttingPoints.Emplace(EdgeBounds.GetMin(), ECoordinateType::VertexCoordinate, -1, ExtremityTolerances[0]);
	ImposedIsoCuttingPoints.Emplace(EdgeBounds.GetMax(), ECoordinateType::VertexCoordinate, -1, ExtremityTolerances[1]);

	double MinDeltaU = HUGE_VALUE;
	for (const double& DeltaU : DeltaUs)
	{
		MinDeltaU = FMath::Min(MinDeltaU, DeltaU);
	}

#ifdef DEBUG_MESH_EDGE
	{
		F3DDebugSession _(FString::Printf(TEXT("Edge %d"), InEdge.GetId()));
		for (int32 Index = 0; Index < ActiveEdge.GetImposedCuttingPoints().Num(); ++Index)
		{
			const FImposedCuttingPoint& CuttingPoint = ActiveEdge.GetImposedCuttingPoints()[Index];
			ImposedIsoCuttingPoints.Emplace(CuttingPoint.Coordinate, ECoordinateType::ImposedCoordinate, CuttingPoint.OppositNodeIndex, MinDeltaU * AThird);
			FCurvePoint Point;
			ActiveEdge.EvaluatePoint(CuttingPoint.Coordinate, 0, Point);
			DisplayPoint(Point.Point);
		}
	}
#endif

	// Add Edge intersection with inner surface grid Iso
	double EdgeTolerance = FMath::Min(ExtremityTolerances[0], ExtremityTolerances[1]);
	if (!EdgeIntersectionWithIsoU_Coordinates.IsEmpty())
	{
		FillImposedIsoCuttingPoints(EdgeIntersectionWithIsoU_Coordinates, IsoUCoordinate, EdgeTolerance, InEdge, ImposedIsoCuttingPoints);
	}

	if (!EdgeIntersectionWithIsoV_Coordinates.IsEmpty())
	{
		FillImposedIsoCuttingPoints(EdgeIntersectionWithIsoV_Coordinates, IsoVCoordinate, EdgeTolerance, InEdge, ImposedIsoCuttingPoints);
	}

	ImposedIsoCuttingPoints.Sort([](const FCuttingPoint& Point1, const FCuttingPoint& Point2) { return Point1.Coordinate < Point2.Coordinate; });

	auto MergeImposedCuttingPoints = [&](int32& Index, int32& NewIndex, ECoordinateType NewType)
	{
		double DeltaU = FMath::Max(ImposedIsoCuttingPoints[NewIndex].IsoDeltaU, ImposedIsoCuttingPoints[Index].IsoDeltaU);
		if (ImposedIsoCuttingPoints[NewIndex].Type <= ImposedCoordinate && ImposedIsoCuttingPoints[Index].Type <= ImposedCoordinate)
		{
			DeltaU /= 5.;
		}

		if (ImposedIsoCuttingPoints[NewIndex].Coordinate + DeltaU > ImposedIsoCuttingPoints[Index].Coordinate)
		{
			if (ImposedIsoCuttingPoints[Index].Type == VertexCoordinate)
			{
				ImposedIsoCuttingPoints[NewIndex].Coordinate = ImposedIsoCuttingPoints[Index].Coordinate;
				ImposedIsoCuttingPoints[NewIndex].IsoDeltaU = ImposedIsoCuttingPoints[Index].IsoDeltaU;
			}
			else if (ImposedIsoCuttingPoints[NewIndex].Type == VertexCoordinate)
			{
			}
			else if (ImposedIsoCuttingPoints[NewIndex].Type == ImposedCoordinate)
			{
				if (ImposedIsoCuttingPoints[Index].Type == ImposedCoordinate)
				{
					ImposedIsoCuttingPoints[NewIndex].Coordinate = (ImposedIsoCuttingPoints[NewIndex].Coordinate + ImposedIsoCuttingPoints[Index].Coordinate) * 0.5;
				}
			}
			else if (ImposedIsoCuttingPoints[Index].Type == ImposedCoordinate)
			{
				ImposedIsoCuttingPoints[NewIndex].Coordinate = ImposedIsoCuttingPoints[Index].Coordinate;
				ImposedIsoCuttingPoints[NewIndex].Type = ImposedCoordinate;
				ImposedIsoCuttingPoints[NewIndex].IsoDeltaU = ImposedIsoCuttingPoints[Index].IsoDeltaU;
			}
			else if (ImposedIsoCuttingPoints[NewIndex].Type != ImposedIsoCuttingPoints[Index].Type)
			{
				ImposedIsoCuttingPoints[NewIndex].Coordinate = (ImposedIsoCuttingPoints[NewIndex].Coordinate + ImposedIsoCuttingPoints[Index].Coordinate) * 0.5;
				ImposedIsoCuttingPoints[NewIndex].Type = IsoUVCoordinate;
				ImposedIsoCuttingPoints[NewIndex].IsoDeltaU = FMath::Min(ImposedIsoCuttingPoints[NewIndex].IsoDeltaU, ImposedIsoCuttingPoints[Index].IsoDeltaU);
			}

			if (ImposedIsoCuttingPoints[NewIndex].Type <= ImposedCoordinate)
			{
				if (ImposedIsoCuttingPoints[NewIndex].OppositNodeIndex == -1)
				{
					ImposedIsoCuttingPoints[NewIndex].OppositNodeIndex = ImposedIsoCuttingPoints[Index].OppositNodeIndex;
				}
				else
				{
					ImposedIsoCuttingPoints[NewIndex].OppositNodeIndex2 = ImposedIsoCuttingPoints[Index].OppositNodeIndex;
				}
			}
		}
		else
		{
			++NewIndex;
			ImposedIsoCuttingPoints[NewIndex] = ImposedIsoCuttingPoints[Index];
		}
	};

	// If a pair of point isoU/isoV is too close, get the middle of the points
	if (ImposedIsoCuttingPoints.Num() > 1)
	{
		int32 NewIndex = 0;
		for (int32 Index = 1; Index < ImposedIsoCuttingPoints.Num(); ++Index)
		{
			if (ImposedIsoCuttingPoints[Index].Type > ECoordinateType::ImposedCoordinate)
			{
				bool bIsDelete = false;
				for (const FLinearBoundary& ThinZone : InEdge.GetThinZoneBounds())
				{
					if (ThinZone.Contains(ImposedIsoCuttingPoints[Index].Coordinate))
					{
						bIsDelete = true;
						continue;
					}
				}
				if (bIsDelete)
				{
					continue;
				}
			}

			if (ImposedIsoCuttingPoints[NewIndex].Type == ECoordinateType::ImposedCoordinate || ImposedIsoCuttingPoints[Index].Type == ECoordinateType::ImposedCoordinate)
			{
				MergeImposedCuttingPoints(Index, NewIndex, ECoordinateType::ImposedCoordinate);
			}
			else if (ImposedIsoCuttingPoints[NewIndex].Type != ImposedIsoCuttingPoints[Index].Type)
			{
				MergeImposedCuttingPoints(Index, NewIndex, ECoordinateType::IsoUVCoordinate);
			}
			else
			{
				++NewIndex;
				ImposedIsoCuttingPoints[NewIndex] = ImposedIsoCuttingPoints[Index];
			}
		}
		ImposedIsoCuttingPoints.SetNum(NewIndex + 1);
	}

	if (ImposedIsoCuttingPoints.Num() > 1 && (EdgeBounds.GetMax() - ImposedIsoCuttingPoints.Last().Coordinate) < FMath::Min(ImposedIsoCuttingPoints.Last().IsoDeltaU, InEdge.GetDeltaUMaxs().Last()))
	{
		ImposedIsoCuttingPoints.Last().Coordinate = EdgeBounds.GetMax();
		ImposedIsoCuttingPoints.Last().Type = VertexCoordinate;
	}
	else
	{
		ImposedIsoCuttingPoints.Emplace(EdgeBounds.GetMax(), ECoordinateType::VertexCoordinate, -1, InEdge.GetDeltaUMaxs().Last() * AQuarter);
	}

	// max vertex of the edge
	int32 MaxNumberOfVertex = (int32)((EdgeBounds.GetMax() - EdgeBounds.GetMin()) / MinDeltaU) + 5;

	// Final array of the edge mesh vertex 
	TArray<FCuttingPoint>& FinalEdgeCuttingPointCoordinates = InEdge.GetCuttingPoints();

	FinalEdgeCuttingPointCoordinates.Empty(ImposedIsoCuttingPoints.Num() + MaxNumberOfVertex);

#ifdef DEBUG_GETPREFERREDUVCOORDINATESFROMNEIGHBOURS
	TArray<double> CuttingPoints2;
	{
		double ToleranceGeoEdge = FMath::Min(ExtremityTolerances[0], ExtremityTolerances[1]);

		TArray<FCuttingPoint> Extremities;
		Extremities.Reserve(2);
		Extremities.Emplace(EdgeBounds.GetMin(), ECoordinateType::VertexCoordinate, -1, ToleranceGeoEdge);
		Extremities.Emplace(EdgeBounds.GetMax(), ECoordinateType::VertexCoordinate, -1, ToleranceGeoEdge);

		FMesherTools::ComputeFinalCuttingPointsWithImposedCuttingPoints(InEdge.GetCrossingPointUs(), InEdge.GetDeltaUMaxs(), Extremities, CuttingPoints2);
	}
#endif

	if (InEdge.IsDegenerated())
	{
		for (FCuttingPoint CuttingPoint : ImposedIsoCuttingPoints)
		{
			FinalEdgeCuttingPointCoordinates.Emplace(CuttingPoint.Coordinate, ECoordinateType::OtherCoordinate);
		}
	}
	else
	{
		TArray<double> CuttingPoints;
		FMesherTools::ComputeFinalCuttingPointsWithImposedCuttingPoints(InEdge.GetCrossingPointUs(), InEdge.GetDeltaUMaxs(), ImposedIsoCuttingPoints, CuttingPoints);
		for (const double& Coordinate : CuttingPoints)
		{
			FinalEdgeCuttingPointCoordinates.Emplace(Coordinate, ECoordinateType::OtherCoordinate);
		}

#ifdef DEBUG_GETPREFERREDUVCOORDINATESFROMNEIGHBOURS
		{
			F3DDebugSession G(TEXT("Mesh(TSharedRef<FEdge> InEdge"));
			{
				F3DDebugSession G(TEXT("U From Iso"));
				for (const FCuttingPoint& CuttingU : ImposedIsoCuttingPoints)
				{
					CADKernel::DisplayPoint(FPoint2D(CuttingU.Coordinate, 0.0));
				}
			}
			{
				F3DDebugSession G(TEXT("U From Criteria"));
				for (double CuttingU : CuttingPoints2)
				{
					CADKernel::DisplayPoint(FPoint2D(CuttingU, 0.05), EVisuProperty::NonManifoldEdge);
				}
			}
			{
				F3DDebugSession G(TEXT("U Final (Criteria & Iso)"));
				for (double CuttingU : CuttingPoints)
				{
					CADKernel::DisplayPoint(FPoint2D(CuttingU, 0.1), EVisuProperty::PurplePoint);
				}
			}
			//Wait();
		}
#endif

		GenerateEdgeElements(InEdge);
	}

}

//#define DEBUG_MESH_EDGE
void FParametricMesher::GenerateEdgeElements(FTopologicalEdge& Edge)
{
	FTopologicalEdge& ActiveEdge = *Edge.GetLinkActiveEntity();

	bool bSameDirection = Edge.IsSameDirection(ActiveEdge);

	TSharedRef<FEdgeMesh> EdgeMesh = ActiveEdge.GetOrCreateMesh(MeshModel);

	int32 StartVertexNodeIndex = ActiveEdge.GetStartVertex()->GetOrCreateMesh(GetMeshModel())->GetMesh();
	int32 EndVertexNodeIndex = ActiveEdge.GetEndVertex()->GetOrCreateMesh(GetMeshModel())->GetMesh();

	TArray<double> CuttingPointCoordinates;
	CuttingPointCoordinates.Reserve(Edge.GetCuttingPoints().Num());
	for (const FCuttingPoint& CuttingPoint : Edge.GetCuttingPoints())
	{
		CuttingPointCoordinates.Add(CuttingPoint.Coordinate);
	}
	ensureCADKernel(CuttingPointCoordinates.Num() > 1);
	CuttingPointCoordinates.RemoveAt(0);
	CuttingPointCoordinates.Pop();

	TArray<FPoint>& Coordinates = EdgeMesh->GetNodeCoordinates();
	Edge.ApproximatePoints(CuttingPointCoordinates, Coordinates);

	if (!bSameDirection)
	{
		Algo::Reverse(Coordinates);
	}

#ifdef DEBUG_MESH_EDGE
	{
		F3DDebugSession _(FString::Printf(TEXT("Edge Mesh %d"), Edge.GetId()));
		TArray<FPoint2D> Mesh2D;
		Edge.Approximate2DPoints(CuttingPointCoordinates, Mesh2D);
		for (const FPoint2D& Vertex : Mesh2D)
		{
			DisplayPoint(Vertex, EVisuProperty::RedPoint);
		}
		Wait();
	}
#endif

	EdgeMesh->RegisterCoordinates();
	EdgeMesh->Mesh(StartVertexNodeIndex, EndVertexNodeIndex);
	MeshModel->AddMesh(EdgeMesh);
	ActiveEdge.SetMeshed();
}

void FParametricMesher::IsolateQuadFace(TArray<FCostToFace>& QuadSurfaces, TArray<TSharedPtr<FTopologicalFace>>& OtherSurfaces) const
{
	TArray<TSharedPtr<FTopologicalFace>> FlatQuadsAndTriangles;
	FlatQuadsAndTriangles.Reserve(Faces.Num());
	QuadSurfaces.Reserve(Faces.Num() * 2);
	OtherSurfaces.Reserve(Faces.Num());

	for (const TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		if (Face->IsDeleted())
		{
			continue;
		}

		if (Face->IsMeshed())
		{
			continue;
		}

		Face->ComputeSurfaceSideProperties();
	}

	for (const TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		Face->DefineSurfaceType();
		switch (Face->GetQuadType())
		{
		case EQuadType::Quadrangular:
			double LocalMinCurvature;
			double LocalMaxCurvature;
			GetMinMax(Face->GetCurvature(EIso::IsoU).Max, Face->GetCurvature(EIso::IsoV).Max, LocalMinCurvature, LocalMaxCurvature);
			if (LocalMaxCurvature > ConstMinCurvature)
			{
				QuadSurfaces.Emplace(LocalMaxCurvature, Face.ToSharedRef());
				if (LocalMinCurvature > ConstMinCurvature)
				{
					QuadSurfaces.Emplace(LocalMinCurvature, Face.ToSharedRef());
				}
			}
			else
			{
				FlatQuadsAndTriangles.Add(Face);
				OtherSurfaces.Add(Face);
			}
			break;
		case EQuadType::Triangular:
			FlatQuadsAndTriangles.Add(Face);
			OtherSurfaces.Add(Face);
			break;
		case EQuadType::Unset:
		default:
			OtherSurfaces.Add(Face);
		}
	}

	Algo::Sort(QuadSurfaces, [](FCostToFace& SurfaceA, FCostToFace& SurfaceB)
		{
			return (SurfaceA.Cost > SurfaceB.Cost);
		}
	);

#ifdef DEBUG_ISOLATEQUADFACE
	if (QuadSurfaces.Num() > 0)
	{
		Open3DDebugSession(TEXT("Quad Entities"));
		for (const FCostToFace& Quad : QuadSurfaces)
		{
			//FGraphicSession _(FString::Printf(TEXT("Face %d %f"), Quad.Face->GetId(), Quad.Cost));
			Display(Quad.Face);
		}
		Close3DDebugSession();
	}

	if (FlatQuadsAndTriangles.Num() > 0)
	{
		Open3DDebugSession(TEXT("Flat Quads & Triangles"));
		for (const TSharedPtr<FTopologicalFace>& Face : FlatQuadsAndTriangles)
		{
			//FGraphicSession _(FString::Printf(TEXT("Face %d %f"), Face->GetId()));
			Display(Face);
		}
		Close3DDebugSession();
	}

	if (OtherSurfaces.Num() > 0)
	{
		Open3DDebugSession(TEXT("Other Entities"));
		for (const TSharedPtr<FTopologicalFace>& Face : OtherSurfaces)
		{
			//FGraphicSession _(FString::Printf(TEXT("Face %d %f"), Face->GetId()));
			Display(StaticCastSharedPtr<FTopologicalFace>(Face));
		}
		Close3DDebugSession();
	}
#endif
}

void FParametricMesher::LinkQuadSurfaceForMesh(TArray<FCostToFace>& QuadTrimmedSurfaceSet, TArray<TArray<TSharedPtr<FTopologicalFace>>>& OutStrips)
{
	const double GeometricTolerance = 20. * MeshModel->GetGeometricTolerance();

	OutStrips.Reserve(QuadTrimmedSurfaceSet.Num());

	for (FCostToFace& Quad : QuadTrimmedSurfaceSet)
	{
		TSharedPtr<FTopologicalFace> Surface = Quad.Face;
		const FSurfaceCurvature& Curvatures = Surface->GetCurvatures();

		EIso Axe = (!RealCompare(Quad.Cost, Curvatures[EIso::IsoU].Max)) ? EIso::IsoU : EIso::IsoV;

		if (Axe == EIso::IsoU)
		{
			if (Surface->HasMarker1())
			{
				continue;
			}
			Surface->SetMarker1();
		}
		else
		{
			if (Surface->HasMarker2())
			{
				continue;
			}
			Surface->SetMarker2();
		}

		TArray<TSharedPtr<FTopologicalFace>>* QuadStrip = &OutStrips.Emplace_GetRef();
		QuadStrip->Reserve(QuadTrimmedSurfaceSet.Num());
		QuadStrip->Add(Surface);

		const TArray<FEdge2DProperties>& SideProperties = Surface->GetSideProperties();

		int32 StartSideIndex = 0;
		for (; StartSideIndex < 4; StartSideIndex++)
		{
			if (SideProperties[StartSideIndex].IsoType == Axe)
			{
				break;
			}
		}
		if (StartSideIndex == 4)
		{
			continue;
		}

		bool bFirstStep = true;
		int32 SideIndex = StartSideIndex;

		while (Surface)
		{
			int32 EdgeIndex = Surface->GetStartEdgeIndexOfSide(SideIndex);
			double SideLength = Surface->GetSideProperties()[SideIndex].Length3D;
			TSharedPtr<FTopologicalEdge> Edge = Surface->GetLoops()[0]->GetEdges()[EdgeIndex].Entity;

			Surface.Reset();
			FTopologicalEdge* NextEdge = Edge->GetFirstTwinEdge();
			if (NextEdge)
			{
				Surface = (TSharedPtr<FTopologicalFace>)NextEdge->GetLoop()->GetFace();
				ensureCADKernel(Surface);
			}

			if (Surface && (Surface->GetQuadType() == EQuadType::Quadrangular || Surface->GetQuadType() == EQuadType::Triangular))
			{
				// check side length
				int32 LocalEdgeIndex = Surface->GetLoops()[0]->GetEdgeIndex(*NextEdge);
				SideIndex = Surface->GetSideIndex(LocalEdgeIndex);
				double OtherSideLength = Surface->GetSideProperties()[SideIndex].Length3D;

				GetMinMax(OtherSideLength, SideLength);
				if (SideLength - OtherSideLength > GeometricTolerance)
				{
					Surface = nullptr;
				}
			}
			else
			{
				Surface = nullptr;
			}

			if (Surface)
			{
				// Set as processed in a direction
				const TArray<FEdge2DProperties>& LocalSideProperties = Surface->GetSideProperties();
				if (LocalSideProperties[SideIndex].IsoType == EIso::IsoU)
				{
					if (Surface->HasMarker1())
					{
						Surface = nullptr;
					}
					else
					{
						Surface->SetMarker1();
					}
				}
				else
				{
					if (Surface->HasMarker2())
					{
						Surface = nullptr;
					}
					else
					{
						Surface->SetMarker2();
					}
				}
			}

			if (Surface)
			{
				// it's a quad or a tri => add
				if (Surface->GetQuadType() != EQuadType::Other)
				{
					QuadStrip->Add(Surface);
				}

				if (Surface->GetQuadType() == EQuadType::Triangular)
				{
					// stop
					Surface = nullptr;
				}
			}

			if (!Surface)
			{
				if (bFirstStep)
				{
					bFirstStep = false;
					Surface = (*QuadStrip)[0];
					SideIndex = (StartSideIndex + 2) % 4;
					continue;
				}
				else
				{
					break;
				}
			}

			// find opposite side
			SideIndex = (SideIndex + 2) % 4;
		}

		if (QuadStrip->Num() == 1)
		{
			OutStrips.Pop();
		}
	}

	for (const TSharedPtr<FTopologicalFace>& Face : Faces)
	{
		Face->ResetMarkers();
	}
}

void FParametricMesher::MeshSurfaceByFront(TArray<FCostToFace>& QuadTrimmedSurfaceSet)
{
	// Processed3 : Surfaces that have to be meshed are set Processed3
	// Processed1 : Surfaces added in CandidateSurfacesForMesh
	// Processed2 : Surfaces added in SecondChoiceOfCandidateSurfacesForMesh

	FMessage::Printf(EVerboseLevel::Debug, TEXT("Start MeshSurfaceByFront\n"));

	for (TSharedPtr<FTopologicalFace> Face : Faces)
	{
		Face->SetMarker3();
	}

	const double GeometricTolerance = 20. * MeshModel->GetGeometricTolerance();

	TArray<TSharedPtr<FTopologicalFace>> CandidateFacesForMesh; // first in first out
	CandidateFacesForMesh.Reserve(100);

	TArray<TSharedPtr<FTopologicalFace>> SecondChoiceOfCandidateFacesForMesh; // first in first out
	SecondChoiceOfCandidateFacesForMesh.Reserve(100);

	TFunction<void(TSharedRef<FTopologicalFace>)> MeshFace = [&](TSharedRef<FTopologicalFace> Face)
	{
#ifdef DISPLAYDEBUGMESHFACEBYFACESTEP
		Open3DDebugSession(TEXT("Surface " + Utils::ToFString(Face->GetId()));
		Display(Face);
		Close3DDebugSession();
#endif
		Mesh(Face);

#ifdef DISPLAYDEBUGMESHFACEBYFACESTEP
		Open3DDebugSession(TEXT("Mesh " + Utils::ToFString(Face->GetId()));
		DisplayMesh((FFaceMesh&)Face->GetOrCreateMesh(GetMeshModel()));
		Close3DDebugSession();
		//Wait();
#endif

		if (Face->HasMarker1())
		{
			CandidateFacesForMesh.RemoveSingle(Face);
		}
		if (Face->HasMarker2())
		{
			SecondChoiceOfCandidateFacesForMesh.RemoveSingle(Face);
		}

		const TSharedPtr<FTopologicalLoop> Loop = Face->GetLoops()[0];
		for (const FOrientedEdge& OrientedEdge : Loop->GetEdges())
		{
			const TSharedPtr<FTopologicalEdge>& Edge = OrientedEdge.Entity;
			Edge->SetMarker1(); // tmp for debug
			for (FTopologicalEdge* NextEdge : Edge->GetTwinsEntities())
			{
				if (NextEdge->HasMarker1())
				{
					continue;
				}

				TSharedPtr<FTopologicalFace> NextFace = NextEdge->GetFace();
				if (!NextFace.IsValid())
				{
					continue;
				}

				if (!NextFace->HasMarker3())
				{
					// not in the scope of surface to mesh
					continue;
				}

				int32 EdgeIndex;
				int32 LoopIndex;
				NextFace->GetEdgeIndex(*NextEdge, LoopIndex, EdgeIndex);
				if (LoopIndex > 0)
				{
					continue;
				}
				int32 SideIndex = NextFace->GetSideIndex(*NextEdge);
				if (SideIndex == -1)
				{
					continue;
				}

				FEdge2DProperties& SideProperty = NextFace->GetSideProperty(SideIndex);

				double EdgeLength = NextEdge->Length();
				SideProperty.MeshedLength += EdgeLength;
				NextFace->AddMeshedLength(EdgeLength);
				if ((SideProperty.Length3D - SideProperty.MeshedLength) < GeometricTolerance)
				{
					if (!SideProperty.bIsMesh)
					{
						SideProperty.bIsMesh = true;
						NextFace->MeshedSideNum()++;
					}

					if (!NextFace->HasMarker1())
					{
						NextFace->SetMarker1();
						CandidateFacesForMesh.Add(NextFace);
					}
				}
				else
				{
					if (!NextFace->HasMarker2())
					{
						NextFace->SetMarker2();
						SecondChoiceOfCandidateFacesForMesh.Add(NextFace);
					}
				}
			}
		}
	};

	TFunction<void(TSharedRef<FTopologicalFace>)> MeshFacesByFront = [&](TSharedRef<FTopologicalFace> Face)
	{
		if (Face->IsMeshed())
		{
			return;
		}

		MeshFace(Face);

		while (CandidateFacesForMesh.Num() || SecondChoiceOfCandidateFacesForMesh.Num())
		{
			// the candidate are sorted according to the number of meshed side 
			Algo::Sort(CandidateFacesForMesh, [](TSharedPtr<FTopologicalFace> Surface1, TSharedPtr<FTopologicalFace> Surface2) { return Surface1->MeshedSideNum() > Surface2->MeshedSideNum(); });

			int32 IndexOfBestCandidate = -1;
			double CandidateMeshedSideRatio = 0;

			// The first choice will be done in the first set of surface with the max meshed side numbers.
			if (CandidateFacesForMesh.Num())
			{
				int32 MaxMeshedSideNum = CandidateFacesForMesh[0]->MeshedSideNum();
				for (int32 Index = 0; Index < CandidateFacesForMesh.Num(); ++Index)
				{
					if (CandidateFacesForMesh[Index]->IsMeshed())
					{
						CandidateFacesForMesh.RemoveAt(Index);
						--Index;
					}
				}

				// next face with side well meshed are preferred
				int32 Index = 0;
				for (; Index < CandidateFacesForMesh.Num(); ++Index)
				{
					TSharedPtr<FTopologicalFace> CandidateSurface = CandidateFacesForMesh[Index];
					if (CandidateSurface->MeshedSideNum() < MaxMeshedSideNum)
					{
						break;
					}

					if (CandidateMeshedSideRatio < CandidateSurface->MeshedSideRatio())
					{
						CandidateMeshedSideRatio = CandidateSurface->MeshedSideRatio();
						IndexOfBestCandidate = Index;
					}
				}

				// if no candidate has been selected, the choice is done on all next surfaces
				if (IndexOfBestCandidate == -1)
				{
					for (; Index < CandidateFacesForMesh.Num(); ++Index)
					{
						TSharedPtr<FTopologicalFace> CandidateSurface = CandidateFacesForMesh[Index];
						if (CandidateMeshedSideRatio < CandidateSurface->MeshedSideRatio())
						{
							CandidateMeshedSideRatio = CandidateSurface->MeshedSideRatio();
							IndexOfBestCandidate = Index;
						}
					}
				}

				if (IndexOfBestCandidate >= 0)
				{
					ensureCADKernel(CandidateFacesForMesh[IndexOfBestCandidate].IsValid());
					MeshFace(CandidateFacesForMesh[IndexOfBestCandidate].ToSharedRef());
					continue;
				}
			}

			for (int32 Index = 0; Index < SecondChoiceOfCandidateFacesForMesh.Num(); ++Index)
			{
				TSharedPtr<FTopologicalFace> CandidateSurface = SecondChoiceOfCandidateFacesForMesh[Index];
				if (CandidateMeshedSideRatio < CandidateSurface->MeshedSideRatio())
				{
					CandidateMeshedSideRatio = CandidateSurface->MeshedSideRatio();
					IndexOfBestCandidate = Index;
				}
			}
			if (IndexOfBestCandidate >= 0)
			{
				ensureCADKernel(SecondChoiceOfCandidateFacesForMesh[IndexOfBestCandidate].IsValid());
				MeshFace(SecondChoiceOfCandidateFacesForMesh[IndexOfBestCandidate].ToSharedRef());
			}
		}
	};

	// the front is initialized with quad surface
	for (const FCostToFace& Quad : QuadTrimmedSurfaceSet)
	{
		TSharedPtr<FTopologicalFace> Surface = Quad.Face;
		MeshFacesByFront(Surface.ToSharedRef());
	}

	// the the other surface
	for (TSharedPtr<FTopologicalFace> Face : Faces)
	{
		if (!Face->IsMeshed())
		{
			MeshFacesByFront(Face.ToSharedRef());
		}
	}

}

// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
//
//
//                                                                            NOT YET REVIEWED
//
//
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================

void FParametricMesher::MeshThinZoneEdges(FGrid& Grid)
{
#ifdef DEBUG_MESHTHINSURF
	Open3DDebugSession(FString::Printf(TEXT("thin Surfaces cutting on surf %d"), Grid.GetFace()->GetId()));
#endif

	const TArray<FThinZone2D>& ThinZones = Grid.GetThinZones();

	FTimePoint MeshStartTime = FChrono::Now();

	for (const FThinZone2D& Zone : ThinZones)
	{
		bool bFirstSideIsPartiallyMeshed = Zone.GetFirstSide().IsPartiallyMeshed();
		bool bSecondSideIsPartiallyMeshed = Zone.GetSecondSide().IsPartiallyMeshed();

		if (bFirstSideIsPartiallyMeshed && bSecondSideIsPartiallyMeshed)
		{
			// the most meshed edge is meshed first
			double FirstSideMeshedLength = Zone.GetFirstSide().GetMeshedLength();
			double SecondSideMeshedLength = Zone.GetSecondSide().GetMeshedLength();
			if (FirstSideMeshedLength > SecondSideMeshedLength)
			{
				bSecondSideIsPartiallyMeshed = false;
			}
			else
			{
				bFirstSideIsPartiallyMeshed = false;
			}
		}

		if (!bFirstSideIsPartiallyMeshed && !bSecondSideIsPartiallyMeshed)
		{
			if (Zone.GetFirstSide().GetLength() > Zone.GetSecondSide().GetLength())
			{
				GetThinZoneBoundary(Zone.GetFirstSide());
				GetThinZoneBoundary(Zone.GetSecondSide());

				MeshThinZoneSide(Zone.GetFirstSide());
			}
			else
			{
				GetThinZoneBoundary(Zone.GetFirstSide());
				GetThinZoneBoundary(Zone.GetSecondSide());

				MeshThinZoneSide(Zone.GetSecondSide());
			}
		}
		else if (bFirstSideIsPartiallyMeshed && !bSecondSideIsPartiallyMeshed)
		{
			MeshThinZoneSide(Zone.GetFirstSide());
			GetThinZoneBoundary(Zone.GetSecondSide());
		}
		else if (!bFirstSideIsPartiallyMeshed && bSecondSideIsPartiallyMeshed)
		{
			MeshThinZoneSide(Zone.GetSecondSide());
			GetThinZoneBoundary(Zone.GetFirstSide());
		}
	}

	// if the extremity of the thin zone are connected by a short edges path, the edges path are not discretized to avoid a well discretized edge connecting to thin sides

#ifdef DEBUG_MESHTHINSURF
	Close3DDebugSession();
#endif

#ifdef DEBUG_MESHTHINSURF
	Open3DDebugSession(FString("Mesh of ThinZone 2D of surf " + Utils::ToFString(Grid.GetFace()->GetId())));
	for (const FThinZone2D& Zone : ThinZones)
	{
		TArray<TSharedPtr<FTopologicalEdge>> OutEdges;
		Zone.GetFirstSide().GetEdges(OutEdges);
		for (TSharedPtr<FTopologicalEdge> Edge : OutEdges)
		{
			TSharedPtr<FTopologicalEdge> ActiveEdge = StaticCastSharedRef<FTopologicalEdge>(Edge->GetLinkActiveEntity());

			TArray<double> ImposedCuttingPointEdgeCoordinates;
			GetCuttingPointCoordinates(ActiveEdge->GetImposedCuttingPoints(), ImposedCuttingPointEdgeCoordinates);

			TArray<double> ImposedCuttingPointU;
			Edge->TransformActiveEdgeCoordinatesToLocalCoordinates(ImposedCuttingPointEdgeCoordinates, ImposedCuttingPointU);

			TArray<FPoint> ImposedCuttingPoint2D;
			Edge->Approximate2DPoints(ImposedCuttingPointU, ImposedCuttingPoint2D);
			for (FPoint Point : ImposedCuttingPoint2D)
			{
				Display(Point);
			}
		}

		Zone.GetSecondSide().GetEdges(OutEdges);
		for (TSharedPtr<FTopologicalEdge> Edge : OutEdges)
		{
			TSharedPtr<FTopologicalEdge> ActiveEdge = StaticCastSharedRef<FTopologicalEdge>(Edge->GetLinkActiveEntity());

			TArray<double> ImposedCuttingPointEdgeCoordinates;
			GetCuttingPointCoordinates(ActiveEdge->GetImposedCuttingPoints(), ImposedCuttingPointEdgeCoordinates);

			TArray<double> ImposedCuttingPointCoordinates;
			Edge->TransformActiveEdgeCoordinatesToLocalCoordinates(ImposedCuttingPointEdgeCoordinates, ImposedCuttingPointCoordinates);

			TArray<FPoint> ImposedCuttingPoint2D;
			Edge->Approximate2DPoints(ImposedCuttingPointCoordinates, ImposedCuttingPoint2D);
			for (FPoint Point : ImposedCuttingPoint2D)
			{
				Display(Point);
			}
		}
	}
	Close3DDebugSession();
#endif
	Chronos.GlobalMeshThinZones += FChrono::Elapse(MeshStartTime);

}

static void AddActiveEdgeThinZone(TSharedPtr<FTopologicalEdge> Edge, TSharedPtr<FTopologicalEdge> ActiveEdge, FLinearBoundary& SideEdgeCoordinate)
{
	TArray<double> SideEdgeBound;
	SideEdgeBound.SetNum(2);
	SideEdgeBound[0] = SideEdgeCoordinate.GetMin();
	SideEdgeBound[1] = SideEdgeCoordinate.GetMax();

	TArray<double> ActiveEdgeThinZone;
	Edge->TransformActiveEdgeCoordinatesToLocalCoordinates(SideEdgeBound, ActiveEdgeThinZone);
	FLinearBoundary ThinZoneBoundary(ActiveEdgeThinZone[0], ActiveEdgeThinZone[1]);
	ActiveEdge->AddThinZone(ThinZoneBoundary);
};

void FParametricMesher::GetThinZoneBoundary(const FThinZoneSide& Side)
{
	TSharedPtr<FTopologicalEdge> Edge = nullptr;
	TSharedPtr<FTopologicalEdge> ActiveEdge = nullptr;
	FLinearBoundary SideEdgeCoordinate;

	for (const FEdgeSegment* EdgeSegment : Side.GetSegments())
	{
		double UMin = EdgeSegment->GetCoordinate(ELimit::Start);
		double UMax = EdgeSegment->GetCoordinate(ELimit::End);
		GetMinMax(UMin, UMax);

		if (Edge != EdgeSegment->GetEdge())
		{
			if (Edge)
			{
				AddActiveEdgeThinZone(Edge, ActiveEdge, SideEdgeCoordinate);
			}

			Edge = EdgeSegment->GetEdge();
			ActiveEdge = StaticCastSharedRef<FTopologicalEdge>(Edge->GetLinkActiveEntity());

			SideEdgeCoordinate.Set(UMin, UMax);
		}
		else
		{
			SideEdgeCoordinate.ExtendTo(UMin, UMax);
		}
	};
	AddActiveEdgeThinZone(Edge, ActiveEdge, SideEdgeCoordinate);
}

void FParametricMesher::MeshThinZoneSide(const FThinZoneSide& Side)
{
	typedef TFunction<bool(double, double)> CompareMethode;

	TSharedPtr<FTopologicalEdge> Edge;
	TSharedPtr<FTopologicalEdge> ActiveEdge;
	int32 Index = 0;
	int32 Increment = 1;
	TArray<double> EdgeCuttingPointCoordinates;
	FLinearBoundary SideEdgeCoordinate;
	const TArray<int32>* NodeIndices = nullptr;

	TFunction<void(int32&, int32&, const FEdgeSegment*, double, double)> AddImposedCuttingPoint = [&](int32& Index, int32& Increment, const FEdgeSegment* EdgeSegment, double UMin, double UMax)
	{
#ifdef DebugMeshThinSurf
		DisplaySegment(EdgeSegment->GetExtemity(ELimit::End), EdgeSegment->GetExtemity(ELimit::Start));
#endif
		for (; Index >= 0 && Index < EdgeCuttingPointCoordinates.Num(); Index += Increment)
		{
			if (EdgeCuttingPointCoordinates[Index] < UMin)
			{
				break;
			}
			if (EdgeCuttingPointCoordinates[Index] > UMax)
			{
				break;
			}
			FPoint CuttingPoint3D = EdgeSegment->ComputeEdgePoint(EdgeCuttingPointCoordinates[Index]);

			const FEdgeSegment* ClosedSegment = EdgeSegment->GetClosedSegment();
			if (ClosedSegment == nullptr)
			{
#ifdef CADKERNEL_DEV
				Wait();
#endif
				continue;
			}
			double OppositeCuttingPointSegmentU;
			FPoint OppositeCuttingPoint3D = ClosedSegment->ProjectPoint(CuttingPoint3D, OppositeCuttingPointSegmentU);

			double OppositeCuttingPointU = 0;
			TSharedPtr<FTopologicalEdge> OpositEdge = nullptr;
			if (OppositeCuttingPointSegmentU == 0 && ClosedSegment->GetPrevious()->GetClosedSegment())
			{
				OppositeCuttingPoint3D = ClosedSegment->GetPrevious()->ProjectPoint(CuttingPoint3D, OppositeCuttingPointSegmentU);
				OppositeCuttingPointU = ClosedSegment->GetPrevious()->ComputeEdgeCoordinate(OppositeCuttingPointSegmentU);
				OpositEdge = ClosedSegment->GetPrevious()->GetEdge();
			}
			else if (OppositeCuttingPointSegmentU == 1 && ClosedSegment->GetNext()->GetClosedSegment())
			{
				OppositeCuttingPoint3D = ClosedSegment->GetNext()->ProjectPoint(CuttingPoint3D, OppositeCuttingPointSegmentU);
				OppositeCuttingPointU = ClosedSegment->GetNext()->ComputeEdgeCoordinate(OppositeCuttingPointSegmentU);
				OpositEdge = ClosedSegment->GetNext()->GetEdge();
			}
			else
			{
				OpositEdge = ClosedSegment->GetEdge();
				OppositeCuttingPointU = ClosedSegment->ComputeEdgeCoordinate(OppositeCuttingPointSegmentU);
			}

			double OppositeActiveEdgeCuttingPointU = OpositEdge->TransformLocalCoordinateToActiveEdgeCoordinate(OppositeCuttingPointU);

#ifdef DEBUG_MESHTHINSURF
			DisplayPoint(CuttingPoint3D);
			DisplaySegment(ClosedSegment->GetExtemity(ELimit::End), ClosedSegment->GetExtemity(ELimit::Start));
			DisplayPoint(OppositeCuttingPoint3D);
			DisplaySegment(CuttingPoint3D, OppositeCuttingPoint3D);
#endif
			OpositEdge->GetLinkActiveEdge()->AddImposedCuttingPointU(OppositeActiveEdgeCuttingPointU, (*NodeIndices)[Index]);
		}
	};

	TFunction<void(TArray<double>&, double, int32&, CompareMethode)> FindFirstIndexForward = [](TArray<double>& EdgeCuttingPointU, double ULimit, int32& OutIndex, CompareMethode Compare)
	{
		for (; OutIndex < EdgeCuttingPointU.Num(); ++OutIndex)
		{
			if (Compare(ULimit, EdgeCuttingPointU[OutIndex]))
			{
				break;
			}
		}
	};

	TFunction<void(TArray<double>&, double, int32&, CompareMethode)> FindFirstIndexBackward = [](TArray<double>& EdgeCuttingPointU, double ULimit, int32& OutIndex, CompareMethode Compare)
	{
		for (; OutIndex >= 0; --OutIndex)
		{
			if (Compare(ULimit, EdgeCuttingPointU[OutIndex]))
			{
				break;
			}
		}
	};

	TFunction<void(const FEdgeSegment* EdgeSegment)> Process = [&](const FEdgeSegment* EdgeSegment)
	{
		double UMin = EdgeSegment->GetCoordinate(ELimit::Start);
		double UMax = EdgeSegment->GetCoordinate(ELimit::End);
		GetMinMax(UMin, UMax);

		if (Edge != EdgeSegment->GetEdge())
		{
			if (Edge)
			{
				AddActiveEdgeThinZone(Edge, ActiveEdge, SideEdgeCoordinate);
#ifdef DebugMeshThinSurf
				Close3DDebugSession();
#endif
			}

			Edge = EdgeSegment->GetEdge();
			ActiveEdge = StaticCastSharedRef<FTopologicalEdge>(Edge->GetLinkActiveEntity());

			SideEdgeCoordinate.Set(UMin, UMax);

			if (!ActiveEdge->IsMeshed())
			{
				Mesh(*Edge.ToSharedRef(), *Edge->GetFace());
#ifdef DebugMeshThinSurf
				Open3DDebugSession(TEXT("Mesh of Edge"));
				DisplayMesh((const TSharedPtr<FEdgeMesh>&) ActiveEdge->GetMesh());
				Close3DDebugSession();
#endif
			}
#ifdef DebugMeshThinSurf
			Open3DDebugSession(TEXT("Projection of mesh"));
#endif
			NodeIndices = &ActiveEdge->GetOrCreateMesh(MeshModel)->EdgeVerticesIndex;

			TArray<double> CuttingPointCoordinates;
			GetCuttingPointCoordinates(ActiveEdge->GetCuttingPoints(), CuttingPointCoordinates);

			Edge->TransformActiveEdgeCoordinatesToLocalCoordinates(CuttingPointCoordinates, EdgeCuttingPointCoordinates);

			if ((EdgeCuttingPointCoordinates[0] < EdgeCuttingPointCoordinates[1]) == (EdgeSegment->GetCoordinate(ELimit::Start) < EdgeSegment->GetCoordinate(ELimit::End)))
			{
				Index = 0;
				if (EdgeCuttingPointCoordinates[0] < EdgeCuttingPointCoordinates[1])
				{
					FindFirstIndexForward(EdgeCuttingPointCoordinates, UMin, Index, [](double Value1, double Value2) {return (Value1 < Value2); });
				}
				else
				{
					FindFirstIndexForward(EdgeCuttingPointCoordinates, UMax, Index, [](double Value1, double Value2) {return (Value1 > Value2); });
				}
				Increment = 1;
			}
			else
			{
				Index = (int32)EdgeCuttingPointCoordinates.Num() - 1;
				if (EdgeCuttingPointCoordinates[0] < EdgeCuttingPointCoordinates[1])
				{
					FindFirstIndexBackward(EdgeCuttingPointCoordinates, UMax, Index, [](double Value1, double Value2) {return (Value1 > Value2); });
				}
				else
				{
					FindFirstIndexBackward(EdgeCuttingPointCoordinates, UMax, Index, [](double Value1, double Value2) {return (Value1 < Value2); });
				}
				Increment = -1;
			}
		}
		else
		{
			SideEdgeCoordinate.ExtendTo(UMin, UMax);
		}

		AddImposedCuttingPoint(Index, Increment, EdgeSegment, UMin, UMax);
	};

#ifdef DebugMeshThinSurf
	Open3DDebugSession(TEXT("MeshThinZoneSide"));
#endif

	if (Side.IsFirstSide())
	{
		for (const FEdgeSegment* EdgeSegment : Side.GetSegments())
		{
			Process(EdgeSegment);
		}
	}
	else
	{
		const TArray<FEdgeSegment*>& Segments = Side.GetSegments();
		for (int32 SegmentIndex = Segments.Num() - 1; SegmentIndex >= 0; --SegmentIndex)
		{
			Process(Segments[SegmentIndex]);
		}
	}
	AddActiveEdgeThinZone(Edge, ActiveEdge, SideEdgeCoordinate);

#ifdef DebugMeshThinSurf
	Close3DDebugSession();
	Close3DDebugSession();
#endif

}


void FParametricMesher::InitParameters(const FString& ParametersString)
{
	Parameters->SetFromString(ParametersString);
}

#ifdef DEBUG_INTERSECTEDGEISOS
TMap<int32, int32> SurfaceDrawed;
bool bDisplayIsoCurve = true;

void DebugIntersectEdgeIsos(const FTopologicalFace& Face, const TArray<double>& IsoCoordinates, EIso TypeIso)
{
	if (SurfaceDrawed.Find(Face.GetId()) == nullptr)
	{
		SurfaceDrawed.Add(Face.GetId(), 0);
	}

	if (bDisplayIsoCurve && SurfaceDrawed[Face.GetId()] < 2)
	{
		SurfaceDrawed[Face.GetId()]++;

		FSurfacicBoundary Bounds = Face.GetBoundary();

		//{
		//	F3DDebugSession _(FString::Printf(TEXT("Iso %s 3D %d"), TypeIso == EIso::IsoU ? TEXT("U") : TEXT("V"), Face.GetId()));
		//	for (double U : IsoCoordinates)
		//	{
		//		DisplayIsoCurve(*Face.GetCarrierSurface(), U, TypeIso);
		//	}
		//}

		F3DDebugSession _(FString::Printf(TEXT("Iso %s 2D %d"), TypeIso == EIso::IsoU ? TEXT("U") : TEXT("V"), Face.GetId()));
		if (TypeIso == EIso::IsoU)
		{
			for (double U : IsoCoordinates)
			{
				FPoint2D Start(U, Bounds[EIso::IsoV].Min);
				FPoint2D End(U, Bounds[EIso::IsoV].Max);
				DisplaySegment(Start, End, 0, EVisuProperty::Iso);
			}
		}
		else
		{
			for (double V : IsoCoordinates)
			{
				FPoint2D Start(Bounds[EIso::IsoU].Min, V);
				FPoint2D End(Bounds[EIso::IsoU].Max, V);
				DisplaySegment(Start, End, 0, EVisuProperty::Iso);
			}
		}
	}
}

#endif

} // namespace