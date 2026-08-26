// diag_m4_whserver.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: investigate why the linker's state 0x38 (load compiled code from
// .ell) fails in our CLI process.
//
// Background (Ghidra RE-confirmed, see docs/PM_WORKING_MEMORY.md M.4 COMPLETE
// MECHANISM FINDING):
//   - The linker FUN_101a3340 (ordinal 85) populates the SafeArray at
//     warehouse+0x26 ON-DEMAND by signaling the WHServer COM worker thread
//     (state 0x38). The worker reads the .ell file and creates the SafeArray.
//   - If state 0x38 fails (warehouse+0x4e != 0), +0x26 stays NULL and the
//     linker returns NULL.
//   - Verify (state 0xe) ALSO goes through the WHServer worker and WORKS in
//     our CLI. So the WHServer COM worker IS functioning. The question is
//     why state 0x38 specifically fails.
//
// Key warehouse object fields:
//   +0x04: state/command (set by caller, read by worker)
//   +0x12: type code (set before signaling)
//   +0x16: module id (set before signaling)
//   +0x1e: key (0x2000, set before signaling)
//   +0x26: SafeArray pointer (transient mailbox)
//   +0x4e: HRESULT/status (0=success, nonzero=failure)
//   +0x5e: per-module COM object (from state 0xd)
//   +0x6a: auto-reset event (worker-done signal)
//   +0x6e: manual-reset event (wake-worker signal)
//   +0x72: mutex
//   +0x76: owning thread ID
//   +0xAE: main WHServer COM object
//   +0xAA: secondary COM object
//
// Key orlib20 functions:
//   Ordinal_10  (RVA 0x1A9650) — warehouse getter, returns CELWHouse pointer
//   Ordinal_69  (RVA 0x1A3E00) — module-record retrieval, triggers linker
//                                with param_3 != 0
//   Ordinal_70  (RVA 0x15DF30) — refcount/free
//   Ordinal_28  (RVA 0x1A03F0) — master-record lookup (partial copy)
//   ordinal 85  (RVA 0x1A3340) — the linker (exported)
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB-
//      stack worker thread.
//   2. Resolves Ordinal_10 (warehouse getter), Ordinal_69, Ordinal_70,
//      Ordinal_28, and ordinal 85 (linker).
//   3. Inserts + verifies a Function "TS_AUTO_M4_WHS_FUNC" with source
//      `TS_AUTO_M4_WHS_FUNC = 1;`.
//   4. Records the Function's ProgId.
//   5. Gets the warehouse pointer via Ordinal_10(0) (SEH-protected) and
//      reads + prints all the warehouse fields listed above.
//   6. Checks if the .ell file exists at the expected path; reports size.
//   7. Tries calling the linker directly (ordinal 85) with BOTH calling
//      conventions (__stdcall and __fastcall), with errorFlag=0 and =1,
//      passing a partial record buffer (from Ordinal_28) as partialBuf.
//      Before/after each call, reads warehouse+0x4e and warehouse+0x26.
//   8. Tries calling Ordinal_69 with param_3=progId (linker path). Before/
//      after, reads warehouse+0x4e and warehouse+0x26. If non-NULL return,
//      reads record fields +0x26, +0x29, +0x4f, +0x55, +0x7b. Releases via
//      Ordinal_70.
//   9. Checks the .ell file AFTER the linker attempt for changes.
//  10. Re-checks WH_ERRS.LOG / EL_ERRS.LOG after the errorFlag=1 call.
//  11. Cleanup: delete the Function. Check for TSCrashReport.exe.
//
// All orlib20 calls are SEH-protected. The warehouse pointer from
// Ordinal_10(0) is a LIVE pointer — it is NOT freed. Ordinal_69 returns a
// refcounted record — released via Ordinal_70(rec, 0).

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_10 (GetWarehouse, FUN at RVA 0x1A9650): __cdecl, 1 arg (path; 0
//   = default). Returns the warehouse singleton pointer (CELWHouse*).
typedef void* (__cdecl *GetWarehouseFn)(int pathParam);

// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args (category:char, progId:int,
//   flag:int). Returns live record pointer (refcounted — release via
//   Ordinal_70).
typedef void* (__cdecl *Ordinal69Fn)(char category, int progId, int flag);

// Ordinal_70 (FUN_1015df30): __stdcall, 2 args (rec, flag). RET 0x8.
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);

// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args (category, progId, flag,
//   outBuf). RET 0x10. Writes a 0x10A-byte partial copy into outBuf.
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);

// Linker FUN_101a3340 (ordinal 85). Calling convention UNCERTAIN — Ghidra
// says __fastcall (ECX=type, EDX=id, stack: errorFlag, partialBuf), but the
// Frida trace shows args on the stack (type, id, errorFlag, partialBuf) with
// ECX/EDX holding unrelated values. We test BOTH conventions.
//
// __stdcall variant: 4 stack args (type, id, errorFlag, partialBuf). RET 0x10.
typedef void* (__stdcall *LinkerStdcallFn)(int type, int id, int errorFlag,
                                           void* partialBuf);
// __fastcall variant: ECX=type, EDX=id, stack: errorFlag, partialBuf. RET 0x8.
typedef void* (__fastcall *LinkerFastcallFn)(int type, int id, int errorFlag,
                                             void* partialBuf);

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

bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               PtrSehResult* out) {
    out->ptr = nullptr;
    __try {
        out->ptr = fn(category, progId, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct IntSehResult {
    int result;  // -1 if SEH
};

bool callOrd70(Ordinal70Fn fn, void* rec, int flag, IntSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(rec, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord28SehResult {
    unsigned int result;  // 0xFFFFFFFF if SEH
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

bool callLinkerStdcall(LinkerStdcallFn fn, int type, int id, int errorFlag,
                       void* partialBuf, PtrSehResult* out) {
    out->ptr = nullptr;
    __try {
        out->ptr = fn(type, id, errorFlag, partialBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// callLinkerFastcallManual — calls the linker with ALL 4 args on the stack
// (as __stdcall would) AND loads ECX=type, EDX=id (as __fastcall would),
// using inline assembly so we can manually restore ESP regardless of whether
// the callee did RET 0x10 (stdcall, popped 16) or RET 0x8 (fastcall, popped
// 8). This makes the call robust to calling-convention mismatch: a mismatched
// convention would otherwise corrupt the caller's stack frame.
//
// We snapshot ESP before the CALL and restore it after (compensating for any
// imbalance), then read EAX (the return value) into out->ptr. SEH is set up
// by the caller via a separate __try/__except wrapper around this function.
//
// NOTE: This function is NOT itself SEH-protected (inline asm + SEH in the
// same function is rejected by MSVC C2712/C2181). The caller wraps it.
//
// Returns the raw EAX return value (the record pointer, or NULL).
void* callLinkerFastcallManual(void* fn, int whType, int whId, int whErrFlag,
                               void* whPartialBuf) {
    void* retVal = nullptr;
    __asm {
        // Save ESP so we can restore it regardless of how much the callee
        // popped (RET 0x10 vs RET 0x8).
        mov  esi, esp
        // Push the 4 stack args (right-to-left, __stdcall order):
        //   stack[0]=whType, stack[1]=whId, stack[2]=whErrFlag,
        //   stack[3]=whPartialBuf
        // After the 4 pushes, the stack layout matches __stdcall with 4 args.
        push whPartialBuf
        push whErrFlag
        push whId
        push whType
        // Load ECX=whType, EDX=whId (the __fastcall register assignments),
        // in case the callee reads them. This makes the call valid under
        // BOTH conventions.
        mov  ecx, whType
        mov  edx, whId
        // Call the function.
        call fn
        // Capture EAX (the return value) into retVal.
        mov  retVal, eax
        // Restore ESP from the snapshot — this discards any stack imbalance
        // (whether the callee popped 16, 8, or 0 bytes via RET n).
        mov  esp, esi
    }
    return retVal;
}

// SEH-protected wrapper around callLinkerFastcallManual. POD-only struct so
// __try/__except is legal under MSVC C2712.
struct FastcallManualSehResult {
    void* ptr;
    bool  seh;
};

bool callLinkerFastcallSafe(void* fn, int type, int id, int errorFlag,
                            void* partialBuf, FastcallManualSehResult* out) {
    out->ptr = nullptr;
    out->seh = false;
    __try {
        out->ptr = callLinkerFastcallManual(fn, type, id, errorFlag,
                                            partialBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->seh = true;
        return false;
    }
    return true;
}

bool callLinkerFastcall(LinkerFastcallFn fn, int type, int id, int errorFlag,
                        void* partialBuf, PtrSehResult* out) {
    // Deprecated direct path (kept for reference) — NOT used because a
    // convention mismatch corrupts the caller's stack. Use
    // callLinkerFastcallSafe instead.
    (void)fn; (void)type; (void)id; (void)errorFlag; (void)partialBuf;
    out->ptr = nullptr;
    return false;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers. POD-only so __try/__except is legal.
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
    void*         saCache;        // +0xB6
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
    if (!sehReadPtr  (wh + 0xB6, &f.saCache))       f.anySeh = true;
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
    std::printf("    +0xB6 existing SafeArray cache:  %p  [%s]\n",
                f.saCache, f.saCache ? "NON-NULL" : "NULL");
    if (f.anySeh) std::printf("    [one or more field reads raised SEH]\n");
}

// -------------------------------------------------------------------------
// .ell file inspection.
// -------------------------------------------------------------------------
struct EllFileInfo {
    bool        exists;
    __int64     size;
    FILETIME    mtime;
    std::string path;
};

EllFileInfo checkEllFile(const std::string& basePath,
                         const std::string& funcName) {
    EllFileInfo info = {};
    info.exists = false;
    info.size = 0;
    // The .ell file naming convention is elmodule_<type>_<progId>.ell, but
    // we don't know the progId-to-filename mapping for sure. We check both
    // the name-based and the elmodule_<type>_<progId>.ell patterns. Since
    // we have the progId, we check elmodule_4_<progId>.ell (Function=type 4).
    // We also check a name-based file just in case.
    (void)basePath;
    (void)funcName;
    return info;
}

void checkEllFileByProgId(const std::string& binDir, int whType, int progId) {
    char ellPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  binDir.c_str(), whType, progId);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(ellPath, GetFileExInfoStandard, &fad)) {
        __int64 size = ((__int64)fad.nFileSizeHigh << 32) |
                       (__int64)fad.nFileSizeLow;
        std::printf("    .ell file: %s\n", ellPath);
        std::printf("      EXISTS, size = %lld bytes\n", (long long)size);
        // Print mtime.
        SYSTEMTIME st;
        if (FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
            std::printf("      mtime = %04d-%02d-%02d %02d:%02d:%02d\n",
                        st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond);
        }
    } else {
        std::printf("    .ell file: %s\n", ellPath);
        std::printf("      DOES NOT EXIST (GetFileAttributes failed: %lu)\n",
                    GetLastError());
    }
    // Also check for a .xml file with the same base name.
    char xmlPath[512];
    std::snprintf(xmlPath, sizeof(xmlPath), "%s\\elmodule_%d_%d.xml",
                  binDir.c_str(), whType, progId);
    if (GetFileAttributesExA(xmlPath, GetFileExInfoStandard, &fad)) {
        __int64 size = ((__int64)fad.nFileSizeHigh << 32) |
                       (__int64)fad.nFileSizeLow;
        std::printf("    .xml file: %s\n", xmlPath);
        std::printf("      EXISTS, size = %lld bytes\n", (long long)size);
    } else {
        std::printf("    .xml file: %s\n", xmlPath);
        std::printf("      DOES NOT EXIST\n");
    }
}

// -------------------------------------------------------------------------
// Log file tail reader (for WH_ERRS.LOG / EL_ERRS.LOG).
// -------------------------------------------------------------------------
void dumpLogTail(const char* logPath, int tailLines) {
    HANDLE h = CreateFileA(logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("    %s: NOT FOUND (or inaccessible)\n", logPath);
        return;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        std::printf("    %s: GetFileSize failed\n", logPath);
        CloseHandle(h);
        return;
    }
    std::printf("    %s: EXISTS, size = %lld bytes\n", logPath,
                (long long)size.QuadPart);
    if (size.QuadPart == 0) {
        std::printf("      (empty)\n");
        CloseHandle(h);
        return;
    }
    // Read the whole file (cap at 64KB to avoid huge logs).
    __int64 toRead = size.QuadPart;
    if (toRead > 65536) toRead = 65536;
    LARGE_INTEGER zero = {};
    LARGE_INTEGER newPos;
    newPos.QuadPart = size.QuadPart - toRead;
    SetFilePointerEx(h, newPos, nullptr, FILE_BEGIN);
    std::string buf((size_t)toRead, '\0');
    DWORD read = 0;
    if (!ReadFile(h, &buf[0], (DWORD)toRead, &read, nullptr) || read == 0) {
        std::printf("      (ReadFile failed: %lu)\n", GetLastError());
        CloseHandle(h);
        return;
    }
    buf.resize(read);
    CloseHandle(h);
    // Count newlines and print the last tailLines lines.
    int nlCount = 0;
    for (char c : buf) if (c == '\n') nlCount++;
    std::printf("      (showing last %d lines of the trailing %lld bytes)\n",
                tailLines, (long long)toRead);
    // Find the start of the tail.
    int nlSeen = 0;
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); i++) {
        if (buf[i] == '\n') {
            nlSeen++;
            if (nlSeen >= nlCount - tailLines + 1) {
                start = i + 1;
                break;
            }
        }
    }
    if (start >= buf.size()) start = 0;
    std::printf("      --- LOG TAIL ---\n");
    for (size_t i = start; i < buf.size(); i++) {
        std::fputc(buf[i], stdout);
    }
    std::printf("      --- END LOG TAIL ---\n");
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName = "TS_AUTO_M4_WHS_FUNC";
    const std::string binDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";
    const std::string logsDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\Logs";

    std::printf("=== diag_m4_whserver: warehouse state 0x38 (load .ell) "
                "failure diagnostic ===\n");
    std::printf("Purpose: investigate why the linker's state 0x38 (load "
                "compiled code from .ell)\n");
    std::printf("  fails in our CLI process, while verify (state 0xe) "
                "works.\n");
    std::printf("Warehouse fields are read live via Ordinal_10(0). The "
                "linker (ord 85) is\n");
    std::printf("  called directly with BOTH __stdcall and __fastcall "
                "conventions.\n");

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

    // --- Step 2: resolve exports.
    std::printf("\n[2] Resolving Ordinal_10 (warehouse getter), Ordinal_69, "
                "Ordinal_70, Ordinal_28, and ordinal 85 (linker)...\n");
    GetWarehouseFn getWarehouse = nullptr;
    Ordinal69Fn ord69 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    Ordinal28Fn ord28 = nullptr;
    LinkerStdcallFn linkerStd = nullptr;
    LinkerFastcallFn linkerFast = nullptr;
    void* ord85Raw = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        // Ordinal_10 is the warehouse getter. It is an internal function
        // (NOT exported by name) but IS exported by ordinal 10. Use
        // resolveExportByOrdinal(10).
        FARPROC ord10 = resolver.resolveExportByOrdinal(10);
        if (ord10) getWarehouse = reinterpret_cast<GetWarehouseFn>(ord10);
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) {
            ord85Raw = (void*)ord85;
            linkerStd  = reinterpret_cast<LinkerStdcallFn>(ord85);
            linkerFast = reinterpret_cast<LinkerFastcallFn>(ord85);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    Resolved: Ord10(GetWarehouse)=%p Ord69=%p Ord70=%p "
                "Ord28=%p Linker(85)=%p\n",
                (void*)getWarehouse, (void*)ord69, (void*)ord70,
                (void*)ord28, ord85Raw);
    if (!getWarehouse) {
        std::fprintf(stderr, "WARN: Ordinal_10 (GetWarehouse) NOT resolved "
                     "— warehouse inspection will be skipped\n");
    }
    if (!ord69) std::fprintf(stderr, "WARN: Ordinal_69 unresolved\n");
    if (!ord70) std::fprintf(stderr, "WARN: Ordinal_70 unresolved\n");
    if (!ord28) std::fprintf(stderr, "WARN: Ordinal_28 unresolved\n");
    if (!ord85Raw) std::fprintf(stderr, "WARN: ordinal 85 (linker) "
                                "unresolved — direct linker calls skipped\n");

    // --- Step 3: insert + verify the Function. ---
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

    // --- Step 5: get the warehouse pointer and read fields. ---
    std::printf("\n[5] Getting warehouse pointer via Ordinal_10(0)...\n");
    void* whPtr = nullptr;
    if (getWarehouse) {
        PtrSehResult whSeh;
        bool ok = callGetWarehouse(getWarehouse, 0, &whSeh);
        whPtr = whSeh.ptr;
        std::printf("    Ordinal_10(0): %s, warehouse=%p\n",
                    ok ? "OK (no SEH)" : "SEH raised", whPtr);
    } else {
        std::printf("    [SKIPPED: Ordinal_10 not resolved]\n");
    }

    if (whPtr) {
        WhFields f0 = readWhFields(whPtr);
        printWhFields("BEFORE any linker call (post-verify)", f0);
    }

    // --- Step 6: check the .ell file. ---
    std::printf("\n[6] Checking .ell file (Function whType=4, "
                "progId=%d)...\n", funcProgId);
    checkEllFileByProgId(binDir, /*whType=*/4, funcProgId);

    // --- Step 7: try calling the linker directly (ordinal 85). ---
    //   First, get a partial record buffer via Ordinal_28 to pass as
    //   partialBuf.
    std::printf("\n[7] Calling the linker (ordinal 85) directly...\n");
    unsigned char partialBuf[0x10A] = {};
    bool havePartial = false;
    if (ord28) {
        Ord28SehResult ord28Seh;
        bool ord28Ok = callOrd28(ord28, /*category=*/4, funcProgId,
                                 /*flag=*/0, partialBuf, &ord28Seh);
        std::printf("  Ordinal_28(4, %d, 0, buf): %s, ret=%u\n",
                    funcProgId, ord28Ok ? "OK" : "SEH", ord28Seh.result);
        if (ord28Ok && ord28Seh.result == 1) {
            havePartial = true;
            void* sa = nullptr;
            sehReadPtr(partialBuf + 0x26, &sa);
            int ec = 0;
            sehReadInt(partialBuf + 0x29, &ec);
            std::printf("    Partial copy +0x26 SafeArrayPtr=%p, "
                        "+0x29 elemCount=%d\n", sa, ec);
        }
    } else {
        std::printf("  [Ordinal_28 not resolved — partialBuf will be "
                    "NULL]\n");
    }

    // Helper to snapshot warehouse +0x4e and +0x26.
    auto snapWh = [&](const char* tag) {
        if (!whPtr) return;
        int hr = 0;
        void* sa = nullptr;
        sehReadInt(whPtr ? (const char*)whPtr + 0x4e : nullptr, &hr);
        sehReadPtr(whPtr ? (const char*)whPtr + 0x26 : nullptr, &sa);
        std::printf("    [snap %s] warehouse+0x4e=%d (0x%X)  "
                    "warehouse+0x26=%p\n",
                    tag, hr, (unsigned)hr, sa);
    };

    if (ord85Raw) {
        // --- 7a. __stdcall, errorFlag=0, partialBuf=NULL ---
        std::printf("\n  [7a] Linker __stdcall(4, %d, 0, NULL)...\n",
                    funcProgId);
        snapWh("before 7a");
        {
            PtrSehResult r;
            bool ok = callLinkerStdcall(linkerStd, 4, funcProgId, 0,
                                        nullptr, &r);
            std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
            snapWh("after 7a");
            if (ok && r.ptr) {
                std::printf("    [non-NULL return — releasing via "
                            "Ordinal_70]\n");
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }

        // --- 7b. __stdcall, errorFlag=0, partialBuf=partial ---
        std::printf("\n  [7b] Linker __stdcall(4, %d, 0, partialBuf)...\n",
                    funcProgId);
        snapWh("before 7b");
        {
            PtrSehResult r;
            bool ok = callLinkerStdcall(linkerStd, 4, funcProgId, 0,
                                        havePartial ? partialBuf : nullptr,
                                        &r);
            std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
            snapWh("after 7b");
            if (ok && r.ptr) {
                std::printf("    [non-NULL return — releasing via "
                            "Ordinal_70]\n");
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }

        // --- 7c. __fastcall, errorFlag=0, partialBuf=NULL ---
        std::printf("\n  [7c] Linker __fastcall(4, %d, 0, NULL)...\n",
                    funcProgId);
        snapWh("before 7c");
        {
            FastcallManualSehResult r;
            bool ok = callLinkerFastcallSafe(ord85Raw, 4, funcProgId, 0,
                                             nullptr, &r);
            std::printf("    call: %s, rec=%p\n",
                        ok ? "OK (no SEH)" : "SEH raised", r.ptr);
            snapWh("after 7c");
            if (ok && r.ptr) {
                std::printf("    [non-NULL return — releasing via "
                            "Ordinal_70]\n");
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }

        // --- 7d. __fastcall, errorFlag=0, partialBuf=partial ---
        std::printf("\n  [7d] Linker __fastcall(4, %d, 0, partialBuf)...\n",
                    funcProgId);
        snapWh("before 7d");
        {
            FastcallManualSehResult r;
            bool ok = callLinkerFastcallSafe(ord85Raw, 4, funcProgId, 0,
                                             havePartial ? partialBuf : nullptr,
                                             &r);
            std::printf("    call: %s, rec=%p\n",
                        ok ? "OK (no SEH)" : "SEH raised", r.ptr);
            snapWh("after 7d");
            if (ok && r.ptr) {
                std::printf("    [non-NULL return — releasing via "
                            "Ordinal_70]\n");
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }
    } else {
        std::printf("  [SKIPPED: ordinal 85 (linker) not resolved]\n");
    }

    // --- Step 8: try Ordinal_69 with param_3=progId (linker path). ---
    std::printf("\n[8] Calling Ordinal_69(4, %d, %d) — linker path "
                "(param_3=progId)...\n", funcProgId, funcProgId);
    if (ord69) {
        snapWh("before Ord69-linker");
        PtrSehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, funcProgId, &r);
        std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
        snapWh("after Ord69-linker");
        if (ok && r.ptr) {
            // Read record fields: +0x26, +0x29, +0x4f, +0x55, +0x7b.
            const unsigned char* rec = (const unsigned char*)r.ptr;
            void* sa = nullptr;  sehReadPtr(rec + 0x26, &sa);
            int ec = 0;          sehReadInt(rec + 0x29, &ec);
            void* eap = nullptr; sehReadPtr(rec + 0x4f, &eap);
            unsigned char vf = 0; sehReadByte(rec + 0x55, &vf);
            void* rdl = nullptr; sehReadPtr(rec + 0x7b, &rdl);
            std::printf("    Record +0x26 SafeArrayPtr=%p  [%s]\n",
                        sa, sa ? "NON-NULL" : "NULL");
            std::printf("    Record +0x29 elemCount=%d\n", ec);
            std::printf("    Record +0x4f elemArrayPtr=%p  [%s]\n",
                        eap, eap ? "NON-NULL" : "NULL");
            std::printf("    Record +0x55 verified flag=0x%02X\n", vf);
            std::printf("    Record +0x7b dep-list ptr=%p\n", rdl);
            // Release via Ordinal_70.
            IntSehResult r70;
            bool ok70 = callOrd70(ord70, r.ptr, 0, &r70);
            std::printf("    [release] Ordinal_70(rec=%p, 0): %s, "
                        "ret=%d\n", r.ptr, ok70 ? "OK" : "SEH",
                        r70.result);
        } else {
            std::printf("    [no record returned]\n");
        }
    } else {
        std::printf("    [SKIPPED: Ordinal_69 not resolved]\n");
    }

    // --- Step 9: check the .ell file AFTER the linker attempt. ---
    std::printf("\n[9] Checking .ell file AFTER linker attempt...\n");
    checkEllFileByProgId(binDir, /*whType=*/4, funcProgId);

    // --- Step 10: try with errorFlag=1 (emit error on failure). ---
    std::printf("\n[10] Trying linker with errorFlag=1 (emit error on "
                "failure)...\n");
    if (ord85Raw) {
        // __stdcall with errorFlag=1.
        std::printf("  [10a] Linker __stdcall(4, %d, 1, NULL)...\n",
                    funcProgId);
        snapWh("before 10a");
        {
            PtrSehResult r;
            bool ok = callLinkerStdcall(linkerStd, 4, funcProgId, 1,
                                        nullptr, &r);
            std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
            snapWh("after 10a");
            if (ok && r.ptr) {
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }
        // __fastcall with errorFlag=1.
        std::printf("  [10b] Linker __fastcall(4, %d, 1, NULL)...\n",
                    funcProgId);
        snapWh("before 10b");
        {
            FastcallManualSehResult r;
            bool ok = callLinkerFastcallSafe(ord85Raw, 4, funcProgId, 1,
                                             nullptr, &r);
            std::printf("    call: %s, rec=%p\n",
                        ok ? "OK (no SEH)" : "SEH raised", r.ptr);
            snapWh("after 10b");
            if (ok && r.ptr) {
                IntSehResult r70;
                callOrd70(ord70, r.ptr, 0, &r70);
                std::printf("    Ordinal_70: ret=%d\n", r70.result);
            }
        }
    } else {
        std::printf("  [SKIPPED: ordinal 85 (linker) not resolved]\n");
    }

    // --- Step 11: re-check WH_ERRS.LOG / EL_ERRS.LOG after errorFlag=1. ---
    std::printf("\n[11] Re-checking WH_ERRS.LOG / EL_ERRS.LOG after "
                "errorFlag=1...\n");
    {
        char whErrs[512];
        std::snprintf(whErrs, sizeof(whErrs), "%s\\WH_ERRS.LOG",
                      logsDir.c_str());
        char elErrs[512];
        std::snprintf(elErrs, sizeof(elErrs), "%s\\EL_ERRS.LOG",
                      logsDir.c_str());
        std::printf("  --- WH_ERRS.LOG ---\n");
        dumpLogTail(whErrs, /*tailLines=*/50);
        std::printf("  --- EL_ERRS.LOG ---\n");
        dumpLogTail(elErrs, /*tailLines=*/50);
    }

    // --- Final warehouse field snapshot. ---
    if (whPtr) {
        WhFields fEnd = readWhFields(whPtr);
        printWhFields("FINAL (after all linker attempts)", fEnd);
    }

    // --- Step 12: cleanup. ---
    std::printf("\n[12] Cleanup: delete Function...\n");
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId = %d\n", funcProgId);
    std::printf("Warehouse pointer = %p\n", whPtr);
    std::printf("\nKey questions:\n");
    std::printf("  1. Warehouse pointer and field values: see [5] and "
                "FINAL snapshot above.\n");
    std::printf("  2. Does the .ell file exist after verify? see [6] "
                "above.\n");
    std::printf("  3. Linker direct call results (both conventions): "
                "see [7] above.\n");
    std::printf("  4. warehouse+0x4e and +0x26 before/after each linker "
                "attempt: see [snap ...] lines.\n");
    std::printf("  5. Ordinal_69 with param_3=progId: see [8] above.\n");
    std::printf("  6. WH_ERRS.LOG / EL_ERRS.LOG errors: see [11] "
                "above.\n");
    std::printf("  7. Crash check: see [PRE]/[POST] TSCrashReport lines "
                "in stderr.\n");
    std::printf("  8. Cleanup status: see [12] above.\n");

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
    // 120-second hard timeout — insert+verify + multiple linker calls +
    // Ordinal_69 + log inspection.
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
