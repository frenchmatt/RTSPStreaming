// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "VideoEncoder.h"
#include "RHI.h"
#include "RHIResources.h"
#include "Engine/GameViewportClient.h"
#include "HAL/ThreadSafeBool.h"

DECLARE_STATS_GROUP(TEXT("RTSPStreaming"), STATGROUP_RTSPStreaming, STATCAT_Advanced);

class FRenderTarget;
class FServer;
struct ID3D11Device;

class FController final
{
private:
	FController(const FController&) = delete;
	FController& operator=(const FController&) = delete;

public:
	FController(const TCHAR* ServerIP, uint16 ServerPort, const FTexture2DRHIRef& FrameBuffer);
	virtual ~FController();

	void OnFrameBufferReady(const FTexture2DRHIRef& FrameBuffer);	// attached from render thread - at each frame
	void OnPreResizeWindowBackbuffer();								// attached from render thread - at buffer resize from res change, etc.
	void ForceIdrFrame();											// forces encoder to make next available frame I-Frame

	void StartStreaming()											// called when client connects
	{
		bStreamingStarted = true;
		ForceIdrFrame();
	}

	void StopStreaming()											// called when no active clients connected
	{
		bStreamingStarted = false;
	}

	void SetBitrate(uint16 Kbps);									// changes encoder params
	void SetFramerate(int32 Fps);									// changes encoder params

private:
	void CreateVideoEncoder(const FTexture2DRHIRef& FrameBuffer);						// creates encoder
	void UpdateEncoderSettings(const FTexture2DRHIRef& FrameBuffer, int32 Fps = -1);	// updates encoder
	void Stream(uint64 Timestamp, bool Keyframe, const uint8* Data, uint32 Size);		// passes data to Server

private:
	bool						bResizingWindowBackBuffer;			// true when encoder needs to be updated from buffer resize
	FVideoEncoderSettings		VideoEncoderSettings;				// struct for holding encoder params
	TUniquePtr<IVideoEncoder>	VideoEncoder;				
	TUniquePtr<FServer>			Server;

	// we shouldn't start streaming immediately after client is connected because
	// encoding pipeline is not ready yet and a couple of first frames can be lost.
	// instead wait for an explicit command to start streaming
	FThreadSafeBool				bStreamingStarted;					// true when at least one active client connected
	int32						InitialMaxFPS;						// 60 FPS
};