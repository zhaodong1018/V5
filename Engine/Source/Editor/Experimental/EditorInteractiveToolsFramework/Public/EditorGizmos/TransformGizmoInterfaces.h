// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolContextInterfaces.h"
#include "UnrealWidgetFwd.h"
#include "TransformGizmoInterfaces.generated.h"

//
// Interface for the Transform gizmo.
//
UENUM()
enum class EGizmoTransformMode : uint8
{
	None = 0,
	Translate,
	Rotate,
	Scale,
	Max
};


namespace FTransformGizmoUtil
{
	/** Convert UE::Widget::EWidgetMode to ETransformGizmoMode*/
	EGizmoTransformMode GetGizmoMode(UE::Widget::EWidgetMode InWidgetMode);

	/** Convert EEditorGizmoMode to UE::Widget::EWidgetMode*/
	UE::Widget::EWidgetMode GetWidgetMode(EGizmoTransformMode InGizmoMode);
};

UINTERFACE()
class EDITORINTERACTIVETOOLSFRAMEWORK_API UTransformGizmoSource : public UInterface
{
	GENERATED_BODY()
};

/**
 * ITransformGizmoSource is an interface for providing gizmo mode configuration information.
 */
class EDITORINTERACTIVETOOLSFRAMEWORK_API ITransformGizmoSource
{
	GENERATED_BODY()
public:
	/**
	 * Returns the current Editor gizmo mode
	 */
	virtual EGizmoTransformMode GetGizmoMode() const PURE_VIRTUAL(ITransformGizmoSource::GetGizmoMode, return EGizmoTransformMode::None;);

	/**
	 * Returns the current gizmo axes to draw
	 */
	virtual EAxisList::Type GetGizmoAxisToDraw(EGizmoTransformMode InGizmoMode) const PURE_VIRTUAL(ITransformGizmoSource::GetGizmoAxisToDraw, return EAxisList::None;);

	/**
	 * Returns the coordinate system space (world or local) in which to display the gizmo
	 */
	virtual EToolContextCoordinateSystem GetGizmoCoordSystemSpace() const PURE_VIRTUAL(ITransformGizmoSource::GetGizmoCoordSystemSpace, return EToolContextCoordinateSystem::World;);

	/**
	 * Returns a scale factor for the gizmo
	 */
	virtual float GetGizmoScale() const PURE_VIRTUAL(ITransformGizmoSource::GetGizmoScale, return 1.0f;);

	/* 
	 * Returns whether the gizmo should be visible. 
	 */
	virtual bool GetVisible() const PURE_VIRTUAL(ITransformGizmoSource::GetVisible, return false;);

};


UINTERFACE()
class EDITORINTERACTIVETOOLSFRAMEWORK_API UTransformGizmoTarget : public UInterface
{
	GENERATED_BODY()
};

/**
 * ITransformGizmoTarget is an interface for updating transform gizmo related state
 * which matches interfaces provided in the Editor.
 */
class EDITORINTERACTIVETOOLSFRAMEWORK_API ITransformGizmoTarget
{
	GENERATED_BODY()
public:

	/*
	 * Set the current axis when the gizmo is interacting
	 */
	virtual void SetCurrentAxis(EAxisList::Type CurrentAxis) PURE_VIRTUAL(ITransformGizmoTarget::SetCurrentAxis, );
};

