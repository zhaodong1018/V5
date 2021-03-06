// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "TraceServices/Model/TasksProfiler.h"

namespace Insights
{

////////////////////////////////////////////////////////////////////////////////////////////////////

class FTaskEntry
{
	friend class STaskTableTreeView;

public:
	FTaskEntry(const TraceServices::FTaskInfo &TaskInfo);
	~FTaskEntry() {}

	double GetCreatedTimestamp() const { return CreatedTimestamp; }
	uint32 GetCreatedThreadId() const { return CreatedThreadId; }

	double GetLaunchedTimestamp() const { return LaunchedTimestamp; }
	uint32 GetLaunchedThreadId() const { return LaunchedThreadId; }

	double GetScheduledTimestamp() const { return ScheduledTimestamp; }
	uint32 GetScheduledThreadId() const { return ScheduledThreadId; }

	double GetStartedTimestamp() const { return StartedTimestamp; }
	uint32 GetStartedThreadId() const { return StartedThreadId; }

	double GetFinishedTimestamp() const { return FinishedTimestamp; }

	double GetCompletedTimestamp() const { return CompletedTimestamp; }
	uint32 GetCompletedThreadId() const { return CompletedThreadId; }

	uint32 GetNumNested() const { return NumNested; }
	uint32 GetNumPrerequisites() const { return NumPrerequisites; }
	uint32 GetNumSubsequents() const { return NumSubsequents; }

	uint32 GetId() const { return Id; }

private:
	uint32 Id;

	const TCHAR* DebugName; 
	bool bTracked; 
	int32 ThreadToExecuteOn;

	double CreatedTimestamp;
	uint32 CreatedThreadId;

	double LaunchedTimestamp;
	uint32 LaunchedThreadId;

	double ScheduledTimestamp;
	uint32 ScheduledThreadId;

	double StartedTimestamp;
	uint32 StartedThreadId;

	double FinishedTimestamp;

	double CompletedTimestamp;
	uint32 CompletedThreadId;

	uint32 NumNested;
	uint32 NumSubsequents;
	uint32 NumPrerequisites;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace Insights
