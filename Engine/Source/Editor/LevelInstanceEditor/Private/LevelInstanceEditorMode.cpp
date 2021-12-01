// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelInstanceEditorMode.h"
#include "LevelInstanceEditorModeToolkit.h"
#include "LevelInstanceEditorModeCommands.h"
#include "Editor.h"
#include "Engine/World.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelEditorViewport.h"
#include "LevelEditorActions.h"
#include "EditorModeManager.h"

#define LOCTEXT_NAMESPACE "LevelInstanceEditorMode"

FEditorModeID ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId("EditMode.LevelInstance");

ULevelInstanceEditorMode::ULevelInstanceEditorMode()
	: UEdMode()
{
	Info = FEditorModeInfo(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId,
		LOCTEXT("LevelInstanceEditorModeName", "LevelInstanceEditorMode"),
		FSlateIcon(),
		false);

	bContextRestriction = true;

	FEditorDelegates::PreBeginPIE.AddUObject(this, &ULevelInstanceEditorMode::OnPreBeginPIE);
}

ULevelInstanceEditorMode::~ULevelInstanceEditorMode()
{
}

void ULevelInstanceEditorMode::OnPreBeginPIE(bool bSimulate)
{
	if (GLevelEditorModeTools().IsModeActive(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId))
	{
		GLevelEditorModeTools().DeactivateMode(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId);
	}
}

void ULevelInstanceEditorMode::UpdateEngineShowFlags()
{
	for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
	{
		if (LevelVC && LevelVC->GetWorld())
		{
			if(ULevelInstanceSubsystem* LevelInstanceSubsystem = LevelVC->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
			{
				LevelVC->EngineShowFlags.EditingLevelInstance = !!LevelInstanceSubsystem->GetEditingLevelInstance();
			}
		}
	}
}

void ULevelInstanceEditorMode::Enter()
{
	UEdMode::Enter();

	UpdateEngineShowFlags();
}

void ULevelInstanceEditorMode::Exit()
{
	UEdMode::Exit();
		
	UpdateEngineShowFlags();
}

void ULevelInstanceEditorMode::CreateToolkit()
{
	Toolkit = MakeShared<FLevelInstanceEditorModeToolkit>();
}

void ULevelInstanceEditorMode::BindCommands()
{
	UEdMode::BindCommands();
	const TSharedRef<FUICommandList>& CommandList = Toolkit->GetToolkitCommands();
	const FLevelInstanceEditorModeCommands& Commands = FLevelInstanceEditorModeCommands::Get();

	CommandList->MapAction(
		Commands.ExitMode,
		FExecuteAction::CreateUObject(this, &ULevelInstanceEditorMode::ExitModeCommand));

	CommandList->MapAction(
		Commands.ToggleContextRestriction,
		FExecuteAction::CreateUObject(this, &ULevelInstanceEditorMode::ToggleContextRestrictionCommand),
		FCanExecuteAction(),
		FIsActionChecked::CreateUObject(this, &ULevelInstanceEditorMode::IsContextRestrictionCommandEnabled));
}

bool ULevelInstanceEditorMode::IsSelectionDisallowed(AActor* InActor, bool bInSelection) const
{
	const bool bRestrict = bContextRestriction && bInSelection;

	if (bRestrict)
	{
		if (UWorld* World = InActor->GetWorld())
		{
			if (ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(InActor))
			{
				if (LevelInstanceActor->IsEditing())
				{
					return false;
				}
			}

			if (ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>())
			{
				ALevelInstance* EditingLevelInstance = LevelInstanceSubsystem->GetEditingLevelInstance();
				ALevelInstance* LevelInstance = LevelInstanceSubsystem->GetParentLevelInstance(InActor);

				return EditingLevelInstance != LevelInstance;
			}
		}
	}

	return bRestrict;
}

void ULevelInstanceEditorMode::ExitModeCommand()
{	
	if (FEditorModeTools* Manager = GetModeManager())
	{
		Manager->DeactivateMode(EM_LevelInstanceEditorModeId);
	}
}

void ULevelInstanceEditorMode::ToggleContextRestrictionCommand()
{
	bContextRestriction = !bContextRestriction;
}

bool ULevelInstanceEditorMode::IsContextRestrictionCommandEnabled() const
{
	return bContextRestriction;
}

#undef LOCTEXT_NAMESPACE
