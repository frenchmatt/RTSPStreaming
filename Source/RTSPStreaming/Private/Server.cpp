// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Server.h"

FServer::FServer(const FString& IP, uint16 Port, FController& Controller) 
	: Controller(Controller)
	, ExitRequested(false)
	, Thread(TEXT("Server Listener"), [this, IP, Port]() { Run(IP, Port); })
{}

FServer::~FServer()
{
	ExitRequested = true;

	//destroy listener socket
	if (ListenerSocket) 
	{
		FScopeLock Lock(&ListenerSocketMt);
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
	}

	//end thread
	Thread.Join();
}

void FServer::Run(const FString& ServerIP, uint16 ServerPort)
{
	// listen to a single incoming connection from Client
	FIPv4Address BindToAddr;
	bool bResult = FIPv4Address::Parse(ServerIP, BindToAddr);
	checkf(bResult, TEXT("Failed to parse IPv4 address %s"), *ServerIP);

	//builds and checks listener socket
	{
		FScopeLock Lock(&ListenerSocketMt);
		ListenerSocket = FTcpSocketBuilder(TEXT("Client Listener")).
			AsBlocking().
			AsReusable().
			Listening(1).
			BoundToAddress(BindToAddr).
			BoundToPort(ServerPort).
			WithSendBufferSize(10000).
			Build();
		check(ListenerSocket);
	}

	UE_LOG(RTSPStreaming, Log, TEXT("Waiting for connection from Client on %s:%d"), *ServerIP, ServerPort);

	while (!ExitRequested)
	{
		//blocking call accepts incomming client connection
		FSocket* ClientSocket = ListenerSocket->Accept(TEXT("Client"));
		if (!ClientSocket) // usually happens on exit because Listener was closed in destructor
		{
			return;
		}

		//gets client IP
		TSharedPtr<FInternetAddr> ClientAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		ClientSocket->GetPeerAddress(*ClientAddr);

		{
			FScopeLock Lock(&ClientListMt);
			
			//removes dead client streamers from active list
			ClientList.RemoveAllSwap([](FStreamer& ClientStreamer3) { return ClientStreamer3.isDead() == 1; }, true);

			//tells controller to stop passing data if no active clients exist
			if (!ClientList.Num()) 
			{
				Controller.StopStreaming();
			}

			//adds new incomming connection streamer to active list
			ClientList.Emplace(ClientSocket, ServerIP, ClientAddr, *this);
			UE_LOG(RTSPStreaming, Log, TEXT("+%d Accepted connection from Client: %s"), ClientList.Num(), *ClientAddr->ToString(true));
		}
	}

	UE_LOG(RTSPStreaming, Log, TEXT("Client Connection thread exited"));

}

bool FServer::Send(uint64 Timestamp, const uint8* Data, uint32 Size)
{	
	FScopeLock Lock(&ClientListMt);

	//iterates through client streamers
	for (FStreamer& ClientStreamer2 : ClientList)
	{
		//checks if streamer has set up sending sockets and if PLAY was received
		if (ClientStreamer2.isReady())
		{
			//passes encoded frames to client streamers
			if (!ClientStreamer2.Send(Timestamp, Data, Size))
			{
				return false;
			}
		}
	}
	return true;
}


