// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/Future.h"
#include "Compression/CompressedBuffer.h"
#include "HAL/Platform.h"
#include "Memory/SharedBuffer.h"
#include "Misc/Guid.h"
#include "Misc/PackagePath.h"
#include "Virtualization/PayloadId.h"

class FArchive;
class UObject;

//TODO: At some point it might be a good idea to uncomment this to make sure that FVirtualizedUntypedBulkData is
//		never used at runtime (requires a little too much reworking of some assets for now though)
//#if WITH_EDITORONLY_DATA

namespace UE::Virtualization
{

namespace Private
{
/** A wrapper around the oodle compression settings used by FVirtualizedUntypedBulkData. */
struct FCompressionSettings
{
	COREUOBJECT_API FCompressionSettings();
	FCompressionSettings(const FCompressedBuffer& Buffer);

	[[nodiscard]] bool operator ==(const FCompressionSettings& Other) const;
	[[nodiscard]] bool operator != (const FCompressionSettings& Other) const;

	void Reset();

	void Set(ECompressedBufferCompressor InCompressor, ECompressedBufferCompressionLevel InCompressionLevel);
	void SetToDefault();
	void SetToDisabled();

	[[nodiscard]] bool IsSet() const;
	[[nodiscard]] bool IsCompressed() const;

	[[nodiscard]] ECompressedBufferCompressor GetCompressor() const;
	[[nodiscard]] ECompressedBufferCompressionLevel GetCompressionLevel();

private:

	ECompressedBufferCompressor Compressor;
	ECompressedBufferCompressionLevel CompressionLevel;

	bool bIsSet;
};
} // namespace Private

/** 
 * A set of higher level compression options that avoid the need to set the specific
 * oodle options.
 */
enum class ECompressionOptions : uint8
{
	/** Use default compression settings. */
	Default,
	/** Disable compression for the bulkdata entirely. */
	Disabled,
};

/**
 * The goal of this class is to provide an editor time version of BulkData that will work with the content
 * virtualization system.
 *
 * Assuming that the DDC is hot, the virtualized payloads are accessed relatively infrequently, usually when the package
 * is being edited in the editor in some manner. So the payload access is designed around this. If the data is frequently 
 * accessed when running the editor then the user would not gain from having it virtualized as they would end up pulling
 * it immediately anyway.
 *
 * The biggest difference with normal bulkdata is that the access times might be significantly longer if the
 * payload is not readily available which is why the only way to access the payload is by a TFuture or a callback
 * so that the caller is forced to consider how to handle the potential stall and hopefully organize their code 
 * in such a way that the time lag is not noticeable to the user.
 *
 * The second biggest difference is that the caller will own the payload memory once it is returned to them, unlike
 * the old bulkdata class which would retain ownership. Which forces the calling code to be in control of when the 
 * memory is actually allocated and for how long. With the old bulkdata class a single access would leave that memory
 * allocated in a non-obvious way and would lead to memory bloats within the editor.
 *
 * The method ::GetGuid can be used to access a unique identifier for the payload, currently it is based on the 
 * payload itself, so that two objects with the same payload would both have the same Guid. The intent is that we 
 * would be able to share local copies of the payload between branches to reduce the cost of having multiple branches 
 * with similar data on the same machine.
 * 
 */

/** The base class with no type */
class COREUOBJECT_API FVirtualizedUntypedBulkData
{
public:
	FVirtualizedUntypedBulkData() = default;
	FVirtualizedUntypedBulkData(FVirtualizedUntypedBulkData&& Other);
	FVirtualizedUntypedBulkData& operator=(FVirtualizedUntypedBulkData&& Other);

	FVirtualizedUntypedBulkData(const FVirtualizedUntypedBulkData& Other);
	FVirtualizedUntypedBulkData& operator=(const FVirtualizedUntypedBulkData& Other);

	~FVirtualizedUntypedBulkData();

	/** 
	 * Convenience method to make it easier to convert from BulkData to FVirtualizedBulkData and sets the Guid 
	 *
	 * @param BulkData	The bulkdata object to create from.
	 * @param Guid		A guid associated with the bulkdata object which will be used to identify the payload.
	 *					This MUST remain the same between sessions so that the payloads key remains consistent!
	 */
	void CreateFromBulkData(FUntypedBulkData& BulkData, const FGuid& Guid, UObject* Owner);
	/** Fix legacy content that created the Id from non-unique Guids. */
	void CreateLegacyUniqueIdentifier(UObject* Owner);

	/** 
	 * Used to serialize the bulkdata to/from a FArchive
	 * 
	 * @param Ar	The archive to serialize the bulkdata.
	 * @param Owner	The UObject that contains the bulkdata object, if this is a nullptr then the bulkdata will
	 *				assume that it must serialize the payload immediately to memory as it will not be able to
	 *				identify it's package path.
	 * @param bAllowRegistry Legacy parameter to skip registration when loading BulkData we know we will need to
	 *				modify the identifier of. Should always be true for non-legacy serialization.
	 */
	void Serialize(FArchive& Ar, UObject* Owner, bool bAllowRegister=true);

	/** Reset to a truly empty state */
	void Reset();

	// TODO: Probably can just remove this as there probably isn't a good use case for unloading updated payloads as there is no
	// way for us to restore it. In that case ::Reset might as well be used.
	/** Unloads the data (if possible) but leaves it in a state where the data can be reloaded */
	void UnloadData();

	/** 
	 * Removes the ability for the bulkdata object to load it's payload from disk (if it was doing so) 
	 * 
	 * @param Ar						The archive that the bulkdata is being detached from. This should match AttachedAr.
	 * @param bEnsurePayloadIsLoaded	If true and the bulkdata currently is storing it's payload as a package on disk then
	 *									the payload should be loaded into memory so that it can be accessed in the future.
	 *									If false then the payload is not important and does not need to be loaded.
	 */
	void DetachFromDisk(FArchive* Ar, bool bEnsurePayloadIsLoaded);

	/** Returns a unique identifier for the object itself. */
	FGuid GetIdentifier() const;

	/** Returns an unique identifier for the content of the payload. */
	const FPayloadId& GetPayloadId() const 
	{ 
		return PayloadContentId; 
	}

	/** Returns the size of the payload in bytes. */
	int64 GetPayloadSize() const 
	{ 
		return PayloadSize; 
	}

	/** Returns true if the bulkdata object contains a valid payload greater than zero bytes in size. */
	bool HasPayloadData() const 
	{ 
		return PayloadSize > 0; 
	}

	/** Returns if the payload would require loading in order to be accessed. Returns false if the payload is already in memory or of zero length */
	bool DoesPayloadNeedLoading() const
	{
		return Payload.IsNull() && PayloadSize > 0; 
	}

	/** Returns an immutable FCompressedBuffer reference to the payload data. */
	TFuture<FSharedBuffer> GetPayload() const;

	/**
	 * Returns an immutable FCompressedBuffer reference to the payload data.
	 *
	 * Note that depending on the internal storage formats, the payload might not actually be compressed, but that
	 * will be handled by the FCompressedBuffer interface. Call FCompressedBuffer::Decompress() to get access to
	 * the payload in FSharedBuffer format.
	 */
	TFuture<FCompressedBuffer> GetCompressedPayload() const;

	/**
	 * Replaces the existing payload (if any) with a new one. 
	 * It is important to consider the ownership model of the payload being passed in to the method.
	 * 
	 * To pass in a raw pointer use 'FSharedBuffer::...(Data, Size)' to create a valid FSharedBuffer.
	 * Use 'FSharedBuffer::MakeView' if you want to retain ownership on the data being passed in, and use
	 * 'FSharedBuffer::TakeOwnership' if you are okay with the bulkdata object taking over ownership of it.
	 * The bulkdata object must own its internal buffer, so if you pass in a non-owned FSharedBuffer (ie
	 * by using 'FSharedBuffer::MakeView') then a clone of the data will be created internally and assigned
	 * to the bulkdata object.
	 *
	 * @param InPayload	The payload that this bulkdata object should reference. @see FSharedBuffer
	 */
	void UpdatePayload(FSharedBuffer InPayload);

	/**
	 * Utility struct used to compute the Payload ID before calling UpdatePayload
	 */
	struct COREUOBJECT_API FSharedBufferWithID
	{
		FSharedBufferWithID(FSharedBuffer InPayload);

		FSharedBufferWithID() = default;
		FSharedBufferWithID(FSharedBufferWithID&&) = default;
		FSharedBufferWithID& operator=(FSharedBufferWithID&&) = default;

		FSharedBufferWithID(const FSharedBufferWithID&) = delete;
		FSharedBufferWithID& operator=(const FSharedBufferWithID&) = delete;

	private:
		friend FVirtualizedUntypedBulkData;

		FSharedBuffer Payload;
		FPayloadId PayloadId;
	};

	/**
	 * Allows the existing payload to be replaced with a new one.
	 *
	 * 
	 * To pass in a raw pointer use 'FSharedBuffer::...(Data, Size)' to create a valid FSharedBuffer.
	 * Use 'FSharedBuffer::MakeView' if you want to retain ownership on the data being passed in, and use
	 * 'FSharedBuffer::TakeOwnership' if you are okay with the bulkdata object taking over ownership of it.
	 * The bulkdata object must own its internal buffer, so if you pass in a non-owned FSharedBuffer (ie
	 * by using 'FSharedBuffer::MakeView') then a clone of the data will be created internally and assigned
	 * to the bulkdata object.
	 * 
	 * Use this override if you want compute PayloadId before updating the bulkdata
	 *
	 * @param InPayload				The payload to update the bulkdata with
	 */
	void UpdatePayload(FSharedBufferWithID InPayload);

	/** 
	 * Sets the compression options to be applied to the payload during serialization.
	 * 
	 * These settings will continue to be used until the bulkdata object is reset, a subsequent
	 * call to ::SetCompressionOptions is made or the owning package is serialized to disk.
	 * 
	 * @param Option	The high level option to use. @see UE::Virtualization::ECompressionOptions
	 */ 
	void SetCompressionOptions(ECompressionOptions Option);

	/** 
	 * Sets the compression options to be applied to the payload during serialization.
	 * 
	 * These settings will continue to be used until the bulkdata object is reset, a subsequent
	 * call to ::SetCompressionOptions is made or the owning package is serialized to disk.
	 * 
	 * @param Compressor		The Oodle compressor to use. @see ECompressedBufferCompressor
	 * @param CompressionLevel	The Oodle compression level to use. @see ECompressedBufferCompressionLevel
	 */
	void SetCompressionOptions(ECompressedBufferCompressor Compressor, ECompressedBufferCompressionLevel CompressionLevel);

	/**
	* Get the CustomVersions used in the file containing the payload. Currently this is assumed
	* to always be the versions in the InlineArchive
	* 
	* @param InlineArchive The archive that was used to load this object
	* 
	* @return The CustomVersions that apply to the interpretation of the payload.
	*/
	FCustomVersionContainer GetCustomVersions(FArchive& InlineArchive);

	/**
	 * Set this BulkData into Torn-Off mode. It will no longer register with the BulkDataRegistry, even if
	 * copied from another BulkData, and it will pass on this flag to any BulkData copied/moved from it.
	 * Use Reset() to remove this state. Torn-off BulkDatas share the guid with the BulkData they copy from.
	 */
	void TearOff();

	/** Make a torn-off copy of this bulk data. */
	FVirtualizedUntypedBulkData CopyTornOff() const 
	{ 
		return FVirtualizedUntypedBulkData(*this, ETornOff()); 
	}

	// Functions used by the BulkDataRegistry

	/** Used to serialize the bulkdata to/from a limited cache system used by the BulkDataRegistry. */
	void SerializeForRegistry(FArchive& Ar);
	/** Return true if the bulkdata has a source location that persists between editor processes (package file or virtualization). */
	bool CanSaveForRegistry() const;
	/** Return whether the BulkData has legacy payload id that needs to be updated from loaded payload before it can be used in DDC. */
	bool HasPlaceholderPayloadId() const { return EnumHasAnyFlags(Flags, EFlags::LegacyKeyWasGuidDerived); }
	/** Return whether the BulkData is an in-memory payload without a persistent source location. */
	bool IsMemoryOnlyPayload() const;
	/** Load the payload and set the correct payload id, if the bulkdata has a PlaceholderPayloadId. */
	void UpdatePayloadId();

protected:
	enum class ETornOff {};
	FVirtualizedUntypedBulkData(const FVirtualizedUntypedBulkData& Other, ETornOff);

private:
	friend struct FTocEntry;

	/** Flags used to store additional meta information about the bulk data */
	enum class EFlags : uint32
	{
		/** No flags are set */
		None						= 0,
		/** Is the data actually virtualized or not? */
		IsVirtualized				= 1 << 0,
		/** Does the package have access to a .upayload file? */
		HasPayloadSidecarFile		= 1 << 1,
		/** The bulkdata object is currently referencing a payload saved under old bulkdata formats */
		ReferencesLegacyFile		= 1 << 2,
		/** The legacy file being referenced is stored with Zlib compression format */
		LegacyFileIsCompressed		= 1 << 3,
		/** The payload should not have compression applied to it. It is assumed that the payload is already 
			in some sort of compressed format, see the compression documentation above for more details. */
		DisablePayloadCompression	= 1 << 4,
		/** The legacy file being referenced derived its key from guid and it should be replaced with a key-from-hash when saved */
		LegacyKeyWasGuidDerived		= 1 << 5,
		/** The Guid has been registered with the BulkDataRegistry */
		HasRegistered				= 1 << 6,
		/** The BulkData object is a copy used only to represent the id and payload; it does not communicate with the BulkDataRegistry, and will point DDC jobs toward the original BulkData */
		IsTornOff					= 1 << 7,

		TransientFlags				= HasRegistered | IsTornOff,
	};

	/** Used to control what level of error reporting we return from some methods */
	enum ErrorVerbosity
	{
		/** No errors should be logged */
		None = 0,
		/** Everything should be logged */
		All
	};

	FRIEND_ENUM_CLASS_FLAGS(EFlags);

	void UpdatePayloadImpl(FSharedBuffer&& InPayload, FPayloadId&& InPayloadID);

	FCompressedBuffer GetDataInternal() const;

	FCompressedBuffer LoadFromDisk() const;
	FCompressedBuffer LoadFromPackageFile() const;
	FCompressedBuffer LoadFromPackageTrailer() const;
	FCompressedBuffer LoadFromSidecarFile() const;
	FCompressedBuffer LoadFromSidecarFileInternal(ErrorVerbosity Verbosity) const;

	bool SerializeData(FArchive& Ar, FCompressedBuffer& Payload, const EFlags PayloadFlags) const;

	void PushData(const FPackagePath& InPackagePath);
	FCompressedBuffer PullData() const;

	FPackagePath GetPackagePathFromOwner(UObject* Owner, EPackageSegment& OutPackageSegment) const;

	bool CanUnloadData() const;

	void UpdateKeyIfNeeded();

	void RecompressForSerialization(FCompressedBuffer& InOutPayload, EFlags PayloadFlags) const;
	EFlags BuildFlagsForSerialization(FArchive& Ar, bool bUpgradeLegacyData) const;

	bool IsDataVirtualized() const 
	{ 
		return EnumHasAnyFlags(Flags, EFlags::IsVirtualized); 
	}

	bool HasPayloadSidecarFile() const 
	{ 
		return EnumHasAnyFlags(Flags, EFlags::HasPayloadSidecarFile); 
	}

	bool IsReferencingOldBulkData() const 
	{ 
		return EnumHasAnyFlags(Flags, EFlags::ReferencesLegacyFile); 
	}

	void Register(UObject* Owner);
	void Unregister();

	/** Utility to return an apt error message if a payload loaded from this object is considered corrupt. Handles the formatting depending on the various payload flags etc. */
	FString GetCorruptedPayloadErrorMsgForLoad() const;

	/** 
	 * Utility to return an apt error message if the payload is invalid when trying to save the bulkdata. It will try to provide the best info from the given options.
	 * 
	 * @param Linker	The FLinkerSave being used to save the bulkdata at the point of error. This can be nullptr if the bulkdata is not being saved to a package on disk.
	 * @return			The formatted error message.
	 */
	FText GetCorruptedPayloadErrorMsgForSave(FLinkerSave* Linker) const;

	/** Unique identifier for the bulkdata object itself */
	FGuid BulkDataId;

	/** Unique identifier for the contents of the payload*/
	FPayloadId PayloadContentId;

	/** Pointer to the payload if it is held in memory (it has been updated but not yet saved to disk for example) */
	FSharedBuffer Payload;

	/** Length of the payload in bytes */
	int64 PayloadSize = 0;

	//---- The remaining members are used when the payload is not virtualized.
	
	/** The archive representing the file on disk containing the payload (if there is one), we keep the pointer so that the bulkdata object can be detached if needed. */
	FArchive* AttachedAr = nullptr;

	/** Offset of the payload in the file that contains it (INDEX_NONE if the payload does not come from a file)*/
	int64 OffsetInFile = INDEX_NONE;

	/** PackagePath containing the payload (this will be empty if the payload does not come from PackageResourceManager)*/
	FPackagePath PackagePath;

	/** PackageSegment to load with the packagepath (unused if the payload does not come from PackageResourceManager) */
	EPackageSegment PackageSegment;

	/** A 32bit bitfield of flags */
	EFlags Flags = EFlags::None;

	/** 
	 * Compression settings to be applied to the payload when the package is next saved. The settings will be reset if
	 * the payload is unloaded from memory during serialization (i.e. the payload was virtualized or the package was
	 * saved to disk.&
	 */
	Private::FCompressionSettings CompressionSettings;
};

ENUM_CLASS_FLAGS(FVirtualizedUntypedBulkData::EFlags);

//TODO: Probably remove this and change FVirtualizedUntypedBulkData to always be TVirtualizedBulkData<uint8>
/** Type safe versions */
template<typename DataType>
class TVirtualizedBulkData final : public FVirtualizedUntypedBulkData
{
public:
	TVirtualizedBulkData() = default;
	~TVirtualizedBulkData() = default;

	TVirtualizedBulkData<DataType> CopyTornOff() const
	{
		return TVirtualizedBulkData<DataType>(*this, ETornOff());
	}

protected:
	TVirtualizedBulkData(const TVirtualizedBulkData<DataType>& Other, ETornOff) : FVirtualizedUntypedBulkData(Other, ETornOff()) {}
};

using FByteVirtualizedBulkData	= TVirtualizedBulkData<uint8>;
using FWordVirtualizedBulkData	= TVirtualizedBulkData<uint16>;
using FIntVirtualizedBulkData	= TVirtualizedBulkData<int32>;
using FFloatVirtualizedBulkData	= TVirtualizedBulkData<float>;

/** 
  * Represents an entry to the table of contents found at the start of a payload sidecar file.
  * This might be moved to it's own header and the table of contents made into a proper class 
  * if we decide that we want to make access of the payload sidecar file a generic feature.
  */
struct COREUOBJECT_API FTocEntry
{
	FTocEntry() = default;
	FTocEntry(const FVirtualizedUntypedBulkData& BulkData)
		: Identifier(BulkData.PayloadContentId)
		, OffsetInFile(BulkData.OffsetInFile)
		, UncompressedSize(BulkData.PayloadSize)
	{

	}

	friend FArchive& operator<<(FArchive& Ar, FTocEntry& Entry);
	friend void operator<<(FStructuredArchive::FSlot Slot, FTocEntry& Entry);

	static constexpr uint32 PayloadSidecarFileVersion = 1;

	/** Identifier for the payload */
	FPayloadId Identifier;
	/** The offset into the file where we can find the payload, note that a virtualized payload will have an offset of INDEX_NONE */
	int64 OffsetInFile = INDEX_NONE;
	/** The size of the payload when uncompressed. */
	int64 UncompressedSize = INDEX_NONE;
};

/** 
 * A table of contents showing the location of all virtualized bulkdata payloads in a file. 
 * NOTE: This is currently only used by the sidecar functionality.
 */
class COREUOBJECT_API FPayloadToc
{
public:

	void AddEntry(const FVirtualizedUntypedBulkData& BulkData);
	bool FindEntry(const FPayloadId& Identifier, FTocEntry& OutEntry);

	const TArray<UE::Virtualization::FTocEntry>& GetContents() const;

	friend FArchive& operator<<(FArchive& Ar, FPayloadToc& TableOfContents);
	friend void operator<<(FStructuredArchive::FSlot Slot, FPayloadToc& TableOfContents);

private:
	enum class EVersion : uint32
	{
		INITIAL = 0,

		// -----<new versions can be added before this line>-------------------------------------------------
		// - this needs to be the last line (see note below)
		AUTOMATIC_VERSION_PLUS_ONE,
		AUTOMATIC_VERSION = AUTOMATIC_VERSION_PLUS_ONE - 1
	};

	TArray<UE::Virtualization::FTocEntry> Contents;
};

} // namespace UE::Virtualization

//#endif //WITH_EDITORONLY_DATA
