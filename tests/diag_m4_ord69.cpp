// diag_m4_ord69.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
// Historical M.4 investigation; current production reverse-dependent
// enumeration uses the linker and linked per-element dep-lists.
//
// Historical purpose: verify the ABI of `Ordinal_69` (orlib20 ordinal 69,
// RVA 0x1A3E00, FUN_101a3e00) and compare its LIVE in-memory module record
// with the data surfaced by the forward `GetDependencies` export (ord 487)
// and `_GetMasterRecByType@16` (ord 568). The current M.4 production path
// uses the linker (ordinal 85) and linked per-element dep-lists.
//
// === ABI of Ordinal_69 (FUN_101a3e00) — Ghidra-confirmed ===
//
//   Calling convention: __cdecl (RET, no immediate; caller cleans stack).
//     Evidence: prologue `PUSH EBP; MOV EBP,ESP; SUB ESP,0x150`; epilogue
//     `MOV ESP,EBP; POP EBP; RET` (bare RET, no RET n). All 3 call sites in
//     orlib20 (FUN_10196440 @0x101967a8, @0x10196bb5, @0x10196d92;
//     GetDefaultText @0x10153bf1; _GetInputCount@8 @0x10153cea;
//     _GetInputNameEx@16 @0x10153dc6; FUN_10153e40 @0x10153ebb;
//     CELProgramInputs @0x101413c8) push 3 DWORDs and follow the CALL with
//     `ADD ESP,0xC` — caller cleanup of 12 bytes => 3 args, __cdecl.
//
//   Signature (decompiled):
//     undefined4 FUN_101a3e00(char param_1, undefined4 param_2, int param_3);
//
//   Parameters:
//     param_1 (char, [EBP+0x8]):  category byte. Compared against '2' (0x32);
//                if == 0x32 the function returns 0 immediately (a "skip"
//                category). Otherwise FUN_101d43a0(param_1, local_40) builds
//                a power-type array for the category. Frida trace shows
//                TSDev passing 0x4 (Function category) for a Function lookup.
//     param_2 (undefined4, [EBP+0xC]): ProgId (the warehouse index). Passed
//                to FUN_101cff60 as the lookup key. Frida shows 0x127 (295).
//     param_3 (int, [EBP+0x10]): flag. If NON-ZERO, the function takes a
//                SHORT-CIRCUIT path: it calls FUN_101a3a90(1, param_3) and
//                returns its result directly, skipping the
//                FUN_101bff20/FUN_101a9650/FUN_101cff60 dance. Frida shows
//                TSDev passing 0x0 (use the full master-record lookup path).
//
//   Return: undefined4 — a POINTER to the live in-memory master record
//     (FUN_101cff60's `param_4` out-buffer, allocated via operator_new(0x10A)
//     when the caller passes NULL, or the caller-supplied buffer otherwise).
//     Returns 0 on failure (record not found, warehouse unavailable, or
//     category == 0x32). The returned record is the SAME 0x10A-byte layout
//     as _GetMasterRecByType@16's copy BUT populated with the live element
//     array pointer at +0x4f, element count at +0x29, dep-list ptr at
//     element+0x7b, etc. — because FUN_101cff60 returns the in-memory ctx
//     record (or a freshly new'd 0x10A buffer that FUN_101a3a90 then
//     populates via the FUN_101a25a0/FUN_101a22c0/FUN_101a2430 linkers).
//
//   Memory ownership: REFCOUNTED. The returned record has a refcount at
//     +0xCA. FUN_101a3a90 (the short-circuit path) and FUN_101cff60 both
//     INCREMENT +0xCA before returning. The caller MUST decrement via
//     Ordinal_70 (FUN_1015df30, RVA 0x15DF30) — `FUN_1015df30(rec, 0)` —
//     which atomically decrements +0xCA and, when it hits 0, frees the
//     +0xD2 buffer and the record itself. DO NOT free() / GlobalFree() the
//     returned pointer. All 3 in-tree call sites in FUN_10196440 follow
//     their use of the record with `FUN_1015df30(iVar10, 0)` (visible at
//     0x101967d4-region and the GetDefaultText site at 0x10153c1a:
//     `PUSH 0; PUSH ESI; CALL 0x1015df30`).
//
//   Ordinal_70 (FUN_1015df30) ABI:
//     __stdcall, 2 args (rec, flag). RET 0x8. Decrements +0xCA; on reach 0
//     frees +0xD2 buffer and the record. The `flag` arg (param_2) is passed
//     through to inner cleanup but is empirically 0 at all call sites.
//
// === Ordinal_28 (FUN_101a03f0, RVA 0x1A03F0) — NOT a prerequisite ===
//
//   Ordinal_28 is a SIBLING helper, not a prerequisite. Its decompiled body
//   is structurally identical to Ordinal_69's full path
//   (FUN_101a9650 -> FUN_101d43a0 -> FUN_101cff60) but it returns a BOOL
//   (1 if a record was found, 0 otherwise) and writes the record into a
//   CALLER-SUPPLIED buffer (param_4) rather than returning a pointer.
//   Ordinal_28 takes 4 args (__stdcall, RET 0x10): (category, progId, flag,
//   outBuf). It does NOT touch any global state that Ordinal_69 depends on
//   (both call the same FUN_101a9650(0) warehouse getter, which is
//   self-initializing via FUN_101cf1f0). Calling Ordinal_69 alone is
//   sufficient — TSDev's Frida trace shows the call sequence
//   `Ordinal_69(0x4, 0x127, 0x0)` with NO preceding Ordinal_28.
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB
//      stack worker thread.
//   2. Resolves Ordinal_69 via OrlibResolver::resolveExportByOrdinal(69).
//   3. Resolves Ordinal_70 via resolveExportByOrdinal(70) (refcount-dec).
//   4. Resolves Ordinal_28 via resolveExportByOrdinal(28) (sibling, for
//      comparison — NOT a prerequisite, but probed for completeness).
//   5. Resolves GetDependencies (ord 487) by name (for the post-Ord69 probe).
//   6. Inserts + verifies a Function "TS_AUTO_M4_ORD69_FUNC" with source
//      `TS_AUTO_M4_ORD69_FUNC = 1;`.
//   7. Inserts + verifies a Strategy "TS_AUTO_M4_ORD69_STRAT" with source
//      that calls the Function.
//   8. Records both ProgIds.
//   9. Calls Ordinal_69(category=0, strategyProgId, 0) (SEH-protected) to
//      get the Strategy's LIVE record.
//  10. If successful, reads + prints the key offsets:
//        +0x00 first field, +0x29 element count, +0x4f element array ptr,
//        +0x7b dep list ptr (element 0), +0x26 SafeArray ptr.
//  11. If element count > 0 and element array ptr non-NULL, iterates
//      elements and reads +0x73 (dep-list len) and +0x7b (dep-list ptr),
//      scans the dep-record list for the historical 0xFA4 function-reference
//      expectation, prints any matching IDs, and checks if any matches the
//      Function's ProgId.
//  12. Historically probes GetDependencies on the Strategy AFTER Ordinal_69
//      to compare its forward-list result; this does not exercise the current
//      linker-based reverse-dependent implementation.
//  13. Cleanup: Ordinal_70(refcount-dec) the live record, then delete both
//      items. Report TSCrashReport.exe if it spawned; do not terminate it.
//
// All raw pointer reads are SEH-protected (__try/__except) because the
// record data could be partially invalid.
//
// RE ABI reference:
//   docs/RE/RE_M4_GETDEPENDENCIES_ABI.md (GetDependencies, FUN_10153ad0)
//   src/tsdev_lib.h (GetMasterRecByTypeFn typedef, _GetMasterRecByType@16)
//
// Ordinal_69 — __cdecl, 3 args (category:char, progId:int, flag:int).
//   Returns live record pointer (refcounted — release via Ordinal_70).

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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
// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args, caller cleans 12 bytes.
//   char  category   (pushed first / rightmost in __cdecl push order)
//   int   progId
//   int   flag
//   Returns void* (live record pointer, or NULL).
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args, RET 0x8.
//   void* rec
//   int   flag  (always 0 at in-tree call sites)
//   Returns int (1 if freed, 0 if just decremented).
//
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args, RET 0x10.
//   int   category
//   int   progId
//   int   flag
//   void* outBuf  (caller-supplied 0x10A-byte buffer)
//   Returns uint (1 if record found and copied, 0 otherwise).
//
// GetDependencies (ord 487): __stdcall, 4 args, RET 0x10.
//   int    whType
//   int    progId
//   int    elementIndex
//   int*   outDepCount
//   Returns HGLOBAL (GMEM_MOVEABLE) of int dep ProgIds, or NULL.
// -------------------------------------------------------------------------
typedef void* (__cdecl *Ordinal69Fn)(char category, int progId, int flag);
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                               int elementIndex,
                                               int* outDepCount);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct Ord69SehResult {
    void* rec;  // NULL if SEH or function returned NULL
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
    int result;  // -1 if SEH
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

struct GetDependenciesSehResult {
    HGLOBAL handle;
    int depCount;
};

bool callGetDependencies(GetDependenciesFn fn, int whType, int progId,
                         int elementIndex,
                         GetDependenciesSehResult* out) {
    out->handle = NULL;
    out->depCount = 0;
    __try {
        out->handle = fn(whType, progId, elementIndex, &out->depCount);
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
// EasyLanguage source builders (mirror diag_m4_deps.cpp / diag_m4_raw_rec
// patterns).
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
// Analyze a live record returned by Ordinal_69 (historical diagnostic path).
//
// Field layout (per Frida MODREC_FIELDS trace + Ghidra FUN_10153ad0 /
//   FUN_10153980 reads):
//   +0x00: first field (short — ProgId)
//   +0x15: byte flags
//   +0x26: SafeArray pointer
//   +0x29: element count (int)        <<< KEY
//   +0x4f: element array pointer      <<< KEY
//   +0x55: verified flag (byte)
//   +0x7b: dep list pointer (per-element; see below)
//   +0xca: refcount (int)             <<< Ordinal_70 decrements this
//   +0xd6: pointer
//
// Each element (stride 0xb5 from the base at +0x4f):
//   +0x73: dep-record list length (int)
//   +0x7b: dep-record list pointer (int*)
// -------------------------------------------------------------------------
void analyzeLiveRecord(const void* recPtr, const char* label,
                       int expectedProgId, int targetFuncProgId,
                       Ordinal70Fn ord70) {
    std::printf("\n=== LIVE RECORD ANALYSIS: %s ===\n", label);
    if (!recPtr) {
        std::printf("  [record pointer is NULL — nothing to analyze]\n");
        return;
    }

    const unsigned char* rec = (const unsigned char*)recPtr;

    // 1. Hex dump first 0x10A bytes (the master record size).
    unsigned char snap[0x10A] = {};
    if (sehMemcpy(rec, snap, sizeof(snap))) {
        hexDump(snap, sizeof(snap), label);
    } else {
        std::printf("  [SEH copying 0x10A bytes from rec=%p]\n", recPtr);
    }

    // 2. Field reads (all SEH-protected).
    std::printf("\n  --- FIELD READS ---\n");

    // +0x00: first field (short — ProgId)
    short firstShort = 0;
    bool okFs = sehReadShort(rec + 0x00, &firstShort);
    std::printf("    +0x00 first field (short): %s = %d (0x%04X)  "
                "[expected ProgId=%d]\n",
                okFs ? "OK" : "<SEH>", firstShort,
                (unsigned short)firstShort, expectedProgId);

    // +0x15: byte flags
    unsigned char flags15 = 0;
    bool okF15 = sehReadByte(rec + 0x15, &flags15);
    std::printf("    +0x15 flags (byte): %s = 0x%02X\n",
                okF15 ? "OK" : "<SEH>", flags15);

    // +0x26: SafeArray pointer
    void* safeArrayPtr = nullptr;
    bool okSa = sehReadPtr(rec + 0x26, &safeArrayPtr);
    std::printf("    +0x26 SafeArray ptr: %s = %p  [%s]\n",
                okSa ? "OK" : "<SEH>", safeArrayPtr,
                (safeArrayPtr != nullptr) ? "NON-NULL" : "NULL");

    // +0x29: element count (int) — KEY FIELD
    int elemCount = 0;
    bool okEc = sehReadInt(rec + 0x29, &elemCount);
    std::printf("    +0x29 element count (int): %s = %d (0x%X)  "
                "[>>> ZERO OR NONZERO? %s <<<]\n",
                okEc ? "OK" : "<SEH>", elemCount, (unsigned)elemCount,
                (elemCount == 0) ? "ZERO" : "NONZERO");

    // +0x4f: element array pointer — KEY FIELD
    void* elemArrayPtr = nullptr;
    bool okEap = sehReadPtr(rec + 0x4f, &elemArrayPtr);
    std::printf("    +0x4f element array ptr: %s = %p  "
                "[>>> NULL OR NON-NULL? %s <<<]\n",
                okEap ? "OK" : "<SEH>", elemArrayPtr,
                (elemArrayPtr == nullptr) ? "NULL" : "NON-NULL");

    // +0x55: verified flag (byte)
    unsigned char verified = 0;
    bool okVf = sehReadByte(rec + 0x55, &verified);
    std::printf("    +0x55 verified flag (byte): %s = 0x%02X (%u)\n",
                okVf ? "OK" : "<SEH>", verified, verified);

    // +0x7b: dep list pointer (record-level — note: per the Frida trace,
    //   +0x7b is read at the RECORD level for some records, but the
    //   canonical dep-list walk in FUN_10153ad0 reads +0x7b at the
    //   ELEMENT level, stride 0xb5. We read both for completeness.)
    void* recDepListPtr = nullptr;
    bool okRdl = sehReadPtr(rec + 0x7b, &recDepListPtr);
    std::printf("    +0x7b record-level dep-list ptr: %s = %p  [%s]\n",
                okRdl ? "OK" : "<SEH>", recDepListPtr,
                (recDepListPtr != nullptr) ? "NON-NULL" : "NULL");

    // +0xca: refcount (int)
    int refcount = 0;
    bool okRc = sehReadInt(rec + 0xca, &refcount);
    std::printf("    +0xca refcount (int): %s = %d (0x%X)\n",
                okRc ? "OK" : "<SEH>", refcount, (unsigned)refcount);

    // +0xd6: pointer
    void* ptrD6 = nullptr;
    bool okD6 = sehReadPtr(rec + 0xd6, &ptrD6);
    std::printf("    +0xd6 pointer: %s = %p\n",
                okD6 ? "OK" : "<SEH>", ptrD6);

    // 3. Element iteration.
    int totalFa4Found = 0;
    bool anyFuncRefMatchesTarget = false;
    if (elemCount > 0 && elemCount < 1024 && elemArrayPtr != nullptr) {
        std::printf("\n  --- ELEMENT ITERATION (%d elements, stride 0xb5) "
                    "---\n", elemCount);
        const unsigned char* base = (const unsigned char*)elemArrayPtr;
        for (int i = 0; i < elemCount; i++) {
            const unsigned char* elem = base + (size_t)i * 0xb5;
            std::printf("\n    element[%d] @ %p (offset 0x%zx from base):\n",
                        i, (void*)elem, (size_t)i * 0xb5);

            // +0x73: dep-record list length (int)
            int depListLen = 0;
            bool okDll = sehReadInt(elem + 0x73, &depListLen);
            std::printf("      +0x73 dep-list length (int): %s = %d (0x%X)\n",
                        okDll ? "OK" : "<SEH>", depListLen,
                        (unsigned)depListLen);

            // +0x7b: dep-record list pointer (int*)
            void* depListPtr = nullptr;
            bool okDlp = sehReadPtr(elem + 0x7b, &depListPtr);
            std::printf("      +0x7b dep-list ptr: %s = %p\n",
                        okDlp ? "OK" : "<SEH>", depListPtr);

            if (depListPtr != nullptr && depListLen > 0 &&
                depListLen < 0x100000) {
                // Print the first few bytes of the dep-record list.
                size_t dumpLen = (size_t)depListLen;
                if (dumpLen > 64) dumpLen = 64;
                unsigned char tmp[64];
                if (sehMemcpy(depListPtr, tmp, dumpLen)) {
                    char hdr[96];
                    std::snprintf(hdr, sizeof(hdr),
                                  "dep-list element[%d] (first %zu bytes)",
                                  i, dumpLen);
                    hexDump(tmp, dumpLen, hdr);
                } else {
                    std::printf("      [SEH reading dep-list bytes]\n");
                }

                // Scan the dep-record list for the historical 0xFA4
                // function-reference expectation. Per FUN_10153ad0 /
                // FUN_1013a020: each dep record starts with an int opcode; the
                // record stride is *(byte*)(cur+1).
                // We walk until we've consumed depListLen bytes.
                const unsigned char* cur = (const unsigned char*)depListPtr;
                size_t consumed = 0;
                int recIdx = 0;
                int scanCap = 256;
                int elemFa4 = 0;
                while (consumed + 4 <= (size_t)depListLen && scanCap-- > 0) {
                    int opcode = 0;
                    if (!sehReadInt(cur, &opcode)) {
                        std::printf("      [SEH reading opcode at "
                                    "consumed=%zu]\n", consumed);
                        break;
                    }
                    unsigned char stride = 0;
                    if (!sehReadByte(cur + 1, &stride)) {
                        std::printf("      [SEH reading stride at "
                                    "consumed=%zu]\n", consumed);
                        break;
                    }
                    if (opcode == 0xFA4) {
                        elemFa4++;
                        totalFa4Found++;
                        // Function id is at cur + 0xd per FUN_10153ad0.
                        int funcId = 0;
                        bool okFid = sehReadInt(cur + 0xd, &funcId);
                        bool matches = (okFid && funcId == targetFuncProgId);
                        if (matches) anyFuncRefMatchesTarget = true;
                        std::printf("      >>> 0xFA4 (function ref) at "
                                    "dep-rec[%d] @ %p: funcId=%s = %d "
                                    "(0x%X), stride=%u%s\n",
                                    recIdx, (void*)cur,
                                    okFid ? "OK" : "<SEH>",
                                    funcId, (unsigned)funcId, stride,
                                    matches ? "  <<< MATCHES target "
                                              "Function ProgId" : "");
                    }
                    if (stride == 0) {
                        std::printf("      [zero stride at dep-rec[%d] @ %p "
                                    "(opcode=0x%X) — stopping scan]\n",
                                    recIdx, (void*)cur, (unsigned)opcode);
                        break;
                    }
                    cur += stride;
                    consumed += stride;
                    recIdx++;
                }
                std::printf("      dep-rec scan: %d records walked, "
                            "%d 0xFA4 opcodes found\n", recIdx, elemFa4);
            } else {
                std::printf("      [dep-list empty or ptr NULL — no scan]\n");
            }
        }
        std::printf("\n  --- ELEMENT ITERATION SUMMARY: %d 0xFA4 opcodes "
                    "found across all elements ---\n", totalFa4Found);
    } else {
        std::printf("\n  [Skipping element iteration: elemCount=%d "
                    "(%s), elemArrayPtr=%p (%s)]\n",
                    elemCount,
                    (elemCount > 0 && elemCount < 1024) ? "valid" : "invalid",
                    elemArrayPtr,
                    (elemArrayPtr != nullptr) ? "non-NULL" : "NULL");
    }

    std::printf("\n  --- 0xFA4 FUNCTION-REF MATCH ---\n");
    std::printf("    target Function ProgId = %d\n", targetFuncProgId);
    std::printf("    any 0xFA4 funcRef matches target? %s\n",
                anyFuncRefMatchesTarget ? "YES" : "NO");

    std::printf("\n=== END LIVE RECORD ANALYSIS: %s ===\n", label);

    // 4. Refcount-decrement the live record via Ordinal_70.
    //    The record was refcount-incremented by Ordinal_69's internals
    //    (FUN_101cff60 increments +0xCA before returning). We MUST release.
    if (ord70) {
        std::printf("\n  [Cleanup] Calling Ordinal_70(rec=%p, 0) to "
                    "refcount-decrement the live record...\n", recPtr);
        Ord70SehResult ord70Seh;
        bool ord70Ok = callOrd70(ord70, const_cast<void*>(recPtr), 0,
                                 &ord70Seh);
        std::printf("    Ordinal_70: %s, return=%d\n",
                    ord70Ok ? "OK (no SEH)" : "SEH raised",
                    ord70Seh.result);
    } else {
        std::printf("\n  [Cleanup] Ordinal_70 NOT resolved — LEAKING the "
                    "live record (refcount not decremented).\n");
    }
}

// -------------------------------------------------------------------------
// Probe GetDependencies on a (whType, progId) for elemIdx 0..3.
// Returns true if any call matched targetProgId.
// -------------------------------------------------------------------------
bool probeGetDependencies(GetDependenciesFn fn, const char* callerLabel,
                          int whType, int progId, int targetProgId) {
    std::printf("\n  --- GetDependencies(%s) whType=0x%X progId=%d "
                "targetProgId=%d ---\n",
                callerLabel, whType, progId, targetProgId);
    bool anyMatch = false;
    for (int elemIdx = 0; elemIdx <= 3; elemIdx++) {
        GetDependenciesSehResult depSeh;
        bool ok = callGetDependencies(fn, whType, progId, elemIdx, &depSeh);
        if (!ok) {
            std::printf("    elemIdx=%d: SEH raised\n", elemIdx);
            continue;
        }
        std::printf("    elemIdx=%d: handle=%p depCount=%d",
                    elemIdx, depSeh.handle, depSeh.depCount);
        if (depSeh.handle == NULL || depSeh.depCount <= 0) {
            std::printf(" (no deps / out-of-range)\n");
            if (depSeh.handle) GlobalFree(depSeh.handle);
            continue;
        }
        int* deps = (int*)GlobalLock(depSeh.handle);
        if (!deps) {
            std::printf(" (GlobalLock FAILED)\n");
            GlobalFree(depSeh.handle);
            continue;
        }
        bool matched = false;
        std::printf(" deps=[");
        for (int d = 0; d < depSeh.depCount; d++) {
            if (d > 0) std::printf(", ");
            std::printf("%d", deps[d]);
            if (deps[d] == targetProgId) {
                matched = true;
                anyMatch = true;
            }
        }
        std::printf("]");
        if (matched) {
            std::printf("  <<< MATCH (targetProgId=%d found)", targetProgId);
        }
        std::printf("\n");
        GlobalUnlock(depSeh.handle);
        GlobalFree(depSeh.handle);
    }
    if (anyMatch) {
        std::printf("  >> RESULT: targetProgId=%d WAS found in dep list of "
                    "%s (whType=0x%X progId=%d)\n",
                    targetProgId, callerLabel, whType, progId);
    } else {
        std::printf("  >> RESULT: targetProgId=%d NOT found in dep list of "
                    "%s (whType=0x%X progId=%d)\n",
                    targetProgId, callerLabel, whType, progId);
    }
    return anyMatch;
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName   = "TS_AUTO_M4_ORD69_FUNC";
    const std::string stratName  = "TS_AUTO_M4_ORD69_STRAT";

    std::printf("=== diag_m4_ord69: Ordinal_69 ABI + live-record diagnostic "
                "===\n");
    std::printf("Purpose: verify Ordinal_69 (orlib20 ord 69, RVA 0x1A3E00) "
                "returns a LIVE\n");
    std::printf("  in-memory module record with populated element arrays "
                "and dep lists for a\n");
    std::printf("  Strategy that calls a Function.\n");
    std::printf("ABI (Ghidra-confirmed): __cdecl, 3 args "
                "(category:char, progId:int, flag:int),\n");
    std::printf("  returns live record pointer (refcounted — release via "
                "Ordinal_70).\n");

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

    // --- Step 2: resolve Ordinal_69, Ordinal_70, Ordinal_28, GetDependencies.
    std::printf("\n[2] Resolving Ordinal_69 (ord 69), Ordinal_70 (ord 70), "
                "Ordinal_28 (ord 28), GetDependencies (ord 487)...\n");
    Ordinal69Fn ord69 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    Ordinal28Fn ord28 = nullptr;
    GetDependenciesFn getDeps = nullptr;
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
    if (!ord70) {
        std::fprintf(stderr, "WARN: failed to resolve Ordinal_70 — live "
                     "record will LEAK (refcount not decremented)\n");
    }
    if (!ord28) {
        std::fprintf(stderr, "WARN: failed to resolve Ordinal_28 — "
                     "sibling probe will be skipped\n");
    }
    if (!getDeps) {
        std::fprintf(stderr, "WARN: failed to resolve GetDependencies — "
                     "post-Ord69 probe will be skipped\n");
    }
    std::printf("    Resolved: Ordinal_69=%p Ordinal_70=%p Ordinal_28=%p "
                "GetDependencies=%p\n",
                (void*)ord69, (void*)ord70, (void*)ord28, (void*)getDeps);

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

    // --- Step 4: insert + verify the Strategy (caller) that calls the
    // Function. ---
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

    // --- Step 6: call Ordinal_69 for the STRATEGY.
    //   category=0 (Strategy), progId=strategyProgId, flag=0.
    //   NOTE: Frida trace shows TSDev passing category=0x4 for a Function
    //   lookup. For a Strategy lookup the category byte is 0x0 (per
    //   FUN_101d43a0's switch: case 0 -> Strategy power-type array).
    std::printf("\n[6] Calling Ordinal_69(category=0, progId=%d, flag=0) "
                "for the STRATEGY...\n", stratProgId);
    Ord69SehResult ord69Seh;
    bool ord69Ok = callOrd69(ord69, /*category=*/(char)0,
                             /*progId=*/stratProgId, /*flag=*/0, &ord69Seh);
    std::printf("    call result: %s, rec=%p\n",
                ord69Ok ? "OK (no SEH)" : "SEH raised", ord69Seh.rec);
    if (!ord69Ok) {
        std::fprintf(stderr, "  [WARN] Ordinal_69 raised SEH for the "
                     "Strategy. Skipping live-record analysis.\n");
    } else if (!ord69Seh.rec) {
        std::fprintf(stderr, "  [WARN] Ordinal_69 returned NULL for the "
                     "Strategy (record not found / warehouse "
                     "unavailable / category skip).\n");
    }

    // --- Step 7-11: analyze the Strategy's live record. ---
    if (ord69Ok && ord69Seh.rec) {
        analyzeLiveRecord(ord69Seh.rec, "STRATEGY live record (Ordinal_69)",
                          stratProgId, funcProgId, ord70);
    }

    // --- Step 6b: also call Ordinal_69 for the FUNCTION for comparison.
    //   category=4 (Function), progId=funcProgId, flag=0.
    std::printf("\n[6b] Calling Ordinal_69(category=4, progId=%d, flag=0) "
                "for the FUNCTION (comparison)...\n", funcProgId);
    Ord69SehResult ord69FuncSeh;
    bool ord69FuncOk = callOrd69(ord69, /*category=*/(char)4,
                                 /*progId=*/funcProgId, /*flag=*/0,
                                 &ord69FuncSeh);
    std::printf("    call result: %s, rec=%p\n",
                ord69FuncOk ? "OK (no SEH)" : "SEH raised",
                ord69FuncSeh.rec);
    if (ord69FuncOk && ord69FuncSeh.rec) {
        analyzeLiveRecord(ord69FuncSeh.rec,
                          "FUNCTION live record (Ordinal_69)",
                          funcProgId, /*targetFuncProgId=*/funcProgId, ord70);
    }

    // --- Step 12: probe GetDependencies AFTER Ordinal_69.
    //   Hypothesis: GetDependencies gets its own live record internally
    //   (via FUN_101cff60), so calling Ordinal_69 first should NOT change
    //   its output. We expect GetDependencies to STILL return empty dep
    //   lists for the Strategy (the original observed bug).
    if (getDeps) {
        std::printf("\n[12] Probing GetDependencies AFTER Ordinal_69 "
                    "(hypothesis: still empty)...\n");
        // Strategy whType = 0 (cat 0, subtype 0).
        probeGetDependencies(getDeps, "Strategy (post-Ord69)",
                             /*whType=*/0, stratProgId, funcProgId);
        // Function whType = 4 (cat 4, subtype 0).
        probeGetDependencies(getDeps, "Function (post-Ord69, control)",
                             /*whType=*/4, funcProgId, /*target=*/-1);
    } else {
        std::printf("\n[12] SKIPPING GetDependencies probe (not "
                    "resolved).\n");
    }

    // --- Step 12b: probe Ordinal_28 (sibling) for completeness.
    //   Ordinal_28(category, progId, flag, outBuf) — writes a 0x10A-byte
    //   COPY into outBuf and returns 1 if found. NOT a prerequisite for
    //   Ordinal_69; probed only to confirm the sibling ABI.
    if (ord28) {
        std::printf("\n[12b] Probing Ordinal_28 (sibling, NOT a "
                    "prerequisite) for the Strategy...\n");
        unsigned char ord28buf[0x10A] = {};
        Ord28SehResult ord28Seh;
        bool ord28Ok = callOrd28(ord28, /*category=*/0, stratProgId,
                                 /*flag=*/0, ord28buf, &ord28Seh);
        std::printf("    Ordinal_28: %s, return=%u\n",
                    ord28Ok ? "OK (no SEH)" : "SEH raised",
                    ord28Seh.result);
        if (ord28Ok && ord28Seh.result == 1) {
            // Read +0x29 and +0x4f from the copy to confirm whether
            // Ordinal_28's copy has the element array populated (it should
            // NOT — same partial-copy behavior as _GetMasterRecByType@16).
            int ec = 0;
            sehReadInt(ord28buf + 0x29, &ec);
            void* eap = nullptr;
            sehReadPtr(ord28buf + 0x4f, &eap);
            std::printf("    Ordinal_28 copy: +0x29 elemCount=%d, "
                        "+0x4f elemArrayPtr=%p  [%s]\n",
                        ec, eap,
                        (ec == 0 && eap == nullptr)
                            ? "PARTIAL COPY (zeros — same as "
                              "_GetMasterRecByType@16)"
                            : "POPULATED (different from "
                              "_GetMasterRecByType@16)");
        }
    } else {
        std::printf("\n[12b] SKIPPING Ordinal_28 probe (not resolved).\n");
    }

    // --- Step 13: cleanup. ---
    //   The live records returned by Ordinal_69 were already
    //  refcount-decremented inside analyzeLiveRecord via Ordinal_70.
    std::printf("\n[13] Cleanup: delete Strategy + Function...\n");
    lib->deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId (callee) = %d\n", funcProgId);
    std::printf("Strategy ProgId (caller) = %d\n", stratProgId);
    std::printf("Ordinal_69 Strategy call: %s (rec=%p)\n",
                ord69Ok ? "OK" : "SEH", ord69Seh.rec);
    std::printf("Ordinal_69 Function call: %s (rec=%p)\n",
                ord69FuncOk ? "OK" : "SEH", ord69FuncSeh.rec);
    std::printf("Ordinal_70 (refcount-dec): %s\n",
                ord70 ? "resolved — live records released" :
                "NOT resolved — live records LEAKED");

    std::printf("\n=== DIAGNOSIS ===\n");
    std::printf("  See the field reads above for the key offsets:\n");
    std::printf("    +0x29 element count  — ZERO or NONZERO?\n");
    std::printf("    +0x4f element array  — NULL or NON-NULL?\n");
    std::printf("    element +0x73 dep-list length\n");
    std::printf("    element +0x7b dep-list pointer\n");
    std::printf("  If +0x29 is NONZERO and +0x4f is NON-NULL, Ordinal_69 "
                "DID return a live record\n");
    std::printf("    with populated element arrays (the key difference "
                "from _GetMasterRecByType@16).\n");
    std::printf("  If 0xFA4 opcodes were found in the dep list AND any "
                "funcId matches the\n");
    std::printf("    Function's ProgId, Ordinal_69 surfaces the "
                "function-ref dependency that\n");
    std::printf("    GetDependencies fails to enumerate.\n");
    std::printf("  GetDependencies AFTER Ordinal_69 should STILL return "
                "empty dep lists\n");
    std::printf("    (it gets its own live record internally — Ordinal_69 "
                "does not cache for it).\n");

    st->exitCode = 0;
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
    // Ordinal_69 + analysis + dependency probes may take longer than the 60s
    // used by simpler diagnostics.
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
