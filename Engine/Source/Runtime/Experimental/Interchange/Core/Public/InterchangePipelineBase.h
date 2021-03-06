// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Nodes/InterchangeBaseNodeContainer.h"
#include "InterchangeResultsContainer.h"
#include "InterchangeSourceData.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "InterchangePipelineBase.generated.h"

UENUM(BlueprintType)
enum class EInterchangePipelineTask : uint8
{
	PreFactoryImport,
	PostFactoryImport,
	Export
};

UCLASS(BlueprintType, Blueprintable, Experimental)
class INTERCHANGECORE_API UInterchangePipelineBase : public UObject
{
	GENERATED_BODY()

public:

	/**
	 * Non virtual helper to allow blueprint to implement event base function to implement a pre import pipeline,
	 * It is call after the translation and before we parse the graph to call the factory. This is where factory node should be created
	 * by the pipeline. Each factory node should be send to a a interchange factory to create an unreal asset.
	 * @note - the Interchange manager is calling this function not the virtual one that is call by the default implementation.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interchange | Pipeline")
	bool ScriptedExecutePreImportPipeline(UInterchangeBaseNodeContainer* BaseNodeContainer, const TArray<UInterchangeSourceData*>& SourceDatas);
	/** The default implementation (call if the blueprint do not have any implementation) will call the virtual ExecuteImportPipeline */
	bool ScriptedExecutePreImportPipeline_Implementation(UInterchangeBaseNodeContainer* BaseNodeContainer, const TArray<UInterchangeSourceData*>& SourceDatas)
	{
		//By default we call the virtual import pipeline execution
		return ExecutePreImportPipeline(BaseNodeContainer, SourceDatas);
	}

	/**
	 * Non virtual helper to allow blueprint to implement event base function to implement a post import pipeline,
	 * It is call after we completely import an asset. PostEditChange is already called. Some assets uses asynchronous build.
	 * This can be useful if you need build data of an asset to finish the setup of another asset.
	 * @example - PhysicsAsset need skeletal mesh render data to be build properly.
	 * @note - the Interchange manager is calling this function not the virtual one that is call by the default implementation.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interchange | Pipeline")
	bool ScriptedExecutePostImportPipeline(const UInterchangeBaseNodeContainer* BaseNodeContainer, const FString& NodeKey, UObject* CreatedAsset, bool bIsAReimport);
	/** The default implementation (call if the blueprint do not have any implementation) will call the virtual ExecuteImportPipeline */
	bool ScriptedExecutePostImportPipeline_Implementation(const UInterchangeBaseNodeContainer* BaseNodeContainer, const FString& NodeKey, UObject* CreatedAsset, bool bIsAReimport)
	{
		//By default we call the virtual import pipeline execution
		return ExecutePostImportPipeline(BaseNodeContainer, NodeKey, CreatedAsset, bIsAReimport);
	}

	/**
	 * Non virtual helper to allow blueprint to implement event base function let the interchange know if it can run asynchronously.
	 * the Interchange manager is calling this function not the virtual one that is call by the default implementation.
	 */
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Interchange | Pipeline")
	bool ScriptedExecuteExportPipeline(UInterchangeBaseNodeContainer* BaseNodeContainer);

	/** The default implementation (call if the blueprint do not have any implementation) will call the virtual ExecuteExportPipeline */
	bool ScriptedExecuteExportPipeline_Implementation(UInterchangeBaseNodeContainer* BaseNodeContainer)
	{
		return ExecuteExportPipeline(BaseNodeContainer);
	}

	/**
	 * Non virtual helper to allow blueprint to implement event base function let the interchange know if it can run asynchronously.
	 * the Interchange manager is calling this function not the virtual one that is call by the default implementation.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interchange | Pipeline")
	bool ScriptedCanExecuteOnAnyThread(EInterchangePipelineTask PipelineTask);

	/** The default implementation (call if the blueprint do not have any implementation) will call the virtual CanExecuteAsync */
	bool ScriptedCanExecuteOnAnyThread_Implementation(EInterchangePipelineTask PipelineTask)
	{
		return CanExecuteOnAnyThread(PipelineTask);
	}

	/**
	 * Non scripted class should return false here, we have the default to true because scripted class cannot override
	 * this function since it can be call in a asynchronous thread, which python cannot be executed.
	 *
	 * We cannot call ScriptedCanExecuteOnAnyThread for a scripted python pipeline from the task parsing async thread.
	 * This function allow us to not call it and force the ScriptedExecutePostImportPipeline to execute on the game thread.
	 */
	virtual bool IsScripted()
	{
		return true;
	}

	void LoadSettings(const FName PipelineStackName);

	void SaveSettings(const FName PipelineStackName);

	virtual void PreDialogCleanup(const FName PipelineStackName) {};
	
	/**
	 * This function is used to add the given message object directly into the results for this operation.
	 */
	template <typename T>
	T* AddMessage() const
	{
		check(Results != nullptr);
		T* Item = Results->Add<T>();
		return Item;
	}

	void AddMessage(UInterchangeResult* Item) const
	{
		check(Results != nullptr);
		Results->Add(Item);
	}
	
	void SetResultsContainer(UInterchangeResultsContainer* InResults)
	{
		Results = InResults;
	}


protected:

	/**
	 * This function can modify the BaseNodeContainer to create a pipeline that will set the graph and the nodes options has it want it to be imported by the factories
	 * The interchange manager is not calling this function directly. It is calling the blueprint native event in case this object is a blueprint derive object.
	 * By default the scripted implementation is calling this virtual pipeline.
	 */
	virtual bool ExecutePreImportPipeline(UInterchangeBaseNodeContainer* BaseNodeContainer, const TArray<UInterchangeSourceData*>& SourceDatas)
	{
		return false;
	}

	/**
	 * This function can read the node data and apply some change to the imported asset. This is call after the factory create the asset and configure the asset properties.
	 * The interchange manager is not calling this function directly. It is calling the blueprint native event in case this object is a blueprint derive object.
	 * By default the scripted implementation is calling this virtual pipeline.
	 */
	virtual bool ExecutePostImportPipeline(const UInterchangeBaseNodeContainer* BaseNodeContainer, const FString& NodeKey, UObject* CreatedAsset, bool bIsAReimport)
	{
		return false;
	}

	/**
	 * This function tell the interchange manager if we can execute this pipeline in async mode. If it return false, the ScriptedExecuteImportPipeline
	 * will be call on the main thread (GameThread), if true it will be run in a background thread and possibly in parallel. If there is multiple
	 * import process in same time.
	 *
	 */
	virtual bool CanExecuteOnAnyThread(EInterchangePipelineTask PipelineTask)
	{
		return true;
	}

	/** This function can modify the BaseNodeContainer to create a pipeline that will set/validate the graph nodes hierarchy and options.*/
	virtual bool ExecuteExportPipeline(UInterchangeBaseNodeContainer* BaseNodeContainer)
	{
		return false;
	}


	UPROPERTY()
	TObjectPtr<UInterchangeResultsContainer> Results;
};
