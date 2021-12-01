// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/UnrealString.h"
#include "Logging/LogMacros.h"
#include "Math/Vector.h"
#include "Math/Sphere.h"
#include "Math/Box.h"
#include "Misc/LargeWorldCoordinatesSerializer.h"

/**
 * Structure for a combined axis aligned bounding box and bounding sphere with the same origin. (28 bytes).
 */
namespace UE
{
namespace Math
{

template<typename T>
struct TBoxSphereBounds
{
	using FReal = T;

	/** Holds the origin of the bounding box and sphere. */
	TVector<T>	Origin;

	/** Holds the extent of the bounding box. */
	TVector<T> BoxExtent;

	/** Holds the radius of the bounding sphere. */
	T SphereRadius;

public:

	/** Default constructor. */
	TBoxSphereBounds() { }

	/**
	 * Creates and initializes a new instance.
	 *
	 * @param EForceInit Force Init Enum.
	 */
	explicit FORCEINLINE TBoxSphereBounds( EForceInit ) 
		: Origin(ForceInit)
		, BoxExtent(ForceInit)
		, SphereRadius(0)
	{
		DiagnosticCheckNaN();
	}

	/**
	 * Creates and initializes a new instance from the specified parameters.
	 *
	 * @param InOrigin origin of the bounding box and sphere.
	 * @param InBoxExtent half size of box.
	 * @param InSphereRadius radius of the sphere.
	 */
	TBoxSphereBounds( const TVector<T>& InOrigin, const TVector<T>& InBoxExtent, T InSphereRadius )
		: Origin(InOrigin)
		, BoxExtent(InBoxExtent)
		, SphereRadius(InSphereRadius)
	{
		DiagnosticCheckNaN();
	}

	/**
	 * Creates and initializes a new instance from the given Box and Sphere.
	 *
	 * @param Box The bounding box.
	 * @param Sphere The bounding sphere.
	 */
	TBoxSphereBounds( const TBox<T>& Box, const TSphere<T>& Sphere )
	{
		Box.GetCenterAndExtents(Origin,BoxExtent);
		SphereRadius = FMath::Min(BoxExtent.Size(), (Sphere.Center - Origin).Size() + Sphere.W);

		DiagnosticCheckNaN();
	}
	
	/**
	 * Creates and initializes a new instance the given Box.
	 *
	 * The sphere radius is taken from the extent of the box.
	 *
	 * @param Box The bounding box.
	 */
	TBoxSphereBounds( const TBox<T>& Box )
	{
		Box.GetCenterAndExtents(Origin,BoxExtent);
		SphereRadius = BoxExtent.Size();

		DiagnosticCheckNaN();
	}

	/**
	 * Creates and initializes a new instance for the given sphere.
	 */
	TBoxSphereBounds( const TSphere<T>& Sphere )
	{
		Origin = Sphere.Center;
		BoxExtent = TVector<T>(Sphere.W);
		SphereRadius = Sphere.W;

		DiagnosticCheckNaN();
	}

	/**
	 * Creates and initializes a new instance from the given set of points.
	 *
	 * The sphere radius is taken from the extent of the box.
	 *
	 * @param Points The points to be considered for the bounding box.
	 * @param NumPoints Number of points in the Points array.
	 */
	TBoxSphereBounds( const TVector<T>* Points, uint32 NumPoints );

	// Conversion to other type.
	template<typename FArg, TEMPLATE_REQUIRES(!TIsSame<T, FArg>::Value)>
	explicit TBoxSphereBounds(const TBoxSphereBounds<FArg>& From) : TBoxSphereBounds<T>(TVector<T>(From.Origin), TVector<T>(From.BoxExtent), (T)From.SphereRadius) {}

public:

	bool Serialize(FArchive &Ar);
	bool SerializeFromMismatchedTag(FName StructTag, FArchive &Ar);

	/**
	 * Constructs a bounding volume containing both this and B.
	 *
	 * @param Other The other bounding volume.
	 * @return The combined bounding volume.
	 */
	FORCEINLINE TBoxSphereBounds<T> operator+( const TBoxSphereBounds<T>& Other ) const;

	/**
	 * Compare bounding volume this and Other.
	 *
	 * @param Other The other bounding volume.
	 * @return true of they match.
	 */
	FORCEINLINE bool operator==(const TBoxSphereBounds<T>& Other) const;
	
	/**
	 * Compare bounding volume this and Other.
	 *
	 * @param Other The other bounding volume.
	 * @return true of they do not match.
	 */	
	FORCEINLINE bool operator!=(const TBoxSphereBounds<T>& Other) const;

public:

	/**
	 * Calculates the squared distance from a point to a bounding box
	 *
	 * @param Point The point.
	 * @return The distance.
	 */
	FORCEINLINE T ComputeSquaredDistanceFromBoxToPoint( const TVector<T>& Point ) const
	{
		TVector<T> Mins = Origin - BoxExtent;
		TVector<T> Maxs = Origin + BoxExtent;

		return ::ComputeSquaredDistanceFromBoxToPoint(Mins, Maxs, Point);
	}

	/**
	 * Test whether the spheres from two BoxSphereBounds intersect/overlap.
	 * 
	 * @param  A First BoxSphereBounds to test.
	 * @param  B Second BoxSphereBounds to test.
	 * @param  Tolerance Error tolerance added to test distance.
	 * @return true if spheres intersect, false otherwise.
	 */
	FORCEINLINE static bool SpheresIntersect(const TBoxSphereBounds<T>& A, const TBoxSphereBounds<T>& B, T Tolerance = KINDA_SMALL_NUMBER)
	{
		return (A.Origin - B.Origin).SizeSquared() <= FMath::Square(FMath::Max<T>(0.f, A.SphereRadius + B.SphereRadius + Tolerance));
	}

	/**
	 * Test whether the boxes from two BoxSphereBounds intersect/overlap.
	 * 
	 * @param  A First BoxSphereBounds to test.
	 * @param  B Second BoxSphereBounds to test.
	 * @return true if boxes intersect, false otherwise.
	 */
	FORCEINLINE static bool BoxesIntersect(const TBoxSphereBounds<T>& A, const TBoxSphereBounds<T>& B)
	{
		return A.GetBox().Intersect(B.GetBox());
	}

	/**
	 * Gets the bounding box.
	 *
	 * @return The bounding box.
	 */
	FORCEINLINE TBox<T> GetBox() const
	{
		return TBox<T>(Origin - BoxExtent,Origin + BoxExtent);
	}

	/**
	 * Gets the extrema for the bounding box.
	 *
	 * @param Extrema 1 for positive extrema from the origin, else negative
	 * @return The boxes extrema
	 */
	TVector<T> GetBoxExtrema( uint32 Extrema ) const
	{
		if (Extrema)
		{
			return Origin + BoxExtent;
		}

		return Origin - BoxExtent;
	}

	/**
	 * Gets the bounding sphere.
	 *
	 * @return The bounding sphere.
	 */
	FORCEINLINE TSphere<T> GetSphere() const
	{
		return TSphere<T>(Origin,SphereRadius);
	}

	/**
	 * Increase the size of the box and sphere by a given size.
	 *
	 * @param ExpandAmount The size to increase by.
	 * @return A new box with the expanded size.
	 */
	FORCEINLINE TBoxSphereBounds<T> ExpandBy( T ExpandAmount ) const
	{
		return TBoxSphereBounds(Origin, BoxExtent + ExpandAmount, SphereRadius + ExpandAmount);
	}

	/**
	 * Gets a bounding volume transformed by a matrix.
	 *
	 * @param M The matrix.
	 * @return The transformed volume.
	 */
	TBoxSphereBounds<T> TransformBy( const TMatrix<T>& M ) const;

	/**
	 * Gets a bounding volume transformed by a FTransform object.
	 *
	 * @param M The FTransform object.
	 * @return The transformed volume.
	 */
	TBoxSphereBounds<T> TransformBy( const TTransform<T>& M ) const;

	/**
	 * Get a textual representation of this bounding box.
	 *
	 * @return Text describing the bounding box.
	 */
	FString ToString() const;

	/**
	 * Constructs a bounding volume containing both A and B.
	 *
	 * This is a legacy version of the function used to compute primitive bounds, to avoid the need to rebuild lighting after the change.
	 */
	friend TBoxSphereBounds<T> Union( const TBoxSphereBounds<T>& A,const TBoxSphereBounds<T>& B )
	{
		return A + B;
	}

#if ENABLE_NAN_DIAGNOSTIC
	FORCEINLINE void DiagnosticCheckNaN() const
	{
		if (Origin.ContainsNaN())
		{
			logOrEnsureNanError(TEXT("Origin contains NaN: %s"), *Origin.ToString());
			const_cast<TBoxSphereBounds*>(this)->Origin = TVector<T>::ZeroVector;
		}
		if (BoxExtent.ContainsNaN())
		{
			logOrEnsureNanError(TEXT("BoxExtent contains NaN: %s"), *BoxExtent.ToString());
			const_cast<TBoxSphereBounds*>(this)->BoxExtent = TVector<T>::ZeroVector;
		}
		if (FMath::IsNaN(SphereRadius) || !FMath::IsFinite(SphereRadius))
		{
			logOrEnsureNanError(TEXT("SphereRadius contains NaN: %f"), SphereRadius);
			const_cast<TBoxSphereBounds*>(this)->SphereRadius = 0.f;
		}
	}
#else
	FORCEINLINE void DiagnosticCheckNaN() const {}
#endif

	inline bool ContainsNaN() const
	{
		return Origin.ContainsNaN() || BoxExtent.ContainsNaN() || !FMath::IsFinite(SphereRadius);
	}
};


/* TBoxSphereBounds<T> inline functions
 *****************************************************************************/

/**
 * Serializes the given bounding volume from or into the specified archive.
 *
 * @param Ar The archive to serialize from or into.
 * @param Bounds The bounding volume to serialize.
 * @return The archive..
 */
inline FArchive& operator<<(FArchive& Ar, TBoxSphereBounds<float>& Bounds)
{
	Ar << Bounds.Origin << Bounds.BoxExtent << Bounds.SphereRadius;
	return Ar;
}

/**
 * Serializes the given bounding volume from or into the specified archive.
 *
 * @param Ar The archive to serialize from or into.
 * @param Bounds The bounding volume to serialize.
 * @return The archive..
 */
inline FArchive& operator<<(FArchive& Ar, TBoxSphereBounds<double>& Bounds)
{
	Ar << Bounds.Origin << Bounds.BoxExtent;
	// LWC_TODO: Serializer
	//if(!Ar.IsPersistent())
	//{
	//	Ar << Bounds.SphereRadius;
	//}
	//else
	{
		float Radius = (float)Bounds.SphereRadius;
		Ar << Radius;
		if (Ar.IsLoading())
		{
			Bounds.SphereRadius = Radius;
		}
	}

	return Ar;
}

template<typename T>
FORCEINLINE TBoxSphereBounds<T>::TBoxSphereBounds( const TVector<T>* Points, uint32 NumPoints )
{
	TBox<T> BoundingBox(ForceInit);

	// find an axis aligned bounding box for the points.
	for (uint32 PointIndex = 0; PointIndex < NumPoints; PointIndex++)
	{
		BoundingBox += Points[PointIndex];
	}

	BoundingBox.GetCenterAndExtents(Origin, BoxExtent);

	// using the center of the bounding box as the origin of the sphere, find the radius of the bounding sphere.
	T SquaredSphereRadius = 0.0f;

	for (uint32 PointIndex = 0; PointIndex < NumPoints; PointIndex++)
	{
		SquaredSphereRadius = FMath::Max(SquaredSphereRadius, (Points[PointIndex] - Origin).SizeSquared());	// LWC_TODO: Precision loss
	}

	SphereRadius = FMath::Sqrt(SquaredSphereRadius);

	DiagnosticCheckNaN();
}

template<typename T>
FORCEINLINE TBoxSphereBounds<T> TBoxSphereBounds<T>::operator+( const TBoxSphereBounds<T>& Other ) const
{
	TBox<T> BoundingBox(ForceInit);

	BoundingBox += (this->Origin - this->BoxExtent);
	BoundingBox += (this->Origin + this->BoxExtent);
	BoundingBox += (Other.Origin - Other.BoxExtent);
	BoundingBox += (Other.Origin + Other.BoxExtent);

	// build a bounding sphere from the bounding box's origin and the radii of A and B.
	TBoxSphereBounds<T> Result(BoundingBox);

	Result.SphereRadius = FMath::Min(Result.SphereRadius, FMath::Max((Origin - Result.Origin).Size() + SphereRadius, (Other.Origin - Result.Origin).Size() + Other.SphereRadius));
	Result.DiagnosticCheckNaN();

	return Result;
}

template<typename T>
FORCEINLINE bool TBoxSphereBounds<T>::operator==(const TBoxSphereBounds<T>& Other) const
{
	return Origin == Other.Origin && BoxExtent == Other.BoxExtent &&  SphereRadius == Other.SphereRadius;
}

template<typename T>
FORCEINLINE bool TBoxSphereBounds<T>::operator!=(const TBoxSphereBounds<T>& Other) const
{
	return !(*this == Other);
}

template<typename T>
FORCEINLINE bool TBoxSphereBounds<T>::Serialize(FArchive &Ar)
{
	Ar << *this;
	return true;
}

template<typename T>
FORCEINLINE FString TBoxSphereBounds<T>::ToString() const
{
	return FString::Printf(TEXT("Origin=%s, BoxExtent=(%s), SphereRadius=(%f)"), *Origin.ToString(), *BoxExtent.ToString(), SphereRadius);
}

template<typename T>
TBoxSphereBounds<T> TBoxSphereBounds<T>::TransformBy(const TMatrix<T>& M) const
{
#if ENABLE_NAN_DIAGNOSTIC
	if (M.ContainsNaN())
	{
		logOrEnsureNanError(TEXT("Input Matrix contains NaN/Inf! %s"), *M.ToString());
		(const_cast<TMatrix<T>*>(&M))->SetIdentity();
	}
#endif

	TBoxSphereBounds<T> Result;

	const TVectorRegisterType<T> VecOrigin = VectorLoadFloat3(&Origin);
	const TVectorRegisterType<T> VecExtent = VectorLoadFloat3(&BoxExtent);

	const TVectorRegisterType<T> m0 = VectorLoadAligned(M.M[0]);
	const TVectorRegisterType<T> m1 = VectorLoadAligned(M.M[1]);
	const TVectorRegisterType<T> m2 = VectorLoadAligned(M.M[2]);
	const TVectorRegisterType<T> m3 = VectorLoadAligned(M.M[3]);

	TVectorRegisterType<T> NewOrigin = VectorMultiply(VectorReplicate(VecOrigin, 0), m0);
	NewOrigin = VectorMultiplyAdd(VectorReplicate(VecOrigin, 1), m1, NewOrigin);
	NewOrigin = VectorMultiplyAdd(VectorReplicate(VecOrigin, 2), m2, NewOrigin);
	NewOrigin = VectorAdd(NewOrigin, m3);

	TVectorRegisterType<T> NewExtent = VectorAbs(VectorMultiply(VectorReplicate(VecExtent, 0), m0));
	NewExtent = VectorAdd(NewExtent, VectorAbs(VectorMultiply(VectorReplicate(VecExtent, 1), m1)));
	NewExtent = VectorAdd(NewExtent, VectorAbs(VectorMultiply(VectorReplicate(VecExtent, 2), m2)));

	VectorStoreFloat3(NewExtent, &(Result.BoxExtent.X));
	VectorStoreFloat3(NewOrigin, &(Result.Origin.X));

	TVectorRegisterType<T> MaxRadius = VectorMultiply(m0, m0);
	MaxRadius = VectorMultiplyAdd(m1, m1, MaxRadius);
	MaxRadius = VectorMultiplyAdd(m2, m2, MaxRadius);
	MaxRadius = VectorMax(VectorMax(MaxRadius, VectorReplicate(MaxRadius, 1)), VectorReplicate(MaxRadius, 2));
	Result.SphereRadius = FMath::Sqrt(VectorGetComponent(MaxRadius, 0)) * SphereRadius;

	// For non-uniform scaling, computing sphere radius from a box results in a smaller sphere.
	T const BoxExtentMagnitude = FMath::Sqrt(VectorDot3Scalar(NewExtent, NewExtent));
	Result.SphereRadius = FMath::Min(Result.SphereRadius, BoxExtentMagnitude);

	Result.DiagnosticCheckNaN();
	return Result;
}

/**
 * Gets a bounding volume transformed by a FTransform object.
 *
 * @param M The FTransform object.
 * @return The transformed volume.
 */
 template<typename T>
TBoxSphereBounds<T> TBoxSphereBounds<T>::TransformBy(const TTransform<T>& M) const
{
#if ENABLE_NAN_DIAGNOSTIC
	M.DiagnosticCheckNaN_All();
#endif

	const TMatrix<T> Mat = M.ToMatrixWithScale();
	TBoxSphereBounds<T> Result = TransformBy(Mat);
	return Result;
}


} // namespace UE::Math
} // namespace UE

UE_DECLARE_LWC_TYPE(BoxSphereBounds, 3);

template <> struct TIsPODType<FBoxSphereBounds3f> { enum { Value = true }; };
template <> struct TIsPODType<FBoxSphereBounds3d> { enum { Value = true }; };
template <> struct TIsUECoreVariant<FBoxSphereBounds3f> { enum { Value = true }; };
template <> struct TIsUECoreVariant<FBoxSphereBounds3d> { enum { Value = true }; };

template<>
inline bool FBoxSphereBounds3f::SerializeFromMismatchedTag(FName StructTag, FArchive& Ar)
{
	return UE_SERIALIZE_VARIANT_FROM_MISMATCHED_TAG(Ar, BoxSphereBounds, BoxSphereBounds3f, BoxSphereBounds3d);
}

template<>
inline bool FBoxSphereBounds3d::SerializeFromMismatchedTag(FName StructTag, FArchive& Ar)
{
	return UE_SERIALIZE_VARIANT_FROM_MISMATCHED_TAG(Ar, BoxSphereBounds, BoxSphereBounds3d, BoxSphereBounds3f);

}