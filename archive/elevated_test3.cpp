// elevated_test3.cpp
//
// Focused elevated test: CreateEntryEX + full INSERT pipeline + cleanup.
// Skips record enumeration (which crashed in test2).
//
// Build: cmake --build build --config Release --target elevated_test3
// Run (MUST be elevated): build\Release\elevated_test3.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);
typedef short (__stdcall *SaveTextEX_t)(const char* sourceText, int whType,
    int progId, void* buffer);
typedef int (__stdcall *VerifyModule_New_t)(int contextVal, int whType,
    int progId, int isMatch, void* compileCtx);
typedef void (__stdcall *StartCompilingProcess_t)(void);
typedef void (__stdcall *EndCompilingProcess_t)(void);
typedef void (__stdcall *GetCompilingErrors_t)(void* outStruct);
typedef int (__stdcall *GetErrorLevel_t)(int handle);
typedef void (__stdcall *SetSigDeleted_t)(void* masterRecord);
typedef BOOL (__stdcall *GetMasterRecByName_t)(const char* name, int whType, void* outRecord);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";

static HMODULE g_hOrlib = NULL;

template<typename T>
T Resolve(const char* name) {
    T p = (T)GetProcAddress(g_hOrlib, name);
    if (!p) std::printf("  FAILED to resolve \"%s\"\n", name);
    return p;
}

// Run probe in worker thread with large stack
DWORD WINAPI probe_main(LPVOID);

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    HANDLE hThread = CreateThread(NULL, 64 * 1024 * 1024,
        (LPTHREAD_START_ROUTINE)probe_main, NULL, 0, NULL);
    if (!hThread) { std::printf("FATAL: CreateThread failed\n"); return 1; }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    return (int)exitCode;
}

DWORD WINAPI probe_main(LPVOID) {
    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev;
        DWORD cbSize = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cbSize))
            fIsElevated = elev.TokenIsElevated;
        CloseHandle(hToken);
    }
    std::printf("=== elevated_test3: INSERT pipeline test ===\n");
    std::printf("Process is %s\n", fIsElevated ? "ELEVATED" : "NOT ELEVATED");
    std::printf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { std::printf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { std::printf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    std::printf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pCreateEntry = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveText = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry) {
        std::printf("FATAL: missing required exports\n"); return 5;
    }

    // Step 1: OrlibInit
    std::printf("\n--- Step 1: OrlibInit(NULL, 1) ---\n");
    BOOL initOk = pOrlibInit(NULL, 1);
    std::printf("  OrlibInit(NULL, 1) = %d\n", initOk);
    if (!initOk) { std::printf("FATAL: OrlibInit failed\n"); return 6; }

    // Step 2: Confirm master records loaded
    std::printf("\n--- Step 2: Master Record Counts ---\n");
    if (pGetNum) {
        __try {
            int count0 = pGetNum(0);
            std::printf("  Strategies(0) = %d\n", count0);
            int count1 = pGetNum(1);
            std::printf("  Functions(1) = %d\n", count1);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 3: CreateEntryEX — THE CRITICAL TEST
    std::printf("\n--- Step 3: CreateEntryEX ---\n");
    const char* testName = "zzz_cli_test_probe";
    int errCode = 0;
    short newProgId = -1;
    __try {
        // Confirmed param order: (WHType, subType, string2, string1=name, flags, &errCode)
        newProgId = pCreateEntry(0, 0, "test", testName, 0, &errCode);
        std::printf("  CreateEntryEX(0, 0, \"test\", \"%s\", 0, &err) = %d (0x%X)\n",
            testName, (int)newProgId, (unsigned short)newProgId);
        std::printf("  errCode = %d\n", errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        newProgId = -1;
    }

    if (newProgId < 0) {
        std::printf("\n  CreateEntryEX failed. INSERT not viable with current approach.\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }

    // Step 4: FindExistPos with the test name — confirms the entry was created
    std::printf("\n--- Step 4: FindExistPos(\"%s\") ---\n", testName);
    int whType = -1;
    __try {
        int foundProgId = pFindExistPos(testName, &whType);
        std::printf("  FindExistPos(\"%s\") = %d (whType=%d)\n", testName, foundProgId, whType);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 5: SaveTextEX — set source text directly (headless path)
    std::printf("\n--- Step 5: SaveTextEX (direct call) ---\n");
    const char* testSource =
        "Inputs: Price(Close), Length(10);\n"
        "Variables: Avg(0);\n"
        "Avg = Average(Price, Length);\n"
        "Plot1(Avg, \"Avg\");\n";
    char saveBuffer[4096];
    memset(saveBuffer, 0, sizeof(saveBuffer));
    short saveResult = -1;
    __try {
        saveResult = pSaveText(testSource, 0, newProgId, saveBuffer);
        std::printf("  SaveTextEX(source, 0, %d, buffer) = %d (0x%X)\n",
            (int)newProgId, (int)saveResult, (unsigned short)saveResult);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 6: StartCompilingProcess
    std::printf("\n--- Step 6: StartCompilingProcess ---\n");
    if (pStartComp) {
        __try {
            pStartComp();
            std::printf("  StartCompilingProcess() -> OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 7: VerifyModule_New
    std::printf("\n--- Step 7: VerifyModule_New ---\n");
    if (pVerify) {
        int verifyResult = -1;
        __try {
            // (contextVal, WHType, ProgId, isMatch, compileCtx)
            verifyResult = pVerify(0, 0, newProgId, 1, NULL);
            std::printf("  VerifyModule_New(0, 0, %d, 1, NULL) = %d\n", newProgId, verifyResult);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        }

        // Check compile errors
        if (pGetErrors) {
            int errStruct[4] = {0,0,0,0};
            __try {
                pGetErrors(errStruct);
                std::printf("  GetCompilingErrors: {%d, %d, %d, %d}\n",
                    errStruct[0], errStruct[1], errStruct[2], errStruct[3]);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  GetCompilingErrors CRASHED: 0x%X\n", GetExceptionCode());
            }
        }
        if (pGetErrLevel) {
            __try {
                int errLevel = pGetErrLevel(0);
                std::printf("  GetErrorLevel(0) = %d\n", errLevel);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  GetErrorLevel CRASHED: 0x%X\n", GetExceptionCode());
            }
        }
    }

    // Step 8: EndCompilingProcess
    std::printf("\n--- Step 8: EndCompilingProcess ---\n");
    if (pEndComp) {
        __try {
            pEndComp();
            std::printf("  EndCompilingProcess() -> OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 9: Check if entry was persisted to disk
    std::printf("\n--- Step 9: Check persistence ---\n");
    char ellPath[512];
    sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", (int)newProgId);
    char xmlPath[512];
    sprintf(xmlPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.xml", (int)newProgId);
    FILE* fell = fopen(ellPath, "rb");
    FILE* fxml = fopen(xmlPath, "rb");
    if (fell) { std::printf("  FOUND: %s\n", ellPath); fclose(fell); }
    else      { std::printf("  NOT FOUND: %s\n", ellPath); }
    if (fxml) { std::printf("  FOUND: %s\n", xmlPath); fclose(fxml); }
    else      { std::printf("  NOT FOUND: %s\n", xmlPath); }

    // Step 10: CLEANUP — delete the test entry
    std::printf("\n--- Step 10: Cleanup ---\n");
    if (pGetRecByName && pSetSigDeleted) {
        char masterRec[268];
        memset(masterRec, 0, sizeof(masterRec));
        __try {
            BOOL gotRec = pGetRecByName(testName, 0, masterRec);
            std::printf("  GetMasterRecByName(\"%s\", 0, &rec) = %d\n", testName, gotRec);
            if (gotRec) {
                pSetSigDeleted(masterRec);
                std::printf("  SetSigDeleted(&rec) called\n");
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        }
    } else {
        std::printf("  WARNING: cleanup functions not available\n");
    }

    // Verify cleanup
    fell = fopen(ellPath, "rb");
    if (fell) { std::printf("  WARNING: %s still exists!\n", ellPath); fclose(fell); }
    else      { std::printf("  Verified: %s deleted\n", ellPath); }

    // Step 11: OrlibExit
    std::printf("\n--- Step 11: OrlibExit ---\n");
    if (pOrlibExit) {
        BOOL exitOk = pOrlibExit();
        std::printf("  OrlibExit() = %d\n", exitOk);
    }

    FreeLibrary(g_hOrlib);
    std::printf("\n=== elevated_test3 complete ===\n");
    std::printf("\nINTERPRETATION:\n");
    std::printf("  If CreateEntryEX returned >=0: INSERT is viable headlessly (when elevated)!\n");
    std::printf("  If SaveTextEX returned non-zero: source text setting works!\n");
    std::printf("  If VerifyModule_New returned without crash: verification works!\n");
    std::printf("  If .ell/.xml files appeared: persistence works!\n");
    return 0;
}
