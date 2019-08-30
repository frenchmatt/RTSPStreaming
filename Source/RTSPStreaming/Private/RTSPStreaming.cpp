// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "RTSPStreaming.h"
#include "RTSPStreamingCommon.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "Slate/SceneViewport.h"
#include "Controller.h"
#include "RenderingThread.h"
#include "RendererInterface.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include <SocketSubsystem.h>
#include <IPAddress.h>

#define LOCTEXT_NAMESPACE "FRTSPStreamingModule"

DEFINE_LOG_CATEGORY(RTSPStreaming);

void FRTSPStreamingModule::StartupModule()
{
	//parses for RTSP disable
	FString enableStream = TEXT("false");
	FParse::Value(FCommandLine::Get(), TEXT("DisableRTSPStream="), enableStream);
	if (enableStream == "true") 
	{
		//return before any module functions can be attached to the render thread
		return;
	}

	// detect hardware capabilities, init nvidia capture libs, etc
	check(GDynamicRHI);
	void* Device = GDynamicRHI->RHIGetNativeDevice();
	// During cooking RHI device is invalid, skip error logging in this case as it causes the build to fail.
	if (Device)
	{
		FString RHIName = GDynamicRHI->GetName();
		if (RHIName != TEXT("D3D11"))
		{
			UE_LOG(RTSPStreaming, Error, TEXT("Failed to initialise RTSP Streaming plugin because it only supports DX11"));
			return;
		}
	}

	// subscribe to engine delegates here for init / framebuffer creation / whatever
	if (UGameEngine * GameEngine = Cast<UGameEngine>(GEngine))
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FRTSPStreamingModule::OnBackBufferReady_RenderThread);
			FSlateApplication::Get().GetRenderer()->OnPreResizeWindowBackBuffer().AddRaw(this, &FRTSPStreamingModule::OnPreResizeWindowBackbuffer);
		}
	}	
}

void FRTSPStreamingModule::ShutdownModule()
{
	//unsubscribe to engine delegates here
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().RemoveAll(this);
		FSlateApplication::Get().GetRenderer()->OnPreResizeWindowBackBuffer().RemoveAll(this);
	}
}

void FRTSPStreamingModule::UpdateViewport(FSceneViewport* Viewport)
{
	//gets reference to viewport for after update
	FRHIViewport* const ViewportRHI = Viewport->GetViewportRHI().GetReference();
}

void FRTSPStreamingModule::OnBackBufferReady_RenderThread(SWindow& SlateWindow, const FTexture2DRHIRef& BackBuffer)
{
	check(IsInRenderingThread());

	if (!Controller)
	{
		//parses for RTSP server configuration
		FString ServerIP = TEXT("127.0.0.1");
		FParse::Value(FCommandLine::Get(), TEXT("RTSPStreamingIP="), ServerIP);
		uint16 ServerPort = 8554;
		FParse::Value(FCommandLine::Get(), TEXT("RTSPStreamingPort="), ServerPort);

		//creates new controller
		Controller = MakeUnique<FController>(*ServerIP, ServerPort, BackBuffer);
	}

	//passes buffer to controller
	Controller->OnFrameBufferReady(BackBuffer);
}

void FRTSPStreamingModule::OnPreResizeWindowBackbuffer(void* BackBuffer)
{
	if (Controller)
	{
		FRTSPStreamingModule* Plugin = this;
		ENQUEUE_RENDER_COMMAND(FRTSPStreamingModuleOnPreResizeWindowBackbuffer)(
			[Plugin](FRHICommandListImmediate& RHICmdList)
			{
				Plugin->OnPreResizeWindowBackbuffer_RenderThread();
			});

		// Make sure OnPreResizeWindowBackbuffer_RenderThread is executed before continuing
		FlushRenderingCommands();
	}
}

void FRTSPStreamingModule::OnPreResizeWindowBackbuffer_RenderThread()
{
	Controller->OnPreResizeWindowBackbuffer();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FRTSPStreamingModule, RTSPStreaming)