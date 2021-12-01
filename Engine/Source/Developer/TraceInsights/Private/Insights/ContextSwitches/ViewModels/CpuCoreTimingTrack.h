// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Insights
#include "Insights/ViewModels/TimingEventsTrack.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

class FThreadTrackEvent;

namespace Insights
{

class FContextSwitchesSharedState;

////////////////////////////////////////////////////////////////////////////////////////////////////

class FCpuCoreTimingTrack : public FTimingEventsTrack
{
	INSIGHTS_DECLARE_RTTI(FCpuCoreTimingTrack, FTimingEventsTrack)

public:
	explicit FCpuCoreTimingTrack(FContextSwitchesSharedState& InSharedState, const FString& InName, uint32 InCoreNumber)
		: FTimingEventsTrack(InName)
		, SharedState(InSharedState)
		, CoreNumber(InCoreNumber)
	{
	}

	virtual ~FCpuCoreTimingTrack() {}

	uint32 GetCoreNumber() const { return CoreNumber; }

	virtual void BuildDrawState(ITimingEventsTrackDrawStateBuilder& Builder, const ITimingTrackUpdateContext& Context) override;
	virtual void BuildFilteredDrawState(ITimingEventsTrackDrawStateBuilder& Builder, const ITimingTrackUpdateContext& Context) override;

	virtual void Draw(const ITimingTrackDrawContext& Context) const override;

	virtual void InitTooltip(FTooltipDrawState& InOutTooltip, const ITimingEvent& InTooltipEvent) const override;

	virtual void BuildContextMenu(FMenuBuilder& MenuBuilder) override;

protected:
	virtual const TSharedPtr<const ITimingEvent> GetEvent(double InTime, double SecondsPerPixel, int32 Depth) const override;

private:
	FContextSwitchesSharedState& SharedState;

	uint32 CoreNumber;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace Insights
