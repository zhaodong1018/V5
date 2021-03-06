// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassCommonTypes.h"
#include "MassProcessor.h"
#include "IndexedHandle.h"
#include "MassLODTypes.h"
#include "Subsystems/WorldSubsystem.h"

#include "MassLODManager.generated.h"

class UMassLODManager;

/*
 * Base mass LOD processor to store common information for all LOD processors
 */
UCLASS(abstract)
class MASSLOD_API UMassProcessor_LODBase : public UMassProcessor
{
	GENERATED_BODY()

public:
	virtual void Initialize(UObject& Owner) override;

protected:
	UPROPERTY()
	UWorld* World = nullptr;

	UPROPERTY()
	UMassLODManager* LODManager = nullptr;
};

/*
 * Handle that lets you reference the concept of a viewer
 */
USTRUCT()
struct MASSLOD_API FMassViewerHandle : public FIndexedHandleBase
{
	GENERATED_BODY()

	friend class UMassLODManager;
};

USTRUCT()
struct FViewerInfo
{
	GENERATED_BODY()

	UPROPERTY(transient)
	APlayerController* PlayerController = nullptr;
	
	FName StreamingSourceName;

	FMassViewerHandle Handle;
	uint32 HashValue = 0;

	FVector Location;
	FRotator Rotation;
	float FOV = 90.0f;
	float AspectRatio = 16.0f / 9.0f;

	bool bEnabled = true;

	void Reset();

	bool IsLocal() const;
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnViewerAdded, FMassViewerHandle ViewerHandle, APlayerController* PlayerController, FName StreamingSourceName);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnViewerRemoved, FMassViewerHandle ViewerHandle, APlayerController* PlayerController, FName StreamingSourceName);

/*
 * Manager responsible to manage and synchronized available viewers
 */
UCLASS()
class MASSLOD_API UMassLODManager : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Checks the validity of a viewer handle */
	bool IsValidViewer(const FMassViewerHandle& ViewerHandle) const { return GetValidViewerIdx(ViewerHandle) != INDEX_NONE; }

	/** Returns the index of the viewer if valid, otherwise INDEX_NONE is return */
	int32 GetValidViewerIdx(const FMassViewerHandle& ViewerHandle) const;

	/** Returns the array of viewers */
	const TArray<FViewerInfo>& GetViewers() const { return Viewers; }

	/** Synchronize the viewers if not done this frame and returns the updated array */
	const TArray<FViewerInfo>& GetSynchronizedViewers();

	/** Returns viewer handle from the PlayerController pointer */
	FMassViewerHandle GetViewerHandleFromPlayerController(const APlayerController* PlayerController) const;

	/** Returns viewer handle from the streaming source name */
	FMassViewerHandle GetViewerHandleFromStreamingSource(const FName StreamingSourceName) const;

	/** Returns PlayerController pointer from the viewer handle */
	APlayerController* GetPlayerControllerFromViewerHandle(const FMassViewerHandle& ViewerHandle) const;

	/** Returns the delegate called when new viewer are added to the list */
	FOnViewerAdded& GetOnViewerAddedDelegate() { return OnViewerAddedDelegate;  }

	/** Returns the delegate called when viewer are removed from the list */
	FOnViewerRemoved& GetOnViewerRemovedDelegate() { return OnViewerRemovedDelegate; }

protected:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;

	/** Called at the start of the PrePhysics mass processing phase and calls SynchronizeViewers */ 
	void OnPrePhysicsPhaseStarted(float DeltaTime);

	/** Synchronizes the viewers from the engine PlayerController list */
	void SynchronizeViewers();

	/** Adds a viewer to the list and sends notification about addition */
	void AddViewer(APlayerController* PlayerController, FName StreamingSourceName = NAME_None);

	/** Removes a viewer to the list and send notification about removal */
	void RemoveViewer(const FMassViewerHandle& ViewerHandle);

	/** Returns the next new viewer serial number */
	uint32 GetNextViewerSerialNumber() { return ViewerSerialNumberCounter++; }

	/** Player controller EndPlay callback, removing viewers from the list */
	UFUNCTION()
	void OnPlayerControllerEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);

private:
	/** Removes a viewer to the list and send notification about removal */
	void RemoveViewerInternal(const FMassViewerHandle& ViewerHandle);

	/** The actual array of viewer's information*/
	UPROPERTY(Transient)
	TArray<FViewerInfo> Viewers;

	/** The map that do reverse look up to get ViewerHandle */
	UPROPERTY(Transient)
	TMap<uint32, FMassViewerHandle> ViewerMap;

	uint64 LastSynchronizedFrame = 0;

	/** Viewer serial number counter */
	uint32 ViewerSerialNumberCounter = 0;

	/** Free list of indices in the sparse viewer array */
	TArray<int32> ViewerFreeIndices;

	/** Delegates to notify anyone who needs to know about viewer changes */
	FOnViewerAdded OnViewerAddedDelegate;
	FOnViewerRemoved OnViewerRemovedDelegate;

};

