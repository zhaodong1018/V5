// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Chaos/Core.h"
#include "Chaos/ParticleHandle.h"

namespace Chaos
{
	inline uint32 OrderIndependentHashCombine(const uint32 A, const uint32 B)
	{
		if (A < B)
		{
			return ::HashCombine(A, B);
		}
		else
		{
			return ::HashCombine(B, A);
		}
	}

	/**
	 * @brief Order particles in a consistent way for use by Broadphase and Resim
	*/
	inline bool ShouldSwapParticleOrder(const FGeometryParticleHandle* Particle0, const FGeometryParticleHandle* Particle1)
	{
		const bool bIsParticle1Preferred = (Particle1->ParticleID() < Particle0->ParticleID());
		const bool bSwapOrder = !FConstGenericParticleHandle(Particle0)->IsDynamic() || !bIsParticle1Preferred;
		return bSwapOrder;
	}

	/**
	 * @brief A key which uniquely identifes a particle pair for use by the collision detection system
	 * This key will be the same if particles order is reversed.
	*/
	class FCollisionParticlePairKey
	{
	public:
		using KeyType = uint64;

		FCollisionParticlePairKey()
		{
			Key.Key64 = 0;
		}

		FCollisionParticlePairKey(const FGeometryParticleHandle* Particle0, const FGeometryParticleHandle* Particle1)
		{
			GenerateKey(Particle0, Particle1);
		}

		uint64 GetKey() const
		{
			return Key.Key64;
		}

	private:
		void GenerateKey(const FGeometryParticleHandle* Particle0, const FGeometryParticleHandle* Particle1)
		{
			int32 ID0 = (Particle0->ParticleID().LocalID != INDEX_NONE) ? Particle0->ParticleID().LocalID : Particle0->ParticleID().GlobalID;
			int32 ID1 = (Particle1->ParticleID().LocalID != INDEX_NONE) ? Particle1->ParticleID().LocalID : Particle1->ParticleID().GlobalID;

			if (ID0 < ID1)
			{
				Key.Key32s[0] = ID0;
				Key.Key32s[1] = ID1;
			}
			else
			{
				Key.Key32s[0] = ID1;
				Key.Key32s[1] = ID0;
			}
		}

		union FIDKey
		{
			uint64 Key64;
			int32 Key32s[2];
		};

		FIDKey Key;
	};

	/**
	 * @brief A key which uniquely identifes a collision constraint within a particle pair
	 * 
	 * This key only needs to be uinque within the context of a particle pair. There is no
	 * guarantee of global uniqueness. This key is only used by the FMultiShapePairCollisionDetector
	 * class which is used for colliding shape pairs where each shape is actually a hierarchy
	 * of shapes. 
	 * 
	*/
	class FCollisionParticlePairConstraintKey
	{
	public:
		FCollisionParticlePairConstraintKey()
			: Key(0)
		{
		}

		FCollisionParticlePairConstraintKey(const FImplicitObject* Implicit0, const FBVHParticles* Simplicial0, const FImplicitObject* Implicit1, const FBVHParticles* Simplicial1)
			: Key(0)
		{
			check((Implicit0 != nullptr) || (Simplicial0 != nullptr));
			check((Implicit1 != nullptr) || (Simplicial1 != nullptr));
			GenerateHash(Implicit0, Simplicial0, Implicit1, Simplicial1);
		}

		uint32 GetKey() const
		{
			return Key;
		}

		friend bool operator==(const FCollisionParticlePairConstraintKey& L, const FCollisionParticlePairConstraintKey& R)
		{
			return L.Key == R.Key;
		}

		friend bool operator!=(const FCollisionParticlePairConstraintKey& L, const FCollisionParticlePairConstraintKey& R)
		{
			return !(L == R);
		}

		friend bool operator<(const FCollisionParticlePairConstraintKey& L, const FCollisionParticlePairConstraintKey& R)
		{
			return L.Key < R.Key;
		}

	private:
		void GenerateHash(const FImplicitObject* Implicit0, const FBVHParticles* Simplicial0, const FImplicitObject* Implicit1, const FBVHParticles* Simplicial1)
		{
			const uint32 Hash0 = (Implicit0 != nullptr) ? ::GetTypeHash(Implicit0) : ::GetTypeHash(Simplicial0);
			const uint32 Hash1 = (Implicit1 != nullptr) ? ::GetTypeHash(Implicit1) : ::GetTypeHash(Simplicial1);
			Key = OrderIndependentHashCombine(Hash0, Hash1);
		}

		uint32 Key;
	};
}