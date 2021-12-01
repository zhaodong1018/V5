// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ConsoleVariablesEditorListRow.h"
#include "SConsoleVariablesEditorList.h"

#include "CoreMinimal.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SCompoundWidget.h"

class SConsoleVariablesEditorListValueInput;
class SConsoleVariablesEditorListRowHoverWidgets;

class SConsoleVariablesEditorListRow : public SMultiColumnTableRow<FConsoleVariablesEditorListRowPtr>
{
public:
	
	SLATE_BEGIN_ARGS(SConsoleVariablesEditorListRow)
	{}

	SLATE_END_ARGS()
	
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable, const TWeakPtr<FConsoleVariablesEditorListRow> InRow);

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnName) override;
	
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;

	virtual ~SConsoleVariablesEditorListRow() override;
	
	void FlashRow();
	
	EVisibility GetFlashImageVisibility() const;
	FSlateColor GetFlashImageColorAndOpacity() const;

	static const FSlateBrush* GetBorderImage(const FConsoleVariablesEditorListRow::EConsoleVariablesEditorListRowType InRowType);

	TSharedRef<SWidget> GenerateCells(const FName& InColumnName, const TSharedPtr<FConsoleVariablesEditorListRow> PinnedItem);

	ECheckBoxState GetCheckboxState() const;
	void OnCheckboxStateChange(const ECheckBoxState InNewState) const;

	TSharedRef<SWidget> GenerateValueCellWidget(const TSharedPtr<FConsoleVariablesEditorListRow> PinnedItem);

private:
	
	TWeakPtr<FConsoleVariablesEditorListRow> Item;

	TArray<TSharedPtr<SImage>> FlashImages;

	TSharedPtr<SConsoleVariablesEditorListValueInput> ValueChildInputWidget;

	#define LOCTEXT_NAMESPACE "ConsoleVariablesEditor"

	FText ValueWidgetToolTipFormatText = LOCTEXT("ValueWidgetToolTipFormatText", "Custom Value: {0}\nPreset Value: {1}\nStartup Value: {2} (Set By {3})");
	FText RevertButtonFormatText = LOCTEXT("RevertButtonFormatText", "Reset to Preset Value: {0}");

	#undef LOCTEXT_NAMESPACE
	
	TSharedPtr<SConsoleVariablesEditorListRowHoverWidgets> HoverableWidgetsPtr;

	FCurveSequence FlashAnimation;

	const float FlashAnimationDuration = 0.75f;
	const FLinearColor FlashColor = FLinearColor::White;
};

class SConsoleVariablesEditorListRowHoverWidgets : public SCompoundWidget
{
public:
	
	SLATE_BEGIN_ARGS(SConsoleVariablesEditorListRowHoverWidgets)
	{}

	SLATE_END_ARGS()
	
	void Construct(const FArguments& InArgs, const TWeakPtr<FConsoleVariablesEditorListRow> InRow);

	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;

	virtual ~SConsoleVariablesEditorListRowHoverWidgets() override;

private:
	
	TWeakPtr<FConsoleVariablesEditorListRow> Item;
	
	TSharedPtr<SButton> RemoveButtonPtr;
};
