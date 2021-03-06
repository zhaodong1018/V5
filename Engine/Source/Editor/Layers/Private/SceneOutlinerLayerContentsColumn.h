// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/SWidget.h"
#include "ActorTreeItem.h"
#include "Widgets/Views/SHeaderRow.h"
#include "ISceneOutlinerColumn.h"
#include "Editor/Layers/Private/LayerViewModel.h"

template<typename ItemType> class STableRow;

/**
 * A custom column for the SceneOutliner which allows the user to remove actors from
 * the contents of a layer with a single click
 */
class FSceneOutlinerLayerContentsColumn : public ISceneOutlinerColumn
{

public:

	/**
	 *	Constructor
	 */
	FSceneOutlinerLayerContentsColumn( const TSharedRef< class FLayerViewModel >& InViewModel );

	virtual ~FSceneOutlinerLayerContentsColumn() {}
	
	static FName GetID();

	//////////////////////////////////////////////////////////////////////////
	// Begin ISceneOutlinerColumn Implementation

	virtual FName GetColumnID() override;

	virtual SHeaderRow::FColumn::FArguments ConstructHeaderRowColumn() override;

	virtual const TSharedRef< SWidget > ConstructRowWidget(FSceneOutlinerTreeItemRef TreeItem, const STableRow<FSceneOutlinerTreeItemPtr>& Row) override;

	// End ISceneOutlinerColumn Implementation
	//////////////////////////////////////////////////////////////////////////

private:
	FReply OnRemoveFromLayerClicked( const TWeakObjectPtr< AActor > Actor );
	
private:

	/**	 */
	const TSharedRef< FLayerViewModel > ViewModel;
};
