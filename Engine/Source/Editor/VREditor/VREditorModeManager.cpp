// Copyright Epic Games, Inc. All Rights Reserved.

#include "VREditorModeManager.h"
#include "InputCoreTypes.h"
#include "VREditorMode.h"
#include "Modules/ModuleManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerInput.h"
#include "Editor.h"
#include "HAL/PlatformApplicationMisc.h"

#include "EngineGlobals.h"
#include "LevelEditor.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "EditorWorldExtension.h"
#include "ViewportWorldInteraction.h"
#include "VRModeSettings.h"
#include "Dialogs/Dialogs.h"
#include "ProjectDescriptor.h"
#include "Interfaces/IProjectManager.h"
#include "UnrealEdMisc.h"
#include "Classes/EditorStyleSettings.h"
#include "VREditorInteractor.h"

#define LOCTEXT_NAMESPACE "VREditor"

FVREditorModeManager::FVREditorModeManager() :
	CurrentVREditorMode( nullptr ),
	bEnableVRRequest( false ),
	HMDWornState( EHMDWornState::Unknown ), 
	bAddedViewportWorldInteractionExtension( false )
{
}

FVREditorModeManager::~FVREditorModeManager()
{
	CurrentVREditorMode = nullptr;
}

void FVREditorModeManager::Tick( const float DeltaTime )
{
	// You can only auto-enter VR if the setting is enabled. Other criteria are that the VR Editor is enabled in experimental settings, that you are not in PIE, and that the editor is foreground.
	IHeadMountedDisplay * const HMD = GEngine != nullptr && GEngine->XRSystem.IsValid() ? GEngine->XRSystem->GetHMDDevice() : nullptr;
	if (GetDefault<UVRModeSettings>()->bEnableAutoVREditMode
		&& HMD
		&& FPlatformApplicationMisc::IsThisApplicationForeground())
	{
		const EHMDWornState::Type LatestHMDWornState = HMD->GetHMDWornState();
		if (HMDWornState != LatestHMDWornState)
		{
			HMDWornState = LatestHMDWornState;
			if (HMDWornState == EHMDWornState::Worn && CurrentVREditorMode == nullptr)
			{
				EnableVREditor( true, false );
			}
			else if (HMDWornState == EHMDWornState::NotWorn && CurrentVREditorMode != nullptr)
			{
				EnableVREditor( false, false );
			}
		}
	}

	if(CurrentVREditorMode != nullptr && CurrentVREditorMode->WantsToExitMode())
	{
		// For a standard exit, also take the HMD out of stereo mode
		const bool bShouldDisableStereo = true;
		CloseVREditor( bShouldDisableStereo );
	}

	// Start the VR Editor mode
	if (bEnableVRRequest)
	{
		EnableVREditor(true, false);
		bEnableVRRequest = false;
	}
}

bool FVREditorModeManager::IsTickable() const
{
	const FProjectDescriptor* CurrentProject = IProjectManager::Get().GetCurrentProject();
	return CurrentProject != nullptr;
}

void FVREditorModeManager::EnableVREditor( const bool bEnable, const bool bForceWithoutHMD )
{
	// Don't do anything when the current VR Editor is already in the requested state
	if( bEnable != IsVREditorActive() )
	{
		if( bEnable && ( IsVREditorAvailable() || bForceWithoutHMD ))
		{
			UEditorStyleSettings* StyleSettings = GetMutableDefault<UEditorStyleSettings>();
			const UVRModeSettings* VRModeSettings = GetDefault<UVRModeSettings>();
			bool bUsingDefaultInteractors = true;
			if (VRModeSettings)
			{
				const TSoftClassPtr<UVREditorInteractor> InteractorClassSoft = VRModeSettings->InteractorClass;
				InteractorClassSoft.LoadSynchronous();

				if (InteractorClassSoft.IsValid())
				{
					bUsingDefaultInteractors = (InteractorClassSoft.Get() == UVREditorInteractor::StaticClass());
				}
			}

			{
				FSuppressableWarningDialog::FSetupInfo SetupInfo(LOCTEXT("VRModeEntry_Message", "VR Mode enables you to work on your project in virtual reality using motion controllers. This feature is still under development, so you may experience bugs or crashes while using it."),
					LOCTEXT("VRModeEntry_Title", "Entering VR Mode - Experimental"), "Warning_VRModeEntry", GEditorSettingsIni);

				SetupInfo.ConfirmText = LOCTEXT("VRModeEntry_ConfirmText", "Continue");
				SetupInfo.CancelText = LOCTEXT("VRModeEntry_CancelText", "Cancel");
				SetupInfo.bDefaultToSuppressInTheFuture = true;
				FSuppressableWarningDialog VRModeEntryWarning(SetupInfo);

				if (VRModeEntryWarning.ShowModal() != FSuppressableWarningDialog::Cancel)
				{
					StartVREditorMode(bForceWithoutHMD);
				}
			}
		}
		else if( !bEnable )
		{
			// For a standard exit, take the HMD out of stereo mode
			const bool bShouldDisableStereo = true;
			CloseVREditor( bShouldDisableStereo );
		}
	}
}

bool FVREditorModeManager::IsVREditorActive() const
{
	return CurrentVREditorMode != nullptr && CurrentVREditorMode->IsActive();
}

const static FName WMRSytemName = FName(TEXT("WindowsMixedRealityHMD"));
const static FName OXRSytemName = FName(TEXT("OpenXR"));
bool FVREditorModeManager::IsVREditorAvailable() const
{
	if (GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice() && GEngine->XRSystem->GetHMDDevice()->IsHMDEnabled())
	{
		// TODO: UE-71871/UE-73237 Work around for avoiding starting VRMode when using WMR
		FName SystemName = GEngine->XRSystem->GetSystemName();
		const bool bIsWMR = SystemName == WMRSytemName || SystemName == OXRSytemName;
		return !bIsWMR && !GEditor->IsPlayingSessionInEditor();
	}
	else
	{
		return false;
	}
}

bool FVREditorModeManager::IsVREditorButtonActive() const
{
	const bool bHasHMDDevice = GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice() && GEngine->XRSystem->GetHMDDevice()->IsHMDEnabled();
	return bHasHMDDevice;
}


UVREditorMode* FVREditorModeManager::GetCurrentVREditorMode()
{
	return CurrentVREditorMode;
}

void FVREditorModeManager::AddReferencedObjects( FReferenceCollector& Collector )
{
	Collector.AddReferencedObject( CurrentVREditorMode );
}

void FVREditorModeManager::StartVREditorMode( const bool bForceWithoutHMD )
{
	if (!IsEngineExitRequested())
	{
		UVREditorMode* VRMode = nullptr;
		{
			UWorld* World = GEditor->bIsSimulatingInEditor ? GEditor->PlayWorld : GWorld;
			UEditorWorldExtensionCollection* ExtensionCollection = GEditor->GetEditorWorldExtensionsManager()->GetEditorWorldExtensions(World);
			check(ExtensionCollection != nullptr);
		
			// Add viewport world interaction to the collection if not already there
			UViewportWorldInteraction* ViewportWorldInteraction = Cast<UViewportWorldInteraction>(ExtensionCollection->FindExtension(UViewportWorldInteraction::StaticClass()));
			if (ViewportWorldInteraction == nullptr)
			{
				ViewportWorldInteraction = NewObject<UViewportWorldInteraction>(ExtensionCollection);
				check(ViewportWorldInteraction != nullptr);

				ExtensionCollection->AddExtension(ViewportWorldInteraction);
				bAddedViewportWorldInteractionExtension = true;
			}
			else
			{
				ViewportWorldInteraction->UseVWInteractions();
			}

			// Create vr editor mode.
			VRMode = NewObject<UVREditorMode>();
			check(VRMode != nullptr);
			ExtensionCollection->AddExtension(VRMode);
		}

		// Tell the level editor we want to be notified when selection changes
		{
			FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>( "LevelEditor" );
			LevelEditor.OnMapChanged().AddRaw( this, &FVREditorModeManager::OnMapChanged );
		}
	
		CurrentVREditorMode = VRMode;
		CurrentVREditorMode->SetActuallyUsingVR( !bForceWithoutHMD );

		CurrentVREditorMode->Enter();

		if (CurrentVREditorMode->IsActuallyUsingVR())
		{
			OnVREditingModeEnterHandle.Broadcast();
		}
	}
}

void FVREditorModeManager::CloseVREditor( const bool bShouldDisableStereo )
{
	FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>( "LevelEditor" );
	if( LevelEditor != nullptr )
	{
		LevelEditor->OnMapChanged().RemoveAll( this );
	}

	if( CurrentVREditorMode != nullptr )
	{
		UViewportWorldInteraction* WorldInteraction = &CurrentVREditorMode->GetWorldInteraction();
		CurrentVREditorMode->Exit( bShouldDisableStereo );

		UEditorWorldExtensionCollection* Collection = CurrentVREditorMode->GetOwningCollection();
		check(Collection != nullptr);
		Collection->RemoveExtension(CurrentVREditorMode);

		if (bAddedViewportWorldInteractionExtension)
		{
			Collection->RemoveExtension(WorldInteraction);
			bAddedViewportWorldInteractionExtension = false;
		}
		else
		{
			WorldInteraction->UseLegacyInteractions();
		}

		if (CurrentVREditorMode->IsActuallyUsingVR())
		{
			OnVREditingModeExitHandle.Broadcast();
		}

		CurrentVREditorMode = nullptr;

	}
}

void FVREditorModeManager::SetDirectWorldToMeters( const float NewWorldToMeters )
{
	GWorld->GetWorldSettings()->WorldToMeters = NewWorldToMeters; //@todo VREditor: Do not use GWorld
	ENGINE_API extern float GNewWorldToMetersScale;
	GNewWorldToMetersScale = 0.0f;
}

void FVREditorModeManager::OnMapChanged( UWorld* World, EMapChangeType MapChangeType )
{
	if( CurrentVREditorMode && CurrentVREditorMode->IsActive() )
	{
		// When changing maps, we are going to close VR editor mode but then reopen it, so don't take the HMD out of stereo mode
		const bool bShouldDisableStereo = false;
		CloseVREditor( bShouldDisableStereo );
		if (MapChangeType != EMapChangeType::SaveMap)
		{
			bEnableVRRequest = true;
		}
	}
	CurrentVREditorMode = nullptr;
}

#undef LOCTEXT_NAMESPACE
