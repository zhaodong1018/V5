// Copyright Epic Games, Inc. All Rights Reserved.

#include "BlueprintModes/WidgetDesignerApplicationMode.h"

#include "BlueprintEditorTabs.h"
#include "SBlueprintEditorToolbar.h"
#include "BlueprintEditorSharedTabFactories.h"

#include "WidgetBlueprintEditorToolbar.h"
#include "UMGEditorModule.h"
#include "StatusBarSubsystem.h"
#include "ToolMenus.h"

#include "TabFactory/PaletteTabSummoner.h"
#include "TabFactory/LibraryTabSummoner.h"
#include "TabFactory/HierarchyTabSummoner.h"
#include "TabFactory/BindWidgetTabSummoner.h"
#include "TabFactory/DesignerTabSummoner.h"
#include "TabFactory/DetailsTabSummoner.h"
#include "TabFactory/AnimationTabSummoner.h"
#include "TabFactory/NavigationTabSummoner.h"
#include "BlueprintModes/WidgetBlueprintApplicationModes.h"

#define LOCTEXT_NAMESPACE "WidgetDesignerMode"

/////////////////////////////////////////////////////
// FWidgetDesignerApplicationMode

FWidgetDesignerApplicationMode::FWidgetDesignerApplicationMode(TSharedPtr<FWidgetBlueprintEditor> InWidgetEditor)
	: FWidgetBlueprintApplicationMode(InWidgetEditor, FWidgetBlueprintApplicationModes::DesignerMode)
{
	// Override the default created category here since "Designer Editor" sounds awkward
	WorkspaceMenuCategory = FWorkspaceItem::NewGroup(LOCTEXT("WorkspaceMenu_WidgetDesigner", "Widget Designer"));

	TabLayout = FTabManager::NewLayout("WidgetBlueprintEditor_Designer_Layout_v4_555")
	->AddArea
	(
		FTabManager::NewPrimaryArea()
		->SetOrientation(Orient_Horizontal)
		->Split
		(
			FTabManager::NewSplitter()
			->SetSizeCoefficient( 0.15f )
			->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.5f)
				->SetForegroundTab(FPaletteTabSummoner::TabID)
				->AddTab(FPaletteTabSummoner::TabID, ETabState::OpenedTab)
				->AddTab(FLibraryTabSummoner::TabID, ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.5f)
				->SetForegroundTab(FHierarchyTabSummoner::TabID)
				->AddTab(FHierarchyTabSummoner::TabID, ETabState::OpenedTab)
				->AddTab(FBindWidgetTabSummoner::TabID, ETabState::OpenedTab)
			)
		)
		->Split
		(
			FTabManager::NewSplitter()
			->SetSizeCoefficient(0.85f)
			->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewSplitter()
				->SetSizeCoefficient(0.7f)
				->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewStack()
					->SetHideTabWell(true)
					->SetSizeCoefficient(0.85f)
					->AddTab( FDesignerTabSummoner::TabID, ETabState::OpenedTab )
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.35f)
					->AddTab(FDetailsTabSummoner::TabID, ETabState::OpenedTab)
				)
			)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient( 0.3f )
				->AddTab(FAnimationTabSummoner::TabID, ETabState::ClosedTab)
				->AddTab(FBlueprintEditorTabs::CompilerResultsID, ETabState::ClosedTab)
				->SetForegroundTab(FAnimationTabSummoner::TabID)
			)
		)
	);

	// Add Tab Spawners
	//TabFactories.RegisterFactory(MakeShareable(new FSelectionDetailsSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FDetailsTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FDesignerTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FHierarchyTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FBindWidgetTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FPaletteTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FLibraryTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FAnimationTabSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FCompilerResultsSummoner(InWidgetEditor)));
	TabFactories.RegisterFactory(MakeShareable(new FNavigationTabSummoner(InWidgetEditor)));

	IUMGEditorModule& EditorModule = FModuleManager::GetModuleChecked<IUMGEditorModule>("UMGEditor");
	EditorModule.OnRegisterTabsForEditor().Broadcast(*this, TabFactories);

	//Make sure we start with our existing list of extenders instead of creating a new one
	IUMGEditorModule& UMGEditorModule = FModuleManager::LoadModuleChecked<IUMGEditorModule>("UMGEditor");
	ToolbarExtender = UMGEditorModule.GetToolBarExtensibilityManager()->GetAllExtenders();
	
	InWidgetEditor->GetWidgetToolbarBuilder()->AddWidgetBlueprintEditorModesToolbar(ToolbarExtender);

	if (UToolMenu* Toolbar = InWidgetEditor->RegisterModeToolbarIfUnregistered(GetModeName()))
	{
		InWidgetEditor->GetWidgetToolbarBuilder()->AddWidgetReflector(Toolbar);
		InWidgetEditor->GetToolbarBuilder()->AddCompileToolbar(Toolbar);
		InWidgetEditor->GetToolbarBuilder()->AddDebuggingToolbar(Toolbar);
	}
}

void FWidgetDesignerApplicationMode::RegisterTabFactories(TSharedPtr<FTabManager> InTabManager)
{
	TSharedPtr<FWidgetBlueprintEditor> BP = GetBlueprintEditor();

	BP->RegisterToolbarTab(InTabManager.ToSharedRef());
	BP->PushTabFactories(TabFactories);
}

void FWidgetDesignerApplicationMode::PreDeactivateMode()
{
	//FWidgetBlueprintApplicationMode::PreDeactivateMode();
}

void FWidgetDesignerApplicationMode::PostActivateMode()
{
	//FWidgetBlueprintApplicationMode::PostActivateMode();

	TSharedPtr<FWidgetBlueprintEditor> BP = GetBlueprintEditor();

	FStatusBarDrawer WidgetAnimSequencerDrawer(FAnimationTabSummoner::WidgetAnimSequencerDrawerID);
	WidgetAnimSequencerDrawer.GetDrawerContentDelegate.BindSP(BP.Get(), &FWidgetBlueprintEditor::OnGetWidgetAnimSequencer);
	WidgetAnimSequencerDrawer.OnDrawerOpenedDelegate.BindSP(BP.Get(), &FWidgetBlueprintEditor::OnWidgetAnimSequencerOpened);
	WidgetAnimSequencerDrawer.OnDrawerDismissedDelegate.BindSP(BP.Get(), &FWidgetBlueprintEditor::OnWidgetAnimSequencerDismissed);
	WidgetAnimSequencerDrawer.ButtonText = LOCTEXT("StatusBar_WidgetAnimSequencer", "Animations");
	WidgetAnimSequencerDrawer.ToolTipText = LOCTEXT("StatusBar_WidgetAnimSequencerToolTip", "Opens animation sequencer (Ctrl+Shift+Space Bar).");
	WidgetAnimSequencerDrawer.Icon = FAppStyle::Get().GetBrush("UMGEditor.AnimTabIcon");
	BP->RegisterDrawer(MoveTemp(WidgetAnimSequencerDrawer), 1);

	BP->OnEnteringDesigner();
}

#undef LOCTEXT_NAMESPACE
