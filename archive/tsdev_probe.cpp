// tsdev_probe.cpp
//
// Headless smoke-test probe for orlib20.dll.
//
// Loads the 32-bit orlib20.dll from the TradeStation 9.5 install, calls
// _OrlibInit@8(NULL, 1) (TSDev.exe's exact call) then _OrlibExit@0(), and
// reports success/failure WITHOUT requiring the TradeStation Desktop to be
// running. This is the CRITICAL GATE (TVS-2) for the orlib20-direct approach.
//
// Build (from project root):
//   cmake -S . -B build -A Win32
//   cmake --build build --config Release
// Run:
//   build\Release\tsdev_probe.exe

#include <cstdio>
#include <cstdlib>
#include <windows.h>

// --- orlib20 function pointer types -----------------------------------------
// From FEASIBILITY.md §5.1 and tmp/re_artifacts/orlib_init_args.md:
//   Init     : BOOL __stdcall _OrlibInit@8(const char* workDirPath, int contextId)
//   Teardown : BOOL __stdcall _OrlibExit@0(void)   (returns BOOL, HIGH confidence)
typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";

int main() {
    std::printf("=== tsdev_probe: orlib20.dll headless init smoke test ===\n");
    std::printf("sizeof(void*) = %zu (must be 4 for 32-bit)\n", sizeof(void*));
    if (sizeof(void*) != 4) {
        std::printf("GATE-FAILED: probe is not 32-bit; cannot load 32-bit DLL\n");
        return 2;
    }

    // 1. Set DLL search path so orlib20.dll's sibling dependencies resolve.
    //    orlib20.dll imports from TsShare.dll, tslib.dll, orshr20.dll, ELRte.dll,
    //    ELMDCore.dll, ECL.dll, tskit.dll, tsdmgr.dll, mfc140u.dll, etc., all in
    //    the same Program directory.
    if (!SetDllDirectoryW(kTsProgramDir)) {
        std::printf("GATE-FAILED: SetDllDirectoryW failed, GetLastError=%lu\n",
                    GetLastError());
        return 3;
    }
    std::printf("SetDllDirectoryW -> OK (search dir set to TS Program dir)\n");

    // 2. Load orlib20.dll.
    HMODULE hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!hOrlib) {
        DWORD err = GetLastError();
        std::printf("GATE-FAILED: LoadLibraryW(\"orlib20.dll\") failed, "
                    "GetLastError=%lu (0x%lX)\n", err, err);
        return 4;
    }
    std::printf("LoadLibraryW(\"orlib20.dll\") -> OK, handle=0x%p\n",
                (void*)hOrlib);

    // 3. Resolve _OrlibInit (decorated name first, undecorated fallback).
    OrlibInit_t pOrlibInit = (OrlibInit_t)GetProcAddress(hOrlib, "_OrlibInit@8");
    const char* initName = "_OrlibInit@8";
    if (!pOrlibInit) {
        pOrlibInit = (OrlibInit_t)GetProcAddress(hOrlib, "OrlibInit");
        initName = "OrlibInit";
    }
    if (!pOrlibInit) {
        std::printf("GATE-FAILED: could not resolve OrlibInit export "
                    "(tried _OrlibInit@8 and OrlibInit), GetLastError=%lu\n",
                    GetLastError());
        FreeLibrary(hOrlib);
        return 5;
    }
    std::printf("Resolved OrlibInit via export name \"%s\" -> 0x%p\n",
                initName, (void*)pOrlibInit);

    // 4. Resolve _OrlibExit (decorated name first, undecorated fallback).
    OrlibExit_t pOrlibExit = (OrlibExit_t)GetProcAddress(hOrlib, "_OrlibExit@0");
    const char* exitName = "_OrlibExit@0";
    if (!pOrlibExit) {
        pOrlibExit = (OrlibExit_t)GetProcAddress(hOrlib, "OrlibExit");
        exitName = "OrlibExit";
    }
    if (!pOrlibExit) {
        std::printf("GATE-FAILED: could not resolve OrlibExit export "
                    "(tried _OrlibExit@0 and OrlibExit), GetLastError=%lu\n",
                    GetLastError());
        FreeLibrary(hOrlib);
        return 6;
    }
    std::printf("Resolved OrlibExit via export name \"%s\" -> 0x%p\n",
                exitName, (void*)pOrlibExit);

    // 5. Call _OrlibInit(NULL, 1) — TSDev.exe's exact call.
    //    workDirPath = NULL (sentinel; orlib20 derives MyWork internally).
    //    contextId   = 1    (TSDev uses 1; ELIS uses 0).
    SetLastError(0);
    std::printf("Calling _OrlibInit(NULL, 1)...\n");
    BOOL initOk = pOrlibInit(NULL, 1);
    DWORD initErr = GetLastError();
    std::printf("  _OrlibInit(NULL, 1) returned %d (0x%X), GetLastError=%lu\n",
                initOk, (unsigned)initOk, initErr);

    if (!initOk) {
        std::printf("GATE-FAILED: _OrlibInit(NULL, 1) returned FALSE (0) "
                    "headlessly. GetLastError=%lu. The orlib20-direct approach "
                    "is likely BLOCKED without the TradeStation Desktop.\n",
                    initErr);
        FreeLibrary(hOrlib);
        return 7;
    }

    // 6. Init succeeded — call _OrlibExit() to tear down cleanly.
    SetLastError(0);
    std::printf("Calling _OrlibExit()...\n");
    BOOL exitRv = pOrlibExit();
    DWORD exitErr = GetLastError();
    std::printf("  _OrlibExit() returned %d (0x%X), GetLastError=%lu\n",
                exitRv, (unsigned)exitRv, exitErr);

    FreeLibrary(hOrlib);

    // 7. Summary.
    std::printf("=== GATE-PASSED: orlib20.dll loaded, _OrlibInit(NULL,1) "
                "returned TRUE, _OrlibExit() returned %d headlessly. "
                "The orlib20-direct approach is viable without the "
                "TradeStation Desktop. ===\n", exitRv);
    return 0;
}
