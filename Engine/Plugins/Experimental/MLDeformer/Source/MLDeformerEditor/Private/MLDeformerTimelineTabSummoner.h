// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WorkflowOrientedApp/WorkflowTabFactory.h"

class FMLDeformerEditorToolkit;
class IDetailsView;

struct FMLDeformerTimelineTabSummoner : public FWorkflowTabFactory
{
public:
	static const FName TabID;

	FMLDeformerTimelineTabSummoner(const TSharedRef<FMLDeformerEditorToolkit>& InEditor);
	virtual TSharedRef<SWidget> CreateTabBody(const FWorkflowTabSpawnInfo& Info) const override;
	virtual TSharedPtr<SToolTip> CreateTabToolTipWidget(const FWorkflowTabSpawnInfo& Info) const override;

protected:
	TWeakPtr<FMLDeformerEditorToolkit> Editor;
	TSharedPtr<IDetailsView> DetailsView;
};
