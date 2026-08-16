// elevated_test6.cpp
//
// Clean test: properly clean up leftovers, use working CreateEntryEX pattern,
// test SaveModule_New + Verify with non-zero contextVal, write log to file.
//
// Build: cmake --build build --config Release --target elevated_test6
// Run (MUST be elevated): build\Release\elevated_test6.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);
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
typedef void (__stdcall *CloseWarehouse_t)(int whType);

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
    // Also print to stdout
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

DWORD WINAPI probe_main(LPVOID);

int main() {
    g_log = fopen("elevated_test6_log.txt", "w");
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
    logf("=== elevated_test6: Clean INSERT+persist test ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { logf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    logf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pCreateEntry = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");
    auto pCloseWh = Resolve<CloseWarehouse_t>("_CloseWarehouseForCurrentThreadAndPath@4");

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry || !pSaveModule || !pVerify) {
        logf("FATAL: missing exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");
    if (pGetNum) {
        __try { logf("  MasterRecs(0) = %d\n", pGetNum(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  MasterRecs(0) CRASHED\n"); }
    }

    // Step 2: Clean up ALL leftover test entries from prior runs
    logf("\n--- Step 2: Cleanup leftovers ---\n");
    const char* cleanupNames[] = {
        "zzz_cli_test_probe", "test",
        "zzz_arg3_is_name", "zzz_arg4_is_name",
        "MyTestStrategyA", "MyTestStrategyB", "MyTestStrategyC",
        "MyTestStrategyD", "MyTestStrategyE", "MyTestStrategyF",
        NULL
    };
    for (int i = 0; cleanupNames[i]; i++) {
        if (pGetRecByName && pSetSigDeleted) {
            char rec[268]; memset(rec, 0, sizeof(rec));
            __try {
                BOOL got = pGetRecByName(cleanupNames[i], 0, rec);
                if (got) {
                    pSetSigDeleted(rec);
                    logf("  Deleted: \"%s\"\n", cleanupNames[i]);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  Cleanup crash for \"%s\": 0x%X\n", cleanupNames[i], GetExceptionCode());
            }
        }
    }
    // Also try WHType=1 (functions) for some names
    for (int i = 0; cleanupNames[i]; i++) {
        if (pGetRecByName && pSetSigDeleted) {
            char rec[268]; memset(rec, 0, sizeof(rec));
            __try {
                BOOL got = pGetRecByName(cleanupNames[i], 1, rec);
                if (got) {
                    pSetSigDeleted(rec);
                    logf("  Deleted (fn): \"%s\"\n", cleanupNames[i]);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    if (pGetNum) {
        __try { logf("  MasterRecs(0) after cleanup = %d\n", pGetNum(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 3: CreateEntryEX — use the EXACT pattern that worked in test4
    // (both arg3 and arg4 non-empty)
    logf("\n--- Step 3: CreateEntryEX ---\n");
    const char* testName = "zzz_cli_insert_test";
    const char* testSecondary = "test note";
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
        logf("\n  CreateEntryEX failed. Trying with different names...\n");
        // Try a few more names in case of name collision
        const char* altNames[] = {"zzz_test_001", "zzz_test_002", "zzz_test_003", NULL};
        for (int i = 0; altNames[i] && progId < 0; i++) {
            errCode = 0;
            __try {
                progId = pCreateEntry(0, 0, altNames[i], "note", 0, &errCode);
                logf("  CreateEntryEX(0, 0, \"%s\", \"note\", 0, &err) = %d (err=%d)\n",
                    altNames[i], (int)progId, errCode);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  CRASHED: 0x%X\n", GetExceptionCode());
            }
        }
        if (progId < 0) {
            logf("\n  All CreateEntryEX attempts failed.\n");
            if (pOrlibExit) pOrlibExit();
            FreeLibrary(g_hOrlib);
            return 7;
        }
        testName = altNames[0]; // simplified
    }

    // Step 4: FindExistPos
    logf("\n--- Step 4: FindExistPos ---\n");
    int whType = -1;
    __try {
        int found = pFindExistPos(testName, &whType);
        logf("  FindExistPos(\"%s\") = %d (whType=%d)\n", testName, found, whType);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 5: SaveModule_New with hidden EDIT
    logf("\n--- Step 5: SaveModule_New with hidden EDIT ---\n");
    const char* testSource =
        "Inputs: Price(Close), Length(10);\n"
        "Variables: Avg(0);\n"
        "Avg = Average(Price, Length);\n"
        "Plot1(Avg, \"Avg\");\n";
    wchar_t wSource[4096];
    MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
    HWND hEdit = CreateWindowW(L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit) {
        logf("  Hidden EDIT created: hwnd=0x%p, textLen=%d\n", (void*)hEdit, GetWindowTextLengthW(hEdit));
        char saveBuffer[4096]; memset(saveBuffer, 0, sizeof(saveBuffer));
        int saveResult = -1;
        __try {
            saveResult = pSaveModule(0, progId, NULL, hEdit, saveBuffer);
            logf("  SaveModule_New(0, %d, NULL, hwnd, buf) = %d\n", (int)progId, saveResult);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  CRASHED: 0x%X\n", GetExceptionCode());
        }
        DestroyWindow(hEdit);
    } else {
        logf("  FAILED to create EDIT: %lu\n", GetLastError());
    }

    // Step 6: Verify with non-zero contextVal (the key change!)
    logf("\n--- Step 6: Verify pipeline (contextVal=1) ---\n");
    if (pStartComp) {
        __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  StartComp CRASH: 0x%X\n", GetExceptionCode()); }
    }
    // Try contextVal=1 (TSDev uses this->field_0x18 which is non-zero)
    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", (int)progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  Verify CRASH (ctx=1): 0x%X\n", GetExceptionCode());
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

    // Step 7: Check persistence
    logf("\n--- Step 7: Check persistence ---\n");
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

    // Step 8: Try CloseWarehouse to flush
    logf("\n--- Step 8: CloseWarehouse flush ---\n");
    if (pCloseWh) {
        __try {
            pCloseWh(0);
            logf("  CloseWarehouseForCurrentThreadAndPath(0) called\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  CloseWarehouse CRASH: 0x%X\n", GetExceptionCode());
        }
        // Check persistence again
        f = fopen(ellPath, "rb");
        if (f) { logf("  After CloseWarehouse, FOUND: %s\n", ellPath); fclose(f); }
        else   { logf("  After CloseWarehouse, NOT FOUND: %s\n", ellPath); }
    }

    // Step 9: FindExistPos again (after verify + close)
    logf("\n--- Step 9: FindExistPos after verify ---\n");
    whType = -1;
    __try {
        int found = pFindExistPos(testName, &whType);
        logf("  FindExistPos(\"%s\") = %d (whType=%d)\n", testName, found, whType);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 10: Cleanup
    logf("\n--- Step 10: Cleanup ---\n");
    if (pGetRecByName && pSetSigDeleted) {
        char rec[268]; memset(rec, 0, sizeof(rec));
        __try {
            BOOL got = pGetRecByName(testName, 0, rec);
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

    // Verify cleanup
    f = fopen(ellPath, "rb");
    if (f) { logf("  WARNING: %s still exists!\n", ellPath); fclose(f); }
    else   { logf("  Verified: %s deleted\n", ellPath); }

    // Step 11: OrlibExit
    logf("\n--- Step 11: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test6 complete ===\n");
    logf("\nLog written to elevated_test6_log.txt\n");
    return 0;
}
