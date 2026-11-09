// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE (WINDOWS) =======================

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>

// Winsock headers must come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "helper.h"

// Link with Winsock library in MSVC
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// Shared constants with Phase 1
const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;

// ---- Structs ----
struct Variable
{
    string name;
    int32_t value;
};

struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

#pragma pack(push, 1)
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
#pragma pack(pop)

// ---- Structs only the client needs ----
struct TdbgReader
{
    ifstream file;
    TTDBHeader header;
    vector<int64_t> index;
};

struct DebugSession
{
    bool active_flag;    // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};
inline bool readString(ifstream& in, string& str)
{
    int32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(int32_t));
    if (in.gcount() < static_cast<streamsize>(sizeof(int32_t)) || len < 0 || len > 4096) return false;

    str.resize(len);
    if (len > 0) {
        in.read(&str[0], len);
        if (in.gcount() < static_cast<streamsize>(len)) return false;
    }
    return true;
}

// Deserializes a Snapshot structure directly from an open binary file stream
inline bool readSnapshotFromStream(ifstream& in, Snapshot& snap)
{
    in.read(reinterpret_cast<char*>(&snap.stackDepth), sizeof(int32_t));
    if (in.gcount() < static_cast<streamsize>(sizeof(int32_t)) || snap.stackDepth < 0 || snap.stackDepth > MAX_STACK_DEPTH) {
        return false;
    }

    for (int i = 0; i < snap.stackDepth; ++i) {
        Frame& f = snap.callStack[i];

        if (!readString(in, f.func_name)) return false;

        in.read(reinterpret_cast<char*>(&f.argc), sizeof(int32_t));
        if (f.argc < 0 || f.argc > MAX_VARS_PER_FRAME) return false;

        for (int j = 0; j < f.argc; ++j) {
            if (!readString(in, f.argv[j].name)) return false;
            in.read(reinterpret_cast<char*>(&f.argv[j].value), sizeof(int32_t));
        }

        in.read(reinterpret_cast<char*>(&f.returnLine), sizeof(int32_t));

        in.read(reinterpret_cast<char*>(&f.localCount), sizeof(int32_t));
        if (f.localCount < 0 || f.localCount > MAX_VARS_PER_FRAME) return false;

        for (int j = 0; j < f.localCount; ++j) {
            if (!readString(in, f.locals[j].name)) return false;
            in.read(reinterpret_cast<char*>(&f.locals[j].value), sizeof(int32_t));
        }
    }

    return true;
}

// Verifies CRC-32 checksum embedded at the end of a binary payload file
inline bool verifyChecksum(const char* filePath)
{
    ifstream file(filePath, ios::binary);
    if (!file.is_open()) return false;

    uint32_t crcTable[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        crcTable[i] = c;
    }

    file.seekg(0, ios::end);
    streamsize fileSize = file.tellg();
    if (fileSize < 4) return false;

    streamsize payloadSize = fileSize - 4;
    file.seekg(0, ios::beg);

    uint32_t crc = 0xFFFFFFFF;
    char buffer[4096];
    streamsize remaining = payloadSize;

    while (remaining > 0) {
        streamsize toRead = (remaining < static_cast<streamsize>(sizeof(buffer))) ? remaining : sizeof(buffer);
        file.read(buffer, toRead);
        streamsize readBytes = file.gcount();
        if (readBytes <= 0) break;

        for (streamsize i = 0; i < readBytes; i++) {
            uint8_t byte = static_cast<uint8_t>(buffer[i]);
            crc = crcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        }
        remaining -= readBytes;
    }

    crc ^= 0xFFFFFFFF;

    uint32_t expectedCrc = 0;
    file.read(reinterpret_cast<char*>(&expectedCrc), sizeof(uint32_t));

    return (crc == expectedCrc);
}
// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(SOCKET sock, char* buf, size_t n)
{
    size_t received = 0;
    while (received < n)
    {
        int result = recv(sock, buf + received, static_cast<int>(n - received), 0);
        if (result <= 0)
            return false;
        received += result;
    }
    return true;
}

bool sendAll(SOCKET sock, const char* buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        int result = send(sock, buf + sent, static_cast<int>(n - sent), 0);
        if (result <= 0)
            return false;
        sent += result;
    }
    return true;
}

SOCKET connectToHost(const char* host, int port)
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

bool uploadSource(SOCKET sock, const char* sourcePath)
{
    ifstream inFile(sourcePath, ios::binary);
    if (!inFile.is_open()) return false;

    char buffer[IO_BUFFER_SIZE];
    while (inFile.read(buffer, sizeof(buffer)) || inFile.gcount() > 0)
    {
        streamsize bytesRead = inFile.gcount();
        if (!sendAll(sock, buffer, static_cast<size_t>(bytesRead)))
        {
            inFile.close();
            return false;
        }
    }
    inFile.close();

    shutdown(sock, SD_SEND);
    return true;
}

bool receiveResponse(SOCKET sock, const char* outTdbgPath, string& outError)
{
    char status;
    if (!recvAll(sock, &status, 1))
    {
        outError = "Failed to receive status byte from load balancer.";
        return false;
    }

    if (status == 1) // Error response
    {
        char buffer[1024];
        while (true)
        {
            int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            outError += buffer;
        }
        return false;
    }
    else if (status == 0) // Success response: payload is the file
    {
        ofstream outFile(outTdbgPath, ios::binary);
        if (!outFile.is_open())
        {
            outError = "Could not open local file to write .tdbg payload.";
            return false;
        }

        char buffer[IO_BUFFER_SIZE];
        while (true)
        {
            int bytes = recv(sock, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            outFile.write(buffer, bytes);
        }
        outFile.close();

        // Verification check (helper-dependent)
        if (!verifyChecksum(outTdbgPath))
        {
            remove(outTdbgPath);
            outError = "checksum mismatch, response corrupted";
            return false;
        }
        return true;
    }

    outError = "Unknown status byte returned by server.";
    return false;
}

bool requestDebugFile(const char* host, int port, const string& sourcePath, string& outError)
{
    SOCKET sock = connectToHost(host, port);
    if (sock == INVALID_SOCKET)
    {
        outError = "Failed to connect to load balancer at " + string(host) + ":" + to_string(port);
        return false;
    }

    if (!uploadSource(sock, sourcePath.c_str()))
    {
        outError = "Failed to send source file to load balancer.";
        closesocket(sock);
        return false;
    }

    bool success = receiveResponse(sock, TDBG_FILE, outError);
    closesocket(sock);
    return success;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// =============================================================================================

bool openTdbg(const char* path, TdbgReader& r)
{
    r.file.open(path, ios::binary);
    if (!r.file.is_open()) return false;

    // Read Header
    r.file.read(reinterpret_cast<char*>(&r.header), sizeof(TTDBHeader));
    if (r.file.gcount() < sizeof(TTDBHeader)) {
        r.file.close();
        return false;
    }

    // Check Magic Bytes
    if (memcmp(r.header.magic, "TTDB", 4) != 0) {
        r.file.close();
        return false;
    }

    // Seek to index section and read index offsets
    r.file.seekg(r.header.indexOffset, ios::beg);
    r.index.resize(r.header.stepCount);
    if (r.header.stepCount > 0)
    {
        r.file.read(reinterpret_cast<char*>(r.index.data()), r.header.stepCount * sizeof(int64_t));
        if (r.file.gcount() < static_cast<streamsize>(r.header.stepCount * sizeof(int64_t))) {
            r.file.close();
            return false;
        }
    }

    return true;
}

void closeTdbg(TdbgReader& r)
{
    if (r.file.is_open()) {
        r.file.close();
    }
    r.index.clear();
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount) 
        return nullptr;

    int64_t offset = r.index[step];
    r.file.seekg(offset, ios::beg);

    Snapshot* s = new Snapshot();
    if (!readSnapshotFromStream(r.file, *s)) { 
        delete s;
        return nullptr;
    }

    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// =============================================================================================

void initSession(DebugSession& s)
{
    s.active_flag = false;
    s.currentStep = 0;
}

void closeSession(DebugSession& s)
{
    if (s.active_flag) {
        closeTdbg(s.reader);
        s.active_flag = false;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s);

bool showCurrent(DebugSession& s)
{
    if (!s.active_flag) {
        cout << "No active debugging session. Run 'start <source_file>' first." << endl;
        return false;
    }

    Snapshot* snap = readSnapshotAt(s.reader, static_cast<int32_t>(s.currentStep));
    if (!snap) {
        cout << "damaged .tdbg file" << endl;
        return false;
    }

    printSnapshot(static_cast<int32_t>(s.currentStep + 1), s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}

bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag) {
        cout << "No active debugging session." << endl;
        return false;
    }

    int64_t targetStep = s.currentStep + delta;
    if (targetStep < 0) 
        targetStep = 0;
    if (targetStep >= s.reader.header.stepCount) 
    {
        targetStep = s.reader.header.stepCount - 1;
    }

    s.currentStep = targetStep;
    return showCurrent(s);
}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath, err)) {
        cout << "Error: " << err << endl;
        return false;
    }

    TdbgReader newReader;
    if (!openTdbg(TDBG_FILE, newReader)) {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    if (s.active_flag) {
        closeTdbg(s.reader);
    }

    s.reader = move(newReader);
    s.active_flag = true;
    s.currentStep = 0;

    return showCurrent(s);
}

bool cmdNext(DebugSession& s) {
    return moveBy(s, 1);
}

bool cmdPrv(DebugSession& s) {
    return moveBy(s, -1);
}

bool cmdJmpNext(DebugSession& s, int32_t n) {
    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession& s, int32_t n) {
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "=== Step " << step << " / " << totalSteps << " ===" << endl;
    for (int i = 0; i < s.stackDepth; ++i) {
        const Frame& f = s.callStack[i];
        cout << "[" << i << "] " << f.func_name << "() (return line " << f.returnLine << ")" << endl;

        if (f.argc > 0) {
            cout << "    Arguments:" << endl;
            for (int j = 0; j < f.argc; ++j) {
                cout << "      " << f.argv[j].name << " = " << f.argv[j].value << endl;
            }
        }
        if (f.localCount > 0) {
            cout << "    Locals:" << endl;
            for (int j = 0; j < f.localCount; ++j) {
                cout << "      " << f.locals[j].name << " = " << f.locals[j].value << endl;
            }
        }
    }
}

void printHelp()
{
    cout << "Available Commands:" << endl;
    cout << "  start <path>     - Upload source file and begin debugging session" << endl;
    cout << "  next             - Move forward 1 step" << endl;
    cout << "  prv              - Move backward 1 step" << endl;
    cout << "  jmp next <n>     - Jump forward n steps" << endl;
    cout << "  jmp prv <n>      - Jump backward n steps" << endl;
    cout << "  help             - Show this help menu" << endl;
    cout << "  quit / exit      - Terminate program" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    stringstream ss(line);
    vector<string> tokens;
    string word;
    while (ss >> word) {
        tokens.push_back(word);
    }

    if (tokens.empty()) return true;

    // Optional lead word "ttdb" removal
    if (tokens[0] == "ttdb") {
        tokens.erase(tokens.begin());
        if (tokens.empty()) return true;
    }

    string cmd = tokens[0];

    if (cmd == "start") {
        if (tokens.size() != 2) {
            cout << "Usage: start <source_path>" << endl;
            return false;
        }
        return cmdStart(s, tokens[1]);
    }
    else if (cmd == "next") {
        if (tokens.size() != 1) {
            cout << "Usage: next" << endl;
            return false;
        }
        return cmdNext(s);
    }
    else if (cmd == "prv") {
        if (tokens.size() != 1) {
            cout << "Usage: prv" << endl;
            return false;
        }
        return cmdPrv(s);
    }
    else if (cmd == "jmp") {
        if (tokens.size() != 3) {
            cout << "Usage: jmp next <n> OR jmp prv <n>" << endl;
            return false;
        }
        int32_t n = atoi(tokens[2].c_str());
        if (n <= 0 || n > MAX_JUMP) {
            cout << "Invalid jump distance. Must be between 1 and " << MAX_JUMP << endl;
            return false;
        }
        if (tokens[1] == "next") {
            return cmdJmpNext(s, n);
        }
        else if (tokens[1] == "prv") {
            return cmdJmpPrv(s, n);
        }
        else {
            cout << "Unknown direction. Use 'next' or 'prv'." << endl;
            return false;
        }
    }
    else if (cmd == "help") {
        printHelp();
        return true;
    }
    else if (cmd == "quit" || cmd == "exit") {
        quitRequested = true;
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}

int main(int argc, char* argv[])
{
    // Host/Port override via CLI arguments
    if (argc >= 2) 
        LB_HOST = argv[1];
    if (argc >= 3) 
        LB_PORT = atoi(argv[2]);

    // Initialize Winsock
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        cerr << "WSAStartup failed: " << res << endl;
        return 1;
    }

    DebugSession session;
    initSession(session);

    cout << "=== Time-Travel Debugger Client ===" << endl;
    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested) {
        cout << PROMPT;
        if (!getline(cin, line)) {
            break; // End of file (EOF) reached
        }
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    WSACleanup();
    return 0;
}