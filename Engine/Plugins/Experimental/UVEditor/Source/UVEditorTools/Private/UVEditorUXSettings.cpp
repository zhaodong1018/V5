// Copyright Epic Games, Inc. All Rights Reserved.

#include "UVEditorUXSettings.h"

const float FUVEditorUXSettings::CameraFarPlaneWorldZ(-10.0);

// 2D Viewport Depth Offsets (Organized by "layers" from the camera's perspective, descending order
const float FUVEditorUXSettings::CameraNearPlaneProportionZ(0.6); // Top layer
const float FUVEditorUXSettings::SewLineDepthOffset(0.5f);
const float FUVEditorUXSettings::SelectionWireframeDepthBias(0.4);
const float FUVEditorUXSettings::SelectionTriangleDepthBias(0.3);
const float FUVEditorUXSettings::WireframeDepthOffset(0.2);
const float FUVEditorUXSettings::UnwrapTriangleDepthOffset(0.1);

// Note that this offset can only be applied when we use our own background material
// for a user-supplied texture, and we can't use it for a user-provided material.
// So for consistency this should stay at zero.
const float FUVEditorUXSettings::BackgroundQuadDepthOffset(0.0); // Bottom layer

// 3D Viewport Depth Offsets
const float FUVEditorUXSettings::LivePreviewHighlightDepthOffset(0.5);

// Opacities
const float FUVEditorUXSettings::UnwrapTriangleOpacity(1.0);
const float FUVEditorUXSettings::UnwrapTriangleOpacityWithBackground(0.25);
const float FUVEditorUXSettings::SelectionTriangleOpacity(1.0f);

// Per Asset Shifts
const float FUVEditorUXSettings::UnwrapBoundaryHueShift(30);
const float FUVEditorUXSettings::UnwrapBoundarySaturation(0.50);
const float FUVEditorUXSettings::UnwrapBoundaryValue(0.50);

// Colors
const FColor FUVEditorUXSettings::UnwrapTriangleFillColor(FColor::FromHex("#696871"));
const FColor FUVEditorUXSettings::UnwrapTriangleWireframeColor(FColor::FromHex("#989898"));
const FColor FUVEditorUXSettings::SelectionTriangleFillColor(FColor::FromHex("#8C7A52"));
const FColor FUVEditorUXSettings::SelectionTriangleWireframeColor(FColor::FromHex("#DDA209"));
const FColor FUVEditorUXSettings::SelectionHoverTriangleFillColor(FColor::FromHex("#4E719B"));
const FColor FUVEditorUXSettings::SelectionHoverTriangleWireframeColor(FColor::FromHex("#0E86FF"));
const FColor FUVEditorUXSettings::SewSideLeftColor(FColor::Red);
const FColor FUVEditorUXSettings::SewSideRightColor(FColor::Green);
const FColor FUVEditorUXSettings::XAxisColor(FColor::Red);
const FColor FUVEditorUXSettings::YAxisColor(FColor::Green);
const FColor FUVEditorUXSettings::GridMajorColor(FColor::FromHex("#888888"));
const FColor FUVEditorUXSettings::GridMinorColor(FColor::FromHex("#777777"));

// Thicknesses
const float FUVEditorUXSettings::LivePreviewHighlightThickness(2.0);
const float FUVEditorUXSettings::SelectionLineThickness(1.5);
const float FUVEditorUXSettings::SelectionPointThickness(6);
const float FUVEditorUXSettings::SewLineHighlightThickness(3.0f);
const float FUVEditorUXSettings::AxisThickness(2.0);
const float FUVEditorUXSettings::GridMajorThickness(1.0);

// Grid
const int32 FUVEditorUXSettings::GridSubdivisionsPerLevel(4);
const int32 FUVEditorUXSettings::GridLevels(3);


FLinearColor FUVEditorUXSettings::GetTriangleColorByTargetIndex(int32 TargetIndex)
{
	double GoldenAngle = 137.50776405;

	FLinearColor BaseColorHSV = FLinearColor::FromSRGBColor(UnwrapTriangleFillColor).LinearRGBToHSV();
	BaseColorHSV.R = FMath::Fmod(BaseColorHSV.R + (GoldenAngle / 2.0 * TargetIndex), 360);;

	return BaseColorHSV.HSVToLinearRGB();
}

FLinearColor FUVEditorUXSettings::GetWireframeColorByTargetIndex(int32 TargetIndex)
{
	return FLinearColor::FromSRGBColor(UnwrapTriangleWireframeColor);;
}

FLinearColor FUVEditorUXSettings::GetBoundaryColorByTargetIndex(int32 TargetIndex)
{
	FLinearColor BaseColorHSV = GetTriangleColorByTargetIndex(TargetIndex).LinearRGBToHSV();
	FLinearColor BoundaryColorHSV = BaseColorHSV;
	BoundaryColorHSV.R = FMath::Fmod((BoundaryColorHSV.R + UnwrapBoundaryHueShift), 360);
	BoundaryColorHSV.G = UnwrapBoundarySaturation;
	BoundaryColorHSV.B = UnwrapBoundaryValue;
	return BoundaryColorHSV.HSVToLinearRGB();
}
