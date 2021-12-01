// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Engine/Engine.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "Subsystems/EngineSubsystem.h"
#include "UObject/Object.h"

#include "MetasoundAssetSubsystem.generated.h"

// Forward Declarations
class UAssetManager;
class FMetasoundAssetBase;

struct FDirectoryPath;


USTRUCT(BlueprintType)
struct METASOUNDENGINE_API FMetaSoundAssetDirectory
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Directories, meta = (RelativePath, LongPackageName))
	FDirectoryPath Directory;
};

/** The subsystem in charge of the MetaSound asset registry */
UCLASS()
class METASOUNDENGINE_API UMetaSoundAssetSubsystem : public UEngineSubsystem, public Metasound::Frontend::IMetaSoundAssetManager
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;

	void AddOrUpdateAsset(const FAssetData& InAssetData, bool bInRegisterWithFrontend = true);
	void RemoveAsset(UObject& InObject, bool bInUnregisterWithFrontend = true);
	void RemoveAsset(const FAssetData& InAssetData, bool bInUnregisterWithFrontend = true);
	void RenameAsset(const FAssetData& InAssetData, bool bInReregisterWithFrontend = true);

	virtual void AddAssetReferences(FMetasoundAssetBase& InAssetBase) override;
	virtual void AddOrUpdateAsset(UObject& InObject, bool bInRegisterWithFrontend = true) override;
	virtual bool CanAutoUpdate(const FMetasoundFrontendClassName& InClassName) const override;
	virtual bool ContainsKey(const Metasound::Frontend::FNodeRegistryKey& InRegistryKey) const override;
	virtual const FSoftObjectPath* FindObjectPathFromKey(const Metasound::Frontend::FNodeRegistryKey& RegistryKey) const override;
	virtual TSet<Metasound::Frontend::FNodeRegistryKey> GetReferencedKeys(const FMetasoundAssetBase& InAssetBase) const override;
	virtual void RescanAutoUpdateDenyList() override;
	virtual FMetasoundAssetBase* TryLoadAsset(const FSoftObjectPath& InObjectPath) const override;
	virtual FMetasoundAssetBase* TryLoadAssetFromKey(const Metasound::Frontend::FNodeRegistryKey& RegistryKey) const override;
	virtual bool TryLoadReferencedAssets(const FMetasoundAssetBase& InAssetBase, TArray<FMetasoundAssetBase*>& OutReferencedAssets) const override;

	UFUNCTION(BlueprintCallable, Category = "MetaSounds|Registration")
	void RegisterAssetClassesInDirectories(const TArray<FMetaSoundAssetDirectory>& Directories);

	UFUNCTION(BlueprintCallable, Category = "MetaSounds|Registration")
	void UnregisterAssetClassesInDirectories(const TArray<FMetaSoundAssetDirectory>& Directories);

protected:
	void PostEngineInit();
	void PostInitAssetScan();
	void RebuildDenyListCache(const UAssetManager& InAssetManager);
	void SearchAndIterateDirectoryAssets(const TArray<FDirectoryPath>& InDirectories, TFunctionRef<void(const FAssetData&)> InFunction);
	void SynchronizeAssetClassDisplayName(const FAssetData& InAssetData);

private:
	int32 AutoUpdateDenyListChangeID = INDEX_NONE;
	TSet<FName> AutoUpdateDenyListCache;
	TMap<Metasound::Frontend::FNodeRegistryKey, FSoftObjectPath> PathMap;
};
