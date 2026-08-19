// elevated_test16.cpp
//
// Patch VerifyModule_New to clear [ebp-0x50] before the flag check.
// Insert "mov byte ptr [ebp-0x50], 0" before the "test byte ptr [ebp-0x50], 2"
// at 0x10169856.
//
// Original: 0x10169856: f6 45 b0 02  (test byte ptr [ebp-0x50], 2)
// We patch to: c6 45 b0 00       (mov byte ptr [ebp-0x50], 0)
//              90                 (nop)
// This clears the flag before the test, forcing the save path.

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
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

bool patchBytes(void* addr, const unsigned char* newBytes, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(addr, newBytes, len);
    DWORD dummy;
    VirtualProtect(addr, len, oldProtect, &dummy);
    return true;
}

DWORD WINAPI probe_main(LPVOID);

int main() {
    g_log = fopen("elevated_test16_log.txt", "w");
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

void checkPersistence(int progId, const char* label) {
    char ellPath[512];
    sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", progId);
    char xmlPath[512];
    sprintf(xmlPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.xml", progId);
    FILE* f = fopen(ellPath, "rb");
    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); logf("  %s: .ell EXISTS (%ld bytes)\n", label, sz); fclose(f); }
    else   { logf("  %s: .ell NOT FOUND\n", label); }
    f = fopen(xmlPath, "rb");
    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); logf("  %s: .xml EXISTS (%ld bytes)\n", label, sz); fclose(f); }
    else   { logf("  %s: .xml NOT FOUND\n", label); }
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test16: Clear [ebp-0x50] flag ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    char* base = (char*)g_hOrlib;
    logf("orlib20.dll base = 0x%p\n", (void*)base);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pSaveTextEX = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");

    if (!pOrlibInit || !pFindExistPos || !pVerify) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: Patch [ebp-0x50] flag check
    // At RVA 0x169856: f6 45 b0 02 (test byte ptr [ebp-0x50], 2)
    // Replace with:    c6 45 b0 00 (mov byte ptr [ebp-0x50], 0)
    //                   90          (nop)
    logf("\n--- Step 2: Patch [ebp-0x50] flag ---\n");
    void* patchAddr = base + 0x169856;
    unsigned char origBytes[4];
    memcpy(origBytes, patchAddr, 4);
    logf("  Original bytes at 0x%p: %02X %02X %02X %02X\n",
        patchAddr, origBytes[0], origBytes[1], origBytes[2], origBytes[3]);

    // Verify it's the expected instruction
    if (origBytes[0] != 0xF6 || origBytes[1] != 0x45 || origBytes[2] != 0xB0 || origBytes[3] != 0x02) {
        logf("  WARNING: Unexpected bytes! Expected F6 45 B0 02\n");
    }

    // Patch: mov byte ptr [ebp-0x50], 0 = C6 45 B0 00, nop = 90
    unsigned char newBytes[4] = { 0xC6, 0x45, 0xB0, 0x00 };
    if (patchBytes(patchAddr, newBytes, 4)) {
        logf("  Patched to: C6 45 B0 00 (mov byte ptr [ebp-0x50], 0)\n");
        // Also need to NOP the jne that follows (at 0x16985A)
        void* jneAddr = base + 0x16985A;
        unsigned char origJne[2];
        memcpy(origJne, jneAddr, 2);
        unsigned char nops[2] = { 0x90, 0x90 };
        patchBytes(jneAddr, nops, 2);
        logf("  Also NOPed jne at 0x%p (was %02X %02X)\n", jneAddr, origJne[0], origJne[1]);
    }

    // Step 3: Find existing entry
    logf("\n--- Step 3: FindExistPos ---\n");
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

    // Step 4: SaveModule_New with EDIT
    logf("\n--- Step 4: SaveModule_New ---\n");
    const char* testSource = "if Close > Open then Buy next bar at market;\n";
    wchar_t wSource[4096];
    MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
    HWND hEdit = CreateWindowExW(0, L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit && IsWindow(hEdit)) {
        char modBuf[64]; memset(modBuf, 0, sizeof(modBuf));
        __try {
            int r = pSaveModule(0, progId, NULL, hEdit, modBuf);
            logf("  SaveModule_New = %d\n", r);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  SaveModule_New CRASHED\n");
        }
        DestroyWindow(hEdit);
    }

    // Step 5: Also try SaveTextEX with NULL buffer
    logf("\n--- Step 5: SaveTextEX ---\n");
    __try {
        int r = pSaveTextEX(testSource, 0, progId, NULL);
        logf("  SaveTextEX(source, 0, %d, NULL) = %d\n", progId, r);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  SaveTextEX CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 6: Verify pipeline (with patched flag)
    logf("\n--- Step 6: VerifyModule_New (patched) ---\n");
    if (pStartComp) { __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    __try {
        int v = pVerify(1, 0, progId, 1, NULL);
        logf("  VerifyModule_New(1, 0, %d, 1, NULL) = %d\n", progId, v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
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
    if (pEndComp) { __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    // Step 7: Check persistence
    logf("\n--- Step 7: Check persistence ---\n");
    checkPersistence(progId, "After patched verify");

    // Step 8: Restore original bytes
    logf("\n--- Step 8: Restore patches ---\n");
    patchBytes(patchAddr, origBytes, 4);
    // Note: we can't perfectly restore the jne because we overwrote it,
    // but the program is about to exit anyway
    logf("  Restored test bytes (jne not restored, but exiting)\n");

    // Step 9: OrlibExit
    logf("\n--- Step 9: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test16 complete ===\n");
    return 0;
}
