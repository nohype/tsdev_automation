// elevated_test15.cpp
//
// Test VerifyModule_New on ProgId 2565 (which TSDev created .ell/.xml for).
// If files get updated, the save path works for properly-initialized entries.
// Also scan ProgIds 2560-2570 to find all entries.

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
    g_log = fopen("elevated_test15_log.txt", "w");
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
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f);
        fclose(f);
        // Get file time
        WIN32_FILE_ATTRIBUTE_DATA fad;
        GetFileAttributesExA(ellPath, GetFileExInfoStandard, &fad);
        logf("  %s: .ell EXISTS (%ld bytes)\n", label, sz);
    } else {
        logf("  %s: .ell NOT FOUND\n", label);
    }
    f = fopen(xmlPath, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f);
        fclose(f);
        logf("  %s: .xml EXISTS (%ld bytes)\n", label, sz);
    } else {
        logf("  %s: .xml NOT FOUND\n", label);
    }
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test15: Test save on TSDev-created entry ===\n");
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

    // Step 2: Patch save-skip
    logf("\n--- Step 2: Patch save-skip ---\n");
    unsigned char origBytes[2];
    void* patchAddr = base + 0x16985A;
    memcpy(origBytes, patchAddr, 2);
    unsigned char nops[2] = { 0x90, 0x90 };
    patchBytes(patchAddr, nops, 2);
    logf("  Patched.\n");

    // Step 3: Check which ProgIds have .ell files (scan 2560-2570)
    logf("\n--- Step 3: Scan ProgIds 2560-2570 for .ell files ---\n");
    for (int pid = 2560; pid <= 2570; pid++) {
        char ellPath[512];
        sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", pid);
        FILE* f = fopen(ellPath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f);
            fclose(f);
            logf("  ProgId %d: .ell EXISTS (%ld bytes)\n", pid, sz);
        }
    }

    // Step 4: Try VerifyModule_New on ProgId 2565 (TSDev-created, has .ell)
    logf("\n--- Step 4: VerifyModule_New on ProgId 2565 (TSDev-created) ---\n");
    checkPersistence(2565, "Before verify");

    const char* testSource = "if Close > Open then Buy next bar at market;\n";

    // Save source via SaveModule_New with EDIT
    wchar_t wSource[4096];
    MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
    HWND hEdit = CreateWindowExW(0, L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit && IsWindow(hEdit)) {
        char modBuf[64]; memset(modBuf, 0, sizeof(modBuf));
        __try {
            int r = pSaveModule(0, 2565, NULL, hEdit, modBuf);
            logf("  SaveModule_New(0, 2565, ...) = %d\n", r);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  SaveModule_New CRASHED\n");
        }
        DestroyWindow(hEdit);
    }

    // Also try SaveTextEX with NULL buffer
    __try {
        int r = pSaveTextEX(testSource, 0, 2565, NULL);
        logf("  SaveTextEX(source, 0, 2565, NULL) = %d\n", r);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  SaveTextEX CRASHED\n");
    }

    // Verify
    if (pStartComp) { __try { pStartComp(); logf("  StartCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }
    __try {
        int v = pVerify(1, 0, 2565, 1, NULL);
        logf("  VerifyModule_New(1, 0, 2565, 1, NULL) = %d\n", v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }
    if (pGetErrors) {
        int e[4] = {0,0,0,0};
        __try { pGetErrors(e); logf("  GetErrors: {%d,%d,%d,%d}\n", e[0],e[1],e[2],e[3]); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pEndComp) { __try { pEndComp(); logf("  EndCompilingProcess() OK\n"); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    checkPersistence(2565, "After verify");

    // Step 5: Also try ProgId 2566
    logf("\n--- Step 5: VerifyModule_New on ProgId 2566 ---\n");
    checkPersistence(2566, "Before verify");

    hEdit = CreateWindowExW(0, L"EDIT", wSource,
        WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hEdit && IsWindow(hEdit)) {
        char modBuf[64]; memset(modBuf, 0, sizeof(modBuf));
        __try { pSaveModule(0, 2566, NULL, hEdit, modBuf); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        DestroyWindow(hEdit);
    }
    __try { pSaveTextEX(testSource, 0, 2566, NULL); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    if (pStartComp) { __try { pStartComp(); } __except(EXCEPTION_EXECUTE_HANDLER) {} }
    __try {
        int v = pVerify(1, 0, 2566, 1, NULL);
        logf("  VerifyModule_New(1, 0, 2566, 1, NULL) = %d\n", v);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CRASHED: 0x%X\n", GetExceptionCode());
    }
    if (pEndComp) { __try { pEndComp(); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    checkPersistence(2566, "After verify");

    // Step 6: Restore patch
    logf("\n--- Step 6: Restore patch ---\n");
    patchBytes(patchAddr, origBytes, 2);

    // Step 7: OrlibExit
    logf("\n--- Step 7: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test15 complete ===\n");
    return 0;
}
