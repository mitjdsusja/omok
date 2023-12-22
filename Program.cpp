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
		cout << "�� Ŭ���̾�Ʈ�� ����ƽ��ϴ�." << endl;
		EnterCriticalSection(&g_cs);
		g_listClients.push_back(hClient);
		LeaveCriticalSection(&g_cs);

		pNewUser = new USERSESSION;
		ZeroMemory(pNewUser, sizeof(USERSESSION));
		pNewUser->hSocket = hClient;

		// �񵿱� ���� ó���� ���� OVERLAPPED ����ü ����
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

		// Ŭ���̾�Ʈ�� ���� ������ �񵿱� �����Ѵ�.
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

	cout << "[IOCP �۾��� ������ ����] \n";
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
				cout << "GQCS : Ŭ���̾�Ʈ�� ���������� ������ ������." << endl;
			}
			// �������� Recv
			else {
				for (int i = 0; i < dwTransferredSize; i++) {
					cout << pSession->buffer[i];
				}
				cout << endl;
				SendMessageAll(pSession->buffer, dwTransferredSize);
				memset(pSession->buffer, 0, sizeof(pSession->buffer));

				// �ٽ� IOCP�� ���
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
				cout << "GQCS : IOCP �ڵ��� �������ϴ�" << endl;
				break;
			}
			else {
				if (pSession != NULL) {
					CloseClient(pSession->hSocket);
					delete pWol;
					delete pSession;
				}

				cout << "GQCS : ���� ���� Ȥ�� �������� ���� ����" << endl;
			}
		}
	}

	cout << "[IOCP �۾��� ������ ����]" << endl;
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

	// ��� �����尡 ����� �� �Ӱ迵�� ����
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
		cout << "ERROR : ��Ʈ�� �̹� ������Դϴ�." << endl;
		closesocket(g_listenSocket);
		return;
	}
	cout << "BINDING SUCCESS" << endl;

	if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		cout << "ERROR : ���� ���·� ��Ȱ�� �� �����ϴ�." << endl;
		closesocket(g_listenSocket);
		return;
	}
	cout << "LISTEN SUCCESS" << endl;
}

void SendMessageAll(char* msg, int size) {
	// TODO : Ŭ���̾�Ʈ���� �޽����� ��ε�ĳ��Ʈ
	list<SOCKET>::iterator it;

	::EnterCriticalSection(&g_cs);
	for (it = g_listClients.begin(); it != g_listClients.end(); ++it)
		::send(*it, msg, size, 0);
		//cout << "�޽����� ���½��ϴ� \n";
	::LeaveCriticalSection(&g_cs);
}


int main() {
	//���� �ʱ�ȭ
	WSADATA wsa = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		cout << "ERROR : ������ �ʱ�ȭ �� �� �����ϴ�." << endl;
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
			cout << "ERROR : IOCP�� ������ �� �����ϴ�." << endl;
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
