// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundWave.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "ContentStreaming.h"
#include "DecoderInputFactory.h"
#include "DSP/ParamInterpolator.h"
#include "IAudioCodec.h"
#include "IAudioCodecRegistry.h"
#include "MetasoundPrimitives.h"
#include "Sound/SoundWave.h"


static int32 DisableMetasoundWaveAssetCachePriming = 0;
FAutoConsoleVariableRef CVarDisableMetasoundWaveAssetCachePriming(
	TEXT("au.MetaSound.DisableWaveCachePriming"),
	DisableMetasoundWaveAssetCachePriming,
	TEXT("Disables MetaSound Wave Cache Priming.\n")
	TEXT("0 (default): Enabled, 1: Disabled"),
	ECVF_Default);

namespace Metasound
{

	FWaveAsset::FWaveAsset(const TUniquePtr<Audio::IProxyData>& InInitData)
	{
		if (InInitData.IsValid())
		{
			if (InInitData->CheckTypeCast<FSoundWaveProxy>())
			{
				// should we be getting handed a SharedPtr here?
				SoundWaveProxy = MakeShared<FSoundWaveProxy, ESPMode::ThreadSafe>(InInitData->GetAs<FSoundWaveProxy>());

				if (ensureAlways(SoundWaveProxy.IsValid()))
				{
					// TODO HACK: Prime the sound for playback.
					//
					// Preferably playback latency would be controlled externally.
					// With the current decoder and waveplayer implementation, the 
					// wave player does not know whether samples were actually decoded
					// or if the decoder is still waiting on the stream cache. Generally
					// this is not an issue except for looping. Looping requires counting
					// of decoded samples to get exact loop points. When the decoder 
					// returns zeroed audio (because the stream cache has not loaded
					// the requested chunk) the sample counting gets off. Currently
					// there is not route to expose that information to the wave 
					// player to correct the sample counting logic. 
					//
					// In hopes of mitigating the issue, the stream cache
					// is primed here in the hopes that the chunk is ready by the
					// time that the decoder attempts to decode audio.
					if (0 == DisableMetasoundWaveAssetCachePriming)
					{
						if (SoundWaveProxy->IsStreaming())
						{
							if (SoundWaveProxy->GetNumChunks() > 1)
							{
								IStreamingManager::Get().GetAudioStreamingManager().RequestChunk(SoundWaveProxy, 1, [](EAudioChunkLoadResult) {});
							}
						}
					}
				}
			}
		}
	}

	bool FWaveAsset::IsSoundWaveValid() const
	{
		return SoundWaveProxy.IsValid();
	}
}

namespace Audio
{
	FSimpleDecoderWrapper::FSimpleDecoderWrapper(const InitParams& InInitParams)
	: OutputSampleRate(InInitParams.OutputSampleRate)
	, MaxPitchShiftCents(InInitParams.MaxPitchShiftMagnitudeAllowedInOctaves * 1200.0f)
	, MaxPitchShiftRatio(FMath::Pow(2.0f, InInitParams.MaxPitchShiftMagnitudeAllowedInOctaves))
	, DecodeBlockSizeInFrames(64)
	, OutputBlockSizeInFrames(InInitParams.OutputBlockSizeInFrames)
	{
		check(OutputBlockSizeInFrames > 0);
	}

	bool FSimpleDecoderWrapper::SetWave(const FSoundWaveProxyPtr& InWave, float InStartTimeSeconds, float InInitialPitchShiftSemitones)
	{
		bool bSuccessful = true;

		if (InWave.IsValid())
		{
			// Determine which values differ so that correct things can be updated.
			const bool bIsDifferentWave = (InWave.Get() != Wave.Get());
			const bool bUpdateNumChannels = (InWave->GetNumChannels() != NumChannels);
			const bool bUpdateInputSampleRate = (InWave->GetSampleRate() != InputSampleRate);

			const bool bUpdateAudioFormat = bUpdateNumChannels || bUpdateInputSampleRate || !bIsInitialized;
			const bool bReinitDecoder = bIsDifferentWave || !bIsInitialized;

			Wave = InWave;

			if (bReinitDecoder)
			{
				// try to initialize decoders
				bSuccessful = InitializeDecodersInternal(Wave, InStartTimeSeconds);
			}
			else
			{
				// If the wave has not changed, only need to seek.
				bSuccessful = SeekToTime(InStartTimeSeconds);
			}
			bDecoderIsDone = !bSuccessful;

			if (bUpdateAudioFormat)
			{
				// initialize input/output data
				InputSampleRate = Wave->GetSampleRate();
				FsOutToInRatio = (OutputSampleRate / InputSampleRate);

				NumChannels = Wave->GetNumChannels();
				DecodeBlockSizeInSamples = DecodeBlockSizeInFrames * NumChannels;

				// set Circular Buffer capacity
				int32 Capacity = FMath::Max(1, static_cast<int32>(OutputBlockSizeInFrames * NumChannels * (1.0f + FsOutToInRatio * MaxPitchShiftRatio) * 2));
				OutputCircularBuffer.Reserve(Capacity, true /* bRetainExistingSamples */);

				Resampler.Init(Audio::EResamplingMethod::Linear, FsOutToInRatio, NumChannels);
				bIsInitialized = true;
			}

			if (bUpdateNumChannels)
			{
				
				PitchShifter.Reset(Wave->GetNumChannels(), InInitialPitchShiftSemitones);
				// Have to discard previous samples if the channel count changes. 
				OutputCircularBuffer.SetNum(0);
			}
			else
			{
				PitchShifter.UpdatePitchShift(InInitialPitchShiftSemitones);
			}
		}

		return bSuccessful;
	}

	bool FSimpleDecoderWrapper::SeekToTime(const float InSeconds)
	{
		if (Wave.IsValid())
		{
			// try to initialize decoders
			bool bSuccessful = InitializeDecodersInternal(Wave, InSeconds);
			bDecoderIsDone = !bSuccessful;

			return bSuccessful;
		}

		return false;
	}
	
	bool FSimpleDecoderWrapper::CanGenerateAudio() const
	{
		// If there is a valid decoder, than this object can generate audio.
		const bool bCanDecoderGenerateAudio = !bDecoderIsDone && Input.IsValid() && Output.IsValid() && Decoder.IsValid() && (NumChannels > 0);

		// If there is audio remaining in the output circular buffer, then this can 
		// generate some audio.
		const bool bIsAudioInOutputBuffer = (0 != OutputCircularBuffer.Num());

		return bCanDecoderGenerateAudio || bIsAudioInOutputBuffer;
	}

	uint32 FSimpleDecoderWrapper::GenerateAudio(float* OutputDest, int32 NumOutputFrames, int32& OutNumFramesConsumed, float PitchShiftInCents, bool bIsLooping)
	{
		if (0 == NumChannels)
		{
			return 0;
		}

		const uint32 NumOutputSamples = NumOutputFrames * NumChannels;

		OutNumFramesConsumed = 0;

		if (OutputCircularBuffer.Num() < NumOutputSamples)
		{

			// (multiply by two just to be sure we can handle SRC output size)
			const int32 MaxNumResamplerOutputFramesPerBlock = FMath::CeilToInt(FsOutToInRatio * DecodeBlockSizeInFrames) * 2;
			const int32 MaxNumResamplerOutputSamplesPerBlock = MaxNumResamplerOutputFramesPerBlock * NumChannels;

			PreSrcBuffer.Reset(DecodeBlockSizeInSamples);
			PreSrcBuffer.AddUninitialized(DecodeBlockSizeInSamples);

			PostSrcBuffer.Reset(MaxNumResamplerOutputSamplesPerBlock);
			PostSrcBuffer.AddUninitialized(MaxNumResamplerOutputSamplesPerBlock);

			Resampler.SetSampleRateRatio(FsOutToInRatio);
			PitchShifter.UpdatePitchShift(FMath::Clamp(PitchShiftInCents, -MaxPitchShiftCents, MaxPitchShiftCents) / 100.0f);
		

			// perform SRC and push to circular buffer until we have enough frames for the output
			while (Decoder && !(bDecoderIsDone || bDecoderHasLooped) && (OutputCircularBuffer.Num() < NumOutputSamples))
			{
				// get more audio from the decoder
				Audio::IDecoderOutput::FPushedAudioDetails Details;
				const Audio::IDecoder::EDecodeResult  DecodeResult = Decoder->Decode(bIsLooping);
				const int32 NumFramesDecoded = Output->PopAudio(PreSrcBuffer, Details) / NumChannels;

				bDecoderIsDone = DecodeResult == Audio::IDecoder::EDecodeResult::Finished;
				bDecoderHasLooped = DecodeResult == Audio::IDecoder::EDecodeResult::Looped;

				OutNumFramesConsumed += NumFramesDecoded;
				int32 NumResamplerOutputFrames = 0;
				int32 Error = Resampler.ProcessAudio(PreSrcBuffer.GetData(), NumFramesDecoded, bDecoderIsDone, PostSrcBuffer.GetData(), MaxNumResamplerOutputFramesPerBlock, NumResamplerOutputFrames);
				ensure(Error == 0);

				if (!PostSrcBuffer.Num() || !NumResamplerOutputFrames)
				{
					continue;
				}
				int32 OutputFrames = FMath::Min((int32)PostSrcBuffer.Num(), (int32)(NumResamplerOutputFrames * NumChannels));

				// perform linear pitch shift into OutputCircularBuffer
				if (NumResamplerOutputFrames > 0)
				{
					const TArrayView<float> BufferToPitchShift(PostSrcBuffer.GetData(), NumResamplerOutputFrames * NumChannels);
					PitchShifter.ProcessAudio(BufferToPitchShift, OutputCircularBuffer);
				}
			}
		}

		if (OutputCircularBuffer.Num() >= NumOutputSamples)
		{
			OutputCircularBuffer.Pop(OutputDest, NumOutputSamples);
		}
		else if (ensure(bDecoderHasLooped || bDecoderIsDone))
		{
			bDecoderHasLooped = false;

			const int32 NumSamplesToPop = OutputCircularBuffer.Num();
			const int32 NumSamplesRemaining = NumOutputSamples - NumSamplesToPop;
			OutputCircularBuffer.Pop(OutputDest, OutputCircularBuffer.Num());
			FMemory::Memzero(&OutputDest[NumSamplesToPop], sizeof(float) * NumSamplesRemaining);
			return NumSamplesToPop;
		}
		else
		{
			ensureMsgf(false, TEXT("Something went wrong with decoding."));
			bDecoderIsDone = true;
			return 0;
		}

		return NumOutputSamples; // update once we are aware of partial decode on last buffer
	}

	uint32 FSimpleDecoderWrapper::GetNumChannels() const
	{
		return NumChannels;
	}

	bool FSimpleDecoderWrapper::InitializeDecodersInternal(const FSoundWaveProxyPtr& InWave, float InStartTimeSeconds)
	{
		if (!ensure(InWave.IsValid()))
		{
			return false;
		}

		// Input:
		FName OldFormat = InWave->GetRuntimeFormat();
		// TODO: Why is this shared? Doesn't look like it needs to be shared. 
		Input = MakeShareable(Audio::CreateBackCompatDecoderInput(OldFormat, InWave).Release());
		Input->SeekToTime(InStartTimeSeconds);

		if (!Input)
		{
			return false;
		}

		// acquire codec:
		ICodecRegistry::FCodecPtr Codec = ICodecRegistry::Get().FindCodecByParsingInput(Input.Get());
		if (!Codec)
		{
			return false;
		}

		// specify requirements
		IDecoderOutput::FRequirements Reqs
		{
			Float32_Interleaved,
			static_cast<int32>(DecodeBlockSizeInFrames),
			static_cast<int32>(OutputSampleRate)
		};

		// Output:
		Output = IDecoderOutput::Create(Reqs);

		// Decoder:
		Decoder = Codec->CreateDecoder(Input.Get(), Output.Get());

		// return true if all the components were successfully create
		return Input.IsValid() && Output.IsValid() && Decoder.IsValid();
	}
} // namespace Audio
