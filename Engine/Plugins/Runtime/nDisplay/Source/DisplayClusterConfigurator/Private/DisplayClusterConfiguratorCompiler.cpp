// Copyright Epic Games, Inc. All Rights Reserved.

#include "DisplayClusterConfiguratorCompiler.h"
#include "DisplayClusterConfiguratorUtils.h"

#include "DisplayClusterConfigurationTypes.h"

#include "DisplayClusterRootActor.h"
#include "Blueprints/DisplayClusterBlueprint.h"
#include "Blueprints/DisplayClusterBlueprintGeneratedClass.h"
#include "Components/DisplayClusterCameraComponent.h"
#include "Camera/CameraComponent.h"

#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "ObjectTools.h"

#define LOCTEXT_NAMESPACE "DisplayClusterConfiguratorCompiler"


bool FDisplayClusterConfiguratorKismetCompiler::CanCompile(const UBlueprint* Blueprint)
{
	return Blueprint->IsA<UDisplayClusterBlueprint>();
}

void FDisplayClusterConfiguratorKismetCompiler::Compile(UBlueprint* Blueprint,
	const FKismetCompilerOptions& CompileOptions, FCompilerResultsLog& Results)
{
	FDisplayClusterConfiguratorKismetCompilerContext Compiler(Blueprint, Results, CompileOptions);
	Compiler.Compile();
}

bool FDisplayClusterConfiguratorKismetCompiler::GetBlueprintTypesForClass(UClass* ParentClass,
	UClass*& OutBlueprintClass, UClass*& OutBlueprintGeneratedClass) const
{
	if (ParentClass && ParentClass->IsChildOf<ADisplayClusterRootActor>())
	{
		OutBlueprintClass = UDisplayClusterBlueprint::StaticClass();
		OutBlueprintGeneratedClass = UDisplayClusterBlueprintGeneratedClass::StaticClass();
		return true;
	}

	return false;
}

FDisplayClusterConfiguratorKismetCompilerContext::FDisplayClusterConfiguratorKismetCompilerContext(UBlueprint* InBlueprint, FCompilerResultsLog& InMessageLog,
	const FKismetCompilerOptions& InCompilerOptions) : FKismetCompilerContext(InBlueprint, InMessageLog,
	                                                                          InCompilerOptions), DCGeneratedBP(nullptr)
{
}

void FDisplayClusterConfiguratorKismetCompilerContext::SpawnNewClass(const FString& NewClassName)
{
	DCGeneratedBP = FindObject<UDisplayClusterBlueprintGeneratedClass>(Blueprint->GetOutermost(), *NewClassName);

	if (DCGeneratedBP == nullptr)
	{
		DCGeneratedBP = NewObject<UDisplayClusterBlueprintGeneratedClass>(Blueprint->GetOutermost(), *NewClassName, RF_Public | RF_Transactional);
	}
	else
	{
		// Already existed, but wasn't linked in the Blueprint yet due to load ordering issues
		FBlueprintCompileReinstancer::Create(DCGeneratedBP);
	}
	NewClass = DCGeneratedBP;
}

void FDisplayClusterConfiguratorKismetCompilerContext::OnNewClassSet(UBlueprintGeneratedClass* ClassToUse)
{
	DCGeneratedBP = CastChecked<UDisplayClusterBlueprintGeneratedClass>(ClassToUse);
}

void FDisplayClusterConfiguratorKismetCompilerContext::PreCompile()
{
	Super::PreCompile();
	ValidateConfiguration();
}

void FDisplayClusterConfiguratorKismetCompilerContext::SaveSubObjectsFromCleanAndSanitizeClass(
	FSubobjectCollection& SubObjectsToSave, UBlueprintGeneratedClass* ClassToClean)
{
	SavedSubObjects.Empty();
	FKismetCompilerContext::SaveSubObjectsFromCleanAndSanitizeClass(SubObjectsToSave, ClassToClean);

	UDisplayClusterBlueprint* DCBlueprint = CastChecked<UDisplayClusterBlueprint>(Blueprint);
	if (UDisplayClusterConfigurationData* ConfigData = DCBlueprint->GetConfig())
	{
		SubObjectsToSave.AddObject(ConfigData);

		// Find all subobjects owned by this object.. same as how `AddObject` above works.
		{
			SavedSubObjects.Add(ConfigData);
			ForEachObjectWithOuter(ConfigData, [this](UObject* Child)
			{
				SavedSubObjects.Add(Child);
			});
		}
	}
}

void FDisplayClusterConfiguratorKismetCompilerContext::CopyTermDefaultsToDefaultObject(UObject* DefaultObject)
{
	Super::CopyTermDefaultsToDefaultObject(DefaultObject);

	UDisplayClusterBlueprint* DCBlueprint = CastChecked<UDisplayClusterBlueprint>(Blueprint);
	if (DCBlueprint->HasAnyFlags(RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_NeedInitialization))
	{
		return;
	}
	
	if (Blueprint->bIsNewlyCreated)
	{
		ADisplayClusterRootActor* RootActor = CastChecked<ADisplayClusterRootActor>(DefaultObject);
		RootActor->PreviewNodeId = DisplayClusterConfigurationStrings::gui::preview::PreviewNodeAll;
	}

	auto GetCorrectOuter = [this](UObject* InObject)
	{
		check (InObject);
		
		UObject* InOuter = InObject->GetOuter();
		check(InOuter);

		// Find the saved sub-object outer.
		if (const TArray<UObject*>::ElementType* FoundSavedOuter = SavedSubObjects.FindByPredicate([InOuter](const UObject* SubObject)
		{
			check(SubObject);
			return InOuter->GetName() == SubObject->GetName();
		}))
		{
			return *FoundSavedOuter;
		}
		
		// Fallback to original outer.
		return InOuter;
	};

	// Restore all saved sub-objects to their original locations. The sub-objects are dynamically added and
	// already correct by this point. They need to be restored so transaction history can be preserved and undos
	// function after a compile. If we ever add in compile modification of sub-objects under
	// CopyTermDefaultsToDefaultObject then we'll need to adjust this logic.
	for (UObject* SavedSubObject : SavedSubObjects)
	{
		if (UObject* NewObject = static_cast<UObject*>(FindObjectWithOuter(DefaultObject, SavedSubObject->GetClass(), *SavedSubObject->GetName())))
		{
			const ERenameFlags RenFlags = REN_DontCreateRedirectors | REN_ForceNoResetLoaders | REN_NonTransactional | REN_DoNotDirty;
			
			const bool bSubObjectIsNew = NewObject != SavedSubObject;
			ensure(bSubObjectIsNew);

			UObject* DesiredOuter = GetCorrectOuter(NewObject);
			check(DesiredOuter != GetTransientPackage());
			
			if (bSubObjectIsNew)
			{
				// Invalidate the newly created sub-object.
				NewObject->Rename(nullptr, GetTransientPackage(), RenFlags);
				NewObject->SetFlags(RF_Transient);
				FLinkerLoad::InvalidateExport(NewObject);
			}

			if (DesiredOuter != SavedSubObject->GetOuter())
			{
				// Restore the original sub-object.
				SavedSubObject->Rename(nullptr, DesiredOuter, RenFlags);
			}

			if (bSubObjectIsNew)
			{
				// Update all properties to point to the original sub-object.
				TArray<UObject*> ObjectsToReplace { NewObject };
				ObjectTools::ForceReplaceReferences(SavedSubObject, ObjectsToReplace);
			}
		}		
	}
}

void FDisplayClusterConfiguratorKismetCompilerContext::ValidateConfiguration()
{
	UDisplayClusterBlueprint* DCBlueprint = CastChecked<UDisplayClusterBlueprint>(Blueprint);
	if (Blueprint->bIsNewlyCreated)
	{
		return;
	}
	
	UDisplayClusterConfigurationData* BlueprintData = DCBlueprint->GetOrLoadConfig();
	if (!BlueprintData)
	{
		MessageLog.Error(*LOCTEXT("NoConfigError", "Critical Error: Configuration data not found!").ToString());
		return;
	}

	if (!BlueprintData->Cluster)
	{
		MessageLog.Error(*LOCTEXT("NoClusterError", "No cluster information found!").ToString());
		return;
	}

	if (BlueprintData->Cluster->Nodes.Num() == 0)
	{
		MessageLog.Warning(*LOCTEXT("NoClusterNodesWarning", "No cluster nodes found. Please add a cluster node.").ToString());
		return;
	}
	
	if (!FDisplayClusterConfiguratorUtils::IsMasterNodeInConfig(BlueprintData))
	{
		MessageLog.Warning(*LOCTEXT("NoMasterNodeWarning", "Master cluster node not set. Please set a master node.").ToString());
	}

	bool bAtLeastOneViewportFound = false;

	for (const auto& ClusterNodes : BlueprintData->Cluster->Nodes)
	{
		if (ClusterNodes.Value->Viewports.Num() > 0)
		{
			// Pass with at least one set.
			bAtLeastOneViewportFound = true;
			
			for (const auto& Viewport : ClusterNodes.Value->Viewports)
			{
				if (Viewport.Value->ProjectionPolicy.Type.IsEmpty())
				{
					MessageLog.Warning(*LOCTEXT("NoPolicyError", "No projection policy assigned to viewport @@.").ToString(), Viewport.Value);
				}
			}
		}
	}

	if (!bAtLeastOneViewportFound)
	{
		MessageLog.Warning(*LOCTEXT("NoViewportsError", "No viewports found. Please add a viewport.").ToString());
	}
}

#undef LOCTEXT_NAMESPACE
