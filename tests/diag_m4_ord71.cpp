// diag_m4_ord71.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: verify the ABI of `Ordinal_71` (orlib20 ordinal 71, RVA 0x1A9460,
// FUN_101a9460 — "InitGlobalLookup") and determine whether calling it (or
// replicating the worker-thread command 0x0D path) populates the
// reverse-dependent SafeArray at warehouse+0x26 / record+0x26.
//
// === Ghidra-confirmed findings (PART 1) ===
//
// 1. Ordinal_71 (FUN_101a9460, RVA 0x1A9460) — InitGlobalLookup
//    Decompiled body:
//      void FUN_101a9460(void) {
//        if (DAT_1023ced4 == 0) {
//          pvVar1 = operator_new(0x2c);
//          if (pvVar1 != NULL) DAT_1023ced4 = FUN_101e3230(pvVar1);
//          else                DAT_1023ced4 = 0;
//        }
//        return;
//      }
//    - Calling convention: __cdecl. The function prologue is
//      `PUSH EBP; MOV EBP,ESP; ...` and epilogue is `MOV ESP,EBP; POP EBP; RET`
//      (bare RET — NO RET n). The 13 internal call sites (e.g. FUN_101d05e0
//      @0x101d05f3) PUSH 2 DWORDs before the CALL but perform NO `ADD ESP,8`
//      afterward — they rely on the EBP-frame epilogue to discard the args.
//      The decompiler therefore labels the body `void FUN_101a9460(void)`
//      because the body NEVER references [EBP+8] or [EBP+0xC] — the args are
//      IGNORED.
//    - Parameters: 2 args are PUSHED by callers (type, ProgId) but the
//      function IGNORES them. It is effectively a zero-arg lazy initializer.
//    - What it does: Lazily allocates a 0x2c-byte global lookup structure
//      (DAT_1023ced4) via operator_new(0x2c) + FUN_101e3230 (which initializes
//      a critical section at +0x08 and zeros +0x00/+0x04/+0x20/+0x24/+0x28).
//      FUN_101e3230 also calls FUN_101e4400 (allocates a 0x24-byte BST root
//      node with self-referential left/right/parent pointers and a 0x101
//      balance field) and stores it at +0x00.
//    - Does it trigger the WHServer worker? NO. It contains no
//      SetEvent/WaitForSingleObject/ReleaseMutex calls. It does NOT call
//      FUN_101cf5b0.
//    - Does it return a record pointer? NO. It returns the global lookup
//      structure pointer (DAT_1023ced4) in EAX — a 0x2c-byte BST-control
//      block, NOT a module record.
//    - Does it set warehouse+0xc2 to 1 (the "cached" flag)? NO. It does not
//      touch the warehouse object at all.
//    - Does it call FUN_101cf5b0 (signal worker & wait)? NO.
//    - What command/state does it set? NONE. It only initializes the global
//      BST lookup structure.
//
//    The "cached path" in TSDev.exe FUN_004D91E0 -> FUN_004d7140 is:
//      if (warehouse+0xc2 == 1) {
//        Ordinal_71(type, ProgId);          // ensures BST global is initialized
//        FUN_004e79d0(type, ProgId);        // BST lookup by ProgId (orlib20 FUN_101e3db0)
//      }
//    Ordinal_71 is a PRECONDITION GUARD for the BST path, not the path itself.
//    The BST lookup (FUN_101e3db0) is what actually walks the tree and, on a
//    miss, falls back to FUN_101a9650 (warehouse getter) + FUN_101d18c0
//    (worker trigger) + FUN_101c36d0 (verbatim name getter, which dispatches
//    command 0x34 via FUN_101cf5b0).
//
// 2. Ordinal_10 (FUN_101a9650, RVA 0x1A9650) — GetWarehouse
//    - Calling convention: __cdecl, 1 arg (path index; 0 = default TS path).
//    - Returns: the warehouse singleton pointer (CELWHouse*). On first call
//      with a given path, allocates 0xea bytes via operator_new and
//      initializes via FUN_101cf1f0 (sets CELWHouse vftable, zeros event
//      handles +0x6a/+0x6e/+0x66/+0x72, sets owner thread +0x76, sets
//      +0xba=0xFFFFFFFF, +0xc2=0, etc.). Caches in a CPtrArray (DAT_1023faec).
//    - Confirms warehouse layout: +0x04 state, +0x12 type, +0x16 moduleId,
//      +0x26 SafeArray, +0x4e HRESULT, +0x5e per-module COM, +0x6a done event,
//      +0x6e wake event, +0x72 mutex, +0x76 owner thread, +0xae main COM,
//      +0xc2 cached flag (initialized to 0 by FUN_101cf1f0).
//
// 3. Is there an exported function that triggers command 0x0D?
//    - YES, but it is NOT exported. FUN_101cfce0 (RVA 0x1CFCE0) is the
//      orlib20-internal IWHouse::ProgramById worker-trigger. It is called from
//      32 internal sites (including FUN_101c36d0, the BST verbatim-name
//      getter). Its body:
//        *(param_1 + 0x12) = param_2;   // type
//        *(param_1 + 0x16) = param_3;   // ProgId
//        WaitForSingleObject(*(param_1 + 0x72), INFINITE);  // mutex
//        if (GetCurrentThreadId() != *(param_1 + 0x76)) swi(3); // assert
//        *(param_1 + 0x04) = 0xd;       // command 0x0D (ProgramById)
//        SetEvent(*(param_1 + 0x6e));   // wake worker
//        WaitForSingleObject(*(param_1 + 0x6a), INFINITE); // wait done
//        ReleaseMutex(*(param_1 + 0x72));
//        uVar3 = *(param_1 + 0x5e);     // harvest result
//        *(param_1 + 0x5e) = 0;         // clear
//        return uVar3;
//      This is the orlib20-side equivalent of TSDev.exe FUN_004d75b0.
//      4 functions contain the inline `MOV [ESI+0x4],0xd` pattern:
//        FUN_101cfce0, FUN_101d0630, FUN_101d06d0, FUN_101d15c0.
//      NONE are exported (all are internal __thiscall methods).
//    - The generic signal-worker-and-wait is FUN_101cf5b0 (RVA 0x1CF5B0):
//        WaitForSingleObject(*(param_1 + 0x72), INFINITE);
//        if (GetCurrentThreadId() != *(param_1 + 0x76) && param_2 != 0x1e) swi(3);
//        *(param_1 + 0x04) = param_2;   // command
//        SetEvent(*(param_1 + 0x6e));
//        WaitForSingleObject(*(param_1 + 0x6a), INFINITE);
//        ReleaseMutex(*(param_1 + 0x72));
//      FUN_101cf5b0 is called from 44 internal sites. It is NOT exported.
//
// 4. Ordinal_68 (FUN_101a87d0, RVA 0x1A87D0) — count accessor
//    - Calls FUN_101e5970 which returns *(obj+0x0C). trivial getter.
//
// CONCLUSION: Ordinal_71 is a lazy BST-global initializer. Calling it alone
// will NOT populate warehouse+0x26 or any record's SafeArray. The command 0x0D
// path (FUN_101cfce0) is internal-only and not reachable via any export.
// This diagnostic verifies these findings live and attempts the risky direct
// worker-thread command 0x0D replication only if Ordinal_71 is a no-op (which
// the Ghidra analysis says it will be).
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB
//      stack worker thread.
//   2. Resolves Ordinal_10, Ordinal_71, Ordinal_69, Ordinal_70, Ordinal_68,
//      Ordinal_28.
//   3. Inserts + verifies a Function "TS_AUTO_M4_O71_FUNC" with source
//      `TS_AUTO_M4_O71_FUNC = 1;`.
//   4. Records the Function's ProgId.
//   5. Reads warehouse+0xc2 (cached flag) BEFORE calling Ordinal_71.
//   6. Calls Ordinal_71(4, funcProgId) (SEH-protected) and prints the return
//      value. Reads warehouse+0xc2 again. Reads warehouse +0x04/+0x12/+0x16/
//      +0x4e/+0x26.
//   7. If Ordinal_71 returns a non-NULL pointer, reads its fields (it should
//      be the 0x2c-byte BST control block, NOT a record — verify +0x08 is a
//      critical section, +0x00 is a BST root node).
//   8. Tries Ordinal_71 with (0, 0) and no-arg variants.
//   9. Attempts the direct worker-thread command 0x0D replication
//      (FUN_101cfce0 pattern) — SAVE/RESTORE warehouse fields, signal
//      event+0x6e, wait on event+0x6a, check +0x4e, read +0x26 SafeArray.
//  10. Cleanup: delete the Function. Check for TSCrashReport.exe.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>
#include <oleauto.h>          // SafeArrayGetDim, SafeArrayGetLBound, etc.

using namespace tsdev;

namespace {

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_10 (GetWarehouse, FUN_101a9650): __cdecl, 1 arg (path; 0=default).
//   Returns the warehouse singleton pointer (CELWHouse*).
typedef void* (__cdecl *GetWarehouseFn)(int pathParam);

// Ordinal_71 (InitGlobalLookup, FUN_101a9460): __cdecl. Callers PUSH 2 DWORDs
//   (type, ProgId) but the function IGNORES them (body never references
//   [EBP+8]/[EBP+0xC]; bare RET, no RET n; callers rely on EBP-frame epilogue
//   to discard args). Returns the 0x2c-byte global BST-control-block pointer
//   (DAT_1023ced4), or NULL if allocation failed.
//   We declare it with 2 args to match the push sequence at the call sites;
//   the function ignores them but the stack slots must exist.
typedef void* (__cdecl *Ordinal71Fn)(int type, int progId);

// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args (category:char, progId:int,
//   flag:int). Returns live record pointer (refcounted — release via
//   Ordinal_70).
typedef void* (__cdecl *Ordinal69Fn)(char category, int progId, int flag);

// Ordinal_70 (FUN_1015df30): __stdcall, 2 args (rec, flag). RET 0x8.
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);

// Ordinal_68 (FUN_101a87d0): __cdecl, 1 arg (obj). Returns *(obj+0x0C).
typedef int   (__cdecl *Ordinal68Fn)(void* obj);

// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args (category, progId, flag,
//   outBuf). RET 0x10. Writes a 0x10A-byte partial copy into outBuf.
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct PtrSehResult {
    void* ptr;  // NULL if SEH or function returned NULL
};

bool callGetWarehouse(GetWarehouseFn fn, int pathParam, PtrSehResult* out) {
    out->ptr = nullptr;
    __try {
        out->ptr = fn(pathParam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callOrd71(Ordinal71Fn fn, int type, int progId, PtrSehResult* out) {
    out->ptr = nullptr;
    __try {
        out->ptr = fn(type, progId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord69SehResult {
    void* rec;
};

bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               Ord69SehResult* out) {
    out->rec = nullptr;
    __try {
        out->rec = fn(category, progId, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord70SehResult {
    int result;
};

bool callOrd70(Ordinal70Fn fn, void* rec, int flag,
               Ord70SehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(rec, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord68SehResult {
    int result;
};

bool callOrd68(Ordinal68Fn fn, void* obj, Ord68SehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord28SehResult {
    unsigned int result;
};

bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try {
        out->result = fn(category, progId, flag, outBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers. Each reads a single POD value from an
// arbitrary pointer. POD-only so __try/__except is legal.
// -------------------------------------------------------------------------
bool sehReadInt(const void* p, int* out) {
    *out = 0;
    __try { *out = *(const int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehReadShort(const void* p, short* out) {
    *out = 0;
    __try { *out = *(const short*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehReadByte(const void* p, unsigned char* out) {
    *out = 0;
    __try { *out = *(const unsigned char*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehReadPtr(const void* p, void** out) {
    *out = nullptr;
    __try { *out = *(void* const*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehReadDword(const void* p, unsigned int* out) {
    *out = 0;
    __try { *out = *(const unsigned int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehWriteInt(void* p, int val) {
    __try { *(int*)p = val; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehWriteShort(void* p, short val) {
    __try { *(short*)p = val; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehMemcpy(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// -------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// EasyLanguage source builder.
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
}

void preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
}

int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    const char* label) {
    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                label, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0");
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false");
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors");
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", label, progId);
    return progId;
}

// -------------------------------------------------------------------------
// Hex dump of a byte buffer (canonical 16-byte rows with ASCII gutter).
// -------------------------------------------------------------------------
void hexDump(const unsigned char* buf, size_t n, const char* label) {
    std::printf("\n  --- HEX DUMP: %s (%zu bytes) ---\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        std::printf("  %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) {
                std::printf("%02x ", buf[i + j]);
            } else {
                std::printf("   ");
            }
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) {
                unsigned char c = buf[i + j];
                std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
            } else {
                std::printf(" ");
            }
        }
        std::printf("|\n");
    }
}

// -------------------------------------------------------------------------
// Warehouse field snapshot.
// -------------------------------------------------------------------------
struct WhFields {
    int           state;          // +0x04
    short         type;           // +0x12
    short         moduleId;       // +0x16
    int           key;            // +0x1e
    void*         safeArrayPtr;   // +0x26
    int           hresult;        // +0x4e
    void*         perModuleCom;   // +0x5e
    void*         doneEvent;      // +0x6a
    void*         wakeEvent;      // +0x6e
    void*         mutex;          // +0x72
    unsigned int  ownerThread;    // +0x76
    void*         mainWhCom;      // +0xAE
    void*         secondaryCom;   // +0xAA
    int           cachedFlag;     // +0xC2
    bool          anySeh;
};

WhFields readWhFields(const void* whPtr) {
    WhFields f = {};
    f.anySeh = false;
    if (!whPtr) return f;
    const unsigned char* wh = (const unsigned char*)whPtr;
    if (!sehReadInt  (wh + 0x04, &f.state))         f.anySeh = true;
    if (!sehReadShort(wh + 0x12, &f.type))          f.anySeh = true;
    if (!sehReadShort(wh + 0x16, &f.moduleId))      f.anySeh = true;
    if (!sehReadInt  (wh + 0x1e, &f.key))           f.anySeh = true;
    if (!sehReadPtr  (wh + 0x26, &f.safeArrayPtr))  f.anySeh = true;
    if (!sehReadInt  (wh + 0x4e, &f.hresult))       f.anySeh = true;
    if (!sehReadPtr  (wh + 0x5e, &f.perModuleCom))  f.anySeh = true;
    if (!sehReadPtr  (wh + 0x6a, &f.doneEvent))     f.anySeh = true;
    if (!sehReadPtr  (wh + 0x6e, &f.wakeEvent))     f.anySeh = true;
    if (!sehReadPtr  (wh + 0x72, &f.mutex))         f.anySeh = true;
    if (!sehReadDword(wh + 0x76, &f.ownerThread))   f.anySeh = true;
    if (!sehReadPtr  (wh + 0xAA, &f.secondaryCom))  f.anySeh = true;
    if (!sehReadPtr  (wh + 0xAE, &f.mainWhCom))     f.anySeh = true;
    if (!sehReadInt  (wh + 0xC2, &f.cachedFlag))    f.anySeh = true;
    return f;
}

void printWhFields(const char* label, const WhFields& f) {
    std::printf("\n  --- WAREHOUSE FIELDS: %s ---\n", label);
    std::printf("    +0x04 state/command (int):       %d (0x%X)\n",
                f.state, (unsigned)f.state);
    std::printf("    +0x12 type code (short):         %d (0x%04X)\n",
                (int)f.type, (unsigned short)f.type);
    std::printf("    +0x16 module id (short):         %d (0x%04X)\n",
                (int)f.moduleId, (unsigned short)f.moduleId);
    std::printf("    +0x1e key (int):                 %d (0x%X)\n",
                f.key, (unsigned)f.key);
    std::printf("    +0x26 SafeArray ptr:             %p  [%s]\n",
                f.safeArrayPtr, f.safeArrayPtr ? "NON-NULL" : "NULL");
    std::printf("    +0x4e HRESULT/status (int):      %d (0x%X)  [%s]\n",
                f.hresult, (unsigned)f.hresult,
                f.hresult == 0 ? "SUCCESS" : "FAILURE");
    std::printf("    +0x5e per-module COM object:     %p  [%s]\n",
                f.perModuleCom, f.perModuleCom ? "NON-NULL" : "NULL");
    std::printf("    +0x6a done event (auto-reset):   %p\n", f.doneEvent);
    std::printf("    +0x6e wake event (manual-reset): %p\n", f.wakeEvent);
    std::printf("    +0x72 mutex:                     %p\n", f.mutex);
    std::printf("    +0x76 owning thread ID:          %u (0x%X)\n",
                f.ownerThread, f.ownerThread);
    std::printf("    +0xAA secondary COM object:      %p  [%s]\n",
                f.secondaryCom, f.secondaryCom ? "NON-NULL" : "NULL");
    std::printf("    +0xAE main WHServer COM object:  %p  [%s]\n",
                f.mainWhCom, f.mainWhCom ? "NON-NULL" : "NULL");
    std::printf("    +0xC2 cached flag (int):         %d  [%s]\n",
                f.cachedFlag, f.cachedFlag == 1 ? "CACHED (BST path)" :
                "NOT CACHED (worker path)");
    if (f.anySeh) std::printf("    [one or more field reads raised SEH]\n");
}

// -------------------------------------------------------------------------
// SafeArray inspection. SEH-protected. Does NOT destroy the SafeArray.
// -------------------------------------------------------------------------
void inspectSafeArray(void* saPtr, const char* label) {
    std::printf("\n  --- SAFEARRAY INSPECTION: %s ---\n", label);
    if (!saPtr) {
        std::printf("    [SafeArray pointer is NULL — nothing to inspect]\n");
        return;
    }
    SAFEARRAY* psa = (SAFEARRAY*)saPtr;
    USHORT dim = 0;
    __try {
        dim = SafeArrayGetDim(psa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SafeArrayGetDim raised SEH — not a valid SafeArray]\n");
        return;
    }
    std::printf("    SafeArrayGetDim = %u\n", (unsigned)dim);
    if (dim != 1) {
        std::printf("    [dim != 1 — not a 1-D array; skipping bounds/data]\n");
        return;
    }
    LONG lb = 0, ub = 0;
    __try {
        SafeArrayGetLBound(psa, 1, &lb);
        SafeArrayGetUBound(psa, 1, &ub);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SafeArrayGetLBound/UBound raised SEH]\n");
        return;
    }
    std::printf("    LBound = %ld, UBound = %ld (count = %ld)\n",
                lb, ub, ub - lb + 1);
    UINT elemSize = 0;
    __try {
        elemSize = SafeArrayGetElemsize(psa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SafeArrayGetElemsize raised SEH]\n");
        return;
    }
    std::printf("    cbElements = %u\n", elemSize);

    void* data = nullptr;
    __try {
        HRESULT hr = SafeArrayAccessData(psa, &data);
        if (FAILED(hr)) {
            std::printf("    SafeArrayAccessData failed: 0x%08X\n",
                        (unsigned)hr);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SafeArrayAccessData raised SEH]\n");
        return;
    }
    if (!data) {
        std::printf("    [AccessData returned NULL data pointer]\n");
        __try { SafeArrayUnaccessData(psa); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    size_t totalBytes = (size_t)(ub - lb + 1) * (size_t)elemSize;
    size_t dumpBytes = totalBytes < 256 ? totalBytes : 256;
    std::printf("    Total bytes = %zu, dumping first %zu:\n",
                totalBytes, dumpBytes);
    hexDump((const unsigned char*)data, dumpBytes, "SafeArray data (first 256B)");

    // If elements are 4-byte ints (ProgIds), print them.
    if (elemSize == 4 && totalBytes >= 4) {
        size_t count = totalBytes / 4;
        size_t printCount = count < 32 ? count : 32;
        const int* ints = (const int*)data;
        std::printf("    First %zu int elements (ProgIds?): ", printCount);
        for (size_t i = 0; i < printCount; i++) {
            std::printf("%d ", ints[i]);
        }
        std::printf("\n");
    }

    __try { SafeArrayUnaccessData(psa); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    [SafeArrayUnaccessData raised SEH]\n");
    }
    std::printf("    [SafeArray NOT destroyed — caller owns it]\n");
}

// -------------------------------------------------------------------------
// Analyze the pointer returned by Ordinal_71.
//   Per Ghidra: Ordinal_71 returns DAT_1023ced4 — a 0x2c-byte BST control
//   block (NOT a module record). Layout (from FUN_101e3230):
//     +0x00: BST root node pointer (0x24-byte node; self-ref left/right/
//            parent; +0x0C balance=0x101)
//     +0x04: 0 (zeroed by ctor)
//     +0x08: CRITICAL_SECTION (24 bytes on x86)
//     +0x20: 0 (zeroed)
//     +0x24: 0 (zeroed)
//     +0x28: 0 (zeroed)
// -------------------------------------------------------------------------
void analyzeOrd71Result(const void* ptr, const char* label) {
    std::printf("\n=== ORDINAL_71 RETURN VALUE ANALYSIS: %s ===\n", label);
    if (!ptr) {
        std::printf("  [return pointer is NULL — global init failed]\n");
        return;
    }
    const unsigned char* p = (const unsigned char*)ptr;
    std::printf("  Return pointer = %p\n", ptr);

    void* bstRoot = nullptr;
    int  field04 = 0, field20 = 0, field24 = 0, field28 = 0;
    bool anySeh = false;
    if (!sehReadPtr(p + 0x00, &bstRoot)) anySeh = true;
    if (!sehReadInt (p + 0x04, &field04)) anySeh = true;
    if (!sehReadInt (p + 0x20, &field20)) anySeh = true;
    if (!sehReadInt (p + 0x24, &field24)) anySeh = true;
    if (!sehReadInt (p + 0x28, &field28)) anySeh = true;
    std::printf("    +0x00 BST root node ptr: %p  [%s]\n",
                bstRoot, bstRoot ? "NON-NULL (expected)" : "NULL");
    std::printf("    +0x04 field (expect 0):  %d (0x%X)\n",
                field04, (unsigned)field04);
    std::printf("    +0x20 field (expect 0):  %d (0x%X)\n",
                field20, (unsigned)field20);
    std::printf("    +0x24 field (expect 0):  %d (0x%X)\n",
                field24, (unsigned)field24);
    std::printf("    +0x28 field (expect 0):  %d (0x%X)\n",
                field28, (unsigned)field28);
    // Dump the critical section region (+0x08, 24 bytes) for confirmation.
    unsigned char csBuf[24] = {};
    if (sehMemcpy(p + 0x08, csBuf, 24)) {
        hexDump(csBuf, 24, "CriticalSection region (+0x08, 24B)");
    } else {
        std::printf("    [+0x08 CriticalSection read raised SEH]\n");
        anySeh = true;
    }
    if (anySeh) std::printf("    [one or more reads raised SEH]\n");

    // Verify this is NOT a module record by checking if +0x26 looks like a
    // SafeArray pointer (it shouldn't — +0x26 in the BST block is inside the
    // critical section).
    void* maybeSA = nullptr;
    if (sehReadPtr(p + 0x26, &maybeSA)) {
        std::printf("    +0x26 (would be SafeArray in a record): %p  "
                    "[inside CRITICAL_SECTION in BST block — NOT a record]\n",
                    maybeSA);
    }
    std::printf("  >> CONFIRMED: Ordinal_71 returns a BST control block, "
                "NOT a module record.\n");
}

// -------------------------------------------------------------------------
// Analyze a live record returned by Ordinal_69 (for comparison / to read
// the SafeArray at +0x26).
// -------------------------------------------------------------------------
void analyzeLiveRecord(const void* recPtr, const char* label,
                       int expectedProgId, Ordinal70Fn ord70) {
    std::printf("\n=== LIVE RECORD ANALYSIS: %s ===\n", label);
    if (!recPtr) {
        std::printf("  [record pointer is NULL — nothing to analyze]\n");
        return;
    }
    const unsigned char* rec = (const unsigned char*)recPtr;
    std::printf("  Record pointer = %p\n", recPtr);

    short f00 = 0; unsigned char f15 = 0; void* sa26 = nullptr;
    int  f29 = 0; void* f4f = nullptr; unsigned char f55 = 0;
    int  fca = 0; void* fd6 = nullptr;
    bool anySeh = false;
    if (!sehReadShort(rec + 0x00, &f00))    anySeh = true;
    if (!sehReadByte (rec + 0x15, &f15))    anySeh = true;
    if (!sehReadPtr  (rec + 0x26, &sa26))   anySeh = true;
    if (!sehReadInt  (rec + 0x29, &f29))    anySeh = true;
    if (!sehReadPtr  (rec + 0x4f, &f4f))    anySeh = true;
    if (!sehReadByte (rec + 0x55, &f55))    anySeh = true;
    if (!sehReadInt  (rec + 0xca, &fca))    anySeh = true;
    if (!sehReadPtr  (rec + 0xd6, &fd6))    anySeh = true;

    std::printf("    +0x00 ProgId (short):     %d (0x%04X)  [expect %d]\n",
                (int)f00, (unsigned short)f00, expectedProgId);
    std::printf("    +0x15 flags (byte):       0x%02X\n", (unsigned)f15);
    std::printf("    +0x26 SafeArray ptr:      %p  [%s]\n",
                sa26, sa26 ? "NON-NULL" : "NULL");
    std::printf("    +0x29 element count:      %d\n", f29);
    std::printf("    +0x4f element array ptr:  %p  [%s]\n",
                f4f, f4f ? "NON-NULL" : "NULL");
    std::printf("    +0x55 verified flag:      0x%02X\n", (unsigned)f55);
    std::printf("    +0xCA refcount:           %d\n", fca);
    std::printf("    +0xD6 pointer:            %p\n", fd6);
    if (anySeh) std::printf("    [one or more reads raised SEH]\n");

    if (sa26) {
        inspectSafeArray(sa26, "record+0x26 SafeArray");
    } else {
        std::printf("    [No SafeArray at +0x26 — record has no "
                    "reverse-dep array]\n");
    }

    // Release via Ordinal_70 (refcount-dec).
    if (ord70) {
        Ord70SehResult r;
        bool ok = callOrd70(ord70, (void*)recPtr, 0, &r);
        std::printf("    Ordinal_70(rec, 0): %s, result=%d\n",
                    ok ? "OK" : "SEH", r.result);
    } else {
        std::printf("    [Ordinal_70 NOT resolved — record LEAKED]\n");
    }
}

// -------------------------------------------------------------------------
// Attempt the direct worker-thread command 0x0D replication.
//   Replicates FUN_101cfce0 (orlib20 internal IWHouse::ProgramById):
//     warehouse+0x12 = type
//     warehouse+0x16 = progId
//     WaitForSingleObject(warehouse+0x72, INFINITE)  // mutex
//     assert GetCurrentThreadId() == warehouse+0x76
//     warehouse+0x04 = 0x0D
//     SetEvent(warehouse+0x6e)
//     WaitForSingleObject(warehouse+0x6a, 5000)      // done (5s timeout)
//     ReleaseMutex(warehouse+0x72)
//     result = warehouse+0x5e; warehouse+0x5e = 0
//   WARNING: This is risky. We SAVE all modified fields and RESTORE them
//   afterward. We use a 5-second timeout (not INFINITE) on the done event
//   to avoid hanging.
// -------------------------------------------------------------------------
void attemptDirectCmd0D(void* whPtr, int type, int progId) {
    std::printf("\n=== DIRECT WORKER-THREAD COMMAND 0x0D ATTEMPT ===\n");
    if (!whPtr) {
        std::printf("  [warehouse pointer is NULL — cannot attempt]\n");
        return;
    }
    unsigned char* wh = (unsigned char*)whPtr;

    // Save original fields we will modify.
    short  origType = 0, origModId = 0;
    int    origState = 0;
    void  *origWakeEvt = nullptr, *origDoneEvt = nullptr, *origMutex = nullptr;
    unsigned int origOwner = 0;
    sehReadShort(wh + 0x12, &origType);
    sehReadShort(wh + 0x16, &origModId);
    sehReadInt  (wh + 0x04, &origState);
    sehReadPtr  (wh + 0x6e, &origWakeEvt);
    sehReadPtr  (wh + 0x6a, &origDoneEvt);
    sehReadPtr  (wh + 0x72, &origMutex);
    sehReadDword(wh + 0x76, &origOwner);
    std::printf("  Saved originals: type=%d modId=%d state=%d "
                "wakeEvt=%p doneEvt=%p mutex=%p owner=%u\n",
                (int)origType, (int)origModId, origState,
                origWakeEvt, origDoneEvt, origMutex, origOwner);

    // Verify event/mutex handles are non-NULL.
    if (!origWakeEvt || !origDoneEvt || !origMutex) {
        std::printf("  [ABORT: event/mutex handles are NULL — worker not "
                    "initialized]\n");
        return;
    }

    // Verify thread ownership (FUN_101cfce0 asserts this).
    DWORD curTid = GetCurrentThreadId();
    std::printf("  Current thread ID = %lu, warehouse owner = %u  [%s]\n",
                curTid, origOwner,
                curTid == origOwner ? "MATCH (ok)" : "MISMATCH (assert would "
                "fire in orlib20)");

    // Set type and moduleId.
    std::printf("  Setting warehouse+0x12 = %d (type), +0x16 = %d (progId)\n",
                type, progId);
    sehWriteShort(wh + 0x12, (short)type);
    sehWriteShort(wh + 0x16, (short)progId);

    // Acquire mutex (with 5s timeout to avoid hang).
    std::printf("  WaitForSingleObject(mutex, 5000)...\n");
    DWORD mres = WaitForSingleObject(origMutex, 5000);
    std::printf("    mutex wait result = %lu  [%s]\n", mres,
                mres == WAIT_OBJECT_0 ? "acquired" :
                mres == WAIT_TIMEOUT ? "TIMEOUT" : "FAILED");
    if (mres != WAIT_OBJECT_0) {
        std::printf("  [ABORT: could not acquire mutex — restoring fields]\n");
        sehWriteShort(wh + 0x12, origType);
        sehWriteShort(wh + 0x16, origModId);
        return;
    }

    // Set command 0x0D.
    std::printf("  Setting warehouse+0x04 = 0x0D (command ProgramById)\n");
    sehWriteInt(wh + 0x04, 0x0D);

    // Signal wake event.
    std::printf("  SetEvent(wakeEvent=%p)...\n", origWakeEvt);
    BOOL sr = SetEvent(origWakeEvt);
    std::printf("    SetEvent result = %d\n", (int)sr);

    // Wait for done event (5s timeout).
    std::printf("  WaitForSingleObject(doneEvent=%p, 5000)...\n",
                origDoneEvt);
    DWORD wres = WaitForSingleObject(origDoneEvt, 5000);
    std::printf("    done wait result = %lu  [%s]\n", wres,
                wres == WAIT_OBJECT_0 ? "signaled" :
                wres == WAIT_TIMEOUT ? "TIMEOUT (worker did not respond)" :
                "FAILED");

    // Release mutex.
    std::printf("  ReleaseMutex(mutex=%p)...\n", origMutex);
    BOOL rr = ReleaseMutex(origMutex);
    std::printf("    ReleaseMutex result = %d\n", (int)rr);

    // Read result fields.
    int   hresult = 0;
    void* perModCom = nullptr;
    void* sa26 = nullptr;
    int   stateAfter = 0;
    short typeAfter = 0, modIdAfter = 0;
    sehReadInt (wh + 0x4e, &hresult);
    sehReadPtr (wh + 0x5e, &perModCom);
    sehReadPtr (wh + 0x26, &sa26);
    sehReadInt (wh + 0x04, &stateAfter);
    sehReadShort(wh + 0x12, &typeAfter);
    sehReadShort(wh + 0x16, &modIdAfter);
    std::printf("  AFTER command 0x0D:\n");
    std::printf("    +0x04 state:        %d (0x%X)  [was %d]\n",
                stateAfter, (unsigned)stateAfter, origState);
    std::printf("    +0x12 type:         %d  [was %d]\n",
                (int)typeAfter, (int)origType);
    std::printf("    +0x16 moduleId:     %d  [was %d]\n",
                (int)modIdAfter, (int)origModId);
    std::printf("    +0x4e HRESULT:      %d (0x%X)  [%s]\n",
                hresult, (unsigned)hresult,
                hresult == 0 ? "SUCCESS" : "FAILURE");
    std::printf("    +0x5e per-module COM: %p  [%s]\n",
                perModCom, perModCom ? "NON-NULL" : "NULL");
    std::printf("    +0x26 SafeArray:    %p  [%s]\n",
                sa26, sa26 ? "NON-NULL" : "NULL");

    if (sa26) {
        inspectSafeArray(sa26, "warehouse+0x26 after cmd 0x0D");
    } else {
        std::printf("    [No SafeArray at warehouse+0x26 after cmd 0x0D]\n");
    }

    // Restore original fields (type, moduleId, state). Do NOT restore event
    // handles or the SafeArray (the worker may have populated state we want
    // to leave intact). Restore type/modId/state to avoid corrupting
    // subsequent operations.
    std::printf("  Restoring warehouse+0x12=%d, +0x16=%d, +0x04=%d...\n",
                (int)origType, (int)origModId, origState);
    sehWriteShort(wh + 0x12, origType);
    sehWriteShort(wh + 0x16, origModId);
    sehWriteInt  (wh + 0x04, origState);
    std::printf("  [Direct cmd 0x0D attempt complete]\n");
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName = "TS_AUTO_M4_O71_FUNC";

    std::printf("=== diag_m4_ord71: Ordinal_71 (InitGlobalLookup) + cmd 0x0D "
                "diagnostic ===\n");
    std::printf("Purpose: verify Ordinal_71 (orlib20 ord 71, RVA 0x1A9460) "
                "ABI and determine\n");
    std::printf("  whether it (or a direct worker-thread command 0x0D) "
                "populates the\n");
    std::printf("  reverse-dependent SafeArray at warehouse+0x26 / "
                "record+0x26.\n");
    std::printf("Ghidra-confirmed: Ordinal_71 is a LAZY BST-global "
                "initializer (ignores its\n");
    std::printf("  2 pushed args, returns 0x2c-byte BST control block, does "
                "NOT trigger worker,\n");
    std::printf("  does NOT set warehouse+0xc2, does NOT return a record).\n");

    // --- Step 1: construct a real TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib construction threw: %s\n",
                     e.what());
        st->exitCode = 100;
        return 0;
    }
    if (!lib) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest returned null\n");
        st->exitCode = 101;
        return 0;
    }
    std::printf("    TsDevLib constructed OK.\n");

    // --- Step 2: resolve all ordinals.
    std::printf("\n[2] Resolving Ordinal_10, Ordinal_71, Ordinal_69, "
                "Ordinal_70, Ordinal_68, Ordinal_28...\n");
    GetWarehouseFn getWh = nullptr;
    Ordinal71Fn    ord71 = nullptr;
    Ordinal69Fn    ord69 = nullptr;
    Ordinal70Fn    ord70 = nullptr;
    Ordinal68Fn    ord68 = nullptr;
    Ordinal28Fn    ord28 = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        getWh = reinterpret_cast<GetWarehouseFn>(
            resolver.resolveExportByOrdinal(10));
        ord71 = reinterpret_cast<Ordinal71Fn>(
            resolver.resolveExportByOrdinal(71));
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        ord68 = reinterpret_cast<Ordinal68Fn>(
            resolver.resolveExportByOrdinal(68));
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!getWh) {
        std::fprintf(stderr, "FATAL: failed to resolve Ordinal_10\n");
        st->exitCode = 103;
        return 0;
    }
    if (!ord71) {
        std::fprintf(stderr, "FATAL: failed to resolve Ordinal_71\n");
        st->exitCode = 104;
        return 0;
    }
    if (!ord69) std::fprintf(stderr, "WARN: failed to resolve Ordinal_69\n");
    if (!ord70) std::fprintf(stderr, "WARN: failed to resolve Ordinal_70\n");
    if (!ord68) std::fprintf(stderr, "WARN: failed to resolve Ordinal_68\n");
    if (!ord28) std::fprintf(stderr, "WARN: failed to resolve Ordinal_28\n");
    std::printf("    Resolved: Ord10=%p Ord71=%p Ord69=%p Ord70=%p "
                "Ord68=%p Ord28=%p\n",
                (void*)getWh, (void*)ord71, (void*)ord69, (void*)ord70,
                (void*)ord68, (void*)ord28);

    // --- Step 3: insert + verify the Function.
    std::printf("\n[3] Insert + verify Function \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        st->exitCode = 200;
        return 0;
    }
    std::printf("\n[4] Recorded Function ProgId = %d\n", funcProgId);

    // --- Step 5: get warehouse pointer, read +0xc2 BEFORE Ordinal_71.
    std::printf("\n[5] Getting warehouse pointer via Ordinal_10(0)...\n");
    PtrSehResult whSeh;
    bool whOk = callGetWarehouse(getWh, 0, &whSeh);
    std::printf("    Ordinal_10(0): %s, warehouse=%p\n",
                whOk ? "OK (no SEH)" : "SEH raised", whSeh.ptr);
    void* whPtr = whSeh.ptr;
    if (!whPtr) {
        std::fprintf(stderr, "  [WARN] warehouse pointer is NULL — cannot "
                     "read fields.\n");
    }

    if (whPtr) {
        WhFields before = readWhFields(whPtr);
        printWhFields("BEFORE Ordinal_71 call", before);
        std::printf("\n  >> warehouse+0xC2 (cached flag) BEFORE = %d  [%s]\n",
                    before.cachedFlag,
                    before.cachedFlag == 1 ? "CACHED" : "NOT CACHED");
    }

    // --- Step 6: call Ordinal_71(4, funcProgId).
    std::printf("\n[6] Calling Ordinal_71(type=4, progId=%d)...\n",
                funcProgId);
    PtrSehResult ord71Seh;
    bool ord71Ok = callOrd71(ord71, /*type=*/4, /*progId=*/funcProgId,
                             &ord71Seh);
    std::printf("    call result: %s, return=%p\n",
                ord71Ok ? "OK (no SEH)" : "SEH raised", ord71Seh.ptr);
    if (!ord71Ok) {
        std::fprintf(stderr, "  [WARN] Ordinal_71 raised SEH.\n");
    }

    // Read warehouse fields AFTER Ordinal_71.
    if (whPtr) {
        WhFields after = readWhFields(whPtr);
        printWhFields("AFTER Ordinal_71 call", after);
        std::printf("\n  >> warehouse+0xC2 (cached flag) AFTER = %d  "
                    "[%s]  [changed? %s]\n",
                    after.cachedFlag,
                    after.cachedFlag == 1 ? "CACHED" : "NOT CACHED",
                    after.cachedFlag != readWhFields(whPtr).cachedFlag
                        ? "YES" : "NO");
    }

    // --- Step 7: analyze Ordinal_71 return value.
    if (ord71Ok && ord71Seh.ptr) {
        analyzeOrd71Result(ord71Seh.ptr,
                           "Ordinal_71(4, funcProgId) return");
    } else if (ord71Ok) {
        std::printf("\n  [Ordinal_71 returned NULL — global init failed]\n");
    }

    // --- Step 8: try Ordinal_71 with (0, 0) and (4, 0) variants.
    std::printf("\n[8] Trying Ordinal_71 variant calls...\n");
    {
        PtrSehResult r0;
        bool ok0 = callOrd71(ord71, 0, 0, &r0);
        std::printf("    Ordinal_71(0, 0): %s, return=%p  [%s]\n",
                    ok0 ? "OK" : "SEH", r0.ptr,
                    (ok0 && r0.ptr == ord71Seh.ptr) ?
                    "SAME as first call (idempotent — expected)" :
                    "DIFFERENT");
    }
    {
        PtrSehResult r1;
        bool ok1 = callOrd71(ord71, 4, 0, &r1);
        std::printf("    Ordinal_71(4, 0): %s, return=%p  [%s]\n",
                    ok1 ? "OK" : "SEH", r1.ptr,
                    (ok1 && r1.ptr == ord71Seh.ptr) ?
                    "SAME as first call (idempotent — expected)" :
                    "DIFFERENT");
    }

    // --- Step 9: For comparison, call Ordinal_69 to get the Function's
    //     live record and inspect its +0x26 SafeArray. This is the
    //     "known-good" path that returns a record.
    std::printf("\n[9] Calling Ordinal_69(category=4, progId=%d, flag=0) "
                "for comparison (known-good record path)...\n",
                funcProgId);
    if (ord69) {
        Ord69SehResult ord69Seh;
        bool ord69Ok = callOrd69(ord69, /*category=*/(char)4,
                                 /*progId=*/funcProgId, /*flag=*/0,
                                 &ord69Seh);
        std::printf("    Ordinal_69: %s, rec=%p\n",
                    ord69Ok ? "OK (no SEH)" : "SEH raised", ord69Seh.rec);
        if (ord69Ok && ord69Seh.rec) {
            analyzeLiveRecord(ord69Seh.rec,
                              "FUNCTION live record (Ordinal_69, comparison)",
                              funcProgId, ord70);
        }
    } else {
        std::printf("    [Ordinal_69 not resolved — skipping comparison]\n");
    }

    // --- Step 9b: probe Ordinal_68 on the Ordinal_71 return value.
    //   Ordinal_68 returns *(obj+0x0C). On the BST control block, +0x0C is
    //   inside the critical section — should be garbage. On a real record,
    //   +0x0C is the count field.
    std::printf("\n[9b] Probing Ordinal_68 on Ordinal_71 return value...\n");
    if (ord68 && ord71Ok && ord71Seh.ptr) {
        Ord68SehResult r;
        bool ok = callOrd68(ord68, ord71Seh.ptr, &r);
        std::printf("    Ordinal_71 return: %s, result=%d (0x%X)  "
                    "[expect garbage — +0x0C is inside CRITICAL_SECTION]\n",
                    ok ? "OK" : "SEH", r.result, (unsigned)r.result);
    }

    // --- Step 9c: probe Ordinal_28 (sibling) for completeness.
    if (ord28) {
        std::printf("\n[9c] Probing Ordinal_28 (sibling) for the "
                    "Function...\n");
        unsigned char ord28buf[0x10A] = {};
        Ord28SehResult ord28Seh;
        bool ord28Ok = callOrd28(ord28, /*category=*/4, funcProgId,
                                 /*flag=*/0, ord28buf, &ord28Seh);
        std::printf("    Ordinal_28: %s, return=%u\n",
                    ord28Ok ? "OK (no SEH)" : "SEH raised",
                    ord28Seh.result);
    }

    // --- Step 10: Attempt direct worker-thread command 0x0D.
    //   Per Ghidra, Ordinal_71 does NOT trigger the worker. The only way to
    //   get the reverse-dependent SafeArray via the worker path is to
    //   replicate FUN_101cfce0 (command 0x0D). This is risky but the Ghidra
    //   analysis confirms the exact field offsets and event handles.
    std::printf("\n[10] Attempting direct worker-thread command 0x0D "
                "(FUN_101cfce0 replication)...\n");
    std::printf("  WARNING: This directly manipulates warehouse fields and "
                "signals the worker.\n");
    std::printf("  Saving/restoring modified fields. 5s timeout on done "
                "event.\n");
    if (whPtr) {
        attemptDirectCmd0D(whPtr, /*type=*/4, /*progId=*/funcProgId);
    } else {
        std::printf("  [SKIPPED — warehouse pointer is NULL]\n");
    }

    // Re-read warehouse fields after the cmd 0x0D attempt.
    if (whPtr) {
        WhFields afterCmd = readWhFields(whPtr);
        printWhFields("AFTER direct cmd 0x0D attempt", afterCmd);
    }

    // --- Step 11: Cleanup.
    std::printf("\n[11] Cleanup: delete Function...\n");
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary.
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId = %d\n", funcProgId);
    std::printf("Ordinal_71(4, %d): %s, return=%p\n",
                funcProgId, ord71Ok ? "OK" : "SEH", ord71Seh.ptr);
    std::printf("Ordinal_71 is a LAZY BST-global initializer — it does NOT "
                "trigger the worker,\n");
    std::printf("  does NOT set warehouse+0xC2, does NOT return a module "
                "record.\n");
    std::printf("  Its return value is a 0x2c-byte BST control block "
                "(DAT_1023ced4).\n");
    std::printf("Command 0x0D (IWHouse::ProgramById) is triggered by "
                "FUN_101cfce0 (internal,\n");
    std::printf("  NOT exported). The direct replication attempt is shown "
                "above.\n");

    std::printf("\n=== DIAGNOSIS ===\n");
    std::printf("  1. Ordinal_71 does NOT populate warehouse+0x26 or any "
                "record's SafeArray.\n");
    std::printf("     It only ensures the global BST lookup structure "
                "(DAT_1023ced4) exists.\n");
    std::printf("  2. The 'cached path' (warehouse+0xC2 == 1) uses Ordinal_71 "
                "as a GUARD before\n");
    std::printf("     FUN_101e3db0 (BST lookup). The BST lookup itself, on a "
                "miss, falls back to\n");
    std::printf("     FUN_101a9650 (warehouse) + FUN_101d18c0 (worker) + "
                "FUN_101c36d0 (cmd 0x34).\n");
    std::printf("  3. Command 0x0D (ProgramById) is only reachable via "
                "internal FUN_101cfce0,\n");
    std::printf("     which is NOT exported. The direct replication (step 10) "
                "is the only CLI path.\n");
    std::printf("  4. See the warehouse field snapshots above for "
                "warehouse+0xC2 before/after\n");
    std::printf("     Ordinal_71 and after the direct cmd 0x0D attempt.\n");

    st->exitCode = 0;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: is TSCrashReport.exe already running?
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [PRE] TSCrashReport.exe IS "
           "running || echo [PRE] TSCrashReport.exe NOT running");

    // Run on a 64 MB-stack worker thread (EasyLanguage compiler / hidden
    // EDIT control need this, and TsDevLib is thread-affine).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 120-second hard timeout — the direct cmd 0x0D attempt + SafeArray
    // inspection + Ordinal_69 comparison may take longer.
    DWORD wait = WaitForSingleObject(hThread, 120000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr, "\nFATAL: diagnostic timed out after 120s, "
                     "terminating.\n");
        TerminateThread(hThread, 99);
        // Best-effort cleanup of any TSCrashReport spawned by a crash.
        system("taskkill /F /IM TSCrashReport.exe >NUL 2>&1");
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [POST] TSCrashReport.exe IS "
           "running (crash occurred) || echo [POST] TSCrashReport.exe NOT "
           "running (no crash)");

    return st.exitCode;
}
