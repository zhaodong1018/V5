// Copyright Epic Games, Inc. All Rights Reserved.

#include "NetworkingProfilerCommands.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

// Insights
#include "Insights/InsightsManager.h"
#include "Insights/InsightsStyle.h"
#include "Insights/NetworkingProfiler/NetworkingProfilerManager.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

#define LOCTEXT_NAMESPACE "FNetworkingProfilerCommands"

////////////////////////////////////////////////////////////////////////////////////////////////////
// FNetworkingProfilerMenuBuilder
////////////////////////////////////////////////////////////////////////////////////////////////////

void FNetworkingProfilerMenuBuilder::AddMenuEntry(FMenuBuilder& MenuBuilder, const TSharedPtr< FUICommandInfo >& UICommandInfo, const FUIAction& UIAction)
{
	MenuBuilder.AddMenuEntry
	(
		UICommandInfo->GetLabel(),
		UICommandInfo->GetDescription(),
		UICommandInfo->GetIcon(),
		UIAction,
		NAME_None,
		UICommandInfo->GetUserInterfaceType()
	);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// FNetworkingProfilerCommands
////////////////////////////////////////////////////////////////////////////////////////////////////

FNetworkingProfilerCommands::FNetworkingProfilerCommands()
	: TCommands<FNetworkingProfilerCommands>(
		TEXT("NetworkingProfilerCommand"), // Context name for fast lookup
		NSLOCTEXT("Contexts", "NetworkingProfilerCommand", "Networking Insights"), // Localized context name for displaying
		NAME_None, // Parent
		FInsightsStyle::GetStyleSetName() // Icon Style Set
	)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// UI_COMMAND takes long for the compiler to optimize
PRAGMA_DISABLE_OPTIMIZATION
void FNetworkingProfilerCommands::RegisterCommands()
{
	UI_COMMAND(TogglePacketViewVisibility, "Packets", "Toggles the visibility of the Packets view", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(TogglePacketContentViewVisibility, "Packet Content", "Toggles the visibility of the Packet Content view", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(ToggleNetStatsViewVisibility, "Net Stats", "Toggles the visibility of the Net Stats view", EUserInterfaceActionType::ToggleButton, FInputChord());
}
PRAGMA_ENABLE_OPTIMIZATION

////////////////////////////////////////////////////////////////////////////////////////////////////
// Toggle Commands
////////////////////////////////////////////////////////////////////////////////////////////////////

#define IMPLEMENT_TOGGLE_COMMAND(CmdName, IsEnabled, SetIsEnabled) \
	\
	void FNetworkingProfilerActionManager::Map_##CmdName##_Global()\
	{\
		This->CommandList->MapAction(This->GetCommands().CmdName, CmdName##_Custom());\
	}\
	\
	const FUIAction FNetworkingProfilerActionManager::CmdName##_Custom() \
	{\
		FUIAction UIAction;\
		UIAction.ExecuteAction = FExecuteAction::CreateRaw(this, &FNetworkingProfilerActionManager::CmdName##_Execute);\
		UIAction.CanExecuteAction = FCanExecuteAction::CreateRaw(this, &FNetworkingProfilerActionManager::CmdName##_CanExecute);\
		UIAction.GetActionCheckState = FGetActionCheckState::CreateRaw(this, &FNetworkingProfilerActionManager::CmdName##_GetCheckState);\
		return UIAction;\
	}\
	\
	void FNetworkingProfilerActionManager::CmdName##_Execute()\
	{\
		const bool b##IsEnabled = !This->IsEnabled();\
		This->SetIsEnabled(b##IsEnabled);\
	}\
	\
	bool FNetworkingProfilerActionManager::CmdName##_CanExecute() const\
	{\
		return FInsightsManager::Get()->GetSession().IsValid();\
	}\
	\
	ECheckBoxState FNetworkingProfilerActionManager::CmdName##_GetCheckState() const\
	{\
		const bool b##IsEnabled = This->IsEnabled();\
		return b##IsEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;\
	}

//IMPLEMENT_TOGGLE_COMMAND(ToggleAAAViewVisibility, IsAAAViewVisible, ShowHideAAAView)

#undef IMPLEMENT_TOGGLE_COMMAND

////////////////////////////////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
