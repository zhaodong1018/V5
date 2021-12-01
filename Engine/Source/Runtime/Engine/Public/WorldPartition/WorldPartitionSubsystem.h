// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "Templates/SubclassOf.h"
#include "Misc/Optional.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "WorldPartitionSubsystem.generated.h"

class UWorldPartition;
class UWorldPartitionEditorCell;
class FWorldPartitionActorDesc;

enum class EWorldPartitionRuntimeCellState : uint8;

/**
 * UWorldPartitionSubsystem
 */

UCLASS()
class ENGINE_API UWorldPartitionSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UWorldPartitionSubsystem();

	//~ Begin USubsystem Interface.
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface.

	//~ Begin UWorldSubsystem Interface.
	virtual void PostInitialize() override;
	virtual void UpdateStreamingState() override;
	//~ End UWorldSubsystem Interface.

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaSeconds) override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual ETickableTickType GetTickableTickType() const override;
	virtual TStatId GetStatId() const override;
	//~End FTickableGameObject

	DECLARE_MULTICAST_DELEGATE_OneParam(FWorldPartitionRegisterDelegate, UWorldPartition*);
	FWorldPartitionRegisterDelegate OnWorldPartitionRegistered;
	FWorldPartitionRegisterDelegate OnWorldPartitionUnregistered;

	UFUNCTION(BlueprintCallable, Category = Streaming)
	bool IsStreamingCompleted(EWorldPartitionRuntimeCellState QueryState, const TArray<FWorldPartitionStreamingQuerySource>& QuerySources, bool bExactState) const;

	void DumpStreamingSources(FOutputDevice& OutputDevice) const;

private:
#if WITH_EDITOR
	bool IsRunningConvertWorldPartitionCommandlet() const;
#endif

	UWorldPartition* GetWorldPartition();
	const UWorldPartition* GetWorldPartition() const;
	void Draw(class UCanvas* Canvas, class APlayerController* PC);
	friend class UWorldPartition;

	FDelegateHandle	DrawHandle;

	// GC backup values
	int32 LevelStreamingContinuouslyIncrementalGCWhileLevelsPendingPurge;
	int32 LevelStreamingForceGCAfterLevelStreamedOut;
};
