// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commandlets/CookGlobalShadersCommandlet.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "ShaderCompiler.h"
#include "CookOnTheSide/CookOnTheFlyServer.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "DerivedDataCacheInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogCookGlobalShaders, Log, All);

bool UCookGlobalShadersDeviceHelperStaged::CopyFilesToDevice(class ITargetDevice* Device, const TArray<TPair<FString, FString>>& FilesToCopy) const
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	bool bSuccess = true;
	for (const TPair<FString, FString>& FileToCopy : FilesToCopy)
	{
		FString LocalFile = FileToCopy.Key;
		FString RemoteFile = FPaths::Combine(StagedBuildPath, FileToCopy.Value);
		bSuccess &= PlatformFile.CopyFile(*RemoteFile, *LocalFile);
	}
	return bSuccess;
}

int32 UCookGlobalShadersCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamVals;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, ParamVals);

	// Display help
	if (Switches.Contains("help"))
	{
		UE_LOG(LogCookGlobalShaders, Log, TEXT("CookGlobalShaders"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT("This commandlet will allow you to generate the global shaders file which can be used to override what is used in a cooked build by deploying the loose file."));
		UE_LOG(LogCookGlobalShaders, Log, TEXT("Options:"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Required: -platform=<platform>             (Which platform you want to cook for, i.e. windows)"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Optional: -device=<name>                   (Set which device to use, when enabled the reload command will be sent to the device once the shaders are cooked)"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Optional: -deploy=<optional deploy folder> (Must be used with -device and will deploy the shader file onto the device rather than in the staged builds folder)"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Optional: -stage=<optional path>           (Moved the shader file into the staged builds folder, destination can be overriden)"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Optional: -reload                          (Execute a shader reload on the device, only works if the device is valid or a default one was found"));
		UE_LOG(LogCookGlobalShaders, Log, TEXT(" Optional: -shaderpdb=<path>                (Sets the shader pdb root)"));
		return 0;
	}

	const bool bDeployToDevice = Switches.Contains(TEXT("deploy")) || ParamVals.Contains(TEXT("deploy"));
	const bool bCopyToStaged = Switches.Contains(TEXT("stage"));
	const bool bExecuteReload = Switches.Contains(TEXT("reload"));

	FString DeployFolder;
	if (ParamVals.Contains(TEXT("deploy")))
	{
		DeployFolder = ParamVals[TEXT("deploy")];
	}

	// Parse platform
	FString PlatformName;
	ITargetPlatform* TargetPlatform = nullptr;
	{
		ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();

		if (!ParamVals.Contains(TEXT("platform")))
		{
			UE_LOG(LogCookGlobalShaders, Warning, TEXT("You must include a target platform with -platform=xxx"));
			for (ITargetPlatform* TP : TPM.GetTargetPlatforms())
			{
				UE_LOG(LogCookGlobalShaders, Display, TEXT("   %s"), *TP->PlatformName());
			}
			return 1;
		}
		PlatformName = ParamVals.FindRef(TEXT("platform"));

		TargetPlatform = TPM.FindTargetPlatform(PlatformName);
		if (TargetPlatform == nullptr)
		{
			UE_LOG(LogCookGlobalShaders, Warning, TEXT("Target platform '%s' was not found"), *PlatformName);
			for (ITargetPlatform* TP : TPM.GetTargetPlatforms())
			{
				UE_LOG(LogCookGlobalShaders, Display, TEXT("   %s"), *TP->PlatformName());
			}
			return 1;
		}

		TargetPlatform->RefreshSettings();
	}

	// Get target device
	ITargetDevicePtr TargetDevice;
	FString TargetDeviceName;
	if ( FParse::Value(*Params, TEXT("device="), TargetDeviceName, true) )
	{
		TArray<ITargetDevicePtr> TargetDevices;
		TargetPlatform->GetAllDevices(TargetDevices);

		for ( int i=0; i < TargetDevices.Num(); ++i )
		{
			if ( TargetDevices[i]->GetName().Equals(TargetDeviceName, ESearchCase::IgnoreCase) )
			{
				TargetDevice = TargetDevices[i];
				break;
			}
		}

		if ( !TargetDevice.IsValid() )
		{
			UE_LOG(LogCookGlobalShaders, Warning, TEXT("Failed to find target device '%s', reload / deploy will not be valid"), *TargetDeviceName);

			for (int i = 0; i < TargetDevices.Num(); ++i)
			{
				UE_LOG(LogCookGlobalShaders, Warning, TEXT("	%s"), *TargetDevices[i]->GetName());
			}
		}
	}
	else
	{
		TargetDevice = TargetPlatform->GetDefaultDevice();
	}

	if (!TargetDevice.IsValid() && (bDeployToDevice || bExecuteReload) )
	{
		UE_LOG(LogCookGlobalShaders, Warning, TEXT("No device found to use for reload / deploy"));
	}

	// Find DeviceHelper class to use
	UCookGlobalShadersDeviceHelperBase* DeviceHelper = nullptr;
	if (TargetDevice.IsValid() && bDeployToDevice)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			if (ClassIt->IsChildOf(UCookGlobalShadersDeviceHelperBase::StaticClass()))
			{
				FString ClassName = ClassIt->GetName();
				ClassName.RemoveAt(0, UE_ARRAY_COUNT(TEXT("CookGlobalShadersDeviceHelperBase")), false);
				if (ClassName.Equals(PlatformName))
				{
					DeviceHelper = NewObject<UCookGlobalShadersDeviceHelperBase>(GetTransientPackage(), *ClassIt);
					break;
				}
			}
		}

		if (DeviceHelper == nullptr)
		{
			UE_LOG(LogCookGlobalShaders, Warning, TEXT("Failed to find Device Specific Implementation for '%s' global shaders will not be deployed to the device!"), *PlatformName);
		}
	}
	else if ( bCopyToStaged )
	{
		UCookGlobalShadersDeviceHelperStaged* StagedDeviceHelper = NewObject<UCookGlobalShadersDeviceHelperStaged>();
		if (!FParse::Value(*Params, TEXT("stage="), StagedDeviceHelper->StagedBuildPath, true))
		{
			StagedDeviceHelper->StagedBuildPath = FPaths::ProjectSavedDir() / TEXT("StagedBuilds") / PlatformName;
		}

		DeviceHelper = StagedDeviceHelper;
	}

	// Cook shaders
	TArray<FName> ShaderFormats;
	TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);

	//const bool bContinous = Switches.Contains(TEXT("continuous"));
	//while (bContinous)
	{
		// Cook shaders
		UE_LOG(LogCookGlobalShaders, Log, TEXT("Cooking Global Shaders..."));
		FString OutputDir = FPaths::ProjectSavedDir() / TEXT("CookGlobalShaders") / PlatformName;
		TArray<uint8> OutGlobalShaderMap;
		FShaderRecompileData Arguments(PlatformName, SP_NumPlatforms, ODSCRecompileCommand::Global, nullptr, nullptr, &OutGlobalShaderMap);
		RecompileShadersForRemote(Arguments, OutputDir);

		bool bCopySucceeded = false;

		// Build list of files to copy
		TArray<TPair<FString, FString>> FilesToCopy;
		for (FName ShaderFormat : ShaderFormats)
		{
			const FString GlobalShaderCacheName = FPaths::Combine(OutputDir, TEXT("Engine"), TEXT("GlobalShaderCache-") + ShaderFormat.ToString() + TEXT(".bin"));
			const FString OverrideGlobalShaderCacheName = FPaths::Combine(TEXT("Engine"), TEXT("OverrideGlobalShaderCache-") + ShaderFormat.ToString() + TEXT(".bin"));
			FilesToCopy.Emplace(GlobalShaderCacheName, OverrideGlobalShaderCacheName);
		}

		// Are we copying the built files somewhere?
		if (DeviceHelper != nullptr)
		{
			// Execute Copy
			UE_LOG(LogCookGlobalShaders, Log, TEXT("Copying Cooked Files..."));
			bCopySucceeded = DeviceHelper->CopyFilesToDevice(TargetDevice.Get(), FilesToCopy);
		}
		// if no helper, but we want to deploy, use the TargetPlatform
		else if (bDeployToDevice && TargetDevice != nullptr)
		{
			bCopySucceeded = true;

			TMap<FString,FString> CustomPlatformData;
			CustomPlatformData.Add(TEXT("DeployFolder"), DeployFolder);

			for (auto It : FilesToCopy)
			{
				bCopySucceeded = bCopySucceeded && TargetPlatform->CopyFileToTarget(TargetDevice->GetId().GetDeviceName(), It.Key, It.Value, CustomPlatformData);
			}
		}


		// Execute Reload
		if (bCopySucceeded && bExecuteReload && TargetDevice.IsValid())
		{
			UE_LOG(LogCookGlobalShaders, Log, TEXT("Sending Reload Command..."));
			TargetDevice->ReloadGlobalShadersMap(OutputDir / TEXT("Engine"));
		}
	}
	UE_LOG(LogCookGlobalShaders, Log, TEXT("Complete"));

	DeviceHelper = nullptr;

	// Wait for any DDC writes to complete
	GetDerivedDataCacheRef().WaitForQuiescence(true);

	return 0;
}
