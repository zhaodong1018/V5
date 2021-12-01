// Copyright Epic Games, Inc. All Rights Reserved.

#include "CurveTableEditor.h"
#include "Modules/ModuleManager.h"
#include "Engine/CurveTable.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Layout/Overscroll.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Views/SListView.h"
#include "SPositiveActionButton.h"
#include "EditorStyleSet.h"
#include "Styling/StyleColors.h"
#include "EditorReimportHandler.h"
#include "CurveTableEditorModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "CurveEditor.h" 
#include "SCurveEditorPanel.h"
#include "Tree/SCurveEditorTree.h"
#include "Tree/SCurveEditorTreeSelect.h"
#include "Tree/SCurveEditorTreePin.h"
#include "Tree/ICurveEditorTreeItem.h"
#include "Tree/CurveEditorTreeFilter.h"

#include "Tree/SCurveEditorTreeTextFilter.h"
#include "SSimpleButton.h"

#include "RealCurveModel.h"
#include "RichCurveEditorModel.h"

#include "CurveTableEditorCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

 
#define LOCTEXT_NAMESPACE "CurveTableEditor"

const FName FCurveTableEditor::CurveTableTabId("CurveTableEditor_CurveTable");

struct FCurveTableEditorColumnHeaderData
{
	/** Unique ID used to identify this column */
	FName ColumnId;

	/** Display name of this column */
	FText DisplayName;

	/** The calculated width of this column taking into account the cell data for each row */
	float DesiredColumnWidth;

	/** The evaluated key time **/
	float KeyTime;
};

namespace {

		FName MakeUniqueCurveName( UCurveTable* Table )
		{
				check(Table != nullptr);

				int incr = 0;	
				FName TestName = FName("Curve", incr);

				const TMap<FName, FRealCurve*>& RowMap = Table->GetRowMap();

				while (RowMap.Contains(TestName))
				{
						TestName = FName("Curve", ++incr);
				}

				return TestName;
		}
}

/*
* FCurveTableEditorItem
*
*  FCurveTableEditorItem uses and extends the CurveEditorTreeItem to be used in both our TableView and the CurveEditorTree.
*  The added GenerateTableViewCell handles the table columns unknown to the standard CurveEditorTree.
*
*/ 
class FCurveTableEditorItem : public ICurveEditorTreeItem,  public TSharedFromThis<FCurveTableEditorItem>
{

  	struct CachedKeyInfo
  	{
  		CachedKeyInfo(FKeyHandle& InKeyHandle, FText InDisplayValue) :
  		KeyHandle(InKeyHandle)
  		, DisplayValue(InDisplayValue) {}

  		FKeyHandle KeyHandle;

  		FText DisplayValue;	
  	};

  public: 
	FCurveTableEditorItem (TWeakPtr<FCurveTableEditor> InCurveTableEditor, const FCurveEditorTreeItemID& InTreeID, const FName& InRowId, FCurveTableEditorHandle InRowHandle, const TArray<FCurveTableEditorColumnHeaderDataPtr>& InColumns)
		: CurveTableEditor(InCurveTableEditor)
		, TreeID(InTreeID)
		, RowId(InRowId)
		, RowHandle(InRowHandle)
		, Columns(InColumns)
	{
		DisplayName = FText::FromName(InRowId);

		CacheKeys();
	}

	TSharedPtr<SWidget> GenerateCurveEditorTreeWidget(const FName& InColumnName, TWeakPtr<FCurveEditor> InCurveEditor, FCurveEditorTreeItemID InTreeItemID, const TSharedRef<ITableRow>& InTableRow) override
	{
		if (InColumnName == ColumnNames.Label)
		{
			return SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.Padding(FMargin(4.f))
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.AutoWidth()
				[
					SAssignNew(InlineRenameWidget, SInlineEditableTextBlock)
					.Text(DisplayName)
					.ColorAndOpacity(FSlateColor::UseForeground())
					.OnTextCommitted(this, &FCurveTableEditorItem::HandleNameCommitted)
					.OnVerifyTextChanged(this, &FCurveTableEditorItem::VerifyNameChanged)
				];
		}
		else if (InColumnName == ColumnNames.SelectHeader)
		{
			return SNew(SCurveEditorTreeSelect, InCurveEditor, InTreeItemID, InTableRow);
		}
		else if (InColumnName == ColumnNames.PinHeader)
		{
			return SNew(SCurveEditorTreePin, InCurveEditor, InTreeItemID, InTableRow);
		}

		return GenerateTableViewCell(InColumnName, InCurveEditor, InTreeItemID, InTableRow);
	}

	TSharedPtr<SWidget> GenerateTableViewCell(const FName& InColumnId, TWeakPtr<FCurveEditor> InCurveEditor, FCurveEditorTreeItemID InTreeItemID, const TSharedRef<ITableRow>& InTableRow)
	{
		if (!RowHandle.HasRichCurves())
		{
			FRealCurve* Curve = RowHandle.GetCurve();
			FKeyHandle& KeyHandle = CellDataMap[InColumnId].KeyHandle;

			return SNew(SNumericEntryBox<float>)
				.EditableTextBoxStyle( &FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("CurveTableEditor.Cell.Text") )
				.Value_Lambda([Curve, KeyHandle] () { return Curve->GetKeyValue(KeyHandle); })
				.OnValueChanged_Lambda([this, KeyHandle] (float NewValue) 
				{
					FScopedTransaction Transaction(LOCTEXT("SetKeyValues", "Set Key Values"));
					RowHandle.ModifyOwner();
					RowHandle.GetCurve()->SetKeyValue(KeyHandle, NewValue);
				})
				.Justification(ETextJustify::Right)
			;
		}
		return SNullWidget::NullWidget;
	}

	void CreateCurveModels(TArray<TUniquePtr<FCurveModel>>& OutCurveModels) override
	{
		if (RowHandle.HasRichCurves())
		{
			if (FRichCurve* RichCurve = RowHandle.GetRichCurve())
			{
				const UCurveTable* Table = RowHandle.CurveTable.Get();
				UCurveTable* RawTable = const_cast<UCurveTable*>(Table);

				TUniquePtr<FRichCurveEditorModelRaw> NewCurve = MakeUnique<FRichCurveEditorModelRaw>(RichCurve, RawTable);
				NewCurve->SetShortDisplayName(DisplayName);
				NewCurve->SetColor(FStyleColors::AccentOrange.GetSpecifiedColor());
				OutCurveModels.Add(MoveTemp(NewCurve));
			}
		}
		else
		{
			const UCurveTable* Table = RowHandle.CurveTable.Get();
			UCurveTable* RawTable = const_cast<UCurveTable*>(Table);

			TUniquePtr<FRealCurveModel> NewCurveModel = MakeUnique<FRealCurveModel>(RowHandle.GetCurve(), RawTable);
			NewCurveModel->SetShortDisplayName(DisplayName);

			OutCurveModels.Add(MoveTemp(NewCurveModel));
		}
	}

	bool PassesFilter(const FCurveEditorTreeFilter* InFilter) const override
	{
		if (InFilter->GetType() == ECurveEditorTreeFilterType::Text)
		{
			const FCurveEditorTreeTextFilter* Filter = static_cast<const FCurveEditorTreeTextFilter*>(InFilter);
			for (const FCurveEditorTreeTextFilterTerm& Term : Filter->GetTerms())
			{
				for(const FCurveEditorTreeTextFilterToken& Token : Term.ChildToParentTokens)
				{
					if(Token.Match(*DisplayName.ToString()))
					{
						return true;
					}
				}
			}

			return false;
		}

		return false;
	}

	void CacheKeys()
	{
		if (!RowHandle.HasRichCurves())
		{
			if (FRealCurve* Curve = RowHandle.GetCurve())
			{	
				for (auto Col : Columns)
				{
					FKeyHandle KeyHandle = Curve->FindKey(Col->KeyTime);
					float KeyValue = Curve->GetKeyValue(KeyHandle);

					CellDataMap.Add(Col->ColumnId, CachedKeyInfo(KeyHandle, FText::AsNumber(KeyValue))); 
				}
			}
		}
	}

	void EnterRenameMode()
	{
		InlineRenameWidget->EnterEditingMode();
	}

	bool VerifyNameChanged(const FText& InText, FText& OutErrorMessage)
	{
		FName CheckName = FName(*InText.ToString());
		if (CheckName == RowId)
		{
			return true;	
		}

		if (RowHandle.CurveTable.IsValid())
		{
			UCurveTable* Table = RowHandle.CurveTable.Get();
			const TMap<FName, FRealCurve*>& RowMap = Table->GetRowMap();
			if (RowMap.Contains(CheckName))
			{

				OutErrorMessage = LOCTEXT("NameAlreadyUsed", "Row Names Must Be Unique");
				return false;
			}
			return true;
		}
		return false;
	}

	void HandleNameCommitted(const FText& CommittedText, ETextCommit::Type CommitInfo)
	{
		if (CommitInfo == ETextCommit::OnEnter)
		{
			TSharedPtr<FCurveTableEditor> TableEditorPtr = CurveTableEditor.Pin();
			if (TableEditorPtr != nullptr)
			{
				FName OldName = RowId;
				FName NewName = *CommittedText.ToString();

				DisplayName = CommittedText;
				InlineRenameWidget->SetText(DisplayName);

				RowHandle.RowName = NewName;
				RowId = NewName;

				TableEditorPtr->HandleCurveRename(TreeID, OldName, NewName);

				TSharedPtr<FCurveEditor> CurveEditor = TableEditorPtr->GetCurveEditor();
				FCurveEditorTreeItem& TreeItem = CurveEditor->GetTreeItem(TreeID);
				for (FCurveModelID ModelID : TreeItem.GetCurves())
				{
					if (FCurveModel* CurveModel = CurveEditor->FindCurve(ModelID))
					{
						CurveModel->SetShortDisplayName(DisplayName);
					}
				}
			}
		}
	}

	/** Hold onto a weak ptr to the CurveTableEditor specifically for deleting and renaming  */
	TWeakPtr<FCurveTableEditor> CurveTableEditor;

	/** The CurveEditor's Unique ID for the TreeItem this item is attached to (SetStrongItem) */
	FCurveEditorTreeItemID TreeID;

	/** Unique ID used to identify this row */
	FName RowId;

	/** Display name of this row */
	FText DisplayName;

	/** Array corresponding to each cell in this row */
	TMap<FName, CachedKeyInfo> CellDataMap;

	/** Handle to the row */
	FCurveTableEditorHandle RowHandle;

	/** A Reference to the available columns in the TableView */
	const TArray<FCurveTableEditorColumnHeaderDataPtr>& Columns;

	/** Inline editable text box for renaming */
	TSharedPtr<SInlineEditableTextBlock> InlineRenameWidget;

};


void FCurveTableEditor::RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_CurveTableEditor", "Curve Table Editor"));

	InTabManager->RegisterTabSpawner( CurveTableTabId, FOnSpawnTab::CreateSP(this, &FCurveTableEditor::SpawnTab_CurveTable) )
		.SetDisplayName( LOCTEXT("CurveTableTab", "Curve Table") )
		.SetGroup( WorkspaceMenuCategory.ToSharedRef() );
}


void FCurveTableEditor::UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	InTabManager->UnregisterTabSpawner( CurveTableTabId );
}


FCurveTableEditor::~FCurveTableEditor()
{
	FReimportManager::Instance()->OnPostReimport().RemoveAll(this);
}


void FCurveTableEditor::InitCurveTableEditor( const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UCurveTable* Table )
{
	const TSharedRef< FTabManager::FLayout > StandaloneDefaultLayout = InitCurveTableLayout();

	FAssetEditorToolkit::InitAssetEditor( Mode, InitToolkitHost, FCurveTableEditorModule::CurveTableEditorAppIdentifier, StandaloneDefaultLayout, ShouldCreateDefaultStandaloneMenu(), ShouldCreateDefaultToolbar(), Table );
	
	BindCommands();
	ExtendMenu();
	ExtendToolbar();
	RegenerateMenusAndToolbars();

	FReimportManager::Instance()->OnPostReimport().AddSP(this, &FCurveTableEditor::OnPostReimport);

	GEditor->RegisterForUndo(this);
}

TSharedRef< FTabManager::FLayout > FCurveTableEditor::InitCurveTableLayout()
{
	return FTabManager::NewLayout("Standalone_CurveTableEditor_Layout_v1.1")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split
			(
				FTabManager::NewStack()
				->AddTab(CurveTableTabId, ETabState::OpenedTab)
				->SetHideTabWell(true)
			)
		);
}

void FCurveTableEditor::BindCommands()
{
	FCurveTableEditorCommands::Register();

	ToolkitCommands->MapAction(FGenericCommands::Get().Undo,   FExecuteAction::CreateLambda([]{ GEditor->UndoTransaction(); }));
	ToolkitCommands->MapAction(FGenericCommands::Get().Redo,   FExecuteAction::CreateLambda([]{ GEditor->RedoTransaction(); }));

	ToolkitCommands->MapAction(
		FCurveTableEditorCommands::Get().CurveViewToggle,
		FExecuteAction::CreateSP(this, &FCurveTableEditor::ToggleViewMode),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FCurveTableEditor::IsCurveViewChecked)
	);

	ToolkitCommands->MapAction(
		FCurveTableEditorCommands::Get().AppendKeyColumn,
		FExecuteAction::CreateSP(this, &FCurveTableEditor::OnAddNewKeyColumn)
	);

	ToolkitCommands->MapAction(
		FCurveTableEditorCommands::Get().RenameSelectedCurve,
		FExecuteAction::CreateSP(this, &FCurveTableEditor::OnRenameCurve)
	);


	ToolkitCommands->MapAction(
		FCurveTableEditorCommands::Get().DeleteSelectedCurves,
		FExecuteAction::CreateSP(this, &FCurveTableEditor::OnDeleteCurves)
	);

}

void FCurveTableEditor::ExtendMenu()
{
	MenuExtender = MakeShareable(new FExtender);

	struct Local
	{
		static void ExtendMenu(FMenuBuilder& MenuBuilder)
		{
			MenuBuilder.BeginSection("CurveTableEditor", LOCTEXT("CurveTableEditor", "Curve Table"));
			{
				MenuBuilder.AddMenuEntry(FCurveTableEditorCommands::Get().CurveViewToggle);
			}
			MenuBuilder.EndSection();
			}
	};

	MenuExtender->AddMenuExtension(
		"WindowLayout",
		EExtensionHook::After,
		GetToolkitCommands(),
		FMenuExtensionDelegate::CreateStatic(&Local::ExtendMenu)
	);

	AddMenuExtender(MenuExtender);

	FCurveTableEditorModule& CurveTableEditorModule = FModuleManager::LoadModuleChecked<FCurveTableEditorModule>("CurveTableEditor");
	AddMenuExtender(CurveTableEditorModule.GetMenuExtensibilityManager()->GetAllExtenders(GetToolkitCommands(), GetEditingObjects()));
}

void FCurveTableEditor::ExtendToolbar()
{
	ToolbarExtender = MakeShareable(new FExtender);

	ToolbarExtender->AddToolBarExtension(
		"Asset",
		EExtensionHook::After,
		GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateLambda([this](FToolBarBuilder& ParentToolbarBuilder)
		{

			ParentToolbarBuilder.BeginSection("CurveTable");

			bool HasRichCurves = GetCurveTable()->HasRichCurves();
			ParentToolbarBuilder.AddWidget(
				SNew(SSegmentedControl<ECurveTableViewMode>)
				.Visibility(HasRichCurves ? EVisibility::Collapsed : EVisibility::Visible)
				.OnValueChanged_Lambda([this] (ECurveTableViewMode InMode) {if (InMode != GetViewMode()) ToggleViewMode();  } )
				.Value(this, &FCurveTableEditor::GetViewMode)

				+SSegmentedControl<ECurveTableViewMode>::Slot(ECurveTableViewMode::CurveTable)
			    .Icon(FAppStyle::Get().GetBrush("CurveTableEditor.CurveView"))

				+SSegmentedControl<ECurveTableViewMode>::Slot(ECurveTableViewMode::Grid)
			    .Icon(FAppStyle::Get().GetBrush("CurveTableEditor.TableView"))
			);

			if (InterpMode == RCIM_Constant)
			{
				ParentToolbarBuilder.AddToolBarButton(
					FCurveTableEditorCommands::Get().AppendKeyColumn,
					NAME_None, 
					FText::GetEmpty(),
					TAttribute<FText>(), 
					FSlateIcon(FAppStyle::Get().GetStyleSetName(), "Sequencer.KeySquare")
				);
			}

			if (InterpMode == RCIM_Linear)
			{
				ParentToolbarBuilder.AddToolBarButton(
					FCurveTableEditorCommands::Get().AppendKeyColumn,
					NAME_None, 
					FText::GetEmpty(),
					TAttribute<FText>(), 
					FSlateIcon(FAppStyle::Get().GetStyleSetName(), "Sequencer.KeyTriangle")
				);
			}

			ParentToolbarBuilder.EndSection();
		})
	);

	AddToolbarExtender(ToolbarExtender);

}

FName FCurveTableEditor::GetToolkitFName() const
{
	return FName("CurveTableEditor");
}

FText FCurveTableEditor::GetBaseToolkitName() const
{
	return LOCTEXT( "AppLabel", "CurveTable Editor" );
}

FString FCurveTableEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "CurveTable ").ToString();
}

FLinearColor FCurveTableEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor( 0.0f, 0.0f, 0.2f, 0.5f );
}

void FCurveTableEditor::PreChange(const UCurveTable* Changed, FCurveTableEditorUtils::ECurveTableChangeInfo Info)
{
}

void FCurveTableEditor::PostUndo(bool bSuccess)
{
	RefreshCachedCurveTable();
}

void FCurveTableEditor::PostRedo(bool bSuccess)
{
	RefreshCachedCurveTable();
}

void FCurveTableEditor::PostChange(const UCurveTable* Changed, FCurveTableEditorUtils::ECurveTableChangeInfo Info)
{
	const UCurveTable* Table = GetCurveTable();
	if (Changed == Table)
	{
		HandlePostChange();
	}
}

UCurveTable* FCurveTableEditor::GetCurveTable() const
{
	return Cast<UCurveTable>(GetEditingObject());
}

void FCurveTableEditor::HandlePostChange()
{
	RefreshCachedCurveTable();
}

TSharedRef<SDockTab> FCurveTableEditor::SpawnTab_CurveTable( const FSpawnTabArgs& Args )
{
	check( Args.GetTabId().TabType == CurveTableTabId );

	bUpdatingTableViewSelection = false;

	TSharedRef<SScrollBar> VerticalScrollBar = SNew(SScrollBar)
		.Orientation(Orient_Vertical);

	ColumnNamesHeaderRow = SNew(SHeaderRow)
		.Visibility(this, &FCurveTableEditor::GetTableViewControlsVisibility);

	CurveEditor = MakeShared<FCurveEditor>();

	FCurveEditorInitParams CurveEditorInitParams;
	CurveEditor->InitCurveEditor(CurveEditorInitParams);

	// We want this editor to handle undo, not the CurveEditor because
	// the PostUndo fixes up the selection and in the case of a CurveTable,
	// the curves have been rebuilt on undo and thus need special handling to restore the selection
	GEditor->UnregisterForUndo(CurveEditor.Get());


	CurveEditorTree = SNew(SCurveEditorTree, CurveEditor.ToSharedRef())
		.OnTreeViewScrolled(this, &FCurveTableEditor::OnCurveTreeViewScrolled)
		.OnMouseButtonDoubleClick(this, &FCurveTableEditor::OnRequestCurveRename)
		.OnContextMenuOpening(this, &FCurveTableEditor::OnOpenCurveMenu);

	TSharedRef<SCurveEditorPanel> CurveEditorPanel = SNew(SCurveEditorPanel, CurveEditor.ToSharedRef());

	TableView = SNew(SListView<FCurveEditorTreeItemID>)
		.ListItemsSource(&EmptyItems)
		.OnListViewScrolled(this, &FCurveTableEditor::OnTableViewScrolled)
		.HeaderRow(ColumnNamesHeaderRow)
		.OnGenerateRow(CurveEditorTree.Get(), &SCurveEditorTree::GenerateRow)
		.ExternalScrollbar(VerticalScrollBar)
		.SelectionMode(ESelectionMode::Multi)
		.OnSelectionChanged_Lambda(
			[this](TListTypeTraits<FCurveEditorTreeItemID>::NullableType InItemID, ESelectInfo::Type Type)
			{
				this->OnTableViewSelectionChanged(InItemID, Type);
			}
		);

	CurveEditor->GetTree()->Events.OnItemsChanged.AddSP(this, &FCurveTableEditor::RefreshTableRows);
	CurveEditor->GetTree()->Events.OnSelectionChanged.AddSP(this, &FCurveTableEditor::RefreshTableRowsSelection);

	ViewMode = GetCurveTable()->HasRichCurves() ? ECurveTableViewMode::CurveTable : ECurveTableViewMode::Grid;

	RefreshCachedCurveTable();

	return SNew(SDockTab)
		.Label( LOCTEXT("CurveTableTitle", "Curve Table") )
		.TabColorScale( GetTabColorScale() )
		[
			SNew(SBorder)
			.Padding(2)
			.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				SNew(SVerticalBox)
				+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(8, 0))
				[
					MakeToolbar(CurveEditorPanel)
				]

				+SVerticalBox::Slot()
				[
					SNew(SSplitter)
					+SSplitter::Slot()
					.Value(.2)
					[
						SNew(SVerticalBox)
					
						+SVerticalBox::Slot()
						.Padding(0, 0, 0, 1) // adjusting padding so as to line up the rows in the cell view
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(2.f, 0.f, 4.f, 0.0)
							[
								SNew(SPositiveActionButton)
								.Icon(FAppStyle::Get().GetBrush("Icons.Plus"))
								.Text(LOCTEXT("Curve", "Curve"))
								.OnClicked(this, &FCurveTableEditor::OnAddCurveClicked)
							]

							+SHorizontalBox::Slot()	
							[
								SNew(SCurveEditorTreeTextFilter, CurveEditor)
							]
						]

						+SVerticalBox::Slot()
						[
							CurveEditorTree.ToSharedRef()
						]

					]
					+SSplitter::Slot()
					[

						SNew(SHorizontalBox)
						.Visibility(this, &FCurveTableEditor::GetTableViewControlsVisibility)

						+SHorizontalBox::Slot()
						[
							SNew(SScrollBox)
							.Orientation(Orient_Horizontal)

							+SScrollBox::Slot()
							[
								TableView.ToSharedRef()
							]
						]

						+SHorizontalBox::Slot()
						.AutoWidth()
						[
							VerticalScrollBar
						]
					]

					+SSplitter::Slot()
					[
						SNew(SBox)
						.Visibility(this, &FCurveTableEditor::GetCurveViewControlsVisibility)
						[
							CurveEditorPanel
						]
					]
				]
			]
		];
}

void FCurveTableEditor::RefreshTableRows()
{
	TableView->RequestListRefresh();
}

void FCurveTableEditor::RefreshTableRowsSelection()
{
	if(bUpdatingTableViewSelection == false)
	{
		TGuardValue<bool> SelectionGuard(bUpdatingTableViewSelection, true);

		TArray<FCurveEditorTreeItemID> CurrentTreeWidgetSelection;
		TableView->GetSelectedItems(CurrentTreeWidgetSelection);
		const TMap<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& CurrentCurveEditorTreeSelection = CurveEditor->GetTreeSelection();

		TArray<FCurveEditorTreeItemID> NewTreeWidgetSelection;
		for (const TPair<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& CurveEditorTreeSelectionEntry : CurrentCurveEditorTreeSelection)
		{
			if (CurveEditorTreeSelectionEntry.Value != ECurveEditorTreeSelectionState::None)
			{
				NewTreeWidgetSelection.Add(CurveEditorTreeSelectionEntry.Key);
				CurrentTreeWidgetSelection.RemoveSwap(CurveEditorTreeSelectionEntry.Key);
			}
		}

		TableView->SetItemSelection(CurrentTreeWidgetSelection, false, ESelectInfo::Direct);
		TableView->SetItemSelection(NewTreeWidgetSelection, true, ESelectInfo::Direct);
	}
}

void FCurveTableEditor::OnTableViewSelectionChanged(FCurveEditorTreeItemID ItemID, ESelectInfo::Type)
{
	if (bUpdatingTableViewSelection == false)
	{
		TGuardValue<bool> SelectionGuard(bUpdatingTableViewSelection, true);
		CurveEditor->GetTree()->SetDirectSelection(TableView->GetSelectedItems(), CurveEditor.Get());
	}
}

void FCurveTableEditor::RefreshCachedCurveTable()
{
	// This will trigger to remove any cached widgets in the TableView while we rebuild the model from the source CurveTable

	const TSet<FCurveModelID>& Pinned = CurveEditor->GetPinnedCurves();
	TSet<FName> PinnedCurves;
	for (auto PinnedCurveID : Pinned)
	{
		FCurveEditorTreeItemID TreeID = CurveEditor->GetTreeIDFromCurveID(PinnedCurveID);
		if (RowIDMap.Contains(TreeID))
		{
			PinnedCurves.Add(RowIDMap[TreeID]);
		}
	}

	TSet<FName> SelectedCurves;
	const TMap<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& Selected = CurveEditor->GetTreeSelection();
	for (const TPair<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& SelectionEntry: Selected)
	{
		if (SelectionEntry.Value != ECurveEditorTreeSelectionState::None)
		{
			if (RowIDMap.Contains(SelectionEntry.Key))
			{
				SelectedCurves.Add(RowIDMap[SelectionEntry.Key]);
			}
		}
	}

	// New Selection 
	TArray<FCurveEditorTreeItemID> NewSelectedItems;

	TableView->SetListItemsSource(EmptyItems);
	
	CurveEditor->RemoveAllTreeItems();

	ColumnNamesHeaderRow->ClearColumns();
	AvailableColumns.Empty();
	RowIDMap.Empty();

	UCurveTable* Table = GetCurveTable();
	if (!Table || Table->GetRowMap().Num() == 0)
	{
		return;
	}

	TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FTextBlockStyle& CellTextStyle = FEditorStyle::GetWidgetStyle<FTextBlockStyle>("DataTableEditor.CellText");
	static const float CellPadding = 10.0f;

	if (Table->HasRichCurves())
	{
		InterpMode = RCIM_Cubic;
		for (const TPair<FName, FRichCurve*>& CurveRow : Table->GetRichCurveRowMap())
		{
			// Setup the CurveEdtiorTree
			const FName& CurveName = CurveRow.Key;
			FCurveEditorTreeItem* TreeItem = CurveEditor->AddTreeItem(FCurveEditorTreeItemID());
			TreeItem->SetStrongItem(MakeShared<FCurveTableEditorItem>(SharedThis(this), TreeItem->GetID(), CurveName, FCurveTableEditorHandle(Table, CurveName), AvailableColumns));
			RowIDMap.Add(TreeItem->GetID(), CurveName);

			if (SelectedCurves.Contains(CurveName))
			{
				NewSelectedItems.Add(TreeItem->GetID());
			}

			if (PinnedCurves.Contains(CurveName))
			{
				for (auto ModelID : TreeItem->GetCurves())
				{
					CurveEditor->PinCurve(ModelID);
				}
			}
		}
	}

	else
	{
		// Find unique column titles and setup columns
		TArray<float> UniqueColumns;
		for (const TPair<FName, FRealCurve*>& CurveRow : Table->GetRowMap())
		{
			FRealCurve* Curve = CurveRow.Value;
			for (auto CurveIt(Curve->GetKeyHandleIterator()); CurveIt; ++CurveIt)
			{
				UniqueColumns.AddUnique(Curve->GetKeyTime(*CurveIt));
			}
		}
		UniqueColumns.Sort();
		for (const float& ColumnTime : UniqueColumns)
		{
			const FText ColumnText = FText::AsNumber(ColumnTime);
			FCurveTableEditorColumnHeaderDataPtr CachedColumnData = MakeShareable(new FCurveTableEditorColumnHeaderData());
			CachedColumnData->ColumnId = *ColumnText.ToString();
			CachedColumnData->DisplayName = ColumnText;
			CachedColumnData->DesiredColumnWidth = FontMeasure->Measure(CachedColumnData->DisplayName, CellTextStyle.Font).X + CellPadding;
			CachedColumnData->KeyTime = ColumnTime;

			AvailableColumns.Add(CachedColumnData);

			ColumnNamesHeaderRow->AddColumn(
				SHeaderRow::Column(CachedColumnData->ColumnId)
				.DefaultLabel(CachedColumnData->DisplayName)
				.FixedWidth(CachedColumnData->DesiredColumnWidth + 50)
				.HAlignHeader(HAlign_Center)
			);
		}

		// Setup the CurveEditorTree 

		// Store the default Interpolation Mode
		InterpMode = RCIM_None;
		for (const TPair<FName, FSimpleCurve*>& CurveRow : Table->GetSimpleCurveRowMap())
		{
			if (InterpMode == RCIM_None) 
			{
				InterpMode = CurveRow.Value->GetKeyInterpMode();
			}

			const FName& CurveName = CurveRow.Key;
			FCurveEditorTreeItem* TreeItem = CurveEditor->AddTreeItem(FCurveEditorTreeItemID());
			TSharedPtr<FCurveTableEditorItem> NewItem = MakeShared<FCurveTableEditorItem>(SharedThis(this), TreeItem->GetID(), CurveName, FCurveTableEditorHandle(Table, CurveName), AvailableColumns);
			OnColumnsChanged.AddSP(NewItem.ToSharedRef(), &FCurveTableEditorItem::CacheKeys);
			TreeItem->SetStrongItem(NewItem);
			RowIDMap.Add(TreeItem->GetID(), CurveName);

			if (SelectedCurves.Contains(CurveName))
			{
				NewSelectedItems.Add(TreeItem->GetID());
			}

			if (PinnedCurves.Contains(CurveName))
			{
				for (auto ModelID : TreeItem->GetOrCreateCurves(CurveEditor.Get()))
				{
					CurveEditor->PinCurve(ModelID);
				}
			}
		}
	}

	TableView->SetListItemsSource(CurveEditorTree->GetSourceItems());

	TGuardValue<bool> SelectionGuard(bUpdatingTableViewSelection, true);
	CurveEditor->SetTreeSelection(MoveTemp(NewSelectedItems));

}

void FCurveTableEditor::OnCurveTreeViewScrolled(double InScrollOffset)
{
	// Synchronize the list views
	TableView->SetScrollOffset(InScrollOffset);
}


void FCurveTableEditor::OnTableViewScrolled(double InScrollOffset)
{
	// Synchronize the list views
	CurveEditorTree->SetScrollOffset(InScrollOffset);
}

void FCurveTableEditor::OnPostReimport(UObject* InObject, bool)
{
	const UCurveTable* Table = GetCurveTable();
	if (Table && Table == InObject)
	{
		RefreshCachedCurveTable();
	}
}

EVisibility FCurveTableEditor::GetTableViewControlsVisibility() const
{
	return ViewMode == ECurveTableViewMode::CurveTable ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility FCurveTableEditor::GetCurveViewControlsVisibility() const
{
	return ViewMode == ECurveTableViewMode::Grid ? EVisibility::Collapsed : EVisibility::Visible;
}

void FCurveTableEditor::ToggleViewMode()
{
	ViewMode = (ViewMode == ECurveTableViewMode::CurveTable) ? ECurveTableViewMode::Grid : ECurveTableViewMode::CurveTable;
}

bool FCurveTableEditor::IsCurveViewChecked() const
{
	return (ViewMode == ECurveTableViewMode::CurveTable);
}

TSharedRef<SWidget> FCurveTableEditor::MakeToolbar(TSharedRef<SCurveEditorPanel>& InEditorPanel)
{

	FToolBarBuilder ToolBarBuilder(InEditorPanel->GetCommands(), FMultiBoxCustomization::None, InEditorPanel->GetToolbarExtender(), true);
	ToolBarBuilder.SetStyle(&FAppStyle::Get(), "Sequencer.ToolBar");
	ToolBarBuilder.BeginSection("Asset");
	ToolBarBuilder.EndSection();
	// We just use all of the extenders as our toolbar, we don't have a need to create a separate toolbar.

	bool HasRichCurves = GetCurveTable()->HasRichCurves();

	return SNew(SHorizontalBox)

	+SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	[
		SNew(SBox)
		.Visibility(this, &FCurveTableEditor::GetCurveViewControlsVisibility)
		[
			ToolBarBuilder.MakeWidget()
		]
	];
}

FReply FCurveTableEditor::OnAddCurveClicked()
{
	FScopedTransaction Transaction(LOCTEXT("AddCurve", "Add Curve"));

	UCurveTable* Table = Cast<UCurveTable>(GetEditingObject());
	check(Table != nullptr);

	Table->Modify();
	if (Table->HasRichCurves())
	{
		FName NewCurveUnique = MakeUniqueCurveName(Table);
		FRichCurve& NewCurve = Table->AddRichCurve(NewCurveUnique);
		FCurveEditorTreeItem* TreeItem = CurveEditor->AddTreeItem(FCurveEditorTreeItemID());
		TreeItem->SetStrongItem(MakeShared<FCurveTableEditorItem>(SharedThis(this), TreeItem->GetID(), NewCurveUnique, FCurveTableEditorHandle(Table, NewCurveUnique), AvailableColumns));
		RowIDMap.Add(TreeItem->GetID(), NewCurveUnique);
	}
	else
	{
		FName NewCurveUnique = MakeUniqueCurveName(Table);
		FSimpleCurve& RealCurve = Table->AddSimpleCurve(NewCurveUnique);
		RealCurve.SetKeyInterpMode(InterpMode);

		// Also add a default key for each column 
		for (auto Column : AvailableColumns)
		{
			RealCurve.AddKey(Column->KeyTime, 0.0);
		}

		FCurveEditorTreeItem* TreeItem = CurveEditor->AddTreeItem(FCurveEditorTreeItemID());
		TSharedPtr<FCurveTableEditorItem> NewItem = MakeShared<FCurveTableEditorItem>(SharedThis(this), TreeItem->GetID(), NewCurveUnique, FCurveTableEditorHandle(Table, NewCurveUnique), AvailableColumns);
		OnColumnsChanged.AddSP(NewItem.ToSharedRef(), &FCurveTableEditorItem::CacheKeys);
		TreeItem->SetStrongItem(NewItem);
		RowIDMap.Add(TreeItem->GetID(), NewCurveUnique);

	}

	return FReply::Handled();
}

void FCurveTableEditor::OnAddNewKeyColumn()
{
	UCurveTable* Table = Cast<UCurveTable>(GetEditingObject());
	check(Table != nullptr);

	if (!Table->HasRichCurves())
	{
		// Compute a new keytime based on the last columns 
		float NewKeyTime = 1.0;
		if (AvailableColumns.Num() > 1)
		{
			float LastKeyTime = AvailableColumns[AvailableColumns.Num() - 1]->KeyTime;
			float PrevKeyTime = AvailableColumns[AvailableColumns.Num() - 2]->KeyTime;
			NewKeyTime = 2.*LastKeyTime - PrevKeyTime;
		}
		else if (AvailableColumns.Num() > 0)
		{
			float LastKeyTime = AvailableColumns[AvailableColumns.Num() - 1]->KeyTime;
			NewKeyTime = LastKeyTime + 1;
		}

		AddNewKeyColumn(NewKeyTime);
	}
}

void FCurveTableEditor::AddNewKeyColumn(float NewKeyTime)
{
	UCurveTable* Table = Cast<UCurveTable>(GetEditingObject());
	check(Table != nullptr);

	if (!Table->HasRichCurves())
	{
		FScopedTransaction Transaction(LOCTEXT("AddKeyColumn", "AddKeyColumn"));
		Table->Modify();	

		// Make sure we don't already have a key at this time

		// 1. Add new keys to every curve
		for (const TPair<FName, FRealCurve*>& CurveRow : Table->GetRowMap())
		{
			FRealCurve* Curve = CurveRow.Value;
			Curve->UpdateOrAddKey(NewKeyTime, Curve->Eval(NewKeyTime));
		}

		// 2. Add Column to our Table
		FCurveTableEditorColumnHeaderDataPtr ColumnData = MakeShareable(new FCurveTableEditorColumnHeaderData());
		const FText ColumnText = FText::AsNumber(NewKeyTime);
		ColumnData->ColumnId = *ColumnText.ToString();
		ColumnData->DisplayName = ColumnText;
		// ColumnData->DesiredColumnWidth = FontMeasure->Measure(ColumnData->DisplayName, CellTextStyle.Font).X + CellPadding;
		ColumnData->KeyTime = NewKeyTime;

		AvailableColumns.Add(ColumnData);

		// 3. Let the CurveTreeItems know they need to recache
		OnColumnsChanged.Broadcast();

		// Add the column to the TableView Header Row
		ColumnNamesHeaderRow->AddColumn(
			SHeaderRow::Column(ColumnData->ColumnId)
			.DefaultLabel(ColumnData->DisplayName)
			.FixedWidth(ColumnData->DesiredColumnWidth + 50)
			.HAlignHeader(HAlign_Center)
		);	
	}
}

void FCurveTableEditor::OnRequestCurveRename(FCurveEditorTreeItemID TreeItemId)
{
	const FCurveEditorTreeItem* TreeItem = CurveEditor->FindTreeItem(TreeItemId);
	if (TreeItem != nullptr)
	{
		TSharedPtr<ICurveEditorTreeItem> CurveEditorTreeItem = TreeItem->GetItem();
		if (CurveEditorTreeItem.IsValid())
		{
			TSharedPtr<FCurveTableEditorItem> CurveTableEditorItem = StaticCastSharedPtr<FCurveTableEditorItem>(CurveEditorTreeItem);
			CurveTableEditorItem->EnterRenameMode();
		}
	}
}

void FCurveTableEditor::HandleCurveRename(FCurveEditorTreeItemID& TreeID, FName& CurrentCurve, FName& NewCurveName)
{
	// Update the underlying Curve Data Asset itself 
	UCurveTable* Table = Cast<UCurveTable>(GetEditingObject());
	check(Table != nullptr);

	FScopedTransaction Transaction(LOCTEXT("RenameCurve", "Rename Curve"));
	Table->SetFlags(RF_Transactional);
	Table->Modify();
	Table->RenameRow(CurrentCurve, NewCurveName);

	FPropertyChangedEvent PropertyChangeStruct(nullptr, EPropertyChangeType::ValueSet);
	Table->PostEditChangeProperty(PropertyChangeStruct);

	// Update our internal map of TreeIDs to FNames
	RowIDMap[TreeID] = NewCurveName;

}

void FCurveTableEditor::OnRenameCurve()
{
	const TMap<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& SelectedRows = CurveEditor->GetTreeSelection();
	if (SelectedRows.Num() == 1)
	{
		for (auto Item : SelectedRows)
		{
			OnRequestCurveRename(Item.Key);
		}		
	}
}

void FCurveTableEditor::OnDeleteCurves()
{
	UCurveTable* Table = Cast<UCurveTable>(GetEditingObject());
	check(Table != nullptr);

	const TMap<FCurveEditorTreeItemID, ECurveEditorTreeSelectionState>& SelectedRows = CurveEditor->GetTreeSelection();

	if (SelectedRows.Num() >= 1)
	{
		FScopedTransaction Transaction(LOCTEXT("DeleteCurveRow", "Delete Curve Rows"));
		Table->SetFlags(RF_Transactional);
		Table->Modify();

		for (auto Item : SelectedRows)
		{
			CurveEditor->RemoveTreeItem(Item.Key);

			FName& CurveName = RowIDMap[Item.Key];

			Table->DeleteRow(CurveName);

			RowIDMap.Remove(Item.Key);
		}

		FPropertyChangedEvent PropertyChangeStruct(nullptr, EPropertyChangeType::ValueSet);
		Table->PostEditChangeProperty(PropertyChangeStruct);
	}
}

TSharedPtr<SWidget> FCurveTableEditor::OnOpenCurveMenu()
{
	int32 SelectedRowCount = CurveEditor->GetTreeSelection().Num();
	if (SelectedRowCount > 0)
	{
		FMenuBuilder MenuBuilder(true /*auto close*/, ToolkitCommands);
		MenuBuilder.BeginSection("Edit");
		if (SelectedRowCount == 1)
		{
			MenuBuilder.AddMenuEntry(
				FCurveTableEditorCommands::Get().RenameSelectedCurve,
				NAME_None,
				TAttribute<FText>(),
				TAttribute<FText>(),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit")
			);
		}
		MenuBuilder.AddMenuEntry(
			FCurveTableEditorCommands::Get().DeleteSelectedCurves,
			NAME_None,
			TAttribute<FText>(),
			TAttribute<FText>(),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete")
		);
		MenuBuilder.EndSection();

		return MenuBuilder.MakeWidget();
	}

	return SNullWidget::NullWidget;
}

#undef LOCTEXT_NAMESPACE