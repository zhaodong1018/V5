// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PBDConstraintRule.h"

#include "Chaos/Island/SolverIsland.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/PBDPositionConstraints.h"
#include "Chaos/PBDSuspensionConstraints.h"
#include "Chaos/PBDRigidDynamicSpringConstraints.h"
#include "Chaos/PBDRigidSpringConstraints.h"

namespace Chaos
{
	int32 ChaosShockPropagationVelocityPerLevelIterations = 1;
	int32 ChaosShockPropagationPositionPerLevelIterations = 1;
	FAutoConsoleVariableRef CVarChaosShockPropagationPositionPerLevelIterations(TEXT("p.Chaos.ShockPropagation.Position.PerLevelIterations"), ChaosShockPropagationPositionPerLevelIterations, TEXT(""));
	FAutoConsoleVariableRef CVarChaosShockPropagationVelocityPerLevelIterations(TEXT("p.Chaos.ShockPropagation.Velocity.PerLevelIterations"), ChaosShockPropagationVelocityPerLevelIterations, TEXT(""));

	int32 ChaosCollisionColorMinParticles = 2000;
	FAutoConsoleVariableRef CVarChaosCollisionColorMinParticles(TEXT("p.Chaos.Collision.Color.MinParticles"), ChaosCollisionColorMinParticles, TEXT(""));

	/** Console variable to disable the levels computation on each island */
	int32 ChaosDisableIslandLevels = false;
	FAutoConsoleVariableRef CVarChaosDisableIslandLevels(TEXT("p.Chaos.Islands.DisableLevels"), ChaosDisableIslandLevels, TEXT(""));

	/** console variable to disable the colors computation on each island */
	int32 ChaosDisableIslandColors = true;
	FAutoConsoleVariableRef CVarChaosDisableIslandColors(TEXT("p.Chaos.Islands.DisableColors"), ChaosDisableIslandColors, TEXT(""));


	template<class ConstraintType>
	TSimpleConstraintRule<ConstraintType>::TSimpleConstraintRule(int32 InPriority, FConstraints& InConstraints)
		: FSimpleConstraintRule(InPriority)
		, Constraints(InConstraints)
	{
	}

	template<class ConstraintType>
	TSimpleConstraintRule<ConstraintType>::~TSimpleConstraintRule()
	{
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::PrepareTick()
	{
		Constraints.PrepareTick();
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::UnprepareTick()
	{
		Constraints.UnprepareTick();
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::UpdatePositionBasedState(const FReal Dt)
	{
		return Constraints.UpdatePositionBasedState(Dt);
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::BindToDatas(FPBDIslandSolverData& InSolverDatas, const uint32 InContainerId)
	{
		Constraints.SetContainerId(InContainerId);

		SolverData = &InSolverDatas;
		if (SolverData != nullptr)
		{
			SolverData->template AddConstraintDatas<ConstraintType>(Constraints.GetContainerId());
		}
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::GatherSolverInput(const FReal Dt)
	{
		if(SolverData)
		{
			Constraints.SetNumIslandConstraints(Constraints.NumConstraints(), *SolverData);
			Constraints.GatherInput(Dt, *SolverData);
		}
	}

	template<class ConstraintType>
	void TSimpleConstraintRule<ConstraintType>::ScatterSolverOutput(const FReal Dt)
	{
		if(SolverData) Constraints.ScatterOutput(Dt, *SolverData);
	}

	template<class ConstraintType>
	bool TSimpleConstraintRule<ConstraintType>::ApplyConstraints(const FReal Dt, const int32 It, const int32 NumIts)
	{
		return SolverData ? Constraints.ApplyPhase1(Dt, It, NumIts, *SolverData) : false;
	}

	template<class ConstraintType>
	bool TSimpleConstraintRule<ConstraintType>::ApplyPushOut(const FReal Dt, const int32 It, const int32 NumIts)
	{
		return SolverData ? Constraints.ApplyPhase2(Dt, It, NumIts, *SolverData) : false;
	}
	
	template<class ConstraintType>
	TPBDConstraintGraphRuleImpl<ConstraintType>::TPBDConstraintGraphRuleImpl(FConstraints& InConstraints, int32 InPriority)
		: FPBDConstraintGraphRule(InPriority)
		, Constraints(InConstraints)
		, ConstraintGraph(nullptr)
	{
	}
	
	template<class ConstraintType>
	void TPBDConstraintGraphRuleImpl<ConstraintType>::BindToGraph(FPBDConstraintGraph& InContactGraph, uint32 InContainerId)
	{
		Constraints.SetContainerId(InContainerId);
		ConstraintGraph = &InContactGraph;
	}

	template<class ConstraintType>
	void TPBDConstraintGraphRuleImpl<ConstraintType>::UpdatePositionBasedState(const FReal Dt)
	{
		Constraints.UpdatePositionBasedState(Dt);
	}
	
	template<class ConstraintType>
	void TPBDConstraintGraphRuleImpl<ConstraintType>::AddToGraph()
	{
		ConstraintGraph->ReserveConstraints(Constraints.NumConstraints());

		for (typename FConstraints::FConstraintContainerHandle * ConstraintHandle : Constraints.GetConstraintHandles())
		{
			if (ConstraintHandle->IsEnabled())
			{
				ConstraintGraph->AddConstraint(GetContainerId(), ConstraintHandle, ConstraintHandle->GetConstrainedParticles());
			}
		}
	}

	template<class ConstraintType>
	TPBDConstraintIslandRule<ConstraintType>::TPBDConstraintIslandRule(FConstraints& InConstraints, int32 InPriority)
		: TPBDConstraintGraphRuleImpl<ConstraintType>(InConstraints, InPriority)
	{
	}

	template<class ConstraintType>
	TPBDConstraintIslandRule<ConstraintType>::~TPBDConstraintIslandRule()
	{
	}

	template<class ConstraintType>
	void TPBDConstraintIslandRule<ConstraintType>::GatherSolverInput(const FReal Dt, int32 Island)
	{
		if(FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
		{
			const TArray<FConstraintHandleHolder>& IslandConstraints = ConstraintGraph->GetIslandConstraints(Island);

			// This will reset the number of constraints inside the solver datas. For now we keep this function since according
			// to the constraints we can use the handles, the indices or the container. when only the container will be used
			// we can replace this call by :
			// IslandSolver->template GetConstraintContainer<typename ConstraintType::FSolverConstraintContainerType>(ContainerId)->Reset();
			Constraints.SetNumIslandConstraints(IslandConstraints.Num(), *IslandSolver);

			for (FConstraintHandle* ConstraintHandle : IslandConstraints)
			{
				if (ConstraintHandle->GetContainerId() == GetContainerId())
				{
					FConstraintContainerHandle* Constraint = ConstraintHandle->As<FConstraintContainerHandle>();

					// Note we are building the SolverBodies as we go, in the order that we visit them. Each constraint
					// references two bodies, so we won't strictly be accessing only in cache order, but it's about as good as it can be.
					if (Constraint->IsEnabled())
					{
						// @todo(chaos): we should provide Particle Levels in the island rule as well (see TPBDConstraintColorRule)
						Constraint->GatherInput(Dt, INDEX_NONE, INDEX_NONE, *IslandSolver);
					}
				}
			}
		}
	}

	template<class ConstraintType>
	void TPBDConstraintIslandRule<ConstraintType>::ScatterSolverOutput(const FReal Dt, int32 Island)
	{
		if(FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
		{
			Constraints.ScatterOutput(Dt, *IslandSolver);
		}
	}
	
	template<class ConstraintType>
	bool TPBDConstraintIslandRule<ConstraintType>::ApplyConstraints(const FReal Dt, int32 Island, const int32 It, const int32 NumIts)
	{
		FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island);
		return IslandSolver ? Constraints.ApplyPhase1Serial(Dt, It, NumIts, *IslandSolver) : false;
	}

	template<class ConstraintType>
	bool TPBDConstraintIslandRule<ConstraintType>::ApplyPushOut(const FReal Dt, int32 Island, const int32 It, const int32 NumIts)
	{
		FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island);
		return IslandSolver  ? Constraints.ApplyPhase2Serial(Dt, It, NumIts, *IslandSolver) : false;
	}

	template<class ConstraintType>
	void TPBDConstraintIslandRule<ConstraintType>::InitializeAccelerationStructures()
	{
		ConstraintGraph->template AddConstraintDatas<ConstraintType>(Constraints.GetContainerId());
	}

	template<class ConstraintType>
	void TPBDConstraintIslandRule<ConstraintType>::UpdateAccelerationStructures(const FReal Dt, const int32 Island)
	{
	}
	
	template<class ConstraintType>
	int32 TPBDConstraintGraphRuleImpl<ConstraintType>::NumConstraints() const
	{ 
		return Constraints.NumConstraints(); 
	}

	template<class ConstraintType>
	TPBDConstraintColorRule<ConstraintType>::TPBDConstraintColorRule(FConstraints& InConstraints, int32 InPriority)
		: TPBDConstraintIslandRule<ConstraintType>(InConstraints, InPriority)
		, ConstraintSets()
	{
	}

	template<class ConstraintType>
	TPBDConstraintColorRule<ConstraintType>::~TPBDConstraintColorRule()
	{
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::GatherSolverInput(const FReal Dt, int32 Island)
	{
		FPBDConstraintGraph* LocalGraph = ConstraintGraph;
		auto GetParticleLevel = [LocalGraph](TGeometryParticleHandle<FReal, 3>* ConstrainedParticle) -> int32
		{
			int32 ParticleLevel = INDEX_NONE;
			if(FPBDRigidParticleHandle* PBDRigid = ConstrainedParticle->CastToRigidParticle())
			{
				if(LocalGraph->GetIslandGraph()->GraphNodes.IsValidIndex(PBDRigid->ConstraintGraphIndex()))
				{
					ParticleLevel = FMath::Max( LocalGraph->GetIslandGraph()->GraphNodes[PBDRigid->ConstraintGraphIndex()].LevelIndex, 0);
				}
			}
			return ParticleLevel;
		};
		if (IsSortingEnabled())
		{
			if (FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
			{
				const int32 GraphIndex = ConstraintGraph->GetGraphIndex(Island);
				if(ConstraintGraph->GetIslandGraph()->GraphIslands.IsValidIndex(GraphIndex))
				{
					const int32 MaxColor = IsSortingUsingColors() ? FMath::Max(1,ConstraintGraph->GetIslandGraph()->GraphIslands[GraphIndex].MaxColors+1) : 1;
					const int32 MaxLevel = IsSortingUsingLevels() ? FMath::Max(1,ConstraintGraph->GetIslandGraph()->GraphIslands[GraphIndex].MaxLevels+1) : 1;
					
					TArray<TPair<int32, int32>>& IslandConstraintSets = ConstraintSets[Island];
					IslandConstraintSets.Reset(MaxLevel * MaxColor);	// Pessimistic array size - we could store the actual required size in coloring algorithm
					int32 ConstraintSetEnd = 0;
					
					Constraints.SetNumIslandConstraints(ConstraintGraph->GetIslandConstraints(Island).Num(), *IslandSolver);
					
					for (int32 Level = 0; Level < MaxLevel; ++Level)
					{
						for (int32 Color = 0; Color < MaxColor; ++Color)
						{
							const int32 OffsetIndex = IslandOffsets[Island] + Level * MaxColor + Color;
							const int32 OffsetBegin = (OffsetIndex == 0) ? 0 : ConstraintOffsets[OffsetIndex-1];
							const int32 OffsetEnd = ConstraintOffsets[OffsetIndex];
							
							 if (OffsetEnd != OffsetBegin)
							 {
							 	// Calculate the range of indices for this color as a set of independent contacts
							 	TPair<int32, int32> ColorConstrainSet(ConstraintSetEnd, ConstraintSetEnd);
							 	for(int32 ConstraintIndex = OffsetBegin; ConstraintIndex < OffsetEnd; ++ConstraintIndex)
							 	{
							 		FConstraintContainerHandle* Constraint = SortedConstraints[ConstraintIndex];
							 		if (Constraint->IsEnabled())
							 		{
							 			// Levels that should be assigned to the bodies for shock propagation
							 			// @todo(chaos): optimize the lookup
							 			const TVector<TGeometryParticleHandle<FReal, 3>*, 2> ConstrainedParticles = Constraint->GetConstrainedParticles();
							 			const int32 Particle0Level = GetParticleLevel(ConstrainedParticles[0]);
							 			const int32 Particle1Level = GetParticleLevel(ConstrainedParticles[1]);
							
							 			// Note we are building the SolverBodies as we go, in the order that we visit them. Each constraint
							 			// references two bodies, so we won't strictly be accessing only in cache order, but it's about as good as it can be.
							 			Constraint->GatherInput(Dt, Particle0Level, Particle1Level, *IslandSolver);
							
							 			// Update the current constraint set of this color
							 			ColorConstrainSet.Value = ++ConstraintSetEnd;
							 		}
							 	}
							 	// Remember the set of constraints of this color
							 	if (IsSortingUsingColors()) 
							 	{
							 		IslandConstraintSets.Add(ColorConstrainSet);
							 	}
							 }
						}
					}
					// If we aren't coloring, we have a single group of all constraints (they have been created in level order above)
					if (!IsSortingUsingColors())
					{
						const TPair<int32, int32> ConstrainSet(0, ConstraintSetEnd);
						IslandConstraintSets.Add(ConstrainSet);
					}
				}
			}
		}
		else
		{
			Base::GatherSolverInput(Dt, Island);
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::ScatterSolverOutput(const FReal Dt, int32 Island)
	{
		if (IsSortingEnabled())
		{
			if (FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
			{
				const TArray<TPair<int32, int32>>& IslandConstraintSets = ConstraintSets[Island];
				for (const TPair<int32, int32>& ConstraintSet : IslandConstraintSets)
				{
					Constraints.ScatterOutput(Dt, ConstraintSet.Key, ConstraintSet.Value, *IslandSolver);
				}
			}
		}
		else
		{
			return Base::ScatterSolverOutput(Dt, Island);
		}
	}

	template<class ConstraintType>
	bool TPBDConstraintColorRule<ConstraintType>::ApplyConstraints(const FReal Dt, int32 Island, const int32 It, const int32 NumIts)
	{
		if (IsSortingEnabled())
		{
			bool bNeedsAnotherIteration = false;
			if (FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
			{
				const TArray<TPair<int32, int32>>& IslandConstraintSets = ConstraintSets[Island];
				if (!IsSortingUsingColors())
				{
					for (const TPair<int32, int32>& ConstraintSet : IslandConstraintSets)
					{
						bNeedsAnotherIteration |= Constraints.ApplyPhase1Serial(Dt, It, NumIts, ConstraintSet.Key, ConstraintSet.Value, *IslandSolver);
					}
				}
				else
				{
					for (const TPair<int32, int32>& ConstraintSet : IslandConstraintSets)
					{
						bNeedsAnotherIteration |= Constraints.ApplyPhase1Parallel(Dt, It, NumIts, ConstraintSet.Key, ConstraintSet.Value, *IslandSolver);
					}
				}
			}
			return bNeedsAnotherIteration;
		}
		else
		{
			return Base::ApplyConstraints(Dt, Island, It, NumIts);
		}
	}

	template<class ConstraintType>
	bool TPBDConstraintColorRule<ConstraintType>::ApplyPushOut(const FReal Dt, int32 Island, const int32 It, const int32 NumIts)
	{
		if (IsSortingEnabled())
		{
			bool bNeedsAnotherIteration = false;
			if (FPBDIslandSolver* IslandSolver = ConstraintGraph->GetSolverIsland(Island))
			{
				const TArray<TPair<int32, int32>>& IslandConstraintSets = ConstraintSets[Island];
				if (!IsSortingUsingColors())
				{
					for (const TPair<int32, int32>& ConstraintSet : IslandConstraintSets)
					{
						bNeedsAnotherIteration |= Constraints.ApplyPhase2Serial(Dt, It, NumIts, ConstraintSet.Key, ConstraintSet.Value, *IslandSolver);
					}
				}
				else
				{
					for (const TPair<int32, int32>& ConstraintSet : IslandConstraintSets)
					{
						bNeedsAnotherIteration |= Constraints.ApplyPhase2Parallel(Dt, It, NumIts, ConstraintSet.Key, ConstraintSet.Value, *IslandSolver);
					}
				}
			}
			return bNeedsAnotherIteration;
		}
		else
		{
			return Base::ApplyPushOut(Dt, Island, It, NumIts);
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::InitializeAccelerationStructures()
	{
		if (IsSortingEnabled())
		{
			ConstraintSets.SetNum(ConstraintGraph->NumIslands());
			ConstraintGraph->template AddConstraintDatas<ConstraintType>(Constraints.GetContainerId());
		}
		else
		{
			Base::InitializeAccelerationStructures();
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::UpdateAccelerationStructures(const FReal Dt, const int32 Island)
	{}
	
	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::SortConstraints()
	{
		// Compute levels for each constraints
		ComputeLevels();

		// Compute colors for each constraints
		ComputeColors();

		// Populate the sorted constraints based on the island/level/color
		PopulateConstraints();
	}
	
	template<class ConstraintType>
    bool TPBDConstraintColorRule<ConstraintType>::IsSortingEnabled() const 
    {
        return IsSortingUsingColors() || IsSortingUsingLevels();
    }
	
	template<class ConstraintType>
    bool TPBDConstraintColorRule<ConstraintType>::IsSortingUsingLevels() const 
    {
    	return !ChaosDisableIslandLevels;
    }
    
    template<class ConstraintType>
    bool TPBDConstraintColorRule<ConstraintType>::IsSortingUsingColors() const
    {
    	return !ChaosDisableIslandColors;
    }
	
	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::ComputeLevels()
	{
		if (IsSortingUsingLevels())
		{
			ConstraintGraph->GetIslandGraph()->ComputeLevels(Constraints.GetContainerId());
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::ComputeColors()
	{
		if (IsSortingUsingColors())
		{
			ConstraintGraph->GetIslandGraph()->ComputeColors(Constraints.GetContainerId(), ChaosCollisionColorMinParticles);
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::ForEachEdges(TFunctionRef<void(const int32, const FPBDConstraintGraph::GraphType::FGraphEdge&)> InFunction)
	{
		for(auto& GraphEdge : ConstraintGraph->GetIslandGraph()->GraphEdges)
		{
			auto& GraphIsland = ConstraintGraph->GetIslandGraph()->GraphIslands[GraphEdge.IslandIndex];
			if(GraphEdge.ItemContainer == Constraints.GetContainerId() && !GraphIsland.IslandItem->IsSleeping())
			{
				const int32 EdgeColor = IsSortingUsingColors() ? FMath::Max(0,GraphEdge.ColorIndex) : 0;
				const int32 EdgeLevel = IsSortingUsingLevels() ? FMath::Max(0,GraphEdge.LevelIndex) : 0;
				const int32 MaxColors = IsSortingUsingColors() ? FMath::Max(1,GraphIsland.MaxColors+1) : 1;

				const int32 IslandOffset = IslandOffsets[GraphIsland.IslandItem->GetIslandIndex()];
				const int32 OffsetIndex = IslandOffset + EdgeLevel * MaxColors + EdgeColor;

				InFunction(OffsetIndex,GraphEdge);
			}
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::PopulateConstraints()
	{
		if(IsSortingEnabled())
		{
			SortedConstraints.SetNum(ConstraintGraph->GetIslandGraph()->GraphEdges.Num(), false);
			IslandOffsets.SetNum(ConstraintGraph->NumIslands(), false);

			// We first fill the island offsets that will be used in the gather
			int32 IslandOffset = 0;
			for(auto& GraphIsland : ConstraintGraph->GetIslandGraph()->GraphIslands)
			{
				if(!GraphIsland.IslandItem->IsSleeping())
				{
					IslandOffsets[GraphIsland.IslandItem->GetIslandIndex()] = IslandOffset;
					
					const int32 MaxColors = IsSortingUsingColors() ? FMath::Max(1,GraphIsland.MaxColors+1) : 1;
					const int32 MaxLevels = IsSortingUsingLevels() ? FMath::Max(1,GraphIsland.MaxLevels+1) : 1;

					IslandOffset += MaxLevels * MaxColors;
				}
			}
			// Initialization of the constraint offsets and the offsets counters
			ConstraintOffsets.SetNum(IslandOffset, false);
			OffsetCounters.SetNum(IslandOffset, false);
			
			FMemory::Memzero(ConstraintOffsets.GetData(), ConstraintOffsets.Num() * sizeof(int32));
			FMemory::Memzero(OffsetCounters.GetData(), ConstraintOffsets.Num() * sizeof(int32));
			
			auto& LocalConstraintOffsets = ConstraintOffsets;
			auto& LocalOffsetCounters = OffsetCounters;
			auto& LocalSortedConstraints = SortedConstraints;

			// Build of the constraint offsets that will be used to know where the sorted constraints will
			// be inserted into the flat array  
			ForEachEdges( [&LocalConstraintOffsets](
				const int32 OffsetIndex, const FPBDConstraintGraph::GraphType::FGraphEdge& GraphEdge)
			{
				++LocalConstraintOffsets[OffsetIndex];
			});

			for(int32 OffsetIndex = 0, OffsetEnd = ConstraintOffsets.Num()-1; OffsetIndex < OffsetEnd; ++OffsetIndex)
			{
				ConstraintOffsets[OffsetIndex+1] += ConstraintOffsets[OffsetIndex];
			}

			// Insert the constraint handles in the right order : Island|Level|Color to be processed by the Gather
			ForEachEdges( [&LocalConstraintOffsets, &LocalOffsetCounters, &LocalSortedConstraints](
				const int32 OffsetIndex, const FPBDConstraintGraph::GraphType::FGraphEdge& GraphEdge)
			{
				const int32 OffsetValue = (OffsetIndex == 0) ? 0 : LocalConstraintOffsets[OffsetIndex-1];
				LocalSortedConstraints[OffsetValue+LocalOffsetCounters[OffsetIndex]] = GraphEdge.EdgeItem->As<FConstraintContainerHandle>();
				
				++LocalOffsetCounters[OffsetIndex];
			});
		}
	}

	template<class ConstraintType>
	void TPBDConstraintColorRule<ConstraintType>::SetUseContactGraph(const bool bInUseContactGraph)
	{}


	template class TSimpleConstraintRule<FPBDCollisionConstraints>;
	template class TSimpleConstraintRule<FPBDJointConstraints>;
	template class TSimpleConstraintRule<FPBDRigidSpringConstraints>;

	template class TPBDConstraintGraphRuleImpl<FPBDCollisionConstraints>;
	template class TPBDConstraintGraphRuleImpl<FPBDJointConstraints>;
	template class TPBDConstraintGraphRuleImpl<FPBDPositionConstraints>;
	template class TPBDConstraintGraphRuleImpl<FPBDSuspensionConstraints>;
	template class TPBDConstraintGraphRuleImpl<FPBDRigidDynamicSpringConstraints>;
	template class TPBDConstraintGraphRuleImpl<FPBDRigidSpringConstraints>;

	template class TPBDConstraintColorRule<FPBDCollisionConstraints>;
	template class TPBDConstraintIslandRule<FPBDJointConstraints>;
	template class TPBDConstraintIslandRule<FPBDPositionConstraints>;
	template class TPBDConstraintIslandRule<FPBDSuspensionConstraints>;
	template class TPBDConstraintIslandRule<FPBDRigidDynamicSpringConstraints>;
	template class TPBDConstraintIslandRule<FPBDRigidSpringConstraints>;
}