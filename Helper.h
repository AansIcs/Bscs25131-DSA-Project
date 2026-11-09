#pragma once

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <vector>

using namespace std;
// ---- Structs for Debugger Engine & File Operations ----
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


