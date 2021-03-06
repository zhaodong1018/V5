// Copyright Epic Games, Inc. All Rights Reserved.

#include "AudioSynesthesiaEditorModule.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "AssetToolsModule.h"
#include "UObject/UObjectIterator.h"
#include "AudioSynesthesiaNRT.h"
#include "AudioSynesthesia.h"
#include "AssetTypeActions_AudioSynesthesiaNRT.h"
#include "AssetTypeActions_AudioSynesthesiaNRTSettings.h"
#include "AssetTypeActions_AudioSynesthesiaSettings.h"


DEFINE_LOG_CATEGORY(LogAudioSynesthesiaEditor);

class FAudioSynesthesiaEditorModule : public IAudioSynesthesiaEditorModule
{
public:
	FAudioSynesthesiaEditorModule()
	{}

	virtual void StartupModule() override
	{
		RegisterAssetActions();
	}

	virtual void ShutdownModule() override
	{}


	virtual void RegisterAssetActions() override
	{
		RegisterAudioSynesthesiaAssetActions<UAudioSynesthesiaNRT, FAssetTypeActions_AudioSynesthesiaNRT>();
		RegisterAudioSynesthesiaAssetActions<UAudioSynesthesiaNRTSettings, FAssetTypeActions_AudioSynesthesiaNRTSettings>();
		RegisterAudioSynesthesiaAssetActions<UAudioSynesthesiaSettings, FAssetTypeActions_AudioSynesthesiaSettings>();
	}

private:

	template<class SynesthesiaAssetType, class SynesthesiaAssetActionsType >
	void RegisterAudioSynesthesiaAssetActions() 
	{
		// Register the audio editor asset type actions
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		// Register base abstract class for asset filtering
		AssetTools.RegisterAssetTypeActions(MakeShared<SynesthesiaAssetActionsType>(nullptr));

		// Look for any sound effect presets to register
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* ChildClass = *It;
			if (ChildClass->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			// look for Synesthesia classes 
			UClass* ParentClass = ChildClass->GetSuperClass();
			if (ParentClass->IsChildOf(SynesthesiaAssetType::StaticClass()))
			{
				SynesthesiaAssetType* Synesthesia = ChildClass->GetDefaultObject<SynesthesiaAssetType>();
				check(Synesthesia);

				if (!RegisteredActions.Contains(Synesthesia) && Synesthesia->HasAssetActions())
				{
					RegisteredActions.Add(Synesthesia);
					AssetTools.RegisterAssetTypeActions(MakeShared<SynesthesiaAssetActionsType>(Synesthesia));
				}
			}
		}
	}	

	TSet<UObject*> RegisteredActions;
};

IMPLEMENT_MODULE( FAudioSynesthesiaEditorModule, AudioSynesthesiaEditor );
