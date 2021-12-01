// Copyright Epic Games, Inc. All Rights Reserved.
#include "WorldPartition/SWorldPartitionEditorGridSpatialHash.h"
#include "GameFramework/WorldSettings.h"
#include "DerivedDataCache/Public/DerivedDataCacheInterface.h"
#include "Misc/SecureHash.h"
#include "WorldPartition/WorldPartitionEditorCell.h"
#include "WorldPartition/WorldPartitionRuntimeSpatialHash.h"
#include "WorldPartition/WorldPartitionEditorSpatialHash.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionMiniMap.h"
#include "WorldPartition/WorldPartitionMiniMapHelper.h"
#include "Framework/Commands/Commands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Layout/WidgetPath.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateTextures.h"
#include "Brushes/SlateColorBrush.h"
#include "ObjectTools.h"
#include "UObject/ObjectResource.h"
#include "EditorStyleSet.h"
#include "Selection.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DDynamic.h"
#include "Misc/HashBuilder.h"
#include "EngineModule.h"
#include "Renderer/Private/RendererModule.h"

#define LOCTEXT_NAMESPACE "WorldPartitionEditor"

TAutoConsoleVariable<bool> CVarDebugDrawOctree(TEXT("wp.Editor.DebugDrawOctree"), false, TEXT("Whether to debug draw the World Partition octree"), ECVF_Default);

SWorldPartitionEditorGridSpatialHash::SWorldPartitionEditorGridSpatialHash()
	: WorldMiniMapBounds(ForceInit)
	, WorldMinimapUsesVirtualTexture(false)
{
}

SWorldPartitionEditorGridSpatialHash::~SWorldPartitionEditorGridSpatialHash()
{
}

void SWorldPartitionEditorGridSpatialHash::Construct(const FArguments& InArgs)
{
	World = InArgs._InWorld;
	WorldPartition = World ? World->GetWorldPartition() : nullptr;

	if (WorldPartition)
	{
		UWorldPartitionEditorSpatialHash* EditorSpatialHash = (UWorldPartitionEditorSpatialHash*)WorldPartition->EditorHash;

		//Update MiniMap data for drawing  
		UpdateWorldMiniMapDetails();

		bShowActors = !WorldMiniMapBrush.HasUObject();
	}

	SWorldPartitionEditorGrid2D::Construct(SWorldPartitionEditorGrid::FArguments().InWorld(InArgs._InWorld));
}

void SWorldPartitionEditorGridSpatialHash::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SWorldPartitionEditorGrid2D::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	UTexture2D* Texture2D = Cast<UTexture2D>(WorldMiniMapBrush.GetResourceObject());

	if ((Texture2D != nullptr) && (Texture2D->IsCurrentlyVirtualTextured() != WorldMinimapUsesVirtualTexture))
	{
		UpdateWorldMiniMapDetails();
	}
}

FReply SWorldPartitionEditorGridSpatialHash::ReloadMiniMap()
{
	UE_LOG(LogTemp, Log, TEXT("Reload MiniMap has been clicked"));

	//Create a new MiniMap if there isn't one.
	AWorldPartitionMiniMap* WorldMiniMap = FWorldPartitionMiniMapHelper::GetWorldPartitionMiniMap(World, true);
	if (!WorldMiniMap)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create Minimap. WorldPartitionMiniMap actor not found in the persistent level."));
		return FReply::Handled();
	}

	WorldMiniMap->Modify();


	// Updating VT is not supported for now
	if (WorldMiniMap->MiniMapTexture != nullptr)
	{
		WorldMiniMap->MiniMapTexture->VirtualTextureStreaming = false;
	}

	WorldMiniMap->UVOffset.bIsValid = false;

	FWorldPartitionMiniMapHelper::CaptureWorldMiniMapToTexture(World, WorldMiniMap, WorldMiniMap->MiniMapSize, static_cast<UTexture2D*&>(WorldMiniMap->MiniMapTexture), TEXT("MinimapTexture"), WorldMiniMap->MiniMapWorldBounds);

	UpdateWorldMiniMapDetails();

	return FReply::Handled();
}

void SWorldPartitionEditorGridSpatialHash::UpdateWorldMiniMapDetails()
{
	auto WorldMiniMap = FWorldPartitionMiniMapHelper::GetWorldPartitionMiniMap(World);
	if (WorldMiniMap)
	{
		WorldMiniMapBounds = FBox2D(FVector2D(WorldMiniMap->MiniMapWorldBounds.Min), FVector2D(WorldMiniMap->MiniMapWorldBounds.Max));
		if (UTexture2D* MiniMapTexture = WorldMiniMap->MiniMapTexture)
		{
			WorldMinimapUsesVirtualTexture = MiniMapTexture->IsCurrentlyVirtualTextured();

			if (WorldMinimapUsesVirtualTexture)
			{
				WorldMiniMapBrush.SetUVRegion(WorldMiniMap->UVOffset);
			}
			else
			{
				WorldMiniMapBrush.SetUVRegion(FBox2D({ 0.f, 0.f }, { 1.f, 1.f }));
			}

			WorldMiniMapBrush.SetImageSize(FVector2D(MiniMapTexture->GetSizeX(), MiniMapTexture->GetSizeY()));
			WorldMiniMapBrush.SetResourceObject(MiniMapTexture);
		}
	}
}

int32 SWorldPartitionEditorGridSpatialHash::PaintGrid(const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId) const
{
	const FBox2D ViewRect(FVector2D(ForceInitToZero), AllottedGeometry.GetLocalSize());
	const FBox2D ViewRectWorld(ScreenToWorld.TransformPoint(ViewRect.Min), ScreenToWorld.TransformPoint(ViewRect.Max));

	UWorldPartitionEditorSpatialHash* EditorSpatialHash = (UWorldPartitionEditorSpatialHash*)WorldPartition->EditorHash;
	
	FBox VisibleGridRectWorld(
		FVector(
			FMath::Max(FMath::FloorToFloat(EditorSpatialHash->Bounds.Min.X / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize, ViewRectWorld.Min.X),
			FMath::Max(FMath::FloorToFloat(EditorSpatialHash->Bounds.Min.Y / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize, ViewRectWorld.Min.Y),
			FMath::FloorToFloat(EditorSpatialHash->Bounds.Min.Z / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize
		),
		FVector(
			FMath::Min(FMath::FloorToFloat(EditorSpatialHash->Bounds.Max.X / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize, ViewRectWorld.Max.X),
			FMath::Min(FMath::FloorToFloat(EditorSpatialHash->Bounds.Max.Y / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize, ViewRectWorld.Max.Y),
			FMath::FloorToFloat(EditorSpatialHash->Bounds.Max.Z / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize
		)
	);

	// Shadow whole grid area
	{
		FSlateColorBrush ShadowBrush(FLinearColor::Black);
		FLinearColor ShadowColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f));

		FPaintGeometry GridGeometry = AllottedGeometry.ToPaintGeometry(
			WorldToScreen.TransformPoint(FVector2D(VisibleGridRectWorld.Min)),
			WorldToScreen.TransformPoint(FVector2D(VisibleGridRectWorld.Max)) - WorldToScreen.TransformPoint(FVector2D(VisibleGridRectWorld.Min))
		);

		FSlateDrawElement::MakeBox(
			OutDrawElements,
			++LayerId,
			GridGeometry,
			&ShadowBrush,
			ESlateDrawEffect::None,
			ShadowColor
		);
	}

	// Draw MiniMap image if any
	if (UTexture2D* Texture2D = Cast<UTexture2D>(WorldMiniMapBrush.GetResourceObject()))
	{	
		FPaintGeometry WorldImageGeometry = AllottedGeometry.ToPaintGeometry(
			WorldToScreen.TransformPoint(WorldMiniMapBounds.Min),
			WorldToScreen.TransformPoint(WorldMiniMapBounds.Max) - WorldToScreen.TransformPoint(WorldMiniMapBounds.Min)
		);

		FSlateDrawElement::MakeRotatedBox(
			OutDrawElements,
			++LayerId,
			WorldImageGeometry,
			&WorldMiniMapBrush
		);

		if (Texture2D->IsCurrentlyVirtualTextured())
		{
			FVirtualTexture2DResource* VTResource = static_cast<FVirtualTexture2DResource*>(Texture2D->GetResource());
			const FVector2D ViewportSize = AllottedGeometry.GetLocalSize();
			const FVector2D ScreenSpaceSize = WorldImageGeometry.GetLocalSize();
			const FVector2D ViewportPositon = -WorldImageGeometry.GetAccumulatedRenderTransform().GetTranslation() + AllottedGeometry.GetAbsolutePosition();

			const FVector2D UV0 = WorldMiniMapBrush.GetUVRegion().Min;
			const FVector2D UV1 = WorldMiniMapBrush.GetUVRegion().Max;

			const ERHIFeatureLevel::Type InFeatureLevel = GMaxRHIFeatureLevel;
			const int32 MipLevel = -1;

			ENQUEUE_RENDER_COMMAND(MakeTilesResident)(
				[InFeatureLevel, VTResource, ScreenSpaceSize, ViewportPositon, ViewportSize, UV0, UV1, MipLevel](FRHICommandListImmediate& RHICmdList)
			{
				// AcquireAllocatedVT() must happen on render thread
				IAllocatedVirtualTexture* AllocatedVT = VTResource->AcquireAllocatedVT();

				IRendererModule& RenderModule = GetRendererModule();
				RenderModule.RequestVirtualTextureTilesForRegion(AllocatedVT, ScreenSpaceSize, ViewportPositon, ViewportSize, UV0, UV1, MipLevel);
				RenderModule.LoadPendingVirtualTextureTiles(RHICmdList, InFeatureLevel);
			});
		}
	}

	struct FCellDesc2D
	{
		FCellDesc2D()
			: Bounds(ForceInitToZero)
		{}

		FBox2D Bounds;
	};

	// Draw shadowed regions
	{
		TMap<uint32, FCellDesc2D> UniqueCells2D;
		EditorSpatialHash->ForEachIntersectingUnloadedRegion(VisibleGridRectWorld, [&](const UWorldPartitionEditorSpatialHash::FCellCoord& CellCoord)
		{
			FBox CellBounds = EditorSpatialHash->GetCellBounds(CellCoord);
			CellBounds = CellBounds.Overlap(EditorSpatialHash->Bounds);

			FHashBuilder HashBuilder;
			HashBuilder << CellCoord.X << CellCoord.Y << CellCoord.Level;
			uint32 CellHash2D = HashBuilder.GetHash();

			FCellDesc2D& CellDesc2D = UniqueCells2D.Add(CellHash2D);
			CellDesc2D.Bounds = FBox2D(FVector2D(CellBounds.Min), FVector2D(CellBounds.Max));
		});

		const bool bDebugDrawOctree = CVarDebugDrawOctree.GetValueOnAnyThread();

		for(auto& UniqueCell: UniqueCells2D)
		{
			const FCellDesc2D& Cell = UniqueCell.Value;

			FPaintGeometry CellGeometry = AllottedGeometry.ToPaintGeometry(
				WorldToScreen.TransformPoint(Cell.Bounds.Min),
				WorldToScreen.TransformPoint(Cell.Bounds.Max) - WorldToScreen.TransformPoint(Cell.Bounds.Min)
			);

			FSlateColorBrush CellBrush(FLinearColor::White);
			FLinearColor CellColor(0, 0, 0, 0.5f);

			if (bDebugDrawOctree)
			{
				CellColor = FLinearColor::MakeFromHSV8((uint8)UniqueCell.Key, 255, 255);
			}
			
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				++LayerId,
				CellGeometry,
				&CellBrush,
				ESlateDrawEffect::None,
				CellColor
			);
		}
	}

	// Draw selected cells
	if (SelectBox.IsValid)
	{
		const FBox VisibleSelectBox = SelectBox.Overlap(VisibleGridRectWorld);

		if (VisibleSelectBox.IsValid)
		{
			TMap<uint32, FCellDesc2D> UniqueCells2D;

			WorldPartition->EditorHash->ForEachIntersectingCell(VisibleSelectBox, [&](UWorldPartitionEditorCell* Cell)
			{
				UWorldPartitionEditorSpatialHash::FCellCoord CellCoord = EditorSpatialHash->GetCellCoords(Cell->Bounds.GetCenter(), 0);

				FHashBuilder HashBuilder;
				HashBuilder << CellCoord.X << CellCoord.Y;
				uint32 CellHash2D = HashBuilder.GetHash();

				FCellDesc2D& CellDesc2D = UniqueCells2D.Add(CellHash2D);

				CellDesc2D.Bounds = FBox2D(FVector2D(Cell->Bounds.Min), FVector2D(Cell->Bounds.Max));
			});

			for (auto& UniqueCell : UniqueCells2D)
			{
				const FCellDesc2D& Cell = UniqueCell.Value;

				FPaintGeometry CellGeometry = AllottedGeometry.ToPaintGeometry(
					WorldToScreen.TransformPoint(Cell.Bounds.Min),
					WorldToScreen.TransformPoint(Cell.Bounds.Max) - WorldToScreen.TransformPoint(Cell.Bounds.Min)
				);

				FSlateColorBrush CellBrush(FLinearColor::White);
				FLinearColor CellColor(1, 1, 1, 0.25f);

				FSlateDrawElement::MakeBox(
					OutDrawElements,
					++LayerId,
					CellGeometry,
					&CellBrush,
					ESlateDrawEffect::None,
					CellColor
				);
			}
		}
	}

	if (FBox2D(FVector2D(VisibleGridRectWorld.Min), FVector2D(VisibleGridRectWorld.Max)).GetArea() > 0.0f)
	{
		const FLinearColor Color = FLinearColor(0.1f, 0.1f, 0.1f, 1.f);

		FIntVector2 TopLeftW(
			FMath::FloorToFloat(VisibleGridRectWorld.Min.X / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize,
			FMath::FloorToFloat(VisibleGridRectWorld.Min.Y / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize
		);

		FIntVector2 BottomRightW(
			FMath::CeilToFloat(VisibleGridRectWorld.Max.X / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize,
			FMath::CeilToFloat(VisibleGridRectWorld.Max.Y / EditorSpatialHash->CellSize) * EditorSpatialHash->CellSize
		);

		TArray<FVector2D> LinePoints;
		LinePoints.SetNum(2);

		// Horizontal
		for (int32 i=TopLeftW.Y; i<=BottomRightW.Y; i+=EditorSpatialHash->CellSize)
		{
			FVector2D LineStartH(TopLeftW.X, i);
			FVector2D LineEndH(BottomRightW.X, i);

			LinePoints[0] = WorldToScreen.TransformPoint(LineStartH);
			LinePoints[1] = WorldToScreen.TransformPoint(LineEndH);

			FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), LinePoints, ESlateDrawEffect::NoBlending, Color, false, 1.0f);
		}

		// Vertical
		for (int32 i=TopLeftW.X; i<=BottomRightW.X; i+=EditorSpatialHash->CellSize)
		{
			FVector2D LineStartH(i, TopLeftW.Y);
			FVector2D LineEndH(i, BottomRightW.Y);

			LinePoints[0] = WorldToScreen.TransformPoint(LineStartH);
			LinePoints[1] = WorldToScreen.TransformPoint(LineEndH);

			FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), LinePoints, ESlateDrawEffect::NoBlending, Color, false, 1.0f);
		}

		++LayerId;
	}

	return SWorldPartitionEditorGrid2D::PaintGrid(AllottedGeometry, MyCullingRect, OutDrawElements, LayerId);
}

#undef LOCTEXT_NAMESPACE