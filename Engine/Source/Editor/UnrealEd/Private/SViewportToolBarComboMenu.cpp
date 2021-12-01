// Copyright Epic Games, Inc. All Rights Reserved.

#include "SViewportToolBarComboMenu.h"
#include "SEditorViewportToolBarMenuButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SButton.h"
#include "EditorStyleSet.h"
#include "Styling/ToolBarStyle.h"

void SViewportToolBarComboMenu::Construct( const FArguments& InArgs )
{
	const FToolBarStyle& ToolBarStyle = FAppStyle::Get().GetWidgetStyle<FToolBarStyle>(InArgs._Style.Get());

	EMultiBlockLocation::Type BlockLocation = InArgs._BlockLocation;

	const FButtonStyle& ButtonStyle = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("EditorViewportToolBar.ComboMenu.ButtonStyle");
	const FCheckBoxStyle& CheckBoxStyle = FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("EditorViewportToolBar.ToggleButton.Start");
	const FTextBlockStyle& LabelStyle = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("EditorViewportToolBar.ComboMenu.LabelStyle");

	const FSlateIcon& Icon = InArgs._Icon.Get();
	ParentToolBar = InArgs._ParentToolBar;


	TSharedRef<SCheckBox> ToggleControl = SNew(SCheckBox)
		.Style(&CheckBoxStyle)
		.ToolTipText(InArgs._ToggleButtonToolTip)
		.OnCheckStateChanged(InArgs._OnCheckStateChanged)
		.IsChecked(InArgs._IsChecked)
		[
			SNew(SImage)
			.Image(Icon.GetIcon())
			.ColorAndOpacity(FSlateColor::UseForeground())
		];


	{
		MenuAnchor = SNew(SMenuAnchor)
		.Placement( MenuPlacement_BelowAnchor )
		.OnGetMenuContent( InArgs._OnGetMenuContent );

		MenuAnchor->SetContent(
			SNew(SBox)
			.MinDesiredWidth(InArgs._MinDesiredButtonWidth > 0.0f ? InArgs._MinDesiredButtonWidth : FOptionalSize())
			[
				SNew(SEditorViewportToolBarMenuButton, MenuAnchor.ToSharedRef())
				.ButtonStyle(&ButtonStyle)
				.ToolTipText(InArgs._MenuButtonToolTip)
				.OnClicked(this, &SViewportToolBarComboMenu::OnMenuClicked)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.TextStyle(&LabelStyle)
					.Text(InArgs._Label)
				]
			]
		);
	}


	ChildSlot
	[
		SNew(SHorizontalBox)
		+SHorizontalBox::Slot()
		.AutoWidth()
		[
			ToggleControl
		]

		+SHorizontalBox::Slot()
		.AutoWidth()
		[
			MenuAnchor.ToSharedRef()
		]
	];
}

FReply SViewportToolBarComboMenu::OnMenuClicked()
{
	// If the menu button is clicked toggle the state of the menu anchor which will open or close the menu
	MenuAnchor->SetIsOpen( !MenuAnchor->IsOpen() );
	ParentToolBar.Pin()->SetOpenMenu( MenuAnchor );
	return FReply::Handled();
}

void SViewportToolBarComboMenu::OnMouseEnter( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	// See if there is another menu on the same tool bar already open
	TWeakPtr<SMenuAnchor> OpenedMenu = ParentToolBar.Pin()->GetOpenMenu();
	if( OpenedMenu.IsValid() && OpenedMenu.Pin()->IsOpen() && OpenedMenu.Pin() != MenuAnchor )
	{
		// There is another menu open so we open this menu and close the other
		ParentToolBar.Pin()->SetOpenMenu( MenuAnchor ); 
		MenuAnchor->SetIsOpen( true );
	}
}