// elevated_test5.cpp
//
// Comprehensive persistence and name-setting investigation test.
//
// Based on static RE findings:
//  - VerifyModule_New@20 DOES perform disk persistence through a post-save
//    function that calls ELMDCore.dll!SaveModule.  The save is dispatched via
//    an internal function (0x101087D0) whose behaviour depends on arg1
//    (contextVal).  TSDev passes a non-zero contextVal from its compile
//    context object (this->field_0x18).  The prior test used contextVal=0,
//    which may cause the save path to fail silently.
//  - CreateEntryEX@24 arg3 is the primary NAME string, set via an internal
//    name-setting chain (0x101CF990 -> 0x101D0860 -> 0x101C4AB0).
//  - arg4 (secondary) is copied to module record offset 0x17 (max 61 chars)
//    via lstrcpyn — it is a short-name / notes field, NOT the primary name.
//  - _CloseWarehouseForCurrentThreadAndPath@4 is called inside
//    VerifyModule_New before the save dispatch.
//  - _SaveTextEX@16(sourceText, WHType, ProgId, buffer) is a direct
//    source-saving path that bypasses the hidden-EDIT-control workaround.
//
// Build: cmake --build build --config Release --target elevated_test5
// Run (MUST be elevated): build\Release\elevated_test5.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

// ---- orlib20.dll function typedefs ----
typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int  (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int  (__stdcall *GetNumofMasterRecsEX_t)(int whType);
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);
typedef int  (__stdcall *SaveModule_New_t)(int whType, int progId,
    void* arg3, HWND hwnd, void* buffer);
typedef int  (__stdcall *SaveTextEX_t)(const char* sourceText,
    int whType, int progId, void* buffer);
typedef int  (__stdcall *VerifyModule_New_t)(int contextVal, int whType,
    int progId, int isMatch, void* compileCtx);
typedef void (__stdcall *StartCompilingProcess_t)(void);
typedef void (__stdcall *EndCompilingProcess_t)(void);
typedef void (__stdcall *GetCompilingErrors_t)(void* outStruct);
typedef int  (__stdcall *GetErrorLevel_t)(int handle);
typedef void (__stdcall *SetSigDeleted_t)(void* masterRecord);
typedef BOOL (__stdcall *GetMasterRecByName_t)(const char* name,
    int whType, void* outRecord);
typedef void (__stdcall *CloseWarehouse_t)(int arg);

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
static const char* kBinDir =
    "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";
static HMODULE g_hOrlib = NULL;

template<typename T>
T Resolve(const char* name) {
    T p = (T)GetProcAddress(g_hOrlib, name);
    if (!p) std::printf("  FAILED to resolve \"%s\"\n", name);
    return p;
}

// Check for .ell and .xml files for a given progId
static void CheckPersistence(int progId) {
    char ellPath[512], xmlPath[512];
    sprintf(ellPath, "%s\\elmodule_0_%d.ell", kBinDir, progId);
    sprintf(xmlPath, "%s\\elmodule_0_%d.xml", kBinDir, progId);
    FILE* f = fopen(ellPath, "rb");
    if (f) { std::printf("  FOUND .ell: %s\n", ellPath); fclose(f); }
    else   { std::printf("  NOT FOUND .ell: %s\n", ellPath); }
    f = fopen(xmlPath, "rb");
    if (f) { std::printf("  FOUND .xml: %s\n", xmlPath); fclose(f); }
    else   { std::printf("  NOT FOUND .xml: %s\n", xmlPath); }
}

// List all .ell and .xml files in Bin (to catch unexpected names)
static void ListBinFiles() {
    char pattern[512];
    sprintf(pattern, "%s\\*.ell", kBinDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::printf("  .ell files in Bin:\n");
        do {
            std::printf("    %s\n", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    } else {
        std::printf("  No .ell files found in Bin\n");
    }
    sprintf(pattern, "%s\\*.xml", kBinDir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::printf("  .xml files in Bin:\n");
        do {
            std::printf("    %s\n", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    } else {
        std::printf("  No .xml files found in Bin\n");
    }
}

// Clean up a test entry by name
static void CleanupEntry(FindExistPos_t pFindExistPos,
                         GetMasterRecByName_t pGetRecByName,
                         SetSigDeleted_t pSetSigDeleted,
                         const char* name) {
    if (!pGetRecByName || !pSetSigDeleted) return;
    char rec[268]; memset(rec, 0, sizeof(rec));
    __try {
        BOOL got = pGetRecByName(name, 0, rec);
        if (got) {
            pSetSigDeleted(rec);
            std::printf("  Deleted: \"%s\"\n", name);
        } else {
            std::printf("  Not found for cleanup: \"%s\"\n", name);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  Cleanup crash for \"%s\": 0x%X\n", name, GetExceptionCode());
    }
}

// Try one full insert+verify cycle with given parameters
static int TryInsertVerify(
    CreateEntryEX_t pCreateEntry,
    SaveTextEX_t pSaveText,
    SaveModule_New_t pSaveModule,
    VerifyModule_New_t pVerify,
    StartCompilingProcess_t pStartComp,
    EndCompilingProcess_t pEndComp,
    GetCompilingErrors_t pGetErrors,
    GetErrorLevel_t pGetErrLevel,
    CloseWarehouse_t pCloseWh,
    FindExistPos_t pFindExistPos,
    const char* testName,
    const char* secondary,
    int verifyContextVal,
    int verifyIsMatch,
    bool useSaveTextEX,
    int testNum)
{
    std::printf("\n=== ATTEMPT %d: name=\"%s\" secondary=\"%s\" "
                "ctxVal=%d isMatch=%d saveText=%d ===\n",
                testNum, testName, secondary,
                verifyContextVal, verifyIsMatch, useSaveTextEX ? 1 : 0);

    // CreateEntryEX
    int errCode = 0;
    short progId = -1;
    __try {
        progId = pCreateEntry(0, 0, testName, secondary, 0, &errCode);
        std::printf("  CreateEntryEX(0,0,\"%s\",\"%s\",0,&err) = %d (err=%d)\n",
            testName, secondary, (int)progId, errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CreateEntryEX CRASHED: 0x%X\n", GetExceptionCode());
        return -1;
    }
    if (progId < 0) {
        std::printf("  CreateEntryEX failed (err=%d)\n", errCode);
        return -1;
    }

    // FindExistPos to check if name is searchable
    int whType = -1;
    __try {
        int found = pFindExistPos(testName, &whType);
        std::printf("  FindExistPos(\"%s\") = %d (whType=%d) %s\n",
            testName, found, whType,
            found >= 0 ? "<-- NAME IS SEARCHABLE" : "<-- NAME NOT FOUND");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  FindExistPos CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Save source text
    const char* testSource =
        "Inputs: Price(Close), Length(10);\n"
        "Variables: Avg(0);\n"
        "Avg = Average(Price, Length);\n"
        "Plot1(Avg, \"Avg\");\n";

    if (useSaveTextEX && pSaveText) {
        char saveBuffer[4096]; memset(saveBuffer, 0, sizeof(saveBuffer));
        __try {
            int r = pSaveText(testSource, 0, progId, saveBuffer);
            std::printf("  SaveTextEX(src,0,%d,buf) = %d\n", (int)progId, r);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  SaveTextEX CRASHED: 0x%X\n", GetExceptionCode());
        }
    } else if (pSaveModule) {
        // Use hidden EDIT control
        wchar_t wSource[4096];
        MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);
        HWND hEdit = CreateWindowW(L"EDIT", wSource,
            WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
        if (hEdit) {
            char saveBuffer[4096]; memset(saveBuffer, 0, sizeof(saveBuffer));
            __try {
                int r = pSaveModule(0, progId, NULL, hEdit, saveBuffer);
                std::printf("  SaveModule_New(0,%d,NULL,hwnd,buf) = %d\n",
                    (int)progId, r);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  SaveModule_New CRASHED: 0x%X\n",
                    GetExceptionCode());
            }
            DestroyWindow(hEdit);
        }
    }

    // Check persistence before verify
    std::printf("  Persistence BEFORE verify:\n");
    CheckPersistence(progId);

    // Verify pipeline
    if (pStartComp) {
        __try { pStartComp(); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  StartComp CRASH: 0x%X\n", GetExceptionCode());
        }
    }
    if (pVerify) {
        __try {
            int v = pVerify(verifyContextVal, 0, progId, verifyIsMatch, NULL);
            std::printf("  VerifyModule_New(%d,0,%d,%d,NULL) = %d\n",
                verifyContextVal, (int)progId, verifyIsMatch, v);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  Verify CRASH: 0x%X\n", GetExceptionCode());
        }
    }
    if (pGetErrors) {
        int e[4] = {0,0,0,0};
        __try { pGetErrors(e); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        std::printf("  GetCompilingErrors: {%d,%d,%d,%d}\n",
            e[0],e[1],e[2],e[3]);
    }
    if (pGetErrLevel) {
        __try {
            std::printf("  GetErrorLevel(0) = %d\n", pGetErrLevel(0));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pEndComp) {
        __try { pEndComp(); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  EndComp CRASH: 0x%X\n", GetExceptionCode());
        }
    }

    // Check persistence after verify
    std::printf("  Persistence AFTER verify:\n");
    CheckPersistence(progId);

    // Try CloseWarehouse
    if (pCloseWh) {
        __try {
            pCloseWh(0);
            std::printf("  CloseWarehouseForCurrentThreadAndPath(0) OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  CloseWarehouse CRASH: 0x%X\n", GetExceptionCode());
        }
        // Check persistence after warehouse close
        std::printf("  Persistence AFTER CloseWarehouse:\n");
        CheckPersistence(progId);
    }

    // Check FindExistPos again after full pipeline
    whType = -1;
    __try {
        int found = pFindExistPos(testName, &whType);
        std::printf("  FindExistPos AFTER pipeline(\"%s\") = %d (whType=%d) %s\n",
            testName, found, whType,
            found >= 0 ? "<-- NAME IS SEARCHABLE" : "<-- NAME NOT FOUND");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  FindExistPos CRASHED: 0x%X\n", GetExceptionCode());
    }

    return progId;
}

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
    std::printf("=== elevated_test5: Persistence & Name Investigation ===\n");
    std::printf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { std::printf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) {
        std::printf("FATAL: LoadLibraryW: %lu\n", GetLastError());
        return 4;
    }

    auto pOrlibInit    = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit    = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum       = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pCreateEntry  = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveModule   = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pSaveText     = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pVerify       = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp    = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp      = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors    = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel  = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted= Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");
    auto pCloseWh      = Resolve<CloseWarehouse_t>(
        "_CloseWarehouseForCurrentThreadAndPath@4");

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry || !pVerify) {
        std::printf("FATAL: missing critical exports\n");
        return 5;
    }

    // Step 1: OrlibInit
    std::printf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) {
        std::printf("FATAL: OrlibInit failed\n");
        return 6;
    }
    std::printf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: Record counts
    if (pGetNum) {
        __try {
            std::printf("  MasterRecs(0) = %d\n", pGetNum(0));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 3: List existing Bin files (baseline)
    std::printf("\n--- Step 3: Baseline Bin files ---\n");
    ListBinFiles();

    // Step 4: Clean up any leftover entries from prior test crashes
    std::printf("\n--- Step 4: Cleanup leftover entries ---\n");
    const char* cleanupNames[] = {
        "MyTestStrategy", "MyTestStrategyA", "MyTestStrategyB",
        "MyTestStrategyC", "MyTestStrategyD", "MyTestStrategyE",
        "MyTestStrategyF", "str3", "str4",
        "zzz_arg3_is_name", "zzz_arg4_is_name", "test",
        NULL
    };
    for (int i = 0; cleanupNames[i]; i++) {
        CleanupEntry(pFindExistPos, pGetRecByName, pSetSigDeleted,
            cleanupNames[i]);
    }

    // Step 5: Run test attempts with different parameter patterns
    std::printf("\n--- Step 5: Test attempts ---\n");

    // Track all created progIds for cleanup
    int createdProgIds[16]; int numCreated = 0;

    // Attempt 1: name in arg3, empty secondary, ctxVal=0, isMatch=1, SaveModule_New
    // (reproduces the original test4 pattern)
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "MyTestStrategyA", "", 0, 1, false, 1);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
    }

    // Attempt 2: name in arg3, empty secondary, ctxVal=1, isMatch=1, SaveTextEX
    // (non-zero contextVal, direct save path)
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "MyTestStrategyB", "", 1, 1, true, 2);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
    }

    // Attempt 3: name in arg3, empty secondary, ctxVal=1, isMatch=0, SaveTextEX
    // (non-zero contextVal, isMatch=0)
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "MyTestStrategyC", "", 1, 0, true, 3);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
    }

    // Attempt 4: name in arg3, name also in secondary, ctxVal=1, isMatch=1, SaveTextEX
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "MyTestStrategyD", "MyTestStrategyD", 1, 1, true, 4);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
    }

    // Attempt 5: empty name, name in secondary, ctxVal=1, isMatch=1, SaveTextEX
    // (test if secondary is the name)
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "", "MyTestStrategyE", 1, 1, true, 5);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
        // Also check FindExistPos for the secondary name
        if (pid > 0) {
            int whType = -1;
            __try {
                int found = pFindExistPos("MyTestStrategyE", &whType);
                std::printf("  FindExistPos(\"MyTestStrategyE\" [secondary]) = %d %s\n",
                    found, found >= 0 ? "<-- SECONDARY IS SEARCHABLE" : "");
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // Attempt 6: name in arg3, ctxVal=0, isMatch=0, SaveTextEX
    // (original ctxVal=0 but with isMatch=0)
    {
        int pid = TryInsertVerify(pCreateEntry, pSaveText, pSaveModule,
            pVerify, pStartComp, pEndComp, pGetErrors, pGetErrLevel,
            pCloseWh, pFindExistPos,
            "MyTestStrategyF", "", 0, 0, true, 6);
        if (pid > 0 && numCreated < 16) createdProgIds[numCreated++] = pid;
    }

    // Step 6: Final Bin file listing
    std::printf("\n--- Step 6: Final Bin files ---\n");
    ListBinFiles();

    // Step 7: Cleanup all created entries
    std::printf("\n--- Step 7: Final cleanup ---\n");
    for (int i = 0; i < numCreated; i++) {
        char name[64];
        sprintf(name, "MyTestStrategy%c", 'A' + i);
        CleanupEntry(pFindExistPos, pGetRecByName, pSetSigDeleted, name);
    }
    // Also try cleanup by the secondary-name patterns
    CleanupEntry(pFindExistPos, pGetRecByName, pSetSigDeleted, "MyTestStrategyE");

    // Step 8: OrlibExit
    std::printf("\n--- Step 8: OrlibExit ---\n");
    if (pOrlibExit) std::printf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    std::printf("\n=== elevated_test5 complete ===\n");
    return 0;
}
