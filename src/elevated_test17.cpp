// elevated_test17.cpp
//
// Read the module structure for different ProgIds to compare their fields.
// Specifically, read the subtype field and the flag at offset 0x79.
//
// We'll use GetMasterRecByName to get the record, then read fields from it.
// But GetMasterRecByName crashes for type != 0, so we'll use type=0 only.
//
// Alternative: Use FindExistPos to get the ProgId, then read the module
// structure directly from the warehouse.

#include <cstdio>
#include <cstring>
#include <windows.h>

typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
typedef BOOL (__stdcall *OrlibExit_t)(void);
typedef int (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
typedef int (__stdcall *GetNumofMasterRecsEX_t)(int whType);

// Internal function: load_module_from_warehouse (RVA 0x1CFF60)
// Called as: ecx=warehouse_obj, edx=type_struct, stack: (arg1, arg2, arg3)
// Returns: module pointer in EAX
typedef void* (__stdcall *LoadModule_t)(int arg1, int arg2, int arg3);

// Get warehouse object (RVA 0x1A9650)
typedef void* (__stdcall *GetWarehouse_t)(int arg);

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
    g_log = fopen("C:\\Users\\user\\projects\\tsdev_automation\\elevated_test17_log.txt", "w");
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

// Dump memory at a pointer, safely
void dumpMem(const char* label, void* ptr, int size) {
    logf("  %s at 0x%p:\n", label, ptr);
    if (!ptr) { logf("    NULL\n"); return; }
    __try {
        unsigned char* p = (unsigned char*)ptr;
        for (int i = 0; i < size; i += 16) {
            char hex[128] = "";
            for (int j = 0; j < 16 && i + j < size; j++) {
                char b[8];
                sprintf(b, "%02X ", p[i + j]);
                strcat(hex, b);
            }
            logf("    +%03X: %s\n", i, hex);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("    CRASHED reading memory\n");
    }
}

DWORD WINAPI probe_main(LPVOID) {
    logf("=== elevated_test17: Compare module structures ===\n");
    logf("sizeof(void*) = %zu\n", sizeof(void*));

    SetDllDirectoryW(kTsProgramDir);
    g_hOrlib = LoadLibraryW(L"orlib20.dll");
    if (!g_hOrlib) { logf("FATAL: LoadLibraryW: %lu\n", GetLastError()); return 4; }
    char* base = (char*)g_hOrlib;
    logf("orlib20.dll base = 0x%p\n", (void*)base);

    auto pOrlibInit = Resolve<OrlibInit_t>("_OrlibInit@8");
    auto pOrlibExit = Resolve<OrlibExit_t>("_OrlibExit@0");
    auto pFindExistPos = Resolve<FindExistPos_t>("FindExistPos");
    auto pGetNum = Resolve<GetNumofMasterRecsEX_t>("_GetNumofMasterRecsEX@4");

    if (!pOrlibInit || !pFindExistPos) {
        logf("FATAL: missing critical exports\n"); return 5;
    }

    // Step 1: OrlibInit
    logf("\n--- Step 1: OrlibInit ---\n");
    if (!pOrlibInit(NULL, 1)) { logf("FATAL: OrlibInit failed\n"); return 6; }
    logf("  OrlibInit(NULL, 1) = TRUE\n");

    // Step 2: Get warehouse object (internal function at 0x1A9650)
    // This is a thiscall: call with ecx=0 (or some context), returns warehouse obj
    logf("\n--- Step 2: Get warehouse object ---\n");
    // 0x1A9650 is called as: push 0; call 0x1A9650 (from VerifyModule_New)
    // It's a stdcall with 1 arg
    typedef void* (__stdcall *GetWh_t)(int);
    auto pGetWh = (GetWh_t)(base + 0x1A9650);
    void* whObj = NULL;
    __try {
        whObj = pGetWh(0);
        logf("  GetWarehouse(0) = 0x%p\n", whObj);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logf("  GetWarehouse CRASHED: 0x%X\n", GetExceptionCode());
    }

    // Step 3: Read the master record array
    // The warehouse object likely contains a pointer to the master records
    // Let's dump the first 256 bytes of the warehouse object
    if (whObj) {
        dumpMem("Warehouse object", whObj, 256);

        // Try to find the master record array pointer
        // From the disassembly, the warehouse object has records at various offsets
        // Let's read pointers at common offsets
        __try {
            void** whPtrs = (void**)whObj;
            for (int i = 0; i < 32; i++) {
                if (whPtrs[i] && !IsBadReadPtr(whPtrs[i], 16)) {
                    logf("  whObj[%d] (offset 0x%X) = 0x%p -> %02X %02X %02X %02X\n",
                        i, i * 4, whPtrs[i],
                        ((unsigned char*)whPtrs[i])[0],
                        ((unsigned char*)whPtrs[i])[1],
                        ((unsigned char*)whPtrs[i])[2],
                        ((unsigned char*)whPtrs[i])[3]);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 4: Find entries and read their records
    logf("\n--- Step 4: Find entries ---\n");
    struct { const char* name; int progId; } entries[] = {
        {"zzz_cli_test8", 2568},
        {"zzz_cli_test7", 2567},
        {"zzz_arg3_is_name", 2561},
        {NULL, 0}
    };

    for (int i = 0; entries[i].name; i++) {
        __try {
            int found = pFindExistPos(entries[i].name, 0);
            logf("  FindExistPos(\"%s\", 0) = %d (expected %d)\n",
                entries[i].name, found, entries[i].progId);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Step 5: Try to read module records via internal load function
    // load_module_from_warehouse (0x1CFF60) is a thiscall:
    //   ecx = warehouse_obj
    //   edx = type_struct
    //   stack: (arg1, arg2, arg3)
    // From VerifyModule_New:
    //   lea eax, [ebp - 0x128]  ; local object
    //   push eax                 ; arg3
    //   push edi                 ; arg2 (ProgId)
    //   lea eax, [ebp - 0x164]  ; type struct
    //   push eax                 ; arg1
    //   call 0x101cff60

    logf("\n--- Step 5: Load module records ---\n");

    // We need a type struct. From the disassembly, it's built by 0x1D43A0
    // Let's call 0x1D43A0 to build it
    // 0x1D43A0 is called as: push &typeStruct; push WHType; call 0x1D43A0
    typedef void (__stdcall *BuildType_t)(int whType, void* typeStruct);
    auto pBuildType = (BuildType_t)(base + 0x1D43A0);

    // load_module_from_warehouse
    // This is a thiscall with ecx=warehouse_obj, edx=typeStruct
    // and 3 stack args. But we can't easily do thiscall from C.
    // Let's use inline asm or a different approach.

    // Actually, let's try a simpler approach: read the master record directly
    // The master record for a ProgId should be at a fixed offset in the warehouse
    // Let's scan the warehouse object for our ProgIds

    if (whObj) {
        logf("\n--- Step 6: Scan warehouse for ProgIds ---\n");
        // Scan a large chunk of memory for our ProgId values
        __try {
            unsigned char* wh = (unsigned char*)whObj;
            int targets[] = {2561, 2564, 2565, 2566, 2567, 2568, 2569};
            for (int t = 0; t < 7; t++) {
                int target = targets[t];
                // Search first 4096 bytes of warehouse object
                for (int offset = 0; offset < 4096 - 4; offset++) {
                    int val = *(int*)(wh + offset);
                    if (val == target) {
                        logf("  ProgId %d found at whObj+0x%X\n", target, offset);
                        // Dump surrounding bytes
                        dumpMem("  context", wh + offset - 8, 32);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            logf("  Scan CRASHED: 0x%X\n", GetExceptionCode());
        }
    }

    // Step 7: OrlibExit
    logf("\n--- Step 7: OrlibExit ---\n");
    if (pOrlibExit) logf("  OrlibExit() = %d\n", pOrlibExit());

    FreeLibrary(g_hOrlib);
    logf("\n=== elevated_test17 complete ===\n");
    return 0;
}
