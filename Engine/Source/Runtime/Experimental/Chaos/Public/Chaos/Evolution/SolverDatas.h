// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Containers/SparseArray.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/Evolution/SolverConstraintContainer.h"

namespace Chaos
{
	
/** Forward Declaration */
class FPBDIslandManager;

/**
* List of bodies and constraints that wil be stored per island.
*/
class CHAOS_API FPBDIslandSolverData
{
public :
	/**
	* Init the solver datas 
	*/
	FPBDIslandSolverData(const int32 IslandIndex = 0) : BodyContainer(MakeUnique<FSolverBodyContainer>()), ConstraintDatas(), IslandIndex(IslandIndex)
	{}

	/** Accessors of the island index */
	FORCEINLINE const int32& GetIslandIndex() const { return IslandIndex; };
	FORCEINLINE int32& GetIslandIndex() { return IslandIndex; };
	
	/** Accessors of the body container */
	FORCEINLINE const FSolverBodyContainer& GetBodyContainer() const { return *BodyContainer.Get(); };
	FORCEINLINE FSolverBodyContainer& GetBodyContainer() { return *BodyContainer.Get(); };

	/**
	 * @brief The number of constraint containers registered
	*/
	int32 NumConstraintContainerIds() const { return ConstraintDatas.Num(); }

	/** Accessors of the constraint container */
	template<typename ContainerType> const ContainerType& GetConstraintContainer(const int32 ContainerId) const;
	template<typename ContainerType> ContainerType& GetConstraintContainer(const int32 ContainerId);

	/** Accessors of the constraint indices */
	const TArray<int32>& GetConstraintIndices(const int32 ContainerId) const;
	TArray<int32>& GetConstraintIndices(const int32 ContainerId);
	
	/** Accessors of the constraint handles */
	const TArray<FConstraintHandle*>& GetConstraintHandles(const int32 ContainerId) const;
	TArray<FConstraintHandle*>& GetConstraintHandles(const int32 ContainerId);

	/** Accessor of one constraint handle given a container id and a constraint index */
	template<typename ConstraintType> const ConstraintType* GetConstraintHandle(const int32 ContainerId, const int32 ConstraintIndex) const;
	template<typename ConstraintType> ConstraintType* GetConstraintHandle(const int32 ContainerId, const int32 ConstraintIndex);

	/** Add a constraint container to the solver island given a container id
	* @param ContainerId Constraints container id from which the solver constraint datas is being built
	*/
	template<typename ConstraintType> void AddConstraintDatas(const int32 ContainerId);

protected:
	/**
	* Constraint datas that will be stored per container (joints, collisions...)
	*/
	struct FConstraintDatas
	{
		/** Constraint indices that will be used for legacy solver */
		TArray<int32> ConstraintIndices;

		/** Constraint handles that will be used for legacy solver */
		TArray<FConstraintHandle*> ConstraintHandles;
	};

	/** Solver body container of that datas */
	TUniquePtr<FSolverBodyContainer> BodyContainer;
	
	/** List of constraint containers (collision, joints...) that will be used to solve constraints */
	TSparseArray<TUniquePtr<FConstraintSolverContainer>> ConstraintContainers;
	
	/** List of constraint datas (collision, joints...) that will be used to solve constraints */
	TSparseArray<FConstraintDatas> ConstraintDatas;

	/** Island index in case these datas belong to an island */
	int32 IslandIndex;
};

template<typename ContainerType>
FORCEINLINE const ContainerType& FPBDIslandSolverData::GetConstraintContainer(const int32 ContainerId) const
{
	return *static_cast<const ContainerType*>(ConstraintContainers[ContainerId].Get());
};

template<typename ContainerType>
FORCEINLINE ContainerType& FPBDIslandSolverData::GetConstraintContainer(const int32 ContainerId)
{
	return *static_cast<ContainerType*>(ConstraintContainers[ContainerId].Get());
};
	
FORCEINLINE const TArray<int32>& FPBDIslandSolverData::GetConstraintIndices(const int32 ContainerId) const
{
	return ConstraintDatas[ContainerId].ConstraintIndices;
};
	
FORCEINLINE TArray<int32>& FPBDIslandSolverData::GetConstraintIndices(const int32 ContainerId)
{
	return ConstraintDatas[ContainerId].ConstraintIndices;
};
	
FORCEINLINE const TArray<FConstraintHandle*>& FPBDIslandSolverData::GetConstraintHandles(const int32 ContainerId) const
{
	return ConstraintDatas[ContainerId].ConstraintHandles;
};

FORCEINLINE TArray<FConstraintHandle*>& FPBDIslandSolverData::GetConstraintHandles(const int32 ContainerId)
{
	return ConstraintDatas[ContainerId].ConstraintHandles;
};

template<typename ConstraintType>
FORCEINLINE const ConstraintType* FPBDIslandSolverData::GetConstraintHandle(const int32 ContainerId, const int32 ConstraintIndex) const
{
	return static_cast<const ConstraintType*>(ConstraintDatas[ContainerId].ConstraintHandles[ConstraintIndex]);
};

template<typename ConstraintType>
FORCEINLINE ConstraintType* FPBDIslandSolverData::GetConstraintHandle(const int32 ContainerId, const int32 ConstraintIndex)
{
	return static_cast<ConstraintType*>(ConstraintDatas[ContainerId].ConstraintHandles[ConstraintIndex]);
};
	
template<typename ConstraintType>
inline void FPBDIslandSolverData::AddConstraintDatas(const int32 ContainerId)
{
	if((ContainerId >= 0) && !ConstraintDatas.IsValidIndex(ContainerId) && !ConstraintContainers.IsValidIndex(ContainerId) )
	{
		ConstraintDatas.Reserve(ContainerId+1);
		ConstraintDatas.EmplaceAt(ContainerId, FConstraintDatas());
		
		ConstraintContainers.Reserve(ContainerId+1);
		ConstraintContainers.EmplaceAt(ContainerId, MakeUnique<typename ConstraintType::FConstraintSolverContainerType>());
	}
}
	
}