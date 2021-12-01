// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Chaos/CollisionResolutionTypes.h"
#include "Chaos/Collision/CollisionApplyType.h"
#include "Chaos/Evolution/SolverConstraintContainer.h"

namespace Chaos
{
	namespace Collisions
	{
		struct FContactParticleParameters;
		struct FContactIterationParameters;
	}

	class FPBDCollisionConstraint;
	class FPBDCollisionSolver;
	class FPBDCollisionSolverAdapter;
	class FSolverBodyContainer;

	/**
	 * @brief A container of low-level data used to solve collision constraints
	*/
	class FPBDCollisionSolverContainer : public FConstraintSolverContainer
	{
	public:
		FPBDCollisionSolverContainer();
		~FPBDCollisionSolverContainer();

		int32 NumSolvers() const { return CollisionSolvers.Num(); }

		void SetMaxPushOutVelocity(const FReal InMaxPushOutVelocity) { MaxPushOutVelocity = InMaxPushOutVelocity; }

		virtual void Reset(const int32 InMaxCollisions) override;

		// Add a solver constraint for the specified constraint and gather the required solver data
		void AddConstraintSolver(FReal Dt, FPBDCollisionConstraint& Constraint, const int32 Particle0Level, const int32 Particle1Level, FSolverBodyContainer& SolverBodyContainer);

		void SolveSwept(const FReal Dt);

		bool SolvePositionSerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);
		bool SolvePositionParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);

		bool SolveVelocitySerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);
		bool SolveVelocityParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);

		void ScatterOutput(const FReal Dt, const int32 BeginIndex, const int32 EndIndex);

	private:
		void UpdatePositionShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);
		void UpdateVelocityShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex);
		void SolveSwept(const FReal Dt, FPBDCollisionConstraint& Constraint, FPBDCollisionSolver& CollisionSolver);
		bool SolvePositionImpl(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const bool bParallel);
		bool SolveVelocityImpl(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const bool bParallel);
		void ScatterOutputImpl(const FReal Dt, const int32 BeginIndex, const int32 EndIndex, const bool bParallel);

		TArray<FPBDCollisionSolverAdapter> CollisionSolvers;
		TArray<int32> SweptCollisionSolvers;
		FReal MaxPushOutVelocity;
	};
}
