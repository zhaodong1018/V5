// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "WorldPartition/IWorldPartitionEditorModule.h"

class FTabManager;
class FLayoutExtender;
class SDockTab;
class FSpawnTabArgs;


/**
 * The module holding all of the UI related pieces for SubLevels management
 */
class FWorldPartitionEditorModule : public IWorldPartitionEditorModule
{
public:
	/**
	 * Called right after the module DLL has been loaded and the module object has been created
	 */
	virtual void StartupModule() override;

	/**
	 * Called before the module is unloaded, right before the module object is destroyed.
	 */
	virtual void ShutdownModule() override;
	
	/**
	 * Creates a world partition widget
	 */
	virtual TSharedRef<class SWidget> CreateWorldPartitionEditor();

	/**
	 * 
	 */
	virtual bool ConvertMap(const FString& InLongPackageName) override;

	/**
	 *
	 */
	virtual FWorldPartitionCreated& OnWorldPartitionCreated() override { return WorldPartitionCreatedEvent; }

private:
	/** Called when the level editors map changes. We will determine if the new map is a valid world partition world and close world partition tabs if not */
	void OnMapChanged(uint32 MapFlags);

	/** Registers world partition tabs spawners with the level editor */
	void RegisterWorldPartitionTabs(TSharedPtr<FTabManager> InTabManager);

	/** Inserts world partition tabs into the level editor layout */
	void RegisterWorldPartitionLayout(FLayoutExtender& Extender);
	
	/** Determines if ia world partition tab can be spawned */	
	bool CanSpawnWorldPartitionTab(const FSpawnTabArgs& Args);

	/** Spawns the world partition tab */
	TSharedRef<SDockTab> SpawnWorldPartitionTab(const FSpawnTabArgs& Args);

	void UpdateTabPermissions(TSharedPtr<FTabManager> InTabManager);
private:
	void OnConvertMap();

	FDelegateHandle LevelEditorExtenderDelegateHandle;

	TSharedPtr<class FHLODLayerAssetTypeActions> HLODLayerAssetTypeActions;

	TWeakPtr<SDockTab> WorldPartitionTab;

	FWorldPartitionCreated WorldPartitionCreatedEvent;
};
