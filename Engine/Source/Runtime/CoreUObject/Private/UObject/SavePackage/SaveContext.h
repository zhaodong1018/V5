// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/Optional.h"
#include "Misc/PackageName.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/LargeMemoryWriter.h"
#include "Serialization/PropertyLocalizationDataGathering.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/UnversionedPropertySerialization.h"
#include "Templates/PimplPtr.h"
#include "Templates/UniquePtr.h"
#include "UObject/AsyncWorkSequence.h"
#include "UObject/LinkerSave.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SavePackage/SavePackageUtilities.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectThreadContext.h"

/**
 * Wraps an object tagged as export along with some of its harvested settings
 */
struct FTaggedExport
{
	UObject* Obj;
	uint32 bNotAlwaysLoadedForEditorGame : 1;

	FTaggedExport()
		: Obj(nullptr)
		, bNotAlwaysLoadedForEditorGame(false)
	{}

	FTaggedExport(UObject* InObj, bool bInNotAlwaysLoadedForEditorGame = true)
		: Obj(InObj)
		, bNotAlwaysLoadedForEditorGame(bInNotAlwaysLoadedForEditorGame)
	{}

	inline bool operator == (const FTaggedExport& Other) const
	{
		return Obj == Other.Obj;
	}
};

inline uint32 GetTypeHash(const FTaggedExport& Export)
{
	return GetTypeHash(Export.Obj);
}


/**
 * Helper class that encapsulate the full necessary context and intermediate result to save a package
 */
class FSaveContext
{
public:
	FSaveContext(UPackage* InPackage, UObject* InAsset, const TCHAR* InFilename, const FSavePackageArgs& InSaveArgs, FUObjectSerializeContext* InSerializeContext = nullptr)
		: Package(InPackage)
		, Asset(InAsset)
		, Filename(InFilename)
		, SaveArgs(InSaveArgs)
		, PackageWriter(InSaveArgs.SavePackageContext ? InSaveArgs.SavePackageContext->PackageWriter : nullptr)
		, SerializeContext(InSerializeContext)
		, ExcludedObjectMarks(SavePackageUtilities::GetExcludedObjectMarksForTargetPlatform(SaveArgs.TargetPlatform))
	{
		// Assumptions & checks
		check(InPackage);
		check(InFilename);
		// if we are cooking we should be doing it in the editor and with a CookedPackageWriter
		check(!IsCooking() || WITH_EDITOR);
		checkf(!IsCooking() || (PackageWriter && PackageWriter->AsCookedPackageWriter()), TEXT("Cook saves require an ICookedPackageWriter"));

		SaveArgs.TopLevelFlags = UE::SavePackageUtilities::NormalizeTopLevelFlags(SaveArgs.TopLevelFlags, IsCooking());
		if (PackageWriter)
		{
			bIgnoreHeaderDiffs = SaveArgs.SavePackageContext->PackageWriterCapabilities.bIgnoreHeaderDiffs;
		}

		// if the asset wasn't provided, fetch it from the package
		if (Asset == nullptr)
		{
			Asset = InPackage->FindAssetInPackage();
		}

		TargetPackagePath = FPackagePath::FromLocalPath(InFilename);
		if (TargetPackagePath.GetHeaderExtension() == EPackageExtension::Unspecified)
		{
			TargetPackagePath.SetHeaderExtension(EPackageExtension::EmptyString);
		}

		bCanUseUnversionedPropertySerialization = CanUseUnversionedPropertySerialization(SaveArgs.TargetPlatform);
		bTextFormat = FString(Filename).EndsWith(FPackageName::GetTextAssetPackageExtension()) || FString(Filename).EndsWith(FPackageName::GetTextMapPackageExtension());
		static const IConsoleVariable* ProcessPrestreamingRequests = IConsoleManager::Get().FindConsoleVariable(TEXT("s.ProcessPrestreamingRequests"));
		if (ProcessPrestreamingRequests)
		{
			bIsProcessingPrestreamPackages = ProcessPrestreamingRequests->GetInt() > 0;
		}
		static const IConsoleVariable* FixupStandaloneFlags = IConsoleManager::Get().FindConsoleVariable(TEXT("save.FixupStandaloneFlags"));
		if (FixupStandaloneFlags)
		{
			bIsFixupStandaloneFlags = FixupStandaloneFlags->GetInt() != 0;
		}

		ObjectSaveContext.Set(InPackage, GetTargetPlatform(), TargetPackagePath, SaveArgs.SaveFlags);
	}

	~FSaveContext()
	{
		CloseLinkerArchives();

		if (TempFilename.IsSet())
		{
			IFileManager::Get().Delete(*TempFilename.GetValue());
		}
		if (TextFormatTempFilename.IsSet())
		{
			IFileManager::Get().Delete(*TextFormatTempFilename.GetValue());
		}

		if (bNeedPreSaveCleanup && Asset)
		{
			UE::SavePackageUtilities::CallPostSaveRoot(Asset, ObjectSaveContext, bNeedPreSaveCleanup);
		}
	}

	const FSavePackageArgs& GetSaveArgs() const
	{
		return SaveArgs;
	}

	const ITargetPlatform* GetTargetPlatform() const
	{
		return SaveArgs.TargetPlatform;
	}

	UPackage* GetPackage() const
	{
		return Package;
	}

	UObject* GetAsset() const
	{
		return Asset;
	}

	const TCHAR* GetFilename() const
	{
		return Filename;
	}

	const FPackagePath& GetTargetPackagePath() const
	{
		return TargetPackagePath;
	}

	EObjectMark GetExcludedObjectMarks() const
	{
		return ExcludedObjectMarks;
	}

	EObjectFlags GetTopLevelFlags() const
	{
		return SaveArgs.TopLevelFlags;
	}

	bool IsUsingSlowTask() const
	{
		return SaveArgs.bSlowTask;
	}

	FOutputDevice* GetError() const
	{
		return SaveArgs.Error;
	}

	const FDateTime& GetFinalTimestamp() const
	{
		return SaveArgs.FinalTimeStamp;
	}

	FSavePackageContext* GetSavePackageContext() const
	{
		return SaveArgs.SavePackageContext;
	}

	bool IsCooking() const
	{
		return SaveArgs.TargetPlatform != nullptr;
	}

	bool IsProceduralSave() const
	{
		return ObjectSaveContext.bProceduralSave;
	}

	bool IsUpdatingLoadedPath() const
	{
		return ObjectSaveContext.bUpdatingLoadedPath;
	}

	bool IsFilterEditorOnly() const
	{
		return Package->HasAnyPackageFlags(PKG_FilterEditorOnly);
	}

	bool IsStripEditorOnly() const
	{
		return !(SaveArgs.SaveFlags & ESaveFlags::SAVE_KeepEditorOnlyCookedPackages);
	}

	bool IsForceByteSwapping() const
	{
		return SaveArgs.bForceByteSwapping;
	}

	bool IsWarningLongFilename() const
	{
		return SaveArgs.bWarnOfLongFilename;
	}

	bool IsTextFormat() const
	{
		return bTextFormat;
	}

	bool IsFromAutoSave() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_FromAutosave);
	}

	bool IsSaveToMemory() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_Async) || PackageWriter;
	}

	bool IsGenerateSaveError() const
	{
		return !(SaveArgs.SaveFlags & SAVE_NoError);
	}

	bool IsKeepGuid() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_KeepGUID);
	}

	bool IsKeepDirty() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_KeepDirty);
	}

	bool IsSaveUnversionedNative() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_Unversioned_Native);
	}

	bool IsSaveUnversionedProperties() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_Unversioned_Properties) && bCanUseUnversionedPropertySerialization;
	}

	bool IsComputeHash() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_ComputeHash);
	}

	bool IsConcurrent() const
	{
		return !!(SaveArgs.SaveFlags & SAVE_Concurrent);
	}

	bool IsCompareLinker() const
	{
		return !!(SaveArgs.SaveFlags & ESaveFlags::SAVE_CompareLinker);
	}

	bool CanSkipEditorReferencedPackagesWhenCooking() const
	{
		return SkipEditorRefCookingSetting;
	}

	bool IsIgnoringHeaderDiff() const
	{
		return bIgnoreHeaderDiffs;
	}

	bool IsProcessingPrestreamingRequests() const
	{
		return bIsProcessingPrestreamPackages;
	}

	bool IsFixupStandaloneFlags() const
	{
		return bIsFixupStandaloneFlags;
	}

	FUObjectSerializeContext* GetSerializeContext() const
	{
		return SerializeContext;
	}

	void SetSerializeContext(FUObjectSerializeContext* InContext)
	{
		SerializeContext = InContext;
	}

	FEDLCookChecker* GetEDLCookChecker() const
	{
		return EDLCookChecker;
	}

	void SetEDLCookChecker(FEDLCookChecker* InCookChecker)
	{
		EDLCookChecker = InCookChecker;
	}

	uint32 GetPortFlags() const
	{
		return PPF_DeepCompareInstances | PPF_DeepCompareDSOsOnly;
	}

	bool GetPreSaveCleanup() const
	{
		return bNeedPreSaveCleanup;
	}

	void SetPreSaveCleanup(bool bInNeedPreSaveCleanup)
	{
		bNeedPreSaveCleanup = bInNeedPreSaveCleanup;
	}

	bool IsStubRequested() const
	{
		return bGenerateFileStub;
	}

	void RequestStubFile()
	{
		bGenerateFileStub = true;
	}

	void AddImport(UObject* InObject, bool bIsEditorOnlyImport = false)
	{
		Imports.Add(InObject);
		if (!bIsEditorOnlyImport)
		{
			ImportsUsedInGame.Add(InObject);
		}
	}

	void AddExport(UObject* InObj, bool bNotAlwaysLoadedForEditorGame)
	{
		Exports.Add(FTaggedExport(InObj, bNotAlwaysLoadedForEditorGame));
	}

	void AddExcluded(UObject* InObject)
	{
		Excluded.Add(InObject);
	}

	void MarkUnsaveable(UObject* InObject);

	bool IsUnsaveable(UObject* InObject) const;

	bool IsImport(UObject* InObject) const
	{
		return Imports.Contains(InObject);
	}

	bool IsExport(UObject* InObject) const
	{
		return Exports.Contains(InObject);
	}

	bool IsIncluded(UObject* InObject) const
	{
		return IsImport(InObject) || IsExport(InObject);
	}

	bool IsExcluded(UObject* InObject) const
	{
		return Excluded.Contains(InObject);
	}

	TSet<FTaggedExport>& GetExports()
	{
		return Exports;
	}

	const TSet<UObject*>& GetImports() const
	{
		return Imports;
	}

	const TSet<UObject*>& GetImportsUsedInGame() const
	{
		return ImportsUsedInGame;
	}

	const TArray<FName>& GetSoftPackageReferenceList() const
	{
		return SoftPackageReferenceList;
	}

	TArray<FName>& GetSoftPackageReferenceList()
	{
		return SoftPackageReferenceList;
	}

	const TSet<FName>& GetSoftPackagesUsedInGame() const
	{
		return SoftPackagesUsedInGame;
	}

	const TMap<UObject*, TArray<FName>>& GetSearchableNamesObjectMap() const
	{
		return SearchableNamesObjectMap;
	}

	TMap<UObject*, TArray<FName>>& GetSearchableNamesObjectMap()
	{
		return SearchableNamesObjectMap;
	}

	const TSet<FNameEntryId>& GetNamesReferencedFromExportData() const
	{
		return NamesReferencedFromExportData;
	}

	const TSet<FNameEntryId>& GetNamesReferencedFromPackageHeader() const
	{
		return NamesReferencedFromPackageHeader;
	}

	const FCustomVersionContainer& GetCustomVersions() const
	{
		return CustomVersions;
	}

	const TMap<UObject*, TSet<UObject*>>& GetObjectDependencies() const
	{
		return ExportObjectDependencies;
	}

	const TMap<UObject*, TSet<UObject*>>& GetNativeObjectDependencies() const
	{
		return ExportNativeObjectDependencies;
	}

	const TSet<UPackage*>& GetPrestreamPackages() const
	{
		return PrestreamPackages;
	}

	TSet<UPackage*>& GetPrestreamPackages()
	{
		return PrestreamPackages;
	}

	bool IsPrestreamPackage(UPackage* InPackage) const
	{
		return PrestreamPackages.Contains(InPackage);
	}

	void AddPrestreamPackages(UPackage* InPackage)
	{
		PrestreamPackages.Add(InPackage);
	}

	bool NameExists(FNameEntryId ComparisonId) const
	{
		for (FNameEntryId DisplayId : NamesReferencedFromExportData)
		{
			if (FName::GetComparisonIdFromDisplayId(DisplayId) == ComparisonId)
			{
				return true;
			}
		}
		for (FNameEntryId DisplayId : NamesReferencedFromPackageHeader)
		{
			if (FName::GetComparisonIdFromDisplayId(DisplayId) == ComparisonId)
			{
				return true;
			}
		}
		return false;
	}

	void SetCustomVersions(FCustomVersionContainer InCustomVersions)
	{
		CustomVersions = MoveTemp(InCustomVersions);
	}

	FLinkerSave* GetLinker() const
	{
		return Linker.Get();
	}

	bool CloseLinkerArchives()
	{
		bool bSuccess = true;
		if (Linker)
		{
			bSuccess = Linker->CloseAndDestroySaver();
		}
		StructuredArchive.Reset();
		Formatter.Reset();
		TextFormatArchive.Reset();
		return bSuccess;
	}

	FSavePackageResultStruct GetFinalResult()
	{
		auto HashCompletionFunc = [](FMD5& State)
		{
			FMD5Hash OutputHash;
			OutputHash.Set(State);
			return OutputHash;
		};

		if (Result != ESavePackageResult::Success)
		{
			return Result;
		}

		ESavePackageResult FinalResult = IsStubRequested() ? ESavePackageResult::GenerateStub : ESavePackageResult::Success;
		return FSavePackageResultStruct(FinalResult, TotalPackageSizeUncompressed,
			AsyncWriteAndHashSequence.Finalize(EAsyncExecution::TaskGraph, MoveTemp(HashCompletionFunc)),
			SerializedPackageFlags, IsCompareLinker() ? MoveTemp(Linker) : nullptr);
	}

	FObjectSaveContextData& GetObjectSaveContext()
	{
		return ObjectSaveContext;
	}

	IPackageWriter* GetPackageWriter() const
	{
		return PackageWriter;
	}

	ISavePackageValidator* GetPackageValidator() const
	{
		return SaveArgs.SavePackageContext ? SaveArgs.SavePackageContext->GetValidator() : nullptr;
	}

public:
	ESavePackageResult Result;

	TPimplPtr<FLinkerSave> Linker;
	TUniquePtr<FArchive> TextFormatArchive;
	TUniquePtr<FArchiveFormatterType> Formatter;
	TUniquePtr<FStructuredArchive> StructuredArchive;

	TOptional<FString> TempFilename;
	TOptional<FString> TextFormatTempFilename;

	EPropertyLocalizationGathererResultFlags GatherableTextResultFlags = EPropertyLocalizationGathererResultFlags::Empty;

	int64 TotalPackageSizeUncompressed = 0;
	int32 OffsetAfterPackageFileSummary = 0;
	int32 OffsetAfterImportMap = 0;
	int32 OffsetAfterExportMap = 0;
	int64 OffsetAfterPayloadToc = 0;
	int32 SerializedPackageFlags = 0;
	TAsyncWorkSequence<FMD5> AsyncWriteAndHashSequence;
	TArray<FLargeMemoryWriter, TInlineAllocator<4>> AdditionalFilesFromExports;
	FSavePackageOutputFileArray AdditionalPackageFiles;
private:
	friend class FPackageHarvester;

	// Args
	UPackage* Package;
	UObject* Asset;
	FPackagePath TargetPackagePath;
	const TCHAR* Filename;
	FSavePackageArgs SaveArgs;
	IPackageWriter* PackageWriter;

	// State context
	FUObjectSerializeContext* SerializeContext = nullptr;
	FObjectSaveContextData ObjectSaveContext;
	bool bCanUseUnversionedPropertySerialization = false;
	bool bTextFormat = false;
	bool bIsProcessingPrestreamPackages = false;
	bool bIsFixupStandaloneFlags = false;
	bool bNeedPreSaveCleanup = false;
	bool bGenerateFileStub = false;
	bool bIgnoreHeaderDiffs = false;

	// Config classes shared with the old Save
	FCanSkipEditorReferencedPackagesWhenCooking SkipEditorRefCookingSetting;

	// Pointer to the EDLCookChecker associated with this context
	FEDLCookChecker* EDLCookChecker = nullptr;

	// Matching any mark in ExcludedObjectMarks indicates that an object should be excluded from being either an import or an export for this save
	const EObjectMark ExcludedObjectMarks;
	// Set of objects excluded (import or exports) through through marks or otherwise (i.e. transient flags, etc)
	TSet<UObject*> Excluded;

	// Set of objects marked as export
	TSet<FTaggedExport> Exports;
	// Set of objects marked as import
	TSet<UObject*> Imports;
	// Subset of this->Imports which are referenced from not-editoronly properties
	TSet<UObject*> ImportsUsedInGame;
	// Set of names referenced from export serialization
	TSet<FNameEntryId> NamesReferencedFromExportData;
	// Set of names referenced from the package header (import and export table object names etc)
	TSet<FNameEntryId> NamesReferencedFromPackageHeader;
	// List of soft package reference found
	TArray<FName> SoftPackageReferenceList;
	// Subset of this->SoftPackageReferenceList which are referenced from not-editoronly properties
	TSet<FName> SoftPackagesUsedInGame;

	// Map of objects to their list of searchable names
	TMap<UObject*, TArray<FName>> SearchableNamesObjectMap;
	// Map of objects to their dependencies
	TMap<UObject*, TSet<UObject*>> ExportObjectDependencies;
	// Map of objects to their native dependencies
	TMap<UObject*, TSet<UObject*>> ExportNativeObjectDependencies;
	// Set of harvested prestream packages
	TSet<UPackage*> PrestreamPackages;
	// Harvested custom versions
	FCustomVersionContainer CustomVersions;
};
