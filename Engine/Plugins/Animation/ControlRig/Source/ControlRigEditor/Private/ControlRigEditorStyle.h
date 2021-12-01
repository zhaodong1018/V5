// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateRoundedBoxBrush.h"

#define TTF_FONT(RelativePath, ...) FSlateFontInfo(RootToContentDir(RelativePath, TEXT(".ttf")), __VA_ARGS__)
#define OTF_FONT(RelativePath, ...) FSlateFontInfo(RootToContentDir(RelativePath, TEXT(".otf")), __VA_ARGS__)

class FControlRigEditorStyle final
	: public FSlateStyleSet
{
public:
	FControlRigEditorStyle()
		: FSlateStyleSet("ControlRigEditorStyle")
	{
		const FVector2D Icon10x10(10.0f, 10.0f);
		const FVector2D Icon14x14(14.0f, 14.0f);
		const FVector2D Icon16x16(16.0f, 16.0f);
		const FVector2D Icon20x20(20.0f, 20.0f);
		const FVector2D Icon24x24(24.0f, 24.0f);
		const FVector2D Icon32x32(32.0f, 32.0f);
		const FVector2D Icon40x40(40.0f, 40.0f);
		const FString PluginContentDir = FPaths::EnginePluginsDir() / TEXT("Animation/ControlRig/Content");
		const FString EngineEditorSlateDir = FPaths::EngineContentDir() / TEXT("Editor/Slate");
		SetContentRoot(PluginContentDir);

		const FSlateColor DefaultForeground(FLinearColor(0.72f, 0.72f, 0.72f, 1.f));

		// Class Icons
		{
			Set("ClassIcon.ControlRigSequence", new IMAGE_BRUSH("Slate/ControlRigSequence_16x", Icon16x16));
			Set("ClassIcon.ControlRigBlueprint", new IMAGE_BRUSH("Slate/ControlRigBlueprint_16x", Icon16x16));
		}

		// Edit mode styles
		Set("ControlRigEditMode", new IMAGE_BRUSH_SVG("Slate/animation", Icon40x40));
		Set("ControlRigEditMode.Small", new IMAGE_BRUSH_SVG("Slate/animation", Icon20x20));

		// Sequencer styles
		{
			Set("ControlRig.ExportAnimSequence", new IMAGE_BRUSH("Slate/ExportAnimSequence_24x", Icon24x24));
			Set("ControlRig.ExportAnimSequence.Small", new IMAGE_BRUSH("Slate/ExportAnimSequence_24x", Icon24x24));
			Set("ControlRig.ReExportAnimSequence", new IMAGE_BRUSH("Slate/ExportAnimSequence_24x", Icon24x24));
			Set("ControlRig.ReExportAnimSequence.Small", new IMAGE_BRUSH("Slate/ExportAnimSequence_24x", Icon24x24));
			Set("ControlRig.ImportFromRigSequence", new IMAGE_BRUSH("Slate/ReImportRigSequence_16x", Icon16x16));
			Set("ControlRig.ImportFromRigSequence.Small", new IMAGE_BRUSH("Slate/ReImportRigSequence_16x", Icon16x16));
			Set("ControlRig.ReImportFromRigSequence", new IMAGE_BRUSH("Slate/ReImportRigSequence_16x", Icon16x16));
			Set("ControlRig.ReImportFromRigSequence.Small", new IMAGE_BRUSH("Slate/ReImportRigSequence_16x", Icon16x16));
		}

		//Tool Styles
		{
			Set("ControlRig.OnlySelectControls", new IMAGE_BRUSH_SVG("Slate/AnimationSelectOnlyControlRig", Icon16x16));
			Set("ControlRig.SnapperTool", new IMAGE_BRUSH_SVG("Slate/AnimationControlRigSnapper", Icon16x16));
			Set("ControlRig.PoseTool", new IMAGE_BRUSH_SVG("Slate/AnimationPoses", Icon16x16));
			Set("ControlRig.CreatePose", new IMAGE_BRUSH_SVG("Slate/AnimationCreatePose", Icon16x16));
			Set("ControlRig.TweenTool", new IMAGE_BRUSH_SVG("Slate/AnimationTweens", Icon16x16));
			Set("ControlRig.EditableMotionTrails", new IMAGE_BRUSH_SVG("Slate/EditableMotionTrails", Icon16x16));
			Set("ControlRig.TemporaryPivot", new IMAGE_BRUSH_SVG("Slate/TemporaryPivot", Icon16x16));
		}

		// Control Rig Editor styles
		{
			Set("ControlRig.TabIcon", new IMAGE_BRUSH("Slate/ControlRigTab_16x", Icon16x16));
			Set("ControlRig.RigUnit", new IMAGE_BRUSH("Slate/ControlRigUnit_16x", Icon16x16));

			Set("HierarchicalProfiler.TabIcon", new IMAGE_BRUSH_SVG("Slate/HierarchicalProfiler", Icon16x16));

			// icons for control units
			Set("ControlRig.ControlUnitOn", new IMAGE_BRUSH("Slate/ControlUnit_On", Icon32x32));
			Set("ControlRig.ControlUnitOff", new IMAGE_BRUSH("Slate/ControlUnit_Off", Icon32x32));

			Set("ControlRig.ExecuteGraph", new IMAGE_BRUSH("Slate/ExecuteGraph", Icon40x40));
			Set("ControlRig.ExecuteGraph.Small", new IMAGE_BRUSH("Slate/ExecuteGraph", Icon20x20));

			Set("ControlRig.AutoCompileGraph", new IMAGE_BRUSH("Slate/AutoCompile", Icon40x40));
			Set("ControlRig.AutoCompileGraph.Small", new IMAGE_BRUSH("Slate/AutoCompile", Icon20x20));

			Set("ControlRig.SetupMode", new IMAGE_BRUSH("Slate/SetupMode", Icon40x40));
			Set("ControlRig.SetupMode.Small", new IMAGE_BRUSH("Slate/SetupMode", Icon20x20));

			Set("ControlRig.UpdateEvent", new IMAGE_BRUSH("Slate/UpdateEvent", Icon40x40));
			Set("ControlRig.InverseEvent", new IMAGE_BRUSH("Slate/InverseEvent", Icon40x40));
			Set("ControlRig.UpdateAndInverse", new IMAGE_BRUSH("Slate/UpdateAndInverse", Icon40x40));
			Set("ControlRig.InverseAndUpdate", new IMAGE_BRUSH("Slate/InverseAndUpdate", Icon40x40));

			Set("ControlRig.Bug.Dot", new IMAGE_BRUSH("Slate/ControlRig_BugDot_32x", Icon16x16));
			Set("ControlRig.Bug.Normal", new IMAGE_BRUSH("Slate/ControlRig_Bug_28x", Icon14x14));
			Set("ControlRig.Bug.Open", new IMAGE_BRUSH("Slate/ControlRig_BugOpen_28x", Icon14x14));
			Set("ControlRig.Bug.Solid", new IMAGE_BRUSH("Slate/ControlRig_BugSolid_28x", Icon14x14));

			Set("ControlRig.ResumeExecution", new IMAGE_BRUSH_SVG("Slate/simulate", Icon40x40));
			Set("ControlRig.ReleaseMode", new IMAGE_BRUSH_SVG("Slate/animation", Icon40x40));
			Set("ControlRig.DebugMode", new IMAGE_BRUSH_SVG("Slate/Bug", Icon40x40));
			
			Set( "ControlRig.StepOut", new IMAGE_BRUSH("Slate/icon_DebugStepOut_40x", Icon40x40));
			Set( "ControlRig.StepInto", new IMAGE_BRUSH( "Slate/icon_DebugStepIn_40x", Icon40x40 ) );
			Set( "ControlRig.StepOver", new IMAGE_BRUSH( "Slate/icon_DebugStepOver_40x", Icon40x40 ) );

			{
				const FString PreviousContentRoot = GetContentRootDir();
				// Common/RoundedSelection_16x lives in the editor slate folder, change the root temporarily
				SetContentRoot(EngineEditorSlateDir);
				// similar style to "LevelViewport.StartingPlayInEditorBorder"
				Set( "ControlRig.Viewport.Border", new BOX_BRUSH( "Old/Window/ViewportDebugBorder", 0.8f, FLinearColor(1.0f,1.0f,1.0f,1.0f) ) );
				// similar style to "AnimViewport.Notification.Warning"
				Set( "ControlRig.Viewport.Notification.ChangeShapeTransform", new BOX_BRUSH("Common/RoundedSelection_16x", 4.0f/16.0f, FLinearColor(FColor(169, 0, 148))));
				// restore the previous content root
				SetContentRoot(PreviousContentRoot);
			}
		}

		// Graph styles
		{
			Set("ControlRig.Node.PinTree.Arrow_Collapsed_Left", new IMAGE_BRUSH("Slate/TreeArrow_Collapsed_Left", Icon10x10, DefaultForeground));
			Set("ControlRig.Node.PinTree.Arrow_Collapsed_Hovered_Left", new IMAGE_BRUSH("Slate/TreeArrow_Collapsed_Hovered_Left", Icon10x10, DefaultForeground));

			Set("ControlRig.Node.PinTree.Arrow_Expanded_Left", new IMAGE_BRUSH("Slate/TreeArrow_Expanded_Left", Icon10x10, DefaultForeground));
			Set("ControlRig.Node.PinTree.Arrow_Expanded_Hovered_Left", new IMAGE_BRUSH("Slate/TreeArrow_Expanded_Hovered_Left", Icon10x10, DefaultForeground));

			Set("ControlRig.Node.PinTree.Arrow_Collapsed_Right", new IMAGE_BRUSH("Slate/TreeArrow_Collapsed_Right", Icon10x10, DefaultForeground));
			Set("ControlRig.Node.PinTree.Arrow_Collapsed_Hovered_Right", new IMAGE_BRUSH("Slate/TreeArrow_Collapsed_Hovered_Right", Icon10x10, DefaultForeground));

			Set("ControlRig.Node.PinTree.Arrow_Expanded_Right", new IMAGE_BRUSH("Slate/TreeArrow_Expanded_Right", Icon10x10, DefaultForeground));
			Set("ControlRig.Node.PinTree.Arrow_Expanded_Hovered_Right", new IMAGE_BRUSH("Slate/TreeArrow_Expanded_Hovered_Right", Icon10x10, DefaultForeground));
		}

		// Tree styles
		{
			Set("ControlRig.Tree.BoneUser", new IMAGE_BRUSH("Slate/BoneUser_16x", Icon16x16));
			Set("ControlRig.Tree.BoneImported", new IMAGE_BRUSH("Slate/BoneImported_16x", Icon16x16));
			Set("ControlRig.Tree.Control", new IMAGE_BRUSH("Slate/Control_16x", Icon16x16));
			Set("ControlRig.Tree.Null", new IMAGE_BRUSH("Slate/Null_16x", Icon16x16));
			Set("ControlRig.Tree.RigidBody", new IMAGE_BRUSH("Slate/RigidBody_16x", Icon16x16));
			Set("ControlRig.Tree.Socket", new IMAGE_BRUSH("Slate/Socket_16x", Icon16x16));
		}

		// Font?
		{
			Set("ControlRig.Hierarchy.Menu", TTF_FONT("Fonts/Roboto-Regular", 12));
		}

		// Space picker
		SpacePickerSelectColor = FStyleColors::Select;
		{
			Set("ControlRig.SpacePicker.RoundedRect", new FSlateRoundedBoxBrush(FStyleColors::White, 4.0f, FStyleColors::Transparent, 0.0f));
		}

		FSlateStyleRegistry::RegisterSlateStyle(*this);
	}

	static FControlRigEditorStyle& Get()
	{
		static FControlRigEditorStyle Inst;
		return Inst;
	}
	
	~FControlRigEditorStyle()
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*this);
	}

	FSlateColor SpacePickerSelectColor;
};

#undef IMAGE_BRUSH
#undef BOX_BRUSH
#undef BORDER_BRUSH
#undef TTF_FONT
#undef OTF_FONT
