// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ConsoleVariablesEditorProjectSettings.generated.h"

UENUM(BlueprintType)
enum class EConsoleVariablesEditorRowDisplayType : uint8
{
	ShowCurrentValue,
	ShowLastEnteredValue
};

UCLASS(config = Engine, defaultconfig)
class CONSOLEVARIABLESEDITOR_API UConsoleVariablesEditorProjectSettings : public UObject
{
	GENERATED_BODY()
public:
	
	UConsoleVariablesEditorProjectSettings(const FObjectInitializer& ObjectInitializer)
	{
		bAddAllChangedConsoleVariablesToCurrentPreset = true;
	}

	/** If true, any console variable changes will be added to the current preset as long as the plugin is loaded. */
	UPROPERTY(Config, EditAnywhere, Category="Console Variables")
	bool bAddAllChangedConsoleVariablesToCurrentPreset;

	/** When a row is unchecked, its associated variable's value will be set to the value recorded when the plugin was loaded.
	 *The value displayed to the user can be configured with this setting, but will not affect the actual applied value.
	 *ShowCurrentValue displays the actual value currently applied to the variable.
	 *ShowLastEnteredValue displays the value that will be applied when the row is checked. */
	UPROPERTY(Config, EditAnywhere, Category="Console Variables")
	EConsoleVariablesEditorRowDisplayType UncheckedRowDisplayType;
};
