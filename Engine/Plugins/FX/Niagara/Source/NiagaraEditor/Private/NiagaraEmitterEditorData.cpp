// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraEmitterEditorData.h"

#include "NiagaraStackEditorData.h"
#include "ScopedTransaction.h"

UNiagaraEmitterEditorData::UNiagaraEmitterEditorData(const FObjectInitializer& ObjectInitializer)
{
	StackEditorData = ObjectInitializer.CreateDefaultSubobject<UNiagaraStackEditorData>(this, TEXT("StackEditorData"));

	if (StackEditorData != nullptr)
	{
		StackEditorData->OnPersistentDataChanged().AddUObject(this, &UNiagaraEmitterEditorData::StackEditorDataChanged);
	}
	
	PlaybackRangeMin = 0;
	PlaybackRangeMax = 10;
}

void UNiagaraEmitterEditorData::PostLoad()
{
	Super::PostLoad();
	if (StackEditorData == nullptr)
	{
		StackEditorData = NewObject<UNiagaraStackEditorData>(this, TEXT("StackEditorData"), RF_Transactional);
		StackEditorData->OnPersistentDataChanged().AddUObject(this, &UNiagaraEmitterEditorData::StackEditorDataChanged);
	}
	StackEditorData->ConditionalPostLoad();
}

UNiagaraStackEditorData& UNiagaraEmitterEditorData::GetStackEditorData() const
{
	return *StackEditorData;
}

TRange<float> UNiagaraEmitterEditorData::GetPlaybackRange() const
{
	return TRange<float>(PlaybackRangeMin, PlaybackRangeMax);
}

void UNiagaraEmitterEditorData::SetPlaybackRange(TRange<float> InPlaybackRange)
{
	PlaybackRangeMin = InPlaybackRange.GetLowerBoundValue();
	PlaybackRangeMax = InPlaybackRange.GetUpperBoundValue();

	OnPersistentDataChanged().Broadcast();
}

void UNiagaraEmitterEditorData::StackEditorDataChanged()
{
	OnPersistentDataChanged().Broadcast();
}



TMap<FFunctionInputSummaryViewKey, FFunctionInputSummaryViewMetadata> UNiagaraEmitterEditorData::GetSummaryViewMetaDataMap() const
{
	return SummaryViewFunctionInputMetadata;
}

FFunctionInputSummaryViewMetadata UNiagaraEmitterEditorData::GetSummaryViewMetaData(const FFunctionInputSummaryViewKey& Key) const
{
	return SummaryViewFunctionInputMetadata.Contains(Key)? SummaryViewFunctionInputMetadata[Key] : FFunctionInputSummaryViewMetadata();
}

void UNiagaraEmitterEditorData::SetSummaryViewMetaData(const FFunctionInputSummaryViewKey& Key, const FFunctionInputSummaryViewMetadata& NewMetadata)
{
	FScopedTransaction ScopedTransaction(NSLOCTEXT("NiagaraEmitter", "EmitterModuleNodeMetaDataChanged", "MetaData for summary view node changed."));
	static const FFunctionInputSummaryViewMetadata Empty;

	if (NewMetadata == Empty)
	{
		SummaryViewFunctionInputMetadata.Remove(Key);
	}
	else
	{
		SummaryViewFunctionInputMetadata.FindOrAdd(Key) = NewMetadata;
	}
	OnPersistentDataChanged().Broadcast();
	// GraphSource->MarkNotSynchronized(TEXT("MetaData for summary view node changed."));
	OnSummaryViewStateChangedDelegate.Broadcast();
	// UNiagaraSystem::RequestCompileForEmitter(this);
}

void UNiagaraEmitterEditorData::ToggleShowSummaryView()
{
	bShowSummaryView = !bShowSummaryView;
	OnPersistentDataChanged().Broadcast();
	OnSummaryViewStateChangedDelegate.Broadcast();
}

FSimpleMulticastDelegate& UNiagaraEmitterEditorData::OnSummaryViewStateChanged()
{
	return OnSummaryViewStateChangedDelegate;
}