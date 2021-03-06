// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelStreamingEncoderFactory.h"
#include "Misc/ScopeLock.h"
#include "PixelStreamingSettings.h"
#include "PixelStreamingVideoEncoder.h"
#include "Utils.h"
#include "absl/strings/match.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"

FPixelStreamingVideoEncoderFactory::FPixelStreamingVideoEncoderFactory(IPixelStreamingSessions* InPixelStreamingSessions)
	: PixelStreamingSessions(InPixelStreamingSessions)
{
	EncoderContext.Factory = this;
}

FPixelStreamingVideoEncoderFactory::~FPixelStreamingVideoEncoderFactory()
{
}

std::vector<webrtc::SdpVideoFormat> FPixelStreamingVideoEncoderFactory::GetSupportedFormats() const
{
	const bool bForceVP8 = PixelStreamingSettings::IsForceVP8();

	std::vector<webrtc::SdpVideoFormat> video_formats;
	if (bForceVP8)
	{
		video_formats.push_back(webrtc::SdpVideoFormat(cricket::kVp8CodecName));
	}
	else
	{
		video_formats.push_back(CreateH264Format(webrtc::H264::kProfileConstrainedBaseline, webrtc::H264::kLevel3_1));
	}

	return video_formats;
}

FPixelStreamingVideoEncoderFactory::CodecInfo FPixelStreamingVideoEncoderFactory::QueryVideoEncoder(const webrtc::SdpVideoFormat& format) const
{
	CodecInfo codec_info = {false, false};
	codec_info.is_hardware_accelerated = true;
	codec_info.has_internal_source = false;
	return codec_info;
}

std::unique_ptr<webrtc::VideoEncoder> FPixelStreamingVideoEncoderFactory::CreateVideoEncoder(const webrtc::SdpVideoFormat& format)
{
	if (absl::EqualsIgnoreCase(format.name, cricket::kVp8CodecName))
		return webrtc::VP8Encoder::Create();
	else
	{
		auto VideoEncoder = std::make_unique<FPixelStreamingVideoEncoder>(this->PixelStreamingSessions, &EncoderContext);
		UE_LOG(PixelStreamer, Log, TEXT("Encoder factory addded new encoder - soon to be associated with a player."));
		return VideoEncoder;
	}
}

void FPixelStreamingVideoEncoderFactory::RemoveStaleEncoders()
{
	// Lock during removing stale encoders
	FScopeLock FactoryLock(&this->FactoryCS);

	TArray<FPlayerId> ToRemove;

	for (auto& Entry : ActiveEncoders)
	{
		FPlayerId PlayerId = Entry.Key;
		FPixelStreamingVideoEncoder* Encoder = Entry.Value;

		// If WebRTC callback is no longer registered with the encoder it is considered stale and to be removed.
		if (!Encoder->IsRegisteredWithWebRTC())
		{
			ToRemove.Add(PlayerId);
		}
	}

	if (ToRemove.Num() > 0)
	{
		for (FPlayerId& PlayerId : ToRemove)
		{
			ActiveEncoders.Remove(PlayerId);
			UE_LOG(PixelStreamer, Log, TEXT("Encoder factory cleaned up stale encoder associated with PlayerId=%s"), *PlayerId);
		}
	}
}

void FPixelStreamingVideoEncoderFactory::OnPostEncode()
{
	FScopeLock FactoryLock(&this->FactoryCS);

	// If we have zero encoders now then shutdown the real hardware encoder too
	if (ActiveEncoders.Num() == 0 && EncoderContext.Encoder != nullptr)
	{
		UE_LOG(PixelStreamer, Log, TEXT("Encoder factory shutting down hardware encoder"));
		EncoderContext.Encoder->ClearOnEncodedPacket();
		EncoderContext.Encoder->Shutdown();
		EncoderContext.Encoder = nullptr;
	}
}

void FPixelStreamingVideoEncoderFactory::OnEncodedImage(const webrtc::EncodedImage& encoded_image, const webrtc::CodecSpecificInfo* codec_specific_info, const webrtc::RTPFragmentationHeader* fragmentation)
{
	// Before sending encoded image to each encoder's callback, check if all encoders we have are still relevant.
	this->RemoveStaleEncoders();

	// Lock as we send encoded image to each encoder.
	FScopeLock FactoryLock(&this->FactoryCS);

	// Go through each encoder and send our encoded image to its callback
	for (auto& Entry : ActiveEncoders)
	{
		FPixelStreamingVideoEncoder* Encoder = Entry.Value;
		if (Encoder->IsRegisteredWithWebRTC())
		{
			Encoder->SendEncodedImage(encoded_image, codec_specific_info, fragmentation);
		}
	}

	// Store the QP of this encoded image as we send the smoothed value to the peers as a proxy for encoding quality
	EncoderContext.SmoothedAvgQP.Update(encoded_image.qp_);
}

void FPixelStreamingVideoEncoderFactory::RegisterVideoEncoder(FPlayerId PlayerId, FPixelStreamingVideoEncoder* Encoder)
{
	// Lock during adding an encoder
	FScopeLock FactoryLock(&this->FactoryCS);
	ActiveEncoders.Add(PlayerId, Encoder);
}

void FPixelStreamingVideoEncoderFactory::UnregisterVideoEncoder(FPlayerId PlayerId)
{
	// Lock during deleting an encoder
	FScopeLock FactoryLock(&this->FactoryCS);

	if (ActiveEncoders.Contains(PlayerId))
	{
		FPixelStreamingVideoEncoder* PixelStreamingEncoder = this->ActiveEncoders[PlayerId];

		if(!PixelStreamingEncoder)
		{
			UE_LOG(PixelStreamer, Error, TEXT("Encoder factory tried to remove any already nullptr PixelStreamingVideoEncoder PlayerId=%s"), *PlayerId);
			return;
		}

		// This will signal this encoder is stale and it will get removed next time we finish encoding
		PixelStreamingEncoder->Release();

		// This will ensure we don't try to send another encoded frame to this encoder.
		ActiveEncoders.Remove(PlayerId);
		UE_LOG(PixelStreamer, Log, TEXT("Encoder factory asked to remove encoder for PlayerId=%s"), *PlayerId);

	}
}

void FPixelStreamingVideoEncoderFactory::ForceKeyFrame()
{
	FScopeLock FactoryLock(&this->FactoryCS);
	// Go through each encoder and send our encoded image to its callback
	for (auto& Entry : ActiveEncoders)
	{
		FPixelStreamingVideoEncoder* Encoder = Entry.Value;
		if (Encoder->IsRegisteredWithWebRTC())
		{
			Encoder->ForceKeyFrame();
		}
	}
}

double FPixelStreamingVideoEncoderFactory::GetLatestQP()
{
	return this->EncoderContext.SmoothedAvgQP.Get();
}