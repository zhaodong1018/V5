// Copyright Epic Games, Inc. All Rights Reserved.

#include "MLDeformerInstance.h"
#include "MLDeformer.h"
#include "MLDeformerAsset.h"
#include "MLDeformerInputInfo.h"
#include "Components/SkeletalMeshComponent.h"
#include "NeuralNetwork.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

void FMLDeformerInstance::Init(UMLDeformerAsset* Asset, USkeletalMeshComponent* SkelMeshComponent)
{
	DeformerAsset = Asset;
	SkeletalMeshComponent = SkelMeshComponent;

	if (SkelMeshComponent == nullptr || Asset == nullptr)
	{
		AssetBonesToSkelMeshMappings.Empty();
		return;
	}

	USkeletalMesh* SkelMesh = SkelMeshComponent->SkeletalMesh;
	if (SkelMesh)
	{
		// Init the bone mapping table.
		const FMLDeformerInputInfo& InputInfo = DeformerAsset->GetInputInfo();
		const int32 NumAssetBones = InputInfo.GetNumBones();
		AssetBonesToSkelMeshMappings.Reset();
		AssetBonesToSkelMeshMappings.AddUninitialized(NumAssetBones);

		// For each bone in the deformer asset, find the matching bone index inside the skeletal mesh component.
		for (int32 Index = 0; Index < NumAssetBones; ++Index)
		{
			const FName BoneName = InputInfo.GetBoneName(Index);
			const int32 SkelMeshBoneIndex = SkeletalMeshComponent->GetBoneIndex(BoneName);
			AssetBonesToSkelMeshMappings[Index] = SkelMeshBoneIndex;
		}
	}

	// Perform a compatibility check.
	bIsCompatible = SkelMesh && CheckCompatibility(SkelMeshComponent, true).IsEmpty();
}

FString FMLDeformerInstance::CheckCompatibility(USkeletalMeshComponent* InSkelMeshComponent, bool LogIssues) const
{
	// If we're not compatible, generate a compatibility string.
	USkeletalMesh* SkelMesh = InSkelMeshComponent ? InSkelMeshComponent->SkeletalMesh.Get() : nullptr;
	if (SkelMesh && !DeformerAsset->GetInputInfo().IsCompatible(SkelMesh))
	{
		const FString ErrorText = DeformerAsset->GetInputInfo().GenerateCompatibilityErrorString(SkelMesh);
		check(!ErrorText.IsEmpty());
		if (LogIssues)
		{
			UE_LOG(LogMLDeformer, Error, TEXT("ML Deformer '%s' isn't compatible with Skeletal Mesh '%s'.\nReason(s):\n%s"), 
				*DeformerAsset->GetName(), 
				*SkelMesh->GetName(), 
				*ErrorText);
		}
		return ErrorText;
	}

	// No errors.
	return FString();
}

int64 FMLDeformerInstance::SetBoneTransforms(float* OutputBuffer, int64 OutputBufferSize, int64 StartIndex)
{
	// Extract the component space bone transforms from the component.
	// Write the output transforms into the BoneTransforms array.
	BoneTransforms = SkeletalMeshComponent->GetBoneSpaceTransforms();

	// Write the transforms into the output buffer.
	const FMLDeformerInputInfo& InputInfo = DeformerAsset->GetInputInfo();
	const int32 AssetNumBones = InputInfo.GetNumBones();
	int64 Index = StartIndex;
	check((Index + AssetNumBones * 4) <= OutputBufferSize); // Make sure we don't write past the OutputBuffer.
	for (int32 BoneIndex = 0; BoneIndex < AssetNumBones; ++BoneIndex)
	{
		const int32 SkelMeshBoneIndex = AssetBonesToSkelMeshMappings[BoneIndex];
		if (SkelMeshBoneIndex != INDEX_NONE)
		{
			const FQuat& Rotation = BoneTransforms[SkelMeshBoneIndex].GetRotation();
			OutputBuffer[Index++] = Rotation.X;
			OutputBuffer[Index++] = Rotation.Y;
			OutputBuffer[Index++] = Rotation.Z;
			OutputBuffer[Index++] = Rotation.W;
		}
		else
		{
			// Use an identity quaternion as input.
			OutputBuffer[Index++] = 0.0f;
			OutputBuffer[Index++] = 0.0f;
			OutputBuffer[Index++] = 0.0f;
			OutputBuffer[Index++] = 1.0f;
		}
	}

	return Index;
}

int64 FMLDeformerInstance::SetCurveValues(float* OutputBuffer, int64 OutputBufferSize, int64 StartIndex)
{
	const FMLDeformerInputInfo& InputInfo = DeformerAsset->GetInputInfo();

	// Write the weights into the output buffer.
	int64 Index = StartIndex;
	const int32 AssetNumCurves = InputInfo.GetNumCurves();
	check((Index + AssetNumCurves) <= OutputBufferSize); // Make sure we don't write past the OutputBuffer.

	// Write the curve weights to the output buffer.
	UAnimInstance* AnimInstance = SkeletalMeshComponent->GetAnimInstance();
	if (AnimInstance)
	{
		for (int32 CurveIndex = 0; CurveIndex < AssetNumCurves; ++CurveIndex)
		{
			const FName CurveName = InputInfo.GetCurveName(CurveIndex);
			const float CurveValue = AnimInstance->GetCurveValue(CurveName);	// Outputs 0.0 when not found.
			OutputBuffer[Index++] = CurveValue;
		}
	}
	else
	{
		for (int32 CurveIndex = 0; CurveIndex < AssetNumCurves; ++CurveIndex)
		{
			OutputBuffer[Index++] = 0.0f;
		}
	}

	return Index;
}

void FMLDeformerInstance::SetNeuralNetworkInputValues(float* InputData, int64 NumInputFloats)
{
	check(SkeletalMeshComponent);

	// Feed data to the network inputs.
	int64 BufferOffset = 0;
	BufferOffset = SetBoneTransforms(InputData, NumInputFloats, BufferOffset);
	BufferOffset = SetCurveValues(InputData, NumInputFloats, BufferOffset);
	check(BufferOffset == NumInputFloats);
}

void FMLDeformerInstance::Update()
{
	// Some safety checks.
	if (DeformerAsset == nullptr || 
		SkeletalMeshComponent == nullptr || 
		SkeletalMeshComponent->SkeletalMesh == nullptr || 
		!bIsCompatible)
	{
		return;
	}

	// Get the network and make sure it's loaded.
	UNeuralNetwork* NeuralNetwork = DeformerAsset->GetInferenceNeuralNetwork();
	if (NeuralNetwork == nullptr || !NeuralNetwork->IsLoaded())
	{
		return;
	}

	// We only support GPU processing of the neural network at the moment.
	check(NeuralNetwork->GetInputDeviceType() == ENeuralDeviceType::CPU);
	check(NeuralNetwork->GetDeviceType() == ENeuralDeviceType::GPU);
	check(NeuralNetwork->GetOutputDeviceType() == ENeuralDeviceType::GPU);

	// If the neural network expects a different number of inputs, do nothing.
	const int64 NumNeuralNetInputs = NeuralNetwork->GetInputTensor().Num();
	const int64 NumDeformerAssetInputs = static_cast<int64>(DeformerAsset->GetInputInfo().CalcNumNeuralNetInputs());
	ensureMsgf(
		NumNeuralNetInputs == NumDeformerAssetInputs, 
		TEXT("Neural network %s expects %d inputs, while the deformer asset expects to feed %d inputs."), 
		*DeformerAsset->GetName(), 
		NumNeuralNetInputs, 
		NumDeformerAssetInputs);
	if (NumNeuralNetInputs != NumDeformerAssetInputs)
	{
		return;
	}

	// Update and write the input values directly into the input tensor.
	float* InputDataPointer = (float*)NeuralNetwork->GetInputDataPointerMutable();
	SetNeuralNetworkInputValues(InputDataPointer, NumNeuralNetInputs);

	// Output deltas will be available on GPU for the DeformerGraph via UMLDeformerDataProvider.
	// So this does not actually modify our mesh directly. It just outputs the deltas, which are then used inside a deformer graph later on, accessible
	// through the data provider class mentioned above.
	// We pass ENeuralDeviceType::CPU as parameter because the inputs come from the CPU.
	// We could later switch to Asynchronous processing if we want to run this in a background thread.
	NeuralNetwork->Run();
}
