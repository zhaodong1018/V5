// Copyright Epic Games, Inc. All Rights Reserved.

#include "SDMXFixtureTypeFunctionsEditorFunctionRow.h"

#include "DMXAttribute.h"
#include "DMXEditorStyle.h"
#include "DMXFixtureTypeSharedData.h"
#include "DragDrop/DMXFixtureFunctionDragDropOp.h"
#include "DragDrop/DMXFixtureMatrixDragDropOp.h"
#include "Library/DMXEntityFixtureType.h"
#include "Widgets/SNameListPicker.h"
#include "Widgets/FixtureType/DMXFixtureTypeFunctionsEditorFunctionItem.h"
#include "Widgets/FixtureType/DMXFixtureTypeFunctionsEditorMatrixItem.h"
#include "Widgets/FixtureType/SDMXFixtureTypeFunctionsEditor.h"
#include "Widgets/FixtureType/SDMXFixtureTypeFunctionsEditorMatrixRow.h"

#include "ScopedTransaction.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"


#define LOCTEXT_NAMESPACE "SDMXFixtureTypeFunctionsEditorFunctionRow"

void SDMXFixtureTypeFunctionsEditorFunctionRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable, const TSharedRef<FDMXFixtureTypeFunctionsEditorFunctionItem>& InFunctionItem)
{
	FunctionItem = InFunctionItem;
	IsSelected = InArgs._IsSelected;

	SMultiColumnTableRow<TSharedPtr<FDMXFixtureTypeFunctionsEditorItemBase>>::Construct(
		FSuperRowType::FArguments()
		.OnDrop(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDrop)
		.OnDragEnter(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDragEnter)
		.OnDragLeave(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDragLeave),
		OwnerTable
	);
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::EnterFunctionNameEditingMode()
{
	FunctionNameEditableTextBlock->EnterEditingMode();
}

FReply SDMXFixtureTypeFunctionsEditorFunctionRow::OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const bool bLeftMouseButton = MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton);
	const TSharedPtr<ITypedTableView<TSharedPtr<FDMXFixtureTypeFunctionsEditorItemBase>>> OwnerTable = OwnerTablePtr.Pin();
	
	if (bLeftMouseButton && OwnerTable.IsValid())
	{
		if(OwnerTable->Private_GetNumSelectedItems() == 1)
		{
			TSharedRef<FDMXFixtureFunctionDragDropOp> DragDropOp = MakeShared<FDMXFixtureFunctionDragDropOp>(SharedThis(this));

			return FReply::Handled().BeginDragDrop(DragDropOp);
		}
	}

	return FReply::Unhandled();
}

TSharedRef<SWidget> SDMXFixtureTypeFunctionsEditorFunctionRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (ColumnName == FDMXFixtureTypeFunctionsEditorCollumnIDs::Status)
	{
		return
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image_Lambda([this]()
					{
						if (!FunctionItem->ErrorStatus.IsEmpty())
						{
							return FEditorStyle::GetBrush("Icons.Error");
						}

						if (!FunctionItem->WarningStatus.IsEmpty())
						{
							return FEditorStyle::GetBrush("Icons.Warning");
						}

						static const FSlateBrush EmptyBrush = FSlateNoResource();
						return &EmptyBrush;
					})
				.ToolTipText_Lambda([this]()
					{
						if (!FunctionItem->ErrorStatus.IsEmpty())
						{
							return FunctionItem->ErrorStatus;
						}
						else if (!FunctionItem->WarningStatus.IsEmpty())
						{
							return FunctionItem->WarningStatus;
						}

						return FText::GetEmpty();
					})
			];
	}
	else if (ColumnName == FDMXFixtureTypeFunctionsEditorCollumnIDs::Channel)
	{
		return
			SNew(SBorder)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.Padding(4.f)
			.BorderImage(FEditorStyle::GetBrush("NoBorder"))
			[
				SAssignNew(StartingChannelEditableTextBlock, SInlineEditableTextBlock)
				.Text_Lambda([this]()
				{
					const int32 StartingChannel = FunctionItem->GetStartingChannel();
					return FText::AsNumber(StartingChannel);
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.IsReadOnly(false)
				.OnVerifyTextChanged(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnVerifyStartingChannelChanged)
				.OnTextCommitted(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnStartingChannelCommitted)
				.IsSelected(IsSelected)
			];
	}
	else if (ColumnName == FDMXFixtureTypeFunctionsEditorCollumnIDs::Name)
	{
		return
			SNew(SBorder)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.Padding(4.f)
			.BorderImage(FEditorStyle::GetBrush("NoBorder"))
			[
				SAssignNew(FunctionNameEditableTextBlock, SInlineEditableTextBlock)
				.Text_Lambda([this]()
					{
						if (FunctionItem->HasValidAttribute())
						{
							return FunctionItem->GetFunctionName();
						}
						return LOCTEXT("InvalidAttributeFunctionName", "<Empty Channel - No Attribute Set>");
					})
				.IsEnabled_Lambda([this]()
					{
						return FunctionItem->HasValidAttribute();
					})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.IsReadOnly(false)
				.OnVerifyTextChanged(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnVerifyFunctionNameChanged)
				.OnTextCommitted(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::OnFunctionNameCommitted)
				.IsSelected(IsSelected)
			];
	}
	else if (ColumnName == FDMXFixtureTypeFunctionsEditorCollumnIDs::Attribute)
	{
		return
			SNew(SBorder)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.Padding(4.f)
			.BorderImage(FEditorStyle::GetBrush("NoBorder"))
			[
				SNew(SNameListPicker)
				.OptionsSource(MakeAttributeLambda(&FDMXAttributeName::GetPossibleValues))
				.UpdateOptionsDelegate(&FDMXAttributeName::OnValuesChanged)
				.IsValid_Lambda([this]()
					{	
						const FName CurrentValue = GetAttributeName();
						if (CurrentValue.IsEqual(FDMXNameListItem::None))
						{
							return true;
						}

						return FunctionItem->HasValidAttribute();
					})
				.Value(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::GetAttributeName)
				.bCanBeNone(FDMXAttributeName::bCanBeNone)
				.bDisplayWarningIcon(true)
				.OnValueChanged(this, &SDMXFixtureTypeFunctionsEditorFunctionRow::SetAttributeName)
			];
	}

	return SNullWidget::NullWidget;
}

FReply SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDrop(const FDragDropEvent& DragDropEvent)
{
	if (const TSharedPtr<FDMXFixtureFunctionDragDropOp> FixtureFunctionDragDropOp = DragDropEvent.GetOperationAs<FDMXFixtureFunctionDragDropOp>())
	{
		if (const TSharedPtr<SDMXFixtureTypeFunctionsEditorFunctionRow> DroppedRow = FixtureFunctionDragDropOp->Row.Pin())
		{
			if (const TSharedPtr<FDMXFixtureTypeFunctionsEditorFunctionItem> DroppedItem = DroppedRow->GetFunctionItem())
			{
				if (const TSharedPtr<FDMXFixtureTypeSharedData>& SharedData = DroppedItem->GetFixtureTypeSharedData())
				{
					if (UDMXEntityFixtureType* ParentFixtureType = DroppedItem->GetFixtureType().Get())
					{
						// Only allow drag drop within the same editor
						if (FunctionItem->GetDMXEditor() == DroppedItem->GetDMXEditor())
						{
							const int32 FunctionToReorderIndex = DroppedRow->GetFunctionItem()->GetFunctionIndex();
							const int32 InsertAtIndex = FunctionItem->GetFunctionIndex();

							const FScopedTransaction ReorderFunctionTransaction(LOCTEXT("ReorderFunctionTransaction", "Reorder Fixture Function"));
							ParentFixtureType->PreEditChange(FDMXFixtureFunction::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FDMXFixtureFunction, Channel)));

							ParentFixtureType->ReorderFunction(FunctionItem->GetModeIndex(), FunctionToReorderIndex, InsertAtIndex);

							ParentFixtureType->PostEditChange();

							// Select the Function, unselect the Matrix
							constexpr bool bMatrixSelected = false;
							SharedData->SetFunctionAndMatrixSelection(TArray<int32>({ InsertAtIndex }), bMatrixSelected);

							return FReply::Handled();
						}
					}
				}
			}
		}
	}
	else if (const TSharedPtr<FDMXFixtureMatrixDragDropOp> FixtureMatrixDragDropOp = DragDropEvent.GetOperationAs<FDMXFixtureMatrixDragDropOp>())
	{
		if (const TSharedPtr<SDMXFixtureTypeFunctionsEditorMatrixRow> DroppedRow = FixtureMatrixDragDropOp->Row.Pin())
		{
			if (const TSharedPtr<FDMXFixtureTypeFunctionsEditorMatrixItem> DroppedItem = DroppedRow->GetMatrixItem())
			{
				if (const TSharedPtr<FDMXFixtureTypeSharedData>& SharedData = FunctionItem->GetFixtureTypeSharedData())
				{
					if (UDMXEntityFixtureType* ParentFixtureType = DroppedItem->GetFixtureType().Get())
					{
						// Only allow drag drop within the same editor
						if (FunctionItem->GetDMXEditor() == DroppedItem->GetDMXEditor())
						{
							const FScopedTransaction ReorderMatrixTransaction(LOCTEXT("ReorderMatrixTransaction", "Reorder Fixture Matrix"));
							ParentFixtureType->PreEditChange(FDMXFixtureFunction::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FDMXFixtureMatrix, FirstCellChannel)));

							ParentFixtureType->ReorderMatrix(FunctionItem->GetModeIndex(), FunctionItem->GetFunctionIndex());

							ParentFixtureType->PostEditChange();

							// Clear the selected functions and select the matrix
							constexpr bool bMatrixSelected = true;
							SharedData->SetFunctionAndMatrixSelection(TArray<int32>(), bMatrixSelected);
						}
					}
				}
			}
		}
	}

	return FReply::Unhandled();
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDragEnter(const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FDMXFixtureFunctionDragDropOp> FixtureFunctionDragDropOp = DragDropEvent.GetOperationAs<FDMXFixtureFunctionDragDropOp>();
	const TSharedPtr<FDMXFixtureMatrixDragDropOp> FixtureMatrixDragDropOp = DragDropEvent.GetOperationAs<FDMXFixtureMatrixDragDropOp>();
	if (FixtureFunctionDragDropOp.IsValid() || FixtureMatrixDragDropOp.IsValid())
	{
		bIsDragDropTarget = true;
	}
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::OnRowDragLeave(const FDragDropEvent& DragDropEvent)
{
	bIsDragDropTarget = false;
}

bool SDMXFixtureTypeFunctionsEditorFunctionRow::OnVerifyStartingChannelChanged(const FText& InNewText, FText& OutErrorMessage)
{
	FString StringValue = InNewText.ToString();
	int32 Value;
	if (LexTryParseString<int32>(Value, *StringValue))
	{
		if (Value > 0 && Value <= DMX_MAX_ADDRESS)
		{
			return true;
		}
	}

	OutErrorMessage = LOCTEXT("InvalidStartingChannelError", "Channel must be set to a value between 1 and 512");

	return false;
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::OnStartingChannelCommitted(const FText& InNewText, ETextCommit::Type InTextCommit)
{
	FString StringValue = InNewText.ToString();
	int32 StartingChannel;
	if (LexTryParseString<int32>(StartingChannel, *StringValue))
	{
		StartingChannel = FMath::Clamp(StartingChannel, 1, DMX_MAX_ADDRESS);
		FunctionItem->SetStartingChannel(StartingChannel);
	}
	else
	{
		StartingChannel = FunctionItem->GetStartingChannel();
	}

	StartingChannelEditableTextBlock->SetText(FText::AsNumber(StartingChannel));
}

bool SDMXFixtureTypeFunctionsEditorFunctionRow::OnVerifyFunctionNameChanged(const FText& InNewText, FText& OutErrorMessage)
{
	FText InvalidReason;
	const bool bValidFunctionName = FunctionItem->IsValidFunctionName(InNewText, InvalidReason);

	if (bValidFunctionName)
	{
		return true;
	}
	else
	{
		OutErrorMessage = InvalidReason;
		return false;
	}
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::OnFunctionNameCommitted(const FText& InNewText, ETextCommit::Type InTextCommit)
{
	FText UniqueFunctionName;
	FunctionItem->SetFunctionName(InNewText, UniqueFunctionName);

	FunctionNameEditableTextBlock->SetText(UniqueFunctionName);
}

FName SDMXFixtureTypeFunctionsEditorFunctionRow::GetAttributeName() const
{
	return FunctionItem->GetAttributeName().GetName();
}

void SDMXFixtureTypeFunctionsEditorFunctionRow::SetAttributeName(FName NewValue)
{
	FDMXAttributeName NewAttributeName;
	NewAttributeName.SetFromName(NewValue);

	FunctionItem->SetAttributeName(NewAttributeName);
}

#undef LOCTEXT_NAMESPACE
