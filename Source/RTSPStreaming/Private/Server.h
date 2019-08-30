// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"
#include "Templates/SharedPointer.h"
#include "Utils.h"
#include "Controller.h"
#include "Streamer.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "RTSPStreamingCommon.h"

class FSocket;

// encapsulates TCP connection to Client
// accepts a single connection from Client, in a loop, accepts a new one once the previous disconnected
// allows sending data to the connection
// runs an internal thread for receiving data, deserialises "Proxy -> UE4" protocol messages and calls 
// appropriate handlers from that internal thread
class FServer final
{
private:
	FServer(const FServer&) = delete;
	FServer& operator=(const FServer&) = delete;

public:
	FServer(const FString& ServerIP, uint16 ServerPort, FController& Controller);	
	~FServer();

	void Run(const FString& ServerIP, uint16 ServerPort);			// Server listener thread
	bool Send(uint64 Timestamp, const uint8* Data, uint32 Size);	// passes data to client Sessions in ClientList

	void StartStreaming()					//tells controller to start streaming
	{
		Controller.StartStreaming();
	}

private:
	FController&		Controller;		
	FCriticalSection	ClientListMt;		// thread lock for ClientList
	TArray<FStreamer>	ClientList;			// list of active client Sessions
	FCriticalSection	ListenerSocketMt;	// thread lock for ListenerSocket
	FSocket*			ListenerSocket;		// socket Listener for incomming client connections
	FThreadSafeBool		ExitRequested;		// true if thread should close
	FThread				Thread;				// listener thread
};