// elevated_test12.cpp
//
// Diagnostic: Read orlib20 global variables before/after VerifyModule_New
// to determine which code path is taken (early exit vs full compile+save).
//
// Key globals:
//   0x1023AAB0: "compiling" flag — set to 1 if compile path is taken
//   0x1023AABC: contextVal — set to arg1 if compile path is taken
//   0x1023AAC4: error count 1
//   0x1023AAC8: error count 2
//
// If these globals change, the compile path was taken.
// If they don't change, we're hitting an early exit.

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

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
static HMODULE g_hOrlib = NULL;
static FILE* g_log = NULL;
static void* g_orlibBase = NULL;

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

// Read global variables from orlib20.dll
struct Globals {
    DWORD compiling;    // [base + 0x23AAB0]
    DWORD contextVal;   // [base + 0x23AABC]
    DWORD errCount1;    // [base + 0x23AAC4]
    DWORD errCount2;    // [base + 0x23AAC8]
};

Globals readGlobals() {
    Globals g = {0};
    char* base = (char*)g_orlibBase;
    __try {
        g.compiling = *(DWORD*)(base + 0x23AAB0);
        g.contextVal = *(DWORD*)(base + 0x23AABC);
        g.errCount1 = *(DWORD*)(base + 0x23AAC4);
        g.errCount2 = *(DWORD*)(base + 0x23AAC8);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return g;
}

DWORD WINAPI probe_main(LPVOID);

int main() {
    g_log = fopen("elevated_test12_log.txt", "w");
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
    logf("=== elevated_test12: VerifyModule_New path diagnostic ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    g_orlibBase = (void*)g_hOrlib;
    logf("orlib20.dll base = 0x%p\n", g_orlibBase);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");

    if (!pOrlibInit || !pFindExistPos || !pVerify) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Read globals before anything
    Globals g0 = readGlobals();
    logf("\n--- Globals after init ---\n");
    logf("  compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        g0.compiling, g0.contextVal, g0.errCount1, g0.errCount2);

    // Step 2: Find existing entry
    logf("\n--- Step 2: FindExistPos ---\n");
    const char* names[] = {"zzz_cli_test8", "zzz_cli_test7", "zzz_cli_test9",
                           "zzz_cli_insert_test", "zzz_arg3_is_name", NULL};
    int progId = -1;
    for (int i = 0; names[i]; i++) {
        __try {
            int found = pFindExistPos(names[i], 0);
            logf("  FindExistPos(\"%s\", 0) = %d\n", names[i], found);
            if (found >= 0 && progId < 0) progId = found;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (progId < 0) {
        logf("  No existing test entry found. Aborting.\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }
    logf("  Using ProgId=%d\n", progId);

    // Step 3: SaveModule_New with EDIT
    logf("\n--- Step 3: SaveModule_New ---\n");
    const char* testSource = "if Close > Open then Buy next bar at market;\n";
    wchar_t wSource[4096];
    MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
    HWND hEdit = CreateWindowExW(0, L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit && IsWindow(hEdit)) {
        char* modBuf = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
        __try {
            int r = pSaveModule(0, progId, NULL, hEdit, modBuf);
            logf("  SaveModule_New = %d\n", r);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  SaveModule_New CRASHED\n");
        }
        VirtualFree(modBuf, 0, MEM_RELEASE);
        DestroyWindow(hEdit);
    }

    // Read globals after SaveModule_New
    Globals g1 = readGlobals();
    logf("  Globals after SaveModule: compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        g1.compiling, g1.contextVal, g1.errCount1, g1.errCount2);

    // Step 4: Verify WITHOUT StartCompilingProcess
    logf("\n--- Step 4: VerifyModule_New WITHOUT StartCompilingProcess ---\n");
    Globals gBefore = readGlobals();
    logf("  Before: compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        gBefore.compiling, gBefore.contextVal, gBefore.errCount1, gBefore.errCount2);

    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    Globals gAfter = readGlobals();
    logf("  After: compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        gAfter.compiling, gAfter.contextVal, gAfter.errCount1, gAfter.errCount2);

    bool compilePathTaken = (gAfter.compiling != gBefore.compiling) ||
                            (gAfter.contextVal != gBefore.contextVal);
    logf("  >>> Compile path taken: %s\n", compilePathTaken ? "YES" : "NO (early exit)");
    if (!compilePathTaken) {
        logf("  >>> EARLY EXIT: VerifyModule_New returned 1 without compiling!\n");
        logf("  >>> This means one of the check functions (0x166E70 or 0x166060) returned non-zero.\n");
    }

    // Step 5: Verify WITH StartCompilingProcess
    logf("\n--- Step 5: VerifyModule_New WITH StartCompilingProcess ---\n");
    if (pStartComp) { __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    gBefore = readGlobals();
    logf("  Before: compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        gBefore.compiling, gBefore.contextVal, gBefore.errCount1, gBefore.errCount2);

    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    gAfter = readGlobals();
    logf("  After: compiling=%d contextVal=%d errCount1=%d errCount2=%d\n",
        gAfter.compiling, gAfter.contextVal, gAfter.errCount1, gAfter.errCount2);

    compilePathTaken = (gAfter.compiling != gBefore.compiling) ||
                       (gAfter.contextVal != gBefore.contextVal);
    logf("  >>> Compile path taken: %s\n", compilePathTaken ? "YES" : "NO (early exit)");

    if (pEndComp) { __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

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
    logf("\n=== elevated_test12 complete ===\n");
    return 0;
}
