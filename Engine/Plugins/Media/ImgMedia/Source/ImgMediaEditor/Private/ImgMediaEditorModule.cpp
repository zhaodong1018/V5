// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#include "AssetToolsModule.h"
#include "AssetTools/ImgMediaSourceActions.h"
#include "Customizations/ImgMediaSourceCustomization.h"
#include "IAssetTools.h"
#include "ImgMediaSource.h"
#include "PropertyEditorModule.h"
#include "UObject/NameTypes.h"


/**
 * Implements the ImgMediaEditor module.
 */
class FImgMediaEditorModule
	: public IModuleInterface
{
public:

	//~ IModuleInterface interface

	virtual void StartupModule() override
	{
		RegisterCustomizations();
		RegisterAssetTools();
	}

	virtual void ShutdownModule() override
	{
		UnregisterAssetTools();
		UnregisterCustomizations();
	}

protected:

	/** Register details view customizations. */
	void RegisterCustomizations()
	{
		ImgMediaSourceName = UImgMediaSource::StaticClass()->GetFName();

		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		{
			PropertyModule.RegisterCustomClassLayout(ImgMediaSourceName, FOnGetDetailCustomizationInstance::CreateStatic(&FImgMediaSourceCustomization::MakeInstance));
		}
	}

	/** Unregister details view customizations. */
	void UnregisterCustomizations()
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		{
			PropertyModule.UnregisterCustomClassLayout(ImgMediaSourceName);
		}
	}

	void RegisterAssetTools()
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		TSharedRef<IAssetTypeActions> Action = MakeShared<FImgMediaSourceActions>();
		AssetTools.RegisterAssetTypeActions(Action);
		RegisteredAssetTypeActions.Add(Action);
	}

	void UnregisterAssetTools()
	{
		FAssetToolsModule* AssetToolsModule = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools");

		if (AssetToolsModule != nullptr)
		{
			IAssetTools& AssetTools = AssetToolsModule->Get();

			for (const TSharedRef<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
			{
				AssetTools.UnregisterAssetTypeActions(Action);
			}
		}
	}

private:

	/** Class names. */
	FName ImgMediaSourceName;

	/** The collection of registered asset type actions. */
	TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;
};


IMPLEMENT_MODULE(FImgMediaEditorModule, ImgMediaEditor);
