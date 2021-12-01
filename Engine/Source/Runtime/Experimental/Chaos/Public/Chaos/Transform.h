// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Real.h"
#include "Chaos/Matrix.h"
#include "Chaos/Rotation.h"
#include "Chaos/Vector.h"

#if !COMPILE_WITHOUT_UNREAL_SUPPORT
#include "Math/Transform.h"
#else
//TODO(mlentine): If we use this class in engine we need to make it more efficient.
//TODO(mlentine): This should really be a class as there is a lot of functionality but static anlysis current forbids this.
struct FTransform
{
public:
	FTransform() {}
	FTransform(const Chaos::TRotation<Chaos::FReal, 3>& Rotation, const Chaos::TVector<Chaos::FReal, 3>& Translation)
		: MRotation(Rotation), MTranslation(Translation)
	{
	}
	FTransform(const FMatrix& Matrix)
	{
		MTranslation[0] = Matrix.M[0][3];
		MTranslation[1] = Matrix.M[1][3];
		MTranslation[2] = Matrix.M[2][3];

		Chaos::FReal angle = sqrt(Matrix.M[0][0] * Matrix.M[0][0] + Matrix.M[1][0] * Matrix.M[1][0]);
		if (angle > 1e-6)
		{
			MRotation[0] = atan2(Matrix.M[2][1], Matrix.M[2][2]);
			MRotation[1] = atan2(-Matrix.M[2][0], angle);
			MRotation[2] = atan2(Matrix.M[1][0], Matrix.M[0][0]);
		}
		else
		{
			MRotation[0] = atan2(-Matrix.M[1][2], Matrix.M[1][1]);
			MRotation[1] = atan2(-Matrix.M[2][0], angle);
			MRotation[2] = 0;
		}
	}
	FTransform(const FTransform& Transform)
		: MRotation(Transform.MRotation), MTranslation(Transform.MTranslation)
	{
	}
	Chaos::TVector<Chaos::FReal, 3> InverseTransformPosition(const Chaos::TVector<Chaos::FReal, 3>& Position)
	{
		Chaos::TVector<Chaos::FReal, 4> Position4(Position[0], Position[1], Position[2], 1);
		Chaos::TVector<Chaos::FReal, 4> NewPosition = ToInverseMatrix() * Position4;
		return Chaos::TVector<Chaos::FReal, 3>(NewPosition[0], NewPosition[1], NewPosition[2]);
	}
	Chaos::TVector<Chaos::FReal, 3> TransformVector(const Chaos::TVector<Chaos::FReal, 3>& Vector)
	{
		Chaos::TVector<Chaos::FReal, 4> Vector4(Vector[0], Vector[1], Vector[2], 0);
		Chaos::TVector<Chaos::FReal, 4> NewVector = ToMatrix() * Vector4;
		return Chaos::TVector<Chaos::FReal, 3>(NewVector[0], NewVector[1], NewVector[2]);
	}
	Chaos::TVector<Chaos::FReal, 3> InverseTransformVector(const Chaos::TVector<Chaos::FReal, 3>& Vector)
	{
		Chaos::TVector<Chaos::FReal, 4> Vector4(Vector[0], Vector[1], Vector[2], 0);
		Chaos::TVector<Chaos::FReal, 4> NewVector = ToInverseMatrix() * Vector4;
		return Chaos::TVector<Chaos::FReal, 3>(NewVector[0], NewVector[1], NewVector[2]);
	}
	Chaos::PMatrix<Chaos::FReal, 3, 3> ToRotationMatrix()
	{
		return Chaos::PMatrix<Chaos::FReal, 3, 3>(
			cos(MRotation[0]), sin(MRotation[0]), 0,
			-sin(MRotation[0]), cos(MRotation[0]), 0,
			0, 0, 1) *
			Chaos::PMatrix<Chaos::FReal, 3, 3>(
				cos(MRotation[1]), 0, -sin(MRotation[1]),
				0, 1, 0,
				sin(MRotation[1]), 0, cos(MRotation[1])) *
			Chaos::PMatrix<Chaos::FReal, 3, 3>(
				1, 0, 0,
				0, cos(MRotation[2]), sin(MRotation[2]),
				0, -sin(MRotation[2]), cos(MRotation[2]));
	}
	Chaos::PMatrix<Chaos::FReal, 4, 4> ToMatrix()
	{
		auto RotationMatrix = ToRotationMatrix();
		return Chaos::PMatrix<Chaos::FReal, 4, 4>(
			RotationMatrix.M[0][0], RotationMatrix.M[1][0], RotationMatrix.M[2][0], 0,
			RotationMatrix.M[0][1], RotationMatrix.M[1][1], RotationMatrix.M[2][1], 0,
			RotationMatrix.M[0][2], RotationMatrix.M[1][2], RotationMatrix.M[2][2], 0,
			MTranslation[0], MTranslation[1], MTranslation[2], 1);
	}
	Chaos::PMatrix<Chaos::FReal, 4, 4> ToInverseMatrix()
	{
		auto RotationMatrix = ToRotationMatrix().GetTransposed();
		auto Vector = (RotationMatrix * MTranslation) * -1;
		return Chaos::PMatrix<Chaos::FReal, 4, 4>(
			RotationMatrix.M[0][0], RotationMatrix.M[1][0], RotationMatrix.M[2][0], 0,
			RotationMatrix.M[0][1], RotationMatrix.M[1][1], RotationMatrix.M[2][1], 0,
			RotationMatrix.M[0][2], RotationMatrix.M[1][2], RotationMatrix.M[2][2], 0,
			Vector[0], Vector[1], Vector[2], 1);
	}

private:
	Chaos::TRotation<Chaos::FReal, 3> MRotation;
	Chaos::TVector<Chaos::FReal, 3> MTranslation;
};
#endif

namespace Chaos
{
	template<class T, int d>
	class TRigidTransform
	{
	private:
		TRigidTransform() {}
		~TRigidTransform() {}
	};

	template<>
	class TRigidTransform<FReal, 2> : public UE::Math::TTransform<FReal>
	{
		using BaseTransform = UE::Math::TTransform<FReal>;
	public:
		TRigidTransform()
			: BaseTransform() {}
		TRigidTransform(const TVector<FReal, 3>& Translation, const TRotation<FReal, 3>& Rotation)
			: BaseTransform(Rotation, Translation) {}
		TRigidTransform(const FMatrix44d& Matrix)
			: BaseTransform(Matrix) {}
		TRigidTransform(const FMatrix44f& Matrix)
			: BaseTransform(Matrix) {}
		TRigidTransform(const BaseTransform& Transform)
			: BaseTransform(Transform) {}
		TRigidTransform<FReal, 2> Inverse() const
		{
			return BaseTransform::Inverse();
		}

		inline TRigidTransform<FReal, 2> operator*(const TRigidTransform<FReal, 2>& Other) const
		{
			return BaseTransform::operator*(Other);
		}
	};

	template<>
	class TRigidTransform<FReal, 3> : public UE::Math::TTransform<FReal>
	{
		using BaseTransform = UE::Math::TTransform<FReal>;
	public:
		TRigidTransform()
			: BaseTransform() {}
		TRigidTransform(const TVector<FReal, 3>& Translation, const TRotation<FReal, 3>& Rotation)
			: BaseTransform(Rotation, Translation) {}
		TRigidTransform(const TVector<FReal, 3>& Translation, const TRotation<FReal, 3>& Rotation, const TVector<FReal, 3>& Scale)
			: BaseTransform(Rotation, Translation, Scale) {}
		TRigidTransform(const FMatrix44d& Matrix)
			: BaseTransform(Matrix) {}
		TRigidTransform(const FMatrix44f& Matrix)
			: BaseTransform(Matrix) {}
		TRigidTransform(const BaseTransform& Transform)
			: BaseTransform(Transform) {}
		TRigidTransform<FReal, 3> Inverse() const
		{
			return BaseTransform::Inverse();
		}

		PMatrix<FReal, 4, 4> ToMatrixWithScale() const
		{
			return BaseTransform::ToMatrixWithScale();
		}

		PMatrix<FReal, 4, 4> ToMatrixNoScale() const
		{
			return BaseTransform::ToMatrixNoScale();
		}

		CHAOS_API Chaos::PMatrix<Chaos::FReal, 4, 4> operator*(const Chaos::PMatrix<Chaos::FReal, 4, 4>& Matrix) const;
		
		inline TRigidTransform<FReal, 3> operator*(const TRigidTransform<FReal, 3>& Other) const
		{
			return BaseTransform::operator*(Other);
		}

		// Get the transform which maps from Other to This, ignoring the scale on both.
		TRigidTransform<FReal, 3> GetRelativeTransformNoScale(const TRigidTransform<FReal, 3>& Other) const
		{
			// @todo(chaos): optimize
			TRotation<FReal, 3> OtherInverse = Other.GetRotation().Inverse();
			return TRigidTransform<FReal, 3>(
				(OtherInverse * (GetTranslation() - Other.GetTranslation())),
				OtherInverse * GetRotation());
		}

		TVector<FReal, 3> TransformNormalNoScale(const TVector<FReal, 3>& Normal) const
		{
			return TransformVectorNoScale(Normal);
		}

		// Transform the normal when scale may be non-unitary. Assumes no scale components are zero.
		TVector<FReal, 3> TransformNormalUnsafe(const TVector<FReal, 3>& Normal) const
		{
			const TVector<FReal, 3> RotatedNormal = TransformNormalNoScale(Normal);
			const TVector<FReal, 3> ScaledNormal = RotatedNormal / GetScale3D();
			const FReal ScaledNormal2 = ScaledNormal.SizeSquared();
			if (ScaledNormal2 > SMALL_NUMBER)
			{
				return ScaledNormal * FMath::InvSqrt(ScaledNormal2);
			}
			else
			{
				return RotatedNormal;
			}
		}
	};
}

inline uint32 GetTypeHash(const Chaos::TRigidTransform<Chaos::FReal, 3>& InTransform)
{
	return HashCombine(GetTypeHash(InTransform.GetTranslation()), HashCombine(GetTypeHash(InTransform.GetRotation().Euler()), GetTypeHash(InTransform.GetScale3D())));
}
