// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CADKernel/Core/Chrono.h"
#include "CADKernel/Core/Types.h"
#include "CADKernel/Topo/TopologicalEdge.h"
#include "CADKernel/Topo/Shell.h"
#include "CADKernel/UI/Progress.h"

namespace CADKernel
{
	class FCriterion;
	class FGrid;
	class FThinZoneSide;
	class FTopologicalEntity;
	class FTopologicalLoop;

	struct FCostToFace
	{
		double Cost;
		TSharedRef<FTopologicalFace> Face;

		FCostToFace(double NewCost, TSharedRef<FTopologicalFace> NewFace)
			: Cost(NewCost)
			, Face(NewFace)
		{
		}

	};

	class CADKERNEL_API FMesherParameters : public FParameters
	{
	public:
		FParameter InconsistencyAngle;

		FMesherParameters();

		void SetInconsistencyAngle(const double Value)
		{
			InconsistencyAngle = Value;
		}
	};

	struct CADKERNEL_API FMesherChronos
	{
		FDuration GlobalDuration;
		FDuration ApplyCriteriaDuration;
		FDuration IsolateQuadPatchDuration;
		FDuration GlobalMeshDuration;
		FDuration GlobalPointCloudDuration;
		FDuration GlobalGeneratePointCloudDuration;
		FDuration GlobalTriangulateDuration;
		FDuration GlobalDelaunayDuration;
		FDuration GlobalMeshAndGetLoopNodes;
		FDuration GlobalMeshEdges;
		FDuration GlobalThinZones;

		FDuration GlobalFindThinZones;
		//FDuration GlobalScaleGrid;
		FDuration GlobalMeshThinZones;

		FMesherChronos()
			: GlobalDuration(FChrono::Init())
			, ApplyCriteriaDuration(FChrono::Init())
			, IsolateQuadPatchDuration(FChrono::Init())
			, GlobalMeshDuration(FChrono::Init())
			, GlobalPointCloudDuration(FChrono::Init())
			, GlobalGeneratePointCloudDuration(FChrono::Init())
			, GlobalTriangulateDuration(FChrono::Init())
			, GlobalDelaunayDuration(FChrono::Init())
			, GlobalMeshAndGetLoopNodes(FChrono::Init())
			, GlobalMeshEdges(FChrono::Init())
			, GlobalThinZones(FChrono::Init())
			, GlobalFindThinZones(FChrono::Init())
			//, GlobalScaleGrid(FChrono::Init())
			, GlobalMeshThinZones(FChrono::Init())
		{}

		void PrintTimeElapse() const
		{
			FMessage::Printf(Log, TEXT("\n\n\n"));
			FChrono::PrintClockElapse(Log, TEXT(""), TEXT("Total"), GlobalDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |  "), TEXT("Apply Criteria"), ApplyCriteriaDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |  "), TEXT("Find Quad Surfaces"), IsolateQuadPatchDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |  "), TEXT("Mesh Time"), GlobalMeshDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  "), TEXT("GeneratePoint Cloud "), GlobalGeneratePointCloudDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  |  "), TEXT("Point Cloud "), GlobalPointCloudDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  "), TEXT("ThinZones "), GlobalThinZones);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  "), TEXT("Mesh ThinZones "), GlobalMeshThinZones);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  "), TEXT("MeshEdges "), GlobalMeshEdges);
			FChrono::PrintClockElapse(Log, TEXT("  |   |  "), TEXT("TriangulateDuration "), GlobalTriangulateDuration);
			FChrono::PrintClockElapse(Log, TEXT("  |   |   |  "), TEXT("Delaunay Duration "), GlobalDelaunayDuration);
		}
	};

	class CADKERNEL_API FParametricMesher
	{
	protected:

		/**
		 * Limit of flatness of quad face
		 */
		const double ConstMinCurvature = 0.001;

		TSharedRef<FModelMesh> MeshModel;
		TSharedRef<FMesherParameters> Parameters;

		TArray<TSharedPtr<FTopologicalFace>> Faces;
		TArray<TSharedPtr<FTopologicalEdge>> Edges;
		TArray<TSharedPtr<FTopologicalVertex>> Vertices;

		FMesherChronos Chronos;

		bool bDisplay = false;

	public:

		FParametricMesher(TSharedRef<FModelMesh> MeshModel);

		const TSharedRef<FModelMesh>& GetMeshModel() const
		{
			return MeshModel;
		}

		TSharedRef<FModelMesh>& GetMeshModel()
		{
			return MeshModel;
		}

		void InitParameters(const FString& ParametersString);

		const TSharedRef<FMesherParameters>& GetParameters() const
		{
			return Parameters;
		}

		void MeshEntities(TArray<TSharedPtr<FEntity>>& InEntities);

		template<typename EntityType>
		void MeshEntity(TSharedRef<EntityType>& Entity)
		{
			MeshEntity((TSharedRef<FTopologicalEntity>&) Entity);
		}

		void MeshEntity(TSharedRef<FModel>& InModel)
		{
			MeshEntity((TSharedRef<FTopologicalEntity>&) InModel);
		}

		void MeshEntity(TSharedRef<FTopologicalEntity>& InEntity)
		{
			TArray<TSharedPtr<FEntity>> Entities;
			Entities.Add(InEntity);
			MeshEntities(Entities);
		}

		void Mesh(TSharedRef<FTopologicalFace> Face);
		void Mesh(FTopologicalEdge& InEdge, FTopologicalFace& CarrierFace);
		void Mesh(TSharedRef<FTopologicalVertex> Vertex);

		void MeshFaceLoops(FGrid& Grid);

		void MeshThinZoneEdges(FGrid&);
		void MeshThinZoneSide(const FThinZoneSide& Side);
		void GetThinZoneBoundary(const FThinZoneSide& Side);

		void GenerateCloud(FGrid& Grid);

	protected:

		void MeshEntities();


		void IsolateQuadFace(TArray<FCostToFace>& QuadSurfaces, TArray<TSharedPtr<FTopologicalFace>>& OtherSurfaces) const;

		void LinkQuadSurfaceForMesh(TArray<FCostToFace>& QuadTrimmedSurfaceSet, TArray<TArray<TSharedPtr<FTopologicalFace>>>& OutStrips);
		void MeshSurfaceByFront(TArray<FCostToFace>& QuadTrimmedSurfaceSet);

		void ApplyEdgeCriteria(FTopologicalEdge& Edge);
		void ApplyFaceCriteria(TSharedRef<FTopologicalFace> Face);

		/**
		 * Generate Edge Elements on active edge from Edge cutting points
		 */
		void GenerateEdgeElements(FTopologicalEdge& Edge);
	};

} // namespace CADKernel

