﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AudioDevice.h"
#include "DSP/BufferVectorOperations.h"
#include "Math/NumericLimits.h"

namespace UE::MediaIoCoreModule::Private
{
	template <typename OutputType>
	TArray<OutputType> ConvertAndUpmixBuffer(const Audio::FAlignedFloatBuffer& InBuffer, int32 NumInputChannels, int32 NumOutputChannels)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MediaIoCoreModule::ConvertAndUpmixBuffer);
		/**
		 * @Note: Audio::DownmixBuffer was not used here because it does not convert data in place,
		 *  this forces us to create an additional buffer to hold the final int32 values. Instead we 
		 *  upmix and convert the values in a single pass.
		 */

		// @Note: We only support upmixing at the moment.
		
		TArray<OutputType> ConvertedBuffer;
		ConvertedBuffer.SetNumZeroed(NumOutputChannels / NumInputChannels * InBuffer.Num());

		OutputType* ConvertedBufferPtr = ConvertedBuffer.GetData();

		for (int32 Index = 0; Index < InBuffer.Num(); Index += 2)
		{
			constexpr double Scale = TNumericLimits<OutputType>::Max();
		
			// Convert values for each channel
			for (int32 ChannelIndex = 0; ChannelIndex < NumInputChannels; ++ChannelIndex)
			{
				// @Note: While this conversion is a common approach, it can introduce distortion. 
				// See: https://www.cs.cmu.edu/~rbd/papers/cmj-float-to-int.html
				const float FloatValue = InBuffer[Index + ChannelIndex];
				OutputType ConvertedValue = static_cast<OutputType>((FloatValue * Scale) + 0.5);
				*(ConvertedBufferPtr + ChannelIndex) = ConvertedValue;
			}
		
			ConvertedBufferPtr += NumOutputChannels;
		}
		return ConvertedBuffer;
	}
}

class MEDIAIOCORE_API FMediaIOAudioOutput
{ 
public:
	struct FAudioOptions
	{
		uint32 InNumInputChannels = 0;
		uint32 InNumOutputChannels = 0;
		FFrameRate InTargetFrameRate;
		uint32 InMaxSampleLatency = 0;
		uint32 InOutputSampleRate = 0;
	};

	FMediaIOAudioOutput(Audio::FPatchOutputStrongPtr InPatchOutput, const FAudioOptions& InAudioOptions);

	/**
	 * Get the audio sample that were accumulated.
	 */
	template <typename OutputType>
	TArray<OutputType> GetAudioSamples() const
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMediaIOAudioOutput::GetAudioSamples);
		
		const Audio::FAlignedFloatBuffer FloatBuffer = GetFloatBuffer();
		return UE::MediaIoCoreModule::Private::ConvertAndUpmixBuffer<OutputType>(FloatBuffer, NumInputChannels, NumOutputChannels);
	}

private:
	int32 GetAudioBuffer(int32 InNumSamplesToPop, float* OutBuffer) const;
	Audio::FAlignedFloatBuffer GetFloatBuffer() const;

private:
	/** The buffer accumulating audio samples. */
	Audio::FPatchOutputStrongPtr PatchOutput;

	/** Number of audio channels on the engine side. */
	int32 NumInputChannels;

	/** Number of audio channels to output. */
	int32 NumOutputChannels;

	FFrameRate TargetFrameRate;

	/** Maximum number of samples to accumulate before they are discarded. */
	uint32 MaxSampleLatency;

	uint32 OutputSampleRate;
};

/**
 * Handles capturing capturing audio samples rendered by the engine and dispatching them to outputs.
 */
class FMediaIOAudioCapture : public ISubmixBufferListener
{
public:
	FMediaIOAudioCapture();
	virtual ~FMediaIOAudioCapture();

	//~ ISubmixBufferListener interface
	virtual void OnNewSubmixBuffer(const USoundSubmix* InOwningSubmix, float* InAudioData, int32 InNumSamples, int32 InNumChannels, const int32 InSampleRate, double InAudioClock) override;

	/** Create an audio output that will receive audio samples. */
	TSharedPtr<FMediaIOAudioOutput> CreateAudioOutput(int32 InNumOutputChannels, FFrameRate InTargetFrameRate, uint32 InMaxSampleLatency, uint32 InOutputSampleRate);

private:
	/** Sample rate on the engine side. */ 
	uint32 SampleRate = 0;

	// Used to make sure we only accumulate audio from the master submix. 
	FName MasterSubmixName;

	/** Number of channels on the engine side. */
	int32 NumChannels = 0;

	/** Utility that allows pushing audio samples to multiple outputs. */
	Audio::FPatchSplitter AudioSplitter;
};
