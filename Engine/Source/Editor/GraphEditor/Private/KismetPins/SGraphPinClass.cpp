// Copyright Epic Games, Inc. All Rights Reserved.


#include "KismetPins/SGraphPinClass.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Editor.h"
#include "ClassViewerModule.h"
#include "ClassViewerFilter.h"
#include "ScopedTransaction.h"
#include "AssetRegistryModule.h"
#include "K2Node_Variable.h"
#include "K2Node_StructOperation.h"
#include "EdGraphSchema_K2.h"

#define LOCTEXT_NAMESPACE "SGraphPinClass"

/////////////////////////////////////////////////////
// SGraphPinClass

void SGraphPinClass::Construct(const FArguments& InArgs, UEdGraphPin* InGraphPinObj)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InGraphPinObj);
	bAllowAbstractClasses = true;
}

FReply SGraphPinClass::OnClickUse()
{
	FEditorDelegates::LoadSelectedAssetsIfNeeded.Broadcast();

	if(GraphPinObj && GraphPinObj->GetSchema())
	{
		const UClass* PinRequiredParentClass = Cast<const UClass>(GraphPinObj->PinType.PinSubCategoryObject.Get());
		ensure(PinRequiredParentClass);

		const UClass* SelectedClass = GEditor->GetFirstSelectedClass(PinRequiredParentClass);
		if(SelectedClass)
		{
			const FScopedTransaction Transaction(NSLOCTEXT("GraphEditor", "ChangeClassPinValue", "Change Class Pin Value"));
			GraphPinObj->Modify();

			GraphPinObj->GetSchema()->TrySetDefaultObject(*GraphPinObj, const_cast<UClass*>(SelectedClass));
		}
	}

	return FReply::Handled();
}

class FGraphPinFilter : public IClassViewerFilter
{
public:
	/** Package containing the graph pin */
	const UPackage* GraphPinOutermostPackage;

	/** All children of these classes will be included unless filtered out by another setting. */
	TSet< const UClass* > AllowedChildrenOfClasses;

	const UClass* RequiredInterface = nullptr;

	bool bAllowAbstractClasses = true;

	virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs ) override
	{
		// If it appears on the allowed child-of classes list (or there is nothing on that list)
		bool Result = (InFilterFuncs->IfInChildOfClassesSet(AllowedChildrenOfClasses, InClass) != EFilterReturn::Failed);
		if (Result)
		{
			check(InClass != nullptr);
			const UPackage* ClassPackage = InClass->GetOutermost();
			check(ClassPackage != nullptr);
		
			// Don't allow classes from a loaded map (e.g. LSBPs) unless we're already working inside that package context. Otherwise, choosing the class would lead to a GLEO at save time.
			Result &= !ClassPackage->ContainsMap() || ClassPackage == GraphPinOutermostPackage;
			Result &= !InClass->HasAnyClassFlags(CLASS_Hidden | CLASS_HideDropDown);
			Result &= bAllowAbstractClasses || !InClass->HasAnyClassFlags(CLASS_Abstract);
			// either there is not a required interface, or our target class DOES implement that interface
			Result &= (RequiredInterface == nullptr || InClass->ImplementsInterface(RequiredInterface));
		}

		return Result;
	}

	virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
	{
		return (InFilterFuncs->IfInChildOfClassesSet( AllowedChildrenOfClasses, InUnloadedClassData) != EFilterReturn::Failed) 
			&& (!InUnloadedClassData->HasAnyClassFlags(CLASS_Hidden | CLASS_HideDropDown))
			&& (bAllowAbstractClasses || !InUnloadedClassData->HasAnyClassFlags(CLASS_Abstract))
			// either there is not a required interface, or our target class DOES implement that interface
			&& (RequiredInterface == nullptr || InUnloadedClassData->ImplementsInterface(RequiredInterface));
	}
};

TSharedRef<SWidget> SGraphPinClass::GenerateAssetPicker()
{
	FClassViewerModule& ClassViewerModule = FModuleManager::LoadModuleChecked<FClassViewerModule>("ClassViewer");

	// Fill in options
	FClassViewerInitializationOptions Options;
	Options.Mode = EClassViewerMode::ClassPicker;
	Options.bShowNoneOption = true;

	// Get the min. spec for the classes allowed
	const UClass* PinRequiredParentClass = Cast<const UClass>(GraphPinObj->PinType.PinSubCategoryObject.Get());
	ensure(PinRequiredParentClass);
	if (PinRequiredParentClass == NULL)
	{
		PinRequiredParentClass = UObject::StaticClass();
	}

	TSharedPtr<FGraphPinFilter> Filter = MakeShareable(new FGraphPinFilter);
	Filter->bAllowAbstractClasses = bAllowAbstractClasses;

	// Check with the node to see if there is any "AllowAbstract" metadata for the pin
	FString AllowAbstractString = GraphPinObj->GetOwningNode()->GetPinMetaData(GraphPinObj->PinName, FBlueprintMetadata::MD_AllowAbstractClasses);

	// Override bAllowAbstractClasses is the AllowAbstract metadata was set
	if (!AllowAbstractString.IsEmpty())
	{
		Filter->bAllowAbstractClasses = AllowAbstractString.ToBool();
	}

	Options.ClassFilters.Add(Filter.ToSharedRef());

	Filter->AllowedChildrenOfClasses.Add(PinRequiredParentClass);
	Filter->GraphPinOutermostPackage = GraphPinObj->GetOuter()->GetOutermost();

	if (UEdGraphNode* ParentNode = GraphPinObj->GetOwningNode())
	{
		FString PossibleInterface = ParentNode->GetPinMetaData(GraphPinObj->PinName, TEXT("MustImplement"));
		if (!PossibleInterface.IsEmpty())
		{
			Filter->RequiredInterface = FindObject<UClass>(ANY_PACKAGE, *PossibleInterface);
		}
	}

	return
		SNew(SBox)
		.WidthOverride(280)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.MaxHeight(500)
			[ 
				SNew(SBorder)
				.Padding(4)
				.BorderImage( FEditorStyle::GetBrush("ToolPanel.GroupBorder") )
				[
					ClassViewerModule.CreateClassViewer(Options, FOnClassPicked::CreateSP(this, &SGraphPinClass::OnPickedNewClass))
				]
			]			
		];
}

FOnClicked SGraphPinClass::GetOnUseButtonDelegate()
{
	return FOnClicked::CreateSP( this, &SGraphPinClass::OnClickUse );
}

void SGraphPinClass::OnPickedNewClass(UClass* ChosenClass)
{
	if(GraphPinObj->IsPendingKill())
	{
		return;
	}

	FString NewPath;
	if (ChosenClass)
	{
		NewPath = ChosenClass->GetPathName();
	}

	if(GraphPinObj->GetDefaultAsString() != NewPath)
	{
		const FScopedTransaction Transaction( NSLOCTEXT("GraphEditor", "ChangeClassPinValue", "Change Class Pin Value" ) );
		GraphPinObj->Modify();

		AssetPickerAnchor->SetIsOpen(false);
		GraphPinObj->GetSchema()->TrySetDefaultObject(*GraphPinObj, ChosenClass);
	}
}

FText SGraphPinClass::GetDefaultComboText() const
{ 
	return LOCTEXT( "DefaultComboText", "Select Class" );
}

const FAssetData& SGraphPinClass::GetAssetData(bool bRuntimePath) const
{
	if (bRuntimePath)
	{
		// For runtime use the default path
		return SGraphPinObject::GetAssetData(bRuntimePath);
	}

	FString CachedRuntimePath = CachedEditorAssetData.ObjectPath.ToString() + TEXT("_C");

	if (GraphPinObj->DefaultObject)
	{
		if (!GraphPinObj->DefaultObject->GetPathName().Equals(CachedRuntimePath, ESearchCase::CaseSensitive))
		{
			// This will cause it to use the UBlueprint
			CachedEditorAssetData = FAssetData(GraphPinObj->DefaultObject, false);
		}
	}
	else if (!GraphPinObj->DefaultValue.IsEmpty())
	{
		if (!GraphPinObj->DefaultValue.Equals(CachedRuntimePath, ESearchCase::CaseSensitive))
		{
			FString EditorPath = GraphPinObj->DefaultValue;
			EditorPath.RemoveFromEnd(TEXT("_C"));
			const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			CachedEditorAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FName(*EditorPath));

			if (!CachedEditorAssetData.IsValid())
			{
				FString PackageName = FPackageName::ObjectPathToPackageName(EditorPath);
				FString PackagePath = FPackageName::GetLongPackagePath(PackageName);
				FString ObjectName = FPackageName::ObjectPathToObjectName(EditorPath);

				// Fake one
				CachedEditorAssetData = FAssetData(FName(*PackageName), FName(*PackagePath), FName(*ObjectName), UObject::StaticClass()->GetFName());
			}
		}
	}
	else
	{
		if (CachedEditorAssetData.IsValid())
		{
			CachedEditorAssetData = FAssetData();
		}
	}

	return CachedEditorAssetData;
}

#undef LOCTEXT_NAMESPACE
