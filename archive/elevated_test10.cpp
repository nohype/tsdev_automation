// elevated_test10.cpp
//
// MODIFY an existing entry instead of creating new ones.
// Uses FindExistPos to find "zzz_cli_test8" (ProgId=2568 from test8),
// saves source text, waits for Frida go signal, then verifies.
// This avoids the CreateEntryEX "no free position" issue.
//
// Build: cmake --build build --config Release --target elevated_test10
// Run: Can be spawned by Frida or run directly.

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int (__stdcall *SaveModule_New_t)(int whType, int progId,
    void* arg3, HWND hwnd, void* buffer);
typedef int (__stdcall *VerifyModule_New_t)(int contextVal, int whType,
    int progId, int isMatch, void* compileCtx);
typedef void (__stdcall *StartCompilingProcess_t)(void);
typedef void (__stdcall *EndCompilingProcess_t)(void);
typedef void (__stdcall *GetCompilingErrors_t)(void* outStruct);
typedef int (__stdcall *GetErrorLevel_t)(int handle);

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
    g_log = fopen("elevated_test10_log.txt", "w");
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
    logf("=== elevated_test10: Modify existing entry + Frida trace ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    logf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");

    if (!pOrlibInit || !pFindExistPos || !pSaveModule || !pVerify) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: Find an existing test entry
    logf("\n--- Step 2: FindExistPos ---\n");
    const char* names[] = {"zzz_cli_test8", "zzz_cli_test7", "zzz_cli_test9",
                           "zzz_cli_insert_test", "zzz_arg3_is_name", NULL};
    int progId = -1;
    const char* foundName = NULL;
    for (int i = 0; names[i]; i++) {
        __try {
            int found = pFindExistPos(names[i], 0);
            logf("  FindExistPos(\"%s\", 0) = %d\n", names[i], found);
            if (found >= 0 && progId < 0) {
                progId = found;
                foundName = names[i];
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (progId < 0) {
        logf("  No existing test entry found. Aborting.\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }
    logf("  Using: \"%s\" ProgId=%d\n", foundName, progId);

    // Step 3: SaveModule_New with proper EDIT
    logf("\n--- Step 3: SaveModule_New with EDIT ---\n");
    const char* testSource = "if Close > Open then Buy next bar at market;\n";
    wchar_t wSource[4096];
    MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
    HWND hEdit = CreateWindowExW(0, L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit && IsWindow(hEdit)) {
        logf("  EDIT: hwnd=0x%p, IsWindow=1, textLen=%d\n", (void*)hEdit, GetWindowTextLengthW(hEdit));
        char* modBuf = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
        if (modBuf) {
            __try {
                int r = pSaveModule(0, progId, NULL, hEdit, modBuf);
                logf("  SaveModule_New(0, %d, NULL, hwnd, buf) = %d\n", progId, r);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
            }
            VirtualFree(modBuf, 0, MEM_RELEASE);
        }
        DestroyWindow(hEdit);
    }

    // Step 4: Wait for Frida go signal
    logf("\n--- Step 4: Waiting for Frida go signal ---\n");
    logf("  Create go_signal.txt to proceed\n");
    fflush(g_log);
    for (int i = 0; i < 120; i++) {
        FILE* go = fopen("go_signal.txt", "r");
        if (go) { fclose(go); break; }
        Sleep(1000);
    }
    remove("go_signal.txt");
    logf("  Go signal received. Proceeding with verify.\n");

    // Step 5: Verify pipeline
    logf("\n--- Step 5: Verify pipeline ---\n");
    if (pStartComp) {
        __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  Verify CRASH: 0x%X\n", GetExceptionCode());
    }
    if (pGetErrors) {
        int e[4] = {0,0,0,0};
        __try { pGetErrors(e); logf("  GetErrors: {%d,%d,%d,%d}\n", e[0],e[1],e[2],e[3]); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pGetErrLevel) {
        __try { logf("  GetErrorLevel(0) = %d\n", pGetErrLevel(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pEndComp) {
        __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 6: Check persistence
    logf("\n--- Step 6: Check persistence ---\n");
    char ellPath[512];
    sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", progId);
    char xmlPath[512];
    sprintf(xmlPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.xml", progId);
    FILE* f = fopen(ellPath, "rb");
    if (f) { logf("  FOUND: %s\n", ellPath); fclose(f); }
    else   { logf("  NOT FOUND: %s\n", ellPath); }
    f = fopen(xmlPath, "rb");
    if (f) { logf("  FOUND: %s\n", xmlPath); fclose(f); }
    else   { logf("  NOT FOUND: %s\n", xmlPath); }

    // Step 7: OrlibExit
    logf("\n--- Step 7: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test10 complete ===\n");
    return 0;
}
