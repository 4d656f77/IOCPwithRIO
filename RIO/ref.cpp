/*********************************************************************************************************
 * Windows Registered I/O (RIO) Sample Code (Echo Server)
 * Minimum requirement: Windows 8 or Windows Server 2012
 * Author: @sm9kr
 *
 * Notice
 * 몇몇 코드 조각들은 (http://www.serverframework.com/asynchronousevents/rio/)에서 가져옴.
 * 그런데 여기도 제대로 완성된 코드가 있었던 것은 아니라서 상당 부분 기능을 추가하여 동작하도록 함.
 * RIO의 사용법을 보여주기 위한 코드라 최대한 간단하게 필요한 부분만 넣었음.
 * 그래서, 버그가 있을 수 있음...
 * UDP형태로 만든 이유는 귀찮아서...TCP로 하면 세션 관리를 따로 해야되고 버퍼 재조립도 해야되는 관계로..
 * RIO를 쓰는 방식은 똑같기 때문에 아래 코드를 보고 TCP에 적용 시키는 것도 어렵지 않음.
 *********************************************************************************************************/

#define NOMINMAX

#include <SDKDDKVer.h>

#include <stdio.h>
#include <tchar.h>

#include <WinSock2.h>
#include <MSWsock.h>
#include <WS2tcpip.h>
#include <iostream>
#include <deque>

#include <process.h>

#pragma comment(lib, "ws2_32.lib")

using std::cout;
using std::endl;
using std::deque;


RIO_EXTENSION_FUNCTION_TABLE g_rio;
RIO_CQ g_completionQueue = 0;
RIO_RQ g_requestQueue = 0;

HANDLE g_hIOCP = NULL;

SOCKET g_socket;

CRITICAL_SECTION g_criticalSection;

RIO_BUFFERID g_sendBufferId;
RIO_BUFFERID g_recvBufferId;
RIO_BUFFERID g_addrBufferId;

char* g_sendBufferPointer = NULL;
char* g_recvBufferPointer = NULL;
char* g_addrBufferPointer = NULL;


typedef std::deque<HANDLE> Threads;
Threads g_threads;

static const unsigned short PORTNUM = 5050;

static const DWORD RIO_PENDING_RECVS = 100000;
static const DWORD RIO_PENDING_SENDS = 10000;

static const DWORD RECV_BUFFER_SIZE = 1024;
static const DWORD SEND_BUFFER_SIZE = 1024;

static const DWORD ADDR_BUFFER_SIZE = 64;

static const DWORD NUM_IOCP_THREADS = 4;

static const DWORD RIO_MAX_RESULTS = 1000;


enum COMPLETION_KEY
{
	CK_STOP = 0,
	CK_START = 1
};

enum OPERATION_TYPE
{
	OP_NONE = 0,
	OP_RECV = 1,
	OP_SEND = 2
};

struct EXTENDED_RIO_BUF : public RIO_BUF
{
	OPERATION_TYPE operation;
};


/// SEND용 RIO_BUF는 순환큐형태로 만들어 놓고 돌려 쓰자.
EXTENDED_RIO_BUF* g_sendRioBufs = NULL;
DWORD g_sendRioBufTotalCount = 0;
__int64 g_sendRioBufIndex = 0;

/// ADDR용 RIO_BUF pointer
EXTENDED_RIO_BUF* g_addrRioBufs = NULL;
DWORD g_addrRioBufTotalCount = 0;
__int64 g_addrRioBufIndex = 0;

/// RIO에서 Registering할 버퍼 할당함수
char* AllocateBufferSpace(const DWORD bufSize, const DWORD bufCount, DWORD& totalBufferSize, DWORD& totalBufferCount);

unsigned int __stdcall IOThread(void* pV);


int _treference(int argc, _TCHAR* argv[])
{
	WSADATA data;
	InitializeCriticalSectionAndSpinCount(&g_criticalSection, 4000);

	if (0 != ::WSAStartup(0x202, &data))
	{
		const DWORD gle = ::GetLastError();
		cout << "WSAStartup Error: " << gle << endl;
		exit(0);
	}


	/// RIO 소켓 생성
	g_socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO);
	if (g_socket == INVALID_SOCKET)
	{
		const DWORD gle = ::GetLastError();
		cout << "WSASocket Error: " << gle << endl;
		exit(0);
	}

	/// port binding
	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(7777);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (SOCKET_ERROR == ::bind(g_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)))
	{
		const DWORD gle = ::GetLastError();
		cout << "Bind Error: " << gle << endl;
		exit(0);
	}

	/// RIO 함수 테이블 가져오기
	GUID functionTableId = WSAID_MULTIPLE_RIO;
	DWORD dwBytes = 0;

	if (NULL != WSAIoctl(g_socket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &functionTableId, sizeof(GUID),
		(void**)&g_rio, sizeof(g_rio), &dwBytes, NULL, NULL))
	{
		const DWORD gle = ::GetLastError();
		cout << "WSAIoctl Error: " << gle << endl;
		exit(0);
	}

	/// rio의 completion 방식은 iocp를 사용.
	g_hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	if (NULL == g_hIOCP)
	{
		const DWORD gle = ::GetLastError();
		cout << "CreateIoCompletionPort Error: " << gle << endl;
		exit(0);
	}


	OVERLAPPED overlapped;

	RIO_NOTIFICATION_COMPLETION completionType;

	completionType.Type = RIO_IOCP_COMPLETION;
	completionType.Iocp.IocpHandle = g_hIOCP;
	completionType.Iocp.CompletionKey = (void*)CK_START;
	completionType.Iocp.Overlapped = &overlapped;

	/// RIO CQ 생성 (RQ 사이즈보다 크거나 같아야 함)
	g_completionQueue = g_rio.RIOCreateCompletionQueue(RIO_PENDING_RECVS + RIO_PENDING_SENDS, &completionType);
	if (g_completionQueue == RIO_INVALID_CQ)
	{
		const DWORD gle = ::GetLastError();
		cout << "RIOCreateCompletionQueue Error: " << gle << endl;
		exit(0);
	}

	/// RIO RQ 생성
	/// SEND CQ와 RECV CQ를 같이 씀.. (따로 만들어 써도 됨)
	g_requestQueue = g_rio.RIOCreateRequestQueue(g_socket, RIO_PENDING_RECVS, 1, RIO_PENDING_SENDS, 1, g_completionQueue, g_completionQueue, NULL);
	if (g_requestQueue == RIO_INVALID_RQ)
	{
		const DWORD gle = ::GetLastError();
		cout << "RIOCreateRequestQueue Error: " << gle << endl;
		exit(0);
	}


	/// SEND용 RIO 버퍼 등록
	{
		DWORD totalBufferCount = 0;
		DWORD totalBufferSize = 0;

		g_sendBufferPointer = AllocateBufferSpace(SEND_BUFFER_SIZE, RIO_PENDING_SENDS, totalBufferSize, totalBufferCount);
		g_sendBufferId = g_rio.RIORegisterBuffer(g_sendBufferPointer, static_cast<DWORD>(totalBufferSize));

		if (g_sendBufferId == RIO_INVALID_BUFFERID)
		{
			const DWORD gle = ::GetLastError();
			cout << "RIORegisterBuffer Error: " << gle << endl;
			exit(0);
		}

		DWORD offset = 0;

		g_sendRioBufs = new EXTENDED_RIO_BUF[totalBufferCount];
		g_sendRioBufTotalCount = totalBufferCount;

		for (DWORD i = 0; i < g_sendRioBufTotalCount; ++i)
		{
			/// RIO operation에 맞도록 할당된 버퍼를 쪼개기
			EXTENDED_RIO_BUF* pBuffer = g_sendRioBufs + i;

			pBuffer->operation = OP_SEND;
			pBuffer->BufferId = g_sendBufferId;
			pBuffer->Offset = offset;
			pBuffer->Length = SEND_BUFFER_SIZE;

			offset += SEND_BUFFER_SIZE;

		}

	}


	/// ADDR용 (RECV시 담겨오는 주소용) RIO 버퍼 등록
	{
		DWORD totalBufferCount = 0;
		DWORD totalBufferSize = 0;

		g_addrBufferPointer = AllocateBufferSpace(ADDR_BUFFER_SIZE, RIO_PENDING_RECVS, totalBufferSize, totalBufferCount);
		g_addrBufferId = g_rio.RIORegisterBuffer(g_addrBufferPointer, static_cast<DWORD>(totalBufferSize));

		if (g_addrBufferId == RIO_INVALID_BUFFERID)
		{
			const DWORD gle = ::GetLastError();
			cout << "RIORegisterBuffer Error: " << gle << endl;
			exit(0);
		}

		DWORD offset = 0;

		g_addrRioBufs = new EXTENDED_RIO_BUF[totalBufferCount];
		g_addrRioBufTotalCount = totalBufferCount;

		for (DWORD i = 0; i < totalBufferCount; ++i)
		{
			EXTENDED_RIO_BUF* pBuffer = g_addrRioBufs + i;

			pBuffer->operation = OP_NONE;
			pBuffer->BufferId = g_addrBufferId;
			pBuffer->Offset = offset;
			pBuffer->Length = ADDR_BUFFER_SIZE;

			offset += ADDR_BUFFER_SIZE;
		}
	}

	/// RECV용 RIO 버퍼 등록 및 RECV 미리 걸어놓기
	{
		DWORD totalBufferCount = 0;
		DWORD totalBufferSize = 0;

		g_recvBufferPointer = AllocateBufferSpace(RECV_BUFFER_SIZE, RIO_PENDING_RECVS, totalBufferSize, totalBufferCount);

		g_recvBufferId = g_rio.RIORegisterBuffer(g_recvBufferPointer, static_cast<DWORD>(totalBufferSize));
		if (g_recvBufferId == RIO_INVALID_BUFFERID)
		{
			const DWORD gle = ::GetLastError();
			cout << "RIORegisterBuffer Error: " << gle << endl;
			exit(0);
		}


		DWORD offset = 0;

		EXTENDED_RIO_BUF* pBufs = new EXTENDED_RIO_BUF[totalBufferCount];

		for (DWORD i = 0; i < totalBufferCount; ++i)
		{
			EXTENDED_RIO_BUF* pBuffer = pBufs + i;

			pBuffer->operation = OP_RECV;
			pBuffer->BufferId = g_recvBufferId;
			pBuffer->Offset = offset;
			pBuffer->Length = RECV_BUFFER_SIZE;

			offset += RECV_BUFFER_SIZE;

			/// 미리 RECV 많이 걸어 놓는다.
			if (!g_rio.RIOReceiveEx(g_requestQueue, pBuffer, 1, NULL, &g_addrRioBufs[g_addrRioBufIndex++], NULL, 0, 0, pBuffer))
			{
				const DWORD gle = ::GetLastError();
				cout << "RIOReceive Error: " << gle << endl;
				exit(0);
			}
		}

		cout << totalBufferCount << " total receives pending" << endl;
	}


	/// IO 쓰레드 생성
	for (DWORD i = 0; i < NUM_IOCP_THREADS; ++i)
	{
		unsigned int notUsed;
		const uintptr_t result = ::_beginthreadex(0, 0, IOThread, (void*)i, 0, &notUsed);
		if (result == 0)
		{
			const DWORD gle = ::GetLastError();
			cout << "_beginthreadex Error: " << gle << endl;
			exit(0);
		}

		g_threads.push_back(reinterpret_cast<HANDLE>(result));
	}

	/// Completion이 준비되었음을 알린다
	INT notifyResult = g_rio.RIONotify(g_completionQueue);
	if (notifyResult != ERROR_SUCCESS)
	{
		const DWORD gle = ::GetLastError();
		cout << "RIONotify Error: " << gle << endl;
		exit(0);
	}


	/// 아무키나 받으면 자원 반환하고 끝내기 -.- ;
	cout << "Press Any Key to Stop" << endl;
	getchar();



	/// 컴플리션키를 CK_STOP으로 쓰면 쓰레드 멈춰라는 명령으로 사용 
	for (Threads::const_iterator it = g_threads.begin(), end = g_threads.end(); it != end; ++it)
	{
		if (0 == ::PostQueuedCompletionStatus(g_hIOCP, 0, CK_STOP, 0))
		{
			const DWORD gle = ::GetLastError();
			cout << "PostQueuedCompletionStatus Error: " << gle << endl;
			exit(0);
		}
	}

	/// 쓰레드 조인
	for (Threads::const_iterator it = g_threads.begin(), end = g_threads.end(); it != end; ++it)
	{
		HANDLE hThread = *it;

		if (WAIT_OBJECT_0 != ::WaitForSingleObject(hThread, INFINITE))
		{
			const DWORD gle = ::GetLastError();
			cout << "WaitForSingleObject (thread join) Error: " << gle << endl;
			exit(0);
		}

		::CloseHandle(hThread);
	}


	if (SOCKET_ERROR == ::closesocket(g_socket))
	{
		const DWORD gle = ::GetLastError();
		cout << "closesocket Error: " << gle << endl;
	}

	g_rio.RIOCloseCompletionQueue(g_completionQueue);

	g_rio.RIODeregisterBuffer(g_sendBufferId);
	g_rio.RIODeregisterBuffer(g_recvBufferId);
	g_rio.RIODeregisterBuffer(g_addrBufferId);

	DeleteCriticalSection(&g_criticalSection);

	return 0;
}


unsigned int __stdcall IOThread(void* pV)
{
	DWORD numberOfBytes = 0;

	ULONG_PTR completionKey = 0;
	OVERLAPPED* pOverlapped = 0;

	RIORESULT results[RIO_MAX_RESULTS];

	while (true)
	{
		if (!::GetQueuedCompletionStatus(g_hIOCP, &numberOfBytes, &completionKey, &pOverlapped, INFINITE))
		{
			const DWORD gle = ::GetLastError();
			cout << "GetQueuedCompletionStatus Error: " << gle << endl;
			exit(0);
		}

		/// ck로 0이 넘어오면 끝낸다
		if (completionKey == CK_STOP)
			break;

		memset(results, 0, sizeof(results));

		ULONG numResults = g_rio.RIODequeueCompletion(g_completionQueue, results, RIO_MAX_RESULTS);
		if (0 == numResults || RIO_CORRUPT_CQ == numResults)
		{
			const DWORD gle = ::GetLastError();
			cout << "RIODequeueCompletion Error: " << gle << endl;
			exit(0);
		}

		/// Dequeue 한 다음에는 다음 컴플리션이 가능하도록 알린다.
		INT notifyResult = g_rio.RIONotify(g_completionQueue);
		if (notifyResult != ERROR_SUCCESS)
		{
			const DWORD gle = ::GetLastError();
			cout << "RIONotify Error: " << gle << endl;
			exit(0);
		}

		for (DWORD i = 0; i < numResults; ++i)
		{
			EXTENDED_RIO_BUF* pBuffer = reinterpret_cast<EXTENDED_RIO_BUF*>(results[i].RequestContext);

			if (OP_RECV == pBuffer->operation)
			{
				/// UDP니까 송신자측에서 보낸 데이터가 전부 안오는 경우는 에러
				if (results[i].BytesTransferred != RECV_BUFFER_SIZE)
					break;

				///// ECHO TEST
				const char* offset = g_recvBufferPointer + pBuffer->Offset;

				/// RQ는 thread-safe하지 않기 때문에 보호해줘야 한다. (최적화를 하지는 않았음. 대충 LOCK...)
				::EnterCriticalSection(&g_criticalSection);
				{

					EXTENDED_RIO_BUF* sendBuf = &(g_sendRioBufs[g_sendRioBufIndex++ % g_sendRioBufTotalCount]);
					char* sendOffset = g_sendBufferPointer + sendBuf->Offset;
					memcpy_s(sendOffset, RECV_BUFFER_SIZE, offset, pBuffer->Length);

					/// TEST PRINT
					cout << strlen(sendOffset) << " ";


					if (!g_rio.RIOSendEx(g_requestQueue, sendBuf, 1, NULL, &g_addrRioBufs[g_addrRioBufIndex % g_addrRioBufTotalCount], NULL, NULL, 0, sendBuf))
					{
						const DWORD gle = ::GetLastError();
						cout << "RIOSend Error: " << gle << endl;
						exit(0);;
					}

				}
				::LeaveCriticalSection(&g_criticalSection);


			}
			else if (OP_SEND == pBuffer->operation)
			{
				/// 마찬가지로 RQ 보호
				::EnterCriticalSection(&g_criticalSection);
				{

					if (!g_rio.RIOReceiveEx(g_requestQueue, pBuffer, 1, NULL, &g_addrRioBufs[g_addrRioBufIndex % g_addrRioBufTotalCount], NULL, 0, 0, pBuffer))
					{
						const DWORD gle = ::GetLastError();
						cout << "RIOReceive Error: " << gle << endl;
						exit(0);;
					}

					g_addrRioBufIndex++;

				}
				::LeaveCriticalSection(&g_criticalSection);

			}
			else
				break;

		}
	}


	return 0;
}


template <typename TV, typename TM>
inline TV RoundDown(TV Value, TM Multiple)
{
	return((Value / Multiple) * Multiple);
}

template <typename TV, typename TM>
inline TV RoundUp(TV Value, TM Multiple)
{
	return(RoundDown(Value, Multiple) + (((Value % Multiple) > 0) ? Multiple : 0));
}


char* AllocateBufferSpace(const DWORD bufSize, const DWORD bufCount, DWORD& totalBufferSize, DWORD& totalBufferCount)
{
	SYSTEM_INFO systemInfo;

	::GetSystemInfo(&systemInfo);

	const unsigned __int64 granularity = systemInfo.dwAllocationGranularity;

	const unsigned __int64 desiredSize = bufSize * bufCount;

	unsigned __int64 actualSize = RoundUp(desiredSize, granularity);

	if (actualSize > std::numeric_limits<DWORD>::max())
	{
		actualSize = (std::numeric_limits<DWORD>::max() / granularity) * granularity;
	}

	totalBufferCount = std::min<DWORD>(bufCount, static_cast<DWORD>(actualSize / bufSize));

	totalBufferSize = static_cast<DWORD>(actualSize);

	char* pBuffer = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(), 0, totalBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

	if (pBuffer == 0)
	{
		const DWORD gle = ::GetLastError();
		cout << "VirtualAllocEx Error: " << gle << endl;
		exit(0);
	}

	return pBuffer;
}