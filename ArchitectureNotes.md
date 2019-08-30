# RTSPStreaming Plugin Architecture
## Class Overview


### FRTSPStreamingModule

This class is the first point of execution. The streamer can be disabled at this point. By returning from StartupModule, functions won't be linked and the streamer will never access Unreal's render thread frame buffer. This allows reducting the number of unique builds needed for an architecture with multiple instances, of which not all should be streaming.

```
FString enableStream = TEXT("false");
FParse::Value(FCommandLine::Get(), TEXT("DisableRTSPStream="), enableStream);
if (enableStream == "true") {	return;   }
```

Currently, this disable is set with the command line flag "-DisableRTSPStream=true", but I assume that this will later be changed. One implementation I wrote is the following:

```
FString File = TEXT("");
FParse::Value(FCommandLine::Get(), TEXT("Config="), File);

FString Filepath = TEXT("");
Filepath += FPaths::RootDir() + File;  

FString Config = TEXT("");

IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
if (PlatformFile.FileExists(*Filepath))
{
	FFileHelper::LoadFileToString(Config, *Filepath);
}

FString enableStream = TEXT("false");
FParse::Value(*Config, TEXT("DisableRTSPStream="), enableStream);
if (enableStream == "true") {	return;   }
```
This implementation reads a file specified by commandline with “-Config=”. Within the passed file there shoule exist the “-DisableRTSPStreaming=true” flag.

Assuming the plugin has not been disabled, it first initializes devices by checking for the correct GPU API, namely Direct 3D 11.

```
void* Device = GDynamicRHI->RHIGetNativeDevice();
```

Second, it also adds plugin module functions to the rendering thread's execution path.

```
FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FRTSPStreamingModule::OnBackBufferReady_RenderThread);
FSlateApplication::Get().GetRenderer()->OnPreResizeWindowBackBuffer().AddRaw(this, &FRTSPStreamingModule::OnPreResizeWindowBackbuffer);
```

After the initial startup during the PostEnginInit portion of initialization (specified in the .uplugin file), these two functions will call all other plugin functionality. The first function is called whenever a frame is rendered. The second is called when the resolution is changed or the buffer size changes. 

```
void FRTSPStreamingModule::OnBackBufferReady_RenderThread(SWindow& SlateWindow, const FTexture2DRHIRef& BackBuffer)
{
    check(IsInRenderingThread());

    if (!Controller)
    {
        FString IP = TEXT("127.0.0.1");
        FParse::Value(FCommandLine::Get(), TEXT("RTSPStreamingIP="), IP);
        uint16 Port = 8554;
        FParse::Value(FCommandLine::Get(), TEXT("RTSPStreamingPort="), Port);

        Controller = MakeUnique<FController>(*IP, Port, BackBuffer);
    }
    
    Controller->OnFrameBufferReady(BackBuffer);
}
```
     
OnBackBufferReady_RenderThread creates the Controller with IP and Port parameters for later use. The line that creates the Controller is the only place the IP and Port variables are used. Therefore, it is here that the configuration parser should pass its parameters. Currently, they are set to read from command line, but I assume that this will later be changed. One implementation I wrote is the following:

```

FString File = TEXT("");
FParse::Value(FCommandLine::Get(), TEXT("Config="), File);

FString Filepath = TEXT("");
Filepath += FPaths::RootDir() + File;  

FString Config = TEXT("");

IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
if (PlatformFile.FileExists(*Filepath))
{
	FFileHelper::LoadFileToString(Config, *Filepath);
}

FString ServerIP = TEXT("127.0.0.1");
FParse::Value(*Config, TEXT("RTSPStreamingIP="), ServerIP);
uint16 ServerPort = 8554;
FParse::Value(*Config, TEXT("RTSPStreamingPort="), ServerPort);


Controller = MakeUnique<FController>(*ServerIP, ServerPort, BackBuffer);
```
This implementation reads a file specified by commandline with "-Config=". Within the passed file there shoule exist the "-RTSPStreamingIP=" and "-RTSPStreamingPort=" flags. This function then passes execution and the rendered data to the Controller's own function through a reference to the render thread's frame buffer.

### FController

First, the Controller object is the link between the encoder, renderer, and server. Second, it is a second layer abstraction for the existing NvVideoEncoder interface. The controller creates and updates the encoder settings like bitrate and framerate. Third, it also exists to force the encoder to produce SPS and PPS data as well as force I-Frames. It also passes execution and the rendered data to a Server object which it creates in its constructor.

The Controller in mostly in charge of housekeeping and logistics. Here, it passes the current time and packet type to the Server.

```
void FController::Stream(uint64 Timestamp, bool Keyframe, const uint8* Data, uint32 Size)
{
	if (bStreamingStarted)
	{
		if (!Server->Send(Timestamp, Data, Size))
		{
			UE_LOG(RTSPStreaming, Log, TEXT("Could not send %s, %d bytes"), Keyframe ? "IDRFrame" : "", Size);
		}
	}
}
```  

### FNvVideoEncoder

This class was left mostly unchanged from the PixelStreaming plugin included with the engine. That plugin was already streaming H.264 encoded video, so I pretty much left it exactly how it was in order not to break what already works. What I do know about it is that it is an interface to the NVEncodeAPI which is a GPU accelerated encoder API. The NvVideoEncoder class is pretty rough. There are lots of commented out code pieces and little notes that lead me to believe this isn't fully finished.

### FServer

The Server object is the manager of client connections. It is separate from the Controller object mostly because it lives within its own thread and it creates and manages its own child threads. The Server owns a socket over which it listens for incomming connections. It also keeps an array of active clients, and it creates a new thread which negotiates RTSP and sends data.

This is the Server thread's main loop.

```
while (!ExitRequested)
{

	FSocket* ClientSocket = ListenerSocket->Accept(TEXT("Client"));
	if (!ClientSocket) // usually happens on exit because Listener was closed in destructor
	{
		return;
	}
    
    TSharedPtr<FInternetAddr> ClientAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	ClientSocket->GetPeerAddress(*ClientAddr);

	{
		FScopeLock Lock(&ClientListMt);
		
		ClientList.RemoveAllSwap([](FStreamer& ClientStreamer3) { return ClientStreamer3.isDead() == 1; }, true);

		ClientList.Emplace(ClientSocket, ServerIP, ClientAddr, *this);
		UE_LOG(RTSPStreaming, Log, TEXT("+%d Accepted connection from Client: %s"), ClientList.Num(), *ClientAddr->ToString(true));
	}
}
```   
    
The Server first listens for incoming connections, then when one is accepted, it first removes from its array all client Streamers which have become dead (set from within the child thread by a flag). Then it creates a new Streamer and adds it to the active array. The Server spawns child Streamer threads because each client can asynchronously negotiate RTSP. Also, an RTSP message must be able to be received at any point by any client. The server also passes data to each client Streamer according to a ready flag set from within the child thread.

```
bool FServer::Send(uint64 Timestamp, const uint8* Data, uint32 Size)
{	
	FScopeLock Lock(&ClientListMt);

	for (FStreamer& ClientStreamer2 : ClientList)
	{
		if (ClientStreamer2.isReady())
		{
			if (!ClientStreamer2.Send(Timestamp, Data, Size))
			{
				return false;
			}
		}
	}
	return true;
}
```
### FStreamer

The streamer is where all the low-level magic happens. It is responsible for setting status flags, initializing sockets, listening and negotiating RTSP, and packetizing and sending data. It is a very large and important class. This is its main loop:

```
while (!ExitReceive)
{
	uint8 BitBuf[2000];
	int32 BytesRead = 0;
	{
		FScopeLock Lock(&StreamerMt);
		if (!RTSPSocket->Recv(BitBuf, 2000, BytesRead))
		{
			...
		}
	}

	//Convert int8 array to char array
	char* RecvBuf = reinterpret_cast<char*>(BitBuf);

    // we filter away everything which seems not to be an RTSP command: O-ption, D-escribe, S-etup, P-lay, T-eardown
	if ((RecvBuf[0] == 'O') || (RecvBuf[0] == 'D') || (RecvBuf[0] == 'S') || (RecvBuf[0] == 'P') || (RecvBuf[0] == 'T'))
	{
		RTSP_CMD_TYPES C = Handle_RTSPRequest(RecvBuf, BytesRead);
		if (C == RTSP_PLAY)
		{
			if (bSocketsReady)
			{
				FScopeLock Lock(&StreamerMt);
				bStreamerReady = true;
				Server.StartStreaming();
			}
		}
		else if (C == RTSP_TEARDOWN)
		{
			FScopeLock Lock(&StreamerMt);
			ExitReceive = true;
			//UE_LOG(RTSPStreaming, Log, TEXT("%d: ExitReceive(t) TEARDOWN"), ClientRTSPPort);
		}
	}
}
...
```

The streamer here listens for RTSP. Then according to the message received, it will use a message handler to do various things. For example, a SETUP message will cause RTP and RTCP sockets to be created and the ready flag to be set.

The other important piece of the Streamer is its packatization and sending of data. This function is called by the parent server thread:

```
bool FStreaming::Send(uint64 Timestamp, RTSPStreamingProtocol::EToProxyMsg PktType, const uint8* Data, uint32 Size)
{
    if (!bSocketsReady) return false;

#define KRtpHeaderSize 12           // size of the RTP header

    uint8        RtpBuf[50000];
    TSharedRef<FInternetAddr> RecvAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    int RtpPacketSize = Size + KRtpHeaderSize;

    // get client address for UDP transport
    m_Client->GetPeerAddress(*RecvAddr);
    RecvAddr->SetPort(m_RtpClientPort);

    memset(RtpBuf, 0x00, sizeof(RtpBuf));
    // Prepare the first 4 byte of the packet. This is the Rtp over Rtsp header in case of TCP based transport
    RtpBuf[0] = '$';        // magic number
    RtpBuf[1] = 0;          // number of multiplexed subchannel on RTPS connection - here the RTP channel
    RtpBuf[2] = (RtpPacketSize & 0x0000FF00) >> 8;
    RtpBuf[3] = (RtpPacketSize & 0x000000FF);
    // Prepare the 12 byte RTP header
    RtpBuf[4] = 0x80;                               // RTP Version - 0b10, 0b0 - Padding, 0b0 - Extensionn, 0b0000 - CSRC count
    RtpBuf[5] = 0xFD;                               // Marker - 0b1, H.264 payload - 125? (dynamic)
    RtpBuf[7] = m_SequenceNumber & 0x0FF;           // each packet is counted with a sequence counter
    RtpBuf[6] = m_SequenceNumber >> 8;
    RtpBuf[8] = (Timestamp & 0x00000000FF000000) >> 24;   // each image gets a timestamp
    RtpBuf[9] = (Timestamp & 0x0000000000FF0000) >> 16;
    RtpBuf[10] = (Timestamp & 0x000000000000FF00) >> 8;
    RtpBuf[11] = (Timestamp & 0x00000000000000FF);
    RtpBuf[12] = 0x13;                               // 4 byte SSRC (sychronization source identifier)
    RtpBuf[13] = 0xf9;                               // we just an arbitrary number here to keep it simple
    RtpBuf[14] = 0x7e;
    RtpBuf[15] = 0x67;

    if (Size > 49500) {return false;}
    memcpy(&RtpBuf[16], Data, Size);

    m_SequenceNumber++;                              // prepare the packet counter for the next packet

    if (m_TCPTransport) // RTP over RTSP - we send the buffer + 4 byte additional header
    {
        FScopeLock Lock(&m_ClientMt);
        if (m_Client)
        {
            int BytesSent = 0;
            int& sent = BytesSent;
            m_Client->Send(RtpBuf, RtpPacketSize + 4, sent);
            return true;
        }
    }
    else                // UDP - we send just the buffer by skipping the 4 byte RTP over RTSP header
    {
        FScopeLock Lock(&m_RtpSocketMt);
        if (m_RtpSocket)
        {
            int BytesSent = 0;
            int& sent = BytesSent;
            m_RtpSocket->SendTo(&RtpBuf[4], RtpPacketSize, sent, *RecvAddr);
            return true;
        }
    }
    return false;
}
```

The packet is built manually here for the RTP and H.264 parameters required. Then data is loaded as payload and sent.