// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ImplicitObject.h"
#include "ChaosArchive.h"
#include "ChaosCheck.h"

namespace Chaos
{

template <typename T, int d = 3>
class TPlaneConcrete
{
public:

	// Scale the plane and assume that any of the scale components could be zero
	static TPlaneConcrete<T> MakeScaledSafe(const TPlaneConcrete<T>& Plane, const TVec3<T>& Scale)
	{
		const TVec3<T> ScaledX = Plane.MX * Scale;
		
		// If all 3 scale components are non-zero we can just inverse-scale the normal
		// If 1 scale component is zero, the normal will point in that direction of the zero scale
		// If 2 scale components are zero, the normal will be zero along the non-zero scale direction
		// If 3 scale components are zero, the normal will be unchanged
		const int32 ZeroX = FMath::IsNearlyZero(Scale.X) ? 1 : 0;
		const int32 ZeroY = FMath::IsNearlyZero(Scale.Y) ? 1 : 0;
		const int32 ZeroZ = FMath::IsNearlyZero(Scale.Z) ? 1 : 0;
		const int32 NumZeros = ZeroX + ZeroY + ZeroZ;
		TVec3<T> ScaledN;
		if (NumZeros == 0)
		{
			// All 3 scale components non-zero
			ScaledN = TVec3<T>(Plane.MNormal.X / Scale.X, Plane.MNormal.Y / Scale.Y, Plane.MNormal.Z / Scale.Z);
		}
		else if (NumZeros == 1)
		{
			// Exactly one Scale component is zero
			ScaledN = TVec3<T>(
				(ZeroX) ? 1.0f : 0.0f,
				(ZeroY) ? 1.0f : 0.0f,
				(ZeroZ) ? 1.0f : 0.0f);
		}
		else if (NumZeros == 2)
		{
			// Exactly two Scale components is zero
			ScaledN = TVec3<T>(
				(ZeroX) ? Plane.MNormal.X : 0.0f,
				(ZeroY) ? Plane.MNormal.Y : 0.0f,
				(ZeroZ) ? Plane.MNormal.Z : 0.0f);
		}
		else // (NumZeros == 3)
		{
			// All 3 scale components are zero
			ScaledN = Plane.MNormal;
		}

		// Even after all the above, we may still get a zero normal (e.g., we scale N=(1,0,0) by S=(0,1,0))
		const T ScaleN2 = ScaledN.SizeSquared();
		if (ScaleN2 > SMALL_NUMBER)
		{
			ScaledN = ScaledN * FMath::InvSqrt(ScaleN2);
		}
		else
		{
			ScaledN = Plane.MNormal;
		}
		
		return TPlaneConcrete<T>(ScaledX, ScaledN);
	}

	// Scale the plane and assume that none of the scale components are zero
	static TPlaneConcrete<T> MakeScaledUnsafe(const TPlaneConcrete<T>& Plane, const TVec3<T>& Scale)
	{
		const TVec3<T> ScaledX = Plane.MX * Scale;
		TVec3<T> ScaledN = Plane.MNormal / Scale;

		// We don't handle zero scales, but we could still end up with a small normal
		const T ScaleN2 = ScaledN.SizeSquared();
		if (ScaleN2 > SMALL_NUMBER)
		{
			ScaledN =  ScaledN * FMath::InvSqrt(ScaleN2);
		}
		else
		{
			ScaledN = Plane.MNormal;
		}

		return TPlaneConcrete<T>(ScaledX, ScaledN);
	}


	TPlaneConcrete() = default;
	TPlaneConcrete(const TVec3<T>& InX, const TVec3<T>& InNormal)
	    : MX(InX)
	    , MNormal(InNormal)
	{
		static_assert(d == 3, "Only dimension 3 is supported");
	}

	/**
	 * Phi is positive on the side of the normal, and negative otherwise.
	 */
	FReal SignedDistance(const FVec3& x) const
	{
		return FVec3::DotProduct(x - (FVec3)MX, (FVec3)MNormal);
	}

	/**
	 * Phi is positive on the side of the normal, and negative otherwise.
	 */
	FReal PhiWithNormal(const FVec3& x, FVec3& Normal) const
	{
		Normal = MNormal;
		return FVec3::DotProduct(x - (FVec3)MX, (FVec3)MNormal);
	}

	FVec3 FindClosestPoint(const FVec3& x, const FReal Thickness = (FReal)0) const
	{
		auto Dist = FVec3::DotProduct(x - (FVec3)MX, (FVec3)MNormal) - Thickness;
		return x - FVec3(Dist * MNormal);
	}

	bool Raycast(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, const FReal Thickness, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex) const
	{
		ensure(FMath::IsNearlyEqual(Dir.SizeSquared(), (FReal)1, (FReal)KINDA_SMALL_NUMBER));
		CHAOS_ENSURE(Length > 0);
		OutFaceIndex = INDEX_NONE;

		const FReal SignedDist = FVec3::DotProduct(StartPoint - (FVec3)MX, (FVec3)MNormal);
		if (FMath::Abs(SignedDist) < Thickness)
		{
			//initial overlap so stop
			//const FReal DirDotNormal = FVec3::DotProduct(Dir, (FVec3)MNormal);
			//OutPosition = StartPoint;
			//OutNormal = DirDotNormal < 0 ? MNormal : -MNormal;
			OutTime = 0;
			return true;
		}

		const FVec3 DirTowardsPlane = SignedDist < 0 ? MNormal : -MNormal;
		const FReal RayProjectedTowardsPlane = FVec3::DotProduct(Dir, DirTowardsPlane);
		const FReal Epsilon = 1e-7f;
		if (RayProjectedTowardsPlane < Epsilon)	//moving parallel or away
		{
			return false;
		}

		//No initial overlap so we are outside the thickness band of the plane. So translate the plane to account for thickness	
		const FVec3 TranslatedPlaneX = (FVec3)MX - Thickness * DirTowardsPlane;
		const FVec3 StartToTranslatedPlaneX = TranslatedPlaneX - StartPoint;
		const FReal LengthTowardsPlane = FVec3::DotProduct(StartToTranslatedPlaneX, DirTowardsPlane);
		const FReal LengthAlongRay = LengthTowardsPlane / RayProjectedTowardsPlane;
		
		if (LengthAlongRay > Length)
		{
			return false;	//never reach
		}

		OutTime = LengthAlongRay;
		OutPosition = StartPoint + (LengthAlongRay + Thickness) * Dir;
		OutNormal = -DirTowardsPlane;
		return true;
	}

	Pair<FVec3, bool> FindClosestIntersection(const FVec3& StartPoint, const FVec3& EndPoint, const FReal Thickness) const
 	{
		FVec3 Direction = EndPoint - StartPoint;
		FReal Length = Direction.Size();
		Direction = Direction.GetSafeNormal();
		FVec3 XPos = (FVec3)MX + (FVec3)MNormal * Thickness;
		FVec3 XNeg = (FVec3)MX - (FVec3)MNormal * Thickness;
		FVec3 EffectiveX = ((XNeg - StartPoint).Size() < (XPos - StartPoint).Size()) ? XNeg : XPos;
		FVec3 PlaneToStart = EffectiveX - StartPoint;
		FReal Denominator = FVec3::DotProduct(Direction, MNormal);
		if (Denominator == 0)
		{
			if (FVec3::DotProduct(PlaneToStart, MNormal) == 0)
			{
				return MakePair(EndPoint, true);
			}
			return MakePair(FVec3(0), false);
		}
		FReal Root = FVec3::DotProduct(PlaneToStart, MNormal) / Denominator;
		if (Root < 0 || Root > Length)
		{
			return MakePair(FVec3(0), false);
		}
		return MakePair(FVec3(Root * Direction + StartPoint), true);
	}

	const TVec3<T>& X() const { return MX; }
	const TVec3<T>& Normal() const { return MNormal; }
	const TVec3<T>& Normal(const TVec3<T>&) const { return MNormal; }

	FORCEINLINE void Serialize(FArchive& Ar)
	{
		Ar << MX << MNormal;
	}

	uint32 GetTypeHash() const
	{
		return HashCombine(UE::Math::GetTypeHash(MX), UE::Math::GetTypeHash(MNormal));
	}

  private:
	TVec3<T> MX;
	TVec3<T> MNormal;
};

template <typename T>
FArchive& operator<<(FArchive& Ar, TPlaneConcrete<T>& PlaneConcrete)
{
	PlaneConcrete.Serialize(Ar);
	return Ar;
}

template<class T, int d>
class TPlane final : public FImplicitObject
{
  public:
	using FImplicitObject::GetTypeName;


	TPlane() : FImplicitObject(0, ImplicitObjectType::Plane) {}	//needed for serialization
	TPlane(const TVector<T, d>& InX, const TVector<T, d>& InNormal)
	    : FImplicitObject(0, ImplicitObjectType::Plane)
		, MPlaneConcrete(InX, InNormal)
	{
	}
	TPlane(const TPlane<T, d>& Other)
	    : FImplicitObject(0, ImplicitObjectType::Plane)
	    , MPlaneConcrete(Other.MPlaneConcrete)
	{
	}
	TPlane(TPlane<T, d>&& Other)
	    : FImplicitObject(0, ImplicitObjectType::Plane)
	    , MPlaneConcrete(MoveTemp(Other.MPlaneConcrete))
	{
	}
	virtual ~TPlane() {}

	static constexpr EImplicitObjectType StaticType()
	{
		return ImplicitObjectType::Plane;
	}

	FReal GetRadius() const
	{
		return 0.0f;
	}

	/**
	 * Phi is positive on the side of the normal, and negative otherwise.
	 */
	T SignedDistance(const TVector<T, d>& x) const
	{
		return MPlaneConcrete.SignedDistance(x);
	}

	/**
	 * Phi is positive on the side of the normal, and negative otherwise.
	 */
	virtual FReal PhiWithNormal(const FVec3& x, FVec3& Normal) const override
	{
		return MPlaneConcrete.PhiWithNormal(x,Normal);
	}

	TVector<T, d> FindClosestPoint(const TVector<T, d>& x, const T Thickness = (T)0) const
	{
		return MPlaneConcrete.FindClosestPoint(x,Thickness);
	}

	virtual bool Raycast(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, const FReal Thickness, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex) const override
	{
		return MPlaneConcrete.Raycast(StartPoint,Dir,Length,Thickness,OutTime,OutPosition,OutNormal,OutFaceIndex);
	}

	virtual Pair<FVec3, bool> FindClosestIntersectionImp(const FVec3& StartPoint, const FVec3& EndPoint, const FReal Thickness) const override
 	{
		return MPlaneConcrete.FindClosestIntersection(StartPoint,EndPoint,Thickness);
	}

	const TVector<T,d>& X() const { return MPlaneConcrete.X(); }
	const TVector<T,d>& Normal() const { return MPlaneConcrete.Normal(); }
	const TVector<T, d>& Normal(const TVector<T, d>&) const { return MPlaneConcrete.Normal(); }
	
	FORCEINLINE void SerializeImp(FArchive& Ar)
	{
		FImplicitObject::SerializeImp(Ar);
		MPlaneConcrete.Serialize(Ar);
	}

	virtual void Serialize(FChaosArchive& Ar) override
	{
		FChaosArchiveScopedMemory ScopedMemory(Ar, GetTypeName());
		SerializeImp(Ar);
	}

	virtual void Serialize(FArchive& Ar) override
	{
		SerializeImp(Ar);
	}

	virtual uint32 GetTypeHash() const override
	{
		return MPlaneConcrete.GetTypeHash();
	}

	const TPlaneConcrete<T>& PlaneConcrete() const { return MPlaneConcrete; }

  private:
	  TPlaneConcrete<T> MPlaneConcrete;
};

template<typename T, int d>
TVector<T, 2> ComputeBarycentricInPlane(const TVector<T, d>& P0, const TVector<T, d>& P1, const TVector<T, d>& P2, const TVector<T, d>& P)
{
	TVector<T, 2> Bary;
	TVector<T, d> P10 = P1 - P0;
	TVector<T, d> P20 = P2 - P0;
	TVector<T, d> PP0 = P - P0;
	T Size10 = P10.SizeSquared();
	T Size20 = P20.SizeSquared();
	T ProjSides = TVector<T, d>::DotProduct(P10, P20);
	T ProjP1 = TVector<T, d>::DotProduct(PP0, P10);
	T ProjP2 = TVector<T, d>::DotProduct(PP0, P20);
	T Denom = Size10 * Size20 - ProjSides * ProjSides;
	using FVec2Real = decltype(Bary.X);
	Bary.X = FVec2Real((Size20 * ProjP1 - ProjSides * ProjP2) / Denom);
	Bary.Y = FVec2Real((Size10 * ProjP2 - ProjSides * ProjP1) / Denom);
	return Bary;
}

template<typename T, int d>
const TVector<T, d> FindClosestPointOnLineSegment(const TVector<T, d>& P0, const TVector<T, d>& P1, const TVector<T, d>& P)
{
	const TVector<T, d> P10 = P1 - P0;
	const TVector<T, d> PP0 = P - P0;
	const T Proj = TVector<T, d>::DotProduct(P10, PP0);
	if (Proj < (T)0) //first check we're not behind
	{
		return P0;
	}

	const T Denom2 = P10.SizeSquared();
	if (Denom2 < (T)1e-4)
	{
		return P0;
	}

	//do proper projection
	const T NormalProj = Proj / Denom2;
	if (NormalProj > (T)1) //too far forward
	{
		return P1;
	}

	return P0 + NormalProj * P10; //somewhere on the line
}


template<typename T, int d>
TVector<T, d> FindClosestPointOnTriangle(const TVector<T, d>& ClosestPointOnPlane, const TVector<T, d>& P0, const TVector<T, d>& P1, const TVector<T, d>& P2, const TVector<T, d>& P)
{
	const T Epsilon = 1e-4f;

	const TVector<T, 2> Bary = ComputeBarycentricInPlane(P0, P1, P2, ClosestPointOnPlane);

	if (Bary[0] >= -Epsilon && Bary[0] <= 1 + Epsilon && Bary[1] >= -Epsilon && Bary[1] <= 1 + Epsilon && (Bary[0] + Bary[1]) <= (1 + Epsilon))
	{
		return ClosestPointOnPlane;
	}

	const TVector<T, d> P10Closest = FindClosestPointOnLineSegment(P0, P1, P);
	const TVector<T, d> P20Closest = FindClosestPointOnLineSegment(P0, P2, P);
	const TVector<T, d> P21Closest = FindClosestPointOnLineSegment(P1, P2, P);

	const T P10Dist2 = (P - P10Closest).SizeSquared();
	const T P20Dist2 = (P - P20Closest).SizeSquared();
	const T P21Dist2 = (P - P21Closest).SizeSquared();

	if (P10Dist2 < P20Dist2)
	{
		if (P10Dist2 < P21Dist2)
		{
			return P10Closest;
		}
		else
		{
			return P21Closest;
		}
	}
	else
	{
		if (P20Dist2 < P21Dist2)
		{
			return P20Closest;
		}
		else
		{
			return P21Closest;
		}
	}
}

template<typename T, int d>
TVector<T, d> FindClosestPointOnTriangle(const TPlane<T, d>& TrianglePlane, const TVector<T, d>& P0, const TVector<T, d>& P1, const TVector<T, d>& P2, const TVector<T, d>& P)
{
	const TVector<T, d> PointOnPlane = TrianglePlane.FindClosestPoint(P);
	return FindClosestPointOnTriangle(PointOnPlane, P0, P1, P2, P);
}


template<typename T, int d>
bool IntersectPlanes2(TVector<T,d>& I, TVector<T,d>& D, const TPlane<T,d>& P1, const TPlane<T,d>& P2)
{
	FVector LI = I, LD = D;
	FPlane LP1(P1.X(), P1.Normal()), LP2(P2.X(), P2.Normal());
	bool RetVal = FMath::IntersectPlanes2(LI,LD,LP1,LP2);
	I = LI; D = LD;
	return RetVal;
}

}
