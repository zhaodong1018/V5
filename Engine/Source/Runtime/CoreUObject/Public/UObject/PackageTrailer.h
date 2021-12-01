// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Compression/CompressedBuffer.h"
#include "Containers/Map.h"
#include "Virtualization/PayloadId.h"

class FArchive;
class FLinkerSave;
class FPackagePath;

namespace UE
{

/** Trailer Format
 * The FPackageTrailer is a container that will commonly be appended to the end of a package file. The main purpose of the trailer is to store
 * the bulkdata payloads contained by the package until they are virtualized or moved to an additional storage location.
 * 
 * By storing the payloads in a data format adjacent to the rest of the package we can perform the virtualization process without needing to 
 * re-save the package itself which in turn should allow for external tools to be able to perform the virtualization process themselves
 * rather than needing to force it through engine code.
 * 
 * The package trailer is intended to an easy format for external code/script to be able to manipulate. To make things clearer we do not 
 * serialize containers directly but write out each data structure one at a time so that it should be easy to see how to manipulate the file.
 * 
 * The file is split into three parts:
 * 
 * [Header]
 * The header contains the useful info about the trailer and the payloads in general. @See UE::FLookupTableEntry for details about 
 * the look up table's data.
 * 
 * [Payload Data]
 * If the trailer is in the workspace domain package then we will store all non-virtualized payloads here. If the trailer is in the editor 
 * domain then there will be no payload data section and the header will be referencing the trailer in the workspace domain instead.
 * 
 * [Footer]
 * The footer allows for us to load the trailer in reverse and replicates the end of package file tag (PACKAGE_FILE_TAG), it should only be
 * used for finding the start of the trailer or validation.
 * 
 * CurrentVersion UE::EPackageTrailerVersion::INITIAL
 * ______________________________________________________________________________________________________________________________________________
 * | [Header]																																	|
 * | Tag				| uint64			| Should match FHeader::HeaderTag, used to identify that the data being read is an FPackageTrailer	|
 * | Version			| uint32			| Version number of the format@see UE::EPackageTrailerVersion										|
 * | HeaderLength		| uint32			| The total size of the header on disk in bytes.													|
 * | PayloadsDataLength	| uint64			| The total size of the payload data on disk in bytes												|
 * | NumPayloads		| int32				| The number of payloads in LookupTableArray														|
 * | LookupTableArray	| FLookupTableEntry | An array of FLookupTableEntry @see UE::Private::FLookupTableEntry									|
 * |____________________________________________________________________________________________________________________________________________|
 * | [Payload Data]																																|
 * | Array				| FCompressedBuffer | A binary blob containing all of the payloads. Individual payloads can be found via				|
 * |										 the LookupTableArray found in the header.															|
 * |____________________________________________________________________________________________________________________________________________|
 * [Footer]
 * | Tag				| uint64			| Should match FFooter::FooterTag, used to identify that the data being read is an FPackageTrailer	|
 * | TrailerLength		| uint64			| The total size of the trailer on disk in bytes. Can be used to find the start of the trailer when	|
 * |										  reading backwards.																				|
 * | PackageTag			| uint32			| The end of package tag, PACKAGE_FILE_TAG. This is used to validate that a package file on disk is	|
 * |										  not corrupt. By ending the trailer with this tag we allow that validation code to work.			|
 * |____________________________________________________________________________________________________________________________________________|
 */

 /** Used to filter requests to a specific type of payload */
enum class EPayloadFilter
{
	/** All payload types. */
	All,
	/** All payloads stored locally in the package file. */
	Local,
	/** All payloads stored in a virtualized backend. */
	Virtualized
};

/** Used to show the status of a payload */
enum class EPayloadStatus
{
	/** The payload is not registered in the package trailer */
	NotFound = 0,
	/** The payload is stored locally on disk */
	StoredLocally,
	/** The payload is virtualized and needs to be accessed via the IVirtualizationSystem */
	StoredVirtualized,
};

/** Lists the various methods of payload access that the trailer supports */
enum class EPayloadAccessMode : uint8
{
	/** The payloads are stored in the Payload Data segment of the trailer and the offsets in FLookupTableEntry will be relative to the start of this segment */
	Relative = 0,
	/** The payloads are stored in the trailer of another file (most likely the workspace domain package file) and the offsets in FLookupTableEntry are absolute offsets in that external file */
	Referenced
};

namespace Private
{

struct FLookupTableEntry
{
	/** Size of the entry when serialized to disk in bytes */
	static constexpr uint32 SizeOnDisk = 44;	// Identifier		| 20 bytes
												// OffsetInFile		| 8 bytes
												// CompressedSize	| 8 bytes
												// RawSize			| 8 bytes

	FLookupTableEntry() = default;
	FLookupTableEntry(const Virtualization::FPayloadId& InIdentifier, uint64 InRawSize);

	friend FArchive& operator<<(FArchive& Ar, FLookupTableEntry& Entry);

	[[nodiscard]] bool IsVirtualized() const
	{
		return OffsetInFile == INDEX_NONE;
	}

	/** Identifier for the payload */
	Virtualization::FPayloadId Identifier;
	/** The offset into the file where we can find the payload, note that a virtualized payload will have an offset of INDEX_NONE */
	int64 OffsetInFile = INDEX_NONE;
	/** The size of the payload when compressed. This will be the same value as RawSize if the payload is not compressed */
	uint64 CompressedSize = INDEX_NONE;
	/** The size of the payload when uncompressed. */
	uint64 RawSize = INDEX_NONE;
};

} // namespace Private

/** 
 * This class is used to build a FPackageTrailer and write it disk.
 * 
 * While saving a package, payloads should be added to a FPackageTrailer via ::AddPayload then once
 * the package has been saved to disk ::BuildAndAppendTrailer should be called. 
 */
class COREUOBJECT_API FPackageTrailerBuilder
{
public:
	using AdditionalDataCallback = TFunction<void(FLinkerSave& LinkerSave)>;

	[[nodiscard]] static FPackageTrailerBuilder Create(const class FPackageTrailer& Trailer, FArchive& Ar);

	// Methods that can be called while building the trailer

	/*
	 * Adds a payload to the builder to be written to the trailer. Duplicate payloads will be discarded and only a 
	 * single instance stored in the trailer.
	 * 
	 * @param Identifier	The identifier of the payload
	 * @param Payload		The payload data
	 * @param Callback		This callback will be invoked once the FPackageTrailer has been built and appended to disk.
	 */
	void AddPayload(const Virtualization::FPayloadId& Identifier, FCompressedBuffer Payload, AdditionalDataCallback&& Callback);
	
	/*
	 * @param ExportsArchive	The linker associated with the package being written to disk.
	 * @param DataArchive		The archive where the package data has been written to. This is where the FPackageTrailer will be written to
	 * @return True if the builder was created and appended successfully and false if any error was encountered
	 */
	[[nodiscard]] bool BuildAndAppendTrailer(FLinkerSave* Linker, FArchive& DataArchive);
	
	/** Returns if the builder has any payload entries or not */
	[[nodiscard]] bool IsEmpty() const;

	// Methods that can be called after building the trailer and appending it to the package file
	[[nodiscard]] int64 FindPayloadOffset(const Virtualization::FPayloadId& Identifier) const;

private:
	
	/** All of the data required to add a payload that is stored on disk */
	struct LocalEntry
	{
		LocalEntry() = default;
		LocalEntry(FCompressedBuffer&& InPayload)
			: Payload(InPayload)
		{

		}
		~LocalEntry() = default;

		FCompressedBuffer Payload;
	};

	/** All of the data required to add a payload that is virtualized */
	struct VirtualizedEntry
	{
		VirtualizedEntry() = default;
		VirtualizedEntry(int64 InCompressedSize, int64 InRawSize)
			: CompressedSize(InCompressedSize)
			, RawSize(InRawSize)
		{

		}
		~VirtualizedEntry() = default;

		int64 CompressedSize = INDEX_NONE;
		int64 RawSize = INDEX_NONE;
	};

	// Members used when building the trailer

	/** Payloads that will be stored locally when the trailer is written to disk */
	TMap<Virtualization::FPayloadId, LocalEntry> LocalEntries;
	/** Payloads that are already virtualized and so will not be written to disk */
	TMap<Virtualization::FPayloadId, VirtualizedEntry> VirtualizedEntries;

	/** Callbacks to invoke once the trailer has been written to the end of a package */
	TArray<AdditionalDataCallback> Callbacks;

	// Members that are only valid one after building the trailer and appending it to the package file

	/** Where in the package file the trailer is located */
	int64 TrailerPositionInFile = INDEX_NONE;

	/** Where in the package file that the payload data is located */
	int64 PayloadPosInFile = INDEX_NONE;

	/** The same look up table that the trailer would have */
	TArray<Private::FLookupTableEntry> PayloadLookupTable;
};

/** 
 *
 * The package trailer should only ever stored the payloads in the workspace domain. If the package trailer is in the editor
 * domain then it's values should be valid, but when loading non-virtualized payloads they need to come from the workspace 
 * domain package.
 */
class COREUOBJECT_API FPackageTrailer
{
public:
	/** 
	 * Returns if the feature is enabled or disabled.
	 * 
	 * Note that this is for development purposes only and should ship as always enabled!
	 */
	[[nodiscard]] static bool IsEnabled();

	/** Try to load a trailer from a given package path. Note that it will always try to load the trailer from the workspace domain */
	[[nodiscard]] static bool TryLoadFromPackage(const FPackagePath& PackagePath, FPackageTrailer& OutTrailer);

	FPackageTrailer() = default;
	~FPackageTrailer() = default;

	/** 
	 * Serializes the trailer from the given archive assuming that the seek position of the archive is already at the correct position
	 * for the trailer.
	 * 
	 * @param Ar	The archive to load the trailer from
	 * @return		True if a valid trailer was found and was able to be loaded, otherwise false.
	 */
	[[nodiscard]] bool TryLoad(FArchive& Ar);

	/** 
	 * Serializes the trailer from the given archive BUT assumes that the seek position of the archive is at the end of the trailer
	 * and so will attempt to read the footer first and use that to find the start of the trailer in order to read the header.
	 * 
	 * @param Ar	The archive to load the trailer from
	 * @return		True if a valid trailer was found and was able to be loaded, otherwise false.
	 */
	[[nodiscard]] bool TryLoadBackwards(FArchive& Ar);

	/** 
	 * Loads the a payload from the provided archive 
	 * 
	 * @param Id The payload to load
	 * @param Ar The archive to load the payload from, it is assumed that this archive is the package file in the workspace domain
	 * 
	 * @return	The payload in the form of a FCompressedBuffer. If the payload does not exist in the trailer or in the archive then
	 *			the FCompressedBuffer will be null.
	 */
	[[nodiscard]] FCompressedBuffer LoadPayload(Virtualization::FPayloadId Id, FArchive& Ar) const;

	/** 
	 * Calling this indicates that the payload has been virtualized and will no longer be stored on disk. 
	 * 
	 * @param Identifier The payload that has been virtualized
	 * @return True if the payload was in the trailer, otherwise false
	 */
	[[nodiscard]] bool UpdatePayloadAsVirtualized(Virtualization::FPayloadId Identifier);

	/** Attempt to find the status of the given payload. @See EPayloadStatus */
	[[nodiscard]] EPayloadStatus FindPayloadStatus(Virtualization::FPayloadId Id) const;

	/** Returns the total size of the of the trailer on disk in bytes */
	[[nodiscard]] int64 GetTrailerLength() const;

	/** Returns an array of the payloads that match the given type. @See EPayloadType */
	[[nodiscard]] TArray<Virtualization::FPayloadId> GetPayloads(EPayloadFilter Type) const;

	struct FHeader
	{
		/** Unique value used to identify the header */
		static constexpr uint64 HeaderTag = 0xD1C43B2E80A5F697;

		/** 
		 * Size of the static header data when serialized to disk in bytes. Note that we still need to 
		 * add the size of the data in PayloadLookupTable to get the final header size on disk 
		 */
		static constexpr uint32 StaticHeaderSizeOnDisk = 29;	// HeaderTag			| 8 bytes
																// Version				| 4 bytes
																// HeaderLength			| 4 bytes
																// PayloadsDataLength	| 8 bytes
																// AccessMode			| 1 byte
																// NumPayloads			| 4 bytes

		/** Expected tag at the start of the header */
		uint64 Tag = 0;
		/** Version of the header */
		int32 Version = INDEX_NONE;
		/** Total length of the header on disk in bytes */
		uint32 HeaderLength = 0;
		/** Total length of the payloads on disk in bytes */
		uint64 PayloadsDataLength = 0;
		/** What sort of access to the payloads does the trailer have */
		EPayloadAccessMode AccessMode = EPayloadAccessMode::Relative;
		/** Lookup table for the payloads on disk */
		TArray<Private::FLookupTableEntry> PayloadLookupTable;
	};

	struct FFooter
	{
		/** Unique value used to identify the footer */
		static constexpr uint64 FooterTag = 0x29BFCA045138DE76;

		/** Size of the footer when serialized to disk in bytes */
		static constexpr uint32 SizeOnDisk = 20;	// Tag				| 8 bytes
													// TrailerLength	| 8 bytes
													// PackageTag		| 4 bytes

		/** Expected tag at the start of the footer */
		uint64 Tag = 0;
		/** Total length of the trailer on disk in bytes */
		uint64 TrailerLength = 0;	
		/** End the trailer with PACKAGE_FILE_TAG, which we expect all package files to end with */
		uint32 PackageTag = 0;
	};

private:
	friend class FPackageTrailerBuilder;

	/** Where in the workspace domain package file the trailer is located */
	int64 TrailerPositionInFile = INDEX_NONE;

	/** 
	 * The header of the trailer. Since this contains the lookup table for payloads we keep this in memory once the trailer
	 * has been loaded. There is no need to keep the footer in memory */
	FHeader Header;
};

/**
 * Used to find the identifiers of the payload in a given package. Note that this will return the payloads included in the
 * package on disk and will not take into account any edits to the package if they are in memory and unsaved.
 *
 * @param PackagePath	The package to look in.
 * @param Filter		What sort of payloads should be returned. @see EPayloadType
 * @param OutPayloadIds	This array will be filled with the FPayloadId values found in the package that passed the filter.
 *						Note that existing content in the array will be preserved. It is up to the caller to empty it.
 *
 * @return 				True if the package was parsed successfully (although it might not have contained any payloads) and false if opening or parsing the package file failed.
 */
[[nodiscard]] COREUOBJECT_API bool FindPayloadsInPackageFile(const FPackagePath& PackagePath, EPayloadFilter Filter, TArray<Virtualization::FPayloadId>& OutPayloadIds);

} //namespace UE