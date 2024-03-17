// RIO
// Registered Input-Output
// RIO enables send and receive operations to be performed with pre-registered buffers using queues for requests and completions. 
// Send and receive operations are queued to a request queue that is associated with a Winsock socket.
// Completed I / O operations are inserted into a completion queue, and many different sockets can be associated with the same completion queue.
// Completion queues can also be split between send and receive completions. Completion operations, such as polling, can be performed entirely in user-mode and without making system calls.

// avoiding the need to lock memory pages and copy OVERLAPPED structures into kernel space when individual requests are issued,
// instead relying on pre - locked buffers, fixed sized completion queues, optional event notification on completions
// and the ability to return multiple completions from kernel space to user space in one go.

// packet delay variation => Jitter
#pragma once
#include <WS2tcpip.h>
#include <MSwsock.h>
#include <Windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#pragma comment(lib, "ws2_32.lib")

// information on the functions that implement the Winsock registered I/O extensions.
RIO_EXTENSION_FUNCTION_TABLE g_rio;
// {0x8509e081,0x96dd,0x4005,{0xb1,0x65,0x9e,0x2e,0xe8,0xc7,0x9e,0x3f}}
GUID functionTableId = WSAID_MULTIPLE_RIO;
GUID GuidAcceptEx = WSAID_ACCEPTEX;

//    typedef struct _GUID {
//        unsigned long  Data1;
//        unsigned short Data2;
//        unsigned short Data3;
//        unsigned char  Data4[8];
//    } GUID;



bool ok = true;

int main()

{

	WSADATA wsaData;
	int iResult;
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		std::wcout << L"WSAStartup failed : " << iResult << std::endl;
		return 1;
	}

	SOCKADDR_IN serverAddr;
	ZeroMemory(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(7777);

	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
	{
		std::wcout << L"Error at socket(): " << WSAGetLastError() << std::endl;
		WSACleanup();
		return 1;
	}

	iResult = bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
	if (iResult == SOCKET_ERROR)
	{
		std::wcout << L"bind failed with error: " << WSAGetLastError() << std::endl;
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}


	iResult = listen(listenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		std::wcout << L"listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	
	// RIO initialization

	


	// iocp 생성
	HANDLE hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	if (hIOCP == NULL)
	{
		std::wcout << L"CreateIoCompletionPort failed with error: " << WSAGetLastError() << std::endl;
		return 1;
	}
	OVERLAPPED overlapped = { 0 };

    RIO_NOTIFICATION_COMPLETION type;
	char ipaddr[200] = { 0 };
	DWORD ipaddrsize = 0;
	
	DWORD dwBytes = 0;
	int result = WSAIoctl(listenSocket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &functionTableId, sizeof(GUID), (void**)&g_rio, sizeof(g_rio), &dwBytes, 0, 0);
	
	// 접속 받고
	//SOCKADDR_IN localaddr = { 0 };
	//int locallen = sizeof(SOCKADDR_IN);
	//SOCKET g_socket = WSAAccept(listenSocket, (sockaddr*)&localaddr, &locallen, NULL, NULL);
	//if (g_socket == INVALID_SOCKET)
	//{
	//	std::wcout << L"accept failed" << std::endl;
	//	return 1;
	//}

	SOCKET g_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_REGISTERED_IO);
	LPFN_ACCEPTEX lpfnAcceptEx = NULL;
	result = WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx, sizeof(lpfnAcceptEx), &dwBytes, NULL, NULL);
	

	BOOL bRetVal = lpfnAcceptEx(listenSocket, g_socket, &ipaddr, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, &overlapped);
	
	std::this_thread::sleep_for(std::chrono::milliseconds(5000));
	
	if (bRetVal == FALSE)
	{
		std::wcout << L"bRetVal failed with error: " << WSAGetLastError() << std::endl;
	}

	// CreateIoCompletionPort( (HANDLE)g_socket, hIOCP, (ULONG_PTR)&g_socket, 0);



    type.Type = RIO_IOCP_COMPLETION;
    type.Iocp.IocpHandle = hIOCP;
    type.Iocp.CompletionKey = &g_socket;
    type.Iocp.Overlapped = &overlapped;

    RIO_CQ Cqueue = g_rio.RIOCreateCompletionQueue(327680, &type);
	
    if (Cqueue == RIO_INVALID_CQ)
    {
        std::cout << "RIO_INVALID_CQ\n";
        return 0;
    }
	//MaxOutstandingReceive 500
	// The maximum number of outstanding receives allowed on the socket.
	// This parameter is usually a small number for most applications.
	// MaxReceiveDataBuffers 100
	// The maximum number of receive data buffers on the socket.
	RIO_RQ Rqueue = g_rio.RIOCreateRequestQueue(g_socket, 256, 1, 256, 1, Cqueue, Cqueue, NULL);
	if (Rqueue == RIO_INVALID_RQ)
	{
		wprintf(L"RIO_INVALID_RQ: %u\n", WSAGetLastError());
		return 0;
	}
	// RIO 버퍼 등록
	char* RIObuffer = new char[10000];
	ZeroMemory(RIObuffer, sizeof(char)* 10000);
	//strcpy_s(RIObuffer, 6, "test\n");
	RIO_BUFFERID recv_bufferID = g_rio.RIORegisterBuffer(RIObuffer, 10000);
	

	// recv 걸기

	RIO_BUF* pBuf = new RIO_BUF;
	pBuf->BufferId = recv_bufferID;
	pBuf->Offset = 0;
	pBuf->Length = 10;

	int ret = 0;
	//for (int i = 0; i < 10; ++i)
	//{
	//	ret = g_rio.RIOSend(Rqueue, pBuf, 1, 0, pBuf);
	//}
	ret = g_rio.RIOReceive(Rqueue, pBuf, 1, 0, pBuf);
	//ret = g_rio.RIOReceiveEx(Rqueue, pBuf, 1, nullptr, pAddrBuf, nullptr, 0, 0, pBuf);
	if (ret == FALSE)
	{
		wprintf(L"RIOReceiveEx: %u\n", WSAGetLastError());
	}

	WSABUF wsaBuf = { 0 };
	DWORD numberOfBytesRecvd = 0;
	DWORD flags = 0;
	// WSARecv(g_socket, &wsaBuf, 1, &numberOfBytesRecvd, &flags, &overlapped,nullptr);



	INT notifyResult = g_rio.RIONotify(Cqueue);
	// 완료 받기
	RIORESULT results[10];
	DWORD localnumberOfBytes = 0;
	ULONG_PTR localcompletionKey = 0;
	OVERLAPPED* localpOverlapped = 0;
	GetQueuedCompletionStatus(hIOCP, &localnumberOfBytes, &localcompletionKey, &localpOverlapped ,INFINITE);
	ZeroMemory(results, sizeof(results));

	ULONG numResults = g_rio.RIODequeueCompletion(Cqueue, results, 10);
	notifyResult = g_rio.RIONotify(Cqueue);
	for (ULONG i = 0; i < numResults; ++i)
	{
		RIO_BUF* result = (RIO_BUF*)results[i].RequestContext;

		std::cout << RIObuffer << std::endl;
	}


	delete pBuf;
	delete[] RIObuffer;
	return 0;
}


//typedef struct RIO_CQ_t* RIO_CQ, ** PRIO_CQ;
// a completion queue descriptor used for I / O completion notification by send and receive requests.
//typedef struct _RIO_NOTIFICATION_COMPLETION {
//    RIO_NOTIFICATION_COMPLETION_TYPE Type;
//    union {
//        struct {
//            HANDLE EventHandle;
//            BOOL NotifyReset;
//        } Event;
//        struct {
//            HANDLE IocpHandle;
//            PVOID CompletionKey;
//            PVOID Overlapped;
//        } Iocp;
//    };
//} RIO_NOTIFICATION_COMPLETION, * PRIO_NOTIFICATION_COMPLETION;


// RIOCloseCompletionQueue
// closes an existing completion queue used for I/O completion notification by send and receive requests with the Winsock registered I/O extensions.
// RIOCreateCompletionQueue
// creates an I / O completion queue of a specific size for use with the Winsock registered I / O extensions.
// RIOCreateRequestQueue
// RIODequeueCompletion
// RIODeregisterBuffer
// RIONotify
// RIOReceive
// RIOReceiveEx
// RIORegisterBuffer
// RIOResizeCompletionQueue
// RIOResizeRequestQueue
// RIOSend
// RIOSendEx
//
// RIO_CQ
// RIO_RQ
// RIO_BUFFERID
// RIO_BUF
// RIO_NOTIFICATION_COMPLETION
// RIO_NOTIFICATION_COMPLETION_TYPE
// RIORESULT