// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Object.h"
#include "VirtualizationFilterSettings.generated.h"

/** 
 * Settings to use when determining if a package should be included/excluded from the
 * content virtualization process.
 */ 
UCLASS(config = Engine)
class UVirtualizationFilterSettings : public UObject
{
	GENERATED_UCLASS_BODY()

	/**
	 * A list of package paths that we should exclude from virtualization. 
	 * Each path can either be a full package path to a specific package or it can be a
	 * path to a directory, in which case all packages under that directory (and subdirectories)
	 * will be excluded.
	 */
	UPROPERTY(Config)
	TArray<FString> ExcludePackagePaths;
};
