// elevated_test.cpp
//
// Minimal probe to test whether running ELEVATED resolves the empty warehouse
// problem. Tests OrlibInit + GetNumofMasterRecsEX + FindExistPos with real names.
//
// Build (from project root):
//   cmake --build build --config Release --target elevated_test
// Run (MUST be elevated — right-click cmd -> Run as administrator):
//   build\Release\elevated_test.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Check if we're elevated
    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev;
        DWORD cbSize = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cbSize)) {
            fIsElevated = elev.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    std::printf("=== elevated_test: orlib20 warehouse loading test ===\n");
    std::printf("Process is %s\n", fIsElevated ? "ELEVATED" : "NOT ELEVATED");
    std::printf("sizeof(void*) = %zu (must be 4 for 32-bit)\n", sizeof(void*));
    if (sizeof(void*) != 4) {
        std::printf("FATAL: probe is not 32-bit\n");
        return 2;
    }

    // Set DLL search path
    if (!SetDllDirectoryW(kTsProgramDir)) {
        std::printf("FATAL: SetDllDirectoryW failed, GetLastError=%lu\n", GetLastError());
        return 3;
    }
    std::printf("SetDllDirectoryW -> OK\n");

    // Load orlib20.dll
    HMODULE hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!hOrlib) {
        std::printf("FATAL: LoadLibraryW failed, GetLastError=%lu\n", GetLastError());
        return 4;
    }
    std::printf("LoadLibraryW(\"orlib20.dll\") -> OK, handle=0x%p\n", (void*)hOrlib);

    // Resolve exports
    auto pOrlibInit = (OrlibInit_t)GetProcAddress(hOrlib, "_OrlibInit@8");
    auto pOrlibExit = (OrlibExit_t)GetProcAddress(hOrlib, "_OrlibExit@0");
    auto pFindExistPos = (FindExistPos_t)GetProcAddress(hOrlib, "FindExistPos");
    auto pGetNumofMasterRecs = (GetNumofMasterRecsEX_t)GetProcAddress(hOrlib, "_GetNumofMasterRecsEX@4");

    std::printf("Resolved: OrlibInit=%p OrlibExit=%p FindExistPos=%p GetNumofMasterRecs=%p\n",
        (void*)pOrlibInit, (void*)pOrlibExit, (void*)pFindExistPos, (void*)pGetNumofMasterRecs);

    if (!pOrlibInit || !pFindExistPos) {
        std::printf("FATAL: Could not resolve required exports\n");
        FreeLibrary(hOrlib);
        return 5;
    }

    // Step 1: OrlibInit
    std::printf("\n--- Step 1: OrlibInit(NULL, 1) ---\n");
    BOOL initOk = pOrlibInit(NULL, 1);
    std::printf("  OrlibInit(NULL, 1) = %d\n", initOk);
    if (!initOk) {
        std::printf("FATAL: OrlibInit failed\n");
        FreeLibrary(hOrlib);
        return 6;
    }

    // Step 2: Check master record counts
    std::printf("\n--- Step 2: Master Record Counts ---\n");
    if (pGetNumofMasterRecs) {
        for (int wt = 0; wt <= 4; wt++) {
            __try {
                int count = pGetNumofMasterRecs(wt);
                std::printf("  GetNumofMasterRecsEX(%d) = %d\n", wt, count);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  GetNumofMasterRecsEX(%d) CRASHED: 0x%X\n", wt, GetExceptionCode());
            }
        }
    } else {
        std::printf("  GetNumofMasterRecsEX not resolved\n");
    }

    // Step 3: FindExistPos with real names
    std::printf("\n--- Step 3: FindExistPos with real names ---\n");
    const char* names[] = {
        "Average", "Close", "High", "Low", "Open", "Volume",
        "MACD", "RSI", "Stochastic", "BollingerBands",
        "MovingAverage", "ATR", "CCI", "Momentum",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        int whType = -1;
        int progId = -1;
        __try {
            progId = pFindExistPos(names[i], &whType);
            std::printf("  FindExistPos(\"%s\") = %d (whType=%d)\n", names[i], progId, whType);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  FindExistPos(\"%s\") CRASHED: 0x%X\n", names[i], GetExceptionCode());
        }
    }

    // Also test a nonexistent name (should return -1)
    {
        int whType = -1;
        int progId = pFindExistPos("zzz_nonexistent_xyz", &whType);
        std::printf("  FindExistPos(\"zzz_nonexistent_xyz\") = %d (expected: -1)\n", progId);
    }

    // Step 4: OrlibExit
    std::printf("\n--- Step 4: OrlibExit ---\n");
    if (pOrlibExit) {
        BOOL exitOk = pOrlibExit();
        std::printf("  OrlibExit() = %d\n", exitOk);
    }

    FreeLibrary(hOrlib);
    std::printf("\n=== elevated_test complete ===\n");
    std::printf("\nINTERPRETATION:\n");
    std::printf("  If GetNumofMasterRecsEX returns >0 AND FindExistPos returns >=0 for real names:\n");
    std::printf("    -> Elevation is the fix. The CLI must run elevated (like TSDev.exe).\n");
    std::printf("  If GetNumofMasterRecsEX still returns 0 or crashes:\n");
    std::printf("    -> Elevation is NOT the fix. Deeper investigation needed.\n");
    return 0;
}
