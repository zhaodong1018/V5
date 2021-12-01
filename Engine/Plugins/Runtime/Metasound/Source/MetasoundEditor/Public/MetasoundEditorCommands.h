// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "EditorStyleSet.h"

namespace Metasound
{
	namespace Editor
	{
		class FEditorCommands : public TCommands<FEditorCommands>
		{
		public:
			/** Constructor */
			FEditorCommands()
				: TCommands<FEditorCommands>("MetaSoundEditor", NSLOCTEXT("Contexts", "MetaSoundEditor", "MetaSound Graph Editor"), NAME_None, "MetaSoundStyle")
			{
			}

			/** Plays the Metasound */
			TSharedPtr<FUICommandInfo> Play;

			/** Stops the currently playing Metasound */
			TSharedPtr<FUICommandInfo> Stop;

			/** Imports the Metasound from Json */
			TSharedPtr<FUICommandInfo> Import;

			/** Exports the Metasound */
			TSharedPtr<FUICommandInfo> Export;

			/** Plays stops the currently playing Metasound */
			TSharedPtr<FUICommandInfo> TogglePlayback;

			/** Selects the Metasound in the content browser. If referencing Metasound nodes are selected, selects referenced assets instead. */
			TSharedPtr<FUICommandInfo> BrowserSync;

			/** Breaks the node input/output link */
			TSharedPtr<FUICommandInfo> BreakLink;

			/** Adds an input to the node */
			TSharedPtr<FUICommandInfo> AddInput;

			/** Removes an input from the node */
			TSharedPtr<FUICommandInfo> DeleteInput;

			/** Shows the MetasoundSource's specific settings in the Inspector (if of respective type) */
			TSharedPtr<FUICommandInfo> EditSourceSettings;

			/** Shows the Metasound's top-level general object settings in the Inspector */
			TSharedPtr<FUICommandInfo> EditMetasoundSettings;

			// Run updates on selected nodes if required by any subset of selected nodes
			TSharedPtr<FUICommandInfo> UpdateNodes;

			// Converts a MetaSound from a restricted, preset edit state to a fully accessible MetaSound.
			TSharedPtr<FUICommandInfo> ConvertFromPreset;

			/** Initialize commands */
			virtual void RegisterCommands() override;
		};
	} // namespace Editor
} // namespace Metasound