// Copyright Epic Games, Inc. All Rights Reserved.

#include "SAudioRadialSlider.h"

#include "DSP/Dsp.h"
#include "Slate/SRadialSlider.h"
#include "Styling/SlateStyleRegistry.h"

const FVector2D SAudioRadialSlider::LinearRange = FVector2D(0.0f, 1.0f);

SAudioRadialSlider::SAudioRadialSlider()
{
}

void SAudioRadialSlider::Construct(const SAudioRadialSlider::FArguments& InArgs)
{
	Style = InArgs._Style;
	OnValueChanged = InArgs._OnValueChanged;
	Value = InArgs._Value;
	CenterBackgroundColor = InArgs._CenterBackgroundColor;
	SliderProgressColor = InArgs._SliderProgressColor;
	SliderBarColor = InArgs._SliderBarColor;
	HandStartEndRatio = InArgs._HandStartEndRatio;
	WidgetLayout = InArgs._WidgetLayout;
	DesiredSizeOverride = InArgs._DesiredSizeOverride;
	SliderCurve = InArgs._SliderCurve;
	// default linear curve from 0.0 to 1.0
	SliderCurve.GetRichCurve()->AddKey(0.0f, 0.0f);
	SliderCurve.GetRichCurve()->AddKey(1.0f, 1.0f);
	
	// Get style 
	const ISlateStyle* AudioWidgetsStyle = FSlateStyleRegistry::FindSlateStyle("AudioWidgetsStyle");
	if (AudioWidgetsStyle)
	{
		Style = &AudioWidgetsStyle->GetWidgetStyle<FAudioRadialSliderStyle>("AudioRadialSlider.Style");
	}

	// Create components
	SAssignNew(Label, SAudioTextBox)
		.Style(&Style->TextBoxStyle)
		.OnValueTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type CommitType)
		{
			const float OutputValue = FCString::Atof(*Text.ToString());
			const float LinValue = GetLinValue(OutputValue);
			Value.Set(LinValue);
			RadialSlider->SetValue(LinValue);
			OnValueChanged.ExecuteIfBound(LinValue);
		});

	SAssignNew(RadialSlider, SRadialSlider)
		.OnValueChanged_Lambda([this](float InLinValue)
		{
			Value.Set(InLinValue);
			OnValueChanged.ExecuteIfBound(InLinValue);
			const float OutputValue = GetOutputValue(InLinValue);
			Label->SetValueText(OutputValue);
		})
		.UseVerticalDrag(true)
		.ShowSliderHand(true)
		.ShowSliderHandle(false);
	RadialSlider->SetCenterBackgroundColor(CenterBackgroundColor.Get());
	RadialSlider->SetSliderProgressColor(SliderProgressColor.Get());
	RadialSlider->SetSliderBarColor(SliderBarColor.Get());
	RadialSlider->SetSliderRange(SliderCurve);

	ChildSlot
	[
		CreateLayoutWidgetSwitcher()
	];

	SetOutputRange(OutputRange);
}

void SAudioRadialSlider::SetCenterBackgroundColor(FSlateColor InColor)
{
	SetAttribute(CenterBackgroundColor, TAttribute<FSlateColor>(InColor), EInvalidateWidgetReason::Paint);
	RadialSlider->SetCenterBackgroundColor(InColor);
}

void SAudioRadialSlider::SetSliderProgressColor(FSlateColor InColor)
{
	SetAttribute(SliderProgressColor, TAttribute<FSlateColor>(InColor), EInvalidateWidgetReason::Paint);
	RadialSlider->SetSliderProgressColor(InColor);
}

void SAudioRadialSlider::SetSliderBarColor(FSlateColor InColor)
{
	SetAttribute(SliderBarColor, TAttribute<FSlateColor>(InColor), EInvalidateWidgetReason::Paint);
	RadialSlider->SetSliderBarColor(InColor);
}

void SAudioRadialSlider::SetHandStartEndRatio(const FVector2D InHandStartEndRatio)
{
	SetAttribute(HandStartEndRatio, TAttribute<FVector2D>(InHandStartEndRatio), EInvalidateWidgetReason::Paint);
	RadialSlider->SetHandStartEndRatio(InHandStartEndRatio);
}

void SAudioRadialSlider::SetWidgetLayout(EAudioRadialSliderLayout InLayout)
{
	SetAttribute(WidgetLayout, TAttribute<EAudioRadialSliderLayout>(InLayout), EInvalidateWidgetReason::Layout);
	LayoutWidgetSwitcher->SetActiveWidgetIndex(InLayout);
}

FVector2D SAudioRadialSlider::ComputeDesiredSize(float) const
{
	static const FVector2D DefaultDesiredSize = FVector2D(50.0f, 81.0f);

	if (DesiredSizeOverride.Get().IsSet())
	{
		return DesiredSizeOverride.Get().GetValue();
	}
	const float SliderRadius = Style->DefaultSliderRadius;
	const FVector2D TextBoxImageSize = Style->TextBoxStyle.BackgroundImage.ImageSize;
	return FVector2D(FMath::Max(SliderRadius, TextBoxImageSize.X), SliderRadius + TextBoxImageSize.Y + Style->LabelPadding);
}

void SAudioRadialSlider::SetDesiredSizeOverride(const FVector2D Size)
{
	SetAttribute(DesiredSizeOverride, TAttribute<TOptional<FVector2D>>(Size), EInvalidateWidgetReason::Layout);
}

TSharedRef<SWidgetSwitcher> SAudioRadialSlider::CreateLayoutWidgetSwitcher()
{
	SAssignNew(LayoutWidgetSwitcher, SWidgetSwitcher);

	float LabelVerticalPadding = Style->LabelPadding;

	LayoutWidgetSwitcher->AddSlot(EAudioRadialSliderLayout::Layout_LabelTop)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, LabelVerticalPadding)
		[
			Label.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			RadialSlider.ToSharedRef()
		]
	];

	LayoutWidgetSwitcher->AddSlot(EAudioRadialSliderLayout::Layout_LabelCenter)
	[
		SNew(SOverlay)
		+SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			RadialSlider.ToSharedRef()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Label.ToSharedRef()
		]
	];

	LayoutWidgetSwitcher->AddSlot(EAudioRadialSliderLayout::Layout_LabelBottom)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			RadialSlider.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.AutoHeight()
		.Padding(0.0f, LabelVerticalPadding, 0.0f, 0.0f)
		[
			Label.ToSharedRef()
		]
	];

	LayoutWidgetSwitcher->SetActiveWidgetIndex(WidgetLayout.Get());
	return LayoutWidgetSwitcher.ToSharedRef();
}

void SAudioRadialSlider::SetValue(float LinValue)
{
	Value.Set(LinValue);
	const float OutputValue = GetOutputValue(LinValue);
	Label->SetValueText(OutputValue);
	RadialSlider->SetValue(LinValue);
}

const float SAudioRadialSlider::GetLinValue(const float OutputValue)
{
	return FMath::GetMappedRangeValueClamped(OutputRange, LinearRange, OutputValue);
}

const float SAudioRadialSlider::GetOutputValue(const float LinValue)
{
	return FMath::GetMappedRangeValueClamped(LinearRange, OutputRange, LinValue);
}

void SAudioRadialSlider::SetOutputRange(const FVector2D Range)
{
	if (Range.Y > Range.X)
	{
		OutputRange = Range;
		SetValue(FMath::Clamp(Value.Get(), Range.X, Range.Y));
		Label->UpdateValueTextWidth(Range);
	}
}

void SAudioRadialSlider::SetLabelBackgroundColor(FSlateColor InColor)
{
	SetAttribute(LabelBackgroundColor, TAttribute<FSlateColor>(InColor), EInvalidateWidgetReason::Paint);
	Label->SetLabelBackgroundColor(InColor);
}

void SAudioRadialSlider::SetUnitsText(const FText Units)
{
	Label->SetUnitsText(Units);
}

void SAudioRadialSlider::SetUnitsTextReadOnly(const bool bIsReadOnly)
{
	Label->SetUnitsTextReadOnly(bIsReadOnly);
}

void SAudioRadialSlider::SetValueTextReadOnly(const bool bIsReadOnly)
{
	Label->SetValueTextReadOnly(bIsReadOnly);
}

void SAudioRadialSlider::SetShowLabelOnlyOnHover(const bool bShowLabelOnlyOnHover)
{
	Label->SetShowLabelOnlyOnHover(bShowLabelOnlyOnHover);
}

void SAudioRadialSlider::SetShowUnitsText(const bool bShowUnitsText)
{
	Label->SetShowUnitsText(bShowUnitsText);
}

void SAudioRadialSlider::SetSliderThickness(const float Thickness)
{
	RadialSlider->SetThickness(FMath::Max(0.0f, Thickness));
}

// SAudioVolumeRadialSlider
SAudioVolumeRadialSlider::SAudioVolumeRadialSlider()
{
}

void SAudioVolumeRadialSlider::Construct(const SAudioRadialSlider::FArguments& InArgs)
{
	SAudioRadialSlider::Construct(InArgs);
	
	SetOutputRange(FVector2D(-100.0f, 0.0f));
	Label->SetUnitsText(FText::FromString("dB"));
}

const float SAudioVolumeRadialSlider::GetOutputValue(const float LinValue)
{
	// convert from linear 0-1 space to decibel OutputRange that has been converted to linear 
	const FVector2D LinearSliderRange = FVector2D(Audio::ConvertToLinear(OutputRange.X), Audio::ConvertToLinear(OutputRange.Y));
	const float LinearSliderValue = FMath::GetMappedRangeValueClamped(LinearRange, LinearSliderRange, LinValue);
	// convert from linear to decibels 
	float OutputValue = Audio::ConvertToDecibels(LinearSliderValue);
	return FMath::Clamp(OutputValue, OutputRange.X, OutputRange.Y);
}

const float SAudioVolumeRadialSlider::GetLinValue(const float OutputValue)
{
	float ClampedValue = FMath::Clamp(OutputValue, OutputRange.X, OutputRange.Y);
	// convert from decibels to linear
	float LinearSliderValue = Audio::ConvertToLinear(ClampedValue);
	// convert from decibel OutputRange that has been converted to linear to linear 0-1 space 
	const FVector2D LinearSliderRange = FVector2D(Audio::ConvertToLinear(OutputRange.X), Audio::ConvertToLinear(OutputRange.Y));
	return FMath::GetMappedRangeValueClamped(LinearSliderRange, LinearRange, LinearSliderValue);
}

// SAudioFrequencyRadialSlider
SAudioFrequencyRadialSlider::SAudioFrequencyRadialSlider()
{
}

void SAudioFrequencyRadialSlider::Construct(const SAudioRadialSlider::FArguments& InArgs)
{
	SAudioRadialSlider::Construct(InArgs);

	SetOutputRange(FVector2D(20.0f, 20000.0f));
	Label->SetUnitsText(FText::FromString("Hz"));
}

const float SAudioFrequencyRadialSlider::GetOutputValue(const float LinValue)
{
	return Audio::GetLogFrequencyClamped(LinValue, LinearRange, OutputRange);
}

const float SAudioFrequencyRadialSlider::GetLinValue(const float OutputValue)
{
	return Audio::GetLinearFrequencyClamped(OutputValue, LinearRange, OutputRange);
}
