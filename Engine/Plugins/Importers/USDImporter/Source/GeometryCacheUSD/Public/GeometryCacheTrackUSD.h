// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GeometryCacheTrack.h"
#include "GeometryCacheMeshData.h"

#include "UsdWrappers/UsdStage.h"

#include "GeometryCacheTrackUSD.generated.h"

typedef TFunction< void( const TWeakObjectPtr<UGeometryCacheTrackUsd>, float Time, FGeometryCacheMeshData& ) > FReadUsdMeshFunction;

/** GeometryCacheTrack for querying USD */
UCLASS(collapsecategories, hidecategories = Object, BlueprintType, config = Engine)
class GEOMETRYCACHEUSD_API UGeometryCacheTrackUsd : public UGeometryCacheTrack
{
	GENERATED_BODY()

	UGeometryCacheTrackUsd();

public:
	void Initialize(
		const UE::FUsdStage& InStage,
		const FString& InPrimPath,
		const FName& InRenderContext,
		const TMap< FString, TMap< FString, int32 > >& InMaterialToPrimvarToUVIndex,
		int32 InStartFrameIndex,
		int32 InEndFrameIndex,
		FReadUsdMeshFunction InReadFunc
	);

	//~ Begin UObject Interface.
	virtual void BeginDestroy() override;
	//~ End UObject Interface.

	//~ Begin UGeometryCacheTrack Interface.
	virtual const bool UpdateMeshData(const float Time, const bool bLooping, int32& InOutMeshSampleIndex, FGeometryCacheMeshData*& OutMeshData) override;
	virtual const bool UpdateBoundsData(const float Time, const bool bLooping, const bool bIsPlayingBackward, int32& InOutBoundsSampleIndex, FBox& OutBounds) override;
	virtual const FGeometryCacheTrackSampleInfo& GetSampleInfo(float Time, const bool bLooping) override;
	//~ End UGeometryCacheTrack Interface.

	const int32 FindSampleIndexFromTime(const float Time, const bool bLooping) const;

	int32 GetStartFrameIndex() const { return StartFrameIndex; }
	int32 GetEndFrameIndex() const { return EndFrameIndex;  }

	bool GetMeshData(int32 SampleIndex, FGeometryCacheMeshData& OutMeshData);

public:
	FName RenderContext;
	int32 StartFrameIndex;
	int32 EndFrameIndex;
	TMap< FString, TMap< FString, int32 > > MaterialToPrimvarToUVIndex;

	FString PrimPath;

	UE::FUsdStage CurrentStage;
	FString StageRootLayerPath;

private:
	FGeometryCacheMeshData MeshData;
	TArray<FGeometryCacheTrackSampleInfo> SampleInfos;
};
