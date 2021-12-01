// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorGizmos/EditorTransformGizmoSource.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"

EGizmoTransformMode UEditorTransformGizmoSource::GetGizmoMode() const
{
	if (FEditorViewportClient* ViewportClient = GetViewportClient())
	{
		UE::Widget::EWidgetMode WidgetMode = ViewportClient->GetWidgetMode();
		return FTransformGizmoUtil::GetGizmoMode(WidgetMode);
	}
	return EGizmoTransformMode::None;
}

EAxisList::Type UEditorTransformGizmoSource::GetGizmoAxisToDraw(EGizmoTransformMode InGizmoMode) const
{ 
	if (FEditorViewportClient* ViewportClient = GetViewportClient())
	{
		UE::Widget::EWidgetMode WidgetMode = ViewportClient->GetWidgetMode();
		return GetModeTools().GetWidgetAxisToDraw(WidgetMode);
	}
	return EAxisList::None;
}

EToolContextCoordinateSystem UEditorTransformGizmoSource::GetGizmoCoordSystemSpace() const
{
	FEditorViewportClient* ViewportClient = GetViewportClient();
	if (ViewportClient && ViewportClient->GetWidgetCoordSystemSpace() == ECoordSystem::COORD_Local)
	{
		return EToolContextCoordinateSystem::Local;
	}
	else
	{
		return EToolContextCoordinateSystem::World;
	}
}

float UEditorTransformGizmoSource::GetGizmoScale() const
{
	return GetModeTools().GetWidgetScale();
}


bool UEditorTransformGizmoSource::GetVisible() const
{
	if (FEditorViewportClient* ViewportClient = GetViewportClient()) 
	{
		if (GetModeTools().GetShowWidget() && GetModeTools().UsesTransformWidget())
		{
			UE::Widget::EWidgetMode WidgetMode = ViewportClient->GetWidgetMode();
			bool bUseLegacyWidget = (WidgetMode == UE::Widget::WM_TranslateRotateZ || WidgetMode == UE::Widget::WM_2D);
			if (!bUseLegacyWidget)
			{
				static IConsoleVariable* const UseLegacyWidgetCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Gizmos.UseLegacyWidget"));
				if (ensure(UseLegacyWidgetCVar))
				{
					bUseLegacyWidget = UseLegacyWidgetCVar->GetInt() > 0;
				}
			}

			return !bUseLegacyWidget;
		}
	}

	return false;
}

FEditorModeTools& UEditorTransformGizmoSource::GetModeTools() const
{
	return GLevelEditorModeTools();
}

FEditorViewportClient* UEditorTransformGizmoSource::GetViewportClient() const
{
	return GetModeTools().GetFocusedViewportClient();
}