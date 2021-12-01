// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * LevelStreamingDynamic
 *
 * Dynamically controlled streaming implementation.
 *
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Templates/SubclassOf.h"
#include "Engine/LevelStreaming.h"
#include "LevelStreamingDynamic.generated.h"

UCLASS(BlueprintType)
class ENGINE_API ULevelStreamingDynamic : public ULevelStreaming
{
	GENERATED_UCLASS_BODY()

	/** Whether the level should be loaded at startup																			*/
	UPROPERTY(Category=LevelStreaming, EditAnywhere)
	uint32 bInitiallyLoaded:1;

	/** Whether the level should be visible at startup if it is loaded 															*/
	UPROPERTY(Category=LevelStreaming, EditAnywhere)
	uint32 bInitiallyVisible:1;
	
	/**  
 	* Stream in a level with a specific location and rotation. You can create multiple instances of the same level!
 	*
 	* The level to be loaded does not have to be in the persistent map's Levels list, however to ensure that the .umap does get
 	* packaged, please be sure to include the .umap in your Packaging Settings:
 	*
 	*   Project Settings -> Packaging -> List of Maps to Include in a Packaged Build (you may have to show advanced or type in filter)
 	* 
 	* @param LevelName - Level package name to load, ex: /Game/Maps/MyMapName, specifying short name like MyMapName will force very slow search on disk
 	* @param Location - World space location where the level should be spawned
 	* @param Rotation - World space rotation for rotating the entire level
	* @param bOutSuccess - Whether operation was successful (map was found and added to the sub-levels list)
	* @param OptionalLevelNameOverride - If set, the loaded level package have this name, which is used by other functions like UnloadStreamLevel. Note this is necessary for server and client networking because the level must have the same name on both.
	* @param OptionalLevelStreamingClass - If set, the level streaming class will be used instead of ULevelStreamingDynamic
 	* @return Streaming level object for a level instance
 	*/ 
 	UFUNCTION(BlueprintCallable, Category = LevelStreaming, meta=(DisplayName = "Load Level Instance (by Name)", WorldContext="WorldContextObject"))
 	static ULevelStreamingDynamic* LoadLevelInstance(UObject* WorldContextObject, FString LevelName, FVector Location, FRotator Rotation, bool& bOutSuccess, const FString& OptionalLevelNameOverride = TEXT(""), TSubclassOf<ULevelStreamingDynamic> OptionalLevelStreamingClass = nullptr);

 	UFUNCTION(BlueprintCallable, Category = LevelStreaming, meta=(DisplayName = "Load Level Instance (by Object Reference)", WorldContext="WorldContextObject"))
 	static ULevelStreamingDynamic* LoadLevelInstanceBySoftObjectPtr(UObject* WorldContextObject, TSoftObjectPtr<UWorld> Level, FVector Location, FRotator Rotation, bool& bOutSuccess, const FString& OptionalLevelNameOverride = TEXT(""), TSubclassOf<ULevelStreamingDynamic> OptionalLevelStreamingClass = nullptr);
 	
	static ULevelStreamingDynamic* LoadLevelInstanceBySoftObjectPtr(UObject* WorldContextObject, TSoftObjectPtr<UWorld> Level, const FTransform LevelTransform, bool& bOutSuccess, const FString& OptionalLevelNameOverride = TEXT(""), TSubclassOf<ULevelStreamingDynamic> OptionalLevelStreamingClass = nullptr);

	//~ Begin UObject Interface
	virtual void PostLoad() override;
	//~ End UObject Interface

	//~ Begin ULevelStreaming Interface
	virtual bool ShouldBeLoaded() const override { return bShouldBeLoaded; }
	//~ End ULevelStreaming Interface

	virtual void SetShouldBeLoaded(bool bShouldBeLoaded) override;

private:

	// Counter used by LoadLevelInstance to create unique level names
	static int32 UniqueLevelInstanceId;

 	static ULevelStreamingDynamic* LoadLevelInstance_Internal(UWorld* World, const FString& LongPackageName, const FTransform LevelTransform, bool& bOutSuccess, const FString& OptionalLevelNameOverride, TSubclassOf<ULevelStreamingDynamic> OptionalLevelStreamingClass);

};

UE_DEPRECATED(4.21, "ULevelStreamingKismet has been renamed to ULevelStreamingDynamic")
typedef ULevelStreamingDynamic ULevelStreamingKismet;