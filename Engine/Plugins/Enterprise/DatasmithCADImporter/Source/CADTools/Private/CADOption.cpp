// Copyright Epic Games, Inc. All Rights Reserved.
#include "CADOptions.h"

namespace CADLibrary
{

FString GCADLibrary = TEXT("KernelIO");
FAutoConsoleVariableRef GCADTranslatorLibrary(
	TEXT("ds.CADTranslator.CADLibrary"),
	GCADLibrary,
	TEXT("Define the used library to import CAD file i.e. \"TechSoft\" or \"KernelIO\".\n"),
	ECVF_Default);

int32 GMaxImportThreads = 0;
FAutoConsoleVariableRef GCADTranslatorMaxImportThreads(
	TEXT("ds.CADTranslator.MaxImportThreads"),
	GMaxImportThreads,
	TEXT("\
CAD file parallel processing\n\
Default is MaxImportThreads = 0\n\
0: multi-processing, n : multi-processing limited to n process. EnableCADCache is mandatory.\n\
1: -if EnableCADCache is true, the scene is read in a sequential mode with cache i.e. cache is used for sub-files already read,\n\
   -if EnableCADCache is false, the scene is read all at once\n"),
	ECVF_Default);

bool FImportParameters::bGDisableCADKernelTessellation = true;
FAutoConsoleVariableRef GCADTranslatorDisableCADKernelTessellation(
	TEXT("ds.CADTranslator.DisableCADKernelTessellation"),
	FImportParameters::bGDisableCADKernelTessellation,
	TEXT("Disable to use CAD import library tessellator.\n"),
	ECVF_Default);

bool FImportParameters::bGEnableCADCache = true;
FAutoConsoleVariableRef GCADTranslatorEnableCADCache(
	TEXT("ds.CADTranslator.EnableCADCache"),
	FImportParameters::bGEnableCADCache,
	TEXT("\
Enable/disable temporary CAD processing file cache. These file will be use in a next import to avoid CAD file processing.\n\
If MaxImportThreads != 1, EnableCADCache value is ignored\n\
Default is enable\n"),
	ECVF_Default);

bool FImportParameters::bGOverwriteCache = false;
FAutoConsoleVariableRef GCADTranslatorOverwriteCache(
	TEXT("ds.CADTranslator.OverwriteCache"),
	FImportParameters::bGOverwriteCache,
	TEXT("Overwrite any existing cache associated with the file being imported.\n"),
	ECVF_Default);

bool FImportParameters::bGEnableTimeControl = true;
FAutoConsoleVariableRef GCADTranslatorEnableTimeControl(
	TEXT("ds.CADTranslator.EnableTimeControl"),
	FImportParameters::bGEnableTimeControl,
	TEXT("Enable the timer that kill the worker if the import time is unusually long. With this time control, the load of the corrupted file is canceled but the rest of the scene is imported.\n"),
	ECVF_Default);

uint32 GetTypeHash(const FImportParameters& ImportParameters)
{
	uint32 ParametersHash = ::GetTypeHash(ImportParameters.bGDisableCADKernelTessellation);
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.ChordTolerance));
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.MaxEdgeLength));
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.MaxNormalAngle));
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.MetricUnit));
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.ScaleFactor));
	ParametersHash = HashCombine(ParametersHash, ::GetTypeHash(ImportParameters.StitchingTechnique));
	return ParametersHash;
}

}
