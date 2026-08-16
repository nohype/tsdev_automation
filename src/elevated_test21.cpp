// elevated_test21.cpp
//
// Delete flow probe v2: Fixed parameter order based on disassembly analysis.
//
// Key finding from RE: The actual parameter order of _GetMasterRecByName@12
// is (whType, name, outRecord), NOT (name, whType, outRecord).
// Similarly, FUN_101e3ac0 takes (whType, name) as stack args.
//
// Build: cmake --build build --config Release --target elevated_test21

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

// --- Typedefs for exported functions ---
typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int  (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int  (__stdcall *CreateEntryEX_t)(int whType, int secondary,
                                          const char* name, int param4,
                                          char param5, int* errCode);
typedef int  (__stdcall *SaveModule_New_t)(int warehouseType, int progId,
                                           void* arg3, HWND editorHwnd,
                                           void* arg5);
typedef int  (__stdcall *VerifyModule_New_t)(HWND hwnd, int warehouseType,
                                             int progId, int flag,
                                             void* compilerObj);
typedef void (__stdcall *GetCompilingErrors_t)(char* buffer);
typedef void (__stdcall *CloseWarehouse_t)(int param);
typedef void (__stdcall *SetSigDeleted_t)(void* record);
typedef BOOL (__stdcall *GetMasterRecByName_t)(int whType, const char* name,
                                               void* outRecord);
typedef int  (__stdcall *CleanMastersEX_t)(int param);

// --- Typedefs for internal functions (by RVA) ---
typedef void* (__cdecl *GetWarehouse_t)(int arg);
typedef void* (__cdecl *InitGlobalLookup_t)(void);

// Internal function RVAs
const DWORD_PTR kRvaGetWarehouse     = 0x1A9650;
const DWORD_PTR kRvaInitGlobalLookup = 0x1A9460;
const DWORD_PTR kRvaFindRecordDirect = 0x1E3AC0;
const DWORD_PTR kRvaFindRecordThread = 0x1D16C0;
const DWORD_PTR kRvaDeleteFromTree   = 0x1E4030;  // FUN_101E4030: removes record from lookup tree

// Internal function RVAs for CELCompiler
const DWORD_PTR kRvaStartCompiling    = 0x165de0;
const DWORD_PTR kRvaEndCompiling      = 0x165ed0;
const DWORD_PTR kRvaCELCompilerCtor   = 0x165a00;
const DWORD_PTR kRvaCELCompilerDtor   = 0x165ac0;
const size_t kCELCompilerSize = 0x58;

static const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
static const char* kMyWorkBin =
    "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

static HMODULE g_hOrlib = NULL;
static DWORD_PTR g_orlibBase = 0;

template<typename T>
T Resolve(const char* name) {
    T p = (T)GetProcAddress(g_hOrlib, name);
    if (!p) printf("  FAILED to resolve \"%s\"\n", name);
    return p;
}

template<typename T>
T ResolveRva(DWORD_PTR rva) {
    return reinterpret_cast<T>(g_orlibBase + rva);
}

// Persistence patch
static const DWORD_PTR kRvaPersistencePatch = 0x11230;
static const unsigned char kPatchBytes[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00
};

bool ApplyPatch() {
    void* addr = reinterpret_cast<void*>(g_orlibBase + kRvaPersistencePatch);
    DWORD oldProtect = 0;
    if (!VirtualProtect(addr, sizeof(kPatchBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    memcpy(addr, kPatchBytes, sizeof(kPatchBytes));
    DWORD dummy = 0;
    VirtualProtect(addr, sizeof(kPatchBytes), oldProtect, &dummy);
    return true;
}

void PreloadDlls() {
    const wchar_t* dlls[] = {
        L"ECL.dll", L"ORELUI20.dll", L"orUI20.dll", L"elobjmod.dll",
        L"elwinforms.dll", L"tschartxelobjmdl.dll", L"ELDebugger.dll"
    };
    for (const wchar_t* dll : dlls) LoadLibraryW(dll);
}

HWND CreateHiddenEdit() {
    HWND hParent = CreateWindowW(L"STATIC", L"", WS_POPUP,
                                 0, 0, 100, 100, HWND_MESSAGE,
                                 nullptr, nullptr, nullptr);
    HWND hEdit = CreateWindowW(L"EDIT", L"",
                               WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
                               0, 0, 100, 100, hParent,
                               nullptr, nullptr, nullptr);
    if (!hEdit) {
        hEdit = CreateWindowW(L"EDIT", L"",
                              ES_MULTILINE | ES_AUTOVSCROLL,
                              0, 0, 100, 100, nullptr,
                              nullptr, nullptr, nullptr);
    }
    return hEdit;
}

void SetSourceText(HWND hwnd, const char* source) {
    int len = MultiByteToWideChar(CP_ACP, 0, source, -1, nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, source, -1, &wide[0], len);
    SetWindowTextW(hwnd, wide.c_str());
}

// SEH-protected call to FindRecordDirect
// __thiscall: ecx=globalObj, stack args: (whType, name) — RET 0x8
// In x86 push order: push name first (higher addr → [EBP+0xc]), push whType second (lower addr → [EBP+0x8])
#pragma warning(push)
#pragma warning(disable: 4731)
bool CallFindRecordDirect(void* fnPtr, void* thisPtr, int whType, const char* name, void** outRecord) {
    *outRecord = nullptr;
    __try {
        void* result = nullptr;
        __asm {
            push name       // → [callee EBP + 0xc] = name
            push whType     // → [callee EBP + 0x8] = whType
            mov ecx, thisPtr
            call fnPtr
            mov result, eax
        }
        *outRecord = result;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("  CallFindRecordDirect CRASHED: 0x%X\n", GetExceptionCode());
        return false;
    }
}
#pragma warning(pop)

// SEH-protected call to FindRecordThreaded
// __thiscall: ecx=warehouse, stack args: (whType, name, &outRecord) — RET 0xC
#pragma warning(push)
#pragma warning(disable: 4731)
bool CallFindRecordThreaded(void* fnPtr, void* thisPtr, int whType, const char* name, void** outRecord) {
    *outRecord = nullptr;
    int result = 0;
    __try {
        __asm {
            push outRecord   // → [callee EBP + 0x10]
            push name        // → [callee EBP + 0xc] = name
            push whType      // → [callee EBP + 0x8] = whType
            mov ecx, thisPtr
            call fnPtr
            mov result, eax
        }
        return (result == 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("  CallFindRecordThreaded CRASHED: 0x%X\n", GetExceptionCode());
        return false;
    }
}
#pragma warning(pop)

// SEH-protected call to DeleteFromTree (FUN_101E4030)
// __thiscall: ecx=globalLookup, stack args: (warehouseType, progId, flag) — RET 0xC
// In x86 push order: push flag first (higher addr → [EBP+0x10]), push progId, push warehouseType
#pragma warning(push)
#pragma warning(disable: 4731)
bool CallDeleteFromTree(void* fnPtr, void* thisPtr, int warehouseType, int progId, int flag) {
    __try {
        __asm {
            push flag           // → [callee EBP + 0x10]
            push progId         // → [callee EBP + 0xc]
            push warehouseType  // → [callee EBP + 0x8]
            mov ecx, thisPtr
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("  CallDeleteFromTree CRASHED: 0x%X\n", GetExceptionCode());
        return false;
    }
}
#pragma warning(pop)

// SEH-protected compile/verify (simplified from tsdev_lib.cpp)
bool CompileAndVerify(HWND hEdit, int whType, int progId) {
    void* compilerMem = malloc(kCELCompilerSize);
    if (!compilerMem) return false;
    memset(compilerMem, 0, kCELCompilerSize);

    // CELCompiler constructor (fastcall: ecx=this)
    auto pCtor = ResolveRva<void (__fastcall *)(void*)>(kRvaCELCompilerCtor);
    auto pDtor = ResolveRva<void (__fastcall *)(void*)>(kRvaCELCompilerDtor);
    auto pStart = ResolveRva<void (__cdecl *)(void)>(kRvaStartCompiling);
    auto pEnd = ResolveRva<void (__fastcall *)(int)>(kRvaEndCompiling);
    auto pVerify = Resolve<VerifyModule_New_t>("_VerifyModule_New@20");
    auto pGetErrors = Resolve<GetCompilingErrors_t>("_GetCompilingErrors@4");

    __try { pCtor(compilerMem); } __except(EXCEPTION_EXECUTE_HANDLER) {
        free(compilerMem); return false;
    }

    *(int*)((char*)compilerMem + 0x10) = 0xa02;
    *(HWND*)((char*)compilerMem + 0x18) = hEdit;
    *(int*)((char*)compilerMem + 0x28) = 6;

    __try { pStart(); } __except(EXCEPTION_EXECUTE_HANDLER) {
        pDtor(compilerMem); free(compilerMem); return false;
    }

    int verifyResult = 0;
    char errBuf[4096] = {0};
    __try {
        verifyResult = pVerify(hEdit, whType, progId, 1, compilerMem);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        pEnd(0); pDtor(compilerMem); free(compilerMem); return false;
    }
    __try { pGetErrors(errBuf); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    pEnd(0);
    pDtor(compilerMem);
    free(compilerMem);

    if (errBuf[0] != '\0') {
        printf("  Compile errors: %s\n", errBuf);
        return false;
    }
    printf("  VerifyModule_New = %d\n", verifyResult);
    return (verifyResult == 1);
}

DWORD WINAPI probe_main(LPVOID) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== elevated_test21: Delete flow probe v2 ===\n\n");

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { printf("FATAL: LoadLibraryW failed: %lu\n", GetLastError()); return 1; }
    g_orlibBase = (DWORD_PTR)g_hOrlib;
    printf("orlib20.dll loaded at 0x%p\n", g_hOrlib);

    PreloadDlls();
    if (!ApplyPatch()) { printf("FATAL: patch failed\n"); return 1; }
    printf("Persistence patch applied\n");

    auto pOrlibInit    = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit    = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pCreateEntry  = Resolve<CreateEntryEX_t>("_CreateEntryEX@24");
    auto pSaveModule   = Resolve<SaveModule_New_t>("_SaveModule_New@20");
    auto pCloseWh      = Resolve<CloseWarehouse_t>("_CloseWarehouseForCurrentThreadAndPath@4");
    auto pSetSigDel    = Resolve<SetSigDeleted_t>("_SetSigDeleted@4");
    auto pGetRecByName = Resolve<GetMasterRecByName_t>("_GetMasterRecByName@12");
    auto pCleanMasters = Resolve<CleanMastersEX_t>("_CleanMastersEX@4");

    auto pGetWarehouse     = ResolveRva<GetWarehouse_t>(kRvaGetWarehouse);
    auto pInitGlobalLookup = ResolveRva<InitGlobalLookup_t>(kRvaInitGlobalLookup);
    void* pFindRecordDirect = ResolveRva<void*>(kRvaFindRecordDirect);
    void* pFindRecordThread = ResolveRva<void*>(kRvaFindRecordThread);

    if (!pOrlibInit || !pOrlibExit || !pFindExistPos || !pCreateEntry ||
        !pSaveModule || !pCloseWh) {
        printf("FATAL: missing required exports\n"); return 1;
    }

    if (!pOrlibInit(NULL, 1)) { printf("FATAL: OrlibInit failed\n"); return 1; }
    printf("OrlibInit(NULL, 1) = TRUE\n");

    HWND hEdit = CreateHiddenEdit();
    if (!hEdit) { printf("FATAL: no EDIT control\n"); pOrlibExit(); return 1; }

    // Create test strategy
    const char* testName = "zzz_delete_probe";
    const char* testSource = "if Close > Open then Buy next bar at market;\n";

    printf("\n--- Creating test strategy \"%s\" ---\n", testName);

    int progId = pFindExistPos(testName, 0);
    if (progId <= 0) {
        int errCode = 0;
        progId = pCreateEntry(0, 0, testName, 0, 0, &errCode);
        printf("  CreateEntryEX = %d (err=%d)\n", progId, errCode);
        if (progId <= 0) { printf("FATAL: CreateEntryEX failed\n"); pOrlibExit(); return 1; }
    } else {
        printf("  Already exists: ProgId=%d\n", progId);
    }

    SetSourceText(hEdit, testSource);
    printf("  SaveModule_New = %d\n", pSaveModule(0, progId, NULL, hEdit, NULL));

    // Full compile/verify to create .ell file
    printf("  Compiling...\n");
    if (!CompileAndVerify(hEdit, 0, progId)) {
        printf("  WARNING: compile failed, .ell may not exist\n");
    }

    printf("  FindExistPos = %d\n", pFindExistPos(testName, 0));

    char ellPath[512];
    snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_0_%d.ell", kMyWorkBin, progId);
    DWORD attr = GetFileAttributesA(ellPath);
    printf("  .ell file: %s\n", (attr != INVALID_FILE_ATTRIBUTES) ? "EXISTS" : "NOT FOUND");

    // === APPROACH: Modify the original master table entry directly ===
    // Key insight from RE:
    // - FindExistPos → GetWarehouse → FindRecordDirect/Threaded
    // - FindRecordDirect creates a 0x241-byte record COPY, not the original
    // - record+0xC points to the original master table entry (in shared memory)
    // - SetSigDeleted modifies the COPY (offsets 0x79, 0xD8), not the original
    // - To persist deletion, we must modify the ORIGINAL master table entry
    // - The master table is in shared memory (file mapping) that persists across OrlibExit/Init
    // - CleanMastersEX checks masterTableEntry+0x79 & 1 and masterTableEntry+0xD8 & 2
    //   If 0x79 bit 0 is set AND 0xD8 bit 1 is NOT set, the record is considered deleted
    
    // Step 1: Get warehouse and find record
    printf("\n--- Step 1: Get record pointer ---\n");
    void* warehouse = nullptr;
    __try { warehouse = pGetWarehouse(0); } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  GetWarehouse CRASHED\n");
    }
    printf("  Warehouse: 0x%p\n", warehouse);
    
    if (!warehouse) {
        printf("FATAL: no warehouse\n"); pOrlibExit(); return 1;
    }
    
    int whMode = *(int*)((char*)warehouse + 0xC2);
    printf("  Warehouse + 0xC2 = %d (mode)\n", whMode);
    printf("  Warehouse + 0xAE = 0x%p (master table)\n", *(void**)((char*)warehouse + 0xAE));
    printf("  Warehouse + 0x5E = 0x%p (current entry)\n", *(void**)((char*)warehouse + 0x5E));
    
    void* recordPtr = nullptr;
    if (whMode == 1) {
        void* globalObj = nullptr;
        __try { globalObj = pInitGlobalLookup(); } __except(EXCEPTION_EXECUTE_HANDLER) {
            printf("  InitGlobalLookup CRASHED\n");
        }
        printf("  Global lookup: 0x%p\n", globalObj);
        if (globalObj) {
            CallFindRecordDirect(pFindRecordDirect, globalObj, 0, testName, &recordPtr);
        }
    } else {
        CallFindRecordThreaded(pFindRecordThread, warehouse, 0, testName, &recordPtr);
    }
    printf("  Record pointer: 0x%p\n", recordPtr);
    
    if (!recordPtr) {
        printf("FATAL: could not find record\n"); pOrlibExit(); return 1;
    }
    
    printf("  Record + 0x79 = 0x%02X (deleted flag)\n",
           *(unsigned char*)((char*)recordPtr + 0x79));
    
    // Get the master table entry pointer from the record
    // record+0xC = warehouse pointer, record+0x10 = master table entry (warehouse+0x5E)
    void* masterEntry = *(void**)((char*)recordPtr + 0x10);
    printf("  Record + 0x10 = 0x%p (master table entry)\n", masterEntry);
    
    if (masterEntry) {
        printf("  Master entry + 0x79 = 0x%02X\n",
               *(unsigned char*)((char*)masterEntry + 0x79));
        printf("  Master entry + 0xD8 = 0x%04X\n",
               *(unsigned short*)((char*)masterEntry + 0xD8));
        // Dump all 0x10A bytes of master entry to find name field
        printf("  Master entry full dump (0x10A bytes):\n");
        for (int i = 0; i < 0x10A; i += 16) {
            printf("  +0x%03X: ", i);
            for (int j = 0; j < 16 && (i+j) < 0x10A; j++) {
                printf("%02X ", *(unsigned char*)((char*)masterEntry + i + j));
            }
            // Also print as ASCII
            printf(" |");
            for (int j = 0; j < 16 && (i+j) < 0x10A; j++) {
                unsigned char c = *(unsigned char*)((char*)masterEntry + i + j);
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("|\n");
        }
    }
    
    // Step 2: Modify the ORIGINAL master table entry
    printf("\n--- Step 2: Modify master table entry ---\n");
    if (masterEntry) {
        // Set deleted flag: byte+0x79 bit 0 = 1, word+0xD8 bit 1 = 0
        unsigned char* p79 = (unsigned char*)((char*)masterEntry + 0x79);
        unsigned short* pD8 = (unsigned short*)((char*)masterEntry + 0xD8);
        printf("  Before: +0x79=0x%02X, +0xD8=0x%04X\n", *p79, *pD8);
        *p79 |= 1;          // set deleted bit
        *pD8 &= 0xFFFD;     // clear bit 1
        printf("  After:  +0x79=0x%02X, +0xD8=0x%04X\n", *p79, *pD8);
        printf("  Master table entry modified!\n");
    }
    
    // Also call SetSigDeleted on the record copy (for consistency)
    printf("\n--- Step 2b: SetSigDeleted on record copy ---\n");
    __try {
        pSetSigDel(recordPtr);
        printf("  SetSigDeleted succeeded\n");
        printf("  Record +0x79 = 0x%02X\n",
               *(unsigned char*)((char*)recordPtr + 0x79));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  SetSigDeleted CRASHED: 0x%X\n", GetExceptionCode());
    }
    
    // Step 2c: Remove from global lookup tree
    printf("\n--- Step 2c: DeleteFromTree ---\n");
    if (whMode == 1) {
        void* globalObj = nullptr;
        __try { globalObj = pInitGlobalLookup(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (globalObj) {
            void* pDeleteFromTree = ResolveRva<void*>(kRvaDeleteFromTree);
            bool ok = CallDeleteFromTree(pDeleteFromTree, globalObj, 0, progId, 0);
            printf("  DeleteFromTree succeeded = %d\n", ok);
        }
    }
    
    // Step 2d: Check FindExistPos WITHOUT close/reopen
    printf("\n--- Step 2d: Check FindExistPos (no close/reopen) ---\n");
    int check1 = pFindExistPos(testName, 0);
    printf("  FindExistPos(\"%s\", 0) = %d\n", testName, check1);
    if (check1 <= 0) {
        printf("  >>> Deletion works in-memory! Issue is close/reopen losing modification.\n");
    } else {
        printf("  >>> Lookup still finds it. Checking if master entry mod persisted...\n");
        if (masterEntry) {
            printf("  Master entry +0x79 = 0x%02X (should be 0x01)\n",
                   *(unsigned char*)((char*)masterEntry + 0x79));
        }
    }
    
    // Step 2e: Try CleanMastersEX while warehouse is loaded
    printf("\n--- Step 2e: CleanMastersEX (warehouse loaded) ---\n");
    if (pCleanMasters) {
        __try {
            int result = pCleanMasters(0);
            printf("  CleanMastersEX(0) = %d\n", result);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            printf("  CleanMastersEX CRASHED: 0x%X\n", GetExceptionCode());
        }
    }
    int check2 = pFindExistPos(testName, 0);
    printf("  FindExistPos after CleanMastersEX = %d\n", check2);
    
    // Step 3: Delete .ell and .xml files
    printf("\n--- Step 3: Delete files ---\n");
    char xmlPath[512];
    snprintf(xmlPath, sizeof(xmlPath), "%s\\elmodule_0_%d.xml", kMyWorkBin, progId);
    printf("  DeleteFile(.ell) = %d\n", DeleteFileA(ellPath));
    printf("  DeleteFile(.xml) = %d\n", DeleteFileA(xmlPath));
    
    // Step 4: CloseWarehouse + OrlibInit
    printf("\n--- Step 4: CloseWarehouse + OrlibInit ---\n");
    __try {
        pCloseWh(0);
        printf("  CloseWarehouse succeeded\n");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  CloseWarehouse CRASHED\n");
    }
    __try {
        BOOL initResult = pOrlibInit(NULL, 1);
        printf("  OrlibInit(NULL, 1) = %d\n", initResult);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  OrlibInit CRASHED\n");
    }
    
    // Step 5: Verify
    printf("\n--- Step 5: Verification ---\n");
    int postDelete = pFindExistPos(testName, 0);
    printf("  FindExistPos(\"%s\", 0) = %d\n", testName, postDelete);
    if (postDelete <= 0) {
        printf("  SUCCESS: Component deleted!\n");
    } else {
        printf("  Component still exists (ProgId=%d)\n", postDelete);
    }

    printf("\n--- OrlibExit ---\n");
    __try { printf("  OrlibExit() = %d\n", pOrlibExit()); }
    __except(EXCEPTION_EXECUTE_HANDLER) { printf("  CRASHED\n"); }

    printf("\n=== DONE ===\n");
    return 0;
}

int main() {
    HANDLE hThread = CreateThread(NULL, 64 * 1024 * 1024,
        (LPTHREAD_START_ROUTINE)probe_main, NULL, 0, NULL);
    if (!hThread) { printf("FATAL: CreateThread failed\n"); return 1; }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    return (int)exitCode;
}
