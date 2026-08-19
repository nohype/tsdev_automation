// elevated_test19.cpp
//
// Test VerifyATechnique on KNOWN EXISTING strategies to see if it works at all.
// Also test with various warehouse types.

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int  (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef bool (__stdcall *VerifyATechnique_t)(const char* name, int warehouseType);

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
    g_log = fopen("elevated_test19_log.txt", "w");
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
    logf("=== elevated_test19: VerifyATechnique on existing strategies ===\n");

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }

    auto pOrlibInit    = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit    = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pVerifyATech  = Resolve<VerifyATechnique_t>("VerifyATechnique");

    if (!pOrlibInit || !pOrlibExit || !pFindExistPos || !pVerifyATech) {
        logf("FATAL: missing exports\n"); return 5;
    }

    logf("OrlibInit...\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }

    // StartCompilingProcess (internal, by address)
    char* base = (char*)g_hOrlib;
    typedef void (__cdecl *StartCompilingProcess_t)(void);
    typedef void (__fastcall *EndCompilingProcess_t)(int param_1);
    auto pStartCompiling = (StartCompilingProcess_t)(base + 0x165de0);
    auto pEndCompiling   = (EndCompilingProcess_t)(base + 0x165ed0);

    // Test with known existing strategy names
    // These are strategies that already have .ell files
    const char* testNames[] = {
        "zzz_cli_test8",      // ProgId 2568 (created earlier)
        "zzz_cli_test7",      // ProgId 2567
        "zzz_arg3_is_name",   // ProgId 2561
        "zzz_cli_persist_test", // ProgId 2570 (just created)
        "Average",            // A function (type 4)
        "MACD",               // A function (type 4)
        NULL
    };

    for (int i = 0; testNames[i]; i++) {
        const char* name = testNames[i];
        logf("\n--- Testing \"%s\" ---\n", name);

        // Try type 0 (Strategy) first
        int progId0 = pFindExistPos(name, 0);
        logf("  FindExistPos(\"%s\", 0) = %d\n", name, progId0);

        if (progId0 > 0) {
            logf("  StartCompilingProcess...\n");
            __try { pStartCompiling(); } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  CRASHED: 0x%X\n", GetExceptionCode());
            }

            logf("  VerifyATechnique(\"%s\", 0)\n", name);
            bool result = false;
            __try {
                result = pVerifyATech(name, 0);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  CRASHED: 0x%X\n", GetExceptionCode());
            }
            logf("  VerifyATechnique = %d\n", (int)result);

            __try { pEndCompiling(0); } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  EndCompiling CRASHED: 0x%X\n", GetExceptionCode());
            }
        }

        // Try type 4 (Function) for Average/MACD
        if (strcmp(name, "Average") == 0 || strcmp(name, "MACD") == 0) {
            int progId4 = pFindExistPos(name, 4);
            logf("  FindExistPos(\"%s\", 4) = %d\n", name, progId4);

            if (progId4 > 0) {
                logf("  StartCompilingProcess...\n");
                __try { pStartCompiling(); } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  CRASHED: 0x%X\n", GetExceptionCode());
                }

                logf("  VerifyATechnique(\"%s\", 4)\n", name);
                bool result = false;
                __try {
                    result = pVerifyATech(name, 4);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  CRASHED: 0x%X\n", GetExceptionCode());
                }
                logf("  VerifyATechnique = %d\n", (int)result);

                __try { pEndCompiling(0); } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  EndCompiling CRASHED: 0x%X\n", GetExceptionCode());
                }
            }
        }
    }

    logf("\n--- OrlibExit ---\n");
    logf("OrlibExit() = %d\n", pOrlibExit());
    FreeLibrary(g_hOrlib);
    logf("\n=== complete ===\n");
    return 0;
}
