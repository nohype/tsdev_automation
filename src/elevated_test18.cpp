// elevated_test18.cpp
//
// THE CRITICAL PERSISTENCE TEST
//
// Based on Ghidra decompilation analysis, the correct INSERT sequence is:
//   1. OrlibInit(NULL, 1)
//   2. CreateEntryEX(0, 0, name, secondary, 0, &err)  -- create Strategy entry
//   3. FindExistPos(name, 0)                          -- get ProgId
//   4. Create hidden EDIT control, set source text
//   5. SaveModule_New(0, progId, NULL, hwnd, NULL)    -- saves source + clears 0xd8 bit 1
//   6. VerifyModule_New(1, 0, progId, 0, NULL)        -- compiles and persists .ell/.xml
//
// Root cause of previous failures:
//   - VerifyModule_New checks module record offset 0xd8 bit 1
//   - If SET, it skips the save dispatch and calls CompileText with NULL source
//   - CreateEntryEX creates entries with this bit SET
//   - SaveModule_New CLEARS this bit (local_5c & 0xfffd in FUN_10159770)
//   - Previous probes called SaveModule_New and VerifyModule_New separately,
//     never in the correct sequence
//
// SaveModule_New argument order (from disassembly):
//   SaveModule_New(warehouseType, progId, flags, editorHwnd, buffer)
//   - flags: NULL for default
//   - buffer: NULL for no password encryption
//
// VerifyModule_New argument order (from decompilation):
//   VerifyModule_New(param1, warehouseType, progId, param4, param5)
//   - param1: 1 for verify (0 for compile-only?)
//   - param4: 0 or 1 (some flag)
//   - param5: NULL (CELCompiler*)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>

// --- orlib20 export typedefs ---
typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int  (__stdcall *CreateEntryEX_t)(int whType, int secondary, const char* name,
                                           int param4, char param5, int* errCode);
typedef int  (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int  (__stdcall *SaveModule_New_t)(int warehouseType, int progId,
                                            void* flags, HWND editorHwnd, void* buffer);
typedef int  (__stdcall *VerifyModule_New_t)(int param1, int warehouseType,
                                              int progId, int param4, void* param5);
typedef int  (__stdcall *SaveTextEX_t)(const char* sourceText, int warehouseType,
                                        int progId, void* passwordBuffer);

// VerifyATechnique(name, warehouseType) - exported wrapper that handles
// CELCompiler object creation and proper compilation setup internally.
// This is the HIGH-LEVEL verify API that TSDev actually uses.
typedef bool (__stdcall *VerifyATechnique_t)(const char* name, int warehouseType);

// Internal functions (not exported, called by address)
typedef void (__cdecl *StartCompilingProcess_t)(void);
typedef void (__fastcall *EndCompilingProcess_t)(int param_1);
typedef void (__stdcall *GetCompilingErrors_t)(void* buffer);

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
    g_log = fopen("C:\\Users\\user\\projects\\tsdev_automation\\elevated_test18_log.txt", "w");
    if (!g_log) { printf("FATAL: can't open log file\n"); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);

    // Run in a thread with a large stack (orlib20 uses deep recursion)
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

// Check if a file exists
bool fileExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES);
}

// List .ell and .xml files for a given ProgId
void checkFiles(int progId) {
    char ellPath[512], xmlPath[512];
    sprintf(ellPath, "%s\\elmodule_0_%d.ell", kMyWorkBin, progId);
    sprintf(xmlPath, "%s\\elmodule_0_%d.xml", kMyWorkBin, progId);

    logf("  Checking: %s\n", ellPath);
    logf("    %s\n", fileExists(ellPath) ? "EXISTS" : "MISSING");
    logf("  Checking: %s\n", xmlPath);
    logf("    %s\n", fileExists(xmlPath) ? "EXISTS" : "MISSING");

    if (fileExists(ellPath)) {
        HANDLE h = CreateFileA(ellPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, NULL);
            logf("    .ell size = %lu bytes\n", size);
            CloseHandle(h);
        }
    }
    if (fileExists(xmlPath)) {
        HANDLE h = CreateFileA(xmlPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, NULL);
            logf("    .xml size = %lu bytes\n", size);
            CloseHandle(h);
        }
    }
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test18: THE CRITICAL PERSISTENCE TEST ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));
    logf("Strategy: CreateEntryEX -> SaveModule_New -> VerifyModule_New\n\n");

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    logf("orlib20.dll base = 0x%p\n", (void*)g_hOrlib);

    // Resolve exports
    auto pOrlibInit      = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit      = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pCreateEntry    = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pFindExistPos   = Resolve<FindExistPos_t>("FindExistPos");
    auto pSaveModuleNew  = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerifyModule   = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pSaveTextEX     = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pVerifyATech    = Resolve<VerifyATechnique_t>("VerifyATechnique");
    auto pGetErrors      = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");

    // Internal functions by address
    char* base = (char*)g_hOrlib;
    auto pStartCompiling = (StartCompilingProcess_t)(base + 0x165de0);
    auto pEndCompiling   = (EndCompilingProcess_t)(base + 0x165ed0);

    if (!pOrlibInit || !pOrlibExit || !pCreateEntry || !pFindExistPos ||
        !pSaveModuleNew || !pVerifyModule) {
        logf("FATAL: missing critical exports\n");
        return 5;
    }
    logf("All exports resolved.\n");
    logf("  StartCompilingProcess @ 0x%p\n", (void*)pStartCompiling);
    logf("  EndCompilingProcess @ 0x%p\n", (void*)pEndCompiling);

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    BOOL initResult = pOrlibInit(NULL, 1);
    logf("  OrlibInit(NULL, 1) = %d\n", initResult);
    if (!initResult) { logf("FATAL: OrlibInit failed\n"); return 6; }

    // Step 2: Create a new Strategy entry with a unique name
    const char* strategyName = "zzz_cli_persist_test";
    logf("\n--- Step 2: CreateEntryEX ---\n");
    logf("  Creating strategy \"%s\"\n", strategyName);

    // Check if it already exists
    int existingProgId = pFindExistPos(strategyName, 0);
    logf("  Pre-check: FindExistPos(\"%s\", 0) = %d\n", strategyName, existingProgId);

    int progId = -1;
    if (existingProgId > 0) {
        logf("  Entry already exists, reusing ProgId %d\n", existingProgId);
        progId = existingProgId;
    } else {
        int errCode = 0;
        int createResult = -1;
        __try {
            createResult = pCreateEntry(0, 0, strategyName, 0, 0, &errCode);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  CreateEntryEX CRASHED: 0x%X\n", GetExceptionCode());
        }
        logf("  CreateEntryEX(0, 0, \"%s\", 0, 0, &err) = %d, errCode = 0x%X\n",
            strategyName, createResult, errCode);

        if (createResult < 0) {
            logf("  CreateEntryEX failed, trying FindExistPos anyway\n");
        }

        progId = pFindExistPos(strategyName, 0);
        logf("  FindExistPos(\"%s\", 0) = %d\n", strategyName, progId);
    }

    if (progId <= 0) {
        logf("FATAL: Could not get a valid ProgId\n");
        pOrlibExit();
        return 7;
    }
    logf("  Using ProgId = %d\n", progId);

    // Check files before save
    logf("\n  Files BEFORE save:\n");
    checkFiles(progId);

    // Step 3: Create a hidden EDIT control with the source text
    logf("\n--- Step 3: Create hidden EDIT control ---\n");
    // Create a message-only parent window (HWND_MESSAGE) for the EDIT control
    HWND hParent = CreateWindowW(L"STATIC", L"", WS_POPUP,
                                 0, 0, 100, 100, HWND_MESSAGE, NULL, NULL, NULL);
    if (!hParent) {
        logf("  Parent window creation failed: %lu, trying without parent\n", GetLastError());
        hParent = NULL;
    }
    HWND hEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
                               0, 0, 100, 100, hParent, NULL, NULL, NULL);
    if (!hEdit) {
        logf("  EDIT with parent failed: %lu, trying without WS_CHILD\n", GetLastError());
        hEdit = CreateWindowW(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL,
                              0, 0, 100, 100, NULL, NULL, NULL, NULL);
    }
    if (!hEdit) {
        logf("FATAL: CreateWindowW(EDIT) failed: %lu\n", GetLastError());
        pOrlibExit();
        return 8;
    }
    logf("  Created EDIT control: HWND = 0x%p\n", (void*)hEdit);
    logf("  IsWindow(hwnd) = %d\n", IsWindow(hEdit));

    // Minimal valid Strategy EasyLanguage source
    const char* sourceText =
        "{ zzz_cli_persist_test } \r\n"
        "Inputs: \r\n"
        "    Length(10); \r\n"
        "Variables: \r\n"
        "    MA(0); \r\n"
        "MA = Average(Close, Length); \r\n"
        "if Close > MA then \r\n"
        "    Buy (\"EnterLong\") next bar at Market; \r\n"
        "if Close < MA then \r\n"
        "    SellShort (\"EnterShort\") next bar at Market; \r\n";

    // Set text into the EDIT control (Unicode)
    int sourceLen = strlen(sourceText);
    wchar_t* wideText = new wchar_t[sourceLen + 1];
    MultiByteToWideChar(CP_ACP, 0, sourceText, -1, wideText, sourceLen + 1);
    SetWindowTextW(hEdit, wideText);
    delete[] wideText;

    // Verify text was set
    int textLen = GetWindowTextLengthW(hEdit);
    logf("  SetWindowTextW done, GetWindowTextLength = %d (expected %d)\n",
        textLen, sourceLen);

    // Step 4: Call SaveModule_New to save source text AND clear 0xd8 bit 1
    logf("\n--- Step 4: SaveModule_New ---\n");
    logf("  SaveModule_New(0, %d, NULL, hwnd, NULL)\n", progId);
    int saveResult = 0;
    __try {
        saveResult = pSaveModuleNew(0, progId, NULL, hEdit, NULL);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
    }
    logf("  SaveModule_New = %d\n", saveResult);

    // Check files after save (before verify)
    logf("\n  Files AFTER SaveModule_New (before Verify):\n");
    checkFiles(progId);

    // Step 5: Use VerifyATechnique (the high-level wrapper TSDev actually uses)
    // It handles CELCompiler object creation and the proper compilation setup.
    logf("\n--- Step 5: VerifyATechnique (high-level wrapper) ---\n");

    if (pVerifyATech) {
        // StartCompilingProcess before verify
        logf("  Calling StartCompilingProcess()...\n");
        __try {
            pStartCompiling();
            logf("  StartCompilingProcess done\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  StartCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
        }

        logf("  VerifyATechnique(\"%s\", 0)\n", strategyName);
        bool vatResult = false;
        __try {
            vatResult = pVerifyATech(strategyName, 0);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  VerifyATechnique CRASHED: 0x%X\n", GetExceptionCode());
        }
        logf("  VerifyATechnique = %d\n", (int)vatResult);

        // Check for compilation errors
        if (pGetErrors) {
            logf("  Calling GetCompilingErrors...\n");
            __try {
                int errorBuf = 0;
                pGetErrors(&errorBuf);
                logf("  GetCompilingErrors done, errorBuf = %d\n", errorBuf);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  GetCompilingErrors CRASHED: 0x%X\n", GetExceptionCode());
            }
        }

        // EndCompilingProcess after verify
        logf("  Calling EndCompilingProcess(0)...\n");
        __try {
            pEndCompiling(0);
            logf("  EndCompilingProcess done\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  EndCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
        }
    } else {
        logf("  VerifyATechnique NOT resolved - falling back to VerifyModule_New\n");
        logf("  VerifyModule_New(1, 0, %d, 0, NULL)\n", progId);
        int verifyResult = 0;
        __try {
            verifyResult = pVerifyModule(1, 0, progId, 0, NULL);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  VerifyModule_New CRASHED: 0x%X\n", GetExceptionCode());
        }
        logf("  VerifyModule_New = %d\n", verifyResult);
    }

    // Check files after verify
    logf("\n  Files AFTER VerifyModule_New:\n");
    checkFiles(progId);

    // Step 6: Cleanup
    logf("\n--- Step 6: Cleanup ---\n");
    DestroyWindow(hEdit);
    logf("  DestroyWindow done\n");

    // Also try SaveTextEX as an alternative approach
    if (pSaveTextEX) {
        logf("\n--- Bonus: Try SaveTextEX + manual flag clear ---\n");
        // Create another entry to test with
        const char* name2 = "zzz_cli_persist_test2";
        int existing2 = pFindExistPos(name2, 0);
        int progId2 = existing2;
        if (progId2 <= 0) {
            int err2 = 0;
            __try {
                pCreateEntry(0, 0, name2, 0, 0, &err2);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            progId2 = pFindExistPos(name2, 0);
        }
        logf("  Entry2: \"%s\" -> ProgId %d\n", name2, progId2);

        if (progId2 > 0) {
            // Save source text directly
            logf("  SaveTextEX(sourceText, 0, %d, NULL)\n", progId2);
            int stResult = 0;
            __try {
                stResult = pSaveTextEX(sourceText, 0, progId2, NULL);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logf("  SaveTextEX CRASHED: 0x%X\n", GetExceptionCode());
            }
            logf("  SaveTextEX = %d\n", stResult);

            // Now use SaveModule_New with hidden EDIT to clear the flag
            HWND hParent2 = CreateWindowW(L"STATIC", L"", WS_POPUP,
                                          0, 0, 100, 100, HWND_MESSAGE, NULL, NULL, NULL);
            if (!hParent2) hParent2 = NULL;
            HWND hEdit2 = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
                                        0, 0, 100, 100, hParent2, NULL, NULL, NULL);
            if (!hEdit2) {
                hEdit2 = CreateWindowW(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL,
                                       0, 0, 100, 100, NULL, NULL, NULL, NULL);
            }
            if (hEdit2) {
                wchar_t* wideText2 = new wchar_t[sourceLen + 1];
                MultiByteToWideChar(CP_ACP, 0, sourceText, -1, wideText2, sourceLen + 1);
                SetWindowTextW(hEdit2, wideText2);
                delete[] wideText2;

                logf("  SaveModule_New(0, %d, NULL, hwnd2, NULL)\n", progId2);
                __try {
                    int r = pSaveModuleNew(0, progId2, NULL, hEdit2, NULL);
                    logf("  SaveModule_New = %d\n", r);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  SaveModule_New CRASHED: 0x%X\n", GetExceptionCode());
                }

                // StartCompilingProcess before Verify
                __try {
                    pStartCompiling();
                    logf("  StartCompilingProcess done\n");
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  StartCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
                }

                if (pVerifyATech) {
                    logf("  VerifyATechnique(\"%s\", 0)\n", name2);
                    __try {
                        bool r = pVerifyATech(name2, 0);
                        logf("  VerifyATechnique = %d\n", (int)r);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        logf("  VerifyATechnique CRASHED: 0x%X\n", GetExceptionCode());
                    }
                } else {
                    logf("  VerifyModule_New(1, 0, %d, 0, NULL)\n", progId2);
                    __try {
                        int r = pVerifyModule(1, 0, progId2, 0, NULL);
                        logf("  VerifyModule_New = %d\n", r);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        logf("  VerifyModule_New CRASHED: 0x%X\n", GetExceptionCode());
                    }
                }

                // EndCompilingProcess after Verify
                __try {
                    pEndCompiling(0);
                    logf("  EndCompilingProcess done\n");
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logf("  EndCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
                }

                logf("\n  Files for entry2:\n");
                checkFiles(progId2);

                DestroyWindow(hEdit2);
            }
        }
    }

    // OrlibExit
    logf("\n--- OrlibExit ---\n");
    logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test18 complete ===\n");
    return 0;
}
