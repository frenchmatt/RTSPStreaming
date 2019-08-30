// MediaLAN 02/2013
// CRTSPSession
// - JPEG packetizer and UDP/TCP based streaming

#include "Streamer.h"
#include "Engine/Engine.h"
#include "SocketSubsystem.h"
#include "SocketTypes.h"
#include "Networking.h"

#define RTPBUFFERSIZE 1280 * 720 * 10

FStreamer::FStreamer(FSocket* aRTSPSocket, const FString aServerIP, TSharedPtr<FInternetAddr> aClientAddr, FServer& aServer)
	: RTPSocket(nullptr)
	, RTCPSocket(nullptr)
	, RTSPSocket(aRTSPSocket)
	, ClientRTSPPort(aClientAddr->GetPort())
	, ClientRTPPort(0)
	, ClientRTCPPort(0)
	, ServerRTPPort(0)
	, ServerRTCPPort(0)
	, SequenceNumber(0)
	, bTCPTransport(false)
	, ServerIP(aServerIP)
	, ClientIP(*aClientAddr->ToString(false))
	, bSocketsReady(false)
	, RTSPSessionID(rand() << 16 | rand() | 0x80000000)
	, bValid(0)
	, Server(aServer)
	, ExitReceive(false)
	, bStreamerReady(false)
	, bDestroyStreamer(false)
	, Thread((TEXT("Client Session: %s"), *aClientAddr->ToString(true)), [this] { Run(); })
{}

FStreamer::~FStreamer()
{
	{
		FScopeLock Lock(&StreamerMt);
		ExitReceive = true;
		//UE_LOG(RTSPStreaming, Log, TEXT("%d: ExitReceive(t) DTOR CALLED"), ClientRTSPPort);
	}

	//ends thread
	Thread.Join();
	
	//destroys sending sockets
	{
		FScopeLock Lock(&StreamerMt);
		if (RTPSocket)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(RTPSocket);
			RTPSocket = nullptr;
		}

		if (RTCPSocket)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(RTCPSocket);
			RTCPSocket = nullptr;
		}

		if (RTSPSocket)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(RTSPSocket);
			RTSPSocket = nullptr;
		}
	}

	//sets status flags
	{
		FScopeLock Lock(&StreamerMt);
		bStreamerReady = false;
		bDestroyStreamer = true;
		//UE_LOG(RTSPStreaming, Log, TEXT("%d: bStreamerReady(f), bDestroyStreamer(t) DTOR CLOSED"), ClientRTSPPort);
	}
}

//checks for streamer equality by client IP:Port
bool FStreamer::operator==(const FStreamer& rhs) const
{
	return ((this->ClientIP == rhs.ClientIP) &&
		(this->ClientRTSPPort == rhs.ClientRTSPPort));
}

//resets parsing arrays
void FStreamer::Init()
{
	RTSPCmdType = RTSP_UNKNOWN;
	memset(URLPreSuffix, 0x00, sizeof(URLPreSuffix));
	memset(URLSuffix, 0x00, sizeof(URLSuffix));
	memset(CSeq, 0x00, sizeof(CSeq));
	memset(URLHostPort, 0x00, sizeof(URLHostPort));
	ContentLength = 0;
}

void FStreamer::Run()
{
	Init();
	Receive();
	//sets status flags
	{
		FScopeLock Lock(&StreamerMt);
		bStreamerReady = false;
		bDestroyStreamer = true;
		//UE_LOG(RTSPStreaming, Log, TEXT("%d: bStreamerReady(f), bDestroyStreamer(t) THREAD CLOSED"), ClientRTSPPort);
	}
	while (!ExitReceive) {}
}

void FStreamer::Receive()
{
	while (!ExitReceive)
	{
		//creates sending buffer
		uint8 BitBuf[2000];
		int32 BytesRead = 0;
		{
			//receives RTSP message
			FScopeLock Lock(&StreamerMt);
			if (!RTSPSocket->Recv(BitBuf, 2000, BytesRead))
			{
				//sets status flags if streamer disconnects
				UE_LOG(RTSPStreaming, Log, TEXT("Server couldn't Recv pending data"));
				{
					bStreamerReady = false;
					bDestroyStreamer = true;
					ExitReceive = true;
				}
				continue;
			}
		}

		//Convert int8 array to char array
		char* RecvBuf = reinterpret_cast<char*>(BitBuf);

		//filter away everything which seems not to be an RTSP command: O-ption, D-escribe, S-etup, P-lay, T-eardown
		if ((RecvBuf[0] == 'O') || (RecvBuf[0] == 'D') || (RecvBuf[0] == 'S') || (RecvBuf[0] == 'P') || (RecvBuf[0] == 'T'))
		{
			//handles message replies and setup
			RTSP_CMD_TYPES C = Handle_RTSPRequest(RecvBuf, BytesRead);
			if (C == RTSP_PLAY)
			{
				//signals server to start passing frames here
				if (bSocketsReady)
				{
					FScopeLock Lock(&StreamerMt);
					bStreamerReady = true;
					Server.StartStreaming();
				}
			}
			else if (C == RTSP_TEARDOWN)
			{
				//ends streaming
				FScopeLock Lock(&StreamerMt);
				ExitReceive = true;
				//UE_LOG(RTSPStreaming, Log, TEXT("%d: ExitReceive(t) TEARDOWN"), ClientRTSPPort);
			}
		}
	}
	{
		FScopeLock Lock(&StreamerMt);
		bStreamerReady = false;
		//UE_LOG(RTSPStreaming, Log, TEXT("%d: bStreamerReady(f) BROKE RECEIVE"), ClientRTSPPort);
	}
	if (!ExitReceive)
	{
		UE_LOG(RTSPStreaming, Log, TEXT("Client disconnected"));
	}
}

bool FStreamer::Send(uint64 Timestamp, const uint8* Data, uint32 Size)
{
	int RTPPacketSize = Size + 12;

	//creates send buffer
	uint8* RTPBuf = new uint8[RTPBUFFERSIZE + 16];
	TSharedRef<FInternetAddr> RecvAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();

	// get client address for UDP transport
	{
		FScopeLock Lock(&RTPSocketMt);
		if (!RTSPSocket) { return false; }
		RTSPSocket->GetPeerAddress(*RecvAddr);
	}
	RecvAddr->SetPort(ClientRTPPort);
	//sets buffer to 0x00
	memset(RTPBuf, 0x00, sizeof(RTPBuf));

	//builds packet
	// Prepare the first 4 byte of the packet. This is the RTP over RTSP header in case of TCP based transport
	RTPBuf[0] = '$';									// magic number
	RTPBuf[1] = 0;										// RTP channel of RTSP connection
	RTPBuf[2] = (RTPPacketSize & 0x0000FF00) >> 8;		// size of packet
	RTPBuf[3] = (RTPPacketSize & 0x000000FF);			// size of packet
	// Prepare the 12 byte RTP header
	RTPBuf[4] = 0x80;									// RTP Version - 0b10, 0b0 - Padding, 0b0 - Extension, 0b0000 - CSRC count
	RTPBuf[5] = 0xE0;									// Marker - 0b1, H.264 payload - 96 (dynamic)
	RTPBuf[7] = SequenceNumber & 0x0FF;					// sequence counter
	RTPBuf[6] = SequenceNumber >> 8;					// sequence counter
	RTPBuf[8] = (Timestamp & 0x00000000FF000000) >> 24; // timestamp
	RTPBuf[9] = (Timestamp & 0x0000000000FF0000) >> 16;	// timestamp
	RTPBuf[10] = (Timestamp & 0x000000000000FF00) >> 8;	// timestamp
	RTPBuf[11] = (Timestamp & 0x00000000000000FF);		// timestamp
	RTPBuf[12] = 0x13;									// 4 byte SSRC (sychronization source identifier)
	RTPBuf[13] = 0xf9;									// we just an arbitrary number here to keep it simple
	RTPBuf[14] = 0x7e;
	RTPBuf[15] = 0x67;

	//appends frame to packet
	memcpy(&RTPBuf[16], Data, Size);

	//prepare the packet counter for the next packet
	SequenceNumber++;                              

	// RTP over RTSP - send the buffer + 4 byte additional header
	if (bTCPTransport) 
	{
		FScopeLock Lock(&RTSPSocketMt);
		if (RTSPSocket)
		{
			int BytesSent = 0;
			int& sent = BytesSent;
			RTSPSocket->Send(RTPBuf, RTPPacketSize + 4, sent);
			delete[] RTPBuf;
			return true;
		}
	}
	// UDP - send but skip the 4 byte RTP over RTSP header
	else              
	{
		FScopeLock Lock(&RTPSocketMt);
		if (RTPSocket)
		{
			int BytesSent = 0;
			int& sent = BytesSent;
			RTPSocket->SendTo(&RTPBuf[4], RTPPacketSize, sent, *RecvAddr);
			delete[] RTPBuf;
			return true;
		}
	}
	delete[] RTPBuf;
	return false;
}

void FStreamer::InitTransport(uint16 aRTPPort, uint16 aRTCPPort, bool TCP)
{
	//sets streamer client vars
	FIPv4Address ServerAddress;
	FIPv4Address::Parse(ServerIP, ServerAddress);

	ClientRTPPort = aRTPPort;
	ClientRTCPPort = aRTCPPort;
	bTCPTransport = TCP;

	if (!bTCPTransport)
	{   // allocate port pairs for RTP/RTCP ports in UDP transport mode
		for (uint16 P = 6970; P < 0xFFFE; P += 2)
		{
			//creates RTP UDP socket
			{	
				FScopeLock Lock(&RTPSocketMt);
				RTPSocket = FUdpSocketBuilder(TEXT("Server RTP")).
					AsNonBlocking().
					AsReusable().
					BoundToAddress(ServerAddress).
					BoundToPort(P).
					WithBroadcast().
					WithSendBufferSize(RTPBUFFERSIZE + 12).
					Build();
			}
			if (RTPSocket)
			{   
				// RTP socket was bound successfully. try to create the consecutive RTCP UDP socket
				{
					FScopeLock Lock(&RTCPSocketMt);
					RTCPSocket = FUdpSocketBuilder(TEXT("Server RTCP")).
						AsNonBlocking().
						AsReusable().
						BoundToAddress(ServerAddress).
						BoundToPort(P + 1).
						WithBroadcast().
						WithSendBufferSize(5000).
						Build();
				}

				if (RTCPSocket)
				{
					//both sockets were created successfully
					ServerRTPPort = P;
					ServerRTCPPort = P + 1;
					bSocketsReady = true;
					break;
				}

				else
				{
					//RTCP socket could not be created - destroy RTP socket and try again
					if (RTPSocket)
					{
						FScopeLock Lock(&RTPSocketMt);
						ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(RTPSocket);
						RTPSocket = nullptr;
					}
				}
			}
		}
	}
}

bool FStreamer::ParseRTSPRequest(char const* aRequest, unsigned aRequestSize)
{
	char     CmdName[RTSP_PARAM_STRING_MAX];
	char     CurRequest[RTSP_BUFFER_SIZE];
	unsigned CurRequestSize;

	Init();
	CurRequestSize = aRequestSize;
	memcpy(CurRequest, aRequest, aRequestSize);

	// check whether the request contains information about the RTP/RTCP UDP client ports (SETUP command)
	char* ClientPortPtr;
	char* TmpPtr;
	char  CP[1024];
	char* pCP;

	ClientPortPtr = strstr(CurRequest, "client_port");
	if (ClientPortPtr != nullptr)
	{
		TmpPtr = strstr(ClientPortPtr, "\r\n");
		if (TmpPtr != nullptr)
		{
			TmpPtr[0] = 0x00;
			strcpy_s(CP, ClientPortPtr);
			pCP = strstr(CP, "=");
			if (pCP != nullptr)
			{
				pCP++;
				strcpy_s(CP, pCP);
				pCP = strstr(CP, "-");
				if (pCP != nullptr)
				{
					pCP[0] = 0x00;
					ClientRTPPort = atoi(CP);
					ClientRTCPPort = ClientRTPPort + 1;
				}
			}
		}
	}

	// Read everything up to the first space as the command name
	bool parseSucceeded = false;
	unsigned i;
	for (i = 0; i < sizeof(CmdName) - 1 && i < CurRequestSize; ++i)
	{
		char c = CurRequest[i];
		if (c == ' ' || c == '\t')
		{
			parseSucceeded = true;
			break;
		}
		CmdName[i] = c;
	}
	CmdName[i] = '\0';
	if (!parseSucceeded) return false;

	// find out the command type
	if (strstr(CmdName, "OPTIONS") != nullptr)			RTSPCmdType = RTSP_OPTIONS;			else
	if (strstr(CmdName, "DESCRIBE") != nullptr)			RTSPCmdType = RTSP_DESCRIBE;		else
	if (strstr(CmdName, "SETUP") != nullptr)			RTSPCmdType = RTSP_SETUP;			else
	if (strstr(CmdName, "PLAY") != nullptr)				RTSPCmdType = RTSP_PLAY;			else
	if (strstr(CmdName, "TEARDOWN") != nullptr)			RTSPCmdType = RTSP_TEARDOWN;		else
	if (strstr(CmdName, "PAUSE") != nullptr)			RTSPCmdType = RTSP_PAUSE;			else
	if (strstr(CmdName, "GET_PARAMETER") != nullptr)	RTSPCmdType = RTSP_GET_PARAMETER;	else
	if (strstr(CmdName, "SET_PARAMETER") != nullptr)	RTSPCmdType = RTSP_SET_PARAMETER;

	// check whether the request contains transport information (UDP or TCP)
	if (RTSPCmdType == RTSP_SETUP)
	{
		TmpPtr = strstr(CurRequest, "RTP/AVP/TCP");
		if (TmpPtr != nullptr) bTCPTransport = true; else bTCPTransport = false;
	};

	// Skip over the prefix of any "RTSP://" or "RTSP:/" URL that follows:
	unsigned j = i + 1;
	while (j < CurRequestSize && (CurRequest[j] == ' ' || CurRequest[j] == '\t')) ++j; // skip over any additional white space
	for (; (int)j < (int)(CurRequestSize - 8); ++j)
	{
		if ((CurRequest[j] == 'r' || CurRequest[j] == 'R') &&
			(CurRequest[j + 1] == 't' || CurRequest[j + 1] == 'T') &&
			(CurRequest[j + 2] == 's' || CurRequest[j + 2] == 'S') &&
			(CurRequest[j + 3] == 'p' || CurRequest[j + 3] == 'P') &&
			CurRequest[j + 4] == ':' && CurRequest[j + 5] == '/')
		{
			j += 6;
			if (CurRequest[j] == '/')
			{   // This is a "RTSP://" URL; skip over the host:port part that follows:
				++j;
				unsigned uidx = 0;
				while (j < CurRequestSize && CurRequest[j] != '/' && CurRequest[j] != ' ')
				{   // extract the host:port part of the URL here
					URLHostPort[uidx] = CurRequest[j];
					uidx++;
					++j;
				}
			}
			else --j;
			i = j;
			break;
		}
	}

	// Look for the URL suffix (before the following "RTSP/"):
	parseSucceeded = false;
	for (unsigned k = i + 1; (int)k < (int)(CurRequestSize - 5); ++k)
	{
		if (CurRequest[k] == 'R' && CurRequest[k + 1] == 'T' &&
			CurRequest[k + 2] == 'S' && CurRequest[k + 3] == 'P' &&
			CurRequest[k + 4] == '/')
		{
			while (--k >= i && CurRequest[k] == ' ') {}
			unsigned k1 = k;
			while (k1 > i && CurRequest[k1] != '/') --k1;
			if (k - k1 + 1 > sizeof(URLSuffix)) return false;
			unsigned n = 0, k2 = k1 + 1;

			while (k2 <= k) URLSuffix[n++] = CurRequest[k2++];
			URLSuffix[n] = '\0';

			if (k1 - i > sizeof(URLPreSuffix)) return false;
			n = 0; k2 = i + 1;
			while (k2 <= k1 - 1) URLPreSuffix[n++] = CurRequest[k2++];
			URLPreSuffix[n] = '\0';
			i = k + 7;
			parseSucceeded = true;
			break;
		}
	}
	if (!parseSucceeded) return false;

	// Look for "CSeq:", skip whitespace, then read everything up to the next \r or \n as 'CSeq':
	parseSucceeded = false;
	for (j = i; (int)j < (int)(CurRequestSize - 5); ++j)
	{
		if (CurRequest[j] == 'C' && CurRequest[j + 1] == 'S' &&
			CurRequest[j + 2] == 'e' && CurRequest[j + 3] == 'q' &&
			CurRequest[j + 4] == ':')
		{
			j += 5;
			while (j < CurRequestSize && (CurRequest[j] == ' ' || CurRequest[j] == '\t')) ++j;
			unsigned n;
			for (n = 0; n < sizeof(CSeq) - 1 && j < CurRequestSize; ++n, ++j)
			{
				char c = CurRequest[j];
				if (c == '\r' || c == '\n')
				{
					parseSucceeded = true;
					break;
				}
				CSeq[n] = c;
			}
			CSeq[n] = '\0';
			break;
		}
	}
	if (!parseSucceeded) return false;

	// Also: Look for "Content-Length:" (optional)
	for (j = i; (int)j < (int)(CurRequestSize - 15); ++j)
	{
		if (CurRequest[j] == 'C' && CurRequest[j + 1] == 'o' &&
			CurRequest[j + 2] == 'n' && CurRequest[j + 3] == 't' &&
			CurRequest[j + 4] == 'e' && CurRequest[j + 5] == 'n' &&
			CurRequest[j + 6] == 't' && CurRequest[j + 7] == '-' &&
			(CurRequest[j + 8] == 'L' || CurRequest[j + 8] == 'l') &&
			CurRequest[j + 9] == 'e' && CurRequest[j + 10] == 'n' &&
			CurRequest[j + 11] == 'g' && CurRequest[j + 12] == 't' &&
			CurRequest[j + 13] == 'h' && CurRequest[j + 14] == ':')
		{
			j += 15;
			while (j < CurRequestSize && (CurRequest[j] == ' ' || CurRequest[j] == '\t')) ++j;
			unsigned num;
			if (sscanf_s(&CurRequest[j], "%u", &num) == 1) ContentLength = num;
		}
	}
	return true;
}

RTSP_CMD_TYPES FStreamer::Handle_RTSPRequest(char const* aRequest, unsigned aRequestSize)
{
	if (ParseRTSPRequest(aRequest, aRequestSize))
	{
		switch (RTSPCmdType)
		{
		case RTSP_OPTIONS:			{  Handle_RTSPOPTION();			break;	}
		case RTSP_DESCRIBE:			{  Handle_RTSPDESCRIBE();		break;	}
		case RTSP_SETUP:			{  Handle_RTSPSETUP();			break;	}
		case RTSP_PLAY:				{  Handle_RTSPPLAY();			break;	}
		case RTSP_PAUSE:			{  Handle_RTSPPAUSE();			break;	}
		case RTSP_GET_PARAMETER:	{  Handle_RTSPGET_PARAMETER();  break;	}
		case RTSP_SET_PARAMETER:	{  Handle_RTSPSET_PARAMETER();  break;	}
		case RTSP_TEARDOWN:			{  Handle_RTSPTEARDOWN();		break;	}
		default:					{										}
		}
	}
	return RTSPCmdType;
}

void FStreamer::Handle_RTSPOPTION()
{
	char Response[1024];
	UpdateDateHeader();
	_snprintf_s(Response, sizeof(Response),
		"RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
		"Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n"
		"%s\r\n\r\n",
		CSeq,
		Date);

	//send(RTSPSocket, Response, strlen(Response), 0);
	int32 BytesSent = 0;
	int& sent = BytesSent;
	{
		FScopeLock Lock(&RTSPSocketMt); 
		if (!RTSPSocket) { return; }
		RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
	}
}

void FStreamer::Handle_RTSPDESCRIBE()
{
	char Response[1024];
	char   SDPBuf[1024];
	char   URLBuf[1024];

	// check whether we know a stream with the URL which is requested
	bValid = 0;        // invalid URL
	if ((strcmp(URLPreSuffix, "stream") == 0) && (strcmp(URLSuffix, "1") == 0)) bValid = 1;
	if (!bValid)
	{   // Stream not available
		UpdateDateHeader();
		_snprintf_s(Response, sizeof(Response),
			"RTSP/1.0 404 Stream Not Found\r\nCSeq: %s\r\n%s\r\n",
			CSeq,
			Date);

		//send(RTSPSocket, Response, strlen(Response), 0);
		int32 BytesSent;
		int& sent = BytesSent;
		{
			FScopeLock Lock(&RTSPSocketMt);
			if (!RTSPSocket) { return; }
			RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
		}
		return;
	}

	// simulate DESCRIBE server response
	char OBuf[256];
	char* ColonPtr;
	strcpy_s(OBuf, URLHostPort);
	ColonPtr = strstr(OBuf, ":");
	if (ColonPtr != nullptr) ColonPtr[0] = 0x00;

	_snprintf_s(SDPBuf, sizeof(SDPBuf),
		"v=0\r\n"
		"o=- %d 1 IN IP4 %s\r\n"
		"s=Session streamed with RTSPStreaming\r\n"
		"i=RTSP-server\r\n"
		"t=0 0\r\n"
		"a=type:broadcast\r\n"
		"a=range:npt=now-\r\n"
		"m=video 0 RTP/AVP 96\r\n"
		"c=IN IP4 0.0.0.0\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=fmtp:96 level-asymmetry-allowed=1;packetization mode=1;profile-level-id=42e033\r\n"
		"a=framerate:60.000000\r\n",
		rand(),
		OBuf);
	char StreamName[64];
	strcpy_s(StreamName, "stream");
	_snprintf_s(URLBuf, sizeof(URLBuf),
		"RTSP://%s/%s",
		URLHostPort,
		StreamName);
	UpdateDateHeader();
	_snprintf_s(Response, sizeof(Response),
		"RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Base: %s/\r\n"
		"Server: RTSPStreaming RTSP Server"
		"%s\r\n"
		"Content-Length: %zd\r\n\r\n"
		"%s",
		CSeq,
		URLBuf,
		Date,
		strlen(SDPBuf),
		SDPBuf);

	//send(RTSPSocket, Response, strlen(Response), 0);
	int32 BytesSent;
	int& sent = BytesSent;
	{
		FScopeLock Lock(&RTSPSocketMt);
		if (!RTSPSocket) { return; }
		RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
	}
}

void FStreamer::Handle_RTSPSETUP()
{
	char Response[1024];
	char Transport[255];

	// init RTP streamer transport type (UDP or TCP) and ports for UDP transport
	InitTransport(ClientRTPPort, ClientRTCPPort, bTCPTransport);

	// simulate SETUP server response
	if (bTCPTransport)
	{
		_snprintf_s(Transport, sizeof(Transport), "RTP/AVP/TCP;unicast;interleaved=0-1");
	}
	else
	{
		UpdateDateHeader();
		_snprintf_s(Transport, sizeof(Transport),
			"RTP/AVP;unicast;destination=%ls;source=%ls;client_port=%i-%i;server_port=%i-%i",
			*ClientIP,
			*ServerIP,
			ClientRTPPort,
			ClientRTCPPort,
			ServerRTPPort,
			ServerRTCPPort);
	}

	_snprintf_s(Response, sizeof(Response),
		"RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
		"%s\r\n"
		"Transport: %s\r\n"
		"Session: %i\r\n\r\n",
		CSeq,
		Date,
		Transport,
		RTSPSessionID);

	//send(RTSPSocket, Response, strlen(Response), 0);
	int32 BytesSent;
	int& sent = BytesSent;
	{
		FScopeLock Lock(&RTSPSocketMt);
		if (!RTSPSocket) { return; }
		RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
	}
}

void FStreamer::Handle_RTSPPLAY()
{
	char Response[1024];
	UpdateDateHeader();
	// simulate SETUP server response
	_snprintf_s(Response, sizeof(Response),
		"RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
		"%s\r\n"
		"Range: npt=0.000-\r\n"
		"Session: %i\r\n"
		"RTP-Info: url=RTSP://127.0.0.1:8554/mjpeg/1/track1\r\n\r\n",
		CSeq,
		Date,
		RTSPSessionID);

	//send(RTSPSocket, Response, strlen(Response), 0);
	int32 BytesSent;
	int& sent = BytesSent;
	{
		FScopeLock Lock(&RTSPSocketMt);
		if (!RTSPSocket) { return; }
		RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
	}
}

void FStreamer::Handle_RTSPTEARDOWN()
{}

void FStreamer::Handle_RTSPSET_PARAMETER()
{}

void FStreamer::Handle_RTSPPAUSE()
{
	char Response[1024];
	UpdateDateHeader();
	// simulate SETUP server response
	_snprintf_s(Response, sizeof(Response),
		"RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
		"%s\r\n",
		CSeq,
		Date);

	//send(RTSPSocket, Response, strlen(Response), 0);
	int32 BytesSent;
	int& sent = BytesSent;
	{
		FScopeLock Lock(&RTSPSocketMt);
		if (!RTSPSocket) { return; }
		RTSPSocket->Send(reinterpret_cast<uint8*>(Response), strlen(Response), sent);
	}
}

void FStreamer::Handle_RTSPGET_PARAMETER()
{}

void FStreamer::UpdateDateHeader()
{
	time_t tt = time(NULL);
	tm ts;
	gmtime_s(&ts, &tt);
	strftime(Date, sizeof Date, "Date: %a, %b %d %Y %H:%M:%S GMT", &ts);
}

int FStreamer::isValidURL()
{
	return bValid;
}

