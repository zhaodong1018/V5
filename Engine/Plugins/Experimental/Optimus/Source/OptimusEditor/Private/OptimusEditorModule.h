// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IOptimusEditorModule.h"


class FOptimusComputeComponentBroker;
class FOptimusEditorGraphNodeFactory;
class FOptimusEditorGraphPinFactory;
class IAssetTypeActions;

class FOptimusEditorModule : public IOptimusEditorModule
{
public:
	FOptimusEditorModule();
	
	// IModuleInterface implementations
	void StartupModule() override;
	void ShutdownModule() override;

	// IOptimusEditorModule implementations
	TSharedRef<IOptimusEditor> CreateEditor(
		const EToolkitMode::Type Mode, 
		const TSharedPtr<IToolkitHost>& InitToolkitHost, 
		UOptimusDeformer* DeformerObject
	) override;

	FOptimusEditorClipboard& GetClipboard() const override;

private:
	void RegisterPropertyCustomizations();
	void UnregisterPropertyCustomizations();

	TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;

	TSharedPtr<FOptimusComputeComponentBroker> ComputeGraphComponentBroker;
	
	TSharedPtr<FOptimusEditorGraphNodeFactory> GraphNodeFactory;
	TSharedPtr<FOptimusEditorGraphPinFactory> GraphPinFactory;

	TArray<FName> CustomizedProperties;

	TSharedRef<FOptimusEditorClipboard> Clipboard;
};
