// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceLevelStreaming.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/WorldPartitionLevelStreamingDynamic.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "LevelInstancePrivate.h"
#include "LevelUtils.h"
#include "Hash/CityHash.h"

#if WITH_EDITOR
#include "Settings/LevelEditorMiscSettings.h"
#include "LevelInstance/LevelInstanceEditorLevelStreaming.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/ITransaction.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "AssetRegistryModule.h"
#include "AssetData.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "LevelInstance/ILevelInstanceEditorModule.h"
#include "HAL/PlatformTime.h"
#include "Engine/Selection.h"
#include "Engine/LevelBounds.h"
#include "LevelInstance/LevelInstanceEditorInstanceActor.h"
#include "Modules/ModuleManager.h"
#include "Engine/Blueprint.h"
#include "LevelInstance/Packed/PackedLevelInstanceActor.h"
#include "LevelInstance/Packed/PackedLevelInstanceBuilder.h"
#include "Engine/LevelScriptBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"
#include "EditorActorFolders.h"
#include "Misc/MessageDialog.h"
#endif

#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "LevelInstanceSubsystem"

DEFINE_LOG_CATEGORY(LogLevelInstance);

ULevelInstanceSubsystem::ULevelInstanceSubsystem()
	: UWorldSubsystem()
{

}

ULevelInstanceSubsystem::~ULevelInstanceSubsystem()
{

}

void ULevelInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

#if WITH_EDITOR
	if (GEditor)
	{
		ILevelInstanceEditorModule& EditorModule = FModuleManager::LoadModuleChecked<ILevelInstanceEditorModule>("LevelInstanceEditor");
		EditorModule.OnExitEditorMode().AddUObject(this, &ULevelInstanceSubsystem::OnExitEditorMode);

		if (!GetWorld()->IsGameWorld())
		{
			FCoreUObjectDelegates::OnObjectPreSave.AddUObject(this, &ULevelInstanceSubsystem::OnObjectPreSave);
			FEditorDelegates::OnPackageDeleted.AddUObject(this, &ULevelInstanceSubsystem::OnPackageDeleted);
		}
	}
#endif
}

void ULevelInstanceSubsystem::Deinitialize()
{
	Super::Deinitialize();

#if WITH_EDITOR
	if (ILevelInstanceEditorModule* EditorModule = FModuleManager::GetModulePtr<ILevelInstanceEditorModule>("LevelInstanceEditor"))
	{
		EditorModule->OnExitEditorMode().RemoveAll(this);
	}

	if (!GetWorld()->IsGameWorld())
	{
		FCoreUObjectDelegates::OnObjectPreSave.RemoveAll(this);
		FEditorDelegates::OnPackageDeleted.RemoveAll(this);
	}
#endif
}

bool ULevelInstanceSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return Super::DoesSupportWorldType(WorldType) || WorldType == EWorldType::EditorPreview;
}

ALevelInstance* ULevelInstanceSubsystem::GetLevelInstance(FLevelInstanceID LevelInstanceID) const
{
	if (ALevelInstance*const* LevelInstanceActor = RegisteredLevelInstances.Find(LevelInstanceID))
	{
		return *LevelInstanceActor;
	}

	return nullptr;
}

FLevelInstanceID::FLevelInstanceID(ULevelInstanceSubsystem* LevelInstanceSubsystem, ALevelInstance* LevelInstanceActor)
{
	LevelInstanceSubsystem->ForEachLevelInstanceAncestorsAndSelf(LevelInstanceActor, [this](const ALevelInstance* AncestorOrSelf)
		{
			Guids.Add(AncestorOrSelf->GetLevelInstanceActorGuid());
			return true;
		});
	check(!Guids.IsEmpty());
	Hash = CityHash64((const char*)Guids.GetData(), Guids.Num() * sizeof(FGuid));
}

FLevelInstanceID ULevelInstanceSubsystem::RegisterLevelInstance(ALevelInstance* LevelInstanceActor)
{
	FLevelInstanceID LevelInstanceID(this, LevelInstanceActor);
	check(LevelInstanceID.IsValid());
	ALevelInstance*& Value = RegisteredLevelInstances.FindOrAdd(LevelInstanceID);
	check(GIsReinstancing || Value == nullptr || Value == LevelInstanceActor);
	Value = LevelInstanceActor;

#if WITH_EDITOR
	FObjectKey Level;
	if (UnregisteringLevelInstances.RemoveAndCopyValue(LevelInstanceActor, Level))
	{
		if (ULevel* LevelPtr = Cast<ULevel>(Level.ResolveObjectPtr()))
		{
			LevelPtr->bIsEditorBeingRemoved = false;
		}
		ensure(UnregisteringLevelInstanceLevels.Remove(Level));
	}
#endif

	return LevelInstanceID;
}

void ULevelInstanceSubsystem::UnregisterLevelInstance(ALevelInstance* LevelInstanceActor)
{
	RegisteredLevelInstances.Remove(LevelInstanceActor->GetLevelInstanceID());

#if WITH_EDITOR
	if (ULevel* Level = GetLevelInstanceLevel(LevelInstanceActor))
	{
		Level->bIsEditorBeingRemoved = true;
		UnregisteringLevelInstanceLevels.Add(Level, LevelInstanceActor);
		UnregisteringLevelInstances.Add(LevelInstanceActor, Level);
	}
#endif
}

void ULevelInstanceSubsystem::RequestLoadLevelInstance(ALevelInstance* LevelInstanceActor, bool bForce /* = false */)
{
	check(LevelInstanceActor && IsValidChecked(LevelInstanceActor) && !LevelInstanceActor->IsUnreachable());
	if (LevelInstanceActor->IsLevelInstancePathValid())
	{
#if WITH_EDITOR
		if (!IsEditingLevelInstance(LevelInstanceActor))
#endif
		{
#if WITH_EDITOR
			if (ULevel* Level = GetLevelInstanceLevel(LevelInstanceActor))
			{
				Level->bIsEditorBeingRemoved = false;
			}
#endif
			
			LevelInstancesToUnload.Remove(LevelInstanceActor->GetLevelInstanceID());

			bool* bForcePtr = LevelInstancesToLoadOrUpdate.Find(LevelInstanceActor);

			// Avoid loading if already loaded. Can happen if actor requests unload/load in same frame. Without the force it means its not necessary.
			if (IsLoaded(LevelInstanceActor) && !bForce && (bForcePtr == nullptr || !(*bForcePtr)))
			{
				return;
			}

			if (bForcePtr != nullptr)
			{
				*bForcePtr |= bForce;
			}
			else
			{
				LevelInstancesToLoadOrUpdate.Add(LevelInstanceActor, bForce);
			}
		}
	}
}

void ULevelInstanceSubsystem::RequestUnloadLevelInstance(ALevelInstance* LevelInstanceActor)
{
#if WITH_EDITOR
	if (ULevel* Level = GetLevelInstanceLevel(LevelInstanceActor))
	{
		Level->bIsEditorBeingRemoved = true;
	}
#endif

	const FLevelInstanceID& LevelInstanceID = LevelInstanceActor->GetLevelInstanceID();
	if (LevelInstances.Contains(LevelInstanceID))
	{
		// LevelInstancesToUnload uses FLevelInstanceID because LevelInstanceActor* can be destroyed in later Tick and we don't need it.
		LevelInstancesToUnload.Add(LevelInstanceID);
	}
	LevelInstancesToLoadOrUpdate.Remove(LevelInstanceActor);
}

bool ULevelInstanceSubsystem::IsLoaded(const ALevelInstance* LevelInstanceActor) const
{
	return LevelInstanceActor->HasValidLevelInstanceID() && LevelInstances.Contains(LevelInstanceActor->GetLevelInstanceID());
}

void ULevelInstanceSubsystem::UpdateStreamingState()
{
	if (!LevelInstancesToUnload.Num() && !LevelInstancesToLoadOrUpdate.Num())
	{
		return;
	}

#if WITH_EDITOR
	// Do not update during transaction
	if (GUndo)
	{
		return;
	}

	FScopedSlowTask SlowTask(LevelInstancesToUnload.Num() + LevelInstancesToLoadOrUpdate.Num() * 2, LOCTEXT("UpdatingLevelInstances", "Updating Level Instances..."), !GetWorld()->IsGameWorld());
	SlowTask.MakeDialogDelayed(1.0f);

	check(!LevelsToRemoveScope);
	LevelsToRemoveScope.Reset(new FLevelsToRemoveScope(this));
#endif

	if (LevelInstancesToUnload.Num())
	{
		TSet<FLevelInstanceID> LevelInstancesToUnloadCopy(MoveTemp(LevelInstancesToUnload));
		for (const FLevelInstanceID& LevelInstanceID : LevelInstancesToUnloadCopy)
		{
#if WITH_EDITOR
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("UnloadingLevelInstance", "Unloading Level Instance"));
#endif
			UnloadLevelInstance(LevelInstanceID);
		}
	}

	if (LevelInstancesToLoadOrUpdate.Num())
	{
		// Unload levels before doing any loading
		TMap<ALevelInstance*, bool> LevelInstancesToLoadOrUpdateCopy(MoveTemp(LevelInstancesToLoadOrUpdate));
		for (const TPair<ALevelInstance*, bool>& Pair : LevelInstancesToLoadOrUpdateCopy)
		{
#if WITH_EDITOR
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("UnloadingLevelInstance", "Unloading Level Instance"));
#endif
			ALevelInstance* LevelInstanceActor = Pair.Key;
			if (Pair.Value)
			{
				UnloadLevelInstance(LevelInstanceActor->GetLevelInstanceID());
			}
		}

#if WITH_EDITOR
		LevelsToRemoveScope.Reset();
		double StartTime = FPlatformTime::Seconds();
#endif
		for (const TPair<ALevelInstance*, bool>& Pair : LevelInstancesToLoadOrUpdateCopy)
		{
#if WITH_EDITOR
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("LoadingLevelInstance", "Loading Level Instance"));
#endif
			LoadLevelInstance(Pair.Key);
		}
#if WITH_EDITOR
		double ElapsedTime = FPlatformTime::Seconds() - StartTime;
		UE_LOG(LogLevelInstance, Log, TEXT("Loaded %s levels in %s seconds"), *FText::AsNumber(LevelInstancesToLoadOrUpdateCopy.Num()).ToString(), *FText::AsNumber(ElapsedTime).ToString());
#endif
	}

#if WITH_EDITOR
	LevelsToRemoveScope.Reset();
#endif
}

void ULevelInstanceSubsystem::LoadLevelInstance(ALevelInstance* LevelInstanceActor)
{
	check(LevelInstanceActor);
	if (IsLoaded(LevelInstanceActor) || !IsValidChecked(LevelInstanceActor) || LevelInstanceActor->IsUnreachable() || !LevelInstanceActor->IsLevelInstancePathValid())
	{
		return;
	}

	const FLevelInstanceID& LevelInstanceID = LevelInstanceActor->GetLevelInstanceID();
	check(!LevelInstances.Contains(LevelInstanceID));

	if (ULevelStreamingLevelInstance* LevelStreaming = ULevelStreamingLevelInstance::LoadInstance(LevelInstanceActor))
	{
		FLevelInstance& LevelInstance = LevelInstances.Add(LevelInstanceID);
		LevelInstance.LevelStreaming = LevelStreaming;
#if WITH_EDITOR
		LevelInstanceActor->OnLevelInstanceLoaded();
#endif
	}
}

void ULevelInstanceSubsystem::UnloadLevelInstance(const FLevelInstanceID& LevelInstanceID)
{
#if WITH_EDITOR
	// Create scope if it doesn't exist
	bool bReleaseScope = false;
	if (!LevelsToRemoveScope)
	{
		bReleaseScope = true;
		LevelsToRemoveScope.Reset(new FLevelsToRemoveScope(this));
	}
#endif
				
	FLevelInstance LevelInstance;
	if (LevelInstances.RemoveAndCopyValue(LevelInstanceID, LevelInstance))
	{
		if (ULevel* LoadedLevel = LevelInstance.LevelStreaming->GetLoadedLevel())
		{
#if WITH_EDITOR
			LoadedLevel->bIsEditorBeingRemoved = true;
#endif

			ForEachActorInLevel(LoadedLevel, [this](AActor* LevelActor)
			{
				if (ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(LevelActor))
				{
					// Make sure to remove from pending loads if we are unloading child can't be loaded
					LevelInstancesToLoadOrUpdate.Remove(LevelInstanceActor);
					
					UnloadLevelInstance(LevelInstanceActor->GetLevelInstanceID());
				}
				return true;
			});
		}

		ULevelStreamingLevelInstance::UnloadInstance(LevelInstance.LevelStreaming);
	}

#if WITH_EDITOR
	if (bReleaseScope)
	{
		LevelsToRemoveScope.Reset();
	}
#endif
}

void ULevelInstanceSubsystem::ForEachActorInLevel(ULevel* Level, TFunctionRef<bool(AActor * LevelActor)> Operation) const
{
	for (AActor* LevelActor : Level->Actors)
	{
		if (IsValid(LevelActor))
		{
			if (!Operation(LevelActor))
			{
				return;
			}
		}
	}
}

void ULevelInstanceSubsystem::ForEachLevelInstanceAncestorsAndSelf(AActor* Actor, TFunctionRef<bool(ALevelInstance*)> Operation) const
{
	if (ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(Actor))
	{
		if (!Operation(LevelInstanceActor))
		{
			return;
		}
	}

	ForEachLevelInstanceAncestors(Actor, Operation);
}

void ULevelInstanceSubsystem::ForEachLevelInstanceAncestors(AActor* Actor, TFunctionRef<bool(ALevelInstance*)> Operation) const
{
	ALevelInstance* ParentLevelInstance = nullptr;
	do
	{
		ParentLevelInstance = GetOwningLevelInstance(Actor->GetLevel());
		Actor = ParentLevelInstance;

	} while (ParentLevelInstance != nullptr && Operation(ParentLevelInstance));
}

ALevelInstance* ULevelInstanceSubsystem::GetOwningLevelInstance(const ULevel* Level) const
{
	if (ULevelStreaming* BaseLevelStreaming = FLevelUtils::FindStreamingLevel(Level))
	{
#if WITH_EDITOR
		if (ULevelStreamingLevelInstanceEditor* LevelStreamingEditor = Cast<ULevelStreamingLevelInstanceEditor>(BaseLevelStreaming))
		{
			return LevelStreamingEditor->GetLevelInstanceActor();
		}
		else 
#endif
		if (ULevelStreamingLevelInstance* LevelStreaming = Cast<ULevelStreamingLevelInstance>(BaseLevelStreaming))
		{
			return LevelStreaming->GetLevelInstanceActor();
		}
		else if (UWorldPartitionLevelStreamingDynamic* WorldPartitionLevelStreaming = Cast<UWorldPartitionLevelStreamingDynamic>(BaseLevelStreaming))
		{
			return GetOwningLevelInstance(WorldPartitionLevelStreaming->GetOuterWorld()->PersistentLevel);
		}
	}

	return nullptr;
}

#if WITH_EDITOR

void ULevelInstanceSubsystem::Tick()
{
	// For non-game world, Tick is responsible of processing LevelInstances to update/load/unload
	if (!GetWorld()->IsGameWorld())
	{
		UpdateStreamingState();
	}
}

void ULevelInstanceSubsystem::OnPackageChanged(UPackage* Package)
{
	if (LevelInstanceEdit)
	{
		if (UWorld* EditWorld = LevelInstanceEdit->GetEditWorld())
		{
			if (EditWorld->GetPackage() == Package)
			{
				LevelInstanceEdit->bCommittedChanges = true;
			}
			else
			{
				TSet<UPackage*> Packages;
				Packages.Append(EditWorld->GetPackage()->GetExternalPackages());
				if (Packages.Contains(Package))
				{
					LevelInstanceEdit->bCommittedChanges = true;
				}
			}
		}
	}
}

void ULevelInstanceSubsystem::OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
	if (!SaveContext.IsProceduralSave() && !(SaveContext.GetSaveFlags() & SAVE_FromAutosave))
	{
		OnPackageChanged(Object->GetPackage());
	}
}

void ULevelInstanceSubsystem::OnPackageDeleted(UPackage* Package)
{
	OnPackageChanged(Package);
}

void ULevelInstanceSubsystem::OnExitEditorMode()
{
	if (LevelInstanceEdit && !bCreatingLevelInstance)
	{
		bool bDiscard = true;
		bool bIsDirty = IsLevelInstanceEditDirty(LevelInstanceEdit.Get());
		if (bIsDirty)
		{
			FText Title = LOCTEXT("CommitOrDiscardChangesTitle", "Save changes?");
			bDiscard = (FMessageDialog::Open(EAppMsgType::YesNo, LOCTEXT("CommitOrDiscardChangesMsg", "Unsaved Level changes will get discarded. Do you want to save them now?"), &Title) == EAppReturnType::No);
		}

		CommitLevelInstanceInternal(LevelInstanceEdit, bDiscard, /*bPromptForSave=*/false);
	}
}

bool ULevelInstanceSubsystem::CanPackLevelInstances() const
{
	return !LevelInstanceEdit;
}

void ULevelInstanceSubsystem::PackLevelInstances()
{
	if (!CanPackLevelInstances())
	{
		return;
	}

	// Add Dependencies first so that we pack the LevelInstances in the proper order (depth first)
	TFunction<void(APackedLevelInstance*, TArray<UBlueprint*>&, TArray<APackedLevelInstance*>&)> GatherDepencenciesRecursive = [&GatherDepencenciesRecursive](APackedLevelInstance* PackedLevelInstance, TArray<UBlueprint*>& BPsToPack, TArray<APackedLevelInstance*>& ToPack)
	{
		// Early out on already processed BPs or non BP Packed LIs.
		UBlueprint* Blueprint = Cast<UBlueprint>(PackedLevelInstance->GetClass()->ClassGeneratedBy);
		if ((Blueprint && BPsToPack.Contains(Blueprint)) || ToPack.Contains(PackedLevelInstance))
		{
			return;
		}
		
		// Recursive deps
		for (const TSoftObjectPtr<UBlueprint>& Dependency : PackedLevelInstance->PackedBPDependencies)
		{
			if (UBlueprint* LoadedDependency = Dependency.LoadSynchronous())
			{
				if (APackedLevelInstance* CDO = Cast<APackedLevelInstance>(LoadedDependency->GeneratedClass ? LoadedDependency->GeneratedClass->GetDefaultObject() : nullptr))
				{
					GatherDepencenciesRecursive(CDO, BPsToPack, ToPack);
				}
			}
		}

		// Add after dependencies
		if (Blueprint)
		{
			BPsToPack.Add(Blueprint);
		}
		else
		{
			ToPack.Add(PackedLevelInstance);
		}
	};

	TArray<APackedLevelInstance*> PackedLevelInstancesToUpdate;
	TArray<UBlueprint*> BlueprintsToUpdate;
	for (TObjectIterator<UWorld> It(RF_ClassDefaultObject | RF_ArchetypeObject, true); It; ++It)
	{
		UWorld* CurrentWorld = *It;
		if (IsValid(CurrentWorld) && CurrentWorld->GetSubsystem<ULevelInstanceSubsystem>() != nullptr)
		{
			for (TActorIterator<APackedLevelInstance> LevelInstanceIt(CurrentWorld); LevelInstanceIt; ++LevelInstanceIt)
			{
				GatherDepencenciesRecursive(*LevelInstanceIt, BlueprintsToUpdate, PackedLevelInstancesToUpdate);
			}
		}
	}

	int32 Count = BlueprintsToUpdate.Num() + PackedLevelInstancesToUpdate.Num();
	if (!Count)
	{
		return;
	}
	
	GEditor->SelectNone(true, true);

	FScopedSlowTask SlowTask(Count, (LOCTEXT("LevelInstance_PackLevelInstances", "Packing Level Instances")));
	SlowTask.MakeDialog();
		
	auto UpdateProgress = [&SlowTask]()
	{
		if (SlowTask.CompletedWork < SlowTask.TotalAmountOfWork)
		{
			SlowTask.EnterProgressFrame(1, FText::Format(LOCTEXT("LevelInstance_PackLevelInstancesProgress", "Packing Level Instance {0} of {1}"), FText::AsNumber(SlowTask.CompletedWork), FText::AsNumber(SlowTask.TotalAmountOfWork)));
		}
	};

	TSharedPtr<FPackedLevelInstanceBuilder> Builder = FPackedLevelInstanceBuilder::CreateDefaultBuilder();
	const bool bCheckoutAndSave = false;
	for (UBlueprint* Blueprint : BlueprintsToUpdate)
	{
		Builder->UpdateBlueprint(Blueprint, bCheckoutAndSave);
		UpdateProgress();
	}

	for (APackedLevelInstance* PackedLevelInstance : PackedLevelInstancesToUpdate)
	{
		PackedLevelInstance->UpdateLevelInstance();
		UpdateProgress();
	}
}

bool ULevelInstanceSubsystem::GetLevelInstanceBounds(const ALevelInstance* LevelInstanceActor, FBox& OutBounds) const
{
	if (IsLoaded(LevelInstanceActor))
	{
		const FLevelInstance& LevelInstance = LevelInstances.FindChecked(LevelInstanceActor->GetLevelInstanceID());
		OutBounds = LevelInstance.LevelStreaming->GetBounds();
		return true;
	}
	else if (const FLevelInstanceEdit* CurrentEdit = GetLevelInstanceEdit(LevelInstanceActor))
	{
		OutBounds = CurrentEdit->LevelStreaming->GetBounds();
		return true;
	}
	else if(LevelInstanceActor->IsLevelInstancePathValid())
	{
		return GetLevelInstanceBoundsFromPackage(LevelInstanceActor->GetActorTransform(), FName(*LevelInstanceActor->GetWorldAssetPackage()), OutBounds);
	}

	return false;
}

bool ULevelInstanceSubsystem::GetLevelInstanceBoundsFromPackage(const FTransform& InstanceTransform, FName LevelPackage, FBox& OutBounds)
{
	FBox LevelBounds;
	if (ULevel::GetLevelBoundsFromPackage(LevelPackage, LevelBounds))
	{
		FVector LevelBoundsLocation;
		FVector BoundsLocation;
		FVector BoundsExtent;
		LevelBounds.GetCenterAndExtents(BoundsLocation, BoundsExtent);

		//@todo_ow: This will result in a new BoundsExtent that is larger than it should. To fix this, we would need the Object Oriented BoundingBox of the actor (the BV of the actor without rotation)
		const FVector BoundsMin = BoundsLocation - BoundsExtent;
		const FVector BoundsMax = BoundsLocation + BoundsExtent;
		OutBounds = FBox(BoundsMin, BoundsMax).TransformBy(InstanceTransform);
		return true;
	}

	return false;
}

void ULevelInstanceSubsystem::ForEachActorInLevelInstance(const ALevelInstance* LevelInstanceActor, TFunctionRef<bool(AActor * LevelActor)> Operation) const
{
	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		ForEachActorInLevel(LevelInstanceLevel, Operation);
	}
}

void ULevelInstanceSubsystem::ForEachLevelInstanceAncestorsAndSelf(const AActor* Actor, TFunctionRef<bool(const ALevelInstance*)> Operation) const
{
	if (const ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(Actor))
	{
		if (!Operation(LevelInstanceActor))
		{
			return;
		}
	}

	ForEachLevelInstanceAncestors(Actor, Operation);
}

void ULevelInstanceSubsystem::ForEachLevelInstanceAncestors(const AActor* Actor, TFunctionRef<bool(const ALevelInstance*)> Operation) const
{
	const ALevelInstance* ParentLevelInstance = nullptr;
	do 
	{
		ParentLevelInstance = GetOwningLevelInstance(Actor->GetLevel());
		Actor = ParentLevelInstance;
	} 
	while (ParentLevelInstance != nullptr && Operation(ParentLevelInstance));
}

void ULevelInstanceSubsystem::ForEachLevelInstanceChild(const ALevelInstance* LevelInstanceActor, bool bRecursive, TFunctionRef<bool(const ALevelInstance*)> Operation) const
{
	ForEachLevelInstanceChildImpl(LevelInstanceActor, bRecursive, Operation);
}

bool ULevelInstanceSubsystem::ForEachLevelInstanceChildImpl(const ALevelInstance* LevelInstanceActor, bool bRecursive, TFunctionRef<bool(const ALevelInstance*)> Operation) const
{
	bool bContinue = true;
	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		ForEachActorInLevel(LevelInstanceLevel, [&bContinue, this, Operation,bRecursive](AActor* LevelActor)
		{
			if (const ALevelInstance* ChildLevelInstanceActor = Cast<ALevelInstance>(LevelActor))
			{
				bContinue = Operation(ChildLevelInstanceActor);
				
				if (bContinue && bRecursive)
				{
					bContinue = ForEachLevelInstanceChildImpl(ChildLevelInstanceActor, bRecursive, Operation);
				}
			}
			return bContinue;
		});
	}

	return bContinue;
}

void ULevelInstanceSubsystem::ForEachLevelInstanceChild(ALevelInstance* LevelInstanceActor, bool bRecursive, TFunctionRef<bool(ALevelInstance*)> Operation) const
{
	ForEachLevelInstanceChildImpl(LevelInstanceActor, bRecursive, Operation);
}

bool ULevelInstanceSubsystem::ForEachLevelInstanceChildImpl(ALevelInstance* LevelInstanceActor, bool bRecursive, TFunctionRef<bool(ALevelInstance*)> Operation) const
{
	bool bContinue = true;
	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		ForEachActorInLevel(LevelInstanceLevel, [&bContinue, this, Operation, bRecursive](AActor* LevelActor)
		{
			if (ALevelInstance* ChildLevelInstanceActor = Cast<ALevelInstance>(LevelActor))
			{
				bContinue = Operation(ChildLevelInstanceActor);

				if (bContinue && bRecursive)
				{
					bContinue = ForEachLevelInstanceChildImpl(ChildLevelInstanceActor, bRecursive, Operation);
				}
			}
			return bContinue;
		});
	}

	return bContinue;
}

bool ULevelInstanceSubsystem::HasDirtyChildrenLevelInstances(const ALevelInstance* LevelInstanceActor) const
{
	bool bDirtyChildren = false;
	ForEachLevelInstanceChild(LevelInstanceActor, /*bRecursive=*/true, [this, &bDirtyChildren](const ALevelInstance* ChildLevelInstanceActor)
	{
		if (IsEditingLevelInstanceDirty(ChildLevelInstanceActor))
		{
			bDirtyChildren = true;
			return false;
		}
		return true;
	});
	return bDirtyChildren;
}

void ULevelInstanceSubsystem::SetIsHiddenEdLayer(ALevelInstance* LevelInstanceActor, bool bIsHiddenEdLayer)
{
	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		ForEachActorInLevel(LevelInstanceLevel, [bIsHiddenEdLayer](AActor* LevelActor)
		{
			LevelActor->SetIsHiddenEdLayer(bIsHiddenEdLayer);
			return true;
		});
	}
}

void ULevelInstanceSubsystem::SetIsTemporarilyHiddenInEditor(ALevelInstance* LevelInstanceActor, bool bIsHidden)
{
	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		ForEachActorInLevel(LevelInstanceLevel, [bIsHidden](AActor* LevelActor)
		{
			LevelActor->SetIsTemporarilyHiddenInEditor(bIsHidden);
			return true;
		});
	}
}

bool ULevelInstanceSubsystem::SetCurrent(ALevelInstance* LevelInstanceActor) const
{
	if (IsEditingLevelInstance(LevelInstanceActor))
	{
		return GetWorld()->SetCurrentLevel(GetLevelInstanceLevel(LevelInstanceActor));
	}

	return false;
}

bool ULevelInstanceSubsystem::IsCurrent(const ALevelInstance* LevelInstanceActor) const
{
	if (IsEditingLevelInstance(LevelInstanceActor))
	{
		return GetLevelInstanceLevel(LevelInstanceActor) == GetWorld()->GetCurrentLevel();
	}

	return false;
}

bool ULevelInstanceSubsystem::MoveActorsToLevel(const TArray<AActor*>& ActorsToRemove, ULevel* DestinationLevel, TArray<AActor*>* OutActors /*= nullptr*/) const
{
	check(DestinationLevel);

	const bool bWarnAboutReferences = true;
	const bool bWarnAboutRenaming = true;
	const bool bMoveAllOrFail = true;
	if (!EditorLevelUtils::MoveActorsToLevel(ActorsToRemove, DestinationLevel, bWarnAboutReferences, bWarnAboutRenaming, bMoveAllOrFail, OutActors))
	{
		UE_LOG(LogLevelInstance, Warning, TEXT("Failed to move actors out of Level Instance because not all actors could be moved"));
		return false;
	}

	ALevelInstance* OwningInstance = GetOwningLevelInstance(DestinationLevel);
	if (!OwningInstance || !OwningInstance->IsEditing())
	{
		for (const auto& Actor : ActorsToRemove)
		{
			const bool bEditing = false;
			Actor->PushLevelInstanceEditingStateToProxies(bEditing);
		}
	}

	return true;
}

bool ULevelInstanceSubsystem::MoveActorsTo(ALevelInstance* LevelInstanceActor, const TArray<AActor*>& ActorsToMove, TArray<AActor*>* OutActors /*= nullptr*/)
{
	check(IsEditingLevelInstance(LevelInstanceActor));
	ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor);
	check(LevelInstanceLevel);

	return MoveActorsToLevel(ActorsToMove, LevelInstanceLevel, OutActors);
}

ALevelInstance* ULevelInstanceSubsystem::CreateLevelInstanceFrom(const TArray<AActor*>& ActorsToMove, const FNewLevelInstanceParams& CreationParams)
{
	TGuardValue<bool> CreateLevelInstanceGuard(bCreatingLevelInstance, true);
	ULevel* CurrentLevel = GetWorld()->GetCurrentLevel();
		
	if (ActorsToMove.Num() == 0)
	{
		UE_LOG(LogLevelInstance, Warning, TEXT("Failed to create Level Instance from empty actor array"));
		return nullptr;
	}
		
	FBox ActorLocationBox(ForceInit);
	for (const AActor* ActorToMove : ActorsToMove)
	{
		const bool bNonColliding = false;
		const bool bIncludeChildren = true;
		ActorLocationBox += ActorToMove->GetComponentsBoundingBox(bNonColliding, bIncludeChildren);

		FText Reason;
		if (!CanMoveActorToLevel(ActorToMove, &Reason))
		{
			UE_LOG(LogLevelInstance, Warning, TEXT("%s"), *Reason.ToString());
			return nullptr;
		}
	}

	FVector LevelInstanceLocation;
	if (CreationParams.PivotType == ELevelInstancePivotType::Actor)
	{
		check(CreationParams.PivotActor);
		LevelInstanceLocation = CreationParams.PivotActor->GetActorLocation();
	}
	else if (CreationParams.PivotType == ELevelInstancePivotType::WorldOrigin)
	{
		LevelInstanceLocation = FVector(0.f, 0.f, 0.f);
	}
	else
	{
		LevelInstanceLocation = ActorLocationBox.GetCenter();
		if (CreationParams.PivotType == ELevelInstancePivotType::CenterMinZ)
		{
			LevelInstanceLocation.Z = ActorLocationBox.Min.Z;
		}
	}
		
	FString LevelFilename;
	if (!CreationParams.LevelPackageName.IsEmpty())
	{
		LevelFilename = FPackageName::LongPackageNameToFilename(CreationParams.LevelPackageName, FPackageName::GetMapPackageExtension());
	}

	ULevelStreamingLevelInstanceEditor* LevelStreaming = StaticCast<ULevelStreamingLevelInstanceEditor*>(EditorLevelUtils::CreateNewStreamingLevelForWorld(*GetWorld(), ULevelStreamingLevelInstanceEditor::StaticClass(), LevelFilename, false, CreationParams.TemplateWorld));
	if (!LevelStreaming)
	{
		UE_LOG(LogLevelInstance, Warning, TEXT("Failed to create new Level Instance level"));
		return nullptr;
	}

	ULevel* LoadedLevel = LevelStreaming->GetLoadedLevel();
	check(LoadedLevel);

	const bool bWarnAboutReferences = true;
	const bool bWarnAboutRenaming = false;
	const bool bMoveAllOrFail = true;

	TSet<FName> DirtyPackages;

	// Capture Packages before Moving actors as they can get GCed in the process
	for (AActor* ActorToMove : ActorsToMove)
	{
		// Don't force saving of unsaved/temp packages onto the user.
		if (!FPackageName::IsTempPackage(ActorToMove->GetPackage()->GetName()))
		{
			DirtyPackages.Add(ActorToMove->GetPackage()->GetFName());
		}
	}

	if (!EditorLevelUtils::MoveActorsToLevel(ActorsToMove, LoadedLevel, bWarnAboutReferences, bWarnAboutRenaming, bMoveAllOrFail))
	{
		ULevelStreamingLevelInstanceEditor::Unload(LevelStreaming);
		UE_LOG(LogLevelInstance, Warning, TEXT("Failed to create Level Instance because some actors couldn't be moved"));
		return nullptr;
	}
	
	// Convert to OFPA
	if (CreationParams.UseExternalActors())
	{
		LoadedLevel->ConvertAllActorsToPackaging(true);
		LoadedLevel->bUseExternalActors = true;
	}

	// Take all actors out of any folders they may have been in since we don't support folders inside of level instances
	for (AActor* Actor : LoadedLevel->Actors)
	{
		if (Actor)
		{
			Actor->SetFolderPath_Recursively(NAME_None);
		}
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = CurrentLevel;
	ALevelInstance* NewLevelInstanceActor = nullptr;
	TSoftObjectPtr<UWorld> WorldPtr(LoadedLevel->GetTypedOuter<UWorld>());
	
	if (CreationParams.Type == ELevelInstanceCreationType::LevelInstance)
	{
		NewLevelInstanceActor = GetWorld()->SpawnActor<ALevelInstance>(ALevelInstance::StaticClass(), SpawnParams);
	}
	else if (CreationParams.Type == ELevelInstanceCreationType::PackedLevelInstance)
	{
		NewLevelInstanceActor = GetWorld()->SpawnActor<APackedLevelInstance>(APackedLevelInstance::StaticClass(), SpawnParams);
	}
	else if (CreationParams.Type == ELevelInstanceCreationType::PackedLevelInstanceBlueprint)
	{
		FString PackageDir = FPaths::GetPath(WorldPtr.GetLongPackageName());
		FString AssetName = FPackedLevelInstanceBuilder::GetPackedBPPrefix() + WorldPtr.GetAssetName();
		FString BPAssetPath = FString::Format(TEXT("{0}/{1}.{1}"), { PackageDir , AssetName });
		const bool bCompile = true;

		UBlueprint* NewBP = nullptr;
		if (CreationParams.LevelPackageName.IsEmpty())
		{
			NewBP = FPackedLevelInstanceBuilder::CreatePackedLevelInstanceBlueprintWithDialog(TSoftObjectPtr<UBlueprint>(BPAssetPath), WorldPtr, bCompile);
		}
		else
		{
			NewBP = FPackedLevelInstanceBuilder::CreatePackedLevelInstanceBlueprint(TSoftObjectPtr<UBlueprint>(BPAssetPath), WorldPtr, bCompile);
		}
				
		if (NewBP)
		{
			NewLevelInstanceActor = GetWorld()->SpawnActor<APackedLevelInstance>(NewBP->GeneratedClass, SpawnParams);
		}

		if (!NewLevelInstanceActor)
		{
			UE_LOG(LogLevelInstance, Warning, TEXT("Failed to create packed level blueprint. Creating non blueprint packed level instance instead."));
			NewLevelInstanceActor = GetWorld()->SpawnActor<APackedLevelInstance>(APackedLevelInstance::StaticClass(), SpawnParams);
		}
	}
	
	check(NewLevelInstanceActor);
	NewLevelInstanceActor->SetWorldAsset(WorldPtr);
	NewLevelInstanceActor->SetActorLocation(LevelInstanceLocation);
	
	// Actors were moved and kept their World positions so when saving we want their positions to actually be relative to the FounationActor/LevelTransform
	// so we set the LevelTransform and we mark the level as having moved its actors. 
	// On Level save FLevelUtils::RemoveEditorTransform will fixup actor transforms to make them relative to the LevelTransform.
	LevelStreaming->LevelTransform = NewLevelInstanceActor->GetActorTransform();
	LoadedLevel->bAlreadyMovedActors = true;

	GEditor->SelectNone(false, true);
	GEditor->SelectActor(NewLevelInstanceActor, true, true);

	NewLevelInstanceActor->OnEdit();

	// Notify parents of edit
	TArray<FLevelInstanceID> AncestorIDs;
	ForEachLevelInstanceAncestors(NewLevelInstanceActor, [&AncestorIDs](ALevelInstance* InAncestor)
	{
		AncestorIDs.Add(InAncestor->GetLevelInstanceID());
		return true;
	});

	for (const FLevelInstanceID& AncestorID : AncestorIDs)
	{
		OnEditChild(AncestorID);
	}
	
	// New level instance
	TUniquePtr<FLevelInstanceEdit> TempLevelInstanceEdit = MakeUnique<FLevelInstanceEdit>(LevelStreaming, NewLevelInstanceActor->GetLevelInstanceID());
	// Force mark it as changed
	TempLevelInstanceEdit->bCommittedChanges = true;

	GetWorld()->SetCurrentLevel(LoadedLevel);

	// Don't force saving of unsaved/temp packages onto the user.
	if (!FPackageName::IsTempPackage(NewLevelInstanceActor->GetPackage()->GetName()))
	{
		DirtyPackages.Add(NewLevelInstanceActor->GetPackage()->GetFName());
	}
			
	ALevelInstance* CommittedLevelInstance = CommitLevelInstanceInternal(TempLevelInstanceEdit, /*bDiscardEdits*/false, CreationParams.bPromptForSave, &DirtyPackages);
	check(!TempLevelInstanceEdit);

	// EditorLevelUtils::CreateNewStreamingLevelForWorld deactivates all modes. Re-activate if needed
	if (LevelInstanceEdit)
	{
		ILevelInstanceEditorModule& EditorModule = FModuleManager::GetModuleChecked<ILevelInstanceEditorModule>("LevelInstanceEditor");
		EditorModule.ActivateEditorMode();
	}

	return CommittedLevelInstance;
}

bool ULevelInstanceSubsystem::BreakLevelInstance(ALevelInstance* LevelInstanceActor, uint32 Levels /* = 1 */, TArray<AActor*>* OutMovedActors /* = nullptr */)
{
	const double StartTime = FPlatformTime::Seconds();

	const uint32 bAvoidRelabelOnPasteSelected = GetMutableDefault<ULevelEditorMiscSettings>()->bAvoidRelabelOnPasteSelected;
	ON_SCOPE_EXIT { GetMutableDefault<ULevelEditorMiscSettings>()->bAvoidRelabelOnPasteSelected = bAvoidRelabelOnPasteSelected; };
	GetMutableDefault<ULevelEditorMiscSettings>()->bAvoidRelabelOnPasteSelected = 1;

	TArray<AActor*> MovedActors;
	BreakLevelInstance_Impl(LevelInstanceActor, Levels, MovedActors);

	USelection* ActorSelection = GEditor->GetSelectedActors();
	ActorSelection->BeginBatchSelectOperation();
	for (AActor* MovedActor : MovedActors)
	{
		GEditor->SelectActor(MovedActor, true, false);
	}
	ActorSelection->EndBatchSelectOperation(false);

	bool bStatus = MovedActors.Num() > 0;

	const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogLevelInstance, Log, TEXT("Break took %s seconds (%s actors)"), *FText::AsNumber(ElapsedTime).ToString(), *FText::AsNumber(MovedActors.Num()).ToString());
	
	if (OutMovedActors)
	{
		*OutMovedActors = MoveTemp(MovedActors);
	}

	return bStatus;
}

void ULevelInstanceSubsystem::BreakLevelInstance_Impl(ALevelInstance* LevelInstanceActor, uint32 Levels, TArray<AActor*>& OutMovedActors)
{
	if (Levels > 0)
	{
		// Can only break the top level LevelInstance
		check(LevelInstanceActor->GetLevel() == GetWorld()->GetCurrentLevel());

		// Actors in a packed level instance will not be streamed in unless they are editing. Must force this before moving.
		if (LevelInstanceActor->IsA<APackedLevelInstance>())
		{
			BlockLoadLevelInstance(LevelInstanceActor);
		}

		// need to ensure that LevelInstanceActor has been streamed in fully
		GEngine->BlockTillLevelStreamingCompleted(LevelInstanceActor->GetWorld());

		// Cannot break a level instance which has a level script
		if (LevelInstanceHasLevelScriptBlueprint(LevelInstanceActor))
		{
			UE_LOG(LogLevelInstance, Warning, TEXT("Failed to completely break Level Instance because some children have Level Scripts."));

			if (LevelInstanceActor->IsA<APackedLevelInstance>())
			{
				BlockUnloadLevelInstance(LevelInstanceActor);
			}
			return;
		}

		TArray<const UDataLayer*> LevelInstanceDataLayers = LevelInstanceActor->GetDataLayerObjects();

		TSet<AActor*> ActorsToMove;
		TFunction<bool(AActor*)> AddActorToMove = [this, &ActorsToMove, &AddActorToMove, &LevelInstanceDataLayers](AActor* Actor)
		{
			if (ActorsToMove.Contains(Actor))
			{
				return true;
			}

			// Skip some actor types
			if (!Actor->IsA<ALevelBounds>() && (Actor != Actor->GetLevel()->GetDefaultBrush()) && !Actor->IsA<AWorldSettings>() && !Actor->IsA<ALevelInstanceEditorInstanceActor>())
			{
				if (CanMoveActorToLevel(Actor))
				{
					FSetActorHiddenInSceneOutliner Show(Actor, false);

					// Detach if Parent Actor can't be moved
					if (AActor* ParentActor = Actor->GetAttachParentActor())
					{
						if (!AddActorToMove(ParentActor))
						{
							Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
						}
					}

					// Apply the same data layer settings to the actors to move out
					if (Actor->SupportsDataLayer() && Actor->IsValidForDataLayer())
					{
						for (const UDataLayer* DataLayer : LevelInstanceDataLayers)
						{
							Actor->AddDataLayer(DataLayer);
						}
					}

					ActorsToMove.Add(Actor);
					return true;
				}
			}

			return false;
		};

		ForEachActorInLevelInstance(LevelInstanceActor, [this, &ActorsToMove, &AddActorToMove](AActor* Actor)
		{
			AddActorToMove(Actor);
			return true;
		});

		ULevel* DestinationLevel = GetWorld()->GetCurrentLevel();
		check(DestinationLevel);

		const bool bWarnAboutReferences = true;
		const bool bWarnAboutRenaming = false;
		const bool bMoveAllOrFail = true;
		if (!EditorLevelUtils::CopyActorsToLevel(ActorsToMove.Array(), DestinationLevel, bWarnAboutReferences, bWarnAboutRenaming, bMoveAllOrFail))
		{
			UE_LOG(LogLevelInstance, Warning, TEXT("Failed to break Level Instance because not all actors could be moved"));
			return;
		}

		if (LevelInstanceActor->IsA<APackedLevelInstance>())
		{
			BlockUnloadLevelInstance(LevelInstanceActor);
		}

		// Destroy the old LevelInstance instance actor
		GetWorld()->DestroyActor(LevelInstanceActor);
	
		const bool bContinueBreak = Levels > 1;
		TArray<ALevelInstance*> Children;

		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);

			if (Actor)
			{
				OutMovedActors.Add(Actor);
			}

			// Break up any sub LevelInstances if more levels are requested
			if (bContinueBreak)
			{
				if (ALevelInstance* ChildLevelInstance = Cast<ALevelInstance>(Actor))
				{
					OutMovedActors.Remove(ChildLevelInstance);

					Children.Add(ChildLevelInstance);
				}
			}
		}

		for (auto& Child : Children)
		{
			BreakLevelInstance_Impl(Child, Levels - 1, OutMovedActors);
		}
	}

	return;
}

ULevel* ULevelInstanceSubsystem::GetLevelInstanceLevel(const ALevelInstance* LevelInstanceActor) const
{
	if (LevelInstanceActor->HasValidLevelInstanceID())
	{
		if (const FLevelInstanceEdit* CurrentEdit = GetLevelInstanceEdit(LevelInstanceActor))
		{
			return LevelInstanceEdit->LevelStreaming->GetLoadedLevel();
		}
		else if (const FLevelInstance* LevelInstance = LevelInstances.Find(LevelInstanceActor->GetLevelInstanceID()))
		{
			return LevelInstance->LevelStreaming->GetLoadedLevel();
		}
	}

	return nullptr;
}

bool ULevelInstanceSubsystem::LevelInstanceHasLevelScriptBlueprint(const ALevelInstance* LevelInstance) const
{
	if (LevelInstance)
	{
		if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstance))
		{
			if (ULevelScriptBlueprint* LevelScriptBP = LevelInstanceLevel->GetLevelScriptBlueprint(true))
			{
				TArray<UEdGraph*> AllGraphs;
				LevelScriptBP->GetAllGraphs(AllGraphs);
				for (UEdGraph* CurrentGraph : AllGraphs)
				{
					for (UEdGraphNode* Node : CurrentGraph->Nodes)
					{
						if (!Node->IsAutomaticallyPlacedGhostNode())
						{
							return true;
						}
					}
				}
			}
		}
	}

	return false;
}

void ULevelInstanceSubsystem::RemoveLevelsFromWorld(const TArray<ULevel*>& InLevels, bool bResetTrans)
{
	if (LevelsToRemoveScope && LevelsToRemoveScope->IsValid())
	{
		for (ULevel* Level : InLevels)
		{
			LevelsToRemoveScope->Levels.AddUnique(Level);
		}
		LevelsToRemoveScope->bResetTrans |= bResetTrans;
	}
	else
	{
		TSet<ULevel*> LevelInstanceLevels;
		for (ULevel* Level : InLevels)
		{
			bool bIsAlreadyInSet = false;
			LevelInstanceLevels.Add(Level, &bIsAlreadyInSet);

			if (!bIsAlreadyInSet)
			{
				ForEachActorInLevel(Level, [this, &LevelInstanceLevels](AActor* LevelActor)
				{
					if (const ALevelInstance* ChildLevelInstanceActor = Cast<ALevelInstance>(LevelActor))
					{
						ForEachLevelInstanceChild(ChildLevelInstanceActor, /*bRecursive*/true, [this, &LevelInstanceLevels](const ALevelInstance* ChildLevelInstanceActor)
						{
							if (ULevel* ChildLevel = GetLevelInstanceLevel(ChildLevelInstanceActor))
							{
								LevelInstanceLevels.Add(ChildLevel);
							}
							return true;
						});
					}
					return true;
				});
			}
		}

		// Flag all levels as bEditorBeingDestroyed. This way, even if child level instances are still pending to be unloaded, TActorIterator won't iterate on them.
		for (ULevel* Level : LevelInstanceLevels)
		{
			Level->bIsEditorBeingRemoved = true;
			FFolder::FRootObject RootObject;
			if (UnregisteringLevelInstanceLevels.RemoveAndCopyValue(Level, RootObject))
			{
				ensure(UnregisteringLevelInstances.Remove(RootObject));
				FActorFolders::Get().OnFolderRootObjectRemoved(*GetWorld(), RootObject);
			}
		}

		// No need to clear the whole editor selection since actor of this level will be removed from the selection by: UEditorEngine::OnLevelRemovedFromWorld
		EditorLevelUtils::RemoveLevelsFromWorld(InLevels, /*bClearSelection*/false, bResetTrans);
	}
}

ULevelInstanceSubsystem::FLevelsToRemoveScope::FLevelsToRemoveScope(ULevelInstanceSubsystem* InOwner)
	: Owner(InOwner)
	, bIsBeingDestroyed(false)
{
}

ULevelInstanceSubsystem::FLevelsToRemoveScope::~FLevelsToRemoveScope()
{
	if (Levels.Num() > 0)
	{
		bIsBeingDestroyed = true;
		double StartTime = FPlatformTime::Seconds();
		ULevelInstanceSubsystem* LevelInstanceSubsystem = Owner.Get();
		check(LevelInstanceSubsystem);
		LevelInstanceSubsystem->RemoveLevelsFromWorld(Levels, bResetTrans);
		double ElapsedTime = FPlatformTime::Seconds() - StartTime;
		UE_LOG(LogLevelInstance, Log, TEXT("Unloaded %s levels in %s seconds"), *FText::AsNumber(Levels.Num()).ToString(), *FText::AsNumber(ElapsedTime).ToString());
	}
}

bool ULevelInstanceSubsystem::CanMoveActorToLevel(const AActor* Actor, FText* OutReason) const
{
	if (Actor->GetWorld() == GetWorld())
	{
		if (const ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(Actor))
		{
			if (IsEditingLevelInstance(LevelInstanceActor))
			{
				if (OutReason != nullptr)
				{
					*OutReason = LOCTEXT("CanMoveActorLevelEditing", "Can't move Level Instance actor while it is being edited");
				}
				return false;
			}

			bool bEditingChildren = false;
			ForEachLevelInstanceChild(LevelInstanceActor, true, [this, &bEditingChildren](const ALevelInstance* ChildLevelInstanceActor)
			{
				if (IsEditingLevelInstance(ChildLevelInstanceActor))
				{
					bEditingChildren = true;
					return false;
				}
				return true;
			});

			if (bEditingChildren)
			{
				if (OutReason != nullptr)
				{
					*OutReason = LOCTEXT("CanMoveActorToLevelChildEditing", "Can't move Level Instance actor while one of its child Level Instance is being edited");
				}
				return false;
			}
		}
	}

	return true;
}

void ULevelInstanceSubsystem::DiscardEdits()
{
	LevelInstanceEdit.Reset();
}

void ULevelInstanceSubsystem::OnActorDeleted(AActor* Actor)
{
	if (ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(Actor))
	{
		if (Actor->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			// We are receiving this event when destroying the old actor after BP reinstantiation. In this case,
			// the newly created actor was already added to the list, so we can safely ignore this case.
			check(GIsReinstancing);
			return;
		}

		// Unregistered Level Instance Actor nothing to do.
		if (!LevelInstanceActor->HasValidLevelInstanceID())
		{
			return;
		}

		const bool bIsEditingLevelInstance = IsEditingLevelInstance(LevelInstanceActor);
		if (!bIsEditingLevelInstance && LevelInstanceActor->IsA<APackedLevelInstance>())
		{
			return;
		}

		const bool bAlreadyRooted = LevelInstanceActor->IsRooted();
		// Unloading LevelInstances leads to GC and Actor can be collected. Add to root temp. It will get collected after the OnActorDeleted callbacks
		if (!bAlreadyRooted)
		{
			LevelInstanceActor->AddToRoot();
		}

		FScopedSlowTask SlowTask(0, LOCTEXT("UnloadingLevelInstances", "Unloading Level Instances..."), !GetWorld()->IsGameWorld());
		SlowTask.MakeDialog();
		check(!IsEditingLevelInstanceDirty(LevelInstanceActor) && !HasDirtyChildrenLevelInstances(LevelInstanceActor));
		if (bIsEditingLevelInstance)
		{
			CommitLevelInstance(LevelInstanceActor);
		}
		else
		{
			// We are ending editing. Discard Non dirty child edits
			ForEachLevelInstanceChild(LevelInstanceActor, /*bRecursive=*/true, [this](const ALevelInstance* ChildLevelInstanceActor)
			{
				if (const FLevelInstanceEdit* ChildLevelInstanceEdit = GetLevelInstanceEdit(ChildLevelInstanceActor))
				{
					check(!IsLevelInstanceEditDirty(ChildLevelInstanceEdit));
					LevelInstanceEdit.Reset();
					return false;
				}
				return true;
			});
		}

		LevelInstancesToLoadOrUpdate.Remove(LevelInstanceActor);
				
		UnloadLevelInstance(LevelInstanceActor->GetLevelInstanceID());
		
		// Remove from root so it gets collected on the next GC if it can be.
		if (!bAlreadyRooted)
		{
			LevelInstanceActor->RemoveFromRoot();
		}
	}
}

bool ULevelInstanceSubsystem::ShouldIgnoreDirtyPackage(UPackage* DirtyPackage, const UWorld* EditingWorld)
{
	if (DirtyPackage == EditingWorld->GetOutermost())
	{
		return false;
	}

	bool bIgnore = true;
	ForEachObjectWithPackage(DirtyPackage, [&bIgnore, EditingWorld](UObject* Object)
	{
		if (Object->GetOutermostObject() == EditingWorld)
		{
			bIgnore = false;
		}

		return bIgnore;
	});

	return bIgnore;
}

ULevelInstanceSubsystem::FLevelInstanceEdit::FLevelInstanceEdit(ULevelStreamingLevelInstanceEditor* InLevelStreaming, FLevelInstanceID InLevelInstanceID)
	: LevelStreaming(InLevelStreaming)
{
	LevelStreaming->LevelInstanceID = InLevelInstanceID;
}

ULevelInstanceSubsystem::FLevelInstanceEdit::~FLevelInstanceEdit()
{
	ULevelStreamingLevelInstanceEditor::Unload(LevelStreaming);
}

UWorld* ULevelInstanceSubsystem::FLevelInstanceEdit::GetEditWorld() const
{
	if (LevelStreaming && LevelStreaming->GetLoadedLevel())
	{
		return LevelStreaming->GetLoadedLevel()->GetTypedOuter<UWorld>();
	}

	return nullptr;
}

FLevelInstanceID ULevelInstanceSubsystem::FLevelInstanceEdit::GetLevelInstanceID() const
{
	return LevelStreaming ? LevelStreaming->GetLevelInstanceID() : FLevelInstanceID();
}

const ULevelInstanceSubsystem::FLevelInstanceEdit* ULevelInstanceSubsystem::GetLevelInstanceEdit(const ALevelInstance* LevelInstanceActor) const
{
	if (LevelInstanceEdit && LevelInstanceEdit->GetLevelInstanceID() == LevelInstanceActor->GetLevelInstanceID())
	{
		return LevelInstanceEdit.Get();
	}

	return nullptr;
}

bool ULevelInstanceSubsystem::IsEditingLevelInstanceDirty(const ALevelInstance* LevelInstanceActor) const
{
	const FLevelInstanceEdit* CurrentEdit = GetLevelInstanceEdit(LevelInstanceActor);
	if (!CurrentEdit)
	{
		return false;
	}

	return IsLevelInstanceEditDirty(CurrentEdit);
}

bool ULevelInstanceSubsystem::IsLevelInstanceEditDirty(const FLevelInstanceEdit* InLevelInstanceEdit) const
{
	const UWorld* EditingWorld = InLevelInstanceEdit->GetEditWorld();
	check(EditingWorld);

	TArray<UPackage*> OutDirtyPackages;
	FEditorFileUtils::GetDirtyPackages(OutDirtyPackages, [EditingWorld](UPackage* DirtyPackage)
	{
		return ULevelInstanceSubsystem::ShouldIgnoreDirtyPackage(DirtyPackage, EditingWorld);
	});

	return OutDirtyPackages.Num() > 0;
}

ALevelInstance* ULevelInstanceSubsystem::GetEditingLevelInstance() const
{
	if (LevelInstanceEdit)
	{
		return GetLevelInstance(LevelInstanceEdit->GetLevelInstanceID());
	}

	return nullptr;
}

bool ULevelInstanceSubsystem::CanEditLevelInstance(const ALevelInstance* LevelInstanceActor, FText* OutReason) const
{
	// Only allow Editing in Editor World
	if (GetWorld()->WorldType != EWorldType::Editor)
	{
		return false;
	}

	if (ULevel* LevelInstanceLevel = GetLevelInstanceLevel(LevelInstanceActor))
	{
		if (LevelInstanceLevel->GetWorldPartition())
		{
			if (OutReason)
			{
				*OutReason = LOCTEXT("CanEditPartitionedLevelInstance", "Can't edit partitioned Level Instance");
			}
			return false;
		}
	}
	
	if (LevelInstanceEdit)
	{
		if (LevelInstanceEdit->GetLevelInstanceID() == LevelInstanceActor->GetLevelInstanceID())
		{
			if (OutReason)
			{
				*OutReason = LOCTEXT("CanEditLevelInstanceAlreadyBeingEdited", "Level Instance already being edited");
			}
		}

		if (IsLevelInstanceEditDirty(LevelInstanceEdit.Get()))
		{
			if (OutReason)
			{
				*OutReason = LOCTEXT("CanEditLevelInstanceDirtyEdit", "Current Level Instance has unsaved changes and needs to be committed first.");
			}
			return false;
		}
	}
	
	if (!LevelInstanceActor->IsLevelInstancePathValid())
	{
		if (OutReason)
		{
			*OutReason = LOCTEXT("CanEditLevelInstanceDirtyInvalid", "Level Instance path is invalid");
		}
		return false;
	}

	if (GetWorld()->PersistentLevel->GetPackage()->GetName() == LevelInstanceActor->GetWorldAssetPackage())
	{
		if (OutReason)
		{
			*OutReason = LOCTEXT("CanEditLevelInstancePersistentLevel", "The Persistent level and the Level Instance are the same");
		}
		return false;
	}

	if (FLevelUtils::FindStreamingLevel(GetWorld(), *LevelInstanceActor->GetWorldAssetPackage()))
	{
		if (OutReason)
		{
			*OutReason = LOCTEXT("CanEditLevelInstanceAlreadyExists", "The same level was added to world outside of Level Instances");
		}
		return false;
	}

	return true;
}

bool ULevelInstanceSubsystem::CanCommitLevelInstance(const ALevelInstance* LevelInstanceActor, FText* OutReason) const
{
	if (!IsEditingLevelInstance(LevelInstanceActor))
	{
		if (OutReason)
		{
			*OutReason = LOCTEXT("CanCommitLevelInstanceNotEditing", "Level Instance is not currently being edited");
		}
		return false;
	}

	return true;
}

void ULevelInstanceSubsystem::EditLevelInstance(ALevelInstance* LevelInstanceActor, TWeakObjectPtr<AActor> ContextActorPtr)
{
	if (EditLevelInstanceInternal(LevelInstanceActor, ContextActorPtr, false))
	{
		ILevelInstanceEditorModule& EditorModule = FModuleManager::GetModuleChecked<ILevelInstanceEditorModule>("LevelInstanceEditor");
		EditorModule.ActivateEditorMode();
	}
}

bool ULevelInstanceSubsystem::EditLevelInstanceInternal(ALevelInstance* LevelInstanceActor, TWeakObjectPtr<AActor> ContextActorPtr, bool bRecursive)
{
	check(CanEditLevelInstance(LevelInstanceActor));
		
	FScopedSlowTask SlowTask(0, LOCTEXT("BeginEditLevelInstance", "Loading Level Instance for edit..."), !GetWorld()->IsGameWorld());
	SlowTask.MakeDialog();

	// Gather information from the context actor to try and select something meaningful after the loading
	FString ActorNameToSelect;
	if (AActor* ContextActor = ContextActorPtr.Get())
	{
		ActorNameToSelect = ContextActor->GetName();
		ForEachLevelInstanceAncestorsAndSelf(ContextActor, [&ActorNameToSelect,LevelInstanceActor](const ALevelInstance* AncestorLevelInstanceActor)
		{
			// stop when we hit the LevelInstance we are about to edit
			if (AncestorLevelInstanceActor == LevelInstanceActor)
			{
				return false;
			}
			
			ActorNameToSelect = AncestorLevelInstanceActor->GetName();
			return true;
		});
	}

	GEditor->SelectNone(false, true);
	
	// Avoid calling OnEditChild twice  on ancestors when EditLevelInstance calls itself
	if (!bRecursive)
	{
		TArray<FLevelInstanceID> AncestorIDs;
		ForEachLevelInstanceAncestors(LevelInstanceActor, [&AncestorIDs](ALevelInstance* InAncestor)
		{
			AncestorIDs.Add(InAncestor->GetLevelInstanceID());
			return true;
		});

		for (const FLevelInstanceID& AncestorID : AncestorIDs)
		{
			OnEditChild(AncestorID);
		}
	}

	// Check if there is an open (but clean) ancestor unload it before opening the LevelInstance for editing
	if (LevelInstanceEdit)
	{	
		// Only support one level of recursion to commit current edit
		check(!bRecursive);
		FLevelInstanceID PendingEditId = LevelInstanceActor->GetLevelInstanceID();
		
		check(!IsLevelInstanceEditDirty(LevelInstanceEdit.Get()));
		CommitLevelInstanceInternal(LevelInstanceEdit);

		ALevelInstance* LevelInstanceToEdit = GetLevelInstance(PendingEditId);
		check(LevelInstanceToEdit);

		return EditLevelInstanceInternal(LevelInstanceToEdit, nullptr, /*bRecursive=*/true);
	}

	// Cleanup async requests in case
	LevelInstancesToUnload.Remove(LevelInstanceActor->GetLevelInstanceID());
	LevelInstancesToLoadOrUpdate.Remove(LevelInstanceActor);
	// Unload right away
	UnloadLevelInstance(LevelInstanceActor->GetLevelInstanceID());
		
	// Load Edit LevelInstance level
	ULevelStreamingLevelInstanceEditor* LevelStreaming = ULevelStreamingLevelInstanceEditor::Load(LevelInstanceActor);
	if (!LevelStreaming)
	{
		LevelInstanceActor->LoadLevelInstance();
		return false;
	}

	LevelInstanceEdit = MakeUnique<FLevelInstanceEdit>(LevelStreaming, LevelInstanceActor->GetLevelInstanceID());
		
	// Try and select something meaningful
	AActor* ActorToSelect = nullptr;
	if (!ActorNameToSelect.IsEmpty())
	{		
		ActorToSelect = FindObject<AActor>(LevelStreaming->GetLoadedLevel(), *ActorNameToSelect);
	}

	// default to LevelInstance
	if (!ActorToSelect)
	{
		ActorToSelect = LevelInstanceActor;
	}
	LevelInstanceActor->SetIsTemporarilyHiddenInEditor(false);

	// Notify
	LevelInstanceActor->OnEdit();

	GEditor->SelectActor(ActorToSelect, true, true);

	for (const auto& Actor : LevelStreaming->LoadedLevel->Actors)
	{
		const bool bEditing = true;
		if (Actor)
		{
			Actor->PushLevelInstanceEditingStateToProxies(bEditing);
		}
	}
	
	// Edit can't be undone
	GEditor->ResetTransaction(LOCTEXT("LevelInstanceEditResetTrans", "Edit Level Instance"));

	return true;
}

ALevelInstance* ULevelInstanceSubsystem::CommitLevelInstance(ALevelInstance* LevelInstanceActor, bool bDiscardEdits, bool bPromptForSave, TSet<FName>* DirtyPackages)
{
	check(LevelInstanceEdit.Get() == GetLevelInstanceEdit(LevelInstanceActor));
	check(CanCommitLevelInstance(LevelInstanceActor));
	ALevelInstance* CommittedLevelInstance = CommitLevelInstanceInternal(LevelInstanceEdit, bDiscardEdits, bPromptForSave, DirtyPackages);

	ILevelInstanceEditorModule& EditorModule = FModuleManager::GetModuleChecked<ILevelInstanceEditorModule>("LevelInstanceEditor");
	EditorModule.DeactivateEditorMode();

	return CommittedLevelInstance;
}

ALevelInstance* ULevelInstanceSubsystem::CommitLevelInstanceInternal(TUniquePtr<FLevelInstanceEdit>& InLevelInstanceEdit, bool bDiscardEdits, bool bPromptForSave, TSet<FName>* DirtyPackages)
{
	ALevelInstance* LevelInstanceActor = GetLevelInstance(InLevelInstanceEdit->GetLevelInstanceID());
	check(InLevelInstanceEdit);
	UWorld* EditingWorld = InLevelInstanceEdit->GetEditWorld();
	check(EditingWorld);
			
	if (IsLevelInstanceEditDirty(InLevelInstanceEdit.Get()) && !bDiscardEdits)
	{
		const bool bPromptUserToSave = bPromptForSave;
		const bool bSaveMapPackages = true;
		const bool bSaveContentPackages = true;
		const bool bFastSave = false;
		const bool bNotifyNoPackagesSaved = false;
		const bool bCanBeDeclined = true;

		if (!FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages, bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined, nullptr,
			[=](UPackage* DirtyPackage)
			{
				if (DirtyPackages && DirtyPackages->Contains(DirtyPackage->GetFName()))
				{
					return false;
				}
				return ShouldIgnoreDirtyPackage(DirtyPackage, EditingWorld);
			}))
		{
			return LevelInstanceActor;
		}
	}

	FScopedSlowTask SlowTask(0, LOCTEXT("EndEditLevelInstance", "Unloading edit Level Instance..."), !GetWorld()->IsGameWorld());
	SlowTask.MakeDialog();

	GEditor->SelectNone(false, true);

	const FString EditPackage = LevelInstanceActor->GetWorldAssetPackage();

	// Remove from streaming level...
	const bool bChangesCommitted = InLevelInstanceEdit->bCommittedChanges;
	InLevelInstanceEdit.Reset();

	if (bChangesCommitted)
	{
		// Sync the AssetData so that the updated instances have the latest Actor Registry Data
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		AssetRegistry.ScanPathsSynchronous({ EditPackage }, true);
	}
	
	// Backup ID on Commit in case Actor gets recreated
	const FLevelInstanceID LevelInstanceID = LevelInstanceActor->GetLevelInstanceID();

	// Notify (Actor might get destroyed by this call if its a packed bp)
	LevelInstanceActor->OnCommit(bChangesCommitted, bPromptForSave);

	// Update pointer since BP Compilation might have invalidated LevelInstanceActor
	LevelInstanceActor = GetLevelInstance(LevelInstanceID);

	TArray<FLevelInstanceID> LevelInstancesToUpdate;
	// Gather list to update
	for (TObjectIterator<UWorld> It(RF_ClassDefaultObject | RF_ArchetypeObject, true); It; ++It)
	{
		UWorld* CurrentWorld = *It;
		if (IsValid(CurrentWorld) && CurrentWorld->GetSubsystem<ULevelInstanceSubsystem>() != nullptr)
		{
			for (TActorIterator<ALevelInstance> LevelInstanceIt(CurrentWorld); LevelInstanceIt; ++LevelInstanceIt)
			{
				ALevelInstance* CurrentLevelInstanceActor = *LevelInstanceIt;
				if (CurrentLevelInstanceActor->GetWorldAssetPackage() == EditPackage && (LevelInstanceActor == CurrentLevelInstanceActor || bChangesCommitted))
				{
					LevelInstancesToUpdate.Add(CurrentLevelInstanceActor->GetLevelInstanceID());
				}
			}
		}
	}

	// Do update
	for (const FLevelInstanceID& LevelInstanceToUpdateID : LevelInstancesToUpdate)
	{
		if (ALevelInstance* LevelInstance = GetLevelInstance(LevelInstanceToUpdateID))
		{
			LevelInstance->UpdateLevelInstance();
		}
	}

	LevelInstanceActor = GetLevelInstance(LevelInstanceID);
	
	// Notify Ancestors
	FLevelInstanceID LevelInstanceToSelectID = LevelInstanceID;
	TArray<FLevelInstanceID> AncestorIDs;
	ForEachLevelInstanceAncestors(LevelInstanceActor, [&LevelInstanceToSelectID, &AncestorIDs](ALevelInstance* AncestorLevelInstance)
	{
		LevelInstanceToSelectID = AncestorLevelInstance->GetLevelInstanceID();
		AncestorIDs.Add(AncestorLevelInstance->GetLevelInstanceID());
		return true;
	});

	for (const FLevelInstanceID& AncestorID : AncestorIDs)
	{
		OnCommitChild(AncestorID, bChangesCommitted);
	}
		
	if (ALevelInstance* Actor = GetLevelInstance(LevelInstanceToSelectID))
	{
		GEditor->SelectActor(Actor, true, true);
	}
				
	// Wait for Level Instances to be loaded
	BlockOnLoading();

	GEngine->BroadcastLevelActorListChanged();

	return GetLevelInstance(LevelInstanceID);
}

void ULevelInstanceSubsystem::SaveLevelInstanceAs(ALevelInstance* LevelInstanceActor)
{
	check(CanCommitLevelInstance(LevelInstanceActor));

	const FLevelInstanceEdit* OldLevelInstanceEdit = GetLevelInstanceEdit(LevelInstanceActor);
	check(OldLevelInstanceEdit);
	UWorld* EditingWorld = OldLevelInstanceEdit->GetEditWorld();
	check(EditingWorld);

	// Reset the level transform before saving
	OldLevelInstanceEdit->LevelStreaming->GetLoadedLevel()->ApplyWorldOffset(-LevelInstanceActor->GetTransform().GetLocation(), false);

	TArray<UObject*> OutObjects;
	FEditorFileUtils::SaveAssetsAs({ EditingWorld }, OutObjects);

	if (OutObjects.Num() == 0 || OutObjects[0] == EditingWorld)
	{
		UE_LOG(LogLevelInstance, Warning, TEXT("Failed to save Level Instance as new asset"));
		return;
	}

	UWorld* SavedWorld = StaticCast<UWorld*>(OutObjects[0]);
	// Discard edits and unload streaming level
	DiscardEdits();
	
	LevelInstanceActor->SetWorldAsset(SavedWorld);

	LoadLevelInstance(LevelInstanceActor);
	GEditor->SelectActor(LevelInstanceActor, true, true);
}

ALevelInstance* ULevelInstanceSubsystem::GetParentLevelInstance(const AActor* Actor) const
{
	check(Actor);
	const ULevel* OwningLevel = Actor->GetLevel();
	check(OwningLevel);
	return GetOwningLevelInstance(OwningLevel);
}

void ULevelInstanceSubsystem::BlockOnLoading()
{
	// Make sure blocking loads can happen and are not part of transaction
	TGuardValue<ITransaction*> TransactionGuard(GUndo, nullptr);

	// Blocking until LevelInstance is loaded and all its child LevelInstances
	while (LevelInstancesToLoadOrUpdate.Num())
	{
		UpdateStreamingState();
	}
}

void ULevelInstanceSubsystem::BlockLoadLevelInstance(ALevelInstance* LevelInstanceActor)
{
	check(!LevelInstanceActor->IsEditing());
	RequestLoadLevelInstance(LevelInstanceActor, true);

	BlockOnLoading();
}

void ULevelInstanceSubsystem::BlockUnloadLevelInstance(ALevelInstance* LevelInstanceActor)
{
	check(!LevelInstanceActor->IsEditing());
	RequestUnloadLevelInstance(LevelInstanceActor);

	BlockOnLoading();
}

bool ULevelInstanceSubsystem::HasChildEdit(const ALevelInstance* LevelInstanceActor) const
{
	const int32* ChildEditCountPtr = ChildEdits.Find(LevelInstanceActor->GetLevelInstanceID());
	return ChildEditCountPtr && *ChildEditCountPtr;
}

void ULevelInstanceSubsystem::OnCommitChild(FLevelInstanceID LevelInstanceID, bool bChildChanged)
{
	int32& ChildEditCount = ChildEdits.FindChecked(LevelInstanceID);
	check(ChildEditCount > 0);
	ChildEditCount--;

	if (ALevelInstance* LevelInstance = GetLevelInstance(LevelInstanceID))
	{
		LevelInstance->OnCommitChild(bChildChanged);
	}
}

void ULevelInstanceSubsystem::OnEditChild(FLevelInstanceID LevelInstanceID)
{
	int32& ChildEditCount = ChildEdits.FindOrAdd(LevelInstanceID, 0);
	// Child edit count can reach 2 maximum in the Context of creating a LevelInstance inside an already editing child level instance
	// through CreateLevelInstanceFrom
	check(ChildEditCount < 2);
	ChildEditCount++;

	if (ALevelInstance* LevelInstance = GetLevelInstance(LevelInstanceID))
	{
		LevelInstance->OnEditChild();
	}
}

#endif

#undef LOCTEXT_NAMESPACE
