// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameFramework/Actor.h"
#include "Containers/Array.h"
#include "NavigationDataChunkActor.generated.h"

class UNavigationDataChunk;
class UNavigationSystemBase;

UCLASS(NotPlaceable)
class ENGINE_API ANavigationDataChunkActor : public AActor
{
	GENERATED_UCLASS_BODY()

public:
#if WITH_EDITOR
	//~ Begin UObject Interface
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	//~ End UObject Interface

	void AddNavigationDataChunkInEditor(const UNavigationSystemBase& NavSys);
#endif // WITH_EDITOR

	const TArray<UNavigationDataChunk*>& GetNavDataChunk() const { return NavDataChunks; }
	TArray<UNavigationDataChunk*>& GetMutableNavDataChunk() { return NavDataChunks; }

	void CollectNavData(const FBox& QueryBounds, FBox& OutTilesBounds);

#if WITH_EDITOR
	void SetDataChunkActorBounds(const FBox& InBounds);

	//~ Begin AActor Interface.
	virtual EActorGridPlacement GetDefaultGridPlacement() const override;
	virtual FBox GetStreamingBounds() const override;
	//~ End AActor Interface.
#endif // WITH_EDITOR
	
protected:
	//~ Begin AActor Interface.
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetActorBounds(bool bOnlyCollidingComponents, FVector& OutOrigin, FVector& OutBoxExtent, bool bIncludeFromChildActors) const override;
	//~ End AActor Interface.

	void AddNavigationDataChunkToWorld();
	void RemoveNavigationDataChunkFromWorld();
	void Log(const TCHAR* FunctionName) const;

	UPROPERTY()
	TArray<TObjectPtr<UNavigationDataChunk>> NavDataChunks;

	UPROPERTY()
	FBox DataChunkActorBounds;
};
