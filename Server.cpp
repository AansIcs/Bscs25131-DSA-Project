// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>   
#include <cstdint>
#include <cstdio>
#include <cstring>    
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; 
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;



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
    Stack()
    {
        top = nullptr;
        count = 0;
    }

    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            cerr << "Stack Overflow Reached" << endl;
            return;
        }
        Node* newNode = new Node();
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
    }

    T pop()
    {
        if (isEmpty())
        {
            cerr << "Stack is Empty" << endl;
            return T();
        }
        Node* Temp = top;
        T value = top->data;
        top = top->next;
        delete Temp;
        count--;
        return value;
    }

    T& peek()
    {
        if (isEmpty())
        {
            cerr << "Stack is Empty" << endl;
            static T dummy;
            return dummy;
        }
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
        while (curr != nullptr && written < maxLen)
        {
            out[written] = curr->data;
            written++;
            curr = curr->next;
        }
        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head;
    TimelineNode* tail;
    int32_t       stepCount;

public:
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
            tail->next = NewNode;
        else
            head = NewNode;

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
        while (curr != nullptr)
        {
            TimelineNode* temp = curr->next;
            delete curr->data; 
            delete curr;         
            curr = temp;        
        }
    }
};

// Core structs
struct Variable
{
    string  name;
    int32_t value;
};
struct Frame
{
    string   func_name;
    int32_t  argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t  returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t  localCount;
};
struct Snapshot
{
    Frame   callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char    magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}
struct FuncEntry
{
    string  funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string  targetFuncName;
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

string lowercase(const string& s)
{
    string r = s;
    for (char& c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        cerr << "Cannot Open File " << sourcePath << endl;
        return false;
    }

    int32_t depth = 0;
    int32_t funCount = 0;
    int32_t NumLines = 0;
    string  Lines;

    while (readSourceLine(in, Lines))
    {
        NumLines++;
        string kw = lowercase(firstWord(Lines));

        if (kw == "func")
        {
            if (depth > 0)
            {
                cerr << "ERROR line " << NumLines
                    << ": nested func inside another func is not allowed." << endl;
                return false;
            }
            depth++;
            funCount++;
        }
        else if (kw == "func_end")
        {
            if (depth == 0)
            {
                cerr << "ERROR line " << NumLines
                    << ": func_end without a matching func." << endl;
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

    cout << "OK " << funCount << " function(s) validated." << endl;
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t recordStart = (int64_t)ftell(f);
    int32_t textSize = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&textSize, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, textSize, f);
    return recordStart;
}

int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField = 0;
    int32_t textSize = 0;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&textSize, sizeof(int32_t), 1, f) != 1)
        return -1;

    char* buf = new char[textSize + 1];
    if (fread(buf, 1, textSize, f) != (size_t)textSize)
    {
        delete[] buf;
        return -1;
    }
    buf[textSize] = '\0';
    outText = string(buf);
    delete[] buf;
    return offsetField;
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry    funcArray[MAX_FUNCS];
    int32_t      funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t      patchCount = 0;

    ifstream in(sourcePath);
    if (!in.is_open())
    {
        cerr << "ERROR: cannot open " << sourcePath << endl;
        return -1;
    }

    FILE* out = fopen(resolveBinPath, "wb");
    if (!out)
    {
        cerr << "ERROR: cannot create " << resolveBinPath << endl;
        return -1;
    }

    int64_t currentOffset = 0;
    string  line;

    while (readSourceLine(in, line))
    {
        int64_t recordStart = writeResolveRecord(out, currentOffset, line);
        string  kw = lowercase(firstWord(line));

        if (kw == "func")
        {
            if (funcCount < MAX_FUNCS)
            {
                funcArray[funcCount].funcName = secondWord(line);
                funcArray[funcCount].byteOffsetInResolveBin = recordStart;
                funcCount++;
            }
        }
        else if (kw == "call")
        {
            if (patchCount < MAX_PATCHES)
            {
                patches[patchCount].byteOffsetOfOffsetField = recordStart;
                patches[patchCount].targetFuncName = secondWord(line);
                patchCount++;
            }
        }
        currentOffset = recordStart + 8 + 4 + (int64_t)line.size();
    }

    in.close();
    fclose(out);

    FILE* rw = fopen(resolveBinPath, "r+b");
    if (!rw)
    {
        cerr << "ERROR: cannot re-open " << resolveBinPath << " for patching" << endl;
        return -1;
    }

    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = -1;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset == -1)
        {
            cerr << "ERROR: call to undefined function '"<< patches[i].targetFuncName << "'" << endl;
            fclose(rw);
            return -1;
        }

        fseek(rw, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        string  oldText;
        readResolveRecord(rw, oldText);   

        istringstream iss(oldText);
        string token;
        string newText;
        int    wordIdx = 0;

        while (iss >> token)
        {
            if (wordIdx == 1)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)targetOffset);
                newText += buf;
            }
            else
            {
                newText += token;
            }
            newText += ' ';
            wordIdx++;
        }
        if (!newText.empty() && newText.back() == ' ')
            newText.pop_back();

        if (newText.size() < oldText.size())
            newText.append(oldText.size() - newText.size(), ' ');
        else if (newText.size() > oldText.size())
            newText = newText.substr(0, oldText.size());

        fseek(rw, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        writeResolveRecord(rw, patches[i].byteOffsetOfOffsetField, newText);
    }

    fclose(rw);

    int64_t mainOffset = -1;
    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }

    if (mainOffset == -1)
    {
        cerr << "ERROR: no 'main' function found — cannot execute." << endl;
        return -1;
    }

    cout << "OK resolve.bin written. main at offset 0x"<< hex << mainOffset << dec << endl;
    return mainOffset;
}

// PASS 0x2: EXECUTION
enum TokenType { KEYWORD, IDENTIFIER, PARAM };
struct Token
{
    TokenType type;
    string    text;
};

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    istringstream iss(line);
    string        word;
    int32_t       count = 0;
    int32_t       wordIdx = 0;

    while ((iss >> word) && count < maxTokens)
    {
        Token t;
        if (wordIdx == 0) 
            t.type = KEYWORD;
        else if (wordIdx == 1) 
            t.type = IDENTIFIER;
        else 
            t.type = PARAM;
        t.text = word;
        tokens[count] = t;
        count++;
        wordIdx++;
    }
    return count;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

Variable* findOrCreateLocal(Frame& frame, const string& name)
{
    for (int32_t i = 0; i < frame.localCount; i++)
        if (frame.locals[i].name == name)
            return &frame.locals[i];

    if (frame.localCount < MAX_VARS_PER_FRAME)
    {
        frame.locals[frame.localCount].name = name;
        frame.locals[frame.localCount].value = 0;
        return &frame.locals[frame.localCount++];
    }

    cerr << "ERROR: too many locals in function '" << frame.func_name << "'" << endl;
    return &frame.locals[0];
}

int32_t resolveValue(const string& token, Frame& frame)
{
    bool isNum = !token.empty();
    for (char c : token)
    {
        if (!isdigit((unsigned char)c) && c != '-')
        {
            isNum = false; 
            break;
        }
    }
    if (isNum)
        return (int32_t)strtol(token.c_str(), nullptr, 10);

    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        return (int32_t)strtol(token.c_str(), nullptr, 16);

    for (int32_t i = 0; i < frame.argc; i++)
        if (frame.argv[i].name == token)
            return frame.argv[i].value;

    for (int32_t i = 0; i < frame.localCount; i++)
        if (frame.locals[i].name == token)
            return frame.locals[i].value;

    cerr << "WARNING: variable '" << token << "' not found in frame '" << frame.func_name << "', defaulting to 0" << endl;
    return 0;
}

void syncParamBack(Frame& callerFrame, const string& callerVarName, int32_t newValue)
{
    for (int32_t i = 0; i < callerFrame.argc; i++)
        if (callerFrame.argv[i].name == callerVarName)
        {
            callerFrame.argv[i].value = newValue; return;
        }

    for (int32_t i = 0; i < callerFrame.localCount; i++)
        if (callerFrame.locals[i].name == callerVarName)
        {
            callerFrame.locals[i].value = newValue; return;
        }
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f)
    {
        cerr << "ERROR: cannot open " << resolveBinPath << endl;
        return;
    }

    Stack<Frame> callStack;

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    fseek(f, (long)mainOffset, SEEK_SET);

    while (!callStack.isEmpty())
    {
        string  line;
        int64_t recOffset = readResolveRecord(f, line);
        if (recOffset == -1)
            break;

        Token   tokens[MAX_TOKENS];
        int32_t nTokens = tokenizeLine(line, tokens, MAX_TOKENS);
        if (nTokens == 0) 
            continue;

        string kw = lowercase(tokens[0].text);

        if (kw == "func")
        {
            
        }
        else if (kw == "func_end")
        {
            Frame finished = callStack.pop();

            if (callStack.isEmpty())
            {
                timeline.record(buildSnapshot(callStack));
                break;
            }

            Frame& caller = callStack.peek();
            for (int32_t i = 0; i < finished.argc; i++)
                syncParamBack(caller, finished.argv[i].name, finished.argv[i].value);

            if (finished.returnLine >= 0)
                fseek(f, (long)finished.returnLine, SEEK_SET);
        }
        else if (kw == "set")
        {
            if (nTokens >= 3)
            {
                string    varName = tokens[1].text;
                int32_t   val = resolveValue(tokens[2].text, callStack.peek());
                Variable* v = findOrCreateLocal(callStack.peek(), varName);
                v->value = val;
            }
        }
        else if (kw == "add")
        {
            if (nTokens >= 3)
            {
                Frame& frame = callStack.peek();
                int32_t srcVal = resolveValue(tokens[2].text, frame);
                bool    found = false;
                for (int32_t i = 0; i < frame.argc; i++)
                    if (frame.argv[i].name == tokens[1].text)
                    {
                        frame.argv[i].value += srcVal; found = true; break;
                    }
                if (!found)
                {
                    Variable* v = findOrCreateLocal(frame, tokens[1].text);
                    v->value += srcVal;
                }
            }
        }
        else if (kw == "sub")
        {
            if (nTokens >= 3)
            {
                Frame& frame = callStack.peek();
                int32_t srcVal = resolveValue(tokens[2].text, frame);
                bool    found = false;
                for (int32_t i = 0; i < frame.argc; i++)
                    if (frame.argv[i].name == tokens[1].text)
                    {
                        frame.argv[i].value -= srcVal; found = true; break;
                    }
                if (!found)
                {
                    Variable* v = findOrCreateLocal(frame, tokens[1].text);
                    v->value -= srcVal;
                }
            }
        }
        else if (kw == "mul")
        {
            if (nTokens >= 3)
            {
                Frame& frame = callStack.peek();
                int32_t srcVal = resolveValue(tokens[2].text, frame);
                bool    found = false;
                for (int32_t i = 0; i < frame.argc; i++)
                    if (frame.argv[i].name == tokens[1].text)
                    {
                        frame.argv[i].value *= srcVal; found = true; break;
                    }
                if (!found)
                {
                    Variable* v = findOrCreateLocal(frame, tokens[1].text);
                    v->value *= srcVal;
                }
            }
        }
        else if (kw == "div")
        {
            if (nTokens >= 3)
            {
                Frame& frame = callStack.peek();
                int32_t srcVal = resolveValue(tokens[2].text, frame);
                if (srcVal == 0)
                {
                    cerr << "ERROR: division by zero" << endl;
                }
                else
                {
                    bool found = false;
                    for (int32_t i = 0; i < frame.argc; i++)
                        if (frame.argv[i].name == tokens[1].text)
                        {
                            frame.argv[i].value /= srcVal; found = true; break;
                        }
                    if (!found)
                    {
                        Variable* v = findOrCreateLocal(frame, tokens[1].text);
                        v->value /= srcVal;
                    }
                }
            }
        }
        else if (kw == "call")
        {
            if (nTokens >= 2)
            {
                int64_t targetOffset = (int64_t)strtoll(tokens[1].text.c_str(), nullptr, 0);
                int64_t returnAddr = (int64_t)ftell(f);
                Frame& callerFrame = callStack.peek();

                fseek(f, (long)targetOffset, SEEK_SET);
                string funcLine;
                readResolveRecord(f, funcLine);

                Frame calleeFrame;
                {
                    istringstream iss2(funcLine);
                    string word;
                    int    wi = 0;
                    calleeFrame.func_name = "";
                    calleeFrame.argc = 0;
                    calleeFrame.localCount = 0;
                    calleeFrame.returnLine = (int32_t)returnAddr;

                    while (iss2 >> word)
                    {
                        if (wi == 0) {
                        }
                        else if (wi == 1) { calleeFrame.func_name = word; }
                        else
                        {
                            int32_t argIdx = wi - 2;
                            int32_t tokenIdx = argIdx + 2;
                            if (argIdx < MAX_VARS_PER_FRAME && tokenIdx < nTokens)
                            {
                                string  callerVarName = tokens[tokenIdx].text;
                                int32_t argValue = resolveValue(callerVarName, callerFrame);
                                calleeFrame.argv[argIdx].name = callerVarName;
                                calleeFrame.argv[argIdx].value = argValue;
                                calleeFrame.argc++;
                            }
                        }
                        wi++;
                    }
                }
                callStack.push(calleeFrame);
            }
        }

        timeline.record(buildSnapshot(callStack));
    }

    fclose(f);
    cout << "OK " << timeline.getStepCount() << " steps recorded." << endl;
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb");
    if (!f)
    {
        cerr << "ERROR: cannot create " << tdbgPath << endl;
        return;
    }

    int32_t stepCount = timeline.getStepCount();

    TTDBHeader hdr;
    memcpy(hdr.magic, "TTDB", 4);
    hdr.version = 1;
    hdr.stepCount = stepCount;
    hdr.indexOffset = 0;           
    writeHeader(f, hdr);

    int64_t* index = new int64_t[stepCount];
    TimelineNode* node = timeline.begin();
    int32_t       step = 0;

    while (node != nullptr && step < stepCount)
    {
        index[step] = (int64_t)ftell(f);
        fwrite(node->data, sizeof(Snapshot), 1, f);
        node = node->next;
        step++;
    }

    int64_t indexStart = (int64_t)ftell(f);
    fwrite(index, sizeof(int64_t), stepCount, f);
    delete[] index;

    fseek(f, 0, SEEK_SET);
    hdr.indexOffset = indexStart;
    writeHeader(f, hdr);

    fclose(f);
    cout << "OK session.tdbg written. " << stepCount<< " snapshots, index at 0x" << hex << indexStart << dec << endl;
}

// main section
int32_t main()
{
    cout << "------------------- Time-Travel Debugger - Phase 1 Server -----------------------" << endl;
    cout << endl;

    if (!validateProgram("source.bin"))
    {
        cerr << "Validation failed - aborting." << endl;
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
        cerr << "Resolve failed - aborting." << endl;
        return 1;
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    if (timeline.getStepCount() == 0)
    {
        cerr << "Execution produced no steps - aborting." << endl;
        return 1;
    }

    writeTdbg(timeline, "session.tdbg");

    cout << "\n---------------------------- Done. session.tdbg is ready for the client.------------------------" << endl;
    return 0;
}