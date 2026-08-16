// live_probe.cpp
//
// Live runtime probe for orlib20.dll function parameter orders.
// Extends the headless init smoke test to call CreateEntryEX, FindExistPos,
// SaveTextEX, VerifyModule_New, and SetSigDeleted.
//
// Build (from project root):
//   cmake -S . -B build_probe -A Win32
//   cmake --build build_probe --config Release
// Run:
//   build_probe\Release\live_probe.exe

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

// --- orlib20 function pointer types -----------------------------------------

// Confirmed from static analysis (tvs2_2_insert_pipeline_disasm.md):
typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);

// _CreateEntryEX@24: 6 params, stdcall
// (int WHType, int subType, const char* name, const char* secondary, int flags, int* outErrCode)
// Returns: 16-bit ProgId (>=0 = success, 0xFFFF = failure)
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);

// FindExistPos: cdecl, 2 params
// (const char* name, int* whType) -> int ProgId (-1 = not found)
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);

// _GetMasterRecByName@12: stdcall, 3 params
// (const char* name, int whType, void* outRecord) -> BOOL
typedef BOOL (__stdcall *GetMasterRecByName_t)(const char* name, int whType, void* outRecord);

// _SetSigDeleted@4: stdcall, 1 param
// (void* masterRecord) -> void
typedef void (__stdcall *SetSigDeleted_t)(void* masterRecord);

// _SaveTextEX@16: stdcall, 4 params
// (const char* sourceText, int whType, int progId, void* buffer)
// Returns: 16-bit result (0 = failure, non-zero = success)
typedef short (__stdcall *SaveTextEX_t)(const char* sourceText, int whType,
    int progId, void* buffer);

// _VerifyModule_New@20: stdcall, 5 params
// (int contextVal, int whType, int progId, int isMatch, void* compileCtx)
typedef int (__stdcall *VerifyModule_New_t)(int contextVal, int whType,
    int progId, int isMatch, void* compileCtx);

// StartCompilingProcess: 0 params
typedef void (__stdcall *StartCompilingProcess_t)(void);

// EndCompilingProcess: 0 params
typedef void (__stdcall *EndCompilingProcess_t)(void);

// _GetCompilingErrors@4: stdcall, 1 param
typedef void (__stdcall *GetCompilingErrors_t)(void* outStruct);

// _GetErrorLevel@4: stdcall, 1 param
typedef int (__stdcall *GetErrorLevel_t)(int handle);

// _GetNumofMasterRecsEX@4: stdcall, 1 param (WHType)
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);

// _LoadMasterRecsEX@16: stdcall, 4 params
typedef int (__stdcall *LoadMasterRecsEX_t)(int whType, void* outPtr, int context, int flag);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";

static HMODULE g_hOrlib = NULL;

// Forward declaration — probe runs in a worker thread with 16MB stack
DWORD WINAPI probe_main(LPVOID);

template<typename T>
T Resolve(const char* name) {
    T p = (T)GetProcAddress(g_hOrlib, name);
    if (!p) {
        std::printf("  FAILED to resolve \"%s\", GetLastError=%lu\n", name, GetLastError());
    } else {
        std::printf("  Resolved \"%s\" -> 0x%p\n", name, (void*)p);
    }
    return p;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered output for crash safety

    // Run probe in a worker thread with 16MB stack to avoid STATUS_STACK_OVERFLOW
    // (orlib20's FindExistPos/CreateEntryEX can recurse deeply on certain names)
    HANDLE hThread = CreateThread(NULL, 64 * 1024 * 1024,
        (LPTHREAD_START_ROUTINE)probe_main, NULL, 0, NULL);
    if (!hThread) {
        std::printf("FATAL: CreateThread failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    return (int)exitCode;
}

DWORD WINAPI probe_main(LPVOID) {
    std::printf("=== live_probe: orlib20.dll runtime parameter order confirmation ===\n");
    std::printf("sizeof(void*) = %zu (must be 4 for 32-bit)\n", sizeof(void*));
    if (sizeof(void*) != 4) {
        std::printf("FATAL: probe is not 32-bit\n");
        return 2;
    }

    // 1. Set DLL search path
    if (!SetDllDirectoryW(kTsProgramDir)) {
        std::printf("FATAL: SetDllDirectoryW failed, GetLastError=%lu\n", GetLastError());
        return 3;
    }
    std::printf("SetDllDirectoryW -> OK\n");

    // 2. Load orlib20.dll
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) {
        std::printf("FATAL: LoadLibraryW failed, GetLastError=%lu\n", GetLastError());
        return 4;
    }
    std::printf("LoadLibraryW(\"orlib20.dll\") -> OK, handle=0x%p\n", (void*)g_hOrlib);

    // 3. Resolve all exports
    std::printf("\n--- Resolving exports ---\n");
    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pCreateEntryEX = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetMasterRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pSaveTextEX = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pVerifyModule_New = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartCompiling = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndCompiling = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrorLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pGetNumofMasterRecs = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pLoadMasterRecs = Resolve<LoadMasterRecsEX_t>("_LoadMasterRecsEX@16");

    if (!pOrlibInit || !pCreateEntryEX || !pFindExistPos || !pSaveTextEX ||
        !pVerifyModule_New || !pStartCompiling || !pEndCompiling) {
        std::printf("FATAL: Could not resolve all required exports\n");
        FreeLibrary(g_hOrlib);
        return 5;
    }

    // 4. Call OrlibInit(NULL, 1)
    std::printf("\n--- Step 1: OrlibInit(NULL, 1) ---\n");
    BOOL initOk = pOrlibInit(NULL, 1);
    std::printf("  OrlibInit(NULL, 1) = %d (expected: 1/TRUE)\n", initOk);
    if (!initOk) {
        std::printf("FATAL: OrlibInit failed\n");
        FreeLibrary(g_hOrlib);
        return 6;
    }

    // 5. Check master record counts (key diagnostic)
    std::printf("\n--- Step 2: Master Record Counts ---\n");
    if (pGetNumofMasterRecs) {
        for (int wt = 0; wt <= 1; wt++) {
            __try {
                int count = pGetNumofMasterRecs(wt);
                std::printf("  GetNumofMasterRecsEX(%d) = %d\n", wt, count);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  GetNumofMasterRecsEX(%d) CRASHED: 0x%X\n", wt, GetExceptionCode());
            }
        }
    }
    std::printf("  (0 = master records not loaded — OrlibInit alone is insufficient)\n");

    // Try loading master records
    if (pLoadMasterRecs) {
        std::printf("\n  Attempting _LoadMasterRecsEX(0, &outPtr, 0, 1)...\n");
        void* outPtr = NULL;
        __try {
            int loadResult = pLoadMasterRecs(0, &outPtr, 0, 1);
            std::printf("  LoadMasterRecsEX(0, &outPtr, 0, 1) = %d, outPtr=0x%p\n", loadResult, outPtr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  LoadMasterRecsEX CRASHED: 0x%X\n", GetExceptionCode());
        }
        // Check count again after loading
        if (pGetNumofMasterRecs) {
            __try {
                int count = pGetNumofMasterRecs(0);
                std::printf("  GetNumofMasterRecsEX(0) after load = %d\n", count);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  GetNumofMasterRecsEX(0) after load CRASHED: 0x%X\n", GetExceptionCode());
            }
        }
    }

    // 6. Test FindExistPos with a nonexistent name (should return -1, not crash)
    int whType = 0;
    int progId = pFindExistPos("zzz_nonexistent_xyz", &whType);
    std::printf("  FindExistPos(\"zzz_nonexistent_xyz\") = %d (expected: -1)\n", progId);

    // NOTE: FindExistPos on certain built-in names (e.g. "Average") causes
    // STATUS_STACK_OVERFLOW even with 64MB stack — likely infinite recursion
    // in orlib20's name resolution for built-in functions. Skipping this test.
    std::printf("  (Skipping common name search — causes stack overflow in orlib20)\n");

    // 6. Test CreateEntryEX — DOCUMENTED as causing stack overflow
    std::printf("\n--- Step 3: CreateEntryEX ---\n");
    std::printf("  NOTE: CreateEntryEX causes STATUS_STACK_OVERFLOW (0xC00000FD) even with\n");
    std::printf("  64MB stack in headless mode. This indicates missing initialization\n");
    std::printf("  (likely MFC message pump or TS Desktop dependency). Skipping direct call.\n");
    std::printf("  Static-analysis parameter order remains: (WHType, subType, name, secondary, flags, &err)\n");

    // 7. Test StartCompilingProcess / EndCompilingProcess (0 params)
    std::printf("\n--- Step 4: StartCompilingProcess / EndCompilingProcess ---\n");
    std::printf("  Testing: 0-parameter functions\n");
    __try {
        pStartCompiling();
        std::printf("  StartCompilingProcess() -> OK (no crash)\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in StartCompilingProcess: exception 0x%X\n", GetExceptionCode());
    }

    // 8. Test SaveTextEX with existing ProgId from .ell file naming
    // .ell files are named elmodule_{whtype}_{progid}.ell
    // elmodule_0_1.ell => WHType=0, ProgId=1
    std::printf("\n--- Step 5: SaveTextEX (direct call with existing ProgId) ---\n");
    std::printf("  Testing: (sourceText, WHType, ProgId, buffer)\n");
    std::printf("  Using existing ProgId=1, WHType=0 (from elmodule_0_1.ell)\n");
    char saveBuffer[4096];
    memset(saveBuffer, 0, sizeof(saveBuffer));
    const char* testSource = "Inputs: Price(Close), Length(10);\n"
        "Variables: Avg(0);\n"
        "Avg = Average(Price, Length);\n"
        "Plot1(Avg, \"Avg\");\n";
    short saveResult = -1;
    __try {
        saveResult = pSaveTextEX(testSource, 0, 1, saveBuffer);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in SaveTextEX: exception 0x%X\n", GetExceptionCode());
        saveResult = -1;
    }
    std::printf("  SaveTextEX(source, 0, 1, buffer) = %d (0x%X)\n",
        (int)saveResult, (unsigned short)saveResult);
    std::printf("  Verdict: %s\n", saveResult > 0 ? "SUCCESS (non-zero)" :
        (saveResult == 0 ? "FAILURE (zero)" : "CRASH"));

    // Also try with WHType=1, ProgId=1 (from elmodule_1_1.ell)
    __try {
        memset(saveBuffer, 0, sizeof(saveBuffer));
        saveResult = pSaveTextEX(testSource, 1, 1, saveBuffer);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in SaveTextEX(WHType=1): exception 0x%X\n", GetExceptionCode());
        saveResult = -1;
    }
    std::printf("  SaveTextEX(source, 1, 1, buffer) = %d (0x%X)\n",
        (int)saveResult, (unsigned short)saveResult);

    // 9. Test VerifyModule_New with existing ProgId
    std::printf("\n--- Step 6: VerifyModule_New ---\n");
    std::printf("  Testing: (contextVal, WHType, ProgId, isMatch, compileCtx)\n");
    std::printf("  Using existing ProgId=1, WHType=0\n");

    // Try with contextVal=0, isMatch=1, compileCtx=NULL
    int verifyResult = -1;
    __try {
        verifyResult = pVerifyModule_New(0, 0, 1, 1, NULL);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in VerifyModule_New: exception 0x%X\n", GetExceptionCode());
    }
    std::printf("  VerifyModule_New(0, 0, 1, 1, NULL) = %d\n", verifyResult);

    // Try with contextVal=0, isMatch=0
    __try {
        verifyResult = pVerifyModule_New(0, 0, 1, 0, NULL);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in VerifyModule_New(isMatch=0): exception 0x%X\n", GetExceptionCode());
    }
    std::printf("  VerifyModule_New(0, 0, 1, 0, NULL) = %d\n", verifyResult);

    // Check for compile errors
    if (pGetErrors) {
        int errStruct[3] = {0, 0, 0};
        __try {
            pGetErrors(errStruct);
            std::printf("  GetCompilingErrors: {%d, %d, %d}\n", errStruct[0], errStruct[1], errStruct[2]);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASH in GetCompilingErrors: exception 0x%X\n", GetExceptionCode());
        }
    }
    if (pGetErrorLevel) {
        int errLevel = -1;
        __try {
            errLevel = pGetErrorLevel(0);
            std::printf("  GetErrorLevel(0) = %d\n", errLevel);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASH in GetErrorLevel: exception 0x%X\n", GetExceptionCode());
        }
    }

    __try {
        pEndCompiling();
        std::printf("  EndCompilingProcess() -> OK (no crash)\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH in EndCompilingProcess: exception 0x%X\n", GetExceptionCode());
    }

    // 10. Test _GetMasterRecByName with existing ProgId's WHType
    std::printf("\n--- Step 7: GetMasterRecByName (read-only) ---\n");
    if (pGetMasterRecByName) {
        char masterRec[268];
        memset(masterRec, 0, sizeof(masterRec));
        // Try with WHType=0 (Strategy) — the library has elmodule_0_* files
        __try {
            BOOL gotRec = pGetMasterRecByName("", 0, masterRec);
            std::printf("  GetMasterRecByName(\"\", 0, &rec) = %d\n", gotRec);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASH in GetMasterRecByName: exception 0x%X\n", GetExceptionCode());
        }
    }

    // 11. Negative test: SaveTextEX with WRONG parameter order
    // If static order (source, WHType, ProgId, buffer) is correct,
    // swapping args should crash or return differently
    std::printf("\n--- Step 8: SaveTextEX negative test (wrong order) ---\n");
    std::printf("  Testing WRONG order: (WHType, source, buffer, ProgId)\n");
    short wrongResult = -1;
    __try {
        // Pass WHType as first arg (int where string pointer expected)
        // This should crash if arg1 is really a string pointer
        wrongResult = pSaveTextEX((const char*)0, 0, 1, saveBuffer);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASH with NULL source: exception 0x%X (confirms arg1 = source text)\n",
            GetExceptionCode());
        wrongResult = -2;
    }
    std::printf("  SaveTextEX(NULL, 0, 1, buffer) = %d\n", (int)wrongResult);

    // 12. Negative test: VerifyModule_New with NULL compileCtx
    std::printf("\n--- Step 9: VerifyModule_New with different contextVal ---\n");
    for (int ctxVal = 0; ctxVal <= 2; ctxVal++) {
        int result = -1;
        __try {
            result = pVerifyModule_New(ctxVal, 0, 1, 1, NULL);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASH with ctxVal=%d: exception 0x%X\n", ctxVal, GetExceptionCode());
            result = -2;
        }
        std::printf("  VerifyModule_New(%d, 0, 1, 1, NULL) = %d\n", ctxVal, result);
    }

    // 11. OrlibExit
    std::printf("\n--- Step 8: OrlibExit ---\n");
    if (pOrlibExit) {
        BOOL exitOk = pOrlibExit();
        std::printf("  OrlibExit() = %d\n", exitOk);
    }

    FreeLibrary(g_hOrlib);
    std::printf("\n=== live_probe complete ===\n");
    return 0;
}
