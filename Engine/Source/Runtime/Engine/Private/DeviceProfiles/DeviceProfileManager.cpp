// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeviceProfiles/DeviceProfileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "UObject/Package.h"
#include "SceneManagement.h"
#include "SystemSettings.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "IDeviceProfileSelectorModule.h"
#include "Misc/DataDrivenPlatformInfoRegistry.h"
#if WITH_EDITOR
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "PlatformInfo.h"
#include "PIEPreviewDeviceProfileSelectorModule.h"
#endif
#include "ProfilingDebugging/CsvProfiler.h"
#include "DeviceProfiles/DeviceProfileFragment.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceProfileManager, Log, All);

static TAutoConsoleVariable<FString> CVarDeviceProfileOverride(
	TEXT("dp.Override"),
	TEXT(""),
	TEXT("DeviceProfile override - setting this will use the named DP as the active DP. In addition, it will restore any\n")
	TEXT(" previous overrides before setting (does a dp.OverridePop before setting after the first time).\n")
	TEXT(" The commandline -dp option will override this on startup, but not when setting this at runtime\n"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarAllowScalabilityGroupsToChangeAtRuntime(
	TEXT("dp.AllowScalabilityGroupsToChangeAtRuntime"),
	0,
	TEXT("If true, device profile scalability bucket cvars will be set with scalability")
	TEXT("priority which allows them to be changed at runtime. Off by default."),
	ECVF_Default);

TMap<FString, FString> UDeviceProfileManager::DeviceProfileScalabilityCVars;

FString UDeviceProfileManager::BackupSuffix = TEXT("_Backup");
TMap<FString, FString> UDeviceProfileManager::PushedSettings;
TArray<FSelectedFragmentProperties> UDeviceProfileManager::PlatformFragmentsSelected;

UDeviceProfileManager* UDeviceProfileManager::DeviceProfileManagerSingleton = nullptr;

UDeviceProfileManager& UDeviceProfileManager::Get(bool bFromPostCDOContruct)
{
	if (DeviceProfileManagerSingleton == nullptr)
	{
		static bool bEntered = false;
		if (bEntered && bFromPostCDOContruct)
		{
			return *(UDeviceProfileManager*)0x3; // we know that the return value is never used, linux hates null here, which would be less weird. 
		}
		bEntered = true;
		DeviceProfileManagerSingleton = NewObject<UDeviceProfileManager>();

		DeviceProfileManagerSingleton->AddToRoot();
		if (!FPlatformProperties::RequiresCookedData())
		{
			DeviceProfileManagerSingleton->LoadProfiles();
		}

		// always start with an active profile, even if we create it on the spot
		UDeviceProfile* ActiveProfile = DeviceProfileManagerSingleton->FindProfile(GetPlatformDeviceProfileName());
		DeviceProfileManagerSingleton->SetActiveDeviceProfile(ActiveProfile);

		// now we allow the cvar changes to be acknowledged
		CVarDeviceProfileOverride.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable)
		{
			UDeviceProfileManager::Get().HandleDeviceProfileOverrideChange();
		}));

		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("dp.Override.Restore"),
			TEXT("Restores any cvars set by dp.Override to their previous value"),
			FConsoleCommandDelegate::CreateLambda([]()
			{
				UDeviceProfileManager::Get().RestoreDefaultDeviceProfile();
			}),
			ECVF_Default
		);

		InitializeSharedSamplerStates();
	}
	return *DeviceProfileManagerSingleton;
}

// Read the cvars from a [DeviceProfileFragment] section.
static bool GetFragmentCVars(const FString& FragmentName, const FString& CVarArrayName, TArray<FString>& FragmentCVarsINOUT, FConfigCacheIni* ConfigSystem)
{
	FString FragmentSectionName = FString::Printf(TEXT("%s %s"), *FragmentName, *UDeviceProfileFragment::StaticClass()->GetName());
	if (ConfigSystem->DoesSectionExist(*FragmentSectionName, GDeviceProfilesIni))
	{
		TArray<FString> FragmentCVars;
		ConfigSystem->GetArray(*FragmentSectionName, *CVarArrayName, FragmentCVars, GDeviceProfilesIni);
		UE_CLOG(FragmentCVars.Num() > 0, LogInit, Log, TEXT("Including %s from fragment: %s"), *CVarArrayName, *FragmentName);
		FragmentCVarsINOUT += FragmentCVars;
	}
	else
	{
		UE_LOG(LogInit, Error, TEXT("Could not find device profile fragment %s."), *FragmentName);
		return false;
	}
	return true;
}

// read the requested fragment from within the +FragmentIncludes= array of a DP.
static void GetCVarsFromDPFragmentIncludes(const FString& CurrentSectionName, const FString& CVarArrayName, TArray<FString>& FragmentCVarsINOUT, FConfigCacheIni* ConfigSystem)
{
	FString FragmentIncludes = TEXT("FragmentIncludes");
	TArray<FString> FragmentIncludeArray;
	ConfigSystem->GetArray(*CurrentSectionName, *FragmentIncludes, FragmentIncludeArray, GDeviceProfilesIni);

	for(const FString& FragmentInclude : FragmentIncludeArray)
	{
		GetFragmentCVars(FragmentInclude, CVarArrayName, FragmentCVarsINOUT, ConfigSystem);
	}
}


static void ExpandScalabilityCVar(FConfigCacheIni* ConfigSystem, const FString& CVarKey, const FString CVarValue, TMap<FString, FString>& ExpandedCVars, bool bOverwriteExistingValue)
{
	// load scalability settings directly from ini instead of using scalability system, so as not to inadvertantly mess anything up
	// if the DP had sg.ResolutionQuality=3, we would read [ResolutionQuality@3]
	FString SectionName = FString::Printf(TEXT("%s@%s"), *CVarKey.Mid(3), *CVarValue);
	// walk over the scalability section and add them in, unless already done
	FConfigSection* ScalabilitySection = ConfigSystem->GetSectionPrivate(*SectionName, false, true, GScalabilityIni);
	if (ScalabilitySection != nullptr)
	{
		for (const auto& Pair : *ScalabilitySection)
		{
			FString ScalabilityKey = Pair.Key.ToString();
			if (bOverwriteExistingValue || !ExpandedCVars.Contains(ScalabilityKey))
			{
				ExpandedCVars.Add(ScalabilityKey, Pair.Value.GetValue());
			}
		}
	}
}

void UDeviceProfileManager::ProcessDeviceProfileIniSettings(const FString& DeviceProfileName, EDeviceProfileMode Mode)
{
	FConfigCacheIni* ConfigSystem = GConfig;
	if (Mode == EDeviceProfileMode::DPM_CacheValues)
	{
#if ALLOW_OTHER_PLATFORM_CONFIG
		// caching is not done super early, so we can assume DPs have been found now
		UDeviceProfile* Profile = UDeviceProfileManager::Get().FindProfile(DeviceProfileName, false);
		check(Profile);
		// use the DP's platform's configs, NOT the running platform
		ConfigSystem = FConfigCacheIni::ForPlatform(*Profile->DeviceType);
#else
		checkNoEntry();
#endif
	}

	if (Mode == EDeviceProfileMode::DPM_SetCVars)
	{
		UE_LOG(LogDeviceProfileManager, Log, TEXT("Applying CVar settings loaded from the selected device profile: [%s]"), *DeviceProfileName);
	}

	check(ConfigSystem);

	TArray< FString > AvailableProfiles;
	GConfig->GetSectionNames( GDeviceProfilesIni, AvailableProfiles );

	// Look up the ini for this tree as we are far too early to use the UObject system
	AvailableProfiles.Remove( TEXT( "DeviceProfiles" ) );

	// Next we need to create a hierarchy of CVars from the Selected Device Profile, to it's eldest parent
	// if we are just caching, this also contains the set of all CVars (including expanding scalability groups)
	TMap<FString, FString> CVarsAlreadySetList;

	// reset some global state for "active DP" mode
	if (Mode == EDeviceProfileMode::DPM_SetCVars)
	{
		DeviceProfileScalabilityCVars.Empty();

		// we should have always pushed away old values by the time we get here
		check(PushedSettings.Num() == 0);

#if !UE_BUILD_SHIPPING
#if PLATFORM_ANDROID
		// allow ConfigRules to override cvars first
		TMap<FString, FString> ConfigRules = FAndroidMisc::GetConfigRulesTMap();
		for (const TPair<FString, FString>& Pair : ConfigRules)
		{
			FString Key = Pair.Key;
			if (Key.StartsWith("cvar_"))
			{
				FString CVarKey = Key.RightChop(5);
				FString CVarValue = Pair.Value;

				UE_LOG(LogDeviceProfileManager, Log, TEXT("Setting ConfigRules Device Profile CVar: [[%s:%s]]"), *CVarKey, *CVarValue);

				// set it and remember it
				OnSetCVarFromIniEntry(*GDeviceProfilesIni, *CVarKey, *CVarValue, ECVF_SetByDeviceProfile);
				CVarsAlreadySetList.Add(CVarKey, CVarValue);
			}
		}
#endif
#endif

#if !UE_BUILD_SHIPPING
		// pre-apply any -dpcvars= items, so that they override anything in the DPs
		FString DPCVarString;
		if (FParse::Value(FCommandLine::Get(), TEXT("DPCVars="), DPCVarString, false) || FParse::Value(FCommandLine::Get(), TEXT("DPCVar="), DPCVarString, false))
		{
			// look over a list of cvars
			TArray<FString> DPCVars;
			DPCVarString.ParseIntoArray(DPCVars, TEXT(","), true);
			for (const FString& DPCVar : DPCVars)
			{
				// split up each Key=Value pair
				FString CVarKey, CVarValue;
				if (DPCVar.Split(TEXT("="), &CVarKey, &CVarValue))
				{
					UE_LOG(LogDeviceProfileManager, Log, TEXT("Setting CommandLine Device Profile CVar: [[%s:%s]]"), *CVarKey, *CVarValue);

					// set it and remember it (no thanks, Ron Popeil)
					OnSetCVarFromIniEntry(*GDeviceProfilesIni, *CVarKey, *CVarValue, ECVF_SetByDeviceProfile);
					CVarsAlreadySetList.Add(CVarKey, CVarValue);
				}
			}
		}
#endif

		// Preload a cvar we rely on
		if (FConfigSection* Section = ConfigSystem->GetSectionPrivate(TEXT("ConsoleVariables"), false, true, *GEngineIni))
		{
			static FName AllowScalabilityAtRuntimeName = TEXT("dp.AllowScalabilityGroupsToChangeAtRuntime");
			if (const FConfigValue* Value = Section->Find(AllowScalabilityAtRuntimeName))
			{
				const FString& KeyString = AllowScalabilityAtRuntimeName.ToString();
				const FString& ValueString = Value->GetValue();
				OnSetCVarFromIniEntry(*GEngineIni, *KeyString, *ValueString, ECVF_SetBySystemSettingsIni);
			}
		}
	}

	FString SectionSuffix = *FString::Printf(TEXT(" %s"), *UDeviceProfile::StaticClass()->GetName());

#if WITH_EDITOR
	TSet<FString> PreviewAllowlistCVars;
	TSet<FString> PreviewDenylistCVars;
	bool bFoundAllowDeny = false;
	if (Mode == EDeviceProfileMode::DPM_CacheValues)
	{
		// Walk up the device profile tree to find the most specific device profile with a Denylist or Allowlist of cvars to apply, and use those Allow/Denylists.
		for(FString CurrentProfileName = DeviceProfileName, CurrentSectionName = DeviceProfileName + SectionSuffix;
			PreviewAllowlistCVars.Num()==0 && PreviewDenylistCVars.Num()==0 && !CurrentProfileName.IsEmpty() && AvailableProfiles.Contains(CurrentSectionName);
			CurrentProfileName = GConfig->GetStr(*CurrentSectionName, TEXT("BaseProfileName"), GDeviceProfilesIni), CurrentSectionName = CurrentProfileName + SectionSuffix)
		{
			TArray<FString> TempAllowlist;
			GConfig->GetArray(*CurrentSectionName, TEXT("PreviewAllowlistCVars"), TempAllowlist, GDeviceProfilesIni);
			for( FString& Item : TempAllowlist)
			{
				PreviewAllowlistCVars.Add(Item);
			}

			TArray<FString> TempDenylist;
			GConfig->GetArray(*CurrentSectionName, TEXT("PreviewDenylistCVars"), TempDenylist, GDeviceProfilesIni);
			for (FString& Item : TempDenylist)
			{
				PreviewDenylistCVars.Add(Item);
			}
		}
	}
#endif


	TSet<FString> FragmentCVarKeys;
	TArray<FString> SelectedFragmentCVars;
	TArray<FSelectedFragmentProperties> FragmentsSelected;

	// Process the fragment matching rules.
	// Only perform the matching process for the base DP for simplicity.
	// Perform the matching process if no fragments were selected or we are previewing. re-use the matched array if already present.
	if (Mode == EDeviceProfileMode::DPM_CacheValues || PlatformFragmentsSelected.Num() == 0)
	{
		// run the matching rules.
		FragmentsSelected = FindMatchingFragments(DeviceProfileName, ConfigSystem);
		if (Mode != EDeviceProfileMode::DPM_CacheValues)
		{
			// Store the newly selected fragments.
			PlatformFragmentsSelected = FragmentsSelected;
		}
	}
	else if (Mode != EDeviceProfileMode::DPM_CacheValues)
	{
		// use the existing selected fragment state.
		FragmentsSelected = PlatformFragmentsSelected;
	}

	// Here we gather the cvars from selected fragments in reverse order
	for (int i = FragmentsSelected.Num() - 1; i >= 0; i--)
	{
		const FSelectedFragmentProperties& SelectedFragment = FragmentsSelected[i];
		if (SelectedFragment.bEnabled)
		{
			TArray<FString> FragmentCVars;
			GetFragmentCVars(*SelectedFragment.Fragment, TEXT("CVars"), FragmentCVars, ConfigSystem);
			for (const FString& FragCVar : FragmentCVars)
			{
				FString CVarKey, CVarValue;
				if (FragCVar.Split(TEXT("="), &CVarKey, &CVarValue))
				{
					if (!FragmentCVarKeys.Find(CVarKey))
					{
						FragmentCVarKeys.Add(CVarKey);
						SelectedFragmentCVars.Add(FragCVar);
					}
				}
			}
		}
	}

	// For each device profile, starting with the selected and working our way up the BaseProfileName tree,
	// Find all CVars and set them 
	FString BaseDeviceProfileName = DeviceProfileName;
	bool bReachedEndOfTree = BaseDeviceProfileName.IsEmpty();
	while( bReachedEndOfTree == false ) 
	{
		FString CurrentSectionName = BaseDeviceProfileName + SectionSuffix;
		
		// Check the profile was available.
		bool bProfileExists = AvailableProfiles.Contains( CurrentSectionName );
		if( bProfileExists )
		{
			// put this up in some shared code somewhere in FGenericPlatformMemory
			const TCHAR* BucketNames[] = {
				TEXT("_Largest"),
				TEXT("_Larger"),
				TEXT("_Default"),
				TEXT("_Smaller"),
				TEXT("_Smallest"),
                TEXT("_Tiniest"),
			};

			for (int Pass = 0; Pass < 2; Pass++)
			{
				// apply the current memory bucket CVars in Pass 0, regular CVars in pass 1 (anything set in Pass 0 won't be set in pass 1)
				FString ArrayName = TEXT("CVars");
				if (Pass == 0)
				{
					// assume default when caching for another platform, since we don' thave a current device to emulate (maybe we want to be able to pass in override memory bucket?)
					if (Mode == EDeviceProfileMode::DPM_CacheValues)
					{
						ArrayName += TEXT("_Default");
					}
					else
					{
						ArrayName += BucketNames[(int32)FPlatformMemory::GetMemorySizeBucket()];
					}
				}

				TArray< FString > CurrentProfilesCVars, FragmentCVars;
				GetCVarsFromDPFragmentIncludes(*CurrentSectionName, *ArrayName, FragmentCVars, ConfigSystem);
				ConfigSystem->GetArray(*CurrentSectionName, *ArrayName, CurrentProfilesCVars, GDeviceProfilesIni);

				if (FragmentCVars.Num())
				{
					// Prepend fragments to CurrentProfilesCVars, fragment cvars should be first so the DP's cvars take priority.
					Swap(CurrentProfilesCVars, FragmentCVars);
					CurrentProfilesCVars += FragmentCVars;
				}

				// now add the selected fragments at the end so these override the DP.
				CurrentProfilesCVars += SelectedFragmentCVars;
				SelectedFragmentCVars.Empty();

				// Iterate over the profile and make sure we do not have duplicate CVars
				{
					TMap< FString, FString > ValidCVars;
					for (TArray< FString >::TConstIterator CVarIt(CurrentProfilesCVars); CVarIt; ++CVarIt)
					{
						FString CVarKey, CVarValue;
						if ((*CVarIt).Split(TEXT("="), &CVarKey, &CVarValue))
						{
							ValidCVars.Add(CVarKey, CVarValue);
						}
					}

					// Empty the current list, and replace with the processed CVars. This removes duplicates
					CurrentProfilesCVars.Empty();

					for (TMap< FString, FString >::TConstIterator ProcessedCVarIt(ValidCVars); ProcessedCVarIt; ++ProcessedCVarIt)
					{
						CurrentProfilesCVars.Add(FString::Printf(TEXT("%s=%s"), *ProcessedCVarIt.Key(), *ProcessedCVarIt.Value()));
					}

				}

				// Iterate over this profiles cvars and set them if they haven't been already.
				for (TArray< FString >::TConstIterator CVarIt(CurrentProfilesCVars); CVarIt; ++CVarIt)
				{
					FString CVarKey, CVarValue;
					if ((*CVarIt).Split(TEXT("="), &CVarKey, &CVarValue))
					{
						if (!CVarsAlreadySetList.Find(CVarKey))
						{
#if WITH_EDITOR
							if (Mode == EDeviceProfileMode::DPM_CacheValues)
							{
								if (PreviewDenylistCVars.Contains(CVarKey))
								{
									UE_LOG(LogInit, Log, TEXT("Skipping Device Profile CVar due to PreviewDenylistCVars: [[%s]]"), *CVarKey);
									continue;
								}

								if (PreviewAllowlistCVars.Num() > 0 && !PreviewAllowlistCVars.Contains(CVarKey))
								{
									UE_LOG(LogInit, Log, TEXT("Skipping Device Profile CVar due to PreviewAllowlistCVars: [[%s]]"), *CVarKey);
									continue;
								}
							}
#endif

							if (Mode == EDeviceProfileMode::DPM_SetCVars)
							{
								IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarKey);
								if (CVar)
								{
									// remember the previous value
									FString OldValue = CVar->GetString();
									PushedSettings.Add(CVarKey, OldValue);

									// indicate we are pushing, not setting
									UE_LOG(LogDeviceProfileManager, Log, TEXT("Pushing Device Profile CVar: [[%s:%s -> %s]]"), *CVarKey, *OldValue, *CVarValue);
								}
								else
								{
									UE_LOG(LogDeviceProfileManager, Warning, TEXT("Creating unregistered Device Profile CVar: [[%s:%s]]"), *CVarKey, *CVarValue);
								}
							}

							// General scalability bucket cvars are set as a suggested default but can be overridden by game settings.
							bool bIsScalabilityBucket = CVarKey.StartsWith(TEXT("sg."));

							if (Mode == EDeviceProfileMode::DPM_CacheValues)
							{
								if (bIsScalabilityBucket)
								{
									// don't overwrite any existing cvars when expanding
									ExpandScalabilityCVar(ConfigSystem, CVarKey, CVarValue, CVarsAlreadySetList, false);
								}

								// cache key with value
								CVarsAlreadySetList.Add(CVarKey, CVarValue);
							}
							// actually set the cvar if not just caching
							else
							{
								
								// Cache any scalability related cvars so we can conveniently reapply them later as a way to reset the device defaults
								if (bIsScalabilityBucket && CVarAllowScalabilityGroupsToChangeAtRuntime.GetValueOnGameThread() > 0)
								{
									DeviceProfileScalabilityCVars.Add(*CVarKey, *CVarValue);
								}

								//If this is a dp preview then we set cvars with their existing priority so that we don't cause future issues when setting by scalability levels etc.
								uint32 CVarPriority = bIsScalabilityBucket ? ECVF_SetByScalability : ECVF_SetByDeviceProfile;
								OnSetCVarFromIniEntry(*GDeviceProfilesIni, *CVarKey, *CVarValue, CVarPriority);
								CVarsAlreadySetList.Add(CVarKey, CVarValue);
							}
						}
					}
				}
			}

			// Get the next device profile name, to look for CVars in, along the tree
			FString NextBaseDeviceProfileName;
			if (ConfigSystem->GetString(*CurrentSectionName, TEXT("BaseProfileName"), NextBaseDeviceProfileName, GDeviceProfilesIni))
			{
				BaseDeviceProfileName = NextBaseDeviceProfileName;
				UE_LOG(LogDeviceProfileManager, Log, TEXT("Going up to parent DeviceProfile [%s]"), *BaseDeviceProfileName);
			}
			else
			{
				BaseDeviceProfileName.Empty();
			}
		}
		
		// Check if we have inevitably reached the end of the device profile tree.
		bReachedEndOfTree = !bProfileExists || BaseDeviceProfileName.IsEmpty();
	}

#if ALLOW_OTHER_PLATFORM_CONFIG
	// copy the running cache into the DP
	if (Mode == EDeviceProfileMode::DPM_CacheValues)
	{
		UDeviceProfile* Profile = UDeviceProfileManager::Get().FindProfile(DeviceProfileName, false);
		Profile->AddExpandedCVars(CVarsAlreadySetList);
	}
#endif
}

/**
* Set the cvar state to PushedSettings.
*/
static void RestorePushedState(TMap<FString, FString>& PushedSettings)
{
	// restore pushed settings
	for (TMap<FString, FString>::TIterator It(PushedSettings); It; ++It)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*It.Key());
		if (CVar)
		{
			// restore it!
			CVar->SetWithCurrentPriority(*It.Value());
			UE_LOG(LogDeviceProfileManager, Log, TEXT("Popping Device Profile CVar: [[%s:%s]]"), *It.Key(), *It.Value());
		}
	}

	PushedSettings.Reset();
}

const FSelectedFragmentProperties* UDeviceProfileManager::GetActiveDeviceProfileFragmentByTag(FName& FragmentTag) const
{
	for (const FSelectedFragmentProperties& SelectedFragment : PlatformFragmentsSelected)
	{
		if (SelectedFragment.Tag == FragmentTag)
		{
			return &SelectedFragment;
		}
	}
	return nullptr;
}

// enable/disable a tagged fragment.
void UDeviceProfileManager::ChangeTaggedFragmentState(FName FragmentTag, bool bNewState)
{
	for (FSelectedFragmentProperties& Fragment : PlatformFragmentsSelected)
	{
		if (Fragment.Tag == FragmentTag)
		{
			if (bNewState != Fragment.bEnabled)
			{
				UE_LOG(LogInit, Log, TEXT("ChangeTaggedFragmentState: %s=%d"), *FragmentTag.ToString(), bNewState);
				// unset entire DP's cvar state.
				RestorePushedState(PushedSettings);
				// set the new state and reapply all fragments.
				Fragment.bEnabled = bNewState;
				ProcessDeviceProfileIniSettings(GetActiveDeviceProfileName(), EDeviceProfileMode::DPM_SetCVars);
			}
			break;
		}
	}
}

void UDeviceProfileManager::InitializeCVarsForActiveDeviceProfile()
{
	FString ActiveProfileName;

	if (DeviceProfileManagerSingleton)
	{
		ActiveProfileName = DeviceProfileManagerSingleton->ActiveDeviceProfile->GetName();
	}
	else
	{
		ActiveProfileName = GetPlatformDeviceProfileName();
	}

	ProcessDeviceProfileIniSettings(ActiveProfileName, EDeviceProfileMode::DPM_SetCVars);
}

#if ALLOW_OTHER_PLATFORM_CONFIG
void UDeviceProfileManager::ExpandDeviceProfileCVars(UDeviceProfile* DeviceProfile)
{
	// get the config system for the platform the DP uses
	FConfigCacheIni* ConfigSystem = FConfigCacheIni::ForPlatform(*DeviceProfile->DeviceType);
	check(ConfigSystem);

	// walk up the chain cvar setBys and emulate what would happen on the target platform
	FString Platform = DeviceProfile->DeviceType;

	// now walk up the stack getting current values

	// ECVF_SetByConstructor:
	//   in PlatformIndependentDefault, used if getting a var but not in this DP

	// ECVF_SetByScalability:
	//	 skipped, i believe this is not really loaded as a normal layer per-se, it's up to the other sections to set with this one

	// ECVF_SetByGameSetting:
	//   skipped, since we don't have a user

	const TCHAR* SectionNames[] = {
		// ECVF_SetByProjectSetting:
		TEXT("/Script/Engine.RendererSettings"),
		TEXT("/Script/Engine.RendererOverrideSettings"),
		TEXT("/Script/Engine.StreamingSettings"),
		TEXT("/Script/Engine.GarbageCollectionSettings"),
		TEXT("/Script/Engine.NetworkSettings"),

		// ECVF_SetBySystemSettingsIni:
		TEXT("SystemSettings"),
		TEXT("ConsoleVariables"),
	};

	// go through possible cvar sections that the target platform would load and read all cvars in them
	TMap<FString, FString> CVarsToAdd;
	for (const TCHAR* SectionName : SectionNames)
	{
		FConfigSection* Section = ConfigSystem->GetSectionPrivate(SectionName, false, true, GEngineIni);
		if (Section != nullptr)
		{
			// add the cvars from the section
			for (const auto& Pair : *Section)
			{
				FString Key = Pair.Key.ToString();
				FString Value = Pair.Value.GetValue();
				if (Key.StartsWith(TEXT("sg.")))
				{
					// @todo ini: If anything in here was already set, overwrite it or skip it?
					// the priorities may cause runtime to fail to set a cvar that this will set blindly, since we are ignoring
					// priority by doing them "in order". Scalablity is one of the lowest priorities, so should almost never be allowed?
					ExpandScalabilityCVar(ConfigSystem, Key, Value, CVarsToAdd, true);
				}
				CVarsToAdd.Add(Pair.Key.ToString(), Pair.Value.GetValue());
			}
		}
	}
	DeviceProfile->AddExpandedCVars(CVarsToAdd);

	// ECVF_SetByDeviceProfile:
	ProcessDeviceProfileIniSettings(DeviceProfile->GetName(), EDeviceProfileMode::DPM_CacheValues);

	// ECVF_SetByGameOverride:
	//   skipped, since we don't have a user

	// ECVF_SetByConsoleVariablesIni:
	//   maybe skip this? it's a weird one, but maybe?

	// ECVF_SetByCommandline:
	//   skip as this would not be expected to apply to emulation

	// ECVF_SetByCode:
	//   skip because it cannot be set by code

	// ECVF_SetByConsole
	//   we could have this if we made a per-platform CVar, not just the shared default value
}
#endif





bool UDeviceProfileManager::DoActiveProfilesReference(const TSet<FString>& DeviceProfilesToQuery)
{
	TArray< FString > AvailableProfiles;
	GConfig->GetSectionNames(GDeviceProfilesIni, AvailableProfiles);

	auto DoesProfileReference = [&AvailableProfiles, GDeviceProfilesIni = GDeviceProfilesIni](const FString& SearchProfile, const TSet<FString>& InDeviceProfilesToQuery)
	{
		// For each device profile, starting with the selected and working our way up the BaseProfileName tree,
		FString BaseDeviceProfileName = SearchProfile;
		bool bReachedEndOfTree = BaseDeviceProfileName.IsEmpty();
		while (bReachedEndOfTree == false)
		{
			FString CurrentSectionName = FString::Printf(TEXT("%s %s"), *BaseDeviceProfileName, *UDeviceProfile::StaticClass()->GetName());
			bool bProfileExists = AvailableProfiles.Contains(CurrentSectionName);
			if (bProfileExists)
			{
				if (InDeviceProfilesToQuery.Contains(BaseDeviceProfileName))
				{
					return true;
				}

				// Get the next device profile name
				FString NextBaseDeviceProfileName;
				if (GConfig->GetString(*CurrentSectionName, TEXT("BaseProfileName"), NextBaseDeviceProfileName, GDeviceProfilesIni))
				{
					BaseDeviceProfileName = NextBaseDeviceProfileName;
				}
				else
				{
					BaseDeviceProfileName.Empty();
				}
			}
			bReachedEndOfTree = !bProfileExists || BaseDeviceProfileName.IsEmpty();
		}
		return false;
	};

	bool bDoActiveProfilesReferenceQuerySet = DoesProfileReference(DeviceProfileManagerSingleton->GetActiveProfile()->GetName(), DeviceProfilesToQuery);
	if (!bDoActiveProfilesReferenceQuerySet && DeviceProfileManagerSingleton->BaseDeviceProfile != nullptr)
	{
		bDoActiveProfilesReferenceQuerySet = DoesProfileReference(DeviceProfileManagerSingleton->BaseDeviceProfile->GetName(), DeviceProfilesToQuery);
	}
	return bDoActiveProfilesReferenceQuerySet;
}

void UDeviceProfileManager::ReapplyDeviceProfile()
{	
	UDeviceProfile* OverrideProfile = DeviceProfileManagerSingleton->BaseDeviceProfile ? DeviceProfileManagerSingleton->GetActiveProfile() : nullptr;
	UDeviceProfile* BaseProfile = DeviceProfileManagerSingleton->BaseDeviceProfile ? DeviceProfileManagerSingleton->BaseDeviceProfile : DeviceProfileManagerSingleton->GetActiveProfile();

	UE_LOG(LogDeviceProfileManager, Log, TEXT("ReapplyDeviceProfile applying profile: [%s]"), *BaseProfile->GetName(), OverrideProfile ? *OverrideProfile->GetName() : TEXT("not set."));

	if (OverrideProfile)
	{
		UE_LOG(LogDeviceProfileManager, Log, TEXT("ReapplyDeviceProfile applying override profile: [%s]"), *OverrideProfile->GetName());
		// reapply the override.
		SetOverrideDeviceProfile(OverrideProfile);
	}
	else
	{
		// reset any fragments, this will cause them to be rematched.
		PlatformFragmentsSelected.Empty();
		// restore to the pre-DP cvar state:
		RestorePushedState(PushedSettings);

		// Apply the active DP. 
		InitializeCVarsForActiveDeviceProfile();

		// broadcast cvar sinks now that we are done
		IConsoleManager::Get().CallAllConsoleVariableSinks();
	}
}

static void TestProfileForCircularReferences(const FString& ProfileName, const FString& ParentName, const FConfigFile &PlatformConfigFile)
{
	TArray<FString> ProfileDependancies;
	ProfileDependancies.Add(ProfileName);
	FString CurrentParent = ParentName;
	while (!CurrentParent.IsEmpty())
	{
		if (ProfileDependancies.FindByPredicate([CurrentParent](const FString& InName) { return InName.Equals(CurrentParent); }))
		{
			UE_LOG(LogDeviceProfileManager, Fatal, TEXT("Device Profile %s has a circular dependency on %s"), *ProfileName, *CurrentParent);
		}
		else
		{
			ProfileDependancies.Add(CurrentParent);
			const FString SectionName = FString::Printf(TEXT("%s %s"), *CurrentParent, *UDeviceProfile::StaticClass()->GetName());
			CurrentParent.Reset();
			PlatformConfigFile.GetString(*SectionName, TEXT("BaseProfileName"), CurrentParent);
		}
	}
}

UDeviceProfile* UDeviceProfileManager::CreateProfile(const FString& ProfileName, const FString& ProfileType, const FString& InSpecifyParentName, const TCHAR* ConfigPlatform)
{
	UDeviceProfile* DeviceProfile = FindObject<UDeviceProfile>( GetTransientPackage(), *ProfileName );
	if (DeviceProfile == NULL)
	{
		// use ConfigPlatform ini hierarchy to look in for the parent profile
		// @todo config: we could likely cache local ini files to speed this up,
		// along with the ones we load in LoadConfig
		// NOTE: This happens at runtime, so maybe only do this if !RequiresCookedData()?
		FConfigFile* PlatformConfigFile;
		FConfigFile LocalConfigFile;
		if (FPlatformProperties::RequiresCookedData())
		{
			PlatformConfigFile = GConfig->Find(GDeviceProfilesIni);
		}
		else
		{
			FConfigCacheIni::LoadLocalIniFile(LocalConfigFile, TEXT("DeviceProfiles"), true, ConfigPlatform);
			PlatformConfigFile = &LocalConfigFile;
		}

		// Build Parent objects first. Important for setup
		FString ParentName = InSpecifyParentName;
		if (ParentName.Len() == 0)
		{
			const FString SectionName = FString::Printf(TEXT("%s %s"), *ProfileName, *UDeviceProfile::StaticClass()->GetName());
			PlatformConfigFile->GetString(*SectionName, TEXT("BaseProfileName"), ParentName);
		}

		UDeviceProfile* ParentObject = nullptr;
		// Recursively build the parent tree
		if (ParentName.Len() > 0 && ParentName != ProfileName)
		{
			ParentObject = FindObject<UDeviceProfile>(GetTransientPackage(), *ParentName);
			if (ParentObject == nullptr)
			{
				TestProfileForCircularReferences(ProfileName, ParentName, *PlatformConfigFile);
				ParentObject = CreateProfile(ParentName, ProfileType, TEXT(""), ConfigPlatform);
			}
		}

		// Create the profile after it's parents have been created.
		DeviceProfile = NewObject<UDeviceProfile>(GetTransientPackage(), *ProfileName);
		if (ConfigPlatform != nullptr)
		{
			// if the config needs to come from a platform, set it now, then reload the config
			DeviceProfile->ConfigPlatform = ConfigPlatform;
			DeviceProfile->LoadConfig();
			DeviceProfile->ValidateProfile();
		}

		// if the config didn't specify a DeviceType, use the passed in one
		if (DeviceProfile->DeviceType.IsEmpty())
		{
			DeviceProfile->DeviceType = ProfileType;
		}

		// final fixups
		DeviceProfile->BaseProfileName = DeviceProfile->BaseProfileName.Len() > 0 ? DeviceProfile->BaseProfileName : ParentName;
		DeviceProfile->Parent = ParentObject;
		// the DP manager can be marked as Disregard for GC, so what it points to needs to be in the Root set
		DeviceProfile->AddToRoot();

		// Add the new profile to the accessible device profile list
		Profiles.Add( DeviceProfile );

		// Inform any listeners that the device list has changed
		ManagerUpdatedDelegate.Broadcast(); 
	}

	return DeviceProfile;
}


void UDeviceProfileManager::DeleteProfile( UDeviceProfile* Profile )
{
	Profiles.Remove( Profile );
}


UDeviceProfile* UDeviceProfileManager::FindProfile( const FString& ProfileName, bool bCreateProfileOnFail )
{
	UDeviceProfile* FoundProfile = nullptr;

	for( int32 Idx = 0; Idx < Profiles.Num(); Idx++ )
	{
		UDeviceProfile* CurrentDevice = CastChecked<UDeviceProfile>( Profiles[Idx] );
		if( CurrentDevice->GetName() == ProfileName )
		{
			FoundProfile = CurrentDevice;
			break;
		}
	}

	if ( bCreateProfileOnFail && FoundProfile == nullptr )
	{
		FoundProfile = CreateProfile(ProfileName, FPlatformProperties::IniPlatformName());
	}
	return FoundProfile;
}


FOnDeviceProfileManagerUpdated& UDeviceProfileManager::OnManagerUpdated()
{
	return ManagerUpdatedDelegate;
}


FOnActiveDeviceProfileChanged& UDeviceProfileManager::OnActiveDeviceProfileChanged()
{
	return ActiveDeviceProfileChangedDelegate;
}


void UDeviceProfileManager::GetProfileConfigFiles(OUT TArray<FString>& OutConfigFiles)
{
	TSet<FString> SetOfPaths;

	// Make sure generic platform is first
	const FString RelativeConfigFilePath = FString::Printf(TEXT("%sDefault%ss.ini"), *FPaths::SourceConfigDir(), *UDeviceProfile::StaticClass()->GetName());
	SetOfPaths.Add(RelativeConfigFilePath);

	for (int32 DeviceProfileIndex = 0; DeviceProfileIndex < Profiles.Num(); ++DeviceProfileIndex)
	{
		UDeviceProfile* CurrentProfile = CastChecked<UDeviceProfile>(Profiles[DeviceProfileIndex]);
		SetOfPaths.Add(CurrentProfile->GetDefaultConfigFilename());
	}
	
	OutConfigFiles = SetOfPaths.Array();
}

void UDeviceProfileManager::LoadProfiles()
{
	if( !HasAnyFlags( RF_ClassDefaultObject ) )
	{
		TMap<FString, FString> DeviceProfileToPlatformConfigMap;
		TArray<FName> ConfidentialPlatforms = FDataDrivenPlatformInfoRegistry::GetConfidentialPlatforms();
		
		checkf(ConfidentialPlatforms.Contains(FPlatformProperties::IniPlatformName()) == false,
			TEXT("UDeviceProfileManager::LoadProfiles is called from a confidential platform (%s). Confidential platforms are not expected to be editor/non-cooked builds."), 
			ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));

		// go over all the platforms we find, starting with the current platform
		for (int32 PlatformIndex = 0; PlatformIndex <= ConfidentialPlatforms.Num(); PlatformIndex++)
		{
			// which platform's set of ini files should we load from?
			FString ConfigLoadPlatform = PlatformIndex == 0 ? FString(FPlatformProperties::IniPlatformName()) : ConfidentialPlatforms[PlatformIndex - 1].ToString();

			// load the DP.ini files (from current platform and then by the extra confidential platforms)
			FConfigFile PlatformConfigFile;
			FConfigCacheIni::LoadLocalIniFile(PlatformConfigFile, TEXT("DeviceProfiles"), true, *ConfigLoadPlatform);

			// load all of the DeviceProfiles
			TArray<FString> ProfileDescriptions;
			PlatformConfigFile.GetArray(TEXT("DeviceProfiles"), TEXT("DeviceProfileNameAndTypes"), ProfileDescriptions);


			// add them to our collection of profiles by platform
			for (const FString& Desc : ProfileDescriptions)
			{
				if (!DeviceProfileToPlatformConfigMap.Contains(Desc))
				{
					DeviceProfileToPlatformConfigMap.Add(Desc, ConfigLoadPlatform);
				}
			}
		}

		// now that we have gathered all the unique DPs, load them from the proper platform hierarchy
		for (auto It = DeviceProfileToPlatformConfigMap.CreateIterator(); It; ++It)
		{
			// the value of the map is in the format Name,DeviceType (DeviceType is usually platform)
			FString Name, DeviceType;
			It.Key().Split(TEXT(","), &Name, &DeviceType);

			if (FindObject<UDeviceProfile>(GetTransientPackage(), *Name) == NULL)
			{
				// set the config platform if it's not the current platform
				if (It.Value() != FPlatformProperties::IniPlatformName())
				{
					CreateProfile(Name, DeviceType, TEXT(""), *It.Value());
				}
				else
				{
					CreateProfile(Name, DeviceType);
				}
			}
		}

#if WITH_EDITOR
		if (!FPlatformProperties::RequiresCookedData())
		{
			// Register Texture LOD settings with each Target Platform
			ITargetPlatformManagerModule& TargetPlatformManager = GetTargetPlatformManagerRef();
			const TArray<ITargetPlatform*>& TargetPlatforms = TargetPlatformManager.GetTargetPlatforms();
			for (int32 PlatformIndex = 0; PlatformIndex < TargetPlatforms.Num(); ++PlatformIndex)
			{
				ITargetPlatform* Platform = TargetPlatforms[PlatformIndex];

				// Set TextureLODSettings
				const UTextureLODSettings* TextureLODSettingsObj = FindProfile(Platform->CookingDeviceProfileName(), false);
				checkf(TextureLODSettingsObj, TEXT("No TextureLODSettings found for %s"), *Platform->CookingDeviceProfileName());

				Platform->RegisterTextureLODSettings(TextureLODSettingsObj);
			}

			// Make backup copies to allow proper saving
			BackupProfiles.Reset();

			for (UDeviceProfile* DeviceProfile : Profiles)
			{
				FString DuplicateName = DeviceProfile->GetName() + BackupSuffix;
				UDeviceProfile* BackupProfile = DuplicateObject<UDeviceProfile>(DeviceProfile, DeviceProfile->GetOuter(), FName(*DuplicateName));
				BackupProfiles.Add(BackupProfile);
			}
		}
#endif

		ManagerUpdatedDelegate.Broadcast();
	}
}


void UDeviceProfileManager::SaveProfiles(bool bSaveToDefaults)
{
	if( !HasAnyFlags( RF_ClassDefaultObject ) )
	{
		if(bSaveToDefaults)
		{
			for (int32 DeviceProfileIndex = 0; DeviceProfileIndex < Profiles.Num(); ++DeviceProfileIndex)
			{
				UDeviceProfile* CurrentProfile = CastChecked<UDeviceProfile>(Profiles[DeviceProfileIndex]);
				FString BackupName = CurrentProfile->GetName() + BackupSuffix;
				UDeviceProfile* BackupProfile = FindObject<UDeviceProfile>(GetTransientPackage(), *BackupName);

				// Don't save if it hasn't changed
				if (!AreProfilesTheSame(CurrentProfile, BackupProfile))
				{
					// Strip out runtime inherited texture groups before save
					UDeviceProfile* ParentProfile = CurrentProfile->GetParentProfile(true);
					if (ParentProfile && CurrentProfile->TextureLODGroups.Num() == ParentProfile->TextureLODGroups.Num())
					{
						// Remove any that are the same, these are saved as a keyed array so the rest will inherit
						for (int32 i = CurrentProfile->TextureLODGroups.Num() - 1; i >= 0; i--)
						{
							if (CurrentProfile->TextureLODGroups[i] == ParentProfile->TextureLODGroups[i])
							{
								CurrentProfile->TextureLODGroups.RemoveAt(i);
							}
						}
					}
					
					CurrentProfile->TryUpdateDefaultConfigFile();

					// Recreate texture groups
					CurrentProfile->ValidateProfile();
				}
			}
		}
		else
		{
			// We do not want to save local changes to profiles as this is not how any other editor works and it confuses the user
			// For changes to save you need to hit the save to defaults button in the device profile editor
		}

		ManagerUpdatedDelegate.Broadcast();
	}
}

#if ALLOW_OTHER_PLATFORM_CONFIG
void UDeviceProfileManager::SetPreviewDeviceProfile(UDeviceProfile* DeviceProfile)
{
	// we're applying a preview mode on top of an overridden DP?
	check(BaseDeviceProfile == nullptr);

	RestorePreviewDeviceProfile();

	UE_LOG(LogDeviceProfileManager, Log, TEXT("SetPreviewDeviceProfile preview to %s"), *DeviceProfile->GetName());
	// apply the preview DP cvars.
	for (const auto& Pair : DeviceProfile->GetAllExpandedCVars())
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Pair.Key);
		// skip over scalability group cvars (maybe they shouldn't be left in the AllExpandedCVars?)
		if (CVar != nullptr && !CVar->TestFlags(EConsoleVariableFlags::ECVF_ScalabilityGroup))
		{
			// remember the previous value so we can restore
			FString OldValue = CVar->GetString();
			PreviewPushedSettings.Add(Pair.Key, OldValue);
			// cheat CVar can only be set in ConsoleVariables.ini
			if (!CVar->TestFlags(EConsoleVariableFlags::ECVF_Cheat))
			{
				// set the cvar to the new value, with same priority that it was before (SetByMask means current priority)
				CVar->SetWithCurrentPriority(ConvertValueFromHumanFriendlyValue(*Pair.Value));
			}
		}
	}
}


void UDeviceProfileManager::RestorePreviewDeviceProfile()
{
	if (PreviewPushedSettings.Num())
	{
		checkf(BaseDeviceProfile == nullptr, TEXT("call to RestorePreviewDeviceProfile while both preview and BaseDeviceProfile has been set?"));

		UE_LOG(LogDeviceProfileManager, Log, TEXT("Restoring Preview DP "));
		// this sets us back to non-preview state.
		RestorePushedState(PreviewPushedSettings);
	}
}
#endif

/**
* Overrides the device profile. The original profile can be restored with RestoreDefaultDeviceProfile
*/
void UDeviceProfileManager::SetOverrideDeviceProfile(UDeviceProfile* DeviceProfile)
{
#if ALLOW_OTHER_PLATFORM_CONFIG
	// we have an active preview running but we're changing the actual device's DP too?
	check(PreviewPushedSettings.Num() == 0);
#endif

	// If we're not already overriding record the BaseDeviceProfile
	if(!BaseDeviceProfile)
	{
		BaseDeviceProfile = DeviceProfileManagerSingleton->GetActiveProfile();
	}
	UE_LOG(LogDeviceProfileManager, Log, TEXT("Overriding DeviceProfile to %s, base device profile %s"), *DeviceProfile->GetName(), *BaseDeviceProfile->GetName() );

	// reset any fragments, this will cause them to be rematched.
	PlatformFragmentsSelected.Empty();
	// restore to the pre-DP cvar state:
	RestorePushedState(PushedSettings);

	// activate new one!
	DeviceProfileManagerSingleton->SetActiveDeviceProfile(DeviceProfile);
	InitializeCVarsForActiveDeviceProfile();

	// broadcast cvar sinks now that we are done
	IConsoleManager::Get().CallAllConsoleVariableSinks();
}

/**
* Restore the device profile to the default for this device
*/
void UDeviceProfileManager::RestoreDefaultDeviceProfile()
{
#if ALLOW_OTHER_PLATFORM_CONFIG
	// We're restoring overridden DP while a preview is active?
	check(PreviewPushedSettings.Num() == 0);
#endif

	// have we been overridden?
	if (BaseDeviceProfile)
	{
		UE_LOG(LogDeviceProfileManager, Log, TEXT("Restoring overridden DP back to %s"), *BaseDeviceProfile->GetName());
		// this differs from previous behavior, we used to push only the cvar state that was modified by the override.
		// But now we restore the entire CVar state to 'pre-DP' stage and reapply the currently active DP.
		// reset the base profile as we are no longer overriding
		RestorePushedState(PushedSettings);
		// reset any fragments, this will cause them to be rematched.
		PlatformFragmentsSelected.Empty();
		SetActiveDeviceProfile(BaseDeviceProfile);
		BaseDeviceProfile = nullptr;
		//set the DP cvar state
		InitializeCVarsForActiveDeviceProfile();
	}
}

void UDeviceProfileManager::HandleDeviceProfileOverrideChange()
{
	FString CVarValue = CVarDeviceProfileOverride.GetValueOnGameThread();
	// only handle when the value is different
	if (CVarValue.Len() > 0 && CVarValue != GetActiveProfile()->GetName())
	{
		UDeviceProfile* NewActiveProfile = FindProfile(CVarValue, false);
		if (NewActiveProfile)
		{
			SetOverrideDeviceProfile(NewActiveProfile);
		}
	}
}

bool UDeviceProfileManager::AreProfilesTheSame(UDeviceProfile* Profile1, UDeviceProfile* Profile2) const
{
	if (!AreTextureGroupsTheSame(Profile1, Profile2))
	{
		// This does null check
		return false;
	}

	if (!Profile1->DeviceType.Equals(Profile2->DeviceType, ESearchCase::CaseSensitive))
	{
		return false;
	}

	if (!Profile1->BaseProfileName.Equals(Profile2->BaseProfileName, ESearchCase::CaseSensitive))
	{
		return false;
	}


	if (Profile1->CVars != Profile2->CVars)
	{
		return false;
	}

	if (Profile1->MatchingRules != Profile2->MatchingRules)
	{
		return false;
	}

	return true;
}

bool UDeviceProfileManager::AreTextureGroupsTheSame(UDeviceProfile* Profile1, UDeviceProfile* Profile2) const
{
	if (!Profile1 || !Profile2)
	{
		return false;
	}

	// If our groups are identical say yes
	if (Profile1->TextureLODGroups == Profile2->TextureLODGroups)
	{
		return true;
	}

	UDeviceProfile* Parent1 = Profile1->GetParentProfile(true);
	UDeviceProfile* Parent2 = Profile2->GetParentProfile(true);

	// Also if both profiles inherit groups with no changes, count them as the same
	if (Parent1 && Parent2 &&
		Profile1->TextureLODGroups == Parent1->TextureLODGroups &&
		Profile2->TextureLODGroups == Parent2->TextureLODGroups)
	{
		return true;
	}

	return false;
}

IDeviceProfileSelectorModule* UDeviceProfileManager::GetPreviewDeviceProfileSelectorModule(FConfigCacheIni* PreviewConfigSystemIn)
{
#if ALLOW_OTHER_PLATFORM_CONFIG && WITH_EDITOR
	// If we're getting the selector for previewing the PIEPreviewDeviceProfileSelector device selector can be given a PreviewDeviceDesciption to return selector info for specific devices.
	FString PreviewDeviceDesciption; 
	if (PreviewConfigSystemIn->GetString(TEXT("DeviceProfileManager"), TEXT("PreviewDeviceDesciption"), PreviewDeviceDesciption, GEngineIni))
	{
		// this should only be specified when previewing.
		if (IPIEPreviewDeviceModule* DPSelectorModule = FModuleManager::LoadModulePtr<IPIEPreviewDeviceModule>(TEXT("PIEPreviewDeviceProfileSelector")))
		{
			// TODO: check success.
			DPSelectorModule->SetPreviewDevice(PreviewDeviceDesciption);
			return DPSelectorModule;
		}
	}
#else
	checkNoEntry();
#endif
	return nullptr;
}

IDeviceProfileSelectorModule* UDeviceProfileManager::GetDeviceProfileSelectorModule()
{
	FString DeviceProfileSelectionModule;
	if (GConfig->GetString(TEXT("DeviceProfileManager"), TEXT("DeviceProfileSelectionModule"), DeviceProfileSelectionModule, GEngineIni))
	{
		if (IDeviceProfileSelectorModule* DPSelectorModule = FModuleManager::LoadModulePtr<IDeviceProfileSelectorModule>(*DeviceProfileSelectionModule))
		{
			return DPSelectorModule;
		}
	}
	return nullptr;
}

const FString UDeviceProfileManager::GetPlatformDeviceProfileName()
{
	FString ActiveProfileName = FPlatformProperties::PlatformName();

	// look for a commandline override (never even calls into the selector plugin)
	FString OverrideProfileName;
	if (FParse::Value(FCommandLine::Get(), TEXT("DeviceProfile="), OverrideProfileName) || FParse::Value(FCommandLine::Get(), TEXT("DP="), OverrideProfileName))
	{
		return OverrideProfileName;
	}

	// look for cvar override
	OverrideProfileName = CVarDeviceProfileOverride.GetValueOnGameThread();
	if (OverrideProfileName.Len() > 0)
	{
		return OverrideProfileName;
	}

	if (IDeviceProfileSelectorModule* DPSelectorModule = GetDeviceProfileSelectorModule())
	{
		ActiveProfileName = DPSelectorModule->GetRuntimeDeviceProfileName();
	}

#if WITH_EDITOR
	if (FPIEPreviewDeviceModule::IsRequestingPreviewDevice())
	{
		IDeviceProfileSelectorModule* PIEPreviewDeviceProfileSelectorModule = FModuleManager::LoadModulePtr<IDeviceProfileSelectorModule>("PIEPreviewDeviceProfileSelector");
		if (PIEPreviewDeviceProfileSelectorModule)
		{
			FString PIEProfileName = PIEPreviewDeviceProfileSelectorModule->GetRuntimeDeviceProfileName();
			if (!PIEProfileName.IsEmpty())
			{
				ActiveProfileName = PIEProfileName;
			}
		}
	}
#endif
	return ActiveProfileName;
}


const FString UDeviceProfileManager::GetActiveDeviceProfileName()
{
	if(ActiveDeviceProfile != nullptr)
	{
		return ActiveDeviceProfile->GetName();
	}
	else
	{
		return GetPlatformDeviceProfileName();
	}
}

const FString UDeviceProfileManager::GetActiveProfileName()
{
	return GetPlatformDeviceProfileName();
}

bool UDeviceProfileManager::GetScalabilityCVar(const FString& CVarName, int32& OutValue)
{
	if (const FString* CVarValue = DeviceProfileScalabilityCVars.Find(CVarName))
	{
		TTypeFromString<int32>::FromString(OutValue, **CVarValue);
		return true;
	}

	return false;
}

bool UDeviceProfileManager::GetScalabilityCVar(const FString& CVarName, float& OutValue)
{
	if (const FString* CVarValue = DeviceProfileScalabilityCVars.Find(CVarName))
	{
		TTypeFromString<float>::FromString(OutValue, **CVarValue);
		return true;
	}

	return false;
}

void UDeviceProfileManager::SetActiveDeviceProfile( UDeviceProfile* DeviceProfile )
{
	ActiveDeviceProfile = DeviceProfile;

	FString ProfileNames;
	for (int32 Idx = 0; Idx < Profiles.Num(); ++Idx)
	{
		UDeviceProfile* Profile = Cast<UDeviceProfile>(Profiles[Idx]);
		const void* TextureLODGroupsAddr = Profile ? Profile->TextureLODGroups.GetData() : nullptr;
		const int32 NumTextureLODGroups = Profile ? Profile->TextureLODGroups.Num() : 0;
		ProfileNames += FString::Printf(TEXT("[%p][%p %d] %s, "), Profile, TextureLODGroupsAddr, NumTextureLODGroups, Profile ? *Profile->GetName() : TEXT("None"));
	}

	const void* TextureLODGroupsAddr = ActiveDeviceProfile ? ActiveDeviceProfile->TextureLODGroups.GetData() : nullptr;
	const int32 NumTextureLODGroups = ActiveDeviceProfile ? ActiveDeviceProfile->TextureLODGroups.Num() : 0;
	UE_LOG(LogDeviceProfileManager, Log, TEXT("Active device profile: [%p][%p %d] %s"), ActiveDeviceProfile, TextureLODGroupsAddr, NumTextureLODGroups, ActiveDeviceProfile ? *ActiveDeviceProfile->GetName() : TEXT("None"));
	UE_LOG(LogDeviceProfileManager, Log, TEXT("Profiles: %s"), *ProfileNames);

	ActiveDeviceProfileChangedDelegate.Broadcast();

#if CSV_PROFILER
	CSV_METADATA(TEXT("DeviceProfile"), *GetActiveDeviceProfileName());
#endif

	// Update the crash context 
	FGenericCrashContext::SetEngineData(TEXT("DeviceProfile.Name"), GetActiveDeviceProfileName());
}


UDeviceProfile* UDeviceProfileManager::GetActiveProfile() const
{
	return ActiveDeviceProfile;
}


void UDeviceProfileManager::GetAllPossibleParentProfiles(const UDeviceProfile* ChildProfile, OUT TArray<UDeviceProfile*>& PossibleParentProfiles) const
{
	for(auto& NextProfile : Profiles)
	{
		UDeviceProfile* ParentProfile = CastChecked<UDeviceProfile>(NextProfile);
		if (ParentProfile->DeviceType == ChildProfile->DeviceType && ParentProfile != ChildProfile)
		{
			bool bIsValidPossibleParent = true;

			UDeviceProfile* CurrentAncestor = ParentProfile;
			do
			{
				if(CurrentAncestor->BaseProfileName == ChildProfile->GetName())
				{
					bIsValidPossibleParent = false;
					break;
				}
				else
				{
					CurrentAncestor = CurrentAncestor->Parent != nullptr ? CastChecked<UDeviceProfile>(CurrentAncestor->Parent) : NULL;
				}
			} while(CurrentAncestor && bIsValidPossibleParent);

			if(bIsValidPossibleParent)
			{
				PossibleParentProfiles.Add(ParentProfile);
			}
		}
	}
}






#if ALLOW_OTHER_PLATFORM_CONFIG
static bool GetCVarForPlatform( FOutputDevice& Ar, FString DPName, FString CVarName)
{
	UDeviceProfile* DeviceProfile = UDeviceProfileManager::Get().FindProfile(DPName, false);
	if (DeviceProfile == nullptr)
	{
		Ar.Logf(TEXT("Unable to find device profile %s"), *DPName);
		return false;
	}

	FString Value;
	const FString* DPValue = DeviceProfile->GetAllExpandedCVars().Find(CVarName);
	if (DPValue != nullptr)
	{
		Value = *DPValue;
	}
	else
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName);
		if (!CVar)
		{
			Ar.Logf(TEXT("Unable to find cvar %s"), *CVarName);
			return false;
		}

		Value = CVar->GetDefaultValueVariable()->GetString();
	}

	Ar.Logf(TEXT("%s@%s = \"%s\""), *DPName, *CVarName, *Value);

	return true;
}

class FPlatformCVarExec : public FSelfRegisteringExec
{
public:

	// FSelfRegisteringExec interface
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override
	{
		if (FParse::Command(&Cmd, TEXT("dpcvar")))
		{
			FString DPName, CVarName;
			if (FString(Cmd).Split(TEXT("@"), &DPName, &CVarName) == false)
			{
				return false;
			}

			return GetCVarForPlatform(Ar, DPName, CVarName);
		}
		else if (FParse::Command(&Cmd, TEXT("dpdump")))
		{
			UDeviceProfile* DeviceProfile = UDeviceProfileManager::Get().FindProfile(Cmd, false);
			if (DeviceProfile)
			{
				Ar.Logf(TEXT("All cvars found for deviceprofile %s"), Cmd);
				for (const auto& Pair : DeviceProfile->GetAllExpandedCVars())
				{
					Ar.Logf(TEXT("%s = %s"), *Pair.Key, *Pair.Value);
				}
			}
		}
		else if (FParse::Command(&Cmd, TEXT("dppreview")))
		{
			UDeviceProfile* DeviceProfile = UDeviceProfileManager::Get().FindProfile(Cmd, false);
			if (DeviceProfile)
			{
				UDeviceProfileManager::Get().SetPreviewDeviceProfile(DeviceProfile);
			}
		}
		else if (FParse::Command(&Cmd, TEXT("dprestore")))
		{
			UDeviceProfileManager::Get().RestorePreviewDeviceProfile();
		}
		else if (FParse::Command(&Cmd, TEXT("dpreload")))
		{
			FConfigCacheIni::ClearOtherPlatformConfigs();
			// @todo ini clear out all DPs AllExpandedCVars
		}
		else if (FParse::Command(&Cmd, TEXT("dpreapply")))
		{
			UDeviceProfileManager::Get().ReapplyDeviceProfile();
		}


		return false;
	}

} GPlatformCVarExec;


#endif