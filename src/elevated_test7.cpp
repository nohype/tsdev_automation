// elevated_test7.cpp
//
// Applies the three critical fixes from static analysis:
// 1. FindExistPos(name, 4) — type 4 for strategies (was using 0)
// 2. SaveTextEX(source, WHType, ProgId, buffer) — bypasses HWND check
// 3. VerifyModule_New with non-zero context
//
// Build: cmake --build build --config Release --target elevated_test7
// Run (MUST be elevated): build\Release\elevated_test7.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int (__stdcall *FindExistPosSubType_t)(const char* name, int type, int subType);
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);
typedef int (__stdcall *SaveTextEX_t)(const char* sourceText, int whType, int progId, void* buffer);
typedef int (__stdcall *SaveModule_New_t)(int whType, int progId,
    void* arg3, HWND hwnd, void* buffer);
typedef int (__stdcall *VerifyModule_New_t)(int contextVal, int whType,
    int progId, int isMatch, void* compileCtx);
typedef void (__stdcall *StartCompilingProcess_t)(void);
typedef void (__stdcall *EndCompilingProcess_t)(void);
typedef void (__stdcall *GetCompilingErrors_t)(void* outStruct);
typedef int (__stdcall *GetErrorLevel_t)(int handle);
typedef void (__stdcall *SetSigDeleted_t)(void* masterRecord);
typedef BOOL (__stdcall *GetMasterRecByName_t)(const char* name, int whType, void* outRecord);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
static HMODULE g_hOrlib = NULL;
static FILE* g_log = NULL;

template<typename T>
T Resolve(const char* name) {
    T p = (T)GetProcAddress(g_hOrlib, name);
    if (!p) fprintf(g_log, "  FAILED to resolve \"%s\"\n", name);
    return p;
}

void logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

DWORD WINAPI probe_main(LPVOID);

int main() {
    g_log = fopen("elevated_test7_log.txt", "w");
    if (!g_log) { printf("FATAL: can't open log file\n"); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);

    HANDLE hThread = CreateThread(NULL, 64 * 1024 * 1024,
        (LPTHREAD_START_ROUTINE)probe_main, NULL, 0, NULL);
    if (!hThread) { logf("FATAL: CreateThread failed\n"); return 1; }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    fclose(g_log);
    return (int)exitCode;
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test7: Apply static analysis fixes ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { logf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    logf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pFindExistPosSubType = Resolve<FindExistPosSubType_t>("_FindExistPosSubType@12");
    auto pCreateEntry = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveTextEX = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry || !pSaveTextEX || !pVerify) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");
    if (pGetNum) {
        __try { logf("  MasterRecs(0) = %d\n", pGetNum(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  MasterRecs(0) CRASHED\n"); }
    }

    // Step 2: Test FindExistPos with type=4 (THE FIX!)
    // First, look up a known existing strategy to verify type=4 works
    logf("\n--- Step 2: Test FindExistPos with type=4 ---\n");
    const char* knownNames[] = {"Average", "MACD", "Close", "High", "Low", NULL};
    for (int i = 0; knownNames[i]; i++) {
        __try {
            int found = pFindExistPos(knownNames[i], 4);
            logf("  FindExistPos(\"%s\", 4) = %d\n", knownNames[i], found);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  FindExistPos(\"%s\", 4) CRASHED: 0x%X\n", knownNames[i], GetExceptionCode());
        }
    }
    // Also try type=0 for comparison
    for (int i = 0; knownNames[i]; i++) {
        __try {
            int found = pFindExistPos(knownNames[i], 0);
            logf("  FindExistPos(\"%s\", 0) = %d  [for comparison]\n", knownNames[i], found);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 3: Clean up leftover test entries from prior runs
    logf("\n--- Step 3: Cleanup leftovers ---\n");
    const char* cleanupNames[] = {
        "zzz_cli_test_probe", "test",
        "zzz_arg3_is_name", "zzz_arg4_is_name",
        "zzz_cli_insert_test",
        "MyTestStrategyA", "MyTestStrategyB", "MyTestStrategyC",
        "MyTestStrategyD", "MyTestStrategyE", "MyTestStrategyF",
        "zzz_frida_test", "zzz_frida_test2",
        NULL
    };
    for (int i = 0; cleanupNames[i]; i++) {
        // Use FindExistPos with type=4 to find them
        __try {
            int progId = pFindExistPos(cleanupNames[i], 4);
            if (progId >= 0) {
                logf("  Found leftover: \"%s\" ProgId=%d\n", cleanupNames[i], progId);
                // Try to delete via GetMasterRecByName + SetSigDeleted
                if (pGetRecByName && pSetSigDeleted) {
                    char rec[268]; memset(rec, 0, sizeof(rec));
                    BOOL got = pGetRecByName(cleanupNames[i], 4, rec);
                    if (got) {
                        pSetSigDeleted(rec);
                        logf("  Deleted: \"%s\"\n", cleanupNames[i]);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  Cleanup crash for \"%s\": 0x%X\n", cleanupNames[i], GetExceptionCode());
        }
    }
    if (pGetNum) {
        __try { logf("  MasterRecs(0) after cleanup = %d\n", pGetNum(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 4: CreateEntryEX
    logf("\n--- Step 4: CreateEntryEX ---\n");
    const char* testName = "zzz_cli_test7";
    const char* testSecondary = "test7 note";
    int errCode = 0;
    short progId = -1;
    __try {
        progId = pCreateEntry(0, 0, testName, testSecondary, 0, &errCode);
        logf("  CreateEntryEX(0, 0, \"%s\", \"%s\", 0, &err) = %d (err=%d / 0x%X)\n",
            testName, testSecondary, (int)progId, errCode, (unsigned)errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
        progId = -1;
    }
    if (progId < 0) {
        logf("\n  CreateEntryEX failed. Aborting.\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }

    // Step 5: FindExistPos with type=4 (THE FIX!)
    logf("\n--- Step 5: FindExistPos with type=4 ---\n");
    __try {
        int found = pFindExistPos(testName, 4);
        logf("  FindExistPos(\"%s\", 4) = %d\n", testName, found);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 6: SaveTextEX (THE FIX! — bypasses HWND check)
    logf("\n--- Step 6: SaveTextEX ---\n");
    const char* testSource =
        "if Close > Open then Buy next bar at market;\n";
    char saveBuffer[4096]; memset(saveBuffer, 0, sizeof(saveBuffer));
    int saveResult = -1;
    __try {
        saveResult = pSaveTextEX(testSource, 0, progId, saveBuffer);
        logf("  SaveTextEX(source, 0, %d, buf) = %d\n", (int)progId, saveResult);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 7: Verify pipeline
    logf("\n--- Step 7: Verify pipeline ---\n");
    if (pStartComp) {
        __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  StartComp CRASH: 0x%X\n", GetExceptionCode()); }
    }
    // Try contextVal=1 (non-zero, matching TSDev)
    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", (int)progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  Verify CRASH: 0x%X\n", GetExceptionCode());
    }
    if (pGetErrors) {
        int e[4] = {0,0,0,0};
        __try { pGetErrors(e); logf("  GetErrors: {%d,%d,%d,%d}\n", e[0],e[1],e[2],e[3]); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  GetErrors CRASH\n"); }
    }
    if (pGetErrLevel) {
        __try { logf("  GetErrorLevel(0) = %d\n", pGetErrLevel(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  GetErrorLevel CRASH\n"); }
    }
    if (pEndComp) {
        __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  EndComp CRASH: 0x%X\n", GetExceptionCode()); }
    }

    // Step 8: Check persistence
    logf("\n--- Step 8: Check persistence ---\n");
    char ellPath[512];
    sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", (int)progId);
    char xmlPath[512];
    sprintf(xmlPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.xml", (int)progId);
    FILE* f = fopen(ellPath, "rb");
    if (f) { logf("  FOUND: %s\n", ellPath); fclose(f); }
    else   { logf("  NOT FOUND: %s\n", ellPath); }
    f = fopen(xmlPath, "rb");
    if (f) { logf("  FOUND: %s\n", xmlPath); fclose(f); }
    else   { logf("  NOT FOUND: %s\n", xmlPath); }

    // Step 9: FindExistPos after verify
    logf("\n--- Step 9: FindExistPos after verify ---\n");
    __try {
        int found = pFindExistPos(testName, 4);
        logf("  FindExistPos(\"%s\", 4) = %d\n", testName, found);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 10: Cleanup
    logf("\n--- Step 10: Cleanup ---\n");
    if (pGetRecByName && pSetSigDeleted) {
        char rec[268]; memset(rec, 0, sizeof(rec));
        __try {
            BOOL got = pGetRecByName(testName, 4, rec);
            if (got) {
                pSetSigDeleted(rec);
                logf("  Deleted: \"%s\"\n", testName);
            } else {
                logf("  Not found for cleanup: \"%s\"\n", testName);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  Cleanup CRASH: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 11: OrlibExit
    logf("\n--- Step 11: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test7 complete ===\n");
    logf("Log written to elevated_test7_log.txt\n");
    return 0;
}
