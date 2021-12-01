// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PropertyEditorDelegates.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"


struct FAssetData;
class SMenuAnchor;
class SMultiLineEditableTextBox;
class SSequencerPlaylistItemWidget;
class SSearchBox;
class STextBlock;
template <typename ItemType> class TTextFilter;
class USequencerPlaylist;
class USequencerPlaylistItem;
class USequencerPlaylistPlayer;


struct FSequencerPlaylistRowData
{
	int32 PlaylistIndex;
	TWeakObjectPtr<USequencerPlaylistItem> WeakItem;

	FSequencerPlaylistRowData(int32 InPlaylistIndex, USequencerPlaylistItem* InItem)
		: PlaylistIndex(InPlaylistIndex)
		, WeakItem(InItem)
	{
	}
};


class SSequencerPlaylistPanel : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SSequencerPlaylistPanel) {}
	SLATE_END_ARGS()

public:
	static const float DefaultWidth;
	static const FName ColumnName_HoverTransport;
	static const FName ColumnName_Items;
	static const FName ColumnName_Offset;
	static const FName ColumnName_Hold;
	static const FName ColumnName_Loop;
	static const FName ColumnName_HoverDetails;

	void Construct(const FArguments& InArgs, USequencerPlaylistPlayer* InPlayer);

	bool InTriggerMode() const { return bTriggerMode; }

private:
	TSharedRef<SWidget> Construct_LeftToolbar();
	TSharedRef<SWidget> Construct_RightToolbar();
	TSharedRef<SWidget> Construct_Transport();
	TSharedRef<SWidget> Construct_AddSearchRow();
	TSharedRef<SWidget> Construct_ItemListView();

	USequencerPlaylist* GetCheckedPlaylist();
	void RegenerateRows();

	TSharedRef<SWidget> BuildOpenPlaylistMenu();
	void OnSavePlaylistAs();
	void OnLoadPlaylist(const FAssetData& InPreset);
	void OnNewPlaylist();

	void GetSearchStrings(const FSequencerPlaylistRowData& Item, TArray<FString>& OutSearchStrings);
	void OnSearchTextChanged(const FText& InFilterText);

	FReply HandleClicked_PlayAll();
	FReply HandleClicked_StopAll();
	FReply HandleClicked_ResetAll();
	FReply HandleClicked_AddSequence();

	FReply HandleClicked_Item_Play(TSharedPtr<SSequencerPlaylistItemWidget> ItemWidget);
	FReply HandleClicked_Item_Stop(TSharedPtr<SSequencerPlaylistItemWidget> ItemWidget);
	FReply HandleClicked_Item_Reset(TSharedPtr<SSequencerPlaylistItemWidget> ItemWidget);
	FReply HandleClicked_Item_Remove(TSharedPtr<SSequencerPlaylistItemWidget> ItemWidget);

	bool HandleItemDetailsIsPropertyVisible(const FPropertyAndParent& PropertyAndParent);

	// Drag and drop handling
	TOptional<EItemDropZone> HandleCanAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone DropZone, TSharedPtr<FSequencerPlaylistRowData> RowData);
	FReply HandleAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone DropZone, TSharedPtr<FSequencerPlaylistRowData> RowData);

private:
	TWeakObjectPtr<USequencerPlaylistPlayer> WeakPlayer;

	bool bTriggerMode = false;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<TTextFilter<const FSequencerPlaylistRowData&>> SearchTextFilter;
	TArray<TSharedPtr<FSequencerPlaylistRowData>> ItemRows;
	TSharedPtr<SListView<TSharedPtr<FSequencerPlaylistRowData>>> ItemListView;
};


class FSequencerPlaylistItemDragDropOp : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FSequencerPlaylistItemDragDropOp, FDragDropOperation)

	TArray<TSharedPtr<FSequencerPlaylistRowData>> SelectedItems;

	static TSharedRef<FSequencerPlaylistItemDragDropOp> New(const TArray<TSharedPtr<FSequencerPlaylistRowData>>& InSelectedItems);

public:
	virtual ~FSequencerPlaylistItemDragDropOp();

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
	{
		return Decorator;
	}

private:
	TSharedPtr<SWidget> Decorator;
};


DECLARE_DELEGATE_RetVal_OneParam(FReply, FOnClickedSequencerPlaylistItem, TSharedPtr<SSequencerPlaylistItemWidget> /*ItemWidget*/);


class SSequencerPlaylistItemWidget : public SMultiColumnTableRow<TSharedPtr<FSequencerPlaylistRowData>>
{
	static const FText PlayItemTooltipText;
	static const FText StopItemTooltipText;
	static const FText ResetItemTooltipText;

	SLATE_BEGIN_ARGS(SSequencerPlaylistItemWidget) {}
		SLATE_ATTRIBUTE(bool, TriggerMode)

		SLATE_EVENT(FOnClickedSequencerPlaylistItem, OnPlayClicked)
		SLATE_EVENT(FOnClickedSequencerPlaylistItem, OnStopClicked)
		SLATE_EVENT(FOnClickedSequencerPlaylistItem, OnResetClicked)
		SLATE_EVENT(FOnClickedSequencerPlaylistItem, OnRemoveClicked)

		SLATE_EVENT(FIsPropertyVisible, OnIsPropertyVisible)
		SLATE_EVENT(FOnCanAcceptDrop, OnCanAcceptDrop)
		SLATE_EVENT(FOnAcceptDrop, OnAcceptDrop)
	SLATE_END_ARGS()

public:
	void Construct(const FArguments& InArgs, TSharedPtr<FSequencerPlaylistRowData> InRowData, const TSharedRef<STableViewBase>& OwnerTableView);

	const TSharedPtr<FSequencerPlaylistRowData>& GetRowData() { return RowData; }
	USequencerPlaylistItem* GetItem() { return GetRowData() ? GetRowData()->WeakItem.Get() : nullptr; }

	//~ Begin STableRow
	void ConstructChildren(ETableViewMode::Type InOwnerTableMode, const TAttribute<FMargin>& InPadding, const TSharedRef<SWidget>& InContent) override;
	//~ End STableRow

	//~ Begin SMultiColumnTableRow
	TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnName) override;
	//~ End SMultiColumnTableRow

	//~ Begin SWidget
	FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	//~ End SWidget

private:
	FReply HandleDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	bool InTriggerMode() const { return TriggerMode.Get(); }
	bool IsRowContentEnabled() const;
	EVisibility GetTriggerModeTransportVisibility() const;
	EVisibility GetHoverTransportCellVisibility() const;

	TSharedRef<SWidget> EnsureSelectedAndBuildContextMenu();
	TSharedRef<SWidget> BuildContextMenu(const TArray<UObject*>& SelectedItems);

private:
	TSharedPtr<FSequencerPlaylistRowData> RowData;
	TSharedPtr<SMenuAnchor> DetailsAnchor;

	TAttribute<bool> TriggerMode;

	FOnClickedSequencerPlaylistItem PlayClickedDelegate;
	FOnClickedSequencerPlaylistItem StopClickedDelegate;
	FOnClickedSequencerPlaylistItem ResetClickedDelegate;
	FOnClickedSequencerPlaylistItem RemoveClickedDelegate;
	FIsPropertyVisible IsPropertyVisibleDelegate;
};
