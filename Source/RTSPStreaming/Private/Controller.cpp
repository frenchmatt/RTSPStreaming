// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Controller.h"
#include "UnrealClient.h"
#include "HAL/Runnable.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "NvVideoEncoder.h"
#include "RTSPStreamingCommon.h"
#include "Utils.h"
#include "Server.h"

DECLARE_DWORD_COUNTER_STAT(TEXT("EncodingFramerate"), STAT_RTSPStreaming_EncodingFramerate, STATGROUP_RTSPStreaming);
DECLARE_DWORD_COUNTER_STAT(TEXT("EncodingBitrate"), STAT_RTSPStreaming_EncodingBitrate, STATGROUP_RTSPStreaming);

TAutoConsoleVariable<int32> CVarEncoderAverageBitRate(
	TEXT("Encoder.AverageBitRate"),
	20000000,
	TEXT("Encoder bit rate before reduction for B/W jitter"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarEncoderMaxBitrate(
	TEXT("Encoder.MaxBitrate"),
	100000000,
	TEXT("Max bitrate no matter what WebRTC says, in Mbps"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<FString> CVarEncoderTargetSize(
	TEXT("Encoder.TargetSize"),
	TEXT("1280x720"),
	TEXT("Encoder target size in format widthxheight"),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarEncoderUseBackBufferSize(
	TEXT("Encoder.UseBackBufferSize"),
	1,
	TEXT("Whether to use back buffer size or custom size"),
	ECVF_Cheat);

TAutoConsoleVariable<int32> CVarStreamerPrioritiseQuality(
	TEXT("Streamer.PrioritiseQuality"),
	0,
	TEXT("Reduces framerate automatically on bitrate reduction to trade FPS/latency for video quality"),
	ECVF_Cheat);

TAutoConsoleVariable<int32> CVarStreamerLowBitrate(
	TEXT("Streamer.LowBitrate"),
	2000,
	TEXT("Lower bound of bitrate for quality adaptation, Kbps"),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarStreamerHighBitrate(
	TEXT("Streamer.HighBitrate"),
	10000,
	TEXT("Upper bound of bitrate for quality adaptation, Kbps"),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarStreamerMinFPS(
	TEXT("Streamer.MinFPS"),
	10,
	TEXT("Minimal FPS for quality adaptation"),
	ECVF_Default);

TAutoConsoleVariable<float> CVarStreamerBitrateReduction(
	TEXT("Streamer.BitrateReduction"),
	50.0,
	TEXT("How much to reduce WebRTC reported bitrate to handle bitrate jitter, in per cent"),
	ECVF_RenderThreadSafe);

const int32 DefaultFPS = 60;

FController::FController(const TCHAR* ServerIP, uint16 ServerPort, const FTexture2DRHIRef& FrameBuffer)
	: bResizingWindowBackBuffer(false)
	, bStreamingStarted(false)
	, InitialMaxFPS(GEngine->GetMaxFPS())
{
	//sets initial FPS for encoder and game
	if (InitialMaxFPS == 0)
	{
		InitialMaxFPS = DefaultFPS;

		check(IsInRenderingThread());
		// we are in the rendering thread but `GEngine->SetMaxFPS()` can be called only in the main thread
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			GEngine->SetMaxFPS(InitialMaxFPS);
		});
	}

	//creates new server
	Server.Reset(new FServer(ServerIP, ServerPort, *this));

	//creates and updates video encoder
	UpdateEncoderSettings(FrameBuffer, InitialMaxFPS);
	CreateVideoEncoder(FrameBuffer);

	UE_LOG(RTSPStreaming, Log, TEXT("Server created: %dx%d %d FPS%s"),
		VideoEncoderSettings.Width, VideoEncoderSettings.Height,
		InitialMaxFPS,
		CVarStreamerPrioritiseQuality.GetValueOnAnyThread() != 0 ? TEXT("FController::FController  , prioritise quality") : TEXT(""));
}

// must be in cpp file cos TUniquePtr incomplete type
FController::~FController()
{}

void FController::CreateVideoEncoder(const FTexture2DRHIRef& FrameBuffer)
{
	//creates encoder
	VideoEncoder.Reset(new FNvVideoEncoder(VideoEncoderSettings, FrameBuffer, [this](uint64 Timestamp, bool KeyFrame, const uint8* Data, uint32 Size)
	{
		Stream(Timestamp, KeyFrame, Data, Size);
	}));

	checkf(VideoEncoder->IsSupported(), TEXT("FController::CreateVideoEncoder  Failed to initialize NvEnc"));
	UE_LOG(RTSPStreaming, Log, TEXT("FController::CreateVideoEncoder  NvEnc initialised"));
}

void FController::OnFrameBufferReady(const FTexture2DRHIRef& FrameBuffer)
{
	//stops passing data if no connected clients
	if (!bStreamingStarted)
	{
		return;
	}

	//get time for encoder latency logging
	uint64 CaptureMs = NowMs();

	// VideoEncoder is reset on disconnection
	if (!VideoEncoder)
	{
		CreateVideoEncoder(FrameBuffer);
		check(VideoEncoder);
	}

	if (bResizingWindowBackBuffer)
	{
		// Re-initialize video encoder if it has been destroyed by OnPreResizeWindowBackbuffer()
		VideoEncoder->PostResizeBackBuffer();
		bResizingWindowBackBuffer = false;
	}

	//encodes a frame from backbuffer
	UpdateEncoderSettings(FrameBuffer);
	VideoEncoder->EncodeFrame(VideoEncoderSettings, FrameBuffer, CaptureMs);
}



void FController::OnPreResizeWindowBackbuffer()
{
	// Destroy video encoder before resizing window so it releases usage of graphics device & back buffer.
	// It's recreated later on in OnFrameBufferReady().
	VideoEncoder->PreResizeBackBuffer();
	bResizingWindowBackBuffer = true;
}

void FController::Stream(uint64 Timestamp, bool Keyframe, const uint8* Data, uint32 Size)
{
	if (bStreamingStarted)
	{
		//passes encoded frame to server
		if (!Server->Send(Timestamp, Data, Size))
		{
			UE_LOG(RTSPStreaming, Log, TEXT("Could not send %s, %d bytes"), Keyframe ? "IDRFrame" : "", Size);
		}
	}
}

void FController::ForceIdrFrame()
{
	if (VideoEncoder)
	{
		VideoEncoder->ForceIdrFrame();
	}
}

void FController::UpdateEncoderSettings(const FTexture2DRHIRef& FrameBuffer, int32 Fps)
{
	float MaxBitrateMbps = CVarEncoderMaxBitrate.GetValueOnRenderThread();

	// HACK(andriy): We reduce WebRTC reported bitrate to compensate for B/W jitter. We have long pipeline
	// before passing encoded frames to WebRTC and a couple of frames are already in the pipeline when
	// WebRTC reports lower bitrate. This often causes that WebRTC Rate Limiter or network drop frames
	// because they exceed available bandwidth. While significant bandwidth drop are not expected to
	// happen often small jitter is possible and causes frequent video distortion. Reducing reported bitrate
	// by a small percentage gives us a chance to avoid frame drops on bandwidth jitter.
	// There're couple of drawbacks:
	// - minor one - we don't use all available bandwidth to achieve best possible quality
	// - major one - we don't use all available bandwidth and in case of network congestion
	// other connections can get upper hand and depress bandwidth allocated for streaming even more.
	// Proper feasible solution is unknown atm.
	//
	// do reduction here instead of e.g. `SetBitrate` because this method is called on every frame and so
	// changes to `CVarStreamerBitrateReduction` will be immediately picked up
	float BitrateReduction = CVarStreamerBitrateReduction.GetValueOnRenderThread();
	uint32 Bitrate = CVarEncoderAverageBitRate.GetValueOnRenderThread();
	uint32 ReducedBitrate = static_cast<uint32>(Bitrate / 100.0 * (100.0 - BitrateReduction));
	ReducedBitrate = FMath::Min(ReducedBitrate, static_cast<uint32>(MaxBitrateMbps * 1000 * 1000));
	VideoEncoderSettings.AverageBitRate = ReducedBitrate;
	SET_DWORD_STAT(STAT_RTSPStreaming_EncodingBitrate, VideoEncoderSettings.AverageBitRate);

	VideoEncoderSettings.FrameRate = Fps >= 0 ? Fps : GEngine->GetMaxFPS();
	SET_DWORD_STAT(STAT_RTSPStreaming_EncodingFramerate, VideoEncoderSettings.FrameRate);

	bool bUseBackBufferSize = CVarEncoderUseBackBufferSize.GetValueOnAnyThread() > 0;
	if (bUseBackBufferSize)
	{
		VideoEncoderSettings.Width = FrameBuffer->GetSizeX();
		VideoEncoderSettings.Height = FrameBuffer->GetSizeY();
	}
	else
	{
		FString EncoderTargetSize = CVarEncoderTargetSize.GetValueOnAnyThread();
		FString TargetWidth, TargetHeight;
		bool bValidSize = EncoderTargetSize.Split(TEXT("x"), &TargetWidth, &TargetHeight);
		if (bValidSize)
		{
			VideoEncoderSettings.Width = FCString::Atoi(*TargetWidth);
			VideoEncoderSettings.Height = FCString::Atoi(*TargetHeight);
		}
	}
}

void FController::SetBitrate(uint16 Kbps)
{
	UE_LOG(RTSPStreaming, Log, TEXT("Set Bitrate to %d Kbps"), Kbps);

	AsyncTask(ENamedThreads::GameThread, [Kbps]()
		{
			CVarEncoderAverageBitRate->Set(Kbps * 1000);
		});

	// reduce framerate proportionally to WebRTC reported bitrate to prioritise quality over FPS/latency
	// by lowering framerate we allocate more bandwidth to fewer frames, thus increasing quality
	if (CVarStreamerPrioritiseQuality.GetValueOnAnyThread())
	{
		int32 Fps;

		// bitrate lower than lower bound results always in min FPS
		// bitrate between lower and upper bounds results in FPS proportionally between min and max FPS
		// bitrate higher than upper bound results always in max FPS
		const uint16 LowerBoundKbps = CVarStreamerLowBitrate.GetValueOnAnyThread();
		const int32 MinFps = FMath::Min(CVarStreamerMinFPS.GetValueOnAnyThread(), InitialMaxFPS);
		const uint16 UpperBoundKbps = CVarStreamerHighBitrate.GetValueOnAnyThread();
		const int32 MaxFps = InitialMaxFPS;

		if (Kbps < LowerBoundKbps)
		{
			Fps = MinFps;
		}
		else if (Kbps < UpperBoundKbps)
		{
			Fps = MinFps + static_cast<uint8>(static_cast<double>(MaxFps - MinFps) / (UpperBoundKbps - LowerBoundKbps) * (Kbps - LowerBoundKbps));
		}
		else
		{
			Fps = MaxFps;
		}

		SetFramerate(Fps);
	}
}

void FController::SetFramerate(int32 Fps)
{
	UE_LOG(RTSPStreaming, Log, TEXT("Set Framerate to %d Fps"), Fps);

	AsyncTask(ENamedThreads::GameThread, [Fps]()
		{
			GEngine->SetMaxFPS(Fps);
		});
}