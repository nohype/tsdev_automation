// elevated_test8.cpp
//
// Key fixes:
// 1. Test FindExistPos with ALL types (0-4) for the new entry
// 2. Fix SaveTextEX — try larger buffer, try calling via SaveModule_New with a REAL window
// 3. Try GetMasterRecByName with type=0 (strategies) not type=4 (functions)
// 4. Map warehouse types to component types
//
// Build: cmake --build build --config Release --target elevated_test8
// Run (MUST be elevated): build\Release\elevated_test8.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
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
    g_log = fopen("elevated_test8_log.txt", "w");
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
    logf("=== elevated_test8: Type mapping + SaveTextEX fix ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { logf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    logf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
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

    // Step 2: Map warehouse types — get counts for each type
    logf("\n--- Step 2: Master record counts by type ---\n");
    for (int t = 0; t <= 4; t++) {
        __try {
            int n = pGetNum(t);
            logf("  MasterRecs(%d) = %d\n", t, n);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  MasterRecs(%d) CRASHED\n", t);
        }
    }

    // Step 3: Test FindExistPos with known names across ALL types
    logf("\n--- Step 3: FindExistPos type mapping ---\n");
    const char* knownNames[] = {"Average", "MACD", "Close", "High", "Low", "Open", "Volume", NULL};
    for (int i = 0; knownNames[i]; i++) {
        for (int t = 0; t <= 4; t++) {
            __try {
                int found = pFindExistPos(knownNames[i], t);
                if (found >= 0) {
                    logf("  FindExistPos(\"%s\", %d) = %d  *** FOUND ***\n", knownNames[i], t, found);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // Step 4: CreateEntryEX with WHType=0 (strategy)
    logf("\n--- Step 4: CreateEntryEX(WHType=0) ---\n");
    const char* testName = "zzz_cli_test8";
    int errCode = 0;
    short progId = -1;
    __try {
        progId = pCreateEntry(0, 0, testName, "test8 note", 0, &errCode);
        logf("  CreateEntryEX(0, 0, \"%s\", \"test8 note\", 0, &err) = %d (err=%d)\n",
            testName, (int)progId, errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    if (progId < 0) {
        logf("  CreateEntryEX failed. Aborting.\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }

    // Step 5: FindExistPos with ALL types for the new entry
    logf("\n--- Step 5: FindExistPos for new entry (all types) ---\n");
    for (int t = 0; t <= 4; t++) {
        __try {
            int found = pFindExistPos(testName, t);
            logf("  FindExistPos(\"%s\", %d) = %d %s\n", testName, t, found,
                found >= 0 ? "*** FOUND ***" : "");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  FindExistPos(\"%s\", %d) CRASHED\n", testName, t);
        }
    }

    // Step 6: GetMasterRecByName with ALL types
    logf("\n--- Step 6: GetMasterRecByName for new entry (all types) ---\n");
    for (int t = 0; t <= 4; t++) {
        char rec[268]; memset(rec, 0, sizeof(rec));
        __try {
            BOOL got = pGetRecByName(testName, t, rec);
            logf("  GetMasterRecByName(\"%s\", %d) = %d %s\n", testName, t, got,
                got ? "*** FOUND ***" : "");
            if (got) {
                // Dump first 32 bytes of the record
                logf("    rec[0..3] = %02x %02x %02x %02x\n",
                    (unsigned char)rec[0], (unsigned char)rec[1],
                    (unsigned char)rec[2], (unsigned char)rec[3]);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  GetMasterRecByName(\"%s\", %d) CRASHED: 0x%X\n", testName, t, GetExceptionCode());
        }
    }

    // Step 7: SaveTextEX — try with a much larger buffer
    logf("\n--- Step 7: SaveTextEX ---\n");
    const char* testSource =
        "if Close > Open then Buy next bar at market;\n";
    // Try with a large aligned buffer
    char* saveBuffer = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
    if (saveBuffer) {
        logf("  Allocated 64KB buffer at 0x%p\n", saveBuffer);
        __try {
            int r = pSaveTextEX(testSource, 0, progId, saveBuffer);
            logf("  SaveTextEX(source, 0, %d, buf) = %d\n", (int)progId, r);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  SaveTextEX CRASHED: 0x%X\n", GetExceptionCode());
        }
        VirtualFree(saveBuffer, 0, MEM_RELEASE);
    } else {
        logf("  VirtualAlloc failed\n");
    }

    // Step 7b: Also try SaveModule_New with a PROPER hidden EDIT window
    logf("\n--- Step 7b: SaveModule_New with proper EDIT ---\n");
    {
        // Create a proper window with a message loop pump
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"HiddenEditParent";
        RegisterClassW(&wc);

        // Create EDIT as a child of a hidden parent window
        wchar_t wSource[4096];
        MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
        HWND hEdit = CreateWindowExW(0, L"EDIT", wSource,
            WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);

        if (hEdit) {
            // Verify the window is valid
            BOOL isValid = IsWindow(hEdit);
            int textLen = GetWindowTextLengthW(hEdit);
            logf("  EDIT created: hwnd=0x%p, IsWindow=%d, textLen=%d\n",
                (void*)hEdit, isValid, textLen);

            if (isValid) {
                char* modBuf = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
                if (modBuf) {
                    __try {
                        int r = pSaveModule(0, progId, NULL, hEdit, modBuf);
                        logf("  SaveModule_New(0, %d, NULL, hwnd, buf) = %d\n", (int)progId, r);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
                    }
                    VirtualFree(modBuf, 0, MEM_RELEASE);
                }
            }
            DestroyWindow(hEdit);
        } else {
            logf("  CreateWindowExW failed: %lu\n", GetLastError());
        }
    }

    // Step 8: Verify pipeline
    logf("\n--- Step 8: Verify pipeline ---\n");
    if (pStartComp) {
        __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { logf("  StartComp CRASH\n"); }
    }
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
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pEndComp) {
        __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 9: Check persistence
    logf("\n--- Step 9: Check persistence ---\n");
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

    // Step 10: FindExistPos after verify (all types)
    logf("\n--- Step 10: FindExistPos after verify (all types) ---\n");
    for (int t = 0; t <= 4; t++) {
        __try {
            int found = pFindExistPos(testName, t);
            logf("  FindExistPos(\"%s\", %d) = %d %s\n", testName, t, found,
                found >= 0 ? "*** FOUND ***" : "");
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 11: Cleanup
    logf("\n--- Step 11: Cleanup ---\n");
    // Try all types for cleanup
    for (int t = 0; t <= 4; t++) {
        if (pGetRecByName && pSetSigDeleted) {
            char rec[268]; memset(rec, 0, sizeof(rec));
            __try {
                BOOL got = pGetRecByName(testName, t, rec);
                if (got) {
                    pSetSigDeleted(rec);
                    logf("  Deleted (type=%d): \"%s\"\n", t, testName);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  Cleanup CRASH (type=%d): 0x%X\n", t, GetExceptionCode());
            }
        }
    }

    // Step 12: OrlibExit
    logf("\n--- Step 12: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test8 complete ===\n");
    return 0;
}
