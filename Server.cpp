// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include<sstream>
#include <cstdint>
#include<stdexcept>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        Top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            cerr << "Stack OverFlow Reached" << endl;
            return;
        }
        Node* newNode = new Node(val);
        newNode->next = top;
        top = newNode;
        count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if (isEmpty())
        {
            cerr << "Stack is Empty" << endl;
            return T;
        }
        Node* Temp = top;
        int value = top->data;
        top = top->next;
        delete Temp;
        count--;

        return value;
    }
    T& peek()
    {
        if (isEmpty())
            cerr << "Stack is Empty"; return T;
        return top->data;
       
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    ~Stack()
    {
        while (!isEmpty())
            pop();
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* curr = top;
        int32_t written = 0;

        while (curr != nullptr and written < maxLen)
        {
            out[written] = curr->data;
            written++;
            curr = curr->next;
        }
        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        TimelineNode* NewNode = new TimelineNode();
        NewNode->data = s;
        NewNode->next = nullptr;
        NewNode->prev = tail;

        if (tail != nullptr)
        {
            tail->next=NewNode;
        }
        else
        {
            head = NewNode;
        }
        tail = NewNode;
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
    ~Timeline()
    {
        TimelineNode* curr = head;
        while (curr->next != nullptr)
        {
            TimelineNode* temp = curr->next;
            delete curr->next;
            delete curr;
            curr = temp;
        }
    }
};

// Core structs
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
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string line;
    while (getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        size_t start = line.find_first_not_of(" \t");
        if (start == string::npos)
            continue;           
        line = line.substr(start);
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/')
            continue;
        out = line;
        return true;
    }
    return false;  
}
string firstWord(const string& line)
{
    istringstream is(line);
    string wrd;
    is >> wrd;
    return wrd;
}
string secondWord(const string& line)
{
    istringstream is(line);
    string wrd;
    is >> wrd;
    is >> wrd;
    return wrd;
}
static string lowercase(const string& s)
{
    string r = s;
    for (char& c : r) 
        c = (char)tolower(c);
    return r;
}
bool validateProgram(const char* sourcePath)
{
    ifstream in("sourcePath");
    if (!in.is_open())
    {
        cerr << "Cannot Open File " << sourcePath << endl;
        return false;
    }
    int32_t depth = 0;
    int32_t funCount = 0;
    int32_t NumLines = 0;
    string Lines;
    while (readSourceLine(in, Lines))
    {
        NumLines++;
        string kw = lowercase(firstWord(Lines));
        if (kw == "func")
        {
            if (depth > 0)
            {
                cerr << "ERROR line " << NumLines << ": nested func inside another func is not allowed." << endl;
                return false;
            }
            depth++;
            funCount++;
        }
        else if (kw == "func_end")
        {
            if (depth == 0)
            {
                cerr << "ERROR line " << NumLines << ": func_end without a matching func." << endl;
                return false;
            }
            depth--;
        }
    }
    if (depth != 0)
    {
        cerr << "ERROR: " << depth << " func were never closed with func_end." << endl;
        return false;
    }

    if (funCount == 0)
    {
        cerr << "ERROR: no functions defined in the source." << endl;
        return false;
    }

    cout << "OK " << funCount << " function validated." << endl;
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}