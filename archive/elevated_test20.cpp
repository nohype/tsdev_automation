// elevated_test20.cpp
//
// THE DEFINITIVE PERSISTENCE TEST
//
// Based on Frida live tracing of TSDev's actual verify flow:
//
// 1. SaveModule_New(whType, progId, stackPtr, hwnd, stackPtr)
//    → FUN_101a07f0 (warehouse state)
//    → ret=1
// 2. StartCompilingProcess()
// 3. VerifyModule_New(hwnd, whType, progId, flag=1, CELCompilerObj)
//    → FUN_101a07f0 (warehouse state)
//    → FUN_101087d0 (source dispatch) → source text
//    → CompileText → ret=1
//    → ret=1
// 4. EndCompilingProcess(0)
//
// Key findings from the trace:
//   - VerifyModule_New param_1 = HWND (the editor window), NOT 0 or 1
//   - VerifyModule_New param_4 = 1 (a flag)
//   - VerifyModule_New param_5 = CELCompiler object (88 bytes, NOT NULL)
//   - The CELCompiler object is created by FUN_10165a00 (constructor)
//   - SaveModule_New arg3 and arg5 are stack pointers (local vars)
//
// CELCompiler object layout (88 bytes = 0x58):
//   +0x00: 0
//   +0x10: 0xa02 (flags)
//   +0x18: HWND
//   +0x28: 6
//   +0x30: 7
//   +0x34: 0x73bc0000
//   +0x48: 7
//   +0x4c: pointer (from FUN_10118d20)
//   +0x54: 1

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int  (__stdcall *CreateEntryEX_t)(int whType, int secondary, const char* name,
                                           int param4, char param5, int* errCode);
typedef int  (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int  (__stdcall *SaveModule_New_t)(int warehouseType, int progId,
                                            void* arg3, HWND editorHwnd, void* arg5);
// CORRECTED: VerifyModule_New(hwnd, whType, progId, flag, compilerObj)
typedef int  (__stdcall *VerifyModule_New_t)(HWND hwnd, int warehouseType,
                                              int progId, int flag, void* compilerObj);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
static const char* kMyWorkBin =
    "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";
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
    g_log = fopen("elevated_test20_log.txt", "w");
    if (!g_log) { printf("FATAL: can't open log file\n"); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);

    // Pre-load orlib20.dll so Frida can hook it
    SetDllDirectoryW(L"C:\\Program Files (x86)\\TradeStation 9.5\\Program");
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    logf("PID = %d, orlib20.dll = 0x%p - waiting 5 seconds for Frida...\n",
        GetCurrentProcessId(), (void*)g_hOrlib);
    fflush(stdout);
    Sleep(5000);

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

bool fileExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES);
}

void checkFiles(int progId) {
    char ellPath[512], xmlPath[512];
    sprintf(ellPath, "%s\\elmodule_0_%d.ell", kMyWorkBin, progId);
    sprintf(xmlPath, "%s\\elmodule_0_%d.xml", kMyWorkBin, progId);
    logf("  %s: %s\n", ellPath, fileExists(ellPath) ? "EXISTS" : "MISSING");
    if (fileExists(ellPath)) {
        HANDLE h = CreateFileA(ellPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            logf("    size = %lu bytes\n", GetFileSize(h, NULL));
            CloseHandle(h);
        }
    }
    logf("  %s: %s\n", xmlPath, fileExists(xmlPath) ? "EXISTS" : "MISSING");
    if (fileExists(xmlPath)) {
        HANDLE h = CreateFileA(xmlPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            logf("    size = %lu bytes\n", GetFileSize(h, NULL));
            CloseHandle(h);
        }
    }
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test20: DEFINITIVE PERSISTENCE TEST ===\n");
    logf("Replicating TSDev's exact verify flow from Frida trace\n\n");

    SetDllDirectoryW(kTsProgramDir);
    if (!g_hOrlib) g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    char* base = (char*)g_hOrlib;
    logf("orlib20.dll base = 0x%p\n", (void*)g_hOrlib);

    // Resolve exports
    auto pOrlibInit      = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit      = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pCreateEntry    = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pFindExistPos   = Resolve<FindExistPos_t>("FindExistPos");
    auto pSaveModuleNew  = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerifyModule   = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");

    if (!pOrlibInit || !pOrlibExit || !pCreateEntry || !pFindExistPos ||
        !pSaveModuleNew || !pVerifyModule) {
        logf("FATAL: missing critical exports\n");
        return 5;
    }

    // Internal functions by address
    // StartCompilingProcess (cdecl)
    typedef void (__cdecl *StartCompilingProcess_t)(void);
    auto pStartCompiling = (StartCompilingProcess_t)(base + 0x165de0);
    // EndCompilingProcess (fastcall, 1 arg)
    typedef void (__fastcall *EndCompilingProcess_t)(int param_1);
    auto pEndCompiling = (EndCompilingProcess_t)(base + 0x165ed0);
    // FUN_10165a00 - CELCompiler constructor (fastcall: ecx=this)
    // Returns the object pointer
    typedef int (__fastcall *CELCompilerCtor_t)(int thisPtr);
    auto pCELCompilerCtor = (CELCompilerCtor_t)(base + 0x165a00);
    // FUN_10165ac0 - CELCompiler destructor (fastcall: ecx=this)
    typedef void (__fastcall *CELCompilerDtor_t)(int thisPtr);
    auto pCELCompilerDtor = (CELCompilerDtor_t)(base + 0x165ac0);
    // FUN_10118d20 - called by constructor to init +0x4c
    // We don't call this directly; the constructor handles it

    logf("All exports resolved.\n");

    // CRITICAL PATCH: Bypass FUN_10011230 (SymWriter path) in orlib20.dll
    //
    // Root cause: SymWriter::Save in ELDebugger.dll calls XmlDocument.Save (a .NET
    // managed method) which crashes because the CLR is only partially initialized
    // in our standalone native process. FUN_10011230 throws an exception, which
    // causes CompileText's SEH handler to skip ELMeta::MDVault::SaveModule.
    //
    // Fix: Patch FUN_10011230 to return 1 immediately. This makes CompileText
    // think the SymWriter step succeeded, so it proceeds to call
    // ELMeta::MDVault::SaveModule, which writes the .ell file (compiled module).
    // The .xml file (debug symbols) is skipped, but it's optional — 7 existing
    // .ell files in MyWork\Bin already work without .xml files.
    //
    // FUN_10011230 is __fastcall with 5 params (ecx, edx, 3 stack args = 12 bytes)
    // Patch: MOV EAX, 1; RET 0x0C  (8 bytes)
    {
        void* patchAddr = (void*)(base + 0x11230);
        DWORD oldProtect = 0;
        if (VirtualProtect(patchAddr, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // MOV EAX, 1 (5 bytes) + RET 0x0C (3 bytes) = 8 bytes
            unsigned char patch[] = {0xB8, 0x01, 0x00, 0x00, 0x00,  // MOV EAX, 1
                                     0xC2, 0x0C, 0x00};              // RET 0x0C
            memcpy(patchAddr, patch, sizeof(patch));
            DWORD dummy = 0;
            VirtualProtect(patchAddr, 16, oldProtect, &dummy);
            logf("  Patched FUN_10011230 at 0x%p (SymWriter bypass)\n", patchAddr);
        } else {
            logf("  WARNING: VirtualProtect failed for FUN_10011230 patch\n");
        }
    }

    // Pre-load .NET support DLLs that TSDev loads during SaveModule_New
    // These were observed via Frida tracing of TSDev's verify flow
    logf("\n--- Pre-loading .NET support DLLs ---\n");
    const wchar_t* supportDlls[] = {
        L"ECL.dll",
        L"ORELUI20.dll",
        L"orUI20.dll",
        L"elobjmod.dll",
        L"elwinforms.dll",
        L"tschartxelobjmdl.dll",
        L"ELDebugger.dll",
    };
    for (int i = 0; i < 7; i++) {
        HMODULE h = LoadLibraryW(supportDlls[i]);
        logf("  LoadLibraryW(%ls) = 0x%p\n", supportDlls[i], (void*)h);
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: Create a new Strategy entry
    const char* strategyName = "zzz_cli_definitive";
    logf("\n--- Step 2: CreateEntryEX ---\n");
    int progId = pFindExistPos(strategyName, 0);
    logf("  Pre-check: FindExistPos(\"%s\", 0) = %d\n", strategyName, progId);

    if (progId <= 0) {
        int errCode = 0;
        int createResult = -1;
        __try {
            createResult = pCreateEntry(0, 0, strategyName, 0, 0, &errCode);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  CreateEntryEX CRASHED: 0x%X\n", GetExceptionCode());
        }
        logf("  CreateEntryEX = %d, errCode = 0x%X\n", createResult, errCode);
        progId = pFindExistPos(strategyName, 0);
        logf("  FindExistPos = %d\n", progId);
    }

    if (progId <= 0) {
        logf("FATAL: Could not get a valid ProgId\n");
        pOrlibExit();
        return 7;
    }
    logf("  Using ProgId = %d\n", progId);

    logf("\n  Files BEFORE:\n");
    checkFiles(progId);

    // Step 3: Create hidden EDIT control with source text
    logf("\n--- Step 3: Create hidden EDIT control ---\n");
    HWND hParent = CreateWindowW(L"STATIC", L"", WS_POPUP,
                                 0, 0, 100, 100, HWND_MESSAGE, NULL, NULL, NULL);
    if (!hParent) hParent = NULL;
    HWND hEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
                               0, 0, 100, 100, hParent, NULL, NULL, NULL);
    if (!hEdit) {
        hEdit = CreateWindowW(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL,
                              0, 0, 100, 100, NULL, NULL, NULL, NULL);
    }
    if (!hEdit) {
        logf("FATAL: CreateWindowW(EDIT) failed: %lu\n", GetLastError());
        pOrlibExit();
        return 8;
    }
    logf("  EDIT HWND = 0x%p, IsWindow = %d\n", (void*)hEdit, IsWindow(hEdit));

    const char* sourceText =
        "{ zzz_cli_definitive } \r\n"
        "Buy (\"EnterLong\") next bar at Market; \r\n";

    int sourceLen = strlen(sourceText);
    wchar_t* wideText = new wchar_t[sourceLen + 1];
    MultiByteToWideChar(CP_ACP, 0, sourceText, -1, wideText, sourceLen + 1);
    SetWindowTextW(hEdit, wideText);
    delete[] wideText;
    logf("  SetWindowTextW done, length = %d\n", GetWindowTextLengthW(hEdit));

    // Step 4: SaveModule_New (saves source + clears 0xd8 bit)
    logf("\n--- Step 4: SaveModule_New ---\n");
    // From trace: arg3 and arg5 are stack pointers (local vars)
    // We'll pass NULL for arg3 and arg5 (they seem to be optional)
    int saveResult = 0;
    __try {
        saveResult = pSaveModuleNew(0, progId, NULL, hEdit, NULL);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
    }
    logf("  SaveModule_New(0, %d, NULL, hwnd, NULL) = %d\n", progId, saveResult);

    logf("\n  Files AFTER SaveModule_New:\n");
    checkFiles(progId);

    // Step 5: Create CELCompiler object (88 bytes = 0x58)
    logf("\n--- Step 5: Create CELCompiler object ---\n");
    // Allocate 88 bytes for the CELCompiler object
    void* compilerMem = malloc(0x58);
    if (!compilerMem) {
        logf("FATAL: malloc(0x58) failed\n");
        pOrlibExit();
        return 9;
    }
    memset(compilerMem, 0, 0x58);
    logf("  Allocated 88 bytes at 0x%p\n", compilerMem);

    // Call the constructor (fastcall: ecx=this)
    int ctorResult = 0;
    __try {
        ctorResult = pCELCompilerCtor((int)compilerMem);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CELCompilerCtor CRASHED: 0x%X\n", GetExceptionCode());
    }
    logf("  CELCompilerCtor = 0x%X\n", ctorResult);

    // Dump the object
    logf("  Object dump:\n");
    unsigned char* p = (unsigned char*)compilerMem;
    for (int i = 0; i < 0x58; i += 4) {
        int val = *(int*)(p + i);
        logf("    +0x%02X: 0x%08X\n", i, val);
    }

    // Set fields to match TSDev's CELCompiler object from Frida trace
    // TSDev had: +0x10: 0xa02, +0x18: HWND, +0x28: 6, +0x54: 1
    *(int*)((char*)compilerMem + 0x10) = 0xa02;  // flags
    *(HWND*)((char*)compilerMem + 0x18) = hEdit;  // editor HWND
    *(int*)((char*)compilerMem + 0x28) = 6;       // unknown but TSDev has it
    logf("  Set +0x10=0xa02, +0x18=hwnd, +0x28=6\n");

    // Step 6: StartCompilingProcess
    logf("\n--- Step 6: StartCompilingProcess ---\n");
    __try {
        pStartCompiling();
        logf("  StartCompilingProcess done\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  StartCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 7: VerifyModule_New with CORRECT arguments
    // From trace: VerifyModule_New(hwnd, whType, progId, flag=1, compilerObj)
    logf("\n--- Step 7: VerifyModule_New (CORRECT args) ---\n");
    logf("  VerifyModule_New(hwnd=0x%p, 0, %d, 1, compilerObj=0x%p)\n",
        (void*)hEdit, progId, compilerMem);
    int verifyResult = 0;
    __try {
        verifyResult = pVerifyModule(hEdit, 0, progId, 1, compilerMem);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  VerifyModule_New CRASHED: 0x%X\n", GetExceptionCode());
    }
    logf("  VerifyModule_New = %d\n", verifyResult);

    // Check for compilation errors
    logf("\n--- Step 7b: GetCompilingErrors ---\n");
    typedef void (__stdcall *GetCompilingErrors_t)(void* buffer);
    auto pGetErrors = (GetCompilingErrors_t)GetProcAddress(g_hOrlib, "_GetCompilingErrors@4");
    if (pGetErrors) {
        // GetCompilingErrors takes a pointer to a buffer/struct
        // From the decompilation, it reads from global error state and fills a buffer
        // Let's try with a large buffer
        char errorBuf[4096] = {0};
        __try {
            pGetErrors(errorBuf);
            logf("  GetCompilingErrors done\n");
            // Try to read the buffer as a string or structure
            logf("  errorBuf first 200 bytes: %.200s\n", errorBuf);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  GetCompilingErrors CRASHED: 0x%X\n", GetExceptionCode());
        }
    } else {
        logf("  GetCompilingErrors not found\n");
    }

    // Also check GetErrorLevel if it exists
    typedef int (__stdcall *GetErrorLevel_t)(void);
    auto pGetErrorLevel = (GetErrorLevel_t)GetProcAddress(g_hOrlib, "_GetErrorLevel@0");
    if (pGetErrorLevel) {
        __try {
            int level = pGetErrorLevel();
            logf("  GetErrorLevel = %d\n", level);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  GetErrorLevel CRASHED: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 8: EndCompilingProcess
    logf("\n--- Step 8: EndCompilingProcess ---\n");
    __try {
        pEndCompiling(0);
        logf("  EndCompilingProcess done\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  EndCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 8b: CloseWarehouseForCurrentThreadAndPath - might flush to disk
    logf("\n--- Step 8b: CloseWarehouseForCurrentThreadAndPath ---\n");
    typedef void (__stdcall *CloseWarehouse_t)(int param);
    auto pCloseWh = (CloseWarehouse_t)GetProcAddress(g_hOrlib, "_CloseWarehouseForCurrentThreadAndPath@4");
    if (pCloseWh) {
        logf("  Calling CloseWarehouseForCurrentThreadAndPath(0)...\n");
        __try {
            pCloseWh(0);
            logf("  CloseWarehouse done\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  CloseWarehouse CRASHED: 0x%X\n", GetExceptionCode());
        }
    } else {
        logf("  CloseWarehouseForCurrentThreadAndPath not found\n");
    }

    // Step 9: Check files
    logf("\n--- Step 9: Check files ---\n");
    checkFiles(progId);

    // Step 10: Cleanup
    logf("\n--- Step 10: Cleanup ---\n");
    // Destroy CELCompiler object
    __try {
        pCELCompilerDtor((int)compilerMem);
        logf("  CELCompilerDtor done\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  CELCompilerDtor CRASHED: 0x%X\n", GetExceptionCode());
    }
    free(compilerMem);
    DestroyWindow(hEdit);
    if (hParent) DestroyWindow(hParent);
    logf("  Cleanup done\n");

    // OrlibExit
    logf("\n--- OrlibExit ---\n");
    logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test20 complete ===\n");
    return 0;
}
