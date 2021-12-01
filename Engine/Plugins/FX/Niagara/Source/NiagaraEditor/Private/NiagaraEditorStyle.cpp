// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraEditorStyle.h"

#include "Framework/Application/SlateApplication.h"
#include "EditorStyleSet.h"
#include "Slate/SlateGameResources.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Interfaces/IPluginManager.h"
#include "Classes/EditorStyleSettings.h"
#include "Styling/StarshipCoreStyle.h"
#include "Styling/StyleColors.h"

TSharedPtr< FSlateStyleSet > FNiagaraEditorStyle::NiagaraEditorStyleInstance = NULL;

void FNiagaraEditorStyle::Initialize()
{
	if (!NiagaraEditorStyleInstance.IsValid())
	{
		NiagaraEditorStyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*NiagaraEditorStyleInstance);
	}
}

void FNiagaraEditorStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*NiagaraEditorStyleInstance);
	ensure(NiagaraEditorStyleInstance.IsUnique());
	NiagaraEditorStyleInstance.Reset();
}

FName FNiagaraEditorStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("NiagaraEditorStyle"));
	return StyleSetName;
}

NIAGARAEDITOR_API FString RelativePathToPluginPath(const FString& RelativePath, const ANSICHAR* Extension)
{
	static FString ContentDir = IPluginManager::Get().FindPlugin(TEXT("Niagara"))->GetContentDir();
	return (ContentDir / RelativePath) + Extension;
}

#define IMAGE_BRUSH( RelativePath, ... ) FSlateImageBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__ )
#define IMAGE_CORE_BRUSH( RelativePath, ... ) FSlateImageBrush( FPaths::EngineContentDir() / "Editor/Slate" / RelativePath + TEXT(".png"), __VA_ARGS__ )
#define IMAGE_PLUGIN_BRUSH( RelativePath, ... ) FSlateImageBrush( RelativePathToPluginPath( RelativePath, ".png" ), __VA_ARGS__ )
#define BOX_BRUSH( RelativePath, ... ) FSlateBoxBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__ )
#define BOX_CORE_BRUSH( RelativePath, ... ) FSlateBoxBrush( FPaths::EngineContentDir() / "Editor/Slate" / RelativePath + TEXT(".png"), __VA_ARGS__ )
#define BOX_PLUGIN_BRUSH( RelativePath, ... ) FSlateBoxBrush( RelativePathToPluginPath( RelativePath, ".png"), __VA_ARGS__ )
#define BORDER_CORE_BRUSH( RelativePath, ... ) FSlateBorderBrush( FPaths::EngineContentDir() / "Editor/Slate" / RelativePath + TEXT(".png"), __VA_ARGS__ )
#define BORDER_BRUSH( RelativePath, ... ) FSlateBorderBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__ )
#define DEFAULT_FONT(...) FStarshipCoreStyle::GetDefaultFontStyle(__VA_ARGS__)
#define IMAGE_PLUGIN_BRUSH_SVG( RelativePath, ... ) FSlateVectorImageBrush( RelativePathToPluginPath( RelativePath, ".svg" ), __VA_ARGS__ )

const FVector2D Icon8x8(8.0f, 8.0f);
const FVector2D Icon12x12(12.0f, 12.0f);
const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);
const FVector2D Icon40x40(40.0f, 40.0f);
const FVector2D Icon64x64(64.0f, 64.0f);

namespace NiagaraEditorStyleImpl {

void InitStats(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle CategoryText = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("DetailsView.CategoryTextStyle");
	const FSlateColor SelectionColor = FEditorStyle::GetSlateColor("SelectionColor");
	const FSlateColor SelectionColor_Pressed = FEditorStyle::GetSlateColor("SelectionColor_Pressed");

	Style->Set("NiagaraEditor.StatsText", CategoryText);
}

void InitAssetPicker(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	FTextBlockStyle AssetPickerBoldAssetNameText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FLinearColor::White)
		.SetFont(DEFAULT_FONT("Bold", 9));

	Style->Set("NiagaraEditor.AssetPickerBoldAssetNameText", AssetPickerBoldAssetNameText);

	FTextBlockStyle AssetPickerAssetNameText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FLinearColor::White)
		.SetFont(DEFAULT_FONT("Regular", 9));

	Style->Set("NiagaraEditor.AssetPickerAssetNameText", AssetPickerAssetNameText);

	FTextBlockStyle AssetPickerAssetCategoryText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 11));

	Style->Set("NiagaraEditor.AssetPickerAssetCategoryText", AssetPickerAssetCategoryText);


	FTextBlockStyle AssetPickerAssetSubcategoryText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 10));

	Style->Set("NiagaraEditor.AssetPickerAssetSubcategoryText", AssetPickerAssetSubcategoryText);

	// New Asset Dialog
	FTextBlockStyle NewAssetDialogOptionText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 11));

	Style->Set("NiagaraEditor.NewAssetDialog.OptionText", NewAssetDialogOptionText);

	FTextBlockStyle NewAssetDialogHeaderText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FLinearColor::White)
		.SetFont(DEFAULT_FONT("Bold", 10));

	Style->Set("NiagaraEditor.NewAssetDialog.HeaderText", NewAssetDialogHeaderText);

	FTextBlockStyle NewAssetDialogSubHeaderText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FLinearColor::White)
		.SetFont(DEFAULT_FONT("Bold", 11));

	Style->Set("NiagaraEditor.NewAssetDialog.SubHeaderText", NewAssetDialogSubHeaderText);

	Style->Set("NiagaraEditor.NewAssetDialog.AddButton", FButtonStyle()
		.SetNormal(BOX_CORE_BRUSH("Common/FlatButton", 2.0f / 8.0f, FLinearColor(0, 0, 0, .25f)))
		.SetHovered(BOX_CORE_BRUSH("Common/FlatButton", 2.0f / 8.0f, FEditorStyle::GetSlateColor("SelectionColor")))
		.SetPressed(BOX_CORE_BRUSH("Common/FlatButton", 2.0f / 8.0f, FEditorStyle::GetSlateColor("SelectionColor_Pressed")))
	);

	Style->Set("NiagaraEditor.NewAssetDialog.SubBorderColor", FLinearColor(FColor(48, 48, 48)));
	Style->Set("NiagaraEditor.NewAssetDialog.ActiveOptionBorderColor", FLinearColor(FColor(96, 96, 96)));

	Style->Set("NiagaraEditor.NewAssetDialog.SubBorder", new BOX_CORE_BRUSH("Common/GroupBorderLight", FMargin(4.0f / 16.0f)));
}

void InitActionMenu(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	// Action Menu
	FTextBlockStyle ActionMenuHeadingText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FSlateColor::UseForeground())
		.SetHighlightColor(FLinearColor(0.02f, 0.3f, 0.0f))
		.SetFont(DEFAULT_FONT("Bold", 10));

	FTextBlockStyle ActionMenuActionText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FSlateColor::UseForeground())
		.SetHighlightColor(FLinearColor(0.02f, 0.3f, 0.0f))
		.SetFont(DEFAULT_FONT("Regular", 9));

	FTextBlockStyle ActionMenuSourceText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FSlateColor::UseForeground())
		.SetFont(DEFAULT_FONT("Regular", 7));

	FTextBlockStyle ActionMenuFilterText = FTextBlockStyle(NormalText)
        .SetColorAndOpacity(FSlateColor::UseForeground())
		.SetHighlightColor(FLinearColor(0.02f, 0.3f, 0.0f))
		.SetShadowOffset(FVector2D(1.f, 1.f))
        .SetFont(DEFAULT_FONT("Bold", 9));
        
	FTextBlockStyle TemplateTabText = FTextBlockStyle(NormalText)
		.SetColorAndOpacity(FSlateColor::UseForeground())
		.SetHighlightColor(FLinearColor(0.02f, 0.3f, 0.0f))
		.SetFont(DEFAULT_FONT("Bold", 11));

	const FCheckBoxStyle NiagaraGraphActionMenuFilterCheckBox = FCheckBoxStyle()
            .SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
            .SetUncheckedImage( FSlateNoResource() )
            .SetUncheckedHoveredImage(BOX_CORE_BRUSH("Common/RoundedSelection_16x", 4.0f/16.0f, FLinearColor(0.7f, 0.7f, 0.7f) ))
            .SetUncheckedPressedImage(BOX_CORE_BRUSH("Common/RoundedSelection_16x", 4.0f/16.0f, FLinearColor(0.8f, 0.8f, 0.8f) ))
            .SetCheckedImage(BOX_CORE_BRUSH("Common/RoundedSelection_16x",  4.0f/16.0f, FLinearColor(0.9f, 0.9f, 0.9f) ))
            .SetCheckedHoveredImage(BOX_CORE_BRUSH("Common/RoundedSelection_16x",  4.0f/16.0f, FLinearColor(1.f, 1.f, 1.f) ))
            .SetCheckedPressedImage(BOX_CORE_BRUSH("Common/RoundedSelection_16x",  4.0f/16.0f, FLinearColor(1.f, 1.f, 1.f) ));

	const FTableRowStyle ActionMenuRowStyle = FTableRowStyle()
            .SetEvenRowBackgroundBrush(FSlateNoResource())
            .SetOddRowBackgroundBrush(FSlateNoResource())
			.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, FLinearColor(1.0f, 1.0f, 1.0f, 0.1f)))
			.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, FLinearColor(1.0f, 1.0f, 1.0f, 0.1f)))
            .SetSelectorFocusedBrush(BORDER_CORE_BRUSH("Common/Selector", FMargin(4.f / 16.f), FStarshipCoreStyle::GetCoreStyle().GetSlateColor("SelectorColor")))
            .SetActiveBrush(FSlateColorBrush(FStyleColors::Select))
            .SetActiveHoveredBrush(FSlateColorBrush(FStyleColors::Select))
            .SetInactiveBrush(FSlateColorBrush(FStyleColors::SelectInactive))
            .SetInactiveHoveredBrush(FSlateColorBrush(FStyleColors::SelectHover))
            .SetActiveHighlightedBrush(FSlateColorBrush(FStyleColors::PrimaryHover))
            .SetInactiveHighlightedBrush(FSlateColorBrush(FStyleColors::SelectParent))
			.SetTextColor(FStyleColors::Foreground)
			.SetSelectedTextColor(FStyleColors::ForegroundInverted)

            .SetDropIndicator_Above(BOX_CORE_BRUSH("Common/DropZoneIndicator_Above", FMargin(10.0f / 16.0f, 10.0f / 16.0f, 0, 0), FStarshipCoreStyle::GetCoreStyle().GetSlateColor("SelectorColor")))
            .SetDropIndicator_Onto(BOX_CORE_BRUSH("Common/DropZoneIndicator_Onto", FMargin(4.0f / 16.0f), FStarshipCoreStyle::GetCoreStyle().GetSlateColor("SelectorColor")))
            .SetDropIndicator_Below(BOX_CORE_BRUSH("Common/DropZoneIndicator_Below", FMargin(10.0f / 16.0f, 0, 0, 10.0f / 16.0f), FStarshipCoreStyle::GetCoreStyle().GetSlateColor("SelectorColor")));
	
	Style->Set("ActionMenu.Row", ActionMenuRowStyle);
	
	Style->Set("ActionMenu.HeadingTextBlock", ActionMenuHeadingText);

	Style->Set("ActionMenu.ActionTextBlock", ActionMenuActionText);

	Style->Set("GraphActionMenu.ActionSourceTextBlock", ActionMenuSourceText);

	Style->Set("GraphActionMenu.ActionFilterTextBlock", ActionMenuFilterText);

	Style->Set("GraphActionMenu.TemplateTabTextBlock", TemplateTabText);
	
	Style->Set("GraphActionMenu.FilterCheckBox", NiagaraGraphActionMenuFilterCheckBox);
}

void InitEmitterHeader(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
	
	// Emitter Header
	FTextBlockStyle StackHeaderText = FTextBlockStyle(NormalText);
	StackHeaderText.SetFont(DEFAULT_FONT("Regular", 11))
		.SetColorAndOpacity(FSlateColor(EStyleColor::White));

	Style->Set("NiagaraEditor.HeadingTextBlock", StackHeaderText);

	FTextBlockStyle StackHeaderTextSubdued = FTextBlockStyle(NormalText);
	StackHeaderTextSubdued.SetFont(DEFAULT_FONT("Regular", 11))
		.SetColorAndOpacity(FStyleColors::Foreground);

	Style->Set("NiagaraEditor.HeadingTextBlockSubdued", StackHeaderTextSubdued);

	FTextBlockStyle TabText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Regular", 12))
		.SetShadowOffset(FVector2D(0, 1))
		.SetShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.9f));
	
	Style->Set("NiagaraEditor.AttributeSpreadsheetTabText", TabText);

	FTextBlockStyle SubduedHeadingText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Regular", 14))
		.SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f)));
	
	Style->Set("NiagaraEditor.SubduedHeadingTextBox", SubduedHeadingText);

	// Details
	FTextBlockStyle DetailsHeadingText = FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 9));

	Style->Set("NiagaraEditor.DetailsHeadingText", DetailsHeadingText);
}

void InitParameters(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
	const FEditableTextBoxStyle NormalEditableTextBox = FStarshipCoreStyle::GetCoreStyle().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");

	// Parameters
	
	FSlateFontInfo NormalFont = FAppStyle::Get().GetFontStyle(TEXT("PropertyWindow.NormalFont"));
	FTextBlockStyle ParameterText = FTextBlockStyle(NormalText)
		.SetFont(NormalFont);

	Style->Set("NiagaraEditor.ParameterText", ParameterText);

	FEditableTextBoxStyle ParameterEditableText = FEditableTextBoxStyle(NormalEditableTextBox)
		.SetFont(NormalFont);

	FInlineEditableTextBlockStyle ParameterEditableTextBox = FInlineEditableTextBlockStyle()
		.SetEditableTextBoxStyle(ParameterEditableText)
		.SetTextStyle(ParameterText);
	Style->Set("NiagaraEditor.ParameterInlineEditableText", ParameterEditableTextBox);


	Style->Set("NiagaraEditor.ParameterName.NamespaceBorder", new BOX_PLUGIN_BRUSH("Icons/NamespaceBorder", FMargin(4.0f / 16.0f)));

	Style->Set("NiagaraEditor.ParameterName.NamespaceText", FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 8))
		.SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.9f))
		.SetShadowOffset(FVector2D(1, 1))
		.SetShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.7f)));

	Style->Set("NiagaraEditor.ParameterName.NamespaceTextDark", FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 8))
		.SetColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f))
		.SetShadowOffset(FVector2D(1, 1))
		.SetShadowColorAndOpacity(FLinearColor(1.0, 1.0, 1.0, 0.25f)));

	Style->Set("NiagaraEditor.ParameterName.TypeText", FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Regular", 8))
		.SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.5f)));
}

void InitParameterMapView(TSharedRef< FSlateStyleSet > Style)
{
	// Parameter Map View
	Style->Set("NiagaraEditor.Stack.DepressedHighlightedButtonBrush", new BOX_CORE_BRUSH("Common/ButtonHoverHint", FMargin(4 / 16.0f), FStyleColors::PrimaryPress));
	Style->Set("NiagaraEditor.Stack.FlatButtonColor", FLinearColor(FColor(205, 205, 205)));

	//Parameters panel
	const FTableRowStyle TreeViewStyle = FEditorStyle::GetWidgetStyle<FTableRowStyle>("DetailsView.TreeView.TableRow");
	FTableRowStyle ParameterPanelRowStyle = FTableRowStyle(TreeViewStyle)
		.SetTextColor(FLinearColor::White)
		.SetSelectedTextColor(FLinearColor::White);
	Style->Set("NiagaraEditor.Parameters.TableRow", ParameterPanelRowStyle);
	
	const FTextBlockStyle CategoryTextStyle = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("DetailsView.CategoryTextStyle");
	FTextBlockStyle ParameterSectionStyle = FTextBlockStyle(CategoryTextStyle)
		.SetColorAndOpacity(FLinearColor::White);
	Style->Set("NiagaraEditor.Parameters.HeaderText", ParameterSectionStyle);
}

void InitCodeView(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	// Code View
	Style->Set("NiagaraEditor.CodeView.Checkbox.Text", FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 12))
		.SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.9f))
		.SetShadowOffset(FVector2D(1, 1))
		.SetShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.9f)));

	constexpr int32 LogFontSize = 9;
	FSlateFontInfo LogFont = DEFAULT_FONT("Mono", LogFontSize);
	FTextBlockStyle NormalLogText = FTextBlockStyle(NormalText)
		.SetFont(LogFont)
		.SetColorAndOpacity(FLinearColor(FColor(0xffffffff)))
		.SetSelectedBackgroundColor(FLinearColor(FColor(0xff666666)));
	Style->Set("NiagaraEditor.CodeView.Hlsl.Normal", NormalLogText);


	const FTextBlockStyle ErrorText = FTextBlockStyle(NormalText)
		.SetUnderlineBrush(IMAGE_BRUSH("White", Icon8x8, FLinearColor::Red, ESlateBrushTileType::Both))
		.SetColorAndOpacity(FLinearColor::Red);
		
	Style->Set("TextEditor.NormalText", NormalText);

	constexpr int32 HlslFontSize = 9;
	FSlateFontInfo HlslFont = DEFAULT_FONT("Mono", HlslFontSize);
	FTextBlockStyle NormalHlslText = FTextBlockStyle(NormalText)
		.SetFont(HlslFont);
	const FTextBlockStyle HlslErrorText = FTextBlockStyle(NormalHlslText)
		.SetUnderlineBrush(IMAGE_BRUSH("White", Icon8x8, FLinearColor::Red, ESlateBrushTileType::Both))
		.SetColorAndOpacity(FLinearColor::Red);
	
	Style->Set("SyntaxHighlight.HLSL.Normal", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(189, 183, 107))));
	Style->Set("SyntaxHighlight.HLSL.Operator", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(220, 220, 220))));
	Style->Set("SyntaxHighlight.HLSL.Keyword", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(86, 156, 214))));
	Style->Set("SyntaxHighlight.HLSL.String", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(214, 157, 133))));
	Style->Set("SyntaxHighlight.HLSL.Number", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(181, 206, 168))));
	Style->Set("SyntaxHighlight.HLSL.Comment", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(87, 166, 74))));
	Style->Set("SyntaxHighlight.HLSL.PreProcessorKeyword", FTextBlockStyle(NormalHlslText).SetColorAndOpacity(FLinearColor(FColor(188, 98, 171))));

	Style->Set("SyntaxHighlight.HLSL.Error", HlslErrorText); 
		
}

void InitSelectedEmitter(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	// Selected Emitter
	FSlateFontInfo SelectedEmitterUnsupportedSelectionFont = DEFAULT_FONT("Regular", 10);
	FTextBlockStyle SelectedEmitterUnsupportedSelectionText = FTextBlockStyle(NormalText)
		.SetFont(SelectedEmitterUnsupportedSelectionFont)
		.SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
	Style->Set("NiagaraEditor.SelectedEmitter.UnsupportedSelectionText", SelectedEmitterUnsupportedSelectionText);
}

void InitToolbarIcons(TSharedRef< FSlateStyleSet > Style)
{
	Style->Set("NiagaraEditor.Apply", new IMAGE_BRUSH("Icons/icon_Niagara_Apply_40x", Icon40x40));
	Style->Set("NiagaraEditor.Apply.Small", new IMAGE_BRUSH("Icons/icon_Niagara_Apply_40x", Icon20x20));
	Style->Set("NiagaraEditor.ApplyScratchPadChanges", new IMAGE_PLUGIN_BRUSH("Icons/Commands/icon_ApplyScratchPadChanges_40x", Icon40x40));
	Style->Set("NiagaraEditor.ApplyScratchPadChanges.Small", new IMAGE_PLUGIN_BRUSH("Icons/Commands/icon_ApplyScratchPadChanges_40x", Icon20x20));
	Style->Set("NiagaraEditor.Compile", new IMAGE_BRUSH("Icons/icon_compile_40x", Icon40x40));
	Style->Set("NiagaraEditor.Compile.Small", new IMAGE_BRUSH("Icons/icon_compile_40x", Icon20x20));
	Style->Set("NiagaraEditor.AddEmitter", new IMAGE_BRUSH("Icons/icon_AddObject_40x", Icon40x40));
	Style->Set("NiagaraEditor.AddEmitter.Small", new IMAGE_BRUSH("Icons/icon_AddObject_40x", Icon20x20));
	Style->Set("NiagaraEditor.UnlockToChanges", new IMAGE_BRUSH("Icons/icon_levels_unlocked_40x", Icon40x40));
	Style->Set("NiagaraEditor.UnlockToChanges.Small", new IMAGE_BRUSH("Icons/icon_levels_unlocked_40x", Icon20x20));
	Style->Set("NiagaraEditor.LockToChanges", new IMAGE_BRUSH("Icons/icon_levels_LockedReadOnly_40x", Icon40x40));
	Style->Set("NiagaraEditor.LockToChanges.Small", new IMAGE_BRUSH("Icons/icon_levels_LockedReadOnly_40x", Icon20x20));
	Style->Set("NiagaraEditor.SimulationOptions", new IMAGE_PLUGIN_BRUSH("Icons/Commands/icon_simulationOptions_40x", Icon40x40));
	Style->Set("NiagaraEditor.SimulationOptions.Small", new IMAGE_PLUGIN_BRUSH("Icons/Commands/icon_simulationOptions_40x", Icon20x20));

	Style->Set("Niagara.Asset.ReimportAsset.Needed", new IMAGE_BRUSH("Icons/icon_Reimport_Needed_40x", Icon40x40));
	Style->Set("Niagara.Asset.ReimportAsset.Default", new IMAGE_BRUSH("Icons/icon_Reimport_40x", Icon40x40));
	
	Style->Set("NiagaraEditor.OverviewNode.IsolatedColor", FLinearColor::Yellow);
	Style->Set("NiagaraEditor.OverviewNode.NotIsolatedColor", FLinearColor::Transparent);
}

void InitIcons(TSharedRef< FSlateStyleSet > Style)
{
	// Icons
	Style->Set("NiagaraEditor.Isolate", new IMAGE_PLUGIN_BRUSH("Icons/Isolate", Icon16x16));
	Style->Set("NiagaraEditor.Module.Pin.TypeSelector", new IMAGE_PLUGIN_BRUSH("Icons/Scratch", Icon16x16, FLinearColor::Gray));
	Style->Set("NiagaraEditor.Module.AddPin", new IMAGE_PLUGIN_BRUSH("Icons/PlusSymbol_12x", Icon12x12, FLinearColor::Gray));
	Style->Set("NiagaraEditor.Module.RemovePin", new IMAGE_PLUGIN_BRUSH("Icons/MinusSymbol_12x", Icon12x12, FLinearColor::Gray));
	Style->Set("NiagaraEditor.Message.CustomNote", new IMAGE_PLUGIN_BRUSH("Icons/icon_custom_note_16x", Icon16x16));
}

void InitOverview(TSharedRef< FSlateStyleSet > Style)
{
	// Overview debug icons
	Style->Set("NiagaraEditor.Overview.DebugActive", new IMAGE_PLUGIN_BRUSH("Icons/OverviewDebugActive", Icon16x16));
	Style->Set("NiagaraEditor.Overview.DebugInactive", new IMAGE_PLUGIN_BRUSH("Icons/OverviewDebugInactive", Icon16x16));
}

void InitEmitterDetails(TSharedRef< FSlateStyleSet > Style)
{
	// Emitter details customization
	Style->Set("NiagaraEditor.MaterialWarningBorder", new BOX_CORE_BRUSH("Common/GroupBorderLight", FMargin(4.0f / 16.0f)));
}

void InitAssetColors(TSharedRef< FSlateStyleSet > Style)
{
	// Asset colors
	Style->Set("NiagaraEditor.AssetColors.System", FLinearColor(1.0f, 0.0f, 0.0f));
	Style->Set("NiagaraEditor.AssetColors.Emitter", FLinearColor(1.0f, 0.3f, 0.0f));
	Style->Set("NiagaraEditor.AssetColors.Script", FLinearColor(1.0f, 1.0f, 0.0f));
	Style->Set("NiagaraEditor.AssetColors.ParameterCollection", FLinearColor(1.0f, 1.0f, 0.3f));
	Style->Set("NiagaraEditor.AssetColors.ParameterCollectionInstance", FLinearColor(1.0f, 1.0f, 0.7f));
	Style->Set("NiagaraEditor.AssetColors.ParameterDefinitions", FLinearColor(0.57f, 0.82f, 0.06f));
	Style->Set("NiagaraEditor.AssetColors.EffectType", FLinearColor(1.f, 1.f, 1.f));
}

void InitThumbnails(TSharedRef< FSlateStyleSet > Style)
{
	// Script factory thumbnails
	Style->Set("NiagaraEditor.Thumbnails.DynamicInputs", new IMAGE_BRUSH("Icons/NiagaraScriptDynamicInputs_64x", Icon64x64));
	Style->Set("NiagaraEditor.Thumbnails.Functions", new IMAGE_BRUSH("Icons/NiagaraScriptFunction_64x", Icon64x64));
	Style->Set("NiagaraEditor.Thumbnails.Modules", new IMAGE_BRUSH("Icons/NiagaraScriptModules_64x", Icon64x64));
}

void InitClassIcon(TSharedRef< FSlateStyleSet > Style)
{
	// Renderer class icons
	Style->Set("ClassIcon.NiagaraSpriteRendererProperties", new IMAGE_PLUGIN_BRUSH("Icons/Renderers/renderer_sprite", Icon16x16));
	Style->Set("ClassIcon.NiagaraMeshRendererProperties", new IMAGE_PLUGIN_BRUSH("Icons/Renderers/renderer_mesh", Icon16x16));
	Style->Set("ClassIcon.NiagaraRibbonRendererProperties", new IMAGE_PLUGIN_BRUSH("Icons/Renderers/renderer_ribbon", Icon16x16));
	Style->Set("ClassIcon.NiagaraLightRendererProperties", new IMAGE_PLUGIN_BRUSH("Icons/Renderers/renderer_light", Icon16x16));
	Style->Set("ClassIcon.NiagaraRendererProperties", new IMAGE_PLUGIN_BRUSH("Icons/Renderers/renderer_default", Icon16x16));
}

void InitStackIcons(TSharedRef< FSlateStyleSet > Style)
{
	//GPU/CPU icons
	Style->Set("NiagaraEditor.Stack.GPUIcon", new IMAGE_PLUGIN_BRUSH("Icons/Simulate_GPU_x40", Icon16x16));
	Style->Set("NiagaraEditor.Stack.CPUIcon", new IMAGE_PLUGIN_BRUSH("Icons/Simulate_CPU_x40", Icon16x16));
}

void InitNiagaraSequence(TSharedRef< FSlateStyleSet > Style)
{
	// Niagara sequence
	Style->Set("NiagaraEditor.NiagaraSequence.DefaultTrackColor", FLinearColor(0, .25f, 0));
}

void InitPlatformSet(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	// Niagara platform set customization
	Style->Set("NiagaraEditor.PlatformSet.DropdownButton", new IMAGE_CORE_BRUSH("Common/ComboArrow", Icon8x8));

	Style->Set("NiagaraEditor.PlatformSet.ButtonText", FTextBlockStyle(NormalText)
		.SetFont(DEFAULT_FONT("Bold", 10))
		.SetColorAndOpacity(FLinearColor(0.72f, 0.72f, 0.72f))
		.SetHighlightColor(FLinearColor(1, 1, 1)));

	// Separator in the action menus
	Style->Set( "MenuSeparator", new BOX_CORE_BRUSH( "Common/Separator", 1/4.0f, FLinearColor(1,1,1,0.2f) ) );
	
	const FString SmallRoundedButtonStart(TEXT("Common/SmallRoundedButtonLeft"));
	const FString SmallRoundedButtonMiddle(TEXT("Common/SmallRoundedButtonCentre"));
	const FString SmallRoundedButtonEnd(TEXT("Common/SmallRoundedButtonRight"));

	Style->Set("NiagaraEditor.Module.Pin.TypeSelector.Button", FButtonStyle()
		.SetNormal(FSlateNoResource())
		.SetPressed(BOX_CORE_BRUSH("Common/Button_Pressed", 8.0f / 32.0f, FStyleColors::PrimaryPress))
		.SetHovered(BOX_CORE_BRUSH("Common/Button_Hovered", 8.0f / 32.0f, FStyleColors::PrimaryHover))
		.SetNormalPadding(FMargin(0, 0, 0, 0))
		.SetPressedPadding(FMargin(0, 0, 0, 0)));
	{
		const FLinearColor NormalColor(0.15, 0.15, 0.15, 1);

		Style->Set("NiagaraEditor.PlatformSet.StartButton", FCheckBoxStyle()
			.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetUncheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), NormalColor))
			.SetUncheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), FStyleColors::PrimaryPress))
			.SetUncheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), FStyleColors::PrimaryHover))
			.SetCheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), FStyleColors::SelectHover))
			.SetCheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), FStyleColors::Select))
			.SetCheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonStart, FMargin(7.f / 16.f), FStyleColors::Select)));

		Style->Set("NiagaraEditor.PlatformSet.MiddleButton", FCheckBoxStyle()
			.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetUncheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), NormalColor))
			.SetUncheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), FStyleColors::PrimaryPress))
			.SetUncheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), FStyleColors::PrimaryHover))
			.SetCheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), FStyleColors::SelectHover))
			.SetCheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), FStyleColors::Select))
			.SetCheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonMiddle, FMargin(7.f / 16.f), FStyleColors::Select)));

		Style->Set("NiagaraEditor.PlatformSet.EndButton", FCheckBoxStyle()
			.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetUncheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), NormalColor))
			.SetUncheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), FStyleColors::SelectHover))
			.SetUncheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), FStyleColors::SelectHover))
			.SetCheckedHoveredImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), FStyleColors::SelectHover))
			.SetCheckedPressedImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), FStyleColors::Select))
			.SetCheckedImage(BOX_CORE_BRUSH(*SmallRoundedButtonEnd, FMargin(7.f / 16.f), FStyleColors::Select)));
	}

	Style->Set("NiagaraEditor.PlatformSet.Include", new IMAGE_CORE_BRUSH("Icons/PlusSymbol_12x", Icon12x12));
	Style->Set("NiagaraEditor.PlatformSet.Exclude", new IMAGE_CORE_BRUSH("Icons/MinusSymbol_12x", Icon12x12));
	Style->Set("NiagaraEditor.PlatformSet.Remove", new IMAGE_CORE_BRUSH("Icons/Cross_12x", Icon12x12));

	const FSlateColor SelectionColor_Inactive = FEditorStyle::GetSlateColor("SelectionColor_Inactive");

	Style->Set("NiagaraEditor.PlatformSet.TreeView", FTableRowStyle()
		.SetEvenRowBackgroundBrush(FSlateNoResource())
		.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetOddRowBackgroundBrush(FSlateNoResource())
		.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetActiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, FStyleColors::Select))
		.SetActiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, FStyleColors::Select))
		.SetInactiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetInactiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive)));

}

void InitDropTarget(TSharedRef< FSlateStyleSet > Style)
{
	Style->Set("NiagaraEditor.DropTarget.BackgroundColor", FLinearColor(1.0f, 1.0f, 1.0f, 0.25f));
	Style->Set("NiagaraEditor.DropTarget.BackgroundColorHover", FLinearColor(1.0f, 1.0f, 1.0f, 0.1f));
	Style->Set("NiagaraEditor.DropTarget.BorderVertical", new IMAGE_PLUGIN_BRUSH("Icons/StackDropTargetBorder_Vertical", FVector2D(2, 8), FLinearColor::White, ESlateBrushTileType::Vertical));
	Style->Set("NiagaraEditor.DropTarget.BorderHorizontal", new IMAGE_PLUGIN_BRUSH("Icons/StackDropTargetBorder_Horizontal", FVector2D(8, 2), FLinearColor::White, ESlateBrushTileType::Horizontal));
}

void InitScriptGraph(TSharedRef< FSlateStyleSet > Style)
{
	Style->Set("NiagaraEditor.ScriptGraph.SearchBorderColor", FLinearColor(.1f, .1f, .1f, 1.f));
}

void InitDebuggerStyle(TSharedRef< FSlateStyleSet > Style)
{
	const FVector2D Icon24x24(24.0f, 24.0f);
	
	Style->Set("NiagaraEditor.Debugger.PlayIcon", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Play", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.SpeedIcon", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Speed", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.PauseIcon", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Pause", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.LoopIcon", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Loop", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.StepIcon", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Step", Icon24x24));

	Style->Set("NiagaraEditor.Debugger.Outliner.Capture", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Capture", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.Outliner.CapturePerf", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Perf_40x", Icon24x24));
	Style->Set("NiagaraEditor.Debugger.Outliner.Filter", new IMAGE_PLUGIN_BRUSH("Icons/Debugger/Filter_24x", Icon24x24));

	const FSlateColor SelectionColor = FEditorStyle::GetSlateColor("SelectionColor");
	const FSlateColor SelectionColor_Pressed = FEditorStyle::GetSlateColor("SelectionColor_Pressed");

	FButtonStyle OutlinerToolBarButton = FButtonStyle()
		.SetNormal(BOX_CORE_BRUSH("Common/FlatButton", FMargin(4 / 16.0f), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)))
		.SetHovered(BOX_CORE_BRUSH("Common/FlatButton", FMargin(4 / 16.0f), SelectionColor))
		.SetPressed(BOX_CORE_BRUSH("Common/FlatButton", FMargin(4 / 16.0f), SelectionColor_Pressed))
		.SetNormalPadding(FMargin(0, 0, 0, 1))
		.SetPressedPadding(FMargin(0, 1, 0, 0));
	Style->Set("NiagaraEditor.Debugger.Outliner.Toolbar", OutlinerToolBarButton);
}

void InitBakerStyle(TSharedRef< FSlateStyleSet > Style)
{
	Style->Set("NiagaraEditor.Baker", new IMAGE_PLUGIN_BRUSH("Icons/Baker/BakerIcon", Icon40x40));
}

void InitCommonColors(TSharedRef< FSlateStyleSet > Style)
{
	Style->Set("NiagaraEditor.CommonColors.System", FLinearColor(FColor(1, 202, 252)));
	Style->Set("NiagaraEditor.CommonColors.Emitter", FLinearColor(FColor(241, 99, 6)));
	Style->Set("NiagaraEditor.CommonColors.Particle", FLinearColor(FColor(131, 218, 9)));
}

void InitToolbar(TSharedRef< FSlateStyleSet > Style)
{
	const FTextBlockStyle NormalText = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");

	// Overview Thumbnail toolbar
	// The ToolbarBuilder requires a specific set of resources with specific names. Do not change names lightly.
	{
		Style->Set( "OverviewStackNodeThumbnailToolBar.Label", FTextBlockStyle(NormalText) .SetFont( DEFAULT_FONT( "Regular", 9 ) ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Background", new FSlateNoResource() );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Icon", new IMAGE_CORE_BRUSH( "Icons/icon_tab_toolbar_16px", Icon16x16 ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Expand", new IMAGE_CORE_BRUSH( "Icons/toolbar_expand_16x", Icon16x16) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.SubMenuIndicator", new IMAGE_CORE_BRUSH( "Common/SubmenuArrow", Icon8x8 ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.SToolBarComboButtonBlock.Padding", FMargin(4.0f,0.0f));
		Style->Set( "OverviewStackNodeThumbnailToolBar.SToolBarButtonBlock.Padding", FMargin(4.0f,0.0f));
		Style->Set( "OverviewStackNodeThumbnailToolBar.SToolBarCheckComboButtonBlock.Padding", FMargin(4.0f,0.0f));
		Style->Set( "OverviewStackNodeThumbnailToolBar.SToolBarButtonBlock.CheckBox.Padding", FMargin(4.0f,0.0f) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.SToolBarComboButtonBlock.ComboButton.Color", FCoreStyle::Get().GetSlateColor("DefaultForeground") );

		Style->Set( "OverviewStackNodeThumbnailToolBar.Block.IndentedPadding", FMargin( 18.0f, 2.0f, 4.0f, 4.0f ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Block.Padding", FMargin( 2.0f, 2.0f, 4.0f, 4.0f ) );

		Style->Set( "OverviewStackNodeThumbnailToolBar.Separator", new FSlateColorBrush( FLinearColor(FColor(96, 96, 96)) ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Separator.Padding", FMargin( 1.f, 0.f, 1.f, 0.f) );

		const FButtonStyle Button = FButtonStyle()
			.SetNormal( BOX_CORE_BRUSH( "Common/Button", FVector2D(32,32), 8.0f/32.0f ) )
			.SetHovered( BOX_CORE_BRUSH( "Common/Button_Hovered", FVector2D(32,32), 8.0f/32.0f ) )
			.SetPressed( BOX_CORE_BRUSH( "Common/Button_Pressed", FVector2D(32,32), 8.0f/32.0f ) )
			.SetNormalPadding( FMargin( 2,2,2,2 ) )
			.SetPressedPadding( FMargin( 2,3,2,1 ) );
		
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button", FButtonStyle(Button)
			.SetNormal ( FSlateNoResource() )
			.SetPressed( BOX_CORE_BRUSH( "Common/RoundedSelection_16x", 4.0f/16.0f, FStyleColors::PrimaryPress ) )
			.SetHovered( BOX_CORE_BRUSH( "Common/RoundedSelection_16x", 4.0f/16.0f, FStyleColors::PrimaryHover ) )
		);
		
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Normal", new FSlateNoResource() );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Pressed", new BOX_CORE_BRUSH( "Common/RoundedSelection_16x", 4.0f/16.0f, FStyleColors::PrimaryPress ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Hovered", new BOX_CORE_BRUSH( "Common/RoundedSelection_16x", 4.0f/16.0f, FStyleColors::PrimaryHover ) );

		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Checked", new BOX_CORE_BRUSH( "Common/RoundedSelection_16x",  4.0f/16.0f, FStyleColors::PrimaryPress ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Checked_Hovered", new BOX_CORE_BRUSH( "Common/RoundedSelection_16x",  4.0f/16.0f, FStyleColors::PrimaryPress ) );
		Style->Set( "OverviewStackNodeThumbnailToolBar.Button.Checked_Pressed", new BOX_CORE_BRUSH( "Common/RoundedSelection_16x",  4.0f/16.0f, FStyleColors::Select ) );

		// The Wrap combo button of the toolbar requires these to be set with these names
		Style->Set("Menu.Background", new BOX_CORE_BRUSH( "Common/GroupBorder", FMargin(4.0f/16.0f) ));
		Style->Set( "Menu.Block.IndentedPadding", FMargin( 18.0f, 2.0f, 4.0f, 4.0f ) );
		Style->Set( "Menu.Block.Padding", FMargin( 2.0f, 2.0f, 4.0f, 4.0f ) );
		Style->Set( "Menu.Label", FTextBlockStyle(NormalText) .SetFont( DEFAULT_FONT( "Regular", 9 ) ) );
	}
}

void InitTabIcons(TSharedRef<FSlateStyleSet> Style)
{
	Style->Set("Tab.Curves", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Curves", Icon16x16));
	Style->Set("Tab.GeneratedCode", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/GeneratedCode", Icon16x16));
	Style->Set("Tab.Log", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Log", Icon16x16));
	Style->Set("Tab.Debugger", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/NiagaraDebugger", Icon16x16));
	Style->Set("Tab.Parameters", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Parameters", Icon16x16));
	Style->Set("Tab.ScratchPad", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/ScratchPad", Icon16x16));
	Style->Set("Tab.ScriptStats", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/ScriptStats", Icon16x16));
	Style->Set("Tab.Settings", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Settings", Icon16x16));
	Style->Set("Tab.Spreadsheet", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Spreadsheet", Icon16x16));
	Style->Set("Tab.SystemOverview", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/SystemOverview", Icon16x16));
	Style->Set("Tab.Timeline", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Timeline", Icon16x16));
	Style->Set("Tab.Viewport", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/Viewport", Icon16x16));
	Style->Set("Tab.VisualEffects", new IMAGE_PLUGIN_BRUSH_SVG("Icons/Tabs/VisualEffects", Icon16x16));
}

void InitOutlinerStyle(TSharedRef< FSlateStyleSet > Style)
{
	const FSlateColor SelectionColor = FEditorStyle::GetSlateColor("SelectionColor");
	const FSlateColor SelectionColor_Inactive = FEditorStyle::GetSlateColor("SelectionColor_Inactive");

	Style->Set("NiagaraEditor.Outliner.WorldItem", FTableRowStyle()
		.SetEvenRowBackgroundBrush(FSlateNoResource())
		.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetOddRowBackgroundBrush(FSlateNoResource())
		.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetActiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetActiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetInactiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetInactiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive)));


	const FLinearColor SystemColor = Style->GetColor("NiagaraEditor.AssetColors.System") * 0.6f;
	const FLinearColor SystemColorEven = SystemColor * 0.85f;
	const FLinearColor SystemColorOdd = SystemColor * 0.7f;
	Style->Set("NiagaraEditor.Outliner.SystemItem", FTableRowStyle()
		.SetEvenRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemColorEven))
		.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemColor))
		.SetOddRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemColorOdd))
		.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemColor))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetActiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetActiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetInactiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetInactiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive)));


	const FLinearColor SystemInstanceColor = Style->GetColor("NiagaraEditor.CommonColors.System") * 0.6f;
	const FLinearColor SystemInstanceColorEven = SystemInstanceColor * 0.85f;
	const FLinearColor SystemInstanceColorOdd = SystemInstanceColor * 0.7f;
	Style->Set("NiagaraEditor.Outliner.ComponentItem", FTableRowStyle()
		.SetEvenRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemInstanceColorEven))
		.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemInstanceColor))
		.SetOddRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemInstanceColorOdd))
		.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SystemInstanceColor))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetActiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetActiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetInactiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetInactiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive)));

	const FLinearColor EmitterInstanceColor = Style->GetColor("NiagaraEditor.CommonColors.Emitter") * 0.6f;
	const FLinearColor EmitterInstanceColorEven = EmitterInstanceColor * 0.85f;
	const FLinearColor EmitterInstanceColorOdd = EmitterInstanceColor * 0.7f;
	Style->Set("NiagaraEditor.Outliner.EmitterItem", FTableRowStyle()
		.SetEvenRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, EmitterInstanceColorEven))
		.SetEvenRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, EmitterInstanceColor))
		.SetOddRowBackgroundBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, EmitterInstanceColorOdd))
		.SetOddRowBackgroundHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, EmitterInstanceColor))
		.SetSelectorFocusedBrush(FSlateNoResource())
		.SetActiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetActiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor))
		.SetInactiveBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive))
		.SetInactiveHoveredBrush(IMAGE_CORE_BRUSH("Common/Selection", Icon8x8, SelectionColor_Inactive)));
}

} // namespace NiagaraEditorStyleImpl

TSharedRef< FSlateStyleSet > FNiagaraEditorStyle::Create()
{
	using namespace NiagaraEditorStyleImpl;

	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("NiagaraEditorStyle"));
	Style->SetContentRoot(FPaths::EngineContentDir() / TEXT("Editor/Slate/Niagara"));

	InitStats(Style);
	InitAssetPicker(Style);
	InitActionMenu(Style);
	InitEmitterHeader(Style);
	InitParameters(Style);
	InitParameterMapView(Style);
	InitCodeView(Style);
	InitSelectedEmitter(Style);
	InitToolbarIcons(Style);
	InitToolbar(Style);
	InitTabIcons(Style);
	InitIcons(Style);
	InitOverview(Style);
	InitEmitterDetails(Style);
	InitAssetColors(Style);
	InitThumbnails(Style);
	InitClassIcon(Style);
	InitStackIcons(Style);
	InitNiagaraSequence(Style);
	InitPlatformSet(Style);
	InitDropTarget(Style);
	InitScriptGraph(Style);
	InitDebuggerStyle(Style);
	InitBakerStyle(Style);
	InitCommonColors(Style);
	InitOutlinerStyle(Style);
	
	return Style;
}

#undef IMAGE_PLUGIN_BRUSH
#undef IMAGE_CORE_BRUSH
#undef IMAGE_BRUSH
#undef BOX_BRUSH
#undef BORDER_BRUSH
#undef BOX_CORE_BRUSH
#undef DEFAULT_FONT

void FNiagaraEditorStyle::ReloadTextures()
{
	FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
}

const ISlateStyle& FNiagaraEditorStyle::Get()
{
	return *NiagaraEditorStyleInstance;
}
