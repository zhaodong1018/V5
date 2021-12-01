// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/WorldPartitionNavigationDataBuilder.h"

#include "CoreMinimal.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "StaticMeshCompiler.h"
#include "Logging/LogMacros.h"
#include "UObject/SavePackage.h"
#include "Commandlets/Commandlet.h"

#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/NavigationData/NavigationDataChunkActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldPartitionNavigationDataBuilder, Log, All);

UWorldPartitionNavigationDataBuilder::UWorldPartitionNavigationDataBuilder(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Size of loaded cell. Set as big as your hardware can afford.
	// @todo: move to a config file.
	IterativeCellSize = 204800;
	
	// Extra padding around loaded cell.
	// @todo: set value programatically.
	IterativeCellOverlapSize = 2000 + 51200;	// tile size + data chunk actor half size (because chunks are currently centered)
}

bool UWorldPartitionNavigationDataBuilder::PreRun(UWorld* World, FPackageSourceControlHelper& PackageHelper)
{
	TArray<FString> Tokens, Switches;
	UCommandlet::ParseCommandLine(FCommandLine::Get(), Tokens, Switches);

	bCleanBuilderPackages = Switches.Contains(TEXT("CleanPackages"));

	return true;
}

bool UWorldPartitionNavigationDataBuilder::RunInternal(UWorld* World, const FCellInfo& InCellInfo, FPackageSourceControlHelper& PackageHelper)
{
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT(" "));
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("============================================================================================================"));
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("RunInternal"));
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Bounds %s ."), *InCellInfo.Bounds.ToString());
	
	UWorldPartitionSubsystem* WorldPartitionSubsystem = World->GetSubsystem<UWorldPartitionSubsystem>();
	check(WorldPartitionSubsystem);

	UWorldPartition* WorldPartition = World->GetWorldPartition();
	check(WorldPartition);
	
	TSet<UPackage*> NavigationDataChunkActorPackages;
	TSet<UPackage*> PackagesToClean;

	// Gather all packages before any navigation data chunk actors are deleted
	for (TActorIterator<ANavigationDataChunkActor> ItActor(World); ItActor; ++ItActor)
	{
		NavigationDataChunkActorPackages.Add(ItActor->GetPackage());
	}

	// Destroy any existing navigation data chunk actors within bounds we are generating, we will make new ones.
	int32 Count = 0;
	FBox GeneratingBounds = InCellInfo.Bounds.ExpandBy(-IterativeCellOverlapSize);

	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   GeneratingBounds %s"), *GeneratingBounds.ToString());
	
	for (TActorIterator<ANavigationDataChunkActor> It(World); It; ++It)
	{
		Count++;
		
		ANavigationDataChunkActor* Actor = *It;
		const FVector Location = Actor->GetActorLocation();
		
		auto IsInside2D = [](const FBox Bounds, const FVector& In) -> bool
		{
			return ((In.X >= Bounds.Min.X) && (In.X < Bounds.Max.X) && (In.Y >= Bounds.Min.Y) && (In.Y < Bounds.Max.Y));
		};

		UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Location %s %s (%s %s)"),
			*Location.ToCompactString(), IsInside2D(GeneratingBounds, Location) ? TEXT("inside") : TEXT("outside"), *Actor->GetName(), *Actor->GetPackage()->GetName());
		
		if (IsInside2D(GeneratingBounds, Location))
		{
			if (bCleanBuilderPackages)
			{
				check(!PackagesToClean.Find(Actor->GetPackage()));
				PackagesToClean.Add(Actor->GetPackage());
			}
			
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Destroy actor %s in package %s."), *Actor->GetName(), *Actor->GetPackage()->GetName());
			World->DestroyActor(Actor);
		}
	}
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Number of ANavigationDataChunkActor: %i"), Count);

	// Check if we are just in cleaning mode.
	if (bCleanBuilderPackages)
	{
		UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Number of packages to clear: %i"), PackagesToClean.Num());
		
		// Just delete all ANavigationDataChunkActor packages
		if (!PackageHelper.Delete(PackagesToClean.Array()))
		{
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Error, TEXT("Error deleting packages."));
		}

		if (!SavePackages(PackagesToClean.Array()))
		{
			return true;
		}

		return true;
	}

	// Make sure static meshes have compiled before generating navigation data
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	
	// Rebuild ANavigationDataChunkActor in loaded bounds
	WorldPartition->GenerateNavigationData(InCellInfo.Bounds);

	// Gather all packages again to include newly created ANavigationDataChunkActor actors
	for (TActorIterator<ANavigationDataChunkActor> ItActor(World); ItActor; ++ItActor)
	{
		NavigationDataChunkActorPackages.Add(ItActor->GetPackage());

		// Log
		FString String;
		if (ItActor->GetPackage())
		{
			String += ItActor->GetPackage()->GetName();
			String += UPackage::IsEmptyPackage(ItActor->GetPackage()) ? " empty" : " ";
			String += ItActor->GetPackage()->IsDirty() ? " dirty" : " ";
		}
		UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Adding package %s (from actor %s)."), *String, *ItActor->GetName());
	}

	TArray<UPackage*> PackagesToSave;
	TArray<UPackage*> PackagesToDelete;

	for (UPackage* ActorPackage : NavigationDataChunkActorPackages)
	{
		// Only change package that have been dirtied
		if (ActorPackage && ActorPackage->IsDirty())
		{
			if (UPackage::IsEmptyPackage(ActorPackage))
			{
				PackagesToDelete.Add(ActorPackage);
			}

			// Save all packages (we need to also save the ones we are deleting).
			PackagesToSave.Add(ActorPackage);
		}
	}

	// Delete packages
	if (!PackagesToDelete.IsEmpty())
	{
		UE_LOG(LogWorldPartitionNavigationDataBuilder, Log, TEXT("Deleting %d packages."), PackagesToDelete.Num());
		for (const UPackage* Package : PackagesToDelete)
		{
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Deleting package  %s."), *Package->GetName());	
		}
		
		if (!PackageHelper.Delete(PackagesToDelete))
		{
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Error, TEXT("Error deleting packages."));
			return true;
		}
	}

	// Save packages
	if (!PackagesToSave.IsEmpty())
	{
		{
			// Checkout packages to save
			TRACE_CPUPROFILER_EVENT_SCOPE(CheckoutPackages);
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Log, TEXT("Checking out %d packages."), PackagesToSave.Num());

			if (PackageHelper.UseSourceControl())
			{
				FEditorFileUtils::CheckoutPackages(PackagesToSave, /*OutPackagesCheckedOut*/nullptr, /*bErrorIfAlreadyCheckedOut*/false);
			}
			else
			{
				// Remove read-only
				for (const UPackage* Package : PackagesToSave)
				{
					const FString PackageFilename = SourceControlHelpers::PackageFilename(Package);
					if (IPlatformFile::GetPlatformPhysical().FileExists(*PackageFilename))
					{
						if (!IPlatformFile::GetPlatformPhysical().SetReadOnly(*PackageFilename, /*bNewReadOnlyValue*/false))
						{
							UE_LOG(LogWorldPartitionNavigationDataBuilder, Error, TEXT("Error setting %s writable"), *PackageFilename);
							return true;
						}
					}
				}
			}
		}

		if (!SavePackages(PackagesToSave))
		{
			return true;
		}
		
		{
			// Add new packages to source control
			TRACE_CPUPROFILER_EVENT_SCOPE(AddingToSourceControl);
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Log, TEXT("Adding packages to source control."));

			for (UPackage* Package : PackagesToSave)
			{
				if (!PackageHelper.AddToSourceControl(Package))
				{
					UE_LOG(LogWorldPartitionNavigationDataBuilder, Error, TEXT("Error adding package %s to source control."), *Package->GetName());
					return true;
				}
			}
		}

		UPackage::WaitForAsyncFileWrites();
	}

	return true;
}

bool UWorldPartitionNavigationDataBuilder::SavePackages(const TArray<UPackage*>& PackagesToSave)
{
	// Save packages
	TRACE_CPUPROFILER_EVENT_SCOPE(SavingPackages);
	UE_LOG(LogWorldPartitionNavigationDataBuilder, Log, TEXT("Saving %d packages."), PackagesToSave.Num());

	for (UPackage* Package : PackagesToSave)
	{
		UE_LOG(LogWorldPartitionNavigationDataBuilder, Verbose, TEXT("   Saving package  %s."), *Package->GetName());
		FString PackageFileName = SourceControlHelpers::PackageFilename(Package);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_Async;
		if (!UPackage::SavePackage(Package, nullptr, *PackageFileName, SaveArgs))
		{
			UE_LOG(LogWorldPartitionNavigationDataBuilder, Error, TEXT("   Error saving package %s."), *Package->GetName());
			return false;
		}
	}

	return true;
}

