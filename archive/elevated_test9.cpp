// elevated_test9.cpp
//
// Modified to add a Sleep before verify so Frida can attach and hook internal functions.
// Also tries SaveTextEX with &buffer (pointer-to-pointer) instead of buffer.
//
// Build: cmake --build build --config Release --target elevated_test9
// Run: Can be spawned by Frida or run directly.

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
    // Log to project directory
    g_log = fopen("elevated_test9_log.txt", "w");
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
    logf("=== elevated_test9: Frida trace + SaveTextEX fix ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));

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

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry || !pVerify) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: CreateEntryEX
    logf("\n--- Step 2: CreateEntryEX ---\n");
    const char* testName = "zzz_cli_test9b";
    int errCode = 0;
    short progId = -1;
    __try {
        progId = pCreateEntry(0, 0, testName, "test9", 0, &errCode);
        logf("  CreateEntryEX(0, 0, \"%s\", \"test9\", 0, &err) = %d (err=%d)\n",
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

    // Step 3: FindExistPos with type=0
    logf("\n--- Step 3: FindExistPos ---\n");
    __try {
        int found = pFindExistPos(testName, 0);
        logf("  FindExistPos(\"%s\", 0) = %d\n", testName, found);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    // Step 4: SaveModule_New with proper EDIT
    logf("\n--- Step 4: SaveModule_New with EDIT ---\n");
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
                logf("  SaveModule_New(0, %d, NULL, hwnd, buf) = %d\n", (int)progId, r);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
            }
            VirtualFree(modBuf, 0, MEM_RELEASE);
        }
        DestroyWindow(hEdit);
    }

    // Step 5: Wait for "go" signal file to allow Frida to attach and hook internal functions
    logf("\n--- Step 5: Waiting for go signal ---\n");
    logf("  Create go_signal.txt to proceed\n");
    fflush(g_log);
    for (int i = 0; i < 120; i++) {  // wait up to 2 minutes
        FILE* go = fopen("go_signal.txt", "r");
        if (go) { fclose(go); break; }
        Sleep(1000);
    }
    // Remove the signal file
    remove("go_signal.txt");
    logf("  Go signal received. Proceeding with verify.\n");

    // Step 6: Verify pipeline
    logf("\n--- Step 6: Verify pipeline ---\n");
    if (pStartComp) {
        __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
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

    // Step 8: OrlibExit
    logf("\n--- Step 8: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test9 complete ===\n");
    return 0;
}
