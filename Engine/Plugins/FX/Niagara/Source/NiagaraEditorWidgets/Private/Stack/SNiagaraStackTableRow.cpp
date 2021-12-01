// Copyright Epic Games, Inc. All Rights Reserved.

#include "Stack/SNiagaraStackTableRow.h"
#include "NiagaraEditorWidgetsStyle.h"
#include "EditorStyleSet.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "ViewModels/Stack/NiagaraStackItem.h"
#include "ViewModels/Stack/NiagaraStackItemGroup.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "NiagaraEditorWidgetsUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraStackCommandContext.h"
#include "Stack/SNiagaraStackIssueIcon.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Framework/Application/SlateApplication.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "Styling/StyleColors.h"
#include "NiagaraEmitterEditorData.h"

#define LOCTEXT_NAMESPACE "NiagaraStackTableRow"

const float IndentSize = 12;

void SNiagaraStackTableRow::Construct(const FArguments& InArgs, UNiagaraStackViewModel* InStackViewModel, UNiagaraStackEntry* InStackEntry, TSharedRef<FNiagaraStackCommandContext> InStackCommandContext, const TSharedRef<STreeView<UNiagaraStackEntry*>>& InOwnerTree)
{
	ContentPadding = InArgs._ContentPadding;
	bIsCategoryIconHighlighted = InArgs._IsCategoryIconHighlighted;
	bShowExecutionCategoryIcon = InArgs._ShowExecutionCategoryIcon;
	NameColumnWidth = InArgs._NameColumnWidth;
	ValueColumnWidth = InArgs._ValueColumnWidth;
	NameColumnWidthChanged = InArgs._OnNameColumnWidthChanged;
	ValueColumnWidthChanged = InArgs._OnValueColumnWidthChanged;
	IssueIconVisibility = InArgs._IssueIconVisibility;
	RowPadding = InArgs._RowPadding;
	StackViewModel = InStackViewModel;
	StackEntry = InStackEntry;
	StackCommandContext = InStackCommandContext;
	OwnerTree = InOwnerTree;

	ExpandedImage = FCoreStyle::Get().GetBrush("TreeArrow_Expanded");
	CollapsedImage = FCoreStyle::Get().GetBrush("TreeArrow_Collapsed");

	ItemBackgroundColor = InArgs._ItemBackgroundColor;
	DisabledItemBackgroundColor = FStyleColors::Recessed;
	ForegroundColor = InArgs._ItemForegroundColor;
	IndicatorColor = InArgs._IndicatorColor;

	ExecutionCategoryToolTipText = (InStackEntry->GetExecutionSubcategoryName() != NAME_None)
		? FText::Format(LOCTEXT("ExecutionCategoryToolTipFormat", "{0} - {1}"), FText::FromName(InStackEntry->GetExecutionCategoryName()), FText::FromName(InStackEntry->GetExecutionSubcategoryName()))
		: FText::FromName(InStackEntry->GetExecutionCategoryName());

	ConstructInternal(
		STableRow<UNiagaraStackEntry*>::FArguments()
			.Style(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.Stack.TableViewRow")
			.OnDragDetected(InArgs._OnDragDetected)
			.OnDragLeave(InArgs._OnDragLeave)
			.OnCanAcceptDrop(InArgs._OnCanAcceptDrop)
			.OnAcceptDrop(InArgs._OnAcceptDrop)
		, OwnerTree.ToSharedRef());
}

void SNiagaraStackTableRow::SetOverrideNameWidth(TOptional<float> InMinWidth, TOptional<float> InMaxWidth)
{
	NameMinWidth = InMinWidth;
	NameMaxWidth = InMaxWidth;
}

void SNiagaraStackTableRow::SetOverrideNameAlignment(EHorizontalAlignment InHAlign, EVerticalAlignment InVAlign)
{
	NameHorizontalAlignment = InHAlign;
	NameVerticalAlignment = InVAlign;
}

void SNiagaraStackTableRow::SetOverrideValueWidth(TOptional<float> InMinWidth, TOptional<float> InMaxWidth)
{
	ValueMinWidth = InMinWidth;
	ValueMaxWidth = InMaxWidth;
}

void SNiagaraStackTableRow::SetOverrideValueAlignment(EHorizontalAlignment InHAlign, EVerticalAlignment InVAlign)
{
	ValueHorizontalAlignment = InHAlign;
	ValueVerticalAlignment = InVAlign;
}

FMargin SNiagaraStackTableRow::GetContentPadding() const
{
	return ContentPadding;
}

void SNiagaraStackTableRow::SetContentPadding(FMargin InContentPadding)
{
	ContentPadding = InContentPadding;
}

// searches for the first parent stack entry without a front divider
UNiagaraStackEntry* GetParentEntryNoDivider(UNiagaraStackEntry* Entry)
{
	UNiagaraStackEntry* Outer = Cast<UNiagaraStackEntry>(Entry->GetOuter());
	if (Outer == nullptr || !Outer->HasFrontDivider())
	{
		return Outer;
	}
	return GetParentEntryNoDivider(Outer);
}

UNiagaraStackEntry* GetEntryAbove(UNiagaraStackEntry* Entry)
{
	UNiagaraStackEntry* Outer = Cast<UNiagaraStackEntry>(Entry->GetOuter());
	if (Outer)
	{
		TArray<UNiagaraStackEntry*> FilteredChildren;
		Outer->GetFilteredChildren(FilteredChildren);
		for (int32 Index = 1; Index < FilteredChildren.Num(); Index++)
		{
			if (FilteredChildren[Index] == Entry)
			{
				return FilteredChildren[Index - 1];
			}
		}
	}
	return nullptr;
}

bool HasVisibleChildren(UNiagaraStackEntry* Entry)
{
	if (!Entry)
	{
		return false;
	}
	TArray<UNiagaraStackEntry*> Children;
	Entry->GetFilteredChildren(Children);
	return Children.Num() > 0;
}

void SNiagaraStackTableRow::SetNameAndValueContent(TSharedRef<SWidget> InNameWidget, TSharedPtr<SWidget> InValueWidget, TSharedPtr<SWidget> InEditConditionWidget, TSharedPtr<SWidget> InResetWidget)
{
	FSlateColor IconColor = FNiagaraEditorWidgetsStyle::Get().GetColor(FNiagaraStackEditorWidgetsUtilities::GetColorNameForExecutionCategory(StackEntry->GetExecutionCategoryName()));
	if (bIsCategoryIconHighlighted)
	{
		IconColor = FNiagaraEditorWidgetsStyle::Get().GetColor(FNiagaraStackEditorWidgetsUtilities::GetIconColorNameForExecutionCategory(StackEntry->GetExecutionCategoryName()));
	}

	FName IconName = FNiagaraStackEditorWidgetsUtilities::GetIconNameForExecutionSubcategory(StackEntry->GetExecutionSubcategoryName(), bIsCategoryIconHighlighted);
	const FSlateBrush* IconBrush = nullptr;

	if(IconName != NAME_None)
	{
		IconBrush = FNiagaraEditorWidgetsStyle::Get().GetBrush(IconName);
	}
	
	TSharedRef<SHorizontalBox> NameContent = SNew(SHorizontalBox)
	.Clipping(EWidgetClipping::OnDemand)
	// Indent
	+ SHorizontalBox::Slot()
	.AutoWidth()
	[
		SNew(SBox)
		.WidthOverride(this, &SNiagaraStackTableRow::GetIndentSize)
	]
	// Expand button
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(0, 0, 1, 0)
	[
		SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.Visibility(this, &SNiagaraStackTableRow::GetExpanderVisibility)
		.OnClicked(this, &SNiagaraStackTableRow::ExpandButtonClicked)
		.ContentPadding(2)
		.HAlign(HAlign_Center)
		[
			SNew(SImage)
			.Image(this, &SNiagaraStackTableRow::GetExpandButtonImage)
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
	]
	// Execution sub-category icon
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.Padding(FMargin(1, 1, 2, 1))
	.VAlign(EVerticalAlignment::VAlign_Center)
	.HAlign(EHorizontalAlignment::HAlign_Center)
	[
		SNew(SBox)
		.WidthOverride(FNiagaraEditorWidgetsStyle::Get().GetFloat("NiagaraEditor.Stack.IconHighlightedSize"))
		.HAlign(EHorizontalAlignment::HAlign_Center)
		.VAlign(EVerticalAlignment::VAlign_Center)
		.ToolTipText(ExecutionCategoryToolTipText)
		.Visibility(this, &SNiagaraStackTableRow::GetExecutionCategoryIconVisibility)
		.IsEnabled_UObject(StackEntry, &UNiagaraStackEntry::GetIsEnabledAndOwnerIsEnabled)
		[
			SNew(SImage)
			.Visibility(this, &SNiagaraStackTableRow::GetExecutionCategoryIconVisibility)
			.Image(IconBrush ? IconBrush : FCoreStyle::Get().GetDefaultBrush())
			.ColorAndOpacity(IconColor)
		]
	]
	// Edit condition
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.Padding(0, 0, 3, 0)
	[
		InEditConditionWidget.IsValid() ? InEditConditionWidget.ToSharedRef() : SNullWidget::NullWidget
	]
	// Name content
	+ SHorizontalBox::Slot()
	.HAlign(NameHorizontalAlignment)
	.VAlign(NameVerticalAlignment)
	[
		InNameWidget
	];

	UNiagaraStackEntry* ParentEntry = GetParentEntryNoDivider(StackEntry);
	bool bIsDisplayedInCategory = StackEntry->HasFrontDivider() && ParentEntry && ParentEntry->GetShouldShowInStack();
	UNiagaraStackEntry* AboveEntry = GetEntryAbove(StackEntry);
	if (!StackEntry->HasFrontDivider() && AboveEntry && AboveEntry->HasFrontDivider())
	{
		ContentPadding.Top += 6;
	}
	if (StackEntry->HasFrontDivider())
	{
		ContentPadding.Left += IndentSize * (bIsDisplayedInCategory ? 3 : 2) - 4;
	}
	bool bInsertDivAbove = StackEntry->GetStackRowStyle() == UNiagaraStackEntry::EStackRowStyle::ItemCategory && HasVisibleChildren(AboveEntry);

	TSharedPtr<SWidget> ChildContent;
	if (InValueWidget.IsValid())
	{
		ChildContent = SNew(SSplitter)
		.Style(FAppStyle::Get(), "DetailsView.Splitter")
		.PhysicalSplitterHandleSize(1.0f)
		.HitDetectionSplitterHandleSize(5.0f)

		+ SSplitter::Slot()
		.Value(NameColumnWidth)
		.OnSlotResized(SSplitter::FOnSlotResized::CreateSP(this, &SNiagaraStackTableRow::OnNameColumnWidthChanged))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.Padding(FMargin(ContentPadding.Left, 0, 0, 0))
		    .AutoWidth()
		    [
				SNew(SBox)
				.WidthOverride(StackEntry->HasFrontDivider() ? 1 : 0)
		    ]
		    + SHorizontalBox::Slot()
            [
				SNew(SBox)
				.Padding(FMargin(0, ContentPadding.Top, 5, ContentPadding.Bottom))
				.MinDesiredWidth(NameMinWidth.IsSet() ? NameMinWidth.GetValue() : FOptionalSize())
				.MaxDesiredWidth(NameMaxWidth.IsSet() ? NameMaxWidth.GetValue() : FOptionalSize())
				[
                    NameContent
                ]
			]
		]

		// Value
		+ SSplitter::Slot()
		.Value(ValueColumnWidth)
		.OnSlotResized(SSplitter::FOnSlotResized::CreateSP(this, &SNiagaraStackTableRow::OnValueColumnWidthChanged))
		[
			SNew(SBox)
			.Padding(FMargin(4, ContentPadding.Top, ContentPadding.Right, ContentPadding.Bottom))
			.HAlign(ValueHorizontalAlignment)
			.VAlign(ValueVerticalAlignment)
			.MinDesiredWidth(ValueMinWidth.IsSet() ? ValueMinWidth.GetValue() : FOptionalSize())
			.MaxDesiredWidth(ValueMaxWidth.IsSet() ? ValueMaxWidth.GetValue() : FOptionalSize())
			[
				InValueWidget.ToSharedRef()
			]
		];
	}
	else
	{
		ChildContent = SNew(SBox)
		.Padding(ContentPadding)
		.HAlign(NameHorizontalAlignment)
		.VAlign(NameVerticalAlignment)
		.MinDesiredWidth(NameMinWidth.IsSet() ? NameMinWidth.GetValue() : FOptionalSize())
		.MaxDesiredWidth(NameMaxWidth.IsSet() ? NameMaxWidth.GetValue() : FOptionalSize())
		[
			NameContent
		];
	}

	FName AccentColorName = FNiagaraStackEditorWidgetsUtilities::GetIconColorNameForExecutionCategory(StackEntry->GetExecutionCategoryName());
	const bool bDisplayingIndicator = IndicatorColor != FStyleColors::Transparent;
	FSlateColor AccentColor = bDisplayingIndicator ? IndicatorColor : ((AccentColorName != NAME_None) ? FNiagaraEditorWidgetsStyle::Get().GetColor(AccentColorName) : FStyleColors::Transparent);

	ChildSlot
	[
		SNew(SHorizontalBox)
		.Visibility(this, &SNiagaraStackTableRow::GetRowVisibility)
		// Accent color.
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(bDisplayingIndicator ? FMargin(0, 0, 5, 0) : FMargin(1, 0, 6, 0) )
		[
			SNew(SBorder)
 			.BorderImage(FEditorStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(AccentColor)
			.Padding(0)
			[
				SNew(SBox)
				.WidthOverride(bDisplayingIndicator ? 6 : 4)
			]
		]
		// Content
		+ SHorizontalBox::Slot()
		.Padding(0.0f)
		[
			// Row content
			SNew(SBorder)
			.BorderImage(bDisplayingIndicator ? FAppStyle::Get().GetBrush("Brushes.Header") : FAppStyle::Get().GetBrush("DetailsView.GridLine"))
			.Padding(FMargin(0, 0, 0, 1))
			[
					SNew(SBorder)
 					.BorderImage(FEditorStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(this, &SNiagaraStackTableRow::GetItemBackgroundColor)
 					.ForegroundColor(ForegroundColor)
					.Padding(0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.Padding(0.0f)
						[
							SNew(SVerticalBox)
							+SVerticalBox::Slot()
							.Padding(bInsertDivAbove ? FMargin(8, 8, 8, 4) : 0)
							.AutoHeight()
							[
								SNew(SBorder)
								.BorderImage(FEditorStyle::GetBrush("WhiteBrush"))
 								.BorderBackgroundColor(FStyleColors::Panel)
								.Visibility(bInsertDivAbove ? EVisibility::Visible : EVisibility::Collapsed)
								.Padding(0)
								[
									SNew(SBox)
									.HeightOverride(1)
								]
							]
							+SVerticalBox::Slot()
							.Padding(0)
							[
								SNew(SBorder)
								.BorderImage(this, &SNiagaraStackTableRow::GetSearchResultBorderBrush)
 								.BorderBackgroundColor(FStyleColors::Select)
								.Padding(0)
								[
									ChildContent.ToSharedRef()
								]
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(3, 0)
						[
							SNew(SNiagaraStackIssueIcon, StackViewModel, StackEntry)
							.Visibility(IssueIconVisibility)
						]
						// Reset To Default
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 3, 0)
						[
							InResetWidget.IsValid() ? InResetWidget.ToSharedRef() : SNullWidget::NullWidget
						]
					]
				]
			]
		
	];
}

void SNiagaraStackTableRow::AddFillRowContextMenuHandler(FOnFillRowContextMenu FillRowContextMenuHandler)
{
	OnFillRowContextMenuHanders.Add(FillRowContextMenuHandler);
}

FReply SNiagaraStackTableRow::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	return FReply::Unhandled();
}

FReply SNiagaraStackTableRow::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		TSharedPtr<ITypedTableView<UNiagaraStackEntry*>> OwnerTable = OwnerTablePtr.Pin();
		if (OwnerTable.IsValid())
		{
			if (OwnerTable->GetSelectedItems().Contains(StackEntry) == false)
			{
				OwnerTable->Private_ClearSelection();
				OwnerTable->Private_SetItemSelection(StackEntry, true, true);
				OwnerTable->Private_SignalSelectionChanged(ESelectInfo::OnMouseClick);
			}
		}

		FMenuBuilder MenuBuilder(true, StackCommandContext->GetCommands());
		for (FOnFillRowContextMenu& OnFillRowContextMenuHandler : OnFillRowContextMenuHanders)
		{
			OnFillRowContextMenuHandler.ExecuteIfBound(MenuBuilder);
		}

		FNiagaraStackEditorWidgetsUtilities::AddStackEntryAssetContextMenuActions(MenuBuilder, *StackEntry);
		StackCommandContext->AddEditMenuItems(MenuBuilder);

		TArray<UNiagaraStackEntry*> EntriesToProcess;
		TArray<UNiagaraStackEntry*> NavigationEntries;
		StackViewModel->GetPathForEntry(StackEntry, EntriesToProcess);
		for (UNiagaraStackEntry* Parent : EntriesToProcess)
		{
			UNiagaraStackItemGroup* GroupParent = Cast<UNiagaraStackItemGroup>(Parent);
			UNiagaraStackItem* ItemParent = Cast<UNiagaraStackItem>(Parent);
			if (GroupParent != nullptr || ItemParent != nullptr)
			{
				MenuBuilder.BeginSection("StackRowNavigation", LOCTEXT("NavigationMenuSection", "Navigation"));
				{
					if (GroupParent != nullptr)
					{
						MenuBuilder.AddMenuEntry(
							LOCTEXT("TopOfSection", "Top of Section"),
							FText::Format(LOCTEXT("NavigateToFormatted", "Navigate to {0}"), Parent->GetDisplayName()),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateSP(this, &SNiagaraStackTableRow::NavigateTo, Parent)));
					}
					if (ItemParent != nullptr)
					{
						MenuBuilder.AddMenuEntry(
							LOCTEXT("TopOfModule", "Top of Module"),
							FText::Format(LOCTEXT("NavigateToFormatted", "Navigate to {0}"), Parent->GetDisplayName()),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateSP(this, &SNiagaraStackTableRow::NavigateTo, Parent)));
					}
				}
				MenuBuilder.EndSection();
			}
		}

		MenuBuilder.BeginSection("StackActions", LOCTEXT("StackActions", "Stack Actions"));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ExpandAllItems", "Expand All"),
			LOCTEXT("ExpandAllItemsToolTip", "Expand all items under this header."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SNiagaraStackTableRow::ExpandChildren)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("CollapseAllItems", "Collapse All"),
			LOCTEXT("CollapseAllItemsToolTip", "Collapse all items under this header."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SNiagaraStackTableRow::CollapseChildren)));
		MenuBuilder.EndSection();


		if (IsValidForSummaryView())
		{
			FUIAction ShowHideSummaryViewAction(
			FExecuteAction::CreateSP(this, &SNiagaraStackTableRow::ToggleShowInSummaryView),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SNiagaraStackTableRow::ShouldShowInSummaryView));
		
			MenuBuilder.BeginSection("SummaryViewActions", LOCTEXT("SummaryViewActions", "Summary View"));
			MenuBuilder.AddMenuEntry(
				LOCTEXT("SummaryViewShow", "Show In Summary View"),
				LOCTEXT("SummaryViewShowTooltip", "Should this parameter be visible in the summary view?"),
				FSlateIcon(),
				ShowHideSummaryViewAction,
				NAME_None, EUserInterfaceActionType::ToggleButton);
			MenuBuilder.EndSection();
		}

		FWidgetPath WidgetPath = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
		FSlateApplication::Get().PushMenu(AsShared(), WidgetPath, MenuBuilder.MakeWidget(), MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		return FReply::Handled();
	}
	return STableRow<UNiagaraStackEntry*>::OnMouseButtonUp(MyGeometry, MouseEvent);
}

const FSlateBrush* SNiagaraStackTableRow::GetBorder() const
{
	// Return no brush here so that the background doesn't change.  The border color changing will be handled by an internal border.
	return FEditorStyle::GetBrush("NoBrush");
}

void SetExpansionStateRecursive(UNiagaraStackEntry* StackEntry, bool bIsExpanded)
{
	if (StackEntry->GetCanExpand())
	{
		StackEntry->SetIsExpanded(bIsExpanded);
	}

	TArray<UNiagaraStackEntry*> Children;
	StackEntry->GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		SetExpansionStateRecursive(Child, bIsExpanded);
	}
}

void SNiagaraStackTableRow::CollapseChildren()
{
	bool bIsExpanded = false;
	SetExpansionStateRecursive(StackEntry, bIsExpanded);
}

void SNiagaraStackTableRow::ExpandChildren()
{
	bool bIsExpanded = true;
	SetExpansionStateRecursive(StackEntry, bIsExpanded);
}

EVisibility SNiagaraStackTableRow::GetRowVisibility() const
{
	return StackEntry->GetShouldShowInStack()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SNiagaraStackTableRow::GetExecutionCategoryIconVisibility() const
{
	return bShowExecutionCategoryIcon && (StackEntry->GetExecutionSubcategoryName() != NAME_None)
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FOptionalSize SNiagaraStackTableRow::GetIndentSize() const
{
	if (StackEntry->HasFrontDivider())
	{
		UNiagaraStackEntry* ParentEntry = GetParentEntryNoDivider(StackEntry);
		return (StackEntry->GetIndentLevel() - (ParentEntry && ParentEntry->GetShouldShowInStack() ? 1 : 0)) * IndentSize;
	}
	return StackEntry->GetIndentLevel() * IndentSize;
}

EVisibility SNiagaraStackTableRow::GetExpanderVisibility() const
{
	if (StackEntry->GetCanExpand())
	{
		TArray<UNiagaraStackEntry*> Children;
		StackEntry->GetFilteredChildren(Children);
		return Children.Num() > 0
			? EVisibility::Visible
			: EVisibility::Hidden;
	}
	else
	{
		return EVisibility::Collapsed;
	}
}

FReply SNiagaraStackTableRow::ExpandButtonClicked()
{
	const bool bWillBeExpanded = !StackEntry->GetIsExpanded();
	// Recurse the expansion if "shift" is being pressed
	const FModifierKeysState ModKeyState = FSlateApplication::Get().GetModifierKeys();
	if (ModKeyState.IsShiftDown())
	{
		StackEntry->SetIsExpanded_Recursive(bWillBeExpanded);
	}
	else
	{
		StackEntry->SetIsExpanded(bWillBeExpanded);
	}
	return FReply::Handled();
}

const FSlateBrush* SNiagaraStackTableRow::GetExpandButtonImage() const
{
	return StackEntry->GetIsExpanded() ? ExpandedImage : CollapsedImage;
}

void SNiagaraStackTableRow::OnNameColumnWidthChanged(float Width)
{
	NameColumnWidthChanged.ExecuteIfBound(Width);
}

void SNiagaraStackTableRow::OnValueColumnWidthChanged(float Width)
{
	ValueColumnWidthChanged.ExecuteIfBound(Width);
}

FSlateColor SNiagaraStackTableRow::GetItemBackgroundColor() const
{
	return StackEntry->GetIsEnabledAndOwnerIsEnabled() 
		? ItemBackgroundColor 
		: DisabledItemBackgroundColor;
}

const FSlateBrush* SNiagaraStackTableRow::GetSelectionBorderBrush() const
{
	return STableRow<UNiagaraStackEntry*>::GetBorder();
}

const FSlateBrush* SNiagaraStackTableRow::GetSearchResultBorderBrush() const
{
	return StackViewModel->GetCurrentFocusedEntry() == StackEntry
		? FNiagaraEditorWidgetsStyle::Get().GetBrush("NiagaraEditor.Stack.SearchResult")
		: FEditorStyle::GetBrush("NoBrush");
}

void SNiagaraStackTableRow::NavigateTo(UNiagaraStackEntry* Item)
{
	OwnerTree->RequestNavigateToItem(Item, 0);
}

bool SNiagaraStackTableRow::IsValidForSummaryView() const
{
	UNiagaraStackFunctionInput* FunctionInput = Cast<UNiagaraStackFunctionInput>(StackEntry);
	if (FunctionInput && FunctionInput->GetEmitterViewModel())
	{
		UNiagaraEmitter* Emitter = FunctionInput->GetEmitterViewModel()->GetEmitter();
		UNiagaraStackFunctionInput* ParentInput = FNiagaraStackEditorWidgetsUtilities::FindTopMostParentFunctionInput(FunctionInput);			
		if (Emitter && FNiagaraStackEditorWidgetsUtilities::GetSummaryViewInputKeyForFunctionInput(ParentInput).IsSet())
		{
			return true;	
		}
	}	
	return false;
}

void SNiagaraStackTableRow::ToggleShowInSummaryView()
{
	UNiagaraStackFunctionInput* FunctionInput = Cast<UNiagaraStackFunctionInput>(StackEntry);
	if (FunctionInput && FunctionInput->GetEmitterViewModel())
	{
		UNiagaraEmitter* Emitter = FunctionInput->GetEmitterViewModel()->GetEmitter();
		if (Emitter)
		{
			UNiagaraStackFunctionInput* ParentInput = FNiagaraStackEditorWidgetsUtilities::FindTopMostParentFunctionInput(FunctionInput);		
			TOptional<FFunctionInputSummaryViewKey> Key = FNiagaraStackEditorWidgetsUtilities::GetSummaryViewInputKeyForFunctionInput(ParentInput);
			if (Key.IsSet())
			{
				UNiagaraEmitterEditorData* EditorData = Cast<UNiagaraEmitterEditorData>(Emitter->GetEditorData());
				if (EditorData)
				{
					FFunctionInputSummaryViewMetadata SummaryViewMetaData = EditorData->GetSummaryViewMetaData(Key.GetValue());
					SummaryViewMetaData.bVisible = !SummaryViewMetaData.bVisible;			
					EditorData->SetSummaryViewMetaData(Key.GetValue(), SummaryViewMetaData);
				}
			}			
		}
	}
}

bool SNiagaraStackTableRow::ShouldShowInSummaryView() const
{
	UNiagaraStackFunctionInput* FunctionInput = Cast<UNiagaraStackFunctionInput>(StackEntry);
	if (FunctionInput && FunctionInput->GetEmitterViewModel())
	{
		UNiagaraEmitter* Emitter = FunctionInput->GetEmitterViewModel()->GetEmitter();
		if (Emitter)
		{
			UNiagaraStackFunctionInput* ParentInput = FNiagaraStackEditorWidgetsUtilities::FindTopMostParentFunctionInput(FunctionInput);			
			TOptional<FFunctionInputSummaryViewKey> Key = FNiagaraStackEditorWidgetsUtilities::GetSummaryViewInputKeyForFunctionInput(ParentInput);
			if (Key.IsSet())
			{
				UNiagaraEmitterEditorData* EditorData = Cast<UNiagaraEmitterEditorData>(Emitter->GetEditorData());
				if (EditorData)
				{
					return EditorData->GetSummaryViewMetaData(Key.GetValue()).bVisible;
				}
			}			
		}
	}

	return false;
}

#undef LOCTEXT_NAMESPACE
