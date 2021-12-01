// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Core.h"
#include "Chaos/Collision/CollisionContext.h"
#include "Chaos/Collision/CollisionConstraintAllocator.h"
#include "Chaos/Collision/ParticlePairMidPhase.h"
#include "Chaos/ParticleHandle.h"

namespace Chaos
{
	class FCollisionContext;
	class FCollisionConstraintAllocator;

	/**
	 * Generate contact manifolds for particle pairs.
	 * @todo(chaos): Rename FNarrowPhase to FMidPhase
	 */
	class CHAOS_API FNarrowPhase
	{
	public:
		FNarrowPhase(const FReal InBoundsExpansion, const FReal InBoundsVelocityInflation, FCollisionConstraintAllocator& InCollisionAllocator)
			: CollisionAllocator(&InCollisionAllocator)
			, BoundsExpansion(InBoundsExpansion)
			, BoundsVelocityInflation(InBoundsVelocityInflation)
		{
		}

		FCollisionContext& GetContext() { return Context; }

		FReal GetBoundsExpansion() const { return BoundsExpansion; }
		void SetBoundsExpansion(const FReal InBoundsExpansion) { BoundsExpansion = InBoundsExpansion; }
		
		FReal GetBoundsVelocityInflation() const { return BoundsVelocityInflation; }
		void SetBoundsVelocityInflation(const FReal InBoundsVelocityInflation) { BoundsVelocityInflation = InBoundsVelocityInflation; }

		void GenerateCollisions(FReal Dt, TGeometryParticleHandle<FReal, 3>* Particle0, TGeometryParticleHandle<FReal, 3>* Particle1, const bool bForceDisableCCD)
		{
			FParticlePairMidPhase* MidPhase = CollisionAllocator->GetParticlePairMidPhase(Particle0, Particle1);
			if (MidPhase != nullptr)
			{
				Context.bForceDisableCCD = bForceDisableCCD;
				MidPhase->GenerateCollisions(GetBoundsExpansion(), Dt, GetContext());
			}
		}

	private:
		FCollisionContext Context;
		FCollisionConstraintAllocator* CollisionAllocator;
		FReal BoundsExpansion;
		FReal BoundsVelocityInflation;
	};
}
