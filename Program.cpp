#include <iostream>
#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <list>

using namespace std;

typedef struct _USERSESSION {
	SOCKET hSocket;
	char buffer[8192];
} USERSESSION;

/////////////////////////////////////////////////////////////
#define MAX_THREAD_CNT 4

CRITICAL_SECTION g_cs;
SOCKET g_listenSocket;
list<SOCKET> g_listClients;
HANDLE g_hIocp;


void AcceptClient();
void CloseAll();
void CloseClient(SOCKET hSocket);
void ReleaseServer();
void CreateListenSocket(USHORT port);
void SendMessageAll(char* msg, int size);


DWORD WINAPI ThreadAcceptLoop(LPVOID pParam) {
	DWORD			dwReceiveSize = 0;
	DWORD			dwFlag = 0;
	WSABUF			wsaBuf;
	SOCKET			hClient;
	SOCKADDR		clientAddr;
	USERSESSION*	pNewUser;
	LPWSAOVERLAPPED pWol = NULL;
	int				nAddrSize =	sizeof(SOCKADDR);
	int				nRecvResult = 0;

	while ((hClient = accept(g_listenSocket, &clientAddr, &nAddrSize)) != INVALID_SOCKET) {
		cout << "새 클라이언트가 연결됐습니다." << endl;
		EnterCriticalSection(&g_cs);
		g_listClients.push_back(hClient);
		LeaveCriticalSection(&g_cs);

		pNewUser = new USERSESSION;
		ZeroMemory(pNewUser, sizeof(USERSESSION));
		pNewUser->hSocket = hClient;

		// 비동기 수신 처리를 위한 OVERLAPPED 구조체 생성
		pWol = new WSAOVERLAPPED;
		ZeroMemory(pWol, sizeof(WSAOVERLAPPED));

		CreateIoCompletionPort(
			(HANDLE)hClient,
			g_hIocp,
			(ULONG_PTR)pNewUser,
			0
		);

		dwReceiveSize = 0;
		dwFlag = 0;
		wsaBuf.buf = pNewUser->buffer;
		wsaBuf.len = sizeof(pNewUser->buffer);

		// 클라이언트가 보낸 정보를 비동기 수신한다.
		nRecvResult = WSARecv(hClient, &wsaBuf, 1, &dwReceiveSize, &dwFlag, pWol, NULL);
		if (WSAGetLastError() != WSA_IO_PENDING) {
			cout << "ERROR : WSARecv() != WSA_IO_PENDING" << endl;
		}
	}

	return 0;
}
DWORD WINAPI ThreadComplete(LPVOID pParam) {
	DWORD dwTransferredSize = 0;
	DWORD pFlag = 0;
	USERSESSION* pSession = NULL;
	LPWSAOVERLAPPED pWol = NULL;
	BOOL bResult;

	cout << "[IOCP 작업자 스레드 시작] \n";
	while (true) {
		bResult = GetQueuedCompletionStatus(
			g_hIocp,
			&dwTransferredSize,
			(PULONG_PTR)&pSession,
			&pWol,
			INFINITE
		);

		if (bResult == TRUE) {
			if (dwTransferredSize == 0) {
				CloseClient(pSession->hSocket);
				delete pWol;
				delete pSession;
				cout << "GQCS : 클라이언트가 정상적으로 연결을 종료함." << endl;
			}
			// 정상적인 Recv
			else {
				for (int i = 0; i < dwTransferredSize; i++) {
					cout << pSession->buffer[i];
				}
				cout << endl;
				SendMessageAll(pSession->buffer, dwTransferredSize);
				memset(pSession->buffer, 0, sizeof(pSession->buffer));

				// 다시 IOCP에 등록
				DWORD dwReceiveSize = 0;
				DWORD dwFlag = 0;
				WSABUF wsaBuf = { 0 };
				wsaBuf.buf = pSession->buffer;
				wsaBuf.len = sizeof(pSession->buffer);

				WSARecv(
					pSession->hSocket,
					&wsaBuf,
					1,
					&dwReceiveSize,
					&dwFlag,
					pWol,
					NULL
				);
				if (WSAGetLastError() != WSA_IO_PENDING) {
					cout << "GQCS : ERROR : WSARecv() \n";
				}
			}
		}
		else {
			if (pWol == NULL) {
				cout << "GQCS : IOCP 핸들이 닫혔습니다" << endl;
				break;
			}
			else {
				if (pSession != NULL) {
					CloseClient(pSession->hSocket);
					delete pWol;
					delete pSession;
				}

				cout << "GQCS : 서버 종료 혹은 비정상적 연결 종료" << endl;
			}
		}
	}

	cout << "[IOCP 작업자 스레드 종료]" << endl;
	return 0;
}

void AcceptClient() {
	HANDLE hThread;
	DWORD dwThreadID;
	hThread = CreateThread(NULL, 0, ThreadAcceptLoop, (LPVOID)NULL, 0, &dwThreadID);
	CloseHandle(hThread);
}

void CloseAll() {
	list<SOCKET>::iterator iter;
	//lock
	for (iter = g_listClients.begin(); iter != g_listClients.end(); iter++) {
		shutdown(*iter, SD_BOTH);
		closesocket(*iter);
	}
	//unlock
}

void CloseClient(SOCKET hSocket) {
	shutdown(hSocket, SD_BOTH);
	closesocket(hSocket);

	EnterCriticalSection(&g_cs);
	g_listClients.remove(hSocket);
	LeaveCriticalSection(&g_cs);
}

void ReleaseServer() {
	CloseAll();
	Sleep(500);

	shutdown(g_listenSocket, SD_BOTH);
	closesocket(g_listenSocket);
	g_listenSocket = NULL;

	CloseHandle(g_hIocp);
	g_hIocp = NULL;

	// 모든 스레드가 종료된 후 임계영역 제거
	Sleep(500);
	DeleteCriticalSection(&g_cs);

}

void CreateListenSocket(USHORT port) {
	SOCKADDR_IN serverAddr;

	g_listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(port);

	if (bind(g_listenSocket, (SOCKADDR*)&serverAddr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR) {
		cout << "ERROR : 포트가 이미 사용중입니다." << endl;
		closesocket(g_listenSocket);
		return;
	}
	cout << "BINDING SUCCESS" << endl;

	if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		cout << "ERROR : 리슨 상태로 전활할 수 없습니다." << endl;
		closesocket(g_listenSocket);
		return;
	}
	cout << "LISTEN SUCCESS" << endl;
}

void SendMessageAll(char* msg, int size) {
	// TODO : 클라이언트에게 메시지를 브로드캐스트
	list<SOCKET>::iterator it;

	::EnterCriticalSection(&g_cs);
	for (it = g_listClients.begin(); it != g_listClients.end(); ++it)
		::send(*it, msg, size, 0);
		//cout << "메시지를 보냈습니다 \n";
	::LeaveCriticalSection(&g_cs);
}


int main() {
	//윈속 초기화
	WSADATA wsa = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		cout << "ERROR : 윈속을 초기화 할 수 없습니다." << endl;
		return 0;
	}

	// Create CriticalSection Obj
	InitializeCriticalSection(&g_cs);
	// IOCP Set
	{
		g_hIocp = CreateIoCompletionPort(
			INVALID_HANDLE_VALUE,
			NULL,
			0,
			0
		);
		if (g_hIocp == NULL) {
			cout << "ERROR : IOCP를 생성할 수 없습니다." << endl;
			return 0;
		}
	}
	

	HANDLE hThread;
	DWORD dwThreadID;
	for (int i = 0; i < MAX_THREAD_CNT; ++i) {
		dwThreadID = 0;
		hThread = CreateThread(
			NULL,
			0,
			ThreadComplete,
			(LPVOID)NULL,
			0,
			&dwThreadID
		);
		CloseHandle(hThread);
	}
	Sleep(500);
	//
	CreateListenSocket(25000);

	AcceptClient();
	while (true) {

	}
	ReleaseServer();

}
