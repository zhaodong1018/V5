// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cluster.h"
#include "Bounds.h"
#include "MeshSimplify.h"
#include "GraphPartitioner.h"

template< typename T > FORCEINLINE uint32 Min3Index( const T A, const T B, const T C ) { return ( A < B ) ? ( ( A < C ) ? 0 : 2 ) : ( ( B < C ) ? 1 : 2 ); }
template< typename T > FORCEINLINE uint32 Max3Index( const T A, const T B, const T C ) { return ( A > B ) ? ( ( A > C ) ? 0 : 2 ) : ( ( B > C ) ? 1 : 2 ); }

namespace Nanite
{

void CorrectAttributes( float* Attributes )
{
	FVector3f& Normal = *reinterpret_cast< FVector3f* >( Attributes );
	Normal.Normalize();
}

void CorrectAttributesColor( float* Attributes )
{
	CorrectAttributes( Attributes );
	
	FLinearColor& Color = *reinterpret_cast< FLinearColor* >( Attributes + 3 );
	Color = Color.GetClamped();
}


FCluster::FCluster(
	const TArray< FStaticMeshBuildVertex >& InVerts,
	const TArrayView< const uint32 >& InIndexes,
	const TArrayView< const int32 >& InMaterialIndexes,
	const TBitArray<>& InBoundaryEdges,
	uint32 TriBegin, uint32 TriEnd, const TArray< uint32 >& TriIndexes, uint32 InNumTexCoords, bool bInHasColors )
{
	GUID = Murmur32( { TriBegin, TriEnd } );
	
	NumTris = TriEnd - TriBegin;
	//ensure(NumTriangles <= FCluster::ClusterSize);
	
	bHasColors = bInHasColors;
	NumTexCoords = InNumTexCoords;

	Verts.Reserve( NumTris * GetVertSize() );
	Indexes.Reserve( 3 * NumTris );
	BoundaryEdges.Reserve( 3 * NumTris );
	MaterialIndexes.Reserve( NumTris );

	check(InMaterialIndexes.Num() * 3 == InIndexes.Num());

	TMap< uint32, uint32 > OldToNewIndex;
	OldToNewIndex.Reserve( NumTris );

	for( uint32 i = TriBegin; i < TriEnd; i++ )
	{
		uint32 TriIndex = TriIndexes[i];

		for( uint32 k = 0; k < 3; k++ )
		{
			uint32 OldIndex = InIndexes[ TriIndex * 3 + k ];
			uint32* NewIndexPtr = OldToNewIndex.Find( OldIndex );
			uint32 NewIndex = NewIndexPtr ? *NewIndexPtr : ~0u;

			if( NewIndex == ~0u )
			{
				Verts.AddUninitialized( GetVertSize() );
				NewIndex = NumVerts++;
				OldToNewIndex.Add( OldIndex, NewIndex );
				
				const FStaticMeshBuildVertex& InVert = InVerts[ OldIndex ];

				GetPosition( NewIndex ) = InVert.Position;
				GetNormal( NewIndex ) = InVert.TangentZ.ContainsNaN() ? FVector3f::UpVector : InVert.TangentZ;
	
				if( bHasColors )
				{
					GetColor( NewIndex ) = InVert.Color.ReinterpretAsLinear();
				}

				FVector2f* UVs = GetUVs( NewIndex );
				for( uint32 UVIndex = 0; UVIndex < NumTexCoords; UVIndex++ )
				{
					UVs[ UVIndex ] = InVert.UVs[ UVIndex ].ContainsNaN() ? FVector2f::ZeroVector : InVert.UVs[ UVIndex ];
				}

				float* Attributes = GetAttributes( NewIndex );

				// Make sure this vertex is valid from the start
				if( bHasColors )
					CorrectAttributesColor( Attributes );
				else
					CorrectAttributes( Attributes );
			}

			Indexes.Add( NewIndex );
			BoundaryEdges.Add( InBoundaryEdges[ TriIndex * 3 + k ] );
		}

		MaterialIndexes.Add( InMaterialIndexes[ TriIndex ] );
	}

	FindExternalEdges();
	Bound();
}

// Split
FCluster::FCluster( FCluster& SrcCluster, uint32 TriBegin, uint32 TriEnd, const TArray< uint32 >& TriIndexes )
	: MipLevel( SrcCluster.MipLevel )
{
	GUID = Murmur32( { SrcCluster.GUID, TriBegin, TriEnd } );

	NumTexCoords = SrcCluster.NumTexCoords;
	bHasColors   = SrcCluster.bHasColors;
	
	NumTris = TriEnd - TriBegin;

	Verts.Reserve( NumTris * GetVertSize() );
	Indexes.Reserve( 3 * NumTris );
	BoundaryEdges.Reserve( 3 * NumTris );
	MaterialIndexes.Reserve( NumTris );

	TMap< uint32, uint32 > OldToNewIndex;
	OldToNewIndex.Reserve( NumTris );

	for( uint32 i = TriBegin; i < TriEnd; i++ )
	{
		uint32 TriIndex = TriIndexes[i];

		for( uint32 k = 0; k < 3; k++ )
		{
			uint32 OldIndex = SrcCluster.Indexes[ TriIndex * 3 + k ];
			uint32* NewIndexPtr = OldToNewIndex.Find( OldIndex );
			uint32 NewIndex = NewIndexPtr ? *NewIndexPtr : ~0u;

			if( NewIndex == ~0u )
			{
				Verts.AddUninitialized( GetVertSize() );
				NewIndex = NumVerts++;
				OldToNewIndex.Add( OldIndex, NewIndex );

				FMemory::Memcpy( &GetPosition( NewIndex ), &SrcCluster.GetPosition( OldIndex ), GetVertSize() * sizeof( float ) );
			}

			Indexes.Add( NewIndex );
			BoundaryEdges.Add( SrcCluster.BoundaryEdges[ TriIndex * 3 + k ] );
		}

		const int32 MaterialIndex = SrcCluster.MaterialIndexes[ TriIndex ];
		MaterialIndexes.Add( MaterialIndex );
	}

	FindExternalEdges();
	Bound();
}

// Merge
FCluster::FCluster( const TArray< const FCluster*, TInlineAllocator<16> >& MergeList )
{
	NumTexCoords = MergeList[0]->NumTexCoords;
	bHasColors = MergeList[0]->bHasColors;

	// Only need a guess
	const uint32 NumTriangles = ClusterSize * MergeList.Num();

	Verts.Reserve( NumTriangles * GetVertSize() );
	Indexes.Reserve( 3 * NumTriangles );
	BoundaryEdges.Reserve( 3 * NumTriangles );
	MaterialIndexes.Reserve( NumTriangles );

	FHashTable HashTable( 1 << FMath::FloorLog2( NumTriangles ), NumTriangles );

	for( const FCluster* Child : MergeList )
	{
		Bounds += Child->Bounds;

		// Can jump multiple levels but guarantee it steps at least 1.
		MipLevel = FMath::Max( MipLevel, Child->MipLevel + 1 );

		for( int32 i = 0; i < Child->Indexes.Num(); i++ )
		{
			const FVector3f& Position = Child->GetPosition( Child->Indexes[i] );

			uint32 Hash = HashPosition( Position );
			uint32 NewIndex;
			for( NewIndex = HashTable.First( Hash ); HashTable.IsValid( NewIndex ); NewIndex = HashTable.Next( NewIndex ) )
			{
				if( 0 == FMemory::Memcmp( &GetPosition( NewIndex ), &Position, GetVertSize() * sizeof( float ) ) )
				{
					break;
				}
			}
			if( !HashTable.IsValid( NewIndex ) )
			{
				Verts.AddUninitialized( GetVertSize() );
				NewIndex = NumVerts++;
				HashTable.Add( Hash, NewIndex );

				FMemory::Memcpy( &GetPosition( NewIndex ), &Position, GetVertSize() * sizeof( float ) );
			}

			Indexes.Add( NewIndex );
			BoundaryEdges.Add( Child->BoundaryEdges[i] );
		}

		for (int32 i = 0; i < Child->MaterialIndexes.Num(); i++)
		{
			const int32 MaterialIndex = Child->MaterialIndexes[i];
			MaterialIndexes.Add(MaterialIndex);
		}
	}

	NumTris = Indexes.Num() / 3;
}

float FCluster::Simplify( uint32 TargetNumTris, float TargetError, uint32 TargetErrorMaxNumTris )
{
	if( TargetNumTris >= NumTris )
	{
		return 0.0f;
	}

	float SurfaceArea = 0.0f;
	float UVArea[ MAX_STATIC_TEXCOORDS ] = { 0.0f };

	for( uint32 TriIndex = 0; TriIndex < NumTris; TriIndex++ )
	{
		uint32 Index0 = Indexes[ TriIndex * 3 + 0 ];
		uint32 Index1 = Indexes[ TriIndex * 3 + 1 ];
		uint32 Index2 = Indexes[ TriIndex * 3 + 2 ];

		const FVector3f& Position0 = GetPosition( Index0 );
		const FVector3f& Position1 = GetPosition( Index1 );
		const FVector3f& Position2 = GetPosition( Index2 );

		FVector3f Edge1 = Position1 - Position0;
		FVector3f Edge2 = Position2 - Position0;

		SurfaceArea += 0.5f * ( Edge1 ^ Edge2 ).Size();

		FVector2f* UV0 = GetUVs( Index0 );
		FVector2f* UV1 = GetUVs( Index1 );
		FVector2f* UV2 = GetUVs( Index2 );

		for( uint32 UVIndex = 0; UVIndex < NumTexCoords; UVIndex++ )
		{
			FVector2f EdgeUV1 = UV1[ UVIndex ] - UV0[ UVIndex ];
			FVector2f EdgeUV2 = UV2[ UVIndex ] - UV0[ UVIndex ];
			float SignedArea = 0.5f * ( EdgeUV1 ^ EdgeUV2 );
			UVArea[ UVIndex ] += FMath::Abs( SignedArea );

			// Force an attribute discontinuity for UV mirroring edges.
			// Quadric could account for this but requires much larger UV weights which raises error on meshes which have no visible issues otherwise.
			MaterialIndexes[ TriIndex ] |= ( SignedArea >= 0.0f ? 1 : 0 ) << ( UVIndex + 24 );
		}
	}

	float TriangleSize = FMath::Sqrt( SurfaceArea / NumTris );
	
	FFloat32 CurrentSize( FMath::Max( TriangleSize, THRESH_POINTS_ARE_SAME ) );
	FFloat32 DesiredSize( 0.25f );
	FFloat32 FloatScale( 1.0f );

	// Lossless scaling by only changing the float exponent.
	int32 Exponent = FMath::Clamp( (int)DesiredSize.Components.Exponent - (int)CurrentSize.Components.Exponent, -126, 127 );
	FloatScale.Components.Exponent = Exponent + 127;	//ExpBias
	// Scale ~= DesiredSize / CurrentSize
	float PositionScale = FloatScale.FloatValue;

	for( uint32 i = 0; i < NumVerts; i++ )
	{
		GetPosition(i) *= PositionScale;
	}

	uint32 NumAttributes = GetVertSize() - 3;
	float* AttributeWeights = (float*)FMemory_Alloca( NumAttributes * sizeof( float ) );

	// Normal
	AttributeWeights[0] = 1.0f;
	AttributeWeights[1] = 1.0f;
	AttributeWeights[2] = 1.0f;

	if( bHasColors )
	{
		float* ColorWeights = AttributeWeights + 3;
		ColorWeights[0] = 0.0625f;
		ColorWeights[1] = 0.0625f;
		ColorWeights[2] = 0.0625f;
		ColorWeights[3] = 0.0625f;
	}
	
	uint32 TexCoordOffset = 3 + ( bHasColors ? 4 : 0 );
	float* UVWeights = AttributeWeights + TexCoordOffset;

	// Normalize UVWeights
	for( uint32 UVIndex = 0; UVIndex < NumTexCoords; UVIndex++ )
	{
		float TriangleUVSize = FMath::Sqrt( UVArea[ UVIndex ] / NumTris );
		TriangleUVSize = FMath::Max( TriangleUVSize, THRESH_UVS_ARE_SAME );

		UVWeights[ 2 * UVIndex + 0 ] = 1.0f / ( 128.0f * TriangleUVSize );
		UVWeights[ 2 * UVIndex + 1 ] = 1.0f / ( 128.0f * TriangleUVSize );
	}

	FMeshSimplifier Simplifier( Verts.GetData(), NumVerts, Indexes.GetData(), Indexes.Num(), MaterialIndexes.GetData(), NumAttributes );

	Simplifier.SetBoundaryLocked( BoundaryEdges );
	
	Simplifier.SetAttributeWeights( AttributeWeights );
	Simplifier.SetCorrectAttributes( bHasColors ? CorrectAttributesColor : CorrectAttributes );
	Simplifier.SetEdgeWeight( 2.0f );

	float MaxErrorSqr = Simplifier.Simplify( NumVerts, TargetNumTris, TargetError, TargetErrorMaxNumTris );

	check( Simplifier.GetRemainingNumVerts() > 0 );
	check( Simplifier.GetRemainingNumTris() > 0 );
	
	Simplifier.GetBoundaryUnlocked( BoundaryEdges );
	Simplifier.Compact();
	
	Verts.SetNum( Simplifier.GetRemainingNumVerts() * GetVertSize() );
	Indexes.SetNum( Simplifier.GetRemainingNumTris() * 3 );
	MaterialIndexes.SetNum( Simplifier.GetRemainingNumTris() );

	NumVerts = Simplifier.GetRemainingNumVerts();
	NumTris = Simplifier.GetRemainingNumTris();

	float InvScale = 1.0f / PositionScale;
	for( uint32 i = 0; i < NumVerts; i++ )
	{
		GetPosition(i) *= InvScale;
		Bounds += GetPosition(i);
	}

	for( uint32 TriIndex = 0; TriIndex < NumTris; TriIndex++ )
	{
		// Remove UV mirroring bits
		MaterialIndexes[ TriIndex ] &= 0xffffff;
	}

	return FMath::Sqrt( MaxErrorSqr ) * InvScale;
}

void FCluster::Split( FGraphPartitioner& Partitioner ) const
{
	FDisjointSet DisjointSet( NumTris );
	
	TArray< int32 > SharedEdge;
	SharedEdge.AddUninitialized( Indexes.Num() );

	TMultiMap< uint32, int32 > EdgeHashTable;
	EdgeHashTable.Reserve( Indexes.Num() );

	for( int i = 0; i < Indexes.Num(); i++ )
	{
		uint32 TriI = i / 3;
		uint32 i0 = Indexes[ 3 * TriI + (i + 0) % 3 ];
		uint32 i1 = Indexes[ 3 * TriI + (i + 1) % 3 ];

		uint32 Hash0 = HashPosition( GetPosition( i0 ) );
		uint32 Hash1 = HashPosition( GetPosition( i1 ) );
		uint32 Hash = Murmur32( { FMath::Min( Hash0, Hash1 ), FMath::Max( Hash0, Hash1 ) } );

		bool bFound = false;
		for( auto Iter = EdgeHashTable.CreateKeyIterator( Hash ); Iter; ++Iter )
		{
			int32 j = Iter.Value();
			if( SharedEdge[j] == -1 )
			{
				uint32 TriJ = j / 3;
				uint32 j0 = Indexes[ 3 * TriJ + (j + 0) % 3 ];
				uint32 j1 = Indexes[ 3 * TriJ + (j + 1) % 3 ];

				if( GetPosition( i0 ) == GetPosition( j1 ) &&
					GetPosition( i1 ) == GetPosition( j0 ) )
				{
					// Link edges
					SharedEdge[i] = TriJ;
					SharedEdge[j] = TriI;
					DisjointSet.UnionSequential( TriI, TriJ );
					bFound = true;
					break;
				}
			}
		}
		if( !bFound )
		{
			EdgeHashTable.Add( Hash, i );
			SharedEdge[i] = -1;
		}
	}

	auto GetCenter = [ this ]( uint32 TriIndex )
	{
		FVector3f Center;
		Center  = GetPosition( Indexes[ TriIndex * 3 + 0 ] );
		Center += GetPosition( Indexes[ TriIndex * 3 + 1 ] );
		Center += GetPosition( Indexes[ TriIndex * 3 + 2 ] );
		return Center * (1.0f / 3.0f);
	};

	Partitioner.BuildLocalityLinks( DisjointSet, Bounds, GetCenter );

	auto* RESTRICT Graph = Partitioner.NewGraph( NumTris * 3 );

	for( uint32 i = 0; i < NumTris; i++ )
	{
		Graph->AdjacencyOffset[i] = Graph->Adjacency.Num();

		uint32 TriIndex = Partitioner.Indexes[i];

		// Add shared edges
		for( int k = 0; k < 3; k++ )
		{
			int32 AdjIndex = SharedEdge[ 3 * TriIndex + k ];
			if( AdjIndex != -1 )
			{
				Partitioner.AddAdjacency( Graph, AdjIndex, 4 * 65 );
			}
	}

		Partitioner.AddLocalityLinks( Graph, TriIndex, 1 );
	}
	Graph->AdjacencyOffset[ NumTris ] = Graph->Adjacency.Num();

	Partitioner.PartitionStrict( Graph, ClusterSize - 4, ClusterSize, false );
}

void FCluster::FindExternalEdges()
{
	ExternalEdges.Init( true, Indexes.Num() );
	NumExternalEdges = Indexes.Num();

	FHashTable HashTable( 1 << FMath::FloorLog2( Indexes.Num() ), Indexes.Num() );

	for( int32 EdgeIndex = 0; EdgeIndex < Indexes.Num(); EdgeIndex++ )
	{
		if( BoundaryEdges[ EdgeIndex ] )
		{
			ExternalEdges[ EdgeIndex ] = false;
			NumExternalEdges--;
			continue;
		}

		uint32 VertIndex0 = Indexes[ EdgeIndex ];
		uint32 VertIndex1 = Indexes[ Cycle3( EdgeIndex ) ];
	
		const FVector3f& Position0 = GetPosition( VertIndex0 );
		const FVector3f& Position1 = GetPosition( VertIndex1 );
	
		// Find edge with opposite direction that shares these 2 verts.
		/*
			  /\
			 /  \
			o-<<-o
			o->>-o
			 \  /
			  \/
		*/
		uint32 Hash0 = HashPosition( Position0 );
		uint32 Hash1 = HashPosition( Position1 );
		uint32 Hash = Murmur32( { Hash1, Hash0 } );

		uint32 OtherEdgeIndex;
		for( OtherEdgeIndex = HashTable.First( Hash ); HashTable.IsValid( OtherEdgeIndex ); OtherEdgeIndex = HashTable.Next( OtherEdgeIndex ) )
		{
			if( ExternalEdges[ OtherEdgeIndex ] )
			{
				uint32 OtherVertIndex0 = Indexes[ OtherEdgeIndex ];
				uint32 OtherVertIndex1 = Indexes[ Cycle3( OtherEdgeIndex ) ];
			
				if( Position0 == GetPosition( OtherVertIndex1 ) &&
					Position1 == GetPosition( OtherVertIndex0 ) )
				{
					// Found matching edge.
					ExternalEdges[ EdgeIndex ] = false;
					ExternalEdges[ OtherEdgeIndex ] = false;
					NumExternalEdges -= 2;
					break;
				}
			}
		}
		if( !HashTable.IsValid( OtherEdgeIndex ) )
		{
			HashTable.Add( Murmur32( { Hash0, Hash1 } ), EdgeIndex );
		}
	}
}




struct FNormalCone
{
	FVector3f	Axis;
	float	CosAngle;

	FNormalCone() {}
	FNormalCone( const FVector3f& InAxis )
		: Axis( InAxis )
		, CosAngle( 1.0f )
	{
		if( !Axis.Normalize() )
		{
			Axis = FVector3f( 0.0f, 0.0f, 1.0f );
		}
	}
};

FORCEINLINE FMatrix44f OrthonormalBasis( const FVector3f& Vec )
{
	float Sign = Vec.Z >= 0.0f ? 1.0f : -1.0f;
	float a = -1.0f / ( Sign + Vec.Z );
	float b = Vec.X * Vec.Y * a;
	
	return FMatrix44f(
		{ 1.0f + Sign * a * FMath::Square( Vec.X ), Sign * b, -Vec.X * Sign },
		{ b,     Sign + a * FMath::Square( Vec.Y ),           -Vec.Y },
		Vec,
		FVector3f::ZeroVector );
}

FMatrix44f CovarianceToBasis( const FMatrix44f& Covariance )
{
#if 0
	FMatrix44f Eigenvectors;
	FVector3f Eigenvalues;
	diagonalizeSymmetricMatrix( Covariance, Eigenvectors, Eigenvalues );

	//Eigenvectors = Eigenvectors.GetTransposed();

	uint32 i0 = Max3Index( Eigenvalues[0], Eigenvalues[1], Eigenvalues[2] );
	uint32 i1 = (1 << i0) & 3;
	uint32 i2 = (1 << i1) & 3;
	i1 = Eigenvalues[ i1 ] > Eigenvalues[ i2 ] ? i1 : i2;

	FVector3f Eigenvector0 = Eigenvectors.GetColumn( i0 );
	FVector3f Eigenvector1 = Eigenvectors.GetColumn( i1 );

	Eigenvector0.Normalize();
	Eigenvector1 -= ( Eigenvector0 | Eigenvector1 ) * Eigenvector1;
	Eigenvector1.Normalize();

	return FMatrix44f( Eigenvector0, Eigenvector1, Eigenvector0 ^ Eigenvector1, FVector3f::ZeroVector );
#else
	// Start with highest variance cardinal direction
	uint32 HighestVarianceDim = Max3Index( Covariance.M[0][0], Covariance.M[1][1], Covariance.M[2][2] );
	FVector3f Eigenvector0 = FMatrix44f::Identity.GetColumn( HighestVarianceDim );
	
	// Compute dominant eigenvector using power method
	for( int i = 0; i < 32; i++ )
	{
		Eigenvector0 = Covariance.TransformVector( Eigenvector0 );
		Eigenvector0.Normalize();
	}
	if( !Eigenvector0.IsNormalized() )
	{
		Eigenvector0 = FVector3f( 0.0f, 0.0f, 1.0f );
	}

	// Rotate matrix so that Z is Eigenvector0. This allows us to ignore Z dimension and turn this into a 2D problem.
	FMatrix44f ZSpace = OrthonormalBasis( Eigenvector0 );
	FMatrix44f ZLocalCovariance = Covariance * ZSpace;

	// Compute eigenvalues in XY plane. Solve for 2x2.
	float Det = ZLocalCovariance.M[0][0] * ZLocalCovariance.M[1][1] - ZLocalCovariance.M[0][1] * ZLocalCovariance.M[1][0];
	float Trace = ZLocalCovariance.M[0][0] + ZLocalCovariance.M[1][1];
	float Sqr = Trace * Trace - 4.0f * Det;
	if( Sqr < 0.0f )
	{
		return ZSpace;
	}
	float Sqrt = FMath::Sqrt( Sqr );
	
	float Eigenvalue1 = 0.5f * ( Trace + Sqrt );
	float Eigenvalue2 = 0.5f * ( Trace - Sqrt );

	float MinEigenvalue = FMath::Min( Eigenvalue1, Eigenvalue2 );
	float MaxEigenvalue = FMath::Max( Eigenvalue1, Eigenvalue2 );

	// Solve ( Eigenvalue * I - M ) * Eigenvector = 0
	FVector3f Eigenvector1;
	if( FMath::Abs( ZLocalCovariance.M[0][1] ) > FMath::Abs( ZLocalCovariance.M[1][0] ) )
	{
		Eigenvector1 = FVector3f( ZLocalCovariance.M[0][1], MaxEigenvalue - ZLocalCovariance.M[0][0], 0.0f );
	}
	else
	{
		Eigenvector1 = FVector3f( MaxEigenvalue - ZLocalCovariance.M[1][1], ZLocalCovariance.M[1][0], 0.0f );
	}

	Eigenvector1 = ZSpace.TransformVector( Eigenvector1 );
	//Eigenvector1 -= ( Eigenvector0 | Eigenvector1 ) * Eigenvector1;
	Eigenvector1.Normalize();

	return FMatrix44f( Eigenvector0, Eigenvector1, Eigenvector0 ^ Eigenvector1, FVector3f::ZeroVector );
#endif
}

void FCluster::Bound()
{
	Bounds = FBounds();
	
	TArray< FVector, TInlineAllocator<128> > Positions;	//TODO: convert me to FVector3f when FSphere also has a float version
	Positions.SetNum( NumVerts, false );

	for( uint32 i = 0; i < NumVerts; i++ )
	{
		Positions[i] = GetPosition(i);
		Bounds += Positions[i];
	}
	SphereBounds = FSphere( Positions.GetData(), Positions.Num() );
	LODBounds = SphereBounds;

	//auto& Normals = Positions;
	//Normals.Reset( Cluster.NumTris );

	float SurfaceArea = 0.0f;
	FVector3f SurfaceMean( 0.0f );
	
	float MaxEdgeLength2 = 0.0f;
	FVector3f AvgNormal = FVector3f::ZeroVector;
	for( int i = 0; i < Indexes.Num(); i += 3 )
	{
		FVector3f v[3];
		v[0] = GetPosition( Indexes[ i + 0 ] );
		v[1] = GetPosition( Indexes[ i + 1 ] );
		v[2] = GetPosition( Indexes[ i + 2 ] );

		FVector3f Edge01 = v[1] - v[0];
		FVector3f Edge12 = v[2] - v[1];
		FVector3f Edge20 = v[0] - v[2];

		MaxEdgeLength2 = FMath::Max( MaxEdgeLength2, Edge01.SizeSquared() );
		MaxEdgeLength2 = FMath::Max( MaxEdgeLength2, Edge12.SizeSquared() );
		MaxEdgeLength2 = FMath::Max( MaxEdgeLength2, Edge20.SizeSquared() );

#if 0
		// Calculate normals
		FVector3f Normal = Edge01 ^ Edge20;
		if( Normal.Normalize( 1e-12 ) )
		{
			Normals.Add( Normal );
			AvgNormal += Normal;
		}
#endif

		float TriArea = 0.5f * ( Edge01 ^ Edge20 ).Size();
		SurfaceArea += TriArea;
		for( int k = 0; k < 3; k++ )
			SurfaceMean += TriArea * v[k];
	}
	EdgeLength = FMath::Sqrt( MaxEdgeLength2 );
	SurfaceMean /= 3.0f * SurfaceArea;

#if 0
	// Minimal OBB using eigenvectors of covariance
	// https://www.geometrictools.com/Documentation/DynamicCollisionDetection.pdf
	float Covariance[6] = { 0 };
	for( int i = 0; i < Cluster.Indexes.Num(); i += 3 )
	{
		FVector3f v[3];
		v[0] = Cluster.Verts[ Cluster.Indexes[ i + 0 ] ].Position;
		v[1] = Cluster.Verts[ Cluster.Indexes[ i + 1 ] ].Position;
		v[2] = Cluster.Verts[ Cluster.Indexes[ i + 2 ] ].Position;

		float TriArea = 0.5f * ( (v[2] - v[0]) ^ (v[1] - v[0]) ).Size();

		for( int k = 0; k < 3; k++ )
		{
			FVector3f Diff = v[k] - SurfaceMean;
			Covariance[0] += TriArea * Diff[0] * Diff[0];
			Covariance[1] += TriArea * Diff[0] * Diff[1];
			Covariance[2] += TriArea * Diff[0] * Diff[2];
			Covariance[3] += TriArea * Diff[1] * Diff[1];
			Covariance[4] += TriArea * Diff[1] * Diff[2];
			Covariance[5] += TriArea * Diff[2] * Diff[2];
		}
	}
	for( int j = 0; j < 6; j++ )
		Covariance[j] /= 12.0f * SurfaceArea;

	FMatrix44f Axis = CovarianceToBasis( FMatrix44f(
		{ Covariance[0], Covariance[1], Covariance[2] },
		{ Covariance[1], Covariance[3], Covariance[4] },
		{ Covariance[2], Covariance[4], Covariance[5] },
		FVector3f::ZeroVector ) );

	FMatrix44f InvAxis = Axis.GetTransposed();

	FBounds Bounds;
	for( int i = 0; i < Cluster.NumVerts; i++ )
	{
		Bounds += InvAxis.TransformVector( Cluster.Verts[i].Position );
	}

	FVector3f Center = 0.5f * ( Bounds[1] + Bounds[0] );
	FVector3f Extent = 0.5f * ( Bounds[1] - Bounds[0] );

	// Cluster space is [-1,1] cube.
	FMatrix44f BoundsToLocal = FScaleMatrix( Extent ) * FTranslationMatrix( Center ) * Axis;
#endif

#if 0
	FSphere NormalBounds;
	if( Normals.Num() )
	{
		NormalBounds = FSphere( Normals.GetData(), Normals.Num() );
	}

	FNormalCone SphereCone( NormalBounds.Center );
	FNormalCone AvgCone( AvgNormal );
	for( FVector3f& Normal : Normals )
	{
		SphereCone.CosAngle	= FMath::Min( Normal | SphereCone.Axis, SphereCone.CosAngle );
		AvgCone.CosAngle	= FMath::Min( Normal | AvgCone.Axis, AvgCone.CosAngle );
	}

	FNormalCone NormalCone;
	if( SphereCone.CosAngle > AvgCone.CosAngle )
	{
		NormalCone.Axis		= SphereCone.Axis;
		NormalCone.CosAngle	= SphereCone.CosAngle;
	}
	else
	{
		NormalCone.Axis		= AvgCone.Axis;
		NormalCone.CosAngle	= AvgCone.CosAngle;
	}

	if( NormalCone.CosAngle > 0.0f )
	{
		// Cone of plane normals is different from cone bounding their half spaces.
		// The half space's cone angle is the complement of the normal cone's angle and the axis is flipped.
		float SinAngle = FMath::Sqrt( 1.0f - NormalCone.CosAngle * NormalCone.CosAngle );

		Cluster.ConeAxis = -NormalCone.Axis;
		Cluster.ConeCosAngle = SinAngle;
		Cluster.ConeStart = FVector2f( -MAX_FLT, MAX_FLT );
		
		// Push half space cone outside of every triangle's half space.
		for( int i = 0; i < Cluster.Indexes.Num(); i += 3 )
		{
			FVector3f v[3];
			v[0] = Cluster.Verts[ Cluster.Indexes[ i + 0 ] ].Position;
			v[1] = Cluster.Verts[ Cluster.Indexes[ i + 1 ] ].Position;
			v[2] = Cluster.Verts[ Cluster.Indexes[ i + 2 ] ].Position;

			FVector3f Normal = (v[2] - v[0]) ^ (v[1] - v[0]);
			if( Normal.Normalize( 1e-12 ) )
			{
				FPlane Plane( v[0] - Cluster.Bounds.Center, Normal );

				float CosAngle = Normal | NormalCone.Axis;
				float DistAlongAxis = Plane.W / CosAngle;

				Cluster.ConeStart.X = FMath::Max( Cluster.ConeStart.X, DistAlongAxis );
				Cluster.ConeStart.Y = FMath::Min( Cluster.ConeStart.Y, DistAlongAxis );
			}
		}
	}
	else
	{
		// No valid region to backface cull
		Cluster.ConeAxis = FVector3f( 0.0f, 0.0f, 1.0f );
		Cluster.ConeCosAngle = 2.0f;
		Cluster.ConeStart = FVector2f:ZeroVector;
	}
#endif
}

FArchive& operator<<(FArchive& Ar, FMaterialRange& Range)
{
	Ar << Range.RangeStart;
	Ar << Range.RangeLength;
	Ar << Range.MaterialIndex;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FStripDesc& Desc)
{
	for (uint32 i = 0; i < 4; i++)
	{
		for (uint32 j = 0; j < 3; j++)
		{
			Ar << Desc.Bitmasks[i][j];
		}
	}
	Ar << Desc.NumPrevRefVerticesBeforeDwords;
	Ar << Desc.NumPrevNewVerticesBeforeDwords;
	return Ar;
}
/*
FArchive& operator<<(FArchive& Ar, FCluster& Cluster)
{
	Ar << Cluster.NumVerts;
	Ar << Cluster.NumTris;
	Ar << Cluster.NumTexCoords;
	Ar << Cluster.bHasColors;

	Ar << Cluster.Verts;
	Ar << Cluster.Indexes;
	Ar << Cluster.MaterialIndexes;
	Ar << Cluster.BoundaryEdges;
	Ar << Cluster.ExternalEdges;
	Ar << Cluster.NumExternalEdges;

	Ar << Cluster.AdjacentClusters;

	Ar << Cluster.Bounds;
	Ar << Cluster.GUID;
	Ar << Cluster.MipLevel;

	Ar << Cluster.QuantizedPosStart;
	Ar << Cluster.QuantizedPosShift;

	Ar << Cluster.MeshBoundsMin;
	Ar << Cluster.MeshBoundsDelta;

	Ar << Cluster.EdgeLength;
	Ar << Cluster.LODError;

	Ar << Cluster.SphereBounds;
	Ar << Cluster.LODBounds;

	Ar << Cluster.GroupIndex;
	Ar << Cluster.GroupPartIndex;
	Ar << Cluster.GeneratingGroupIndex;

	Ar << Cluster.MaterialRanges;
	Ar << Cluster.QuantizedPositions;

	Ar << Cluster.StripDesc;
	Ar << Cluster.StripIndexData;
	return Ar;
}
*/
} // namespace Nanite