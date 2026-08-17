// elevated_test4.cpp
//
// Test SaveModule_New with hidden EDIT control (the original FEASIBILITY.md approach)
// instead of direct SaveTextEX. Also tests which CreateEntryEX arg is the name.
// Cleans up leftover entries from prior tests.
//
// Build: cmake --build build --config Release --target elevated_test4
// Run (MUST be elevated): build\Release\elevated_test4.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);
typedef short (__stdcall *CreateEntryEX_t)(int whType, int subType,
    const char* name, const char* secondary, int flags, int* outErrCode);
typedef int (__stdcall *SaveModule_New_t)(int whType, int progId,
    void* arg3, HWND hwnd, void* buffer);
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
    std::printf("=== elevated_test4: SaveModule_New + hidden EDIT test ===\n");
    std::printf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { std::printf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { std::printf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pCreateEntry = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveModule = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");

    if (!pOrlibInit || !pFindExistPos || !pCreateEntry) {
        std::printf("FATAL: missing exports\n"); return 5;
    }

    // Step 1: OrlibInit
    std::printf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { std::printf("FATAL: OrlibInit failed\n"); return 6; }
    std::printf("  OrlibInit(NULL, 1) = 1\n");

    // Step 2: Confirm records loaded
    if (pGetNum) {
        __try {
            std::printf("  Strategies(0) = %d\n", pGetNum(0));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 3: Clean up leftover entries from prior test crashes
    std::printf("\n--- Step 3: Cleanup leftover entries ---\n");
    const char* cleanupNames[] = {"zzz_cli_test_probe", "test", NULL};
    for (int i = 0; cleanupNames[i]; i++) {
        if (pGetRecByName && pSetSigDeleted) {
            char rec[268]; memset(rec, 0, sizeof(rec));
            __try {
                BOOL got = pGetRecByName(cleanupNames[i], 0, rec);
                if (got) {
                    pSetSigDeleted(rec);
                    std::printf("  Deleted leftover: \"%s\"\n", cleanupNames[i]);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  Cleanup crash for \"%s\": 0x%X\n", cleanupNames[i], GetExceptionCode());
            }
        }
    }

    // Step 4: CreateEntryEX — use distinctive name in arg3 AND arg4 to determine which is the real name
    std::printf("\n--- Step 4: CreateEntryEX ---\n");
    const char* arg3name = "zzz_arg3_is_name";
    const char* arg4name = "zzz_arg4_is_name";
    int errCode = 0;
    short progId = -1;
    __try {
        progId = pCreateEntry(0, 0, arg3name, arg4name, 0, &errCode);
        std::printf("  CreateEntryEX(0, 0, \"%s\", \"%s\", 0, &err) = %d (err=%d)\n",
            arg3name, arg4name, (int)progId, errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CRASHED: 0x%X\n", GetExceptionCode());
        progId = -1;
    }

    if (progId < 0) {
        std::printf("  CreateEntryEX failed\n");
        if (pOrlibExit) pOrlibExit();
        FreeLibrary(g_hOrlib);
        return 7;
    }

    // Step 5: FindExistPos with both names to determine which is the real name
    std::printf("\n--- Step 5: FindExistPos with both names ---\n");
    for (int i = 0; i < 2; i++) {
        const char* name = (i == 0) ? arg3name : arg4name;
        int whType = -1;
        __try {
            int found = pFindExistPos(name, &whType);
            std::printf("  FindExistPos(\"%s\") = %d (whType=%d) %s\n",
                name, found, whType, found >= 0 ? "<-- THIS IS THE NAME ARG" : "");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  FindExistPos(\"%s\") CRASHED: 0x%X\n", name, GetExceptionCode());
        }
    }

    // Step 6: SaveModule_New with hidden EDIT control
    std::printf("\n--- Step 6: SaveModule_New with hidden EDIT ---\n");
    if (pSaveModule) {
        // Create a hidden EDIT control and set source text
        const char* testSource =
            "Inputs: Price(Close), Length(10);\n"
            "Variables: Avg(0);\n"
            "Avg = Average(Price, Length);\n"
            "Plot1(Avg, \"Avg\");\n";

        // Convert to wide string for SetWindowTextW
        wchar_t wSource[4096];
        MultiByteToWideChar(CP_UTF8, 0, testSource, -1, wSource, 4096);

        HWND hEdit = CreateWindowW(L"EDIT", wSource,
            WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 100, 100,
            NULL, NULL, GetModuleHandle(NULL), NULL);
        if (hEdit) {
            std::printf("  Hidden EDIT control created: hwnd=0x%p\n", (void*)hEdit);
            // Verify text was set
            int textLen = GetWindowTextLengthW(hEdit);
            std::printf("  Edit text length = %d\n", textLen);
        } else {
            std::printf("  FAILED to create EDIT control: %lu\n", GetLastError());
        }

        if (hEdit) {
            char saveBuffer[4096];
            memset(saveBuffer, 0, sizeof(saveBuffer));

            // SaveModule_New(WHType, ProgId, arg3, HWND, buffer)
            // Try arg3=NULL first
            int saveResult = -1;
            __try {
                saveResult = pSaveModule(0, progId, NULL, hEdit, saveBuffer);
                std::printf("  SaveModule_New(0, %d, NULL, hwnd, buf) = %d\n", (int)progId, saveResult);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  SaveModule_New CRASHED (arg3=NULL): 0x%X\n", GetExceptionCode());
            }

            // Try arg3=0
            if (saveResult < 0) {
                __try {
                    saveResult = pSaveModule(0, progId, (void*)0, hEdit, saveBuffer);
                    std::printf("  SaveModule_New(0, %d, 0, hwnd, buf) = %d\n", (int)progId, saveResult);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    std::printf("  SaveModule_New CRASHED (arg3=0): 0x%X\n", GetExceptionCode());
                }
            }

            DestroyWindow(hEdit);
        }
    } else {
        std::printf("  SaveModule_New not resolved\n");
    }

    // Step 7: Check if entry was persisted
    std::printf("\n--- Step 7: Check persistence ---\n");
    char ellPath[512];
    sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", (int)progId);
    FILE* f = fopen(ellPath, "rb");
    if (f) { std::printf("  FOUND: %s\n", ellPath); fclose(f); }
    else   { std::printf("  NOT FOUND: %s\n", ellPath); }

    // Step 8: Try verify pipeline
    std::printf("\n--- Step 8: Verify pipeline ---\n");
    if (pStartComp) {
        __try { pStartComp(); std::printf("  StartCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { std::printf("  StartComp CRASH: 0x%X\n", GetExceptionCode()); }
    }
    if (pVerify) {
        __try {
            int v = pVerify(0, 0, progId, 1, NULL);
            std::printf("  VerifyModule_New(0, 0, %d, 1, NULL) = %d\n", (int)progId, v);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  Verify CRASH: 0x%X\n", GetExceptionCode());
        }
    }
    if (pGetErrors) {
        int e[4] = {0,0,0,0};
        __try { pGetErrors(e); std::printf("  GetErrors: {%d,%d,%d,%d}\n", e[0],e[1],e[2],e[3]); }
        __except(EXCEPTION_EXECUTE_HANDLER) { std::printf("  GetErrors CRASH\n"); }
    }
    if (pGetErrLevel) {
        __try { std::printf("  GetErrorLevel(0) = %d\n", pGetErrLevel(0)); }
        __except(EXCEPTION_EXECUTE_HANDLER) { std::printf("  GetErrorLevel CRASH\n"); }
    }
    if (pEndComp) {
        __try { pEndComp(); std::printf("  EndCompilingProcess() OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { std::printf("  EndComp CRASH\n"); }
    }

    // Check persistence again after verify
    f = fopen(ellPath, "rb");
    if (f) { std::printf("  After verify, FOUND: %s\n", ellPath); fclose(f); }
    else   { std::printf("  After verify, NOT FOUND: %s\n", ellPath); }

    // Step 9: Cleanup
    std::printf("\n--- Step 9: Cleanup ---\n");
    if (pGetRecByName && pSetSigDeleted) {
        for (int i = 0; i < 2; i++) {
            const char* name = (i == 0) ? arg3name : arg4name;
            char rec[268]; memset(rec, 0, sizeof(rec));
            __try {
                BOOL got = pGetRecByName(name, 0, rec);
                if (got) {
                    pSetSigDeleted(rec);
                    std::printf("  Deleted: \"%s\"\n", name);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  Cleanup crash for \"%s\"\n", name);
            }
        }
    }

    // Step 10: OrlibExit
    std::printf("\n--- Step 10: OrlibExit ---\n");
    if (pOrlibExit) std::printf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    std::printf("\n=== elevated_test4 complete ===\n");
    return 0;
}
