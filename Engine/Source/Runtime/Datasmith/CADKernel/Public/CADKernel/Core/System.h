// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CADKernel/Core/Types.h"

#include "CADKernel/UI/Visu.h"
#include "CADKernel/UI/Console.h"
#include "CADKernel/UI/Progress.h"

namespace CADKernel
{
	class FConsole;
	class FKernelParameters;
	class FVisu;

	class FSystem
	{
		friend class FDatabase;

	protected:

		FString ProductName = FString(TEXT("CADKernel"));

		TSharedRef<FKernelParameters> Parameters;

		FVisu DefaultVisu;
		FVisu* Viewer;

		FConsole DefaultConsole;
		FConsole* Console;

		FProgressManager DefaultProgressManager;
		FProgressManager* ProgressManager;

		static TUniquePtr<FSystem> Instance;

		TSharedPtr<FArchive> LogFile;
		EVerboseLevel LogLevel;
		TSharedPtr<FArchive> SpyFile;

#if defined(CADKERNEL_DEV) || defined(CADKERNEL_STDA)
		TSharedPtr<FArchive> QaDataFile;
		TSharedPtr<FArchive> QaHeaderFile;
#endif

		EVerboseLevel VerboseLevel;

	public:

		FSystem();

		void Initialize(bool bIsDll = true, const FString& LogFilePath = TEXT(""), const FString& SpyFilePath = TEXT(""));

		void Shutdown();

		void CloseLogFiles();

		FString GetToolkitVersion() const;
		FString GetCompilationDate() const;

		FVisu* GetVisu() const
		{
			return Viewer;
		}

		FConsole& GetConsole() const
		{
			ensureCADKernel(Console);
			return *Console;
		}

		void SetViewer(FVisu* NewViewer)
		{
			Viewer = NewViewer;
		}

		void SetConsole(FConsole* InConsole)
		{
			Console = InConsole;
		}

		FProgressManager& GetProgressManager()
		{
			ensureCADKernel(ProgressManager);
			return *ProgressManager;
		}
			
		void SetProgressManager(FProgressManager* InProgressManager)
		{
			ProgressManager = InProgressManager;
		}

		TSharedRef<FKernelParameters> GetParameters()
		{
			return Parameters;
		}

		EVerboseLevel GetVerboseLevel() const
		{
			return VerboseLevel;
		}

		void SetVerboseLevel(EVerboseLevel Level)
		{
			VerboseLevel = Level;
		}

		void InitializeCADKernel();

		EVerboseLevel GetLogLevel() const
		{
			return LogLevel;
		}

		void DefineLogFile(const FString& LogFilePath, EVerboseLevel Level = Log);
		TSharedPtr<FArchive> GetLogFile() const
		{
			return LogFile;
		}

		void DefineSpyFile(const FString& SpyFilePath);
		TSharedPtr<FArchive> GetSpyFile() const
		{
			return SpyFile;
		}


#if defined(CADKERNEL_DEV) || defined(CADKERNEL_STDA)
		void DefineQaDataFile(const FString& InLogFilePath);
		TSharedPtr<FArchive> GetQaDataFile() const
		{
			return QaDataFile;
		}

		TSharedPtr<FArchive> GetQaHeaderFile() const
		{
			return QaHeaderFile;
		}
#endif

		static FSystem& Get()
		{
			if (!Instance.IsValid())
			{
				Instance = MakeUnique<FSystem>();
			}
			return *Instance;
		}

	protected:

		void PrintHeader();

	};

} // namespace CADKernel

