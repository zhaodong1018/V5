// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Tools/LegacyEdModeWidgetHelpers.h"
#include "Tools/Modes.h"
#include "StaticMeshEditorModelingMode.generated.h"

class SBorder;

UCLASS()
class UStaticMeshEditorModelingMode : public UBaseLegacyWidgetEdMode
{
	GENERATED_BODY()

public:

	const static FEditorModeID Id;

protected:

	UStaticMeshEditorModelingMode();

	void Enter() override;

	bool UsesToolkits() const override; 
	void CreateToolkit() override;
	

};
