// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE (WINDOWS) =======================

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <signal.h>
#include <chrono>

// Winsock headers must come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "helper.h"

// Link with Winsock library in MSVC
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

// ---- Constants ----
const char* SERVER_PATH = "server.exe";       // Executable path for Windows
const int IO_BUFFER_SIZE = 64 * 1024;        // Fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;               // Connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;           // Clients allowed to wait in our queue
const int STARTUP_MAX_ATTEMPTS = 50;         // After starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;    // Wait between those tries (50 x 100 ms = 5 seconds)

int lbPort = 5000;
string serverHost = "127.0.0.1";             // Loopback address
int serverPort = 5001;

// ---- Queue: holds client sockets ----
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front;
    Node* rear;
    int   count;

public:
    Queue() : front(nullptr), rear(nullptr), count(0) {}

    ~Queue()
    {
        while (!isEmpty())
            dequeue();
    }

    void enqueue(const T& val)
    {
        Node* node = new Node();
        node->data = val;
        node->next = nullptr;

        if (rear == nullptr)
            front = rear = node;
        else
        {
            rear->next = node;
            rear = node;
        }
        count++;
    }

    T dequeue()
    {
        if (isEmpty())
        {
            return T();
        }
        Node* temp = front;
        T val = front->data;
        front = front->next;
        if (front == nullptr)
            rear = nullptr;
        delete temp;
        count--;
        return val;
    }

    bool isEmpty() {
        return front == nullptr;
    }
    int size() {
        return count;
    }
};

struct ClientInfo
{
    SOCKET sock;        // the connected WinSock handle
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// Global shared resources
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

// ---- Basic socket helpers ----
bool sendAll(SOCKET sock, const char* buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        int result = send(sock, buf + sent, static_cast<int>(n - sent), 0);
        if (result <= 0)
            return false;   // connection failed
        sent += result;
    }
    return true;
}

bool sendError(SOCKET sock, const string& message)
{
    // Reply format: [status: 1 byte (value = 1)][payload text]
    string response;
    response.push_back(static_cast<char>(1)); // 1 indicates error
    response.append(message);
    return sendAll(sock, response.c_str(), response.size());
}

SOCKET createListenSocket(int port)
{
    // Create TCP socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) 
    {
        cerr << "socket creation failed: " << WSAGetLastError() << endl;
        exit(1);
    }

    // Set SO_REUSEADDR so port can be rebound quickly
    BOOL opt = TRUE;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        cerr << "setsockopt failed: " << WSAGetLastError() << endl;
        closesocket(sock);
        exit(1);
    }

    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // Bind socket
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cerr << "bind failed: " << WSAGetLastError() << endl;
        closesocket(sock);
        exit(1);
    }

    if (listen(sock, LISTEN_BACKLOG) == SOCKET_ERROR) {
        cerr << "listen failed: " << WSAGetLastError() << endl;
        closesocket(sock);
        exit(1);
    }

    return sock;
}

SOCKET connectToServer(const char* host, int port)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
    // Build Windows process launch string e.g., "server.exe 5001"
    string cmd = string(SERVER_PATH) + " " + to_string(port);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL success = CreateProcessA(NULL,const_cast<char*>(cmd.c_str()),NULL,NULL,FALSE,DETACHED_PROCESS,NULL,NULL,&si,&pi);

    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

SOCKET connectToServerWithWakeup()
{
    // 1. Try immediate connect
    SOCKET serverSock = connectToServer(serverHost.c_str(), serverPort);
    if (serverSock != INVALID_SOCKET) 
        return serverSock;

    // 2. Server down: attempt to spawn backend executable
    if (!startServer(serverPort)) {
        return INVALID_SOCKET;
    }

    // 3. Poll until server opens port or max retries reached
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; ++i) {
        this_thread::sleep_for(chrono::milliseconds(STARTUP_POLL_INTERVAL_MS));
        serverSock = connectToServer(serverHost.c_str(), serverPort);
        if (serverSock != INVALID_SOCKET) 
            return serverSock;
    }

    return INVALID_SOCKET;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY
// =============================================================================================

void relayOneClient(ClientInfo& c)
{
    // 1. Connect/Wakeup Backend Server
    SOCKET serverSock = connectToServerWithWakeup();
    if (serverSock == INVALID_SOCKET) {
        sendError(c.sock, "backend server failed to start");
        closesocket(c.sock);
        return;
    }

    char buffer[IO_BUFFER_SIZE];

    // 2. Client -> Server transmission phase
    while (true) {
        int bytesRead = recv(c.sock, buffer, IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0) 
            break; // Client finished or disconnected
        if (!sendAll(serverSock, buffer, bytesRead)) 
            break;
    }

    // 3. Notify server client payload is complete
    shutdown(serverSock, SD_SEND);

    // 4. Server -> Client response phase
    while (true) {
        int bytesRead = recv(serverSock, buffer, IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0) 
            break; // Server finished response
        if (!sendAll(c.sock, buffer, bytesRead)) 
            break;
    }

    // 5. Cleanup sockets
    closesocket(serverSock);
    closesocket(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(SOCKET listenSock)
{
    while (flag) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock == INVALID_SOCKET) {
            // Check for timeout to continue flag verification loop
            if (WSAGetLastError() == WSAEWOULDBLOCK) 
                continue;
            if (!flag) 
                break;
            cerr << "accept error: " << WSAGetLastError() << endl;
            continue;
        }

        // Convert IP address to string format
        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));

        ClientInfo c;
        c.sock = clientSock;
        c.address = string(ipBuf);
        c.port = ntohs(clientAddr.sin_port);
        c.arrivedAt = time(nullptr);

        // Queue capacity verification inside mutex block
        lock_guard<mutex> lock(queueMutex);
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
            sendError(clientSock, "server busy, try again later");
            closesocket(clientSock);
        }
        else {
            pendingClients.enqueue(c);
        }
    }
}

void dispatcherLoop()
{
    while (flag || !pendingClients.isEmpty()) {
        ClientInfo c;
        bool hasClient = false;

        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty()) {
                c = pendingClients.dequeue();
                hasClient = true;
            }
        }

        if (hasClient) {
            relayOneClient(c);
        }
        else {
            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char* argv[])
{
    // Initialize Winsock
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        cerr << "WSAStartup failed with error: " << res << endl;
        return 1;
    }

    SOCKET listenSock = createListenSocket(lbPort);
    cout << "Load Balancer listening on port " << lbPort << "..." << endl;

    // Attach signal handler for graceful shutdown (Ctrl+C)
    signal(SIGINT, handler);

    // Launch worker acceptor thread
    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    // Run primary dispatcher on main thread
    dispatcherLoop();

    cout << "\nShutting down load balancer gracefully..." << endl;
    closesocket(listenSock);
    WSACleanup();

    return 0;
}