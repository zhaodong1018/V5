// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Chaos/Core.h"

#include "Chaos/CollisionResolutionTypes.h"
#include "Chaos/Collision/CollisionKeys.h"
#include "Chaos/Collision/CollisionVisitor.h"
#include "Chaos/ParticleHandleFwd.h"

#include "ProfilingDebugging/CsvProfiler.h"

namespace Chaos
{
	class FCollisionContext;
	class FCollisionConstraintAllocator;
	class FParticlePairMidPhase;
	class FPBDCollisionConstraint;
	class FPBDCollisionConstraints;
	class FPerShapeData;
	class FSingleShapePairCollisionDetector;

	/**
	 * @brief Handles collision detection for a pair of simple shapes (i.e., not compound shapes)
	 * 
	 * @note this is not used for collisions involving Unions that require a recursive collision test.
	 * @see FMultiShapePairCollisionDetector
	*/
	class CHAOS_API FSingleShapePairCollisionDetector
	{
	public:
		using FCollisionsArray = TArray<FPBDCollisionConstraint*, TInlineAllocator<1>>;

		FSingleShapePairCollisionDetector(
			FGeometryParticleHandle* InParticle0,
			const FPerShapeData* InShape0,
			FGeometryParticleHandle* InParticle1,
			const FPerShapeData* InShape1,
			const EContactShapesType InShapePairType, 
			FParticlePairMidPhase& MidPhase);
		FSingleShapePairCollisionDetector(FSingleShapePairCollisionDetector&& R);
		FSingleShapePairCollisionDetector(const FSingleShapePairCollisionDetector& R) = delete;
		FSingleShapePairCollisionDetector& operator=(const FSingleShapePairCollisionDetector& R) = delete;
		~FSingleShapePairCollisionDetector();

		const FPBDCollisionConstraint* GetConstraint() const{ return Constraint.Get(); }
		FPBDCollisionConstraint* GetConstraint() { return Constraint.Get(); }
		const FGeometryParticleHandle* GetParticle0() const { return Particle0; }
		FGeometryParticleHandle* GetParticle0() { return Particle0; }
		const FGeometryParticleHandle* GetParticle1() const { return Particle1; }
		FGeometryParticleHandle* GetParticle1() { return Particle1; }
		const FPerShapeData* GetShape0() const { return Shape0; }
		const FPerShapeData* GetShape1() const { return Shape1; }

		/**
		 * @brief Have we run collision detection since this Epoch (inclusive)
		*/
		bool IsUsedSince(const int32 CurrentEpoch) const;

		/**
		 * @brief Perform a bounds check and run the narrow phase if necessary
		 * @return The number of collisions constraints that were activated
		*/
		int32 GenerateCollision(
			const FReal CullDistance,
			const bool bUseCCD,
			const FReal Dt);

		/**
		 * @brief Reactivate the collision exactly as it was last frame
		 * @return The number of collisions constraints that were restored
		*/
		int32 RestoreCollision(const FReal CullDistance);

		/**
		 * @brief Reactivate the constraint (essentially the same as Restore but slightly optimized)
		 * @parame SleepEpoch The tick on which the particle went to sleep.
		 * Only constraints that were active when the particle went to sleep should be reactivated.
		*/
		void WakeCollision(const int32 SleepEpoch);

		/**
		 * @brief Set the collision from the parameter and activate it
		 * This is used by the Resim restore functionality
		*/
		void SetCollision(const FPBDCollisionConstraint& Constraint);

	private:
		int32 GenerateCollisionImpl(const FReal CullDistance, const bool bUseCCD, const FReal Dt);

		void CreateConstraint(const FReal CullDistance);

		/**
		 * @brief Add the constraint to the scene's active list
		*/
		bool ActivateConstraint();

		FParticlePairMidPhase& MidPhase;
		TUniquePtr<FPBDCollisionConstraint> Constraint;
		FGeometryParticleHandle* Particle0;
		FGeometryParticleHandle* Particle1;
		const FPerShapeData* Shape0;
		const FPerShapeData* Shape1;
		EContactShapesType ShapePairType;
		bool bEnableOBBCheck0;
		bool bEnableOBBCheck1;
		bool bEnableManifoldCheck;
	};


	/**
	 * @brief A collision detector for shape pairs which are containers of other shapes
	 * This is primarily used by clustered particles that leave their shapes in a Union
	 * rather than flattening into the particle's ShapesArray.
	*/
	class CHAOS_API FMultiShapePairCollisionDetector
	{
	public:
		FMultiShapePairCollisionDetector(
			FGeometryParticleHandle* InParticle0,
			const FPerShapeData* InShape0,
			FGeometryParticleHandle* InParticle1,
			const FPerShapeData* InShape1,
			FParticlePairMidPhase& MidPhase);
		FMultiShapePairCollisionDetector(FMultiShapePairCollisionDetector&& R);
		FMultiShapePairCollisionDetector(const FMultiShapePairCollisionDetector& R) = delete;
		FMultiShapePairCollisionDetector& operator=(const FMultiShapePairCollisionDetector& R) = delete;
		~FMultiShapePairCollisionDetector();

		/**
		 * @brief Perform a bounds check and run the narrow phase if necessary
		 * @return The number of collisions constraints that were activated
		*/
		int32 GenerateCollisions(
			const FReal CullDistance,
			const bool bUseCCD,
			const FReal Dt,
			FCollisionContext& Context);

		/**
		 * @brief Callback from the narrow phase to create a collision constraint for this particle pair.
		 * We should never be asked for a collision for a different particle pair, but the 
		 * implicit objects may be children of the root shape.
		*/
		FPBDCollisionConstraint* FindOrCreateConstraint(
			FGeometryParticleHandle* InParticle0,
			const FImplicitObject* Implicit0,
			const FBVHParticles* BVHParticles0,
			const FRigidTransform3& ShapeRelativeTransform0,
			FGeometryParticleHandle* InParticle1,
			const FImplicitObject* Implicit1,
			const FBVHParticles* BVHParticles1,
			const FRigidTransform3& ShapeRelativeTransform1,
			const FReal CullDistance,
			const EContactShapesType ShapePairType,
			const bool bInUseManifold);

		/**
		 * @brief FindOrCreateConstraint for swept constraints 
		*/
		FPBDCollisionConstraint* FindOrCreateSweptConstraint(
			FGeometryParticleHandle* InParticle0,
			const FImplicitObject* Implicit0,
			const FBVHParticles* BVHParticles0,
			const FRigidTransform3& ShapeRelativeTransform0,
			FGeometryParticleHandle* InParticle1,
			const FImplicitObject* Implicit1,
			const FBVHParticles* BVHParticles1,
			const FRigidTransform3& ShapeRelativeTransform1,
			const FReal CullDistance,
			EContactShapesType ShapePairType);

		/**
		 * @brief Reactivate the collision exactly as it was last frame
		 * @return The number of collisions constraints that were restored
		*/
		int32 RestoreCollisions(const FReal CullDistance);

		/**
		 * @brief Reactivate the constraint (essentially the same as Restore but slightly optimized)
		 * @parame SleepEpoch The tick on which the particle went to sleep.
		 * Only constraints that were active when the particle went to sleep should be reactivated.
		*/
		void WakeCollisions(const int32 SleepEpoch);

		void VisitCollisions(const int32 LastEpoch, const FPBDCollisionVisitor& Visitor) const;

	private:
		FPBDCollisionConstraint* FindConstraint(const FCollisionParticlePairConstraintKey& Key);

		FPBDCollisionConstraint* CreateConstraint(
			FGeometryParticleHandle* Particle0,
			const FImplicitObject* Implicit0,
			const FBVHParticles* BVHParticles0,
			const FRigidTransform3& ShapeRelativeTransform0,
			FGeometryParticleHandle* Particle1,
			const FImplicitObject* Implicit1,
			const FBVHParticles* BVHParticles1,
			const FRigidTransform3& ShapeRelativeTransform1,
			const FReal CullDistance,
			const EContactShapesType ShapePairType,
			const bool bInUseManifold,
			const FCollisionParticlePairConstraintKey& Key);

		int32 ProcessNewConstraints();
		void PruneConstraints();

		FParticlePairMidPhase& MidPhase;
		TMap<uint32, TUniquePtr<FPBDCollisionConstraint>> Constraints;
		TArray<FPBDCollisionConstraint*> NewConstraints;
		FGeometryParticleHandle* Particle0;
		FGeometryParticleHandle* Particle1;
		const FPerShapeData* Shape0;
		const FPerShapeData* Shape1;
	};


	class FMidPhaseRestoreThresholds
	{
	public:
		FMidPhaseRestoreThresholds()
			: PositionThreshold(0)
			, RotationThreshold(0)
		{
		}

		FReal PositionThreshold;	// cm
		FReal RotationThreshold;	// rad
	};

	/**
	 * @brief Produce collisions for a particle pair
	 * A FParticlePairMidPhase object is created for every particle pair whose bounds overlap. It is 
	 * responsible for building a set of potentially colliding shape pairs and running collision
	 * detection on those pairs each tick.
	 * 
	 * Most particles have a array of shapes, but not all shapes participate in collision detection
	 * (some are query-only). The cached shape pair list prevents us from repeatesdly testing the
	 * filters of shape pairs that can never collide.
	 * 
	 * @note Geometry collections and clusters do not have arrays of simple shapes. Clustered particles
	 * typically have a Union as one of the root shapes. In this case we do not attempt to cache the
	 * potentially colliding shape pair set, and must process the unions every tick.
	 * 
	 * @note The lifetime of these objects is handled entirely by the CollisionConstraintAllocator. 
	 * Nothing outside of the CollisionConstraintAllocator should hold a pointer to the detector 
	 * or any constraints it creates for more than the duration of the tick.
	*/
	class CHAOS_API FParticlePairMidPhase
	{
	public:
		FParticlePairMidPhase(
			FGeometryParticleHandle* InParticle0, 
			FGeometryParticleHandle* InParticle1, 
			const FCollisionParticlePairKey& InKey,
			FCollisionConstraintAllocator& InCollisionAllocator);

		UE_NONCOPYABLE(FParticlePairMidPhase);

		~FParticlePairMidPhase();


		FGeometryParticleHandle* GetParticle0() { return Particle0; }

		FGeometryParticleHandle* GetParticle1() { return Particle1; }

		const FCollisionParticlePairKey& GetKey() const { return Key; }

		FCollisionConstraintAllocator& GetCollisionAllocator() { return *CollisionAllocator; }

		bool IsValid() const { return (Particle0 != nullptr) && (Particle1 != nullptr); }

		/**
		 * @brief Have we run collision detection since this Epoch (inclusive)
		*/
		bool IsUsedSince(const int32 Epoch) const;

		/**
		 * @brief Whether the particle pair is sleeping and therefore contacts should not be culled (they will be reused on wake)
		*/
		bool IsSleeping() const { return bIsSleeping; }

		/**
		 * @brief Update the sleeping state
		 * If this switches the state to Awake, it will reactivate any collisions between the particle pair that
		 * were active when they went to sleep.
		*/
		void SetIsSleeping(const bool bInIsSleeping);

		/**
		 * @brief Destroy all collisions and prevent this midphasae from being used any more. Called when one of its particles is destoyed.
		 * It will be culled at the next Prune in the CollisionConstraintAllocator. We don't delete it immediately so that we don't
		 * have to remove it from either Particle's ParticleCollisions array (which is O(N) and unnecessary when the particles are being destroyed)
		*/
		void DetachParticle(FGeometryParticleHandle* Particle);

		/**
		 * @brief Delete all cached data and collisions. Should be called when a particle changes its shapes
		*/
		void Reset();

		/**
		 * @brief Create collision constraints for all colliding shape pairs
		*/
		void GenerateCollisions(
			const FReal CullDistance,
			const FReal Dt,
			FCollisionContext& Context);

		/**
		 * @brief Copy a collision and activate it
		 * This is used by the Resim system to restore saved colisions. If there is already a matching constraint
		 * it will be overwritten, otherwise a new constraint will be added.
		*/
		void InjectCollision(const FPBDCollisionConstraint& Constraint);

		/**
		 * @brief Call a function on each active collision constraint
		 * This including sleeping constraints, but not constraints that are were not used on the last awake tick
		 * but are still kept around as an optimization.
		*/
		void VisitCollisions(const FPBDCollisionVisitor& Visitor) const;

	private:
		/**
		 * @brief Set up the midphase based on the SHapesArrays of the two particles
		*/
		void Init();

		/**
		 * @brief Build the list of potentially colliding shape pairs.
		 * This is all the shape pairs in the partilces' shapes arrays that pass the collision filter.
		*/
		void BuildDetectors();

		/**
		 * @brief Add the shape pair to the list of potentially colliding pairs
		*/
		void TryAddShapePair(
			const FPerShapeData* Shape0, 
			const FPerShapeData* Shape1);

		/**
		 * @brief Decide whether we should have CCD enabled on this constraint
		 * @return true if CCD is enabled this tick, false otherwise
		 * This may return false, even for collisions on CCD-enabled bodies when the bodies are moving slowly
		*/
		bool ShouldEnableCCD(const FReal Dt);

		void InitRestoreThresholds();

		/**
		 * @brief Whether we should reuse the constraint as-is and skip the narrow phase
		 * This will be true if neither particle has moved much. 
		 * This is non-const because it updates some position tracking data.
		*/
		bool ShouldRestoreConstraints(const FReal Dt);

		/**
		 * @brief If the particles have not moved muc, reactivate all the colisions and skip the narrow phase.
		*/
		bool TryRestoreConstraints(const FReal Dt, const FReal CullDistance);


		FGeometryParticleHandle* Particle0;
		FGeometryParticleHandle* Particle1;
		FCollisionParticlePairKey Key;

		TArray<FSingleShapePairCollisionDetector, TInlineAllocator<1>> ShapePairDetectors;
		TArray<FMultiShapePairCollisionDetector> MultiShapePairDetectors;
		FCollisionConstraintAllocator* CollisionAllocator;

		bool bIsCCD;
		bool bIsInitialized;
		bool bRestorable;
		bool bIsSleeping;
		int32 LastUsedEpoch;
		int32 NumActiveConstraints;

		// The particle transforms the last time the collisions were updated (used to determine whether we can restore contacts)
		FMidPhaseRestoreThresholds RestoreThresholdZeroContacts;
		FMidPhaseRestoreThresholds RestoreThreshold;
		FVec3 RestoreParticleP0;
		FVec3 RestoreParticleP1;
		FRotation3 RestoreParticleQ0;
		FRotation3 RestoreParticleQ1;
	};
}
