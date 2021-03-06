// Copyright Epic Games, Inc. All Rights Reserved.

#include "IKRigProcessor.h"
#include "IKRigDefinition.h"
#include "IKRigSolver.h"

void UIKRigProcessor::Initialize(const UIKRigDefinition* InRigAsset, const FIKRigInputSkeleton& InputSkeleton)
{
	// we instantiate UObjects here which MUST be done on game thread...
	check(IsInGameThread());
	check(InRigAsset);
	
	bInitialized = false;

	// bail out if we've already tried initializing with this exact version of the rig asset
	if (bTriedToInitialize)
	{
		return; // don't keep spamming
	}

	// ok, lets try to initialize
	bTriedToInitialize = true;
	
	if (InRigAsset->Skeleton.BoneNames.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Trying to initialize IKRig that has no skeleton: %s"), *InRigAsset->GetName());
		return;
	}

	if (!UIKRigProcessor::IsIKRigCompatibleWithSkeleton(InRigAsset, InputSkeleton))
	{
		UE_LOG(LogTemp, Error, TEXT("Trying to initialize IKRig with a Skeleton that is missing required bones. See output log. %s"), *InRigAsset->GetName());
		return;
	}

	// copy skeleton data from the actual skeleton we want to run on
	Skeleton.SetInputSkeleton(InputSkeleton, InRigAsset->Skeleton.ExcludedBones);
	
	// initialize goals based on source asset
	GoalContainer.Empty();
	const TArray<UIKRigEffectorGoal*>& GoalsInAsset = InRigAsset->GetGoalArray();
	for (const UIKRigEffectorGoal* GoalInAsset : GoalsInAsset)
	{
		// add a copy of the Goal to the container
		GoalContainer.SetIKGoal(GoalInAsset);
	}
	
	// initialize goal bones from asset
	GoalBones.Reset();
	for (const UIKRigEffectorGoal* EffectorGoal : GoalsInAsset)
	{	
		FGoalBone NewGoalBone;
		NewGoalBone.BoneName = EffectorGoal->BoneName;
		NewGoalBone.BoneIndex = Skeleton.GetBoneIndexFromName(EffectorGoal->BoneName);

		// validate that the skeleton we are trying to solve this goal on contains the bone the goal expects
		if (NewGoalBone.BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("IK Rig, %s has a Goal, '%s' that references an unknown bone, '%s'. Cannot evaluate."),
				*InRigAsset->GetName(), *EffectorGoal->GoalName.ToString(), *EffectorGoal->BoneName.ToString());
			return;
		}

		// validate that there is not already a different goal, with the same name, that is using a different bone
		// (all goals with the same name must reference the same bone within a single IK Rig)
		if (const FGoalBone* Bone = GoalBones.Find(EffectorGoal->GoalName))
		{
			if (Bone->BoneName != NewGoalBone.BoneName)
			{
				UE_LOG(LogTemp, Warning, TEXT("IK Rig, %s has a Goal, '%s' that references different bones in different solvers, '%s' and '%s'. Cannot evaluate."),
                *InRigAsset->GetName(), *EffectorGoal->GoalName.ToString(), *Bone->BoneName.ToString(), *NewGoalBone.BoneName.ToString());
				return;
			}
		}
		
		GoalBones.Add(EffectorGoal->GoalName, NewGoalBone);
	}

	// create copies of all the solvers in the IK rig
	const TArray<UIKRigSolver*>& AssetSolvers = InRigAsset->GetSolverArray();
	Solvers.Reset(AssetSolvers.Num());
	int32 SolverIndex = 0;
	for (const UIKRigSolver* IKRigSolver : AssetSolvers)
	{
		if (!IKRigSolver)
		{
			// this can happen if asset references deleted IK Solver type
			// which should only happen during development (if at all)
			UE_LOG(LogTemp, Warning, TEXT("IK Rig, %s has null/unknown solver in it. Please remove it."), *InRigAsset->GetName());
			continue;
		}

		// new solver name
		FString Name = IKRigSolver->GetName() + "_SolverInstance_";
		Name.AppendInt(SolverIndex++);
		UIKRigSolver* Solver = DuplicateObject(IKRigSolver, this, FName(*Name));
		Solver->Initialize(Skeleton);
		Solvers.Add(Solver);
	}

	bInitialized = true;
}

void UIKRigProcessor::Initialize(const UIKRigDefinition* InRigAsset, const FReferenceSkeleton& RefSkeleton)
{
	const FIKRigInputSkeleton InputSkeleton = FIKRigInputSkeleton(RefSkeleton);
	Initialize(InRigAsset, InputSkeleton);
}

bool UIKRigProcessor::IsIKRigCompatibleWithSkeleton(const UIKRigDefinition* InRigAsset, const FIKRigInputSkeleton& InputSkeleton)
{
	// first we validate that all the required bones are in the input skeleton...
	
	TSet<FName> RequiredBones;
	const TArray<UIKRigSolver*>& AssetSolvers = InRigAsset->GetSolverArray();
	for (const UIKRigSolver* Solver : AssetSolvers)
	{
		const FName RootBone = Solver->GetRootBone();
		if (RootBone != NAME_None)
		{
			RequiredBones.Add(RootBone);
		}

		Solver->GetBonesWithSettings(RequiredBones);
	}

	const TArray<UIKRigEffectorGoal*>& Goals = InRigAsset->GetGoalArray();
	for (const UIKRigEffectorGoal* Goal : Goals)
	{
		RequiredBones.Add(Goal->BoneName);
	}

	bool bAllRequiredBonesFound = true;
	for (const FName& RequiredBone : RequiredBones)
	{
		if (!InputSkeleton.BoneNames.Contains(RequiredBone))
		{
			UE_LOG(LogTemp, Warning, TEXT("IK Rig, '%s' is missing a required bone in Skeletal Mesh: '%s'."), *InRigAsset->GetName(), *RequiredBone.ToString());
			bAllRequiredBonesFound = false;
		}
	}

	if (!bAllRequiredBonesFound)
	{
		return false;
	}

	// now we validate that hierarchy matches for all required bones...
	bool bAllParentsValid = true;
	
	for (const FName& RequiredBone : RequiredBones)
	{
		const int32 InputBoneIndex = InputSkeleton.BoneNames.Find(RequiredBone);
		const int32 AssetBoneIndex = InRigAsset->Skeleton.BoneNames.Find(RequiredBone);

		// we shouldn't get this far otherwise due to early return above...
		check(InputBoneIndex != INDEX_NONE && AssetBoneIndex != INDEX_NONE)

		// validate that input skeleton hierarchy is as expected
		const int32 AssetParentIndex = InRigAsset->Skeleton.ParentIndices[AssetBoneIndex];
		if (InRigAsset->Skeleton.BoneNames.IsValidIndex(AssetParentIndex)) // root bone has no parent
		{
			const FName& AssetParentName = InRigAsset->Skeleton.BoneNames[AssetParentIndex];
			const int32 InputParentIndex = InputSkeleton.ParentIndices[InputBoneIndex];
			if (!InputSkeleton.BoneNames.IsValidIndex(InputParentIndex))
			{
				bAllParentsValid = false;
				UE_LOG(LogTemp, Error,
					TEXT("IK Rig is running on a skeleton with a required bone, '%s', that expected to have a valid parent. The expected parent was, '%s'."),
					*RequiredBone.ToString(),
					*AssetParentName.ToString());
				continue;
			}
			const FName& InputParentName = InputSkeleton.BoneNames[InputParentIndex];
			if (AssetParentName != InputParentName)
			{
				// we only warn about this, because it may be nice not to have the exact same hierarchy
				UE_LOG(LogTemp, Warning,
					TEXT("IK Rig is running on a skeleton with a required bone, '%s', that has a different parent '%s'. The expected parent was, '%s'."),
					*InputParentName.ToString(),
					*InputParentName.ToString(),
					*AssetParentName.ToString());
				continue;
			}
		}
	}

	return bAllParentsValid;
}

void UIKRigProcessor::SetInputPoseGlobal(const TArray<FTransform>& InGlobalBoneTransforms) 
{
	check(bInitialized);
	check(InGlobalBoneTransforms.Num() == Skeleton.CurrentPoseGlobal.Num());
	Skeleton.CurrentPoseGlobal = InGlobalBoneTransforms;
	Skeleton.UpdateAllLocalTransformFromGlobal();
}

void UIKRigProcessor::SetInputPoseToRefPose()
{
	check(bInitialized);
	Skeleton.CurrentPoseGlobal = Skeleton.RefPoseGlobal;
	Skeleton.UpdateAllLocalTransformFromGlobal();
}

void UIKRigProcessor::SetIKGoal(const FIKRigGoal& InGoal)
{
	check(bInitialized);
	GoalContainer.SetIKGoal(InGoal);
}

void UIKRigProcessor::SetIKGoal(const UIKRigEffectorGoal* InGoal)
{
	check(bInitialized);
	GoalContainer.SetIKGoal(InGoal);
}

void UIKRigProcessor::Solve(const FTransform& ComponentToWorld)
{
	check(bInitialized);
	
	// convert goals into component space and blend towards input pose by alpha
	ResolveFinalGoalTransforms(ComponentToWorld);

	// run all the solvers
	for (UIKRigSolver* Solver : Solvers)
	{
		#if WITH_EDITOR
		if (Solver->IsEnabled())
		{
			Solver->Solve(Skeleton, GoalContainer);
		}
		#else
		Solver->Solve(Skeleton, GoalContainer);
		#endif
	}

	// make sure rotations are normalized coming out
	Skeleton.NormalizeRotations(Skeleton.CurrentPoseGlobal);
}

void UIKRigProcessor::CopyOutputGlobalPoseToArray(TArray<FTransform>& OutputPoseGlobal) const
{
	OutputPoseGlobal = Skeleton.CurrentPoseGlobal;
}

void UIKRigProcessor::Reset()
{
	Solvers.Reset();
	GoalContainer.Empty();
	GoalBones.Reset();
	Skeleton.Reset();
	SetNeedsInitialized();
}

void UIKRigProcessor::SetNeedsInitialized()
{
	bInitialized = false;
	bTriedToInitialize = false;
};

#if WITH_EDITOR

void UIKRigProcessor::CopyAllInputsFromSourceAssetAtRuntime(const UIKRigDefinition* SourceAsset)
{
	check(SourceAsset)
	
	// copy goal settings
	const TArray<UIKRigEffectorGoal*>& AssetGoals =  SourceAsset->GetGoalArray();
	for (const UIKRigEffectorGoal* AssetGoal : AssetGoals)
	{
		SetIKGoal(AssetGoal);
	}

	// copy solver settings
	const TArray<UIKRigSolver*>& AssetSolvers = SourceAsset->GetSolverArray();
	check(Solvers.Num() == AssetSolvers.Num()); // if number of solvers has been changed, processor should have been reinitialized
	for (int32 SolverIndex=0; SolverIndex<Solvers.Num(); ++SolverIndex)
	{
		Solvers[SolverIndex]->SetEnabled(AssetSolvers[SolverIndex]->IsEnabled());
		Solvers[SolverIndex]->UpdateSolverSettings(AssetSolvers[SolverIndex]);
	}
}

#endif

const FIKRigGoalContainer& UIKRigProcessor::GetGoalContainer() const
{
	check(bInitialized);
	return GoalContainer;
}

FIKRigSkeleton& UIKRigProcessor::GetSkeleton()
{
	check(bInitialized);
	return Skeleton;
}

void UIKRigProcessor::ResolveFinalGoalTransforms(const FTransform& WorldToComponent)
{
	for (FIKRigGoal& Goal : GoalContainer.Goals)
	{
		if (!GoalBones.Contains(Goal.Name))
		{
			// user is changing goals after initialization
			// not necessarily a bad thing, but new goal names won't work until re-init
			continue;
		}

		const FGoalBone& GoalBone = GoalBones[Goal.Name];
		const FTransform& InputPoseBoneTransform = Skeleton.CurrentPoseGlobal[GoalBone.BoneIndex];

		FVector ComponentSpaceGoalPosition = Goal.Position;
		FQuat ComponentSpaceGoalRotation = Goal.Rotation.Quaternion();

		// put goal POSITION in Component Space
		switch (Goal.PositionSpace)
		{
		case EIKRigGoalSpace::Additive:
			// add position offset to bone position
			ComponentSpaceGoalPosition = Skeleton.CurrentPoseGlobal[GoalBone.BoneIndex].GetLocation() + Goal.Position;
			break;
		case EIKRigGoalSpace::Component:
			// was already supplied in Component Space
			break;
		case EIKRigGoalSpace::World:
			// convert from World Space to Component Space
			ComponentSpaceGoalPosition = WorldToComponent.TransformPosition(Goal.Position);
			break;
		default:
			checkNoEntry();
			break;
		}
		
		// put goal ROTATION in Component Space
		switch (Goal.RotationSpace)
		{
		case EIKRigGoalSpace::Additive:
			// add rotation offset to bone rotation
			ComponentSpaceGoalRotation = Goal.Rotation.Quaternion() * Skeleton.CurrentPoseGlobal[GoalBone.BoneIndex].GetRotation();
			break;
		case EIKRigGoalSpace::Component:
			// was already supplied in Component Space
			break;
		case EIKRigGoalSpace::World:
			// convert from World Space to Component Space
			ComponentSpaceGoalRotation = WorldToComponent.TransformRotation(Goal.Rotation.Quaternion());
			break;
		default:
			checkNoEntry();
			break;
		}

		// blend by alpha from the input pose, to the supplied goal transform
		// when Alpha is 0, the goal transform matches the bone transform at the input pose.
		// when Alpha is 1, the goal transform is left fully intact
		Goal.FinalBlendedPosition = FMath::Lerp(
            InputPoseBoneTransform.GetTranslation(),
            ComponentSpaceGoalPosition,
            Goal.PositionAlpha);
		
		Goal.FinalBlendedRotation = FQuat::FastLerp(
            InputPoseBoneTransform.GetRotation(),
            ComponentSpaceGoalRotation,
            Goal.RotationAlpha);
	}
}
