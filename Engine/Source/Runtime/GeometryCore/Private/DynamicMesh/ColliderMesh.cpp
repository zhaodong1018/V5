// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicMesh/ColliderMesh.h"
#include "DynamicMesh/DynamicMesh3.h"

using namespace UE::Geometry;


FColliderMesh::FColliderMesh(const FDynamicMesh3& SourceMesh, const FBuildOptions& BuildOptions)
{
	Initialize(SourceMesh, BuildOptions);
}

FColliderMesh::FColliderMesh()
{
	Reset(false);
}

void FColliderMesh::Reset(bool bReleaseMemory)
{
	Vertices.SetNum(0, bReleaseMemory);
	SourceVertexIDs.SetNum(0, bReleaseMemory);
	Triangles.SetNum(0, bReleaseMemory);
	SourceTriangleIDs.SetNum(0, bReleaseMemory);

	bSourceWasCompactV = bSourceWasCompactT = true;

	AABBTree.SetMesh(this, true);
}


void FColliderMesh::Initialize(const FDynamicMesh3& SourceMesh, const FBuildOptions& BuildOptions)
{
	bSourceWasCompactV = SourceMesh.IsCompactV();
	bSourceWasCompactT = SourceMesh.IsCompactT();

	int32 NumVertices = SourceMesh.VertexCount();
	Vertices.Reserve(NumVertices);
	if (BuildOptions.bBuildVertexMap && bSourceWasCompactV == false)
	{
		SourceVertexIDs.Reserve(NumVertices);
	}

	int32 NumTriangles = SourceMesh.TriangleCount();
	Triangles.Reserve(NumTriangles);
	if (BuildOptions.bBuildTriangleMap && bSourceWasCompactT == false)
	{
		SourceTriangleIDs.Reserve(NumTriangles);
	}

	TArray<int32> SourceToCompactMapV;

	if (bSourceWasCompactV)
	{
		for (FVector3d VertexPos : SourceMesh.VerticesItr())
		{
			Vertices.Add(VertexPos);
		}
	}
	else
	{
		SourceToCompactMapV.SetNum(SourceMesh.MaxVertexID());
		for (int32 VertexID : SourceMesh.VertexIndicesItr())
		{
			SourceToCompactMapV[VertexID] = Vertices.Num();
			Vertices.Add(SourceMesh.GetVertex(VertexID));
			if (BuildOptions.bBuildVertexMap)
			{
				SourceVertexIDs.Add(VertexID);
			}
		}
	}

	if (bSourceWasCompactT && bSourceWasCompactV)
	{
		for (FIndex3i TriIndices : SourceMesh.TrianglesItr())
		{
			Triangles.Add(TriIndices);
		}
	}
	else
	{
		for (int32 TriangleID : SourceMesh.TriangleIndicesItr())
		{
			FIndex3i Tri = SourceMesh.GetTriangle(TriangleID);
			if (!bSourceWasCompactV)
			{
				Tri.A = SourceToCompactMapV[Tri.A];
				Tri.B = SourceToCompactMapV[Tri.B];
				Tri.C = SourceToCompactMapV[Tri.C];
			}
			Triangles.Add(Tri);
			if (BuildOptions.bBuildTriangleMap)
			{
				SourceTriangleIDs.Add(TriangleID);
			}
		}
	}

	AABBTree.SetMesh(this, BuildOptions.bBuildAABBTree);

}


TMeshAABBTree3<FColliderMesh>& FColliderMesh::GetAABBTree()
{
	return AABBTree;
}


int32 FColliderMesh::GetSourceVertexID(int32 VertexID) const 
{ 
	if (bSourceWasCompactV)
	{
		return VertexID;
	}
	else
	{
		return (VertexID >= 0 && VertexID < SourceVertexIDs.Num()) ? SourceVertexIDs[VertexID] : IndexConstants::InvalidID;
	}
}

int32 FColliderMesh::GetSourceTriangleID(int32 TriangleID) const 
{ 
	if (bSourceWasCompactT)
	{
		return TriangleID;
	}
	else
	{
		return (TriangleID >= 0 && TriangleID < SourceTriangleIDs.Num()) ? SourceTriangleIDs[TriangleID] : IndexConstants::InvalidID;
	}
}
