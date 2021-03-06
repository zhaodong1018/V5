// Copyright Epic Games, Inc. All Rights Reserved.

#include "SequencerCommonHelpers.h"
#include "SequencerSelectedKey.h"
#include "DisplayNodes/SequencerSectionKeyAreaNode.h"
#include "DisplayNodes/SequencerObjectBindingNode.h"
#include "DisplayNodes/SequencerTrackNode.h"
#include "Sequencer.h"
#include "SSequencer.h"
#include "ISequencerHotspot.h"
#include "SSequencerSection.h"
#include "SSequencerTreeView.h"
#include "VirtualTrackArea.h"
#include "SequencerContextMenus.h"
#include "IDetailsView.h"
#include "IStructureDetailsView.h"
#include "FrameNumberDetailsCustomization.h"
#include "MovieSceneSectionDetailsCustomization.h"
#include "MovieSceneSequence.h"
#include "MovieScene.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/CoreStyle.h"

void SequencerHelpers::GetAllKeyAreas(TSharedPtr<FSequencerDisplayNode> DisplayNode, TSet<TSharedPtr<IKeyArea>>& KeyAreas)
{
	TArray<TSharedPtr<FSequencerDisplayNode>> NodesToCheck;
	NodesToCheck.Add(DisplayNode);
	while (NodesToCheck.Num() > 0)
	{
		TSharedPtr<FSequencerDisplayNode> NodeToCheck = NodesToCheck[0];
		NodesToCheck.RemoveAt(0);

		if (NodeToCheck->GetType() == ESequencerNode::Track)
		{
			TSharedPtr<FSequencerTrackNode> TrackNode = StaticCastSharedPtr<FSequencerTrackNode>(NodeToCheck);
			TArray<TSharedRef<FSequencerSectionKeyAreaNode>> KeyAreaNodes;
			TrackNode->GetChildKeyAreaNodesRecursively(KeyAreaNodes);
			for (TSharedRef<FSequencerSectionKeyAreaNode> KeyAreaNode : KeyAreaNodes)
			{
				for (TSharedPtr<IKeyArea> KeyArea : KeyAreaNode->GetAllKeyAreas())
				{
					KeyAreas.Add(KeyArea);
				}
			}
		}
		else
		{
			if (NodeToCheck->GetType() == ESequencerNode::KeyArea)
			{
				TSharedPtr<FSequencerSectionKeyAreaNode> KeyAreaNode = StaticCastSharedPtr<FSequencerSectionKeyAreaNode>(NodeToCheck);
				for (TSharedPtr<IKeyArea> KeyArea : KeyAreaNode->GetAllKeyAreas())
				{
					KeyAreas.Add(KeyArea);
				}
			}
			for (TSharedRef<FSequencerDisplayNode> ChildNode : NodeToCheck->GetChildNodes())
			{
				NodesToCheck.Add(ChildNode);
			}
		}
	}
}

int32 SequencerHelpers::GetSectionFromTime(TArrayView<UMovieSceneSection* const> InSections, FFrameNumber Time)
{
	FFrameNumber ClosestLowerBound = TNumericLimits<int32>::Max();
	TOptional<int32> MaxOverlapPriority, MaxProximalPriority;

	int32 MostRelevantIndex = INDEX_NONE;

	for (int32 Index = 0; Index < InSections.Num(); ++Index)
	{
		const UMovieSceneSection* Section = InSections[Index];
		if (Section)
		{
			const int32 ThisSectionPriority = Section->GetOverlapPriority();
			TRange<FFrameNumber> SectionRange = Section->GetRange();

			// If the specified time is within the section bounds
			if (SectionRange.Contains(Time))
			{
				if (ThisSectionPriority >= MaxOverlapPriority.Get(ThisSectionPriority))
				{
					MaxOverlapPriority = ThisSectionPriority;
					MostRelevantIndex = Index;
				}
			}
			// Check for nearby sections if there is nothing overlapping
			else if (!MaxOverlapPriority.IsSet() && SectionRange.HasLowerBound())
			{
				const FFrameNumber LowerBoundValue = SectionRange.GetLowerBoundValue();
				// If this section exists beyond the current time, we can choose it if its closest to the time
				if (LowerBoundValue >= Time)
				{
					if (
						(LowerBoundValue < ClosestLowerBound) ||
						(LowerBoundValue == ClosestLowerBound && ThisSectionPriority >= MaxProximalPriority.Get(ThisSectionPriority))
						)
					{
						MostRelevantIndex = Index;
						ClosestLowerBound = LowerBoundValue;
						MaxProximalPriority = ThisSectionPriority;
					}
				}
			}
		}
	}

	// If we didn't find one, use the last one (or return -1)
	if (MostRelevantIndex == -1)
	{
		MostRelevantIndex = InSections.Num() - 1;
	}

	return MostRelevantIndex;
}

void SequencerHelpers::GetDescendantNodes(TSharedRef<FSequencerDisplayNode> DisplayNode, TSet<TSharedRef<FSequencerDisplayNode>>& Nodes)
{
	for (auto ChildNode : DisplayNode.Get().GetChildNodes())
	{
		Nodes.Add(ChildNode);

		GetDescendantNodes(ChildNode, Nodes);
	}
}

void SequencerHelpers::GetAllSections(TSharedRef<FSequencerDisplayNode> DisplayNode, TSet<TWeakObjectPtr<UMovieSceneSection>>& Sections)
{
	TSet<TSharedRef<FSequencerDisplayNode> > AllNodes;
	AllNodes.Add(DisplayNode);
	GetDescendantNodes(DisplayNode, AllNodes);

	for (auto NodeToCheck : AllNodes)
	{
		TSet<TSharedPtr<IKeyArea> > KeyAreas;
		GetAllKeyAreas(NodeToCheck, KeyAreas);
		
		for (auto KeyArea : KeyAreas)
		{
			UMovieSceneSection* OwningSection = KeyArea->GetOwningSection();
			if (OwningSection != nullptr)
			{
				Sections.Add(OwningSection);	
			}
		}

		if (NodeToCheck->GetType() == ESequencerNode::Track)
		{
			TSharedRef<const FSequencerTrackNode> TrackNode = StaticCastSharedRef<const FSequencerTrackNode>( NodeToCheck );
			UMovieSceneTrack* Track = TrackNode->GetTrack();
			if (Track != nullptr)
			{
				for (TSharedRef<ISequencerSection> TrackSection : TrackNode->GetSections())
				{
					if (UMovieSceneSection* Section = TrackSection->GetSectionObject())
					{
						Sections.Add(Section);
					}
				}
			}
		}
	}
}

bool IsSectionSelectedInNode(FSequencer& Sequencer, TSharedRef<FSequencerDisplayNode> InNode)
{
	if (InNode->GetType() == ESequencerNode::Track)
	{
		TSharedRef<FSequencerTrackNode> TrackNode = StaticCastSharedRef<FSequencerTrackNode>(InNode);

		for (auto Section : TrackNode->GetSections())
		{
			if (Sequencer.GetSelection().IsSelected(Section->GetSectionObject()))
			{
				return true;
			}
		}
	}
	return false;
}

bool AreKeysSelectedInNode(FSequencer& Sequencer, TSharedRef<FSequencerDisplayNode> InNode)
{
	TSet<TSharedPtr<IKeyArea>> KeyAreas;
	SequencerHelpers::GetAllKeyAreas(InNode, KeyAreas);

	for (const FSequencerSelectedKey& Key : Sequencer.GetSelection().GetSelectedKeys())
	{
		if (KeyAreas.Contains(Key.KeyArea))
		{
			return true;
		}
	}

	return false;
}

void SequencerHelpers::ValidateNodesWithSelectedKeysOrSections(FSequencer& Sequencer)
{
	TArray<TSharedRef<FSequencerDisplayNode>> NodesToRemove;

	for (auto Node : Sequencer.GetSelection().GetNodesWithSelectedKeysOrSections())
	{
		if (!IsSectionSelectedInNode(Sequencer, Node) && !AreKeysSelectedInNode(Sequencer, Node))
		{
			NodesToRemove.Add(Node);
		}
	}

	for (auto Node : NodesToRemove)
	{
		Sequencer.GetSelection().RemoveFromNodesWithSelectedKeysOrSections(Node);
	}
}

void SequencerHelpers::UpdateHoveredNodeFromSelectedSections(FSequencer& Sequencer)
{
	FSequencerSelection& Selection = Sequencer.GetSelection();

	TSharedRef<SSequencer> SequencerWidget = StaticCastSharedRef<SSequencer>(Sequencer.GetSequencerWidget());
	TSharedPtr<FSequencerDisplayNode> HoveredNode = SequencerWidget->GetTreeView()->GetNodeTree()->GetHoveredNode();
	if (!HoveredNode.IsValid())
	{
		return;
	}

	if (IsSectionSelectedInNode(Sequencer, HoveredNode.ToSharedRef()))
	{
		Selection.AddToNodesWithSelectedKeysOrSections(HoveredNode.ToSharedRef());
	}
	else
	{
		Selection.RemoveFromNodesWithSelectedKeysOrSections(HoveredNode.ToSharedRef());
	}
}


void SequencerHelpers::UpdateHoveredNodeFromSelectedKeys(FSequencer& Sequencer)
{
	FSequencerSelection& Selection = Sequencer.GetSelection();

	TSharedRef<SSequencer> SequencerWidget = StaticCastSharedRef<SSequencer>(Sequencer.GetSequencerWidget());
	TSharedPtr<FSequencerDisplayNode> HoveredNode = SequencerWidget->GetTreeView()->GetNodeTree()->GetHoveredNode();
	if (!HoveredNode.IsValid())
	{
		return;
	}

	if (AreKeysSelectedInNode(Sequencer, HoveredNode.ToSharedRef()))
	{
		Selection.AddToNodesWithSelectedKeysOrSections(HoveredNode.ToSharedRef());
	}
	else
	{
		Selection.RemoveFromNodesWithSelectedKeysOrSections(HoveredNode.ToSharedRef());
	}
}


void SequencerHelpers::PerformDefaultSelection(FSequencer& Sequencer, const FPointerEvent& MouseEvent)
{
	FSequencerSelection& Selection = Sequencer.GetSelection();
	Selection.SuspendBroadcast();

	// @todo: selection in transactions
	auto ConditionallyClearSelection = [&]{
		if (!MouseEvent.IsShiftDown() && !MouseEvent.IsControlDown())
		{			
			Selection.EmptySelectedSections();
			Selection.EmptySelectedKeys();
			Selection.EmptyNodesWithSelectedKeysOrSections();
		}
	};

	TSharedPtr<ISequencerHotspot> Hotspot = Sequencer.GetHotspot();
	if (!Hotspot.IsValid())
	{
		ConditionallyClearSelection();
		Selection.ResumeBroadcast();
		Selection.GetOnOutlinerNodeSelectionChanged().Broadcast();
		return;
	}

	// Handle right-click selection separately since we never deselect on right click (except for clearing on exclusive selection)
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		if (Hotspot->GetType() == ESequencerHotspot::Key)
		{
			bool bHasClearedSelection = false;
			for (const FSequencerSelectedKey& Key : static_cast<FKeyHotspot*>(Hotspot.Get())->Keys)
			{
				if (!Selection.IsSelected(Key))
				{
					if (!bHasClearedSelection)
					{
						ConditionallyClearSelection();
						bHasClearedSelection = true;
					}
					Selection.AddToSelection(Key);
				}
			}
		}
		else if (Hotspot->GetType() == ESequencerHotspot::Section || Hotspot->GetType() == ESequencerHotspot::EasingArea)
		{
			UMovieSceneSection* Section = static_cast<FSectionHotspot*>(Hotspot.Get())->WeakSection.Get();
			if (!Selection.IsSelected(Section))
			{
				ConditionallyClearSelection();
				Selection.AddToSelection(Section);
			}
		}
		else if (Hotspot->GetType() == ESequencerHotspot::SectionResize_L || Hotspot->GetType() == ESequencerHotspot::SectionResize_R)
		{
			UMovieSceneSection* Section = static_cast<FSectionResizeHotspot*>(Hotspot.Get())->WeakSection.Get();
			if (!Selection.IsSelected(Section))
			{
				ConditionallyClearSelection();
				Selection.AddToSelection(Section);
			}
		}

		if (Hotspot->GetType() == ESequencerHotspot::Key)
		{
			UpdateHoveredNodeFromSelectedKeys(Sequencer);
		}
		else
		{
			UpdateHoveredNodeFromSelectedSections(Sequencer);
		}
		
		Selection.ResumeBroadcast();
		Selection.GetOnOutlinerNodeSelectionChanged().Broadcast();
		return;
	}

	// Normal selection
	ConditionallyClearSelection();

	bool bForceSelect = !MouseEvent.IsControlDown();
		
	if (Hotspot->GetType() == ESequencerHotspot::Key)
	{
		for (const FSequencerSelectedKey& Key : static_cast<FKeyHotspot*>(Hotspot.Get())->Keys)
		{
			if (bForceSelect || !Selection.IsSelected(Key))
			{
				Selection.AddToSelection(Key);
			}
			else
			{
				Selection.RemoveFromSelection(Key);
			}
		}
	}
	else if (Hotspot->GetType() == ESequencerHotspot::Section || Hotspot->GetType() == ESequencerHotspot::EasingArea)
	{
		UMovieSceneSection* Section = static_cast<FSectionHotspot*>(Hotspot.Get())->WeakSection.Get();

		// Never allow infinite sections to be selected through normal click (they're only selectable through right click)
		if (Section->GetRange() != TRange<FFrameNumber>::All())
		{
			if (bForceSelect || !Selection.IsSelected(Section))
			{
				Selection.AddToSelection(Section);
			}
			else
			{
				Selection.RemoveFromSelection(Section);
			}
		}
	}

	if (Hotspot->GetType() == ESequencerHotspot::Key)
	{
		UpdateHoveredNodeFromSelectedKeys(Sequencer);
	}
	else
	{
		UpdateHoveredNodeFromSelectedSections(Sequencer);
	}

	Selection.ResumeBroadcast();
	Selection.GetOnOutlinerNodeSelectionChanged().Broadcast();
}

TSharedPtr<SWidget> SequencerHelpers::SummonContextMenu(FSequencer& Sequencer, const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// @todo sequencer replace with UI Commands instead of faking it

	// Attempt to paste into either the current node selection, or the clicked on track
	TSharedRef<SSequencer> SequencerWidget = StaticCastSharedRef<SSequencer>(Sequencer.GetSequencerWidget());
	const FFrameNumber PasteAtTime = Sequencer.GetLocalTime().Time.FrameNumber;

	// The menu are generated through reflection and sometime the API exposes some recursivity (think about a Widget returning it parent which is also a Widget). Just by reflection
	// it is not possible to determine when the root object is reached. It needs a kind of simulation which is not implemented. Also, even if the recursivity was correctly handled, the possible
	// permutations tend to grow exponentially. Until a clever solution is found, the simple approach is to disable recursively searching those menus. User can still search the current one though.
	// See UE-131257
	const bool bInRecursivelySearchable = false;

	const bool bShouldCloseWindowAfterMenuSelection = true;
	FMenuBuilder MenuBuilder(bShouldCloseWindowAfterMenuSelection, Sequencer.GetCommandBindings(), nullptr, false, &FCoreStyle::Get(), true, NAME_None, bInRecursivelySearchable);

	TSharedPtr<ISequencerHotspot> Hotspot = Sequencer.GetHotspot();

	if (Hotspot.IsValid() && Hotspot->PopulateContextMenu(MenuBuilder, Sequencer, PasteAtTime))
	{
		return MenuBuilder.MakeWidget();
	}
	else if (Sequencer.GetClipboardStack().Num() != 0)
	{
		TSharedPtr<FPasteContextMenu> PasteMenu = FPasteContextMenu::CreateMenu(Sequencer, SequencerWidget->GeneratePasteArgs(PasteAtTime));
		if (PasteMenu.IsValid() && PasteMenu->IsValidPaste())
		{
			PasteMenu->PopulateMenu(MenuBuilder);

			return MenuBuilder.MakeWidget();
		}
	}

	return nullptr;
}

/** A widget which wraps the section details view which is an FNotifyHook which is used to forward
	changes to the section to sequencer. */
class SSectionDetailsNotifyHookWrapper : public SCompoundWidget, public FNotifyHook
{
public:
	SLATE_BEGIN_ARGS(SSectionDetailsNotifyHookWrapper) {}
	SLATE_END_ARGS();

	void Construct(FArguments InArgs) { }

	void SetDetailsAndSequencer(TSharedRef<SWidget> InDetailsPanel, TSharedRef<ISequencer> InSequencer)
	{
		ChildSlot
		[
			InDetailsPanel
		];
		Sequencer = InSequencer;
	}

	//~ FNotifyHook interface
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override
	{
		Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
	}

private:
	TSharedPtr<ISequencer> Sequencer;
};


void SequencerHelpers::AddPropertiesMenu(FSequencer& Sequencer, FMenuBuilder& MenuBuilder, const TArray<TWeakObjectPtr<UObject>>& Sections)
{
	TSharedRef<SSectionDetailsNotifyHookWrapper> DetailsNotifyWrapper = SNew(SSectionDetailsNotifyHookWrapper);
	FDetailsViewArgs DetailsViewArgs;
	{
		DetailsViewArgs.bAllowSearch = false;
		DetailsViewArgs.bCustomFilterAreaLocation = true;
		DetailsViewArgs.bCustomNameAreaLocation = true;
		DetailsViewArgs.bHideSelectionTip = true;
		DetailsViewArgs.bLockable = false;
		DetailsViewArgs.bSearchInitialKeyFocus = true;
		DetailsViewArgs.bUpdatesFromSelection = false;
		DetailsViewArgs.bShowOptions = false;
		DetailsViewArgs.bShowModifiedPropertiesOption = false;
		DetailsViewArgs.NotifyHook = &DetailsNotifyWrapper.Get();
		DetailsViewArgs.ColumnWidth = 0.45f;
	}

	// We pass the current scene to the UMovieSceneSection customization so we can get the overall bounds of the section when we change a section from infinite->bounded.
	UMovieScene* CurrentScene = Sequencer.GetFocusedMovieSceneSequence()->GetMovieScene();

	TSharedRef<INumericTypeInterface<double>> NumericTypeInterface = Sequencer.GetNumericTypeInterface();

	TSharedRef<IDetailsView> DetailsView = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateDetailView(DetailsViewArgs);
	DetailsView->RegisterInstancedCustomPropertyTypeLayout("FrameNumber", FOnGetPropertyTypeCustomizationInstance::CreateLambda([=]() {
		return MakeShared<FFrameNumberDetailsCustomization>(NumericTypeInterface); }));
	DetailsView->RegisterInstancedCustomPropertyLayout(UMovieSceneSection::StaticClass(), FOnGetDetailCustomizationInstance::CreateLambda([=]() {
		return MakeShared<FMovieSceneSectionDetailsCustomization>(NumericTypeInterface, CurrentScene); }));

	// Let section interfaces further customize the properties details view.
	TSharedRef<FSequencerNodeTree> SequencerNodeTree = Sequencer.GetNodeTree();
	for (TWeakObjectPtr<UObject> Section : Sections)
	{
		if (Section.IsValid())
		{
			TOptional<FSectionHandle> SectionHandle = SequencerNodeTree->GetSectionHandle(Cast<UMovieSceneSection>(Section));
			if (SectionHandle)
			{
				TSharedRef<ISequencerSection> SectionInterface = SectionHandle->GetSectionInterface();
				FSequencerSectionPropertyDetailsViewCustomizationParams CustomizationDetails(
					SectionInterface, Sequencer.AsShared(), SectionHandle->GetTrackNode()->GetTrackEditor());
				TSharedPtr<FSequencerObjectBindingNode> ParentObjectBindingNode = SectionHandle->GetTrackNode()->FindParentObjectBindingNode();
				if (ParentObjectBindingNode.IsValid())
				{
					CustomizationDetails.ParentObjectBindingGuid = ParentObjectBindingNode->GetObjectBinding();
				}
				SectionInterface->CustomizePropertiesDetailsView(DetailsView, CustomizationDetails);
			}
		}
	}

	Sequencer.OnInitializeDetailsPanel().Broadcast(DetailsView, Sequencer.AsShared());
	DetailsView->SetObjects(Sections);

	DetailsNotifyWrapper->SetDetailsAndSequencer(DetailsView, Sequencer.AsShared());
	MenuBuilder.AddWidget(DetailsNotifyWrapper, FText::GetEmpty(), true);
}
