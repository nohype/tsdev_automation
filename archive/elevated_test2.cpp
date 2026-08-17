// elevated_test2.cpp
//
// Comprehensive elevated test: enumerate master records, test FindExistPos
// with real names, test CreateEntryEX + full INSERT pipeline.
//
// Build: cmake --build build --config Release --target elevated_test2
// Run (MUST be elevated): build\Release\elevated_test2.exe

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__cdecl *FindExistPos_t)(const char* name, int* whType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);
typedef BOOL (__stdcall *GetMasterRecsPtr_t)(int whType, void** outPtr);
typedef BOOL (__stdcall *GetMasterRecByType_t)(int whType, int index, void* outRec, int recSize);
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

// Master record structure — based on prior RE, records are 266 bytes.
// The name is typically at the start of the record (null-terminated string).
// We'll dump the first 64 bytes of each record to find the name.
struct MasterRec {
    char data[266];
};

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev;
        DWORD cbSize = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cbSize))
            fIsElevated = elev.TokenIsElevated;
        CloseHandle(hToken);
    }
    std::printf("=== elevated_test2: comprehensive orlib20 test ===\n");
    std::printf("Process is %s\n", fIsElevated ? "ELEVATED" : "NOT ELEVATED");
    std::printf("sizeof(void*) = %zu\n", sizeof(void*));
    if (sizeof(void*) != 4) { std::printf("FATAL: not 32-bit\n"); return 2; }

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { std::printf("FATAL: LoadLibraryW failed: %lu\n", GetLastError()); return 4; }
    std::printf("orlib20.dll loaded at 0x%p\n", (void*)g_hOrlib);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");
    auto pGetRecsPtr = Resolve<GetMasterRecsPtr_t>("_GetMasterRecsPtr@8");
    auto pGetRecByType = Resolve<GetMasterRecByType_t>("_GetMasterRecByType@16");
    auto pCreateEntry = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveText = Resolve<SaveTextEX_t>("_SaveTextEX@16");
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pStartComp = Resolve<StartCompilingProcess_t>("StartCompilingProcess");
    auto pEndComp = Resolve<EndCompilingProcess_t>("EndCompilingProcess");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");
    auto pGetErrLevel = Resolve<GetErrorLevel_t>("_GetErrorLevel@4");
    auto pSetSigDeleted = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");

    if (!pOrlibInit || !pFindExistPos || !pGetNum || !pCreateEntry) {
        std::printf("FATAL: missing required exports\n");
        return 5;
    }

    // Step 1: OrlibInit
    std::printf("\n--- Step 1: OrlibInit(NULL, 1) ---\n");
    BOOL initOk = pOrlibInit(NULL, 1);
    std::printf("  OrlibInit(NULL, 1) = %d\n", initOk);
    if (!initOk) { std::printf("FATAL: OrlibInit failed\n"); return 6; }

    // Step 2: Master record counts
    std::printf("\n--- Step 2: Master Record Counts ---\n");
    int counts[5] = {0};
    const char* typeNames[] = {"Strategy", "Function", "Indicator", "Type3", "Type4"};
    for (int wt = 0; wt <= 4; wt++) {
        __try {
            counts[wt] = pGetNum(wt);
            std::printf("  GetNumofMasterRecsEX(%d=%s) = %d\n", wt, typeNames[wt], counts[wt]);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  GetNumofMasterRecsEX(%d) CRASHED: 0x%X\n", wt, GetExceptionCode());
        }
    }

    // Step 3: Enumerate records to find real names
    std::printf("\n--- Step 3: Enumerate master records ---\n");
    char foundNames[10][64];
    int foundProgIds[10];
    int foundWhTypes[10];
    int nFound = 0;

    for (int wt = 0; wt <= 4 && nFound < 10; wt++) {
        if (counts[wt] <= 0) continue;

        // Try GetMasterRecsPtr to get the array pointer
        void* recsPtr = NULL;
        __try {
            BOOL ok = pGetRecsPtr(wt, &recsPtr);
            std::printf("  GetMasterRecsPtr(%d) = %d, ptr=0x%p\n", wt, ok, recsPtr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  GetMasterRecsPtr(%d) CRASHED: 0x%X\n", wt, GetExceptionCode());
            recsPtr = NULL;
        }

        if (recsPtr) {
            // The records are likely an array of 266-byte structs.
            // The name is typically at offset 0 (null-terminated string).
            // Try reading the first few records.
            MasterRec* recs = (MasterRec*)recsPtr;
            int maxShow = counts[wt] < 5 ? counts[wt] : 5;
            for (int i = 0; i < maxShow && nFound < 10; i++) {
                char* name = recs[i].data;
                // Make sure it's a valid string (printable ASCII)
                bool valid = true;
                for (int j = 0; j < 63 && name[j]; j++) {
                    if (name[j] < 32 || name[j] > 126) { valid = false; break; }
                }
                if (valid && name[0] != 0) {
                    std::printf("  [%s %d]: name=\"%s\"\n", typeNames[wt], i, name);
                    if (nFound < 10) {
                        strncpy(foundNames[nFound], name, 63);
                        foundNames[nFound][63] = 0;
                        foundWhTypes[nFound] = wt;
                        // ProgId might be at a different offset — try offset 64 or 128
                        // For now, use the index as a tentative ProgId
                        foundProgIds[nFound] = i;
                        nFound++;
                    }
                } else {
                    // Dump first 16 bytes as hex for debugging
                    std::printf("  [%s %d]: (binary) ", typeNames[wt], i);
                    for (int j = 0; j < 16; j++) std::printf("%02X ", (unsigned char)recs[i].data[j]);
                    std::printf("\n");
                }
            }
        }

        // Also try GetMasterRecByType
        if (pGetRecByType) {
            std::printf("  Trying GetMasterRecByType(%d, 0, ...)...\n", wt);
            MasterRec rec;
            memset(&rec, 0, sizeof(rec));
            __try {
                BOOL ok = pGetRecByType(wt, 0, &rec, sizeof(rec));
                if (ok) {
                    bool valid = true;
                    for (int j = 0; j < 63 && rec.data[j]; j++) {
                        if (rec.data[j] < 32 || rec.data[j] > 126) { valid = false; break; }
                    }
                    if (valid && rec.data[0] != 0) {
                        std::printf("    name=\"%s\"\n", rec.data);
                    } else {
                        std::printf("    (binary) ");
                        for (int j = 0; j < 16; j++) std::printf("%02X ", (unsigned char)rec.data[j]);
                        std::printf("\n");
                    }
                } else {
                    std::printf("    returned FALSE\n");
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("    CRASHED: 0x%X\n", GetExceptionCode());
            }
        }
    }

    // Step 4: Test FindExistPos with found names
    std::printf("\n--- Step 4: FindExistPos with found names ---\n");
    for (int i = 0; i < nFound; i++) {
        int whType = -1;
        __try {
            int progId = pFindExistPos(foundNames[i], &whType);
            std::printf("  FindExistPos(\"%s\") = %d (whType=%d)\n", foundNames[i], progId, whType);
            if (progId >= 0) {
                foundProgIds[i] = progId;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  FindExistPos(\"%s\") CRASHED: 0x%X\n", foundNames[i], GetExceptionCode());
        }
    }

    // Step 5: Test CreateEntryEX
    std::printf("\n--- Step 5: CreateEntryEX ---\n");
    int errCode = 0;
    short newProgId = -1;
    const char* testName = "zzz_cli_test_probe";
    __try {
        // (WHType=0=Strategy, subType=0, string2="test", string1=name, flags=0, &errCode)
        newProgId = pCreateEntry(0, 0, "test", testName, 0, &errCode);
        std::printf("  CreateEntryEX(0, 0, \"test\", \"%s\", 0, &err) = %d (errCode=%d)\n",
            testName, newProgId, errCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  CreateEntryEX CRASHED: 0x%X\n", GetExceptionCode());
        newProgId = -1;
    }

    // Step 6: If CreateEntryEX succeeded, test full INSERT pipeline
    if (newProgId >= 0) {
        std::printf("\n--- Step 6: Full INSERT pipeline ---\n");

        // 6a: SaveTextEX (direct call — the headless path)
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
            std::printf("  SaveTextEX(source, 0, %d, buffer) = %d\n", newProgId, saveResult);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::printf("  SaveTextEX CRASHED: 0x%X\n", GetExceptionCode());
        }

        // 6b: StartCompilingProcess
        if (pStartComp) {
            __try {
                pStartComp();
                std::printf("  StartCompilingProcess() -> OK\n");
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  StartCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
            }
        }

        // 6c: VerifyModule_New
        if (pVerify) {
            int verifyResult = -1;
            __try {
                verifyResult = pVerify(0, 0, newProgId, 1, NULL);
                std::printf("  VerifyModule_New(0, 0, %d, 1, NULL) = %d\n", newProgId, verifyResult);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  VerifyModule_New CRASHED: 0x%X\n", GetExceptionCode());
            }

            // 6d: Check for compile errors
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

        // 6e: EndCompilingProcess
        if (pEndComp) {
            __try {
                pEndComp();
                std::printf("  EndCompilingProcess() -> OK\n");
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  EndCompilingProcess CRASHED: 0x%X\n", GetExceptionCode());
            }
        }

        // 6f: Check if entry was persisted (look for new elmodule file)
        std::printf("\n  Checking for new elmodule files...\n");
        char ellPath[512];
        sprintf(ellPath, "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin\\elmodule_0_%d.ell", newProgId);
        FILE* f = fopen(ellPath, "rb");
        if (f) {
            std::printf("  FOUND: %s (entry was persisted!)\n", ellPath);
            fclose(f);
        } else {
            std::printf("  NOT FOUND: %s (entry not persisted yet — may need SaveModule_New)\n", ellPath);
        }

        // Step 7: CLEANUP — delete the test entry
        std::printf("\n--- Step 7: Cleanup (delete test entry) ---\n");
        if (pGetRecByName && pSetSigDeleted) {
            MasterRec rec;
            memset(&rec, 0, sizeof(rec));
            __try {
                BOOL gotRec = pGetRecByName(testName, 0, &rec);
                std::printf("  GetMasterRecByName(\"%s\", 0, &rec) = %d\n", testName, gotRec);
                if (gotRec) {
                    pSetSigDeleted(&rec);
                    std::printf("  SetSigDeleted(&rec) called\n");
                    std::printf("  Test entry deleted.\n");
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::printf("  Cleanup CRASHED: 0x%X\n", GetExceptionCode());
            }
        } else {
            std::printf("  WARNING: Cannot clean up — GetMasterRecByName or SetSigDeleted not resolved\n");
            std::printf("  Manual cleanup needed: delete elmodule_0_%d.ell/.xml from MyWork\\Bin\n", newProgId);
        }

        // Verify cleanup
        FILE* f2 = fopen(ellPath, "rb");
        if (f2) {
            std::printf("  WARNING: %s still exists after cleanup!\n", ellPath);
            fclose(f2);
        } else {
            std::printf("  Verified: %s no longer exists\n", ellPath);
        }
    } else {
        std::printf("\n  CreateEntryEX failed — skipping INSERT pipeline test.\n");
    }

    // Step 8: OrlibExit
    std::printf("\n--- Step 8: OrlibExit ---\n");
    if (pOrlibExit) {
        BOOL exitOk = pOrlibExit();
        std::printf("  OrlibExit() = %d\n", exitOk);
    }

    FreeLibrary(g_hOrlib);
    std::printf("\n=== elevated_test2 complete ===\n");
    return 0;
}
