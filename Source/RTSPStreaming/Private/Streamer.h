// MediaLAN 02/2013
// CRTSPSession
// - JPEG packetizer and UDP/TCP based streaming

#pragma once

#include "Sockets.h"
#include "Server.h"

// supported RTSP command types
enum RTSP_CMD_TYPES
{
	RTSP_OPTIONS,
	RTSP_DESCRIBE,
	RTSP_SETUP,
	RTSP_PLAY,
	RTSP_TEARDOWN,
	RTSP_PAUSE,
	RTSP_GET_PARAMETER,
	RTSP_SET_PARAMETER,
	RTSP_UNKNOWN
};

#define RTSP_BUFFER_SIZE       10000    // for incoming requests, and outgoing responses
#define RTSP_PARAM_STRING_MAX  200		// for RTSP commands (OPTIONS, DESCRIBE, etc.)

class FStreamer final
{
public:
	FStreamer(FSocket* aRTSPSocket, const FString aServerIP, TSharedPtr<FInternetAddr> aClientAddr, FServer& aServer);
	~FStreamer();
	bool operator==(const FStreamer& rhs) const;						// compares client IP:Port to determine session equality

	void Init();														// resets RTSP message parameters
	void Receive();														// receive loop which receives RTSP messages from client
	void Run();															// RTSP server thread loop

	void InitTransport(uint16 aRTPPort, uint16 aRTCPPort, bool TCP);	// initializes sending sockets
	bool Send(uint64 Timestamp, const uint8* Data, uint32 Size);		// packetizes data and sends to client
	
	bool isReady()														// returns true when play is received
	{
		return bStreamerReady;
	}
	bool isDead()														// returns true when streamer receives TEARDOWN or client disconnects
	{
		return bDestroyStreamer;
	}

	FString GetIP()
	{
		return ClientIP;
	}
	int32 GetPort()
	{
		return ClientRTSPPort;
	}


private:
	bool ParseRTSPRequest(char const* aRequest, unsigned aRequestSize);					// extracts initial information from message
	RTSP_CMD_TYPES Handle_RTSPRequest(char const* aRequest, unsigned aRequestSize);		// RTSP message handler

	void UpdateDateHeader();															// updates Date line information
	int  isValidURL();																	// nonzero if the URL is valid (consider removing)

	// RTSP request command handlers
	void Handle_RTSPOPTION();
	void Handle_RTSPDESCRIBE();
	void Handle_RTSPSETUP();
	void Handle_RTSPPLAY();
	void Handle_RTSPTEARDOWN();
	void Handle_RTSPPAUSE();
	void Handle_RTSPGET_PARAMETER();
	void Handle_RTSPSET_PARAMETER();

private:
	FCriticalSection	RTPSocketMt;		// thread lock for RTPSocket
	FSocket*			RTPSocket;			// RTP client socket 
	FCriticalSection	RTCPSocketMt;		// thread lock for RTCPSocket
	FSocket*			RTCPSocket;			// RTCP client socket
	FCriticalSection	RTSPSocketMt;		// thread lock for RTSPSocket
	FSocket*			RTSPSocket;			// RTSP Client Socket
	uint16				ClientRTSPPort;		// RTSP client port
	uint16				ClientRTPPort;		// RTP client port
	uint16				ClientRTCPPort;		// RTCP client port
	uint16				ServerRTPPort;		// RTP server port
	uint16				ServerRTCPPort;		// RTCP server port
	uint16				SequenceNumber;		// RTP packet number
	bool				bTCPTransport;		// true if client requests RTSP over TCP, false if over UDP
	FString				ServerIP;			// IP address of server
	FString				ClientIP;			// IP address of client
	bool				bSocketsReady;		// true if server sockets are bound and ready to send
	
	char				Date[200];							// Date line in RTSP messages
	int					RTSPSessionID;						// randomly assigned SessionID in RTSP message
	bool				bValid;                             // true if the URL is valid
	FServer&			Server;

	// parameters of the last received RTSP request

	RTSP_CMD_TYPES		RTSPCmdType;                            // RTSP type (OPTIONS, DESCRIBE, etc.)
	char				URLPreSuffix[RTSP_PARAM_STRING_MAX];    // stream name pre suffix 
	char				URLSuffix[RTSP_PARAM_STRING_MAX];       // stream name suffix
	char				CSeq[RTSP_PARAM_STRING_MAX];            // RTSP command sequence number
	char				URLHostPort[RTSP_BUFFER_SIZE];          // host:port part of the URL
	unsigned			ContentLength;                          // SDP string size

	bool				ExitReceive;							// true when the Session has ended and the thread should close
	FCriticalSection	StreamerMt;								// thread lock for streamer flags
	FThreadSafeBool		bStreamerReady;							// true when streamer should receive frames
	FThreadSafeBool		bDestroyStreamer;						// true when streamer should be destroyed
	// should be the last thing declared, otherwise the thread func can access other members that are not
	// initialised yet
	FThread				Thread;									// thread to accept RTSP messages

};
