// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IOptimusNodeGraphCollectionOwner.h"
#include "OptimusCoreNotify.h"
#include "OptimusDataType.h"

#include "CoreMinimal.h"
#include "ComputeFramework/ComputeGraph.h"
#include "Interfaces/Interface_PreviewMeshProvider.h"
#include "Logging/TokenizedMessage.h"
#include "Types/OptimusType_ShaderText.h"

#include "OptimusDeformer.generated.h"

class UComputeDataProvider;
class USkeletalMesh;
class UOptimusActionStack;
class UOptimusDeformer;
class UOptimusResourceDescription;
class UOptimusVariableDescription;
class UOptimusNode_CustomComputeKernel;
enum class EOptimusDiagnosticLevel : uint8;


USTRUCT()
struct FOptimus_ShaderParameterBinding
{
	GENERATED_BODY()
	
	UPROPERTY()
	TObjectPtr<const UOptimusNode> ValueNode = nullptr;
	
	UPROPERTY()
	int32 KernelIndex = INDEX_NONE;
	
	UPROPERTY()
	int32 ParameterIndex = INDEX_NONE;
};


DECLARE_MULTICAST_DELEGATE_OneParam(FOptimusCompileBegin, UOptimusDeformer *);
DECLARE_MULTICAST_DELEGATE_OneParam(FOptimusCompileEnd, UOptimusDeformer *);
DECLARE_MULTICAST_DELEGATE_OneParam(FOptimusGraphCompileMessageDelegate, const TSharedRef<FTokenizedMessage>&);


/**
  * A Deformer Graph is an asset that is used to create and control custom deformations on 
  * skeletal meshes.
  */
UCLASS()
class OPTIMUSDEVELOPER_API UOptimusDeformer :
	public UComputeGraph,
	public IInterface_PreviewMeshProvider,
	public IOptimusNodeGraphCollectionOwner
{
	GENERATED_BODY()

public:
	UOptimusDeformer();

	UOptimusActionStack *GetActionStack() const { return ActionStack; }

	/// Add a setup graph. This graph is executed once when the deformer is first run from a
	/// mesh component. If the graph already exists, this function does nothing and returns 
	/// nullptr.
	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	UOptimusNodeGraph* AddSetupGraph();

	/// Add a trigger graph. This graph will be scheduled to execute on next tick, prior to the
	/// update graph being executed, after being triggered from a blueprint.
	/// @param InName The name to give the graph. The name "Setup" cannot be used, since it's a
	/// reserved name.
	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	UOptimusNodeGraph* AddTriggerGraph(const FString &InName);

	/// Returns the update graph. The update graph will always exist, and there is only one.
	UOptimusNodeGraph* GetUpdateGraph() const;

	/** Remove a graph and delete it. */
	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	bool RemoveGraph(UOptimusNodeGraph* InGraph);

	// Variables
	UFUNCTION(BlueprintCallable, Category = OptimusVariables)
	UOptimusVariableDescription* AddVariable(
		FOptimusDataTypeRef InDataTypeRef,
	    FName InName = NAME_None
		);

	UFUNCTION(BlueprintCallable, Category = OptimusVariables)
	bool RemoveVariable(
	    UOptimusVariableDescription* InVariableDesc
		);

	UFUNCTION(BlueprintCallable, Category = OptimusVariables)
	bool RenameVariable(
	    UOptimusVariableDescription* InVariableDesc,
	    FName InNewName);

	UFUNCTION(BlueprintCallable, Category = OptimusVariables)
	const TArray<UOptimusVariableDescription*>& GetVariables() const { return VariableDescriptions; }

	
	UOptimusVariableDescription* ResolveVariable(
		FName InVariableName
		);

	/** Create a resource owned by this deformer but does not add it to the list of known
	  * resources. Call AddResource for that */
	UOptimusVariableDescription* CreateVariableDirect(
		FName InName
		);

	/** Adds a resource that was created by this deformer and is owned by it. */
	bool AddVariableDirect(
		UOptimusVariableDescription* InVariableDesc
		);

	bool RemoveVariableDirect(
		UOptimusVariableDescription* InVariableDesc
		);

	bool RenameVariableDirect(
	    UOptimusVariableDescription* InVariableDesc,
		FName InNewName
		);



	// Resources
	UFUNCTION(BlueprintCallable, Category = OptimusResources)
	UOptimusResourceDescription* AddResource(
		FOptimusDataTypeRef InDataTypeRef,
	    FName InName = NAME_None
		);

	UFUNCTION(BlueprintCallable, Category = OptimusResources)
	bool RemoveResource(
	    UOptimusResourceDescription* InResourceDesc
		);

	UFUNCTION(BlueprintCallable, Category = OptimusResources)
	bool RenameResource(
	    UOptimusResourceDescription* InResourceDesc,
	    FName InNewName);

	UFUNCTION(BlueprintCallable, Category = OptimusResources)
	const TArray<UOptimusResourceDescription*>& GetResources() const { return ResourceDescriptions; }

	
	UOptimusResourceDescription* ResolveResource(
		FName InResourceName
		);

	/** Create a resource owned by this deformer but does not add it to the list of known
	  * resources. Call AddResource for that */
	UOptimusResourceDescription* CreateResourceDirect(
		FName InName
		);

	/** Adds a resource that was created by this deformer and is owned by it. */
	bool AddResourceDirect(
		UOptimusResourceDescription* InResourceDesc
		);

	bool RemoveResourceDirect(
		UOptimusResourceDescription* InResourceDesc
		);

	bool RenameResourceDirect(
	    UOptimusResourceDescription* InResourceDesc,
		FName InNewName
		);


	/// Graph compilation
	bool Compile();

	/** Returns a multicast delegate that can be subscribed to listen for the start of compilation. */
	FOptimusCompileBegin& GetCompileBeginDelegate()  { return CompileBeginDelegate; }
	/** Returns a multicast delegate that can be subscribed to listen for the end of compilation but before shader compilation is complete. */
	FOptimusCompileEnd& GetCompileEndDelegate() { return CompileEndDelegate; }
	/** Returns a multicast delegate that can be subscribed to listen compilation results. Note that the shader compilation results are async and can be returned after the CompileEnd delegate. */
	FOptimusGraphCompileMessageDelegate& GetCompileMessageDelegate() { return CompileMessageDelegate; }

	/// UObject overrides
	void Serialize(FArchive& Ar) override;
	

	/// UComputeGraph overrides
	void GetKernelBindings(int32 InKernelIndex, TMap<int32, TArray<uint8>>& OutBindings) const override;
	void OnKernelCompilationComplete(int32 InKernelIndex, const TArray<FString>& InCompileErrors) override;

	/// IInterface_PreviewMeshProvider overrides
	void SetPreviewMesh(USkeletalMesh* PreviewMesh, bool bMarkAsDirty = true) override;

	/** Get the preview mesh for this asset */
	USkeletalMesh* GetPreviewMesh() const override;

	/// IOptimusNodeGraphCollectionOwner overrides
	FOptimusGlobalNotifyDelegate& GetNotifyDelegate() override { return GlobalNotifyDelegate; }
	UOptimusNodeGraph* ResolveGraphPath(const FString& InGraphPath) override;
	UOptimusNode* ResolveNodePath(const FString& InNodePath) override;
	UOptimusNodePin* ResolvePinPath(const FString& InPinPath) override;

	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	const TArray<UOptimusNodeGraph*> &GetGraphs() const override { return Graphs; }

	UOptimusNodeGraph* CreateGraph(
	    EOptimusNodeGraphType InType,
	    FName InName,
	    TOptional<int32> InInsertBefore) override;
	bool AddGraph(
	    UOptimusNodeGraph* InGraph,
		int32 InInsertBefore) override;
	bool RemoveGraph(
	    UOptimusNodeGraph* InGraph,
		bool bDeleteGraph) override;

	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	bool MoveGraph(
	    UOptimusNodeGraph* InGraph,
	    int32 InInsertBefore) override;

	UFUNCTION(BlueprintCallable, Category = OptimusNodeGraph)
	bool RenameGraph(
	    UOptimusNodeGraph* InGraph,
	    const FString& InNewName) override;

	
	UPROPERTY(EditAnywhere, Category=Preview)
	USkeletalMesh *Mesh = nullptr;

private:
	UOptimusNodeGraph* ResolveGraphPath(const FString& InPath, FString& OutRemainingPath);
	UOptimusNode* ResolveNodePath(const FString& InPath, FString& OutRemainingPath);
	
	void Notify(EOptimusGlobalNotifyType InNotifyType, UObject *InObject) const;

	FOptimusType_CompilerDiagnostic ProcessCompilationMessage(
		const UOptimusNode* InKernelNode,
		const FString& InMessage
		);

	UPROPERTY()
	TArray<TObjectPtr<UOptimusNodeGraph>> Graphs;

	UPROPERTY()
	TArray<TObjectPtr<UOptimusVariableDescription>> VariableDescriptions;

	UPROPERTY()
	TArray<TObjectPtr<UOptimusResourceDescription>> ResourceDescriptions;

	UPROPERTY(transient)
	TObjectPtr<UOptimusActionStack> ActionStack;

	// Lookup into Graphs array from the UComputeGraph kernel index. 
	UPROPERTY()
	TArray<int32> CompilingKernelToGraph;
	// Lookup into UOptimusNodeGraph::Nodes array from the UComputeGraph kernel index. 
	UPROPERTY()
	TArray<int32> CompilingKernelToNode;

	// List of parameter bindings and which value nodes they map to.
	UPROPERTY()
	TArray<FOptimus_ShaderParameterBinding> AllParameterBindings;

	
	FOptimusGlobalNotifyDelegate GlobalNotifyDelegate;

	FOptimusCompileBegin CompileBeginDelegate;
	
	FOptimusCompileEnd CompileEndDelegate;

	FOptimusGraphCompileMessageDelegate CompileMessageDelegate;

};
