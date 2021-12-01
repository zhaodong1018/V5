// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "OptimusComputeDataInterface.h"
#include "ComputeFramework/ComputeDataProvider.h"
#include "DataInterfaceSkinCacheWrite.generated.h"

class FGPUSkinCache;
class FSkeletalMeshObject;
class USkeletalMeshComponent;

/** Compute Framework Data Interface for reading skeletal mesh. */
UCLASS(Category = ComputeFramework)
class OPTIMUSCORE_API USkeletalMeshSkinCacheDataInterface : public UOptimusComputeDataInterface
{
	GENERATED_BODY()

public:
	//~ Begin UOptimusComputeDataInterface Interface
	FString GetDisplayName() const override;
	TArray<FOptimusCDIPinDefinition> GetPinDefinitions() const override;
	//~ End UOptimusComputeDataInterface Interface
	
	//~ Begin UComputeDataInterface Interface
	void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	void GetSupportedOutputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	void GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& OutBuilder) const override;
	void GetHLSL(FString& OutHLSL) const override;
	void GetSourceTypes(TArray<UClass*>& OutSourceTypes) const override;
	UComputeDataProvider* CreateDataProvider(UObject* InOuter, TArrayView< TObjectPtr<UObject> > InSourceObjects) const override;
	//~ End UComputeDataInterface Interface
};

/** Compute Framework Data Provider for reading skeletal mesh. */
UCLASS(BlueprintType, editinlinenew, Category = ComputeFramework)
class OPTIMUSCORE_API USkeletalMeshSkinCacheDataProvider : public UComputeDataProvider
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Binding)
	TObjectPtr<USkeletalMeshComponent> SkeletalMesh = nullptr;

	//~ Begin UComputeDataProvider Interface
	bool IsValid() const override;
	FComputeDataProviderRenderProxy* GetRenderProxy() override;
	//~ End UComputeDataProvider Interface
};

class FSkeletalMeshSkinCacheDataProviderProxy : public FComputeDataProviderRenderProxy
{
public:
	FSkeletalMeshSkinCacheDataProviderProxy(USkeletalMeshComponent* SkeletalMeshComponent);

	//~ Begin FComputeDataProviderRenderProxy Interface
	int32 GetInvocationCount() const override;
	FIntVector GetDispatchDim(int32 InvocationIndex, FIntVector GroupDim) const override;
	void GetBindings(int32 InvocationIndex, TCHAR const* UID, FBindings& OutBindings) const override;
	//~ End FComputeDataProviderRenderProxy Interface

private:
	FSkeletalMeshObject* SkeletalMeshObject;
	FGPUSkinCache* GPUSkinCache;
};
