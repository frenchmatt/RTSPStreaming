// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "Modules/ModuleInterface.h"
#include "RTSPStreamingCommon.h"

class FSceneViewport;
class SWindow;

class FRTSPStreamingModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:

	void UpdateViewport(FSceneViewport* Viewport);													// changes viewport when camera changes (consider removing)
	void OnBackBufferReady_RenderThread(SWindow& SlateWindow, const FTexture2DRHIRef& BackBuffer);	// attached from render thread - at each frame
	void OnPreResizeWindowBackbuffer(void* BackBuffer);												// attached from render thread - at buffer resize from res change, etc.
	void OnPreResizeWindowBackbuffer_RenderThread();												// attached from render thread - at buffer resize from res change, etc.

	TUniquePtr<FController>		Controller;
	FTexture2DRHIRef			mResolvedFrameBuffer;
};