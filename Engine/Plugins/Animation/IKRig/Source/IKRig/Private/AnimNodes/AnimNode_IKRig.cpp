// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimNodes/AnimNode_IKRig.h"
#include "IKRigProcessor.h"
#include "IKRigDefinition.h"
#include "IKRigSolver.h"
#include "ActorComponents/IKRigInterface.h"
#include "Drawing/ControlRigDrawInterface.h"
#include "Animation/AnimInstanceProxy.h"


void FAnimNode_IKRig::Evaluate_AnyThread(FPoseContext& Output) 
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	if (Source.GetLinkNode() && !bStartFromRefPose)
	{
		Source.Evaluate(Output);
	}
	else
	{
		Output.ResetToRefPose();
	}

	if (!(RigDefinitionAsset && IKRigProcessor))
	{
		return;
	}

	if (!IKRigProcessor->IsInitialized())
	{
		return;
	}

	// copy input pose to solver stack
	CopyInputPoseToSolver(Output.Pose);
	// update target goal transforms
	AssignGoalTargets();
	// run stack of solvers,
	const FTransform WorldToComponent =  Output.AnimInstanceProxy->GetComponentTransform().Inverse();
	IKRigProcessor->Solve(WorldToComponent);
	// updates transforms with new pose
	CopyOutputPoseToAnimGraph(Output.Pose);
}

void FAnimNode_IKRig::CopyInputPoseToSolver(FCompactPose& InputPose)
{
	// start Solve() from REFERENCE pose
	if (bStartFromRefPose)
	{
		IKRigProcessor->SetInputPoseToRefPose();
		return;
	}
	
	// start Solve() from INPUT pose
	// copy local bone transforms into IKRigProcessor skeleton
	FIKRigSkeleton& IKRigSkeleton = IKRigProcessor->GetSkeleton();
	for (FCompactPoseBoneIndex CPIndex : InputPose.ForEachBoneIndex())
	{
		if (int32* Index = CompactPoseToRigIndices.Find(CPIndex))
		{
			// bones that were recorded with rig indices == -1 were not in the
			// Reference Skeleton that the IK Rig was initialized with and therefore
			// are not considered as part of the solve.
			if (*Index != -1)
			{
				IKRigSkeleton.CurrentPoseLocal[*Index] = InputPose[CPIndex];	
			}
		}
	}
	// update global pose in IK Rig
	IKRigSkeleton.UpdateAllGlobalTransformFromLocal();
}

void FAnimNode_IKRig::AssignGoalTargets()
{
	// update goal transforms before solve
	// these transforms can come from a few different sources, handled here...

	// use the goal transforms from the source asset itself
	// this is used to live preview results from the IK Rig editor
	#if WITH_EDITOR
	if (bDriveWithSourceAsset)
	{
		IKRigProcessor->CopyAllInputsFromSourceAssetAtRuntime(RigDefinitionAsset);
		return;
	}
	#endif
	
	// copy transforms from this anim node's goal pins from blueprint
	for (const FIKRigGoal& Goal : Goals)
	{
		IKRigProcessor->SetIKGoal(Goal);
	}

	// override any goals that were manually set with goals from goal creator components (they take precedence)
	for (const TPair<FName, FIKRigGoal>& GoalPair : GoalsFromGoalCreators)
	{
		IKRigProcessor->SetIKGoal(GoalPair.Value);
	}
}

void FAnimNode_IKRig::CopyOutputPoseToAnimGraph(FCompactPose& OutputPose)
{
	FIKRigSkeleton& IKRigSkeleton = IKRigProcessor->GetSkeleton();
	
	// update local transforms of current IKRig pose
	IKRigSkeleton.UpdateAllLocalTransformFromGlobal();

	// copy local transforms to output pose
	for (FCompactPoseBoneIndex CPIndex : OutputPose.ForEachBoneIndex())
	{
		if (int32* Index = CompactPoseToRigIndices.Find(CPIndex))
		{
			// bones that were recorded with rig indices == -1 were not in the
			// Reference Skeleton that the IK Rig was initialized with and therefore
			// are not considered as part of the solve. These transforms are left at their
			// input pose (in local space).
			if (*Index != -1)
			{
				OutputPose[CPIndex] = IKRigSkeleton.CurrentPoseLocal[*Index];	
			}
		}
	}
}

void FAnimNode_IKRig::GatherDebugData(FNodeDebugData& DebugData)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	DebugData.AddDebugItem(FString::Printf(TEXT("%s IK Rig evaluated with %d Goals."), *DebugData.GetNodeName(this), Goals.Num()));
		
	for (const TPair<FName, FIKRigGoal>& GoalPair : GoalsFromGoalCreators)
	{
		DebugData.AddDebugItem(FString::Printf(TEXT("Goal supplied by actor component: %s"), *GoalPair.Value.ToString()));
	}

	for (const FIKRigGoal& Goal : Goals)
	{
		if (GoalsFromGoalCreators.Contains(Goal.Name))
		{
			continue;
		}
		
		DebugData.AddDebugItem(FString::Printf(TEXT("Goal supplied by node pin: %s"), *Goal.ToString()));
	}
}

void FAnimNode_IKRig::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()
	FAnimNode_Base::Initialize_AnyThread(Context);
	Source.Initialize(Context);
}

void FAnimNode_IKRig::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	GetEvaluateGraphExposedInputs().Execute(Context);
	FAnimNode_Base::Update_AnyThread(Context);
	Source.Update(Context);
}

void FAnimNode_IKRig::PreUpdate(const UAnimInstance* InAnimInstance)
{
	if (!IsValid(RigDefinitionAsset))
	{
		return;
	}
	
	if (!IsValid(IKRigProcessor))
	{
		IKRigProcessor = NewObject<UIKRigProcessor>(InAnimInstance->GetOwningComponent());	
	}
	
	// initialize the IK Rig (will only try once on the current version of the rig asset)
	if (!IKRigProcessor->IsInitialized())
	{
		const FReferenceSkeleton& RefSkeleton = InAnimInstance->GetSkelMeshComponent()->SkeletalMesh->GetRefSkeleton();
		IKRigProcessor->Initialize(RigDefinitionAsset, RefSkeleton);
	}
	
	// cache list of goal creator components on the actor
	// TODO tried doing this in Initialize_AnyThread but it would miss some GoalCreator components
	// so it was moved here to be more robust, but we need to profile this and make sure it's not hurting perf
	// (it may be enough to run this once and then never again...needs testing)
	GoalCreators.Reset();
	USkeletalMeshComponent* SkelMeshComponent = InAnimInstance->GetSkelMeshComponent();
	AActor* OwningActor = SkelMeshComponent->GetOwner();
	TArray<UActorComponent*> GoalCreatorComponents =  OwningActor->GetComponentsByInterface( UIKGoalCreatorInterface::StaticClass() );
	for (UActorComponent* GoalCreatorComponent : GoalCreatorComponents)
	{
		IIKGoalCreatorInterface* GoalCreator = Cast<IIKGoalCreatorInterface>(GoalCreatorComponent);
		if (!ensureMsgf(GoalCreator, TEXT("Goal creator component failed cast to IIKGoalCreatorInterface.")))
		{
			continue;
		}
		GoalCreators.Add(GoalCreator);
	}
	
	// pull all the goals out of any goal creators on the owning actor
	// this is done on the main thread because we're talking to actor components here
	GoalsFromGoalCreators.Reset();
	for (IIKGoalCreatorInterface* GoalCreator : GoalCreators)
	{
		GoalCreator->AddIKGoals_Implementation(GoalsFromGoalCreators);
	}
}

void FAnimNode_IKRig::SetProcessorNeedsInitialized()
{
	if (IKRigProcessor)
	{
		IKRigProcessor->SetNeedsInitialized();
	}
}

void FAnimNode_IKRig::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	FAnimNode_Base::CacheBones_AnyThread(Context);
	Source.CacheBones(Context);
	
	const FBoneContainer& RequiredBones = Context.AnimInstanceProxy->GetRequiredBones();
	if (!RequiredBones.IsValid())
	{
		return;
	}
	
	if (!RigDefinitionAsset)
	{
		return;
	}

	if (!IKRigProcessor)
	{
		return;
	}

	if (!IKRigProcessor->IsInitialized())
	{
		return;
	}

	// fill up node names, mapping the anim graph bone indices to the IK Rig bones
	CompactPoseToRigIndices.Reset();
	const TArray<FBoneIndexType>& RequiredBonesArray = RequiredBones.GetBoneIndicesArray();
	const FReferenceSkeleton& RefSkeleton = RequiredBones.GetReferenceSkeleton();
	const int32 NumBones = RequiredBonesArray.Num();
	for (uint16 Index = 0; Index < NumBones; ++Index)
	{
		const int32 MeshBone = RequiredBonesArray[Index];
		if (!ensure(MeshBone != INDEX_NONE))
		{
			continue;
		}
		
		FCompactPoseBoneIndex CPIndex = RequiredBones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBone));
		const FName Name = RefSkeleton.GetBoneName(MeshBone);
		CompactPoseToRigIndices.Add(CPIndex) = IKRigProcessor->GetSkeleton().GetBoneIndexFromName(Name);
	}
}

void FAnimNode_IKRig::ConditionalDebugDraw(
	FPrimitiveDrawInterface* PDI,
	USkeletalMeshComponent* PreviewSkelMeshComp) const
{
#if WITH_EDITOR

	// is anim graph setup?
	if (!(bEnableDebugDraw && PreviewSkelMeshComp && PreviewSkelMeshComp->GetWorld()))
	{
		return;
	}

	// is node setup?
	if (!(RigDefinitionAsset && IKRigProcessor && IKRigProcessor->IsInitialized()))
	{
		return;
	}

	const TArray<FIKRigGoal>& ProcessorGoals = IKRigProcessor->GetGoalContainer().GetGoalArray();
	for (const FIKRigGoal& Goal : ProcessorGoals)
	{
		DrawOrientedWireBox(PDI, Goal.FinalBlendedPosition, FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector, FVector::One() * DebugScale, FLinearColor::Yellow, SDPG_World);
		DrawCoordinateSystem(PDI, Goal.Position, Goal.FinalBlendedRotation.Rotator(), DebugScale, SDPG_World);
	}
#endif
}