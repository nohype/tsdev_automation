// diag_m4_fwd_deps.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
// Historical investigation of the rejected GetDependencies/Option B2 path;
// current production reverse-dependent enumeration uses the linker and linked
// per-element dep-lists.
//
// Purpose: test whether the historical GetDependencies (orlib20 ord 487) and
// _GetStaticDependencyList@12 (ord 531) work correctly when a module
// ACTUALLY HAS dependencies — i.e. a Strategy that calls a Function.
//
// Prior diagnostics tested GetDependencies on a standalone Function with NO
// dependencies (it returned empty, correctly). This was the historical test
// for M.4's Option B2 (reverse-by-inverting-forward). It is retained as a
// rejected-path investigation; it is not the current M.4 implementation.
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB
//      stack worker thread.
//   2. Resolves GetDependencies, _GetStaticDependencyList@12,
//      _DeleteDependencyList@4, _GetMasterRecByType@16, Ordinal_69,
//      Ordinal_70, Ordinal_28.
//   3. Inserts + verifies a Function "TS_AUTO_M4_FWD_FUNC" with body
//      `TS_AUTO_M4_FWD_FUNC = 1;`.
//   4. Inserts + verifies a Strategy "TS_AUTO_M4_FWD_STRAT" that calls the
//      Function: `TS_AUTO_M4_FWD_STRAT = TS_AUTO_M4_FWD_FUNC;` (with
//      fallbacks if verification fails).
//   5. Records both ProgIds.
//   6. Calls GetDependencies on the Strategy (whType=0) — SEH-protected.
//   7. Calls GetDependencies on the Function (orlib20 whType=4) — expected
//      empty. User-created warehouse category is now 1.
//   8. Calls _GetStaticDependencyList@12 on the Strategy.
//   9. Calls _GetStaticDependencyList@12 on the Function.
//  10. Reads the 0x10A partial copy for the Strategy via Ordinal_28 — dumps
//      ALL fields at every 4-byte offset 0x00..0x10A, plus the named offsets
//      0x15, 0x26, 0x29, 0x4f, 0x55, 0x7b, 0xd3, 0xd6.
//  11. Reads the 0x10A partial copy for the Function via Ordinal_28.
//  12. Calls Ordinal_69 on the Strategy and reads +0x26, +0x29, +0x4f,
//      +0x55, +0x7b. Releases via Ordinal_70.
//  13. Calls Ordinal_69 on the Function and reads the same fields.
//  14. CRITICAL: tries GetDependencies with multiple calling conventions
//      (__stdcall 4 args, __cdecl 4 args, __stdcall 3 args, __cdecl 3 args)
//      to determine the actual ABI.
//  15. Cleanup: deletes both items. Reports TSCrashReport.exe if it spawned
//      without terminating it.
//
// All orlib20 calls are SEH-protected. The 90-second limit is an operational
// timeout for this historical diagnostic, not a measured result or M.4
// acceptance evidence.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

using namespace tsdev;
using diag_m4_process::ProcessInspectionResult;

namespace {

ProcessInspectionResult reportCrashReporter(const char* phase) {
    std::printf("[%s] Checking for TSCrashReport.exe...\n", phase);
    const ProcessInspectionResult result =
        diag_m4_process::inspectProcessByName(L"TSCrashReport.exe");
    if (result == ProcessInspectionResult::Found) {
        std::printf("[%s] TSCrashReport.exe IS running (crash report detected)\n",
                    phase);
    } else if (result == ProcessInspectionResult::NotFound) {
        std::printf("[%s] TSCrashReport.exe NOT running (no crash report detected)\n",
                    phase);
    } else {
        std::fprintf(stderr,
                     "[%s] FATAL: process enumeration inspection failure for "
                     "TSCrashReport.exe\n",
                     phase);
    }
    return result;
}

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// GetDependencies (ord 487, FUN_10153f00): the export name is undecorated
// ("GetDependencies"), which is ambiguous w.r.t. calling convention. We
// declare multiple variants and probe each:
//   - __stdcall 4 args (RET 0x10) — the assumed ABI from prior diags.
//   - __cdecl   4 args (caller cleans 16 bytes).
//   - __stdcall 3 args (RET 0x0C) — alternative ABI hypothesis.
//   - __cdecl   3 args (caller cleans 12 bytes).
//
// _GetStaticDependencyList@12 (ord 531): __stdcall, 3 args, RET 0x0C.
//   int* (int param_1, int param_2, int param_3)
//   Returns operator-new'd buffer; free via _DeleteDependencyList@4.
//
// _DeleteDependencyList@4 (ord 397): __stdcall, 1 arg, RET 0x04.
//   void (int* buf)
//
// _GetMasterRecByType@16 (ord 510): __stdcall, 4 args, RET 0x10.
//   uint (int category, int progId, int flag, void* outBuf)
//
// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args (caller cleans 12 bytes).
//   void* (char category, int progId, int flag)
//   Returns live record pointer (refcounted — release via Ordinal_70).
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args, RET 0x08.
//   int (void* rec, int flag)
//
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args, RET 0x10.
//   uint (int category, int progId, int flag, void* outBuf)
// -------------------------------------------------------------------------
typedef HGLOBAL (__stdcall *GetDependenciesStd4Fn)(int whType, int progId,
                                                   int elementIndex,
                                                   int* outDepCount);
typedef HGLOBAL (__cdecl   *GetDependenciesCdec4Fn)(int whType, int progId,
                                                    int elementIndex,
                                                    int* outDepCount);
typedef HGLOBAL (__stdcall *GetDependenciesStd3Fn)(int whType, int progId,
                                                   int* outDepCount);
typedef HGLOBAL (__cdecl   *GetDependenciesCdec3Fn)(int whType, int progId,
                                                    int* outDepCount);

typedef int*    (__stdcall *GetStaticDepListFn)(int param_1, int param_2,
                                                int param_3);
typedef void    (__stdcall *DeleteDepListFn)(int* buf);
typedef unsigned int (__stdcall *GetMasterRecByTypeFn)(int category,
                                                       int progId, int flag,
                                                       void* outBuf);
typedef void*   (__cdecl  *Ordinal69Fn)(char category, int progId, int flag);
typedef int     (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct GetDepsSehResult { HGLOBAL handle; int depCount; };

bool callGetDepsStd4(GetDependenciesStd4Fn fn, int whType, int progId,
                     int elementIndex, GetDepsSehResult* out) {
    out->handle = NULL; out->depCount = 0;
    __try { out->handle = fn(whType, progId, elementIndex, &out->depCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool callGetDepsCdec4(GetDependenciesCdec4Fn fn, int whType, int progId,
                      int elementIndex, GetDepsSehResult* out) {
    out->handle = NULL; out->depCount = 0;
    __try { out->handle = fn(whType, progId, elementIndex, &out->depCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool callGetDepsStd3(GetDependenciesStd3Fn fn, int whType, int progId,
                     GetDepsSehResult* out) {
    out->handle = NULL; out->depCount = 0;
    __try { out->handle = fn(whType, progId, &out->depCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool callGetDepsCdec3(GetDependenciesCdec3Fn fn, int whType, int progId,
                      GetDepsSehResult* out) {
    out->handle = NULL; out->depCount = 0;
    __try { out->handle = fn(whType, progId, &out->depCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct StaticDepSehResult { int* buf; };
bool callStaticDepList(GetStaticDepListFn fn, int p1, int p2, int p3,
                       StaticDepSehResult* out) {
    out->buf = nullptr;
    __try { out->buf = fn(p1, p2, p3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool callDeleteDepList(DeleteDepListFn fn, int* buf) {
    __try { fn(buf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct MasterRecSehResult { unsigned int result; };
bool callGetMasterRecByType(GetMasterRecByTypeFn fn, int category, int progId,
                            int flag, void* outBuf,
                            MasterRecSehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try { out->result = fn(category, progId, flag, outBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord69SehResult { void* rec; };
bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               Ord69SehResult* out) {
    out->rec = nullptr;
    __try { out->rec = fn(category, progId, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord70SehResult { int result; };
bool callOrd70(Ordinal70Fn fn, void* rec, int flag, Ord70SehResult* out) {
    out->result = -1;
    __try { out->result = fn(rec, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord28SehResult { unsigned int result; };
bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try { out->result = fn(category, progId, flag, outBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers (POD-only).
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
// EasyLanguage source builders.
//
// Function: body assigns a constant. The function name in the body MUST
// match the item name.
//
// Strategy: tries several forms that "call" the function. The primary form
// mirrors the user's spec: `STRAT = FUNC;`. If that fails to verify, the
// caller tries fallbacks.
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

// Primary Strategy source per the task spec.
std::string makeStrategySourcePrimary(const std::string& stratName,
                                      const std::string& funcName) {
    return stratName + " = " + funcName + ";\r\n";
}

// Fallback 1: assign to a local Value var (proven to compile in prior diags).
std::string makeStrategySourceValue1(const std::string& /*stratName*/,
                                     const std::string& funcName) {
    return std::string("{ calls ") + funcName + " }\r\n"
           "Value1 = " + funcName + ";\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

// Fallback 2: bare function reference statement.
std::string makeStrategySourceBare(const std::string& /*stratName*/,
                                   const std::string& funcName) {
    return std::string("{ calls ") + funcName + " }\r\n"
           "Value1 = " + funcName + ";\r\n";
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

// Insert + verify a disposable item. Returns ProgId on success, 0 on failure.
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

// Insert + verify a Strategy, trying multiple source forms in order.
int insertAndVerifyStrategyWithFallbacks(
        TsDevLib& lib, const std::string& stratName,
        const std::string& funcName) {
    struct { const char* label; std::string src; } forms[] = {
        { "primary (STRAT = FUNC;)",
          makeStrategySourcePrimary(stratName, funcName) },
        { "fallback1 (Value1 = FUNC; + Buy)",
          makeStrategySourceValue1(stratName, funcName) },
        { "fallback2 (Value1 = FUNC; no order)",
          makeStrategySourceBare(stratName, funcName) },
    };
    for (const auto& f : forms) {
        std::printf("\n  >> Trying Strategy source form: %s\n", f.label);
        int progId = insertAndVerify(lib, ComponentType::Strategy,
                                     stratName, f.src, "Strategy");
        if (progId > 0) {
            std::printf("  >> Strategy verified with form: %s\n", f.label);
            return progId;
        }
        std::printf("  >> Form %s failed; trying next.\n", f.label);
    }
    fail("All Strategy source forms failed verification");
    return 0;
}

// -------------------------------------------------------------------------
// Print a GetDependencies result (handle + depCount + dep array).
// Returns true if targetProgId was found in the dep array.
// -------------------------------------------------------------------------
bool printGetDepsResult(const GetDepsSehResult& r, int targetProgId,
                        const char* prefix) {
    std::printf("%s handle=%p depCount=%d", prefix, r.handle, r.depCount);
    if (r.handle == NULL || r.depCount <= 0) {
        std::printf(" (no deps)\n");
        if (r.handle) GlobalFree(r.handle);
        return false;
    }
    int* deps = (int*)GlobalLock(r.handle);
    if (!deps) {
        std::printf(" (GlobalLock FAILED)\n");
        GlobalFree(r.handle);
        return false;
    }
    bool matched = false;
    std::printf(" deps=[");
    for (int d = 0; d < r.depCount; d++) {
        if (d > 0) std::printf(", ");
        std::printf("%d", deps[d]);
        if (deps[d] == targetProgId) matched = true;
    }
    std::printf("]");
    if (matched) {
        std::printf("  <<< MATCH (targetProgId=%d found)", targetProgId);
    }
    std::printf("\n");
    GlobalUnlock(r.handle);
    GlobalFree(r.handle);
    return matched;
}

// -------------------------------------------------------------------------
// Probe GetDependencies across elementIndex 0..3 for one (whType, progId).
// Returns true if any call matched targetProgId.
// -------------------------------------------------------------------------
bool probeGetDepsStd4(GetDependenciesStd4Fn fn, const char* callerLabel,
                      int whType, int progId, int targetProgId) {
    std::printf("\n  --- GetDependencies[std4](%s) whType=0x%X progId=%d "
                "targetProgId=%d ---\n",
                callerLabel, whType, progId, targetProgId);
    bool anyMatch = false;
    for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
        GetDepsSehResult r;
        bool ok = callGetDepsStd4(fn, whType, progId, elemIdx, &r);
        if (!ok) {
            std::printf("    elemIdx=%d: SEH raised\n", elemIdx);
            continue;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "    elemIdx=%d:", elemIdx);
        if (printGetDepsResult(r, targetProgId, buf)) anyMatch = true;
    }
    std::printf("  >> RESULT[std4]: targetProgId=%d %s found in dep list of "
                "%s (whType=0x%X progId=%d)\n",
                targetProgId, anyMatch ? "WAS" : "NOT",
                callerLabel, whType, progId);
    return anyMatch;
}

// -------------------------------------------------------------------------
// _GetStaticDependencyList@12 probe.
//
// ABI (per docs/RE/RE_M4_DEPLIST_ABI.md and diag_m4_deps2.cpp):
//   int* __stdcall _GetStaticDependencyList@12(int param_1, int param_2,
//                                              int param_3);
//   - param_1: 0 = global tree walk; nonzero = single object pointer
//   - param_2: stored verbatim into header[0]
//   - param_3: stored verbatim into header[1]; also the lookup key
//   - Returns: int* buffer (operator new), NULL on failure
//   - Format: {int header0; int header1;
//              struct{int a; int b;} entries[N];
//              int term0=-1; int term1=-1;}
//   - Free with: void __stdcall _DeleteDependencyList@4(int* buf);
//
// We probe with param_1=0 (global tree walk), param_2=0, param_3=keyProgId.
// Walk entries until terminator {0xFFFFFFFF, 0xFFFFFFFF}.
// -------------------------------------------------------------------------
bool probeStaticDepList(GetStaticDepListFn getFn, DeleteDepListFn delFn,
                        const char* label, int keyProgId, int searchProgId) {
    std::printf("\n  --- _GetStaticDependencyList@12(%s) "
                "param_1=0 param_2=0 param_3(key)=%d searchProgId=%d ---\n",
                label, keyProgId, searchProgId);

    StaticDepSehResult r;
    bool ok = callStaticDepList(getFn, /*p1=*/0, /*p2=*/0,
                                /*p3=*/keyProgId, &r);
    if (!ok) {
        std::printf("    SEH raised\n");
        return false;
    }
    if (r.buf == nullptr) {
        std::printf("    returned NULL\n");
        return false;
    }
    int* p = r.buf;
    int header0 = 0, header1 = 0;
    sehReadInt(p + 0, &header0);
    sehReadInt(p + 1, &header1);
    std::printf("    header[0]=%d (0x%X)  header[1]=%d (0x%X)\n",
                header0, (unsigned)header0, header1, (unsigned)header1);

    bool anyMatch = false;
    int idx = 2;
    int entryCount = 0;
    while (true) {
        int a = 0, b = 0;
        if (!sehReadInt(p + idx, &a) || !sehReadInt(p + idx + 1, &b)) {
            std::printf("    [SEH reading entry[%d] at idx=%d]\n",
                        entryCount, idx);
            break;
        }
        if ((unsigned)a == 0xFFFFFFFFu && (unsigned)b == 0xFFFFFFFFu) {
            break;
        }
        bool matched = (a == searchProgId) || (b == searchProgId);
        std::printf("    entry[%d]: a=%d (0x%X)  b=%d (0x%X)%s\n",
                    entryCount, a, (unsigned)a, b, (unsigned)b,
                    matched ? "  <<< MATCH" : "");
        if (matched) anyMatch = true;
        idx += 2;
        entryCount++;
        if (entryCount > 4096) {
            std::printf("    [safety cap hit at 4096 entries, stopping]\n");
            break;
        }
    }
    std::printf("    total entries: %d\n", entryCount);
    if (anyMatch) {
        std::printf("  >> RESULT: searchProgId=%d WAS found in static dep "
                    "list (key=%d, %s)\n",
                    searchProgId, keyProgId, label);
    } else {
        std::printf("  >> RESULT: searchProgId=%d NOT found in static dep "
                    "list (key=%d, %s)\n",
                    searchProgId, keyProgId, label);
    }

    callDeleteDepList(delFn, r.buf);
    return anyMatch;
}

// -------------------------------------------------------------------------
// Dump a 0x10A-byte buffer as 4-byte int fields at every offset that is a
// multiple of 4, plus the named single-byte / short fields. Also prints a
// canonical hex dump.
// -------------------------------------------------------------------------
void dumpRecordBuffer(const unsigned char* buf, size_t n, const char* label) {
    std::printf("\n  --- HEX DUMP: %s (%zu bytes) ---\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        std::printf("  %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) std::printf("%02x ", buf[i + j]);
            else std::printf("   ");
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

    std::printf("\n  --- 4-BYTE INT FIELDS (%s) ---\n", label);
    for (size_t off = 0; off + 4 <= n; off += 4) {
        int v = 0;
        memcpy(&v, buf + off, 4);
        std::printf("    +0x%03zx (int): %d (0x%X)\n",
                    off, v, (unsigned)v);
    }

    std::printf("\n  --- NAMED FIELDS (%s) ---\n", label);
    struct { size_t off; const char* name; char fmt; } named[] = {
        { 0x15, "flags (byte)",        'B' },
        { 0x26, "SafeArray ptr",       'P' },
        { 0x29, "element count (int)", 'I' },
        { 0x4f, "element array ptr",   'P' },
        { 0x55, "verified flag (byte)",'B' },
        { 0x7b, "dep list ptr",        'P' },
        { 0xd3, "field 0xd3",          'I' },
        { 0xd6, "field 0xd6 (ptr)",    'P' },
    };
    for (const auto& f : named) {
        if (f.off >= n) continue;
        switch (f.fmt) {
            case 'B': {
                unsigned char v = buf[f.off];
                std::printf("    +0x%02zx %s: 0x%02X (%u)\n",
                            f.off, f.name, v, v);
                break;
            }
            case 'I': {
                if (f.off + 4 > n) break;
                int v = 0;
                memcpy(&v, buf + f.off, 4);
                std::printf("    +0x%02zx %s: %d (0x%X)\n",
                            f.off, f.name, v, (unsigned)v);
                break;
            }
            case 'P': {
                if (f.off + sizeof(void*) > n) break;
                void* v = nullptr;
                memcpy(&v, buf + f.off, sizeof(void*));
                std::printf("    +0x%02zx %s: %p  [%s]\n",
                            f.off, f.name, v,
                            v ? "NON-NULL" : "NULL");
                break;
            }
        }
    }
}

// -------------------------------------------------------------------------
// Read an Ordinal_69 live record's key fields and print them. Releases the
// record via Ordinal_70.
// -------------------------------------------------------------------------
void analyzeOrd69Record(void* recPtr, const char* label,
                        Ordinal70Fn ord70) {
    std::printf("\n=== Ordinal_69 LIVE RECORD: %s (rec=%p) ===\n",
                label, recPtr);
    if (!recPtr) {
        std::printf("  [record pointer is NULL]\n");
        return;
    }
    const unsigned char* rec = (const unsigned char*)recPtr;

    unsigned char f15 = 0; sehReadByte(rec + 0x15, &f15);
    void* p26 = nullptr; sehReadPtr(rec + 0x26, &p26);
    int i29 = 0; sehReadInt(rec + 0x29, &i29);
    void* p4f = nullptr; sehReadPtr(rec + 0x4f, &p4f);
    unsigned char b55 = 0; sehReadByte(rec + 0x55, &b55);
    void* p7b = nullptr; sehReadPtr(rec + 0x7b, &p7b);
    int ica = 0; sehReadInt(rec + 0xca, &ica);
    void* pd6 = nullptr; sehReadPtr(rec + 0xd6, &pd6);

    std::printf("    +0x15 flags (byte):       0x%02X (%u)\n", f15, f15);
    std::printf("    +0x26 SafeArray ptr:      %p  [%s]\n",
                p26, p26 ? "NON-NULL" : "NULL");
    std::printf("    +0x29 element count:      %d (0x%X)  [%s]\n",
                i29, (unsigned)i29, i29 ? "NONZERO" : "ZERO");
    std::printf("    +0x4f element array ptr:  %p  [%s]\n",
                p4f, p4f ? "NON-NULL" : "NULL");
    std::printf("    +0x55 verified flag:      0x%02X (%u)\n", b55, b55);
    std::printf("    +0x7b dep list ptr:       %p  [%s]\n",
                p7b, p7b ? "NON-NULL" : "NULL");
    std::printf("    +0xca refcount:           %d (0x%X)\n", ica, (unsigned)ica);
    std::printf("    +0xd6 pointer:            %p\n", pd6);

    if (ord70) {
        std::printf("\n  [Cleanup] Ordinal_70(rec=%p, 0)...\n", recPtr);
        Ord70SehResult r;
        bool ok = callOrd70(ord70, recPtr, 0, &r);
        std::printf("    Ordinal_70: %s, return=%d\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.result);
    } else {
        std::printf("\n  [Cleanup] Ordinal_70 NOT resolved — LEAKING.\n");
    }
    std::printf("=== END Ordinal_69 LIVE RECORD: %s ===\n", label);
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
    bool stratVerified = false;
    bool funcVerified = false;
    bool getDepsStratFoundFunc = false;
    bool getDepsFuncEmpty = false;
    bool staticDepStratFoundFunc = false;
    bool staticDepFuncEmpty = false;
    int stratProgId = 0;
    int funcProgId = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName  = "TS_AUTO_M4_FWD_FUNC";
    const std::string stratName = "TS_AUTO_M4_FWD_STRAT";

    std::printf("=== diag_m4_fwd_deps: GetDependencies / "
                "_GetStaticDependencyList forward-dep diagnostic ===\n");
    std::printf("Purpose: test whether GetDependencies and "
                "_GetStaticDependencyList@12 return the Function's\n");
    std::printf("  ProgId when called on a Strategy that calls the "
                "Function (i.e. when the module\n");
    std::printf("  ACTUALLY HAS dependencies).\n");

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

    // --- Step 2: resolve all exports. ---
    std::printf("\n[2] Resolving exports via OrlibResolver...\n");
    GetDependenciesStd4Fn  getDepsStd4  = nullptr;
    GetDependenciesCdec4Fn getDepsCdec4 = nullptr;
    GetDependenciesStd3Fn  getDepsStd3  = nullptr;
    GetDependenciesCdec3Fn getDepsCdec3 = nullptr;
    GetStaticDepListFn     getStaticDep = nullptr;
    DeleteDepListFn        deleteDep    = nullptr;
    GetMasterRecByTypeFn   getMasterRec = nullptr;
    Ordinal69Fn            ord69        = nullptr;
    Ordinal70Fn            ord70        = nullptr;
    Ordinal28Fn            ord28        = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        FARPROC p = resolver.resolveExport("GetDependencies");
        getDepsStd4  = reinterpret_cast<GetDependenciesStd4Fn>(p);
        getDepsCdec4 = reinterpret_cast<GetDependenciesCdec4Fn>(p);
        getDepsStd3  = reinterpret_cast<GetDependenciesStd3Fn>(p);
        getDepsCdec3 = reinterpret_cast<GetDependenciesCdec3Fn>(p);
        getStaticDep = reinterpret_cast<GetStaticDepListFn>(
            resolver.resolveExport("_GetStaticDependencyList@12"));
        deleteDep    = reinterpret_cast<DeleteDepListFn>(
            resolver.resolveExport("_DeleteDependencyList@4"));
        getMasterRec = reinterpret_cast<GetMasterRecByTypeFn>(
            resolver.resolveExport("_GetMasterRecByType@16"));
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    GetDependencies            = %p\n", (void*)getDepsStd4);
    std::printf("    _GetStaticDependencyList@12= %p\n", (void*)getStaticDep);
    std::printf("    _DeleteDependencyList@4    = %p\n", (void*)deleteDep);
    std::printf("    _GetMasterRecByType@16     = %p\n", (void*)getMasterRec);
    std::printf("    Ordinal_69                 = %p\n", (void*)ord69);
    std::printf("    Ordinal_70                 = %p\n", (void*)ord70);
    std::printf("    Ordinal_28                 = %p\n", (void*)ord28);
    if (!getDepsStd4) {
        std::fprintf(stderr, "FATAL: GetDependencies not resolved\n");
        st->exitCode = 103;
        return 0;
    }

    // --- Step 3: insert + verify the Function (callee). ---
    std::printf("\n[3] Insert + verify Function (callee) \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        st->exitCode = 200;
        return 0;
    }
    st->funcVerified = true;
    st->funcProgId = funcProgId;

    // --- Step 4: insert + verify the Strategy (caller) that calls the
    // Function. Try multiple source forms. ---
    std::printf("\n[4] Insert + verify Strategy (caller) \"%s\" calling "
                "Function (with fallbacks)...\n", stratName.c_str());
    int stratProgId =
        insertAndVerifyStrategyWithFallbacks(*lib, stratName, funcName);
    if (stratProgId <= 0) {
        lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        st->exitCode = 201;
        return 0;
    }
    st->stratVerified = true;
    st->stratProgId = stratProgId;

    std::printf("\n[5] Recorded ProgIds: Function=%d Strategy=%d\n",
                funcProgId, stratProgId);

    // --- Step 6: GetDependencies on the Strategy (whType=0). ---
    // The Strategy depends on the Function; we expect funcProgId in the
    // returned dep list.
    std::printf("\n[6] GetDependencies on the STRATEGY (whType=0, "
                "expecting Function ProgId=%d in dep list)...\n",
                funcProgId);
    bool stratMatched = probeGetDepsStd4(getDepsStd4, "Strategy",
                                         /*whType=*/0, stratProgId,
                                         funcProgId);
    // Also try whType=0x200 (Strategy power-type) and whType=4 (wrong type,
    // control) for completeness.
    if (probeGetDepsStd4(getDepsStd4, "Strategy-whType0x200",
                         /*whType=*/0x200, stratProgId, funcProgId)) {
        stratMatched = true;
    }
    probeGetDepsStd4(getDepsStd4, "Strategy-whType4-control",
                     /*whType=*/4, stratProgId, funcProgId);
    st->getDepsStratFoundFunc = stratMatched;

    // --- Step 7: GetDependencies on the Function (orlib20 whType=4; user-
    // created warehouse category is now 1). Expected empty (the leaf Function
    // has no deps). ---
    std::printf("\n[7] GetDependencies on the FUNCTION (orlib20 whType=4, "
                "expected empty)...\n");
    bool funcFoundAnything = false;
    for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
        GetDepsSehResult r;
        bool ok = callGetDepsStd4(getDepsStd4, /*whType=*/4, funcProgId,
                                  elemIdx, &r);
        if (!ok) {
            std::printf("    elemIdx=%d: SEH raised\n", elemIdx);
            continue;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "    elemIdx=%d:", elemIdx);
        std::printf("[Function orlib20 whType=4] ");
        if (printGetDepsResult(r, /*target=*/-1, buf)) funcFoundAnything = true;
        if (r.handle != NULL || r.depCount != 0) funcFoundAnything = true;
    }
    // Also try whType=0 for the Function (wrong type, control).
    for (int elemIdx = 0; elemIdx <= 1; elemIdx++) {
        GetDepsSehResult r;
        bool ok = callGetDepsStd4(getDepsStd4, /*whType=*/0, funcProgId,
                                  elemIdx, &r);
        if (!ok) continue;
        if (r.handle != NULL || r.depCount != 0) funcFoundAnything = true;
        if (r.handle) GlobalFree(r.handle);
    }
    st->getDepsFuncEmpty = !funcFoundAnything;
    std::printf("  >> Function GetDependencies: %s\n",
                st->getDepsFuncEmpty ? "EMPTY (expected)" :
                                       "NON-EMPTY (unexpected)");

    // --- Step 8: _GetStaticDependencyList@12 on the Strategy. ---
    // Key=strategyProgId (caller); search=funcProgId (callee). If the API
    // returns forward deps, the Function's ProgId should appear.
    std::printf("\n[8] _GetStaticDependencyList@12 on the STRATEGY "
                "(key=strategyProgId=%d, search=funcProgId=%d)...\n",
                stratProgId, funcProgId);
    if (getStaticDep && deleteDep) {
        st->staticDepStratFoundFunc = probeStaticDepList(
            getStaticDep, deleteDep, "Strategy-key",
            /*keyProgId=*/stratProgId, /*searchProgId=*/funcProgId);
    } else {
        std::printf("    [SKIPPED — _GetStaticDependencyList@12 or "
                    "_DeleteDependencyList@4 not resolved]\n");
    }

    // --- Step 9: _GetStaticDependencyList@12 on the Function. ---
    // Key=functionProgId (callee); search=strategyProgId (caller). If the
    // API returns reverse deps, the Strategy's ProgId should appear. We
    // also expect the function's own static dep list to be empty (a leaf
    // Function calls nothing).
    std::printf("\n[9] _GetStaticDependencyList@12 on the FUNCTION "
                "(key=funcProgId=%d, search=stratProgId=%d)...\n",
                funcProgId, stratProgId);
    if (getStaticDep && deleteDep) {
        bool funcFoundStrat = probeStaticDepList(
            getStaticDep, deleteDep, "Function-key",
            /*keyProgId=*/funcProgId, /*searchProgId=*/stratProgId);
        // Also report whether the function's static dep list is empty.
        // Re-probe and count entries (no search target).
        StaticDepSehResult r2;
        if (callStaticDepList(getStaticDep, 0, 0, funcProgId, &r2) &&
            r2.buf != nullptr) {
            int* p = r2.buf;
            int idx = 2, cnt = 0;
            while (cnt < 4096) {
                int a = 0, b = 0;
                if (!sehReadInt(p + idx, &a) ||
                    !sehReadInt(p + idx + 1, &b)) break;
                if ((unsigned)a == 0xFFFFFFFFu &&
                    (unsigned)b == 0xFFFFFFFFu) break;
                idx += 2; cnt++;
            }
            std::printf("  >> Function static dep list entry count: %d "
                        "(%s)\n", cnt, cnt == 0 ? "EMPTY (expected)" :
                                                  "NON-EMPTY");
            st->staticDepFuncEmpty = (cnt == 0);
            callDeleteDepList(deleteDep, r2.buf);
        }
        (void)funcFoundStrat;
    } else {
        std::printf("    [SKIPPED — exports not resolved]\n");
    }

    // --- Step 10: Ordinal_28 partial copy for the Strategy. ---
    // Ordinal_28(category=0, progId=stratProgId, flag=0, outBuf) writes a
    // 0x10A-byte COPY into outBuf and returns 1 if found.
    std::printf("\n[10] Ordinal_28 partial copy for the STRATEGY "
                "(category=0, progId=%d)...\n", stratProgId);
    if (ord28) {
        unsigned char buf28[0x10A] = {};
        Ord28SehResult r;
        bool ok = callOrd28(ord28, /*category=*/0, stratProgId,
                            /*flag=*/0, buf28, &r);
        std::printf("    Ordinal_28: %s, return=%u\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.result);
        if (ok && r.result == 1) {
            char hdr[128];
            std::snprintf(hdr, sizeof(hdr),
                          "Strategy Ordinal_28 copy (0x10A bytes)");
            dumpRecordBuffer(buf28, sizeof(buf28), hdr);
        } else {
            std::printf("    [Ordinal_28 returned %u — no copy]\n", r.result);
        }
    } else {
        std::printf("    [SKIPPED — Ordinal_28 not resolved]\n");
    }

    // --- Step 11: Ordinal_28 partial copy for the Function. ---
    std::printf("\n[11] Ordinal_28 partial copy for the FUNCTION "
                "(category=1, progId=%d)...\n", funcProgId);
    if (ord28) {
        unsigned char buf28[0x10A] = {};
        Ord28SehResult r;
        bool ok = callOrd28(ord28, /*category=*/1, funcProgId,
                            /*flag=*/0, buf28, &r);
        std::printf("    Ordinal_28: %s, return=%u\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.result);
        if (ok && r.result == 1) {
            char hdr[128];
            std::snprintf(hdr, sizeof(hdr),
                          "Function Ordinal_28 copy (0x10A bytes)");
            dumpRecordBuffer(buf28, sizeof(buf28), hdr);
        } else {
            std::printf("    [Ordinal_28 returned %u — no copy]\n", r.result);
        }
    } else {
        std::printf("    [SKIPPED — Ordinal_28 not resolved]\n");
    }

    // --- Step 12: Ordinal_69 live record for the Strategy. ---
    std::printf("\n[12] Ordinal_69 live record for the STRATEGY "
                "(category=0, progId=%d, flag=0)...\n", stratProgId);
    if (ord69) {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, /*category=*/(char)0, stratProgId,
                            /*flag=*/0, &r);
        std::printf("    Ordinal_69: %s, rec=%p\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.rec);
        if (ok && r.rec) {
            analyzeOrd69Record(r.rec, "STRATEGY", ord70);
        }
    } else {
        std::printf("    [SKIPPED — Ordinal_69 not resolved]\n");
    }

    // --- Step 13: Ordinal_69 live record for the Function.
    //     orlib20 still passes category=4 to Ordinal_69 for a Function
    //     (user-created warehouse category is now 1). ---
    std::printf("\n[13] Ordinal_69 live record for the FUNCTION "
                "(category=4, progId=%d, flag=0)...\n", funcProgId);
    if (ord69) {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, /*category=*/(char)4, funcProgId,
                            /*flag=*/0, &r);
        std::printf("    Ordinal_69: %s, rec=%p\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.rec);
        if (ok && r.rec) {
            analyzeOrd69Record(r.rec, "FUNCTION", ord70);
        }
    } else {
        std::printf("    [SKIPPED — Ordinal_69 not resolved]\n");
    }

    // --- Step 14: CRITICAL — try GetDependencies with different ABIs.
    // The export name "GetDependencies" is undecorated, which is ambiguous.
    // Try __cdecl 4, __stdcall 3, __cdecl 3 in addition to __stdcall 4.
    // For each variant, call on the Strategy (whType=0, progId=stratProgId)
    // at elemIdx=0 and report the result. The variant that returns a
    // non-empty dep list containing funcProgId is the correct ABI.
    //
    // NOTE: trying the wrong ABI can corrupt the stack. We attempt each
    // variant in a fresh SEH frame; if it raises SEH we move on. We also
    // restore ESP from EBP via a small asm stub is NOT possible here, so
    // we rely on the SEH frame + the fact that the worker thread has a
    // 64MB stack (lots of headroom). If the wrong ABI corrupts ESP such
    // that the SEH frame unwinder can't find the handler, the process may
    // crash — TSCrashReport.exe would then spawn, which we detect.
    std::printf("\n[14] CRITICAL: trying GetDependencies with different "
                "calling conventions on the Strategy...\n");
    std::printf("  (whType=0, progId=%d, elemIdx=0, target=%d)\n",
                stratProgId, funcProgId);

    // Variant A: __stdcall 4 args (already tested in step 6 — repeat for
    // a single elemIdx=0 call here for direct comparison).
    {
        GetDepsSehResult r;
        bool ok = callGetDepsStd4(getDepsStd4, 0, stratProgId, 0, &r);
        std::printf("  [A] __stdcall 4 args: %s", ok ? "OK" : "SEH");
        if (ok) {
            std::printf(" handle=%p depCount=%d", r.handle, r.depCount);
            if (r.handle && r.depCount > 0) {
                int* deps = (int*)GlobalLock(r.handle);
                if (deps) {
                    std::printf(" deps=[");
                    bool m = false;
                    for (int d = 0; d < r.depCount; d++) {
                        if (d) std::printf(",");
                        std::printf("%d", deps[d]);
                        if (deps[d] == funcProgId) m = true;
                    }
                    std::printf("]%s", m ? " <<<MATCH" : "");
                    GlobalUnlock(r.handle);
                }
            }
            if (r.handle) GlobalFree(r.handle);
        }
        std::printf("\n");
    }

    // Variant B: __cdecl 4 args. Caller cleans 16 bytes. If the real ABI
    // is __stdcall 4, this leaves 16 bytes on the stack — but the next
    // function call will rebalance, and the SEH frame is intact.
    {
        GetDepsSehResult r;
        bool ok = callGetDepsCdec4(getDepsCdec4, 0, stratProgId, 0, &r);
        std::printf("  [B] __cdecl   4 args: %s", ok ? "OK" : "SEH");
        if (ok) {
            std::printf(" handle=%p depCount=%d", r.handle, r.depCount);
            if (r.handle && r.depCount > 0) {
                int* deps = (int*)GlobalLock(r.handle);
                if (deps) {
                    std::printf(" deps=[");
                    bool m = false;
                    for (int d = 0; d < r.depCount; d++) {
                        if (d) std::printf(",");
                        std::printf("%d", deps[d]);
                        if (deps[d] == funcProgId) m = true;
                    }
                    std::printf("]%s", m ? " <<<MATCH" : "");
                    GlobalUnlock(r.handle);
                }
            }
            if (r.handle) GlobalFree(r.handle);
        }
        std::printf("\n");
    }

    // Variant C: __stdcall 3 args. If the real ABI is __stdcall 4, this
    // passes only 3 args — the 4th (outDepCount pointer) is whatever was
    // on the stack. The callee will RET 0x0C (popping 12 bytes) but we
    // pushed only 12, so ESP is restored correctly. The depCount write
    // may go to a garbage address — SEH should catch the resulting AV.
    {
        GetDepsSehResult r;
        bool ok = callGetDepsStd3(getDepsStd3, 0, stratProgId, &r);
        std::printf("  [C] __stdcall 3 args: %s", ok ? "OK" : "SEH");
        if (ok) {
            std::printf(" handle=%p depCount=%d", r.handle, r.depCount);
            if (r.handle) GlobalFree(r.handle);
        }
        std::printf("\n");
    }

    // Variant D: __cdecl 3 args. Caller cleans 12 bytes.
    {
        GetDepsSehResult r;
        bool ok = callGetDepsCdec3(getDepsCdec3, 0, stratProgId, &r);
        std::printf("  [D] __cdecl   3 args: %s", ok ? "OK" : "SEH");
        if (ok) {
            std::printf(" handle=%p depCount=%d", r.handle, r.depCount);
            if (r.handle) GlobalFree(r.handle);
        }
        std::printf("\n");
    }

    // --- Step 15: cleanup. ---
    std::printf("\n[15] Cleanup: delete Strategy + Function...\n");
    lib->deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId (callee) = %d\n", funcProgId);
    std::printf("Strategy ProgId (caller) = %d\n", stratProgId);
    std::printf("Strategy verified?       %s\n",
                st->stratVerified ? "YES" : "NO");
    std::printf("Function verified?        %s\n",
                st->funcVerified ? "YES" : "NO");
    std::printf("GetDependencies(Strategy) found Function? %s\n",
                st->getDepsStratFoundFunc ? "YES" : "NO");
    std::printf("GetDependencies(Function) empty?          %s\n",
                st->getDepsFuncEmpty ? "YES" : "NO (unexpected)");
    std::printf("_GetStaticDepList(Strategy-key) found Function? %s\n",
                st->staticDepStratFoundFunc ? "YES" : "NO");
    std::printf("_GetStaticDepList(Function-key) empty?         %s\n",
                st->staticDepFuncEmpty ? "YES" : "NO (unexpected)");

    std::printf("\n=== HISTORICAL DIAGNOSTIC OBSERVATION (not current M.4) ===\n");
    if (st->getDepsStratFoundFunc) {
        std::printf("  - Historical probe: GetDependencies returned the "
                    "Function's ProgId in the Strategy's dep list.\n");
        std::printf("  - Do not treat this as current Option B2 viability; M.4 "
                    "uses the linker-based path documented in section 8.7.\n");
    } else {
        std::printf("  - Historical probe: GetDependencies did not return the "
                    "Function's ProgId in the Strategy's dep list.\n");
        std::printf("  - The rejected Option B2 probe was not viable as-is; M.4 "
                    "uses the linker-based path documented in section 8.7.\n");
    }
    if (st->staticDepStratFoundFunc) {
        std::printf("  - Historical probe: _GetStaticDependencyList@12 returned "
                    "the Function's ProgId when keyed on the Strategy.\n");
        std::printf("  - This does not establish current M.4 viability; the "
                    "production path uses the linker.\n");
    }

    st->exitCode = (st->getDepsStratFoundFunc || st->staticDepStratFoundFunc)
                   ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was terminated.\n");
        return 97;
    }

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
    // 90-second hard timeout — report a hang without terminating the worker.
    // Multiple dependency probes may take longer than 60 seconds.
    DWORD wait = WaitForSingleObject(hThread, 90000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 90s; worker was "
                     "not forcibly terminated and no process cleanup was "
                     "attempted. Manual exact-PID recovery is required.\n");
        CloseHandle(hThread);
        return 98;
    }
    if (wait == WAIT_FAILED) {
        std::fprintf(stderr,
                     "\nFATAL: waiting for diagnostic worker failed: %lu; "
                     "no thread or process was forcibly terminated.\n",
                     GetLastError());
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    if (reportCrashReporter("POST") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be inspected after the "
                     "diagnostic; no process cleanup was attempted.\n");
        return 97;
    }

    return st.exitCode;
}
