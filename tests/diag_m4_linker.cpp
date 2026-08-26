// diag_m4_linker.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: test whether calling `Ordinal_69` (orlib20 ordinal 69,
// RVA 0x1A3E00, FUN_101a3e00) with `param_3 != 0` (the linker / short-circuit
// path) returns a record with populated element arrays.
//
// === ABI of Ordinal_69 (FUN_101a3e00) — Ghidra-confirmed ===
//
//   __cdecl, 3 args (char category, int progId, int flag).
//     param_3 == 0: full path. FUN_101cff60 builds a 0x10A partial copy,
//                   then FUN_101a3a90(1, local_154) links it. Returns the
//                   linked record (or 0 on failure).
//     param_3 != 0: SHORT-CIRCUIT. Calls FUN_101a3a90(1, param_3) directly,
//                   which checks warehouse+0x23d and either returns the
//                   cached live record or invokes the linker FUN_101a3340
//                   (ordinal 85).
//
// === Linker FUN_101a3340 (ordinal 85) — Ghidra-confirmed ===
//
//   __fastcall, 4 args (ECX=type, EDX=progId, stack: errorFlag, partialBuf).
//   Reads the SafeArray at record+0x26 (compiled object code), parses it,
//   and populates element arrays at +0x4f, +0x53, +0x57, +0x4b, +0x5b,
//   +0x67, +0x6f.
//
// === Ordinal_70 (FUN_1015df30) — refcount decrement ===
//   __stdcall, 2 args (rec, flag). Decrements +0xCA; on reach 0 frees
//   +0xD2 buffer and the record.
//
// === Ordinal_28 (FUN_101a03f0) — partial copy ===
//   __stdcall, 4 args (category, progId, flag, outBuf). Writes a 0x10A-byte
//   partial copy into outBuf. Returns 1 if found, 0 otherwise.
//
// === GetDependencies (ord 487) ===
//   __stdcall, 4 args (whType, progId, elementIndex, outDepCount).
//   Returns HGLOBAL of int dep ProgIds, or NULL.
//
// This diagnostic runs Tests A through G as described in the task brief:
//   A: Ordinal_69(category, progId, 0)         — normal path (control)
//   B: Ordinal_69(category, progId, progId)    — short-circuit / linker path
//   C: Ordinal_69(category, progId, 1)         — short-circuit with minimal
//                                                 non-zero value
//   D: Same A/B/C for the Strategy (category 0)
//   E: Direct call to linker (ordinal 85) — __fastcall
//   F: GetDependencies after the linker path
//   G: SafeArray at +0x26 right after verify (via Ordinal_28 partial copy)
//
// All orlib20 calls are SEH-protected (__try/__except). All raw pointer
// reads are SEH-protected. The diagnostic does NOT modify any production
// code; it only inserts + verifies + deletes test items.

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
// -------------------------------------------------------------------------
typedef void* (__cdecl  *Ordinal69Fn)(char category, int progId, int flag);
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                               int elementIndex,
                                               int* outDepCount);
// Linker FUN_101a3340 (ordinal 85): __fastcall — ECX=type, EDX=progId,
// stack: errorFlag (int), partialBuf (void*).
typedef void* (__fastcall *LinkerFn)(int type, int id, int errorFlag,
                                     void* partialBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct Ord69SehResult { void* rec; };
bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               Ord69SehResult* out) {
    out->rec = nullptr;
    __try { out->rec = fn(category, progId, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord70SehResult { int result; };
bool callOrd70(Ordinal70Fn fn, void* rec, int flag,
               Ord70SehResult* out) {
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

struct GetDependenciesSehResult { HGLOBAL handle; int depCount; };
bool callGetDependencies(GetDependenciesFn fn, int whType, int progId,
                         int elementIndex,
                         GetDependenciesSehResult* out) {
    out->handle = NULL;
    out->depCount = 0;
    __try { out->handle = fn(whType, progId, elementIndex, &out->depCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct LinkerSehResult { void* rec; };
bool callLinker(LinkerFn fn, int type, int id, int errorFlag,
                void* partialBuf, LinkerSehResult* out) {
    out->rec = nullptr;
    __try { out->rec = fn(type, id, errorFlag, partialBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
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
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

std::string makeStrategySourceCalling(const std::string& /*name*/,
                                      const std::string& calleeName) {
    return std::string("{ calls ") + calleeName + " }\r\n"
           "Value1 = " + calleeName + ";\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
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
// Field snapshot of a live record returned by Ordinal_69 / linker.
// Captures the key offsets requested by the brief.
// -------------------------------------------------------------------------
struct RecFields {
    short   firstShort;     // +0x00
    unsigned char flags15;  // +0x15
    void*   safeArrayPtr;   // +0x26
    int     elemCount;      // +0x29
    void*   elemArrayPtr;   // +0x4f
    unsigned char verified; // +0x55
    void*   recDepListPtr;  // +0x7b
    int     refcount;       // +0xCA
    void*   ptrD6;          // +0xD6
    bool    anySeh;
};

RecFields readFields(const void* recPtr) {
    RecFields f = {};
    f.anySeh = false;
    if (!recPtr) return f;
    const unsigned char* rec = (const unsigned char*)recPtr;
    if (!sehReadShort(rec + 0x00, &f.firstShort)) f.anySeh = true;
    if (!sehReadByte (rec + 0x15, &f.flags15))    f.anySeh = true;
    if (!sehReadPtr  (rec + 0x26, &f.safeArrayPtr)) f.anySeh = true;
    if (!sehReadInt  (rec + 0x29, &f.elemCount))  f.anySeh = true;
    if (!sehReadPtr  (rec + 0x4f, &f.elemArrayPtr)) f.anySeh = true;
    if (!sehReadByte (rec + 0x55, &f.verified))   f.anySeh = true;
    if (!sehReadPtr  (rec + 0x7b, &f.recDepListPtr)) f.anySeh = true;
    if (!sehReadInt  (rec + 0xCA, &f.refcount))   f.anySeh = true;
    if (!sehReadPtr  (rec + 0xD6, &f.ptrD6))      f.anySeh = true;
    return f;
}

void printFields(const char* label, const RecFields& f, int expectedProgId) {
    std::printf("\n  --- FIELD READS: %s ---\n", label);
    std::printf("    +0x00 first field (short): %d (0x%04X)  "
                "[expected ProgId=%d]\n",
                f.firstShort, (unsigned short)f.firstShort, expectedProgId);
    std::printf("    +0x15 flags (byte): 0x%02X\n", f.flags15);
    std::printf("    +0x26 SafeArray ptr: %p  [%s]\n",
                f.safeArrayPtr, f.safeArrayPtr ? "NON-NULL" : "NULL");
    std::printf("    +0x29 element count (int): %d (0x%X)  [%s]\n",
                f.elemCount, (unsigned)f.elemCount,
                f.elemCount == 0 ? "ZERO" : "NONZERO");
    std::printf("    +0x4f element array ptr: %p  [%s]\n",
                f.elemArrayPtr, f.elemArrayPtr ? "NON-NULL" : "NULL");
    std::printf("    +0x55 verified flag (byte): 0x%02X (%u)\n",
                f.verified, f.verified);
    std::printf("    +0x7b record-level dep-list ptr: %p  [%s]\n",
                f.recDepListPtr, f.recDepListPtr ? "NON-NULL" : "NULL");
    std::printf("    +0xCA refcount (int): %d (0x%X)\n",
                f.refcount, (unsigned)f.refcount);
    std::printf("    +0xD6 pointer: %p\n", f.ptrD6);
    if (f.anySeh) std::printf("    [one or more field reads raised SEH]\n");
}

// Returns true if the record has populated element arrays.
bool fieldsPopulated(const RecFields& f) {
    return f.elemCount > 0 && f.elemArrayPtr != nullptr;
}

// Release a refcounted record via Ordinal_70.
void releaseRec(Ordinal70Fn ord70, const char* label, void* rec) {
    if (!rec) return;
    if (ord70) {
        Ord70SehResult r;
        bool ok = callOrd70(ord70, rec, 0, &r);
        std::printf("    [release %s] Ordinal_70(rec=%p, 0): %s, ret=%d\n",
                    label, rec, ok ? "OK" : "SEH", r.result);
    } else {
        std::printf("    [release %s] Ordinal_70 NOT resolved — LEAKING "
                    "rec=%p\n", label, rec);
    }
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName  = "TS_AUTO_M4_LINKER_FUNC";
    const std::string stratName = "TS_AUTO_M4_LINKER_STRAT";

    std::printf("=== diag_m4_linker: Ordinal_69 param_3 != 0 (linker path) "
                "diagnostic ===\n");
    std::printf("Purpose: test whether Ordinal_69 with param_3 != 0 returns "
                "a record with\n");
    std::printf("  populated element arrays, vs. param_3 == 0 (normal "
                "path), and whether\n");
    std::printf("  calling the linker (ordinal 85) directly produces "
                "populated arrays.\n");

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
    std::printf("\n[2] Resolving Ordinal_69 (69), Ordinal_70 (70), "
                "Ordinal_28 (28), GetDependencies, and ordinal 85 "
                "(linker FUN_101a3340)...\n");
    Ordinal69Fn ord69 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    Ordinal28Fn ord28 = nullptr;
    GetDependenciesFn getDeps = nullptr;
    LinkerFn linker = nullptr;
    void* linkerAddr = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        getDeps = reinterpret_cast<GetDependenciesFn>(
            resolver.resolveExport("GetDependencies"));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) {
            linkerAddr = (void*)ord85;
            linker = reinterpret_cast<LinkerFn>(ord85);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!ord69) {
        std::fprintf(stderr, "FATAL: failed to resolve Ordinal_69\n");
        st->exitCode = 103;
        return 0;
    }
    if (!ord70) std::fprintf(stderr, "WARN: Ordinal_70 unresolved\n");
    if (!ord28) std::fprintf(stderr, "WARN: Ordinal_28 unresolved\n");
    if (!getDeps) std::fprintf(stderr, "WARN: GetDependencies unresolved\n");
    if (!linker) std::fprintf(stderr, "WARN: ordinal 85 (linker) "
                              "unresolved — Test E will be skipped\n");
    std::printf("    Resolved: Ord69=%p Ord70=%p Ord28=%p GetDeps=%p "
                "Linker(85)=%p\n",
                (void*)ord69, (void*)ord70, (void*)ord28, (void*)getDeps,
                linkerAddr);

    // --- Step 3: insert + verify the Function (callee). ---
    std::printf("\n[3] Insert + verify Function (callee) \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) { st->exitCode = 200; return 0; }

    // --- Step 4: insert + verify the Strategy (caller). ---
    std::printf("\n[4] Insert + verify Strategy (caller) \"%s\" calling "
                "Function...\n", stratName.c_str());
    int stratProgId = insertAndVerify(*lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceCalling(stratName,
                                                                funcName),
                                      "Strategy");
    if (stratProgId <= 0) {
        lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        st->exitCode = 201;
        return 0;
    }

    std::printf("\n[5] Recorded ProgIds: Function=%d Strategy=%d\n",
                funcProgId, stratProgId);

    // =====================================================================
    // TEST G: SafeArray at +0x26 right after verify (via Ordinal_28).
    // =====================================================================
    std::printf("\n=== TEST G: SafeArray at +0x26 right after verify "
                "(via Ordinal_28 partial copy) ===\n");
    if (ord28) {
        unsigned char ord28buf[0x10A] = {};
        Ord28SehResult ord28Seh;
        bool ord28Ok = callOrd28(ord28, /*category=*/4, funcProgId,
                                 /*flag=*/0, ord28buf, &ord28Seh);
        std::printf("  Ordinal_28(4, %d, 0, buf): %s, ret=%u\n",
                    funcProgId,
                    ord28Ok ? "OK" : "SEH", ord28Seh.result);
        if (ord28Ok && ord28Seh.result == 1) {
            void* sa = nullptr;
            sehReadPtr(ord28buf + 0x26, &sa);
            int ec = 0;
            sehReadInt(ord28buf + 0x29, &ec);
            void* eap = nullptr;
            sehReadPtr(ord28buf + 0x4f, &eap);
            std::printf("    Partial copy +0x26 SafeArrayPtr=%p  [%s]\n",
                        sa, sa ? "NON-NULL" : "NULL");
            std::printf("    Partial copy +0x29 elemCount=%d\n", ec);
            std::printf("    Partial copy +0x4f elemArrayPtr=%p\n", eap);
            std::printf("    >> _VerifyModule_New@20 %s the SafeArray in "
                        "the master record\n",
                        sa ? "DID populate" : "did NOT populate");
        } else {
            std::printf("    [Ordinal_28 did not return 1 — cannot "
                        "inspect partial copy]\n");
        }
    } else {
        std::printf("  [SKIPPED: Ordinal_28 not resolved]\n");
    }

    // =====================================================================
    // TESTS A/B/C for the FUNCTION (category 4).
    // =====================================================================
    RecFields fA_func, fB_func, fC_func;
    bool haveA_func = false, haveB_func = false, haveC_func = false;

    // --- Test A: Ordinal_69(4, funcProgId, 0) — normal path ---
    std::printf("\n=== TEST A (Function): Ordinal_69(4, %d, 0) — normal "
                "path ===\n", funcProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, 0, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fA_func = readFields(r.rec);
            printFields("Test A (Function, param_3=0)", fA_func, funcProgId);
            haveA_func = true;
            releaseRec(ord70, "Test A func", r.rec);
        } else {
            std::printf("  [no record returned — Test A func skipped]\n");
        }
    }

    // --- Test B: Ordinal_69(4, funcProgId, funcProgId) — short-circuit ---
    std::printf("\n=== TEST B (Function): Ordinal_69(4, %d, %d) — "
                "short-circuit / linker path ===\n",
                funcProgId, funcProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, funcProgId, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fB_func = readFields(r.rec);
            printFields("Test B (Function, param_3=progId)", fB_func,
                        funcProgId);
            haveB_func = true;
            releaseRec(ord70, "Test B func", r.rec);
        } else {
            std::printf("  [no record returned — Test B func skipped]\n");
        }
    }

    // --- Test C: Ordinal_69(4, funcProgId, 1) — minimal non-zero ---
    std::printf("\n=== TEST C (Function): Ordinal_69(4, %d, 1) — minimal "
                "non-zero ===\n", funcProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, 1, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fC_func = readFields(r.rec);
            printFields("Test C (Function, param_3=1)", fC_func, funcProgId);
            haveC_func = true;
            releaseRec(ord70, "Test C func", r.rec);
        } else {
            std::printf("  [no record returned — Test C func skipped]\n");
        }
    }

    // Compare A/B/C for the Function.
    std::printf("\n=== TESTS A/B/C COMPARISON (Function) ===\n");
    if (haveA_func) {
        std::printf("  Test A (param_3=0):       elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fA_func.elemCount, fA_func.elemArrayPtr,
                    fA_func.safeArrayPtr,
                    fieldsPopulated(fA_func) ? "YES" : "NO");
    } else { std::printf("  Test A: no record\n"); }
    if (haveB_func) {
        std::printf("  Test B (param_3=progId):  elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fB_func.elemCount, fB_func.elemArrayPtr,
                    fB_func.safeArrayPtr,
                    fieldsPopulated(fB_func) ? "YES" : "NO");
    } else { std::printf("  Test B: no record\n"); }
    if (haveC_func) {
        std::printf("  Test C (param_3=1):       elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fC_func.elemCount, fC_func.elemArrayPtr,
                    fC_func.safeArrayPtr,
                    fieldsPopulated(fC_func) ? "YES" : "NO");
    } else { std::printf("  Test C: no record\n"); }

    // =====================================================================
    // TESTS A/B/C for the STRATEGY (category 0).
    // =====================================================================
    RecFields fA_strat, fB_strat, fC_strat;
    bool haveA_strat = false, haveB_strat = false, haveC_strat = false;

    // --- Test A (Strategy) ---
    std::printf("\n=== TEST A (Strategy): Ordinal_69(0, %d, 0) — normal "
                "path ===\n", stratProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)0, stratProgId, 0, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fA_strat = readFields(r.rec);
            printFields("Test A (Strategy, param_3=0)", fA_strat,
                        stratProgId);
            haveA_strat = true;
            releaseRec(ord70, "Test A strat", r.rec);
        } else {
            std::printf("  [no record returned — Test A strat skipped]\n");
        }
    }

    // --- Test B (Strategy) ---
    std::printf("\n=== TEST B (Strategy): Ordinal_69(0, %d, %d) — "
                "short-circuit / linker path ===\n",
                stratProgId, stratProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)0, stratProgId, stratProgId, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fB_strat = readFields(r.rec);
            printFields("Test B (Strategy, param_3=progId)", fB_strat,
                        stratProgId);
            haveB_strat = true;
            releaseRec(ord70, "Test B strat", r.rec);
        } else {
            std::printf("  [no record returned — Test B strat skipped]\n");
        }
    }

    // --- Test C (Strategy) ---
    std::printf("\n=== TEST C (Strategy): Ordinal_69(0, %d, 1) — minimal "
                "non-zero ===\n", stratProgId);
    {
        Ord69SehResult r;
        bool ok = callOrd69(ord69, (char)0, stratProgId, 1, &r);
        std::printf("  call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
        if (ok && r.rec) {
            fC_strat = readFields(r.rec);
            printFields("Test C (Strategy, param_3=1)", fC_strat,
                        stratProgId);
            haveC_strat = true;
            releaseRec(ord70, "Test C strat", r.rec);
        } else {
            std::printf("  [no record returned — Test C strat skipped]\n");
        }
    }

    std::printf("\n=== TESTS A/B/C COMPARISON (Strategy) ===\n");
    if (haveA_strat) {
        std::printf("  Test A (param_3=0):       elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fA_strat.elemCount, fA_strat.elemArrayPtr,
                    fA_strat.safeArrayPtr,
                    fieldsPopulated(fA_strat) ? "YES" : "NO");
    } else { std::printf("  Test A: no record\n"); }
    if (haveB_strat) {
        std::printf("  Test B (param_3=progId):  elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fB_strat.elemCount, fB_strat.elemArrayPtr,
                    fB_strat.safeArrayPtr,
                    fieldsPopulated(fB_strat) ? "YES" : "NO");
    } else { std::printf("  Test B: no record\n"); }
    if (haveC_strat) {
        std::printf("  Test C (param_3=1):       elemCount=%d, "
                    "elemArray=%p, SafeArray=%p, populated=%s\n",
                    fC_strat.elemCount, fC_strat.elemArrayPtr,
                    fC_strat.safeArrayPtr,
                    fieldsPopulated(fC_strat) ? "YES" : "NO");
    } else { std::printf("  Test C: no record\n"); }

    // =====================================================================
    // TEST E: Direct call to the linker (ordinal 85).
    // =====================================================================
    std::printf("\n=== TEST E: Direct call to linker (ordinal 85, "
                "__fastcall) ===\n");
    RecFields fE_func, fE_strat;
    bool haveE_func = false, haveE_strat = false;
    if (linker) {
        // Function: type=4, id=funcProgId, errorFlag=0, partialBuf=NULL.
        std::printf("  [E-func] Linker(4, %d, 0, NULL)...\n", funcProgId);
        {
            LinkerSehResult r;
            bool ok = callLinker(linker, 4, funcProgId, 0, nullptr, &r);
            std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
            if (ok && r.rec) {
                fE_func = readFields(r.rec);
                printFields("Test E (Function, direct linker)", fE_func,
                            funcProgId);
                haveE_func = true;
                releaseRec(ord70, "Test E func", r.rec);
            } else {
                std::printf("    [no record returned]\n");
            }
        }
        // Strategy: type=0, id=stratProgId.
        std::printf("  [E-strat] Linker(0, %d, 0, NULL)...\n", stratProgId);
        {
            LinkerSehResult r;
            bool ok = callLinker(linker, 0, stratProgId, 0, nullptr, &r);
            std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.rec);
            if (ok && r.rec) {
                fE_strat = readFields(r.rec);
                printFields("Test E (Strategy, direct linker)", fE_strat,
                            stratProgId);
                haveE_strat = true;
                releaseRec(ord70, "Test E strat", r.rec);
            } else {
                std::printf("    [no record returned]\n");
            }
        }
        if (haveE_func) {
            std::printf("  >> Test E (Function, direct linker): "
                        "elemCount=%d elemArray=%p populated=%s\n",
                        fE_func.elemCount, fE_func.elemArrayPtr,
                        fieldsPopulated(fE_func) ? "YES" : "NO");
        }
        if (haveE_strat) {
            std::printf("  >> Test E (Strategy, direct linker): "
                        "elemCount=%d elemArray=%p populated=%s\n",
                        fE_strat.elemCount, fE_strat.elemArrayPtr,
                        fieldsPopulated(fE_strat) ? "YES" : "NO");
        }
    } else {
        std::printf("  [SKIPPED: ordinal 85 (linker) not resolved]\n");
    }

    // =====================================================================
    // TEST F: GetDependencies after the linker path.
    // =====================================================================
    std::printf("\n=== TEST F: GetDependencies after the linker path "
                "(Test B) ===\n");
    if (getDeps) {
        // Function whType = 4.
        for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
            GetDependenciesSehResult r;
            bool ok = callGetDependencies(getDeps, /*whType=*/4, funcProgId,
                                          elemIdx, &r);
            std::printf("  GetDeps(4, %d, elemIdx=%d): %s, handle=%p, "
                        "depCount=%d",
                        funcProgId, elemIdx, ok ? "OK" : "SEH",
                        r.handle, r.depCount);
            if (r.handle && r.depCount > 0) {
                int* deps = (int*)GlobalLock(r.handle);
                if (deps) {
                    std::printf(" deps=[");
                    for (int d = 0; d < r.depCount; d++) {
                        if (d > 0) std::printf(", ");
                        std::printf("%d", deps[d]);
                    }
                    std::printf("]");
                    GlobalUnlock(r.handle);
                }
            }
            std::printf("\n");
            if (r.handle) GlobalFree(r.handle);
        }
        // Strategy whType = 0.
        for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
            GetDependenciesSehResult r;
            bool ok = callGetDependencies(getDeps, /*whType=*/0, stratProgId,
                                          elemIdx, &r);
            std::printf("  GetDeps(0, %d, elemIdx=%d): %s, handle=%p, "
                        "depCount=%d",
                        stratProgId, elemIdx, ok ? "OK" : "SEH",
                        r.handle, r.depCount);
            if (r.handle && r.depCount > 0) {
                int* deps = (int*)GlobalLock(r.handle);
                if (deps) {
                    std::printf(" deps=[");
                    for (int d = 0; d < r.depCount; d++) {
                        if (d > 0) std::printf(", ");
                        std::printf("%d", deps[d]);
                        if (deps[d] == funcProgId)
                            std::printf("<<MATCH");
                    }
                    std::printf("]");
                    GlobalUnlock(r.handle);
                }
            }
            std::printf("\n");
            if (r.handle) GlobalFree(r.handle);
        }
    } else {
        std::printf("  [SKIPPED: GetDependencies not resolved]\n");
    }

    // =====================================================================
    // Cleanup.
    // =====================================================================
    std::printf("\n[Cleanup] Deleting Strategy + Function...\n");
    lib->deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // =====================================================================
    // Final summary.
    // =====================================================================
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId = %d\n", funcProgId);
    std::printf("Strategy ProgId = %d\n", stratProgId);
    std::printf("\nQuestion 2: Does Ordinal_69 with param_3 != 0 return a "
                "record with populated element arrays?\n");
    if (haveB_func) {
        std::printf("  Function (Test B, param_3=progId): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fB_func) ? "YES" : "NO",
                    fB_func.elemCount, fB_func.elemArrayPtr);
    } else { std::printf("  Function (Test B): no record returned\n"); }
    if (haveB_strat) {
        std::printf("  Strategy (Test B, param_3=progId): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fB_strat) ? "YES" : "NO",
                    fB_strat.elemCount, fB_strat.elemArrayPtr);
    } else { std::printf("  Strategy (Test B): no record returned\n"); }
    if (haveC_func) {
        std::printf("  Function (Test C, param_3=1): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fC_func) ? "YES" : "NO",
                    fC_func.elemCount, fC_func.elemArrayPtr);
    }
    if (haveC_strat) {
        std::printf("  Strategy (Test C, param_3=1): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fC_strat) ? "YES" : "NO",
                    fC_strat.elemCount, fC_strat.elemArrayPtr);
    }
    std::printf("\nQuestion 3: Does calling the linker directly (ord 85) "
                "return populated arrays?\n");
    if (haveE_func) {
        std::printf("  Function (Test E, direct linker): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fE_func) ? "YES" : "NO",
                    fE_func.elemCount, fE_func.elemArrayPtr);
    } else { std::printf("  Function (Test E): no record / linker "
                         "unresolved\n"); }
    if (haveE_strat) {
        std::printf("  Strategy (Test E, direct linker): populated=%s "
                    "(elemCount=%d, elemArray=%p)\n",
                    fieldsPopulated(fE_strat) ? "YES" : "NO",
                    fE_strat.elemCount, fE_strat.elemArrayPtr);
    } else { std::printf("  Strategy (Test E): no record / linker "
                         "unresolved\n"); }

    std::printf("\nQuestion 5: Does GetDependencies return non-empty "
                "results after the linker path?\n");
    std::printf("  See TEST F output above for depCount values per "
                "elemIdx.\n");

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

    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 120-second hard timeout — Tests A-G plus insert/verify of two items.
    DWORD wait = WaitForSingleObject(hThread, 120000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr, "\nFATAL: diagnostic timed out after 120s, "
                     "terminating.\n");
        TerminateThread(hThread, 99);
        system("taskkill /F /IM TSCrashReport.exe >NUL 2>&1");
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn?
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [POST] TSCrashReport.exe IS "
           "running (crash occurred) || echo [POST] TSCrashReport.exe NOT "
           "running (no crash)");

    return st.exitCode;
}
