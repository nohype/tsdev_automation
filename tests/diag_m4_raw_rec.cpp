// diag_m4_raw_rec.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
// Historical rejected-path investigation; current M.4 uses the linker and
// linked per-element dep-lists, not GetDependencies.
//
// Historical purpose: investigate the raw master-record structure to understand
// why the forward GetDependencies export (orlib20 ordinal 487) returned empty
// dep lists for a Strategy that calls a Function. This is not the current M.4
// reverse-dependent path.
//
// GetDependencies internally:
//   1. Calls FUN_101a9650(0) to get the warehouse.
//   2. Calls FUN_101cff60(powerTypeArray, progId, local_14c) to look up the
//      master record (the IN-MEMORY warehouse record pointer "ctx").
//   3. Checks (local_d3 & 4) != 0 — a "has dependencies" flag.
//   4. If flag set, calls FUN_10153ad0 which reads:
//        ctx + 0x29  -> element count (int)
//        ctx + 0x4f  -> element array base pointer (int*)
//        element[i] = base + i*0xb5
//        element + 0x73 -> dep-record list length (int)
//        element + 0x7b -> dep-record list pointer (int*)
//      and scans the dep-record list for the historical opcode 0xFA4
//      expectation (current evidence identifies compiler-written refs as 0xFA3).
//
// The 0x10A-byte master record from _GetMasterRecByType@16 is a COPY of the
// master record. The historical task hypothesis was that this copy had the
// SAME layout as the in-memory ctx record (element count at +0x29, element
// array ptr at +0x4f, etc.). This diagnostic dumps the raw 0x10A bytes and
// records those offsets alongside the current linker-based findings.
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB
//      stack worker thread.
//   2. Inserts + verifies a Function "TS_AUTO_M4_DIAG_RFUNC" with source
//      `TS_AUTO_M4_DIAG_RFUNC = 1;`.
//   3. Inserts + verifies a Strategy "TS_AUTO_M4_DIAG_RSTRAT" with source
//      that calls the Function.
//   4. Records both ProgIds.
//   5. Constructs an OribResolver to resolve _GetMasterRecByType@16.
//   6. For the Strategy (caller), calls
//      _GetMasterRecByType@16(category=0, subtype=0, index=strategyProgId,
//      outBuf) to get the 0x10A-byte master record.
//   7. Dumps the raw record in hex (all 0x10A bytes).
//   8. Reads + prints the fields at the specified offsets.
//   9. If element count > 0 AND element array pointer is non-NULL, iterates
//      elements and reads +0x73 (dep list len) and +0x7b (dep list ptr),
//      and looks for the historical 0xFA4 function-reference expectation.
//  10. Intentionally skips Ordinal_69 and Ordinal_28; their ABI/live-record
//      behavior is covered by the separate M.4 ordinal diagnostics.
//  11. Cleans up: deletes both items.
//
// All raw pointer reads are SEH-protected (__try/__except) because the
// record data could be partially invalid.
//
// RE ABI reference:
//   docs/RE/RE_M4_GETDEPENDENCIES_ABI.md (GetDependencies, FUN_10153ad0)
//   src/tsdev_lib.h (GetMasterRecByTypeFn typedef, _GetMasterRecByType@16)
//
// _GetMasterRecByType@16 — __stdcall, 4 params (category, subtype,
// index=ProgId, outBuf). Returns 1 on success (record found and copied to
// outBuf), 0 on failure. The record is 0x10A (266) bytes.

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
// SEH-protected call wrapper for _GetMasterRecByType@16.
// POD-only so __try/__except is legal under MSVC C2712.
// -------------------------------------------------------------------------
typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType,
                                              int index, void* outBuf);

struct GetMasterRecByTypeSehResult {
    int result;  // -1 if _GetMasterRecByType raised
};

bool callGetMasterRecByType(GetMasterRecByTypeFn fn, int category,
                            int subType, int index, void* outBuf,
                            GetMasterRecByTypeSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(category, subType, index, outBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers. Each reads a single POD value from an
// arbitrary pointer. POD-only so __try/__except is legal.
//
// Returns true on success, false on SEH. The out value is zeroed first so
// a false return leaves a deterministic 0.
// -------------------------------------------------------------------------
bool sehReadInt(const void* p, int* out) {
    *out = 0;
    __try {
        *out = *(const int*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool sehReadShort(const void* p, short* out) {
    *out = 0;
    __try {
        *out = *(const short*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool sehReadByte(const void* p, unsigned char* out) {
    *out = 0;
    __try {
        *out = *(const unsigned char*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool sehReadPtr(const void* p, void** out) {
    *out = nullptr;
    __try {
        *out = *(void* const*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH-protected memcpy from a possibly-invalid source.
bool sehMemcpy(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
// EasyLanguage source builders (mirror diag_m4_deps.cpp patterns).
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

// Pre-cleanup: delete the named item if it already exists (re-run safety).
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
// Print a 30-byte ASCII field (null-terminated inline string starting at
// the given offset). SEH-protected.
// -------------------------------------------------------------------------
void printAsciiField(const unsigned char* rec, size_t off, size_t len,
                     const char* fieldName) {
    char tmp[64] = {};
    bool ok = sehMemcpy(rec + off, tmp, len);
    if (!ok) {
        std::printf("    +0x%02zx (%s): <SEH reading %zu bytes>\n",
                    off, fieldName, len);
        return;
    }
    // Force NUL-terminate at len; replace non-printables with '.'.
    tmp[len] = 0;
    for (size_t i = 0; i < len; i++) {
        if (tmp[i] == 0) break;
        if ((unsigned char)tmp[i] < 0x20 || (unsigned char)tmp[i] >= 0x7f) {
            tmp[i] = '.';
        }
    }
    std::printf("    +0x%02zx (%s): \"%s\"\n", off, fieldName, tmp);
}

// -------------------------------------------------------------------------
// Dump + analyze a 0x10A-byte master record.
//
// Field layout per the task hypothesis (same as the in-memory ctx record
// that FUN_10153ad0 reads):
//   +0x00: ProgId (short)
//   +0x17: name (inline, up to ~30 bytes)
//   +0x29: element count (int)
//   +0x4f: pointer to element array (int*), stride 0xb5 per element
//   +0xd6: pointer
//   +0xe9: subtype (byte)
//
// Each element (at stride 0xb5 from the base at +0x4f):
//   +0x73: dep-record list length (int)
//   +0x7b: pointer to dep-record list (int*)
// -------------------------------------------------------------------------
void dumpAndAnalyzeMasterRec(const unsigned char* rec, const char* label,
                             int expectedProgId) {
    std::printf("\n=== MASTER RECORD ANALYSIS: %s ===\n", label);
    if (!rec) {
        std::printf("  [record pointer is NULL -- nothing to analyze]\n");
        return;
    }

    // 1. Full hex dump (all 0x10A bytes).
    hexDump(rec, 0x10A, label);

    // 2. Field reads (all SEH-protected).
    std::printf("\n  --- FIELD READS ---\n");

    // +0x00: ProgId (short)
    short progIdShort = 0;
    bool okProg = sehReadShort(rec + 0x00, &progIdShort);
    std::printf("    +0x00 ProgId (short): %s = %d (0x%04X)  [expected=%d]\n",
                okProg ? "OK" : "<SEH>", progIdShort,
                (unsigned short)progIdShort, expectedProgId);

    // +0x17: name (first 30 bytes as ASCII)
    printAsciiField(rec, 0x17, 30, "name (30 bytes)");

    // +0x29: element count (int) — KEY FIELD
    int elemCount = 0;
    bool okEc = sehReadInt(rec + 0x29, &elemCount);
    std::printf("    +0x29 element count (int): %s = %d (0x%X)  "
                "[>>> ZERO OR NONZERO? %s <<<]\n",
                okEc ? "OK" : "<SEH>", elemCount, (unsigned)elemCount,
                (elemCount == 0) ? "ZERO" : "NONZERO");

    // +0x4f: element array pointer (int*) — KEY FIELD
    void* elemArrayPtr = nullptr;
    bool okEap = sehReadPtr(rec + 0x4f, &elemArrayPtr);
    std::printf("    +0x4f element array ptr (int*): %s = %p  "
                "[>>> NULL OR NON-NULL? %s <<<]\n",
                okEap ? "OK" : "<SEH>", elemArrayPtr,
                (elemArrayPtr == nullptr) ? "NULL" : "NON-NULL");

    // +0xd6: pointer
    void* ptrD6 = nullptr;
    bool okD6 = sehReadPtr(rec + 0xd6, &ptrD6);
    std::printf("    +0xd6 pointer: %s = %p\n",
                okD6 ? "OK" : "<SEH>", ptrD6);

    // +0xe9: subtype (byte)
    unsigned char subType = 0;
    bool okSt = sehReadByte(rec + 0xe9, &subType);
    std::printf("    +0xe9 subtype (byte): %s = 0x%02X (%u)\n",
                okSt ? "OK" : "<SEH>", subType, subType);

    // 3. If element count > 0 AND element array pointer is non-NULL,
    //    iterate elements and read +0x73 (dep list len) and +0x7b (dep
    //    list ptr) for each. SEH-protected.
    if (elemCount > 0 && elemCount < 1024 && elemArrayPtr != nullptr) {
        std::printf("\n  --- ELEMENT ITERATION (%d elements, stride 0xb5) "
                    "---\n", elemCount);
        const unsigned char* base = (const unsigned char*)elemArrayPtr;
        int fa4Found = 0;
        for (int i = 0; i < elemCount; i++) {
            const unsigned char* elem = base + (size_t)i * 0xb5;
            std::printf("\n    element[%d] @ %p (offset 0x%zx from base):\n",
                        i, elem, (size_t)i * 0xb5);

            // +0x73: dep-record list length (int)
            int depListLen = 0;
            bool okDll = sehReadInt(elem + 0x73, &depListLen);
            std::printf("      +0x73 dep-list length (int): %s = %d (0x%X)\n",
                        okDll ? "OK" : "<SEH>", depListLen,
                        (unsigned)depListLen);

            // +0x7b: dep-record list pointer (int*)
            void* depListPtr = nullptr;
            bool okDlp = sehReadPtr(elem + 0x7b, &depListPtr);
            std::printf("      +0x7b dep-list ptr (int*): %s = %p\n",
                        okDlp ? "OK" : "<SEH>", depListPtr);

            if (depListPtr != nullptr && depListLen > 0 &&
                depListLen < 0x100000) {
                // Print the first few bytes of the dep-record list.
                size_t dumpLen = (size_t)depListLen;
                if (dumpLen > 64) dumpLen = 64;  // cap
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
                // function-reference expectation. Per FUN_10153ad0: each dep
                // record starts with an int opcode; the record stride is
                // *(byte*)(cur + 1). We walk
                // until we've consumed depListLen bytes.
                const unsigned char* cur = (const unsigned char*)depListPtr;
                size_t consumed = 0;
                int recIdx = 0;
                int scanCap = 256;  // safety cap on records
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
                        fa4Found++;
                        // The function id is at cur + 0xd per FUN_10153ad0.
                        int funcId = 0;
                        bool okFid = sehReadInt(cur + 0xd, &funcId);
                        std::printf("      >>> 0xFA4 (function ref) found at "
                                    "dep-rec[%d] @ %p: funcId=%s = %d "
                                    "(0x%X), stride=%u\n",
                                    recIdx, cur,
                                    okFid ? "OK" : "<SEH>",
                                    funcId, (unsigned)funcId, stride);
                    }
                    if (stride == 0) {
                        // Avoid infinite loop on a zero-stride record.
                        std::printf("      [zero stride at dep-rec[%d] @ %p "
                                    "(opcode=0x%X) -- stopping scan]\n",
                                    recIdx, cur, (unsigned)opcode);
                        break;
                    }
                    cur += stride;
                    consumed += stride;
                    recIdx++;
                }
                std::printf("      dep-rec scan: %d records walked, "
                            "%d 0xFA4 opcodes found\n", recIdx, fa4Found);
            } else {
                std::printf("      [dep-list empty or ptr NULL -- no scan]\n");
            }
        }
        std::printf("\n  --- ELEMENT ITERATION SUMMARY: %d 0xFA4 opcodes "
                    "found across all elements ---\n", fa4Found);
    } else {
        std::printf("\n  [Skipping element iteration: elemCount=%d "
                    "(%s), elemArrayPtr=%p (%s)]\n",
                    elemCount,
                    (elemCount > 0 && elemCount < 1024) ? "valid" : "invalid",
                    elemArrayPtr,
                    (elemArrayPtr != nullptr) ? "non-NULL" : "NULL");
    }

    std::printf("\n=== END MASTER RECORD ANALYSIS: %s ===\n", label);
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName   = "TS_AUTO_M4_DIAG_RFUNC";
    const std::string stratName  = "TS_AUTO_M4_DIAG_RSTRAT";

    std::printf("=== diag_m4_raw_rec: raw master record structure diagnostic "
                "===\n");
    std::printf("Purpose: dump the 0x10A-byte master record from "
                "_GetMasterRecByType@16 for a Strategy that calls a "
                "Function, and check whether the element-count / element-"
                "array-ptr / dep-list fields are populated.\n");

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

    // --- Step 2: resolve _GetMasterRecByType@16 via a separate
    // OrlibResolver. ---
    std::printf("\n[2] Resolving _GetMasterRecByType@16 via "
                "OrlibResolver...\n");
    GetMasterRecByTypeFn getMasterRecByType = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        getMasterRecByType = reinterpret_cast<GetMasterRecByTypeFn>(
            resolver.resolveExport("_GetMasterRecByType@16"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver/resolveExport threw: %s\n",
                     e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!getMasterRecByType) {
        std::fprintf(stderr,
                     "FATAL: failed to resolve _GetMasterRecByType@16\n");
        st->exitCode = 103;
        return 0;
    }
    std::printf("    Resolved _GetMasterRecByType@16=%p\n",
                getMasterRecByType);

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

    // --- Step 6: call _GetMasterRecByType@16 for the Strategy (caller).
    // category=0 (Strategy), subtype=0, index=strategyProgId. ---
    std::printf("\n[6] Calling _GetMasterRecByType@16 for the STRATEGY "
                "(category=0, subtype=0, index=%d)...\n", stratProgId);
    unsigned char stratRec[0x10A] = {};
    GetMasterRecByTypeSehResult stratMrSeh;
    bool stratOk = callGetMasterRecByType(getMasterRecByType,
                                          /*category=*/0, /*subType=*/0,
                                          stratProgId, stratRec,
                                          &stratMrSeh);
    std::printf("    call result: %s, return=%d\n",
                stratOk ? "OK (no SEH)" : "SEH raised",
                stratMrSeh.result);
    if (!stratOk || stratMrSeh.result != 1) {
        std::fprintf(stderr,
                     "  [WARN] _GetMasterRecByType@16 did not return 1 for "
                     "the Strategy; record may be empty/zeroed. Proceeding "
                     "to dump anyway.\n");
    }

    // --- Step 7+8+9: dump + analyze the Strategy's master record. ---
    dumpAndAnalyzeMasterRec(stratRec, "STRATEGY master record", stratProgId);

    // --- Bonus: also dump the Function's master record for comparison. ---
    std::printf("\n[BONUS] Calling _GetMasterRecByType@16 for the FUNCTION "
                "(category=1, subtype=0, index=%d) for comparison...\n",
                funcProgId);
    unsigned char funcRec[0x10A] = {};
    GetMasterRecByTypeSehResult funcMrSeh;
    bool funcOk = callGetMasterRecByType(getMasterRecByType,
                                         /*category=*/1, /*subType=*/0,
                                         funcProgId, funcRec,
                                         &funcMrSeh);
    std::printf("    call result: %s, return=%d\n",
                funcOk ? "OK (no SEH)" : "SEH raised",
                funcMrSeh.result);
    dumpAndAnalyzeMasterRec(funcRec, "FUNCTION master record", funcProgId);

    // --- Step 10/11: Ordinal_69 and Ordinal_28 are SKIPPED (ABI unknown;
    // calling them blindly could crash). ---
    std::printf("\n[10/11] SKIPPING Ordinal_69 (RVA 0x1A3E00) and "
                "Ordinal_28 (RVA 0x1A03F0) -- ABIs unknown, calling them "
                "blindly could crash.\n");

    // --- Step 12: cleanup. ---
    std::printf("\n[12] Cleanup: delete Strategy + Function...\n");
    lib->deleteEntry(ComponentType::Strategy, stratName, /*flush=*/false);
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId (callee) = %d\n", funcProgId);
    std::printf("Strategy ProgId (caller) = %d\n", stratProgId);
    std::printf("_GetMasterRecByType@16 Strategy call: %s (return=%d)\n",
                stratOk ? "OK" : "SEH", stratMrSeh.result);
    std::printf("_GetMasterRecByType@16 Function call: %s (return=%d)\n",
                funcOk ? "OK" : "SEH", funcMrSeh.result);

    std::printf("\n=== DIAGNOSIS ===\n");
    std::printf("  See the field reads above for the key offsets:\n");
    std::printf("    +0x29 element count  -- ZERO or NONZERO?\n");
    std::printf("    +0x4f element array  -- NULL or NON-NULL?\n");
    std::printf("    element +0x73 dep-list length\n");
    std::printf("    element +0x7b dep-list pointer\n");
    std::printf("  If +0x29 is ZERO or +0x4f is NULL, the 0x10A-byte master "
                "record copy does NOT carry the in-memory element array, "
                "and GetDependencies (which reads the IN-MEMORY ctx record "
                "via FUN_101cff60, NOT this copy) cannot be diagnosed from "
                "this copy alone.\n");
    std::printf("  If +0x29 is NONZERO and +0x4f is NON-NULL but the dep "
                "lists are empty, the dep records were never materialized "
                "for this item.\n");

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
    // 60-second hard timeout — report a hang without terminating the worker.
    DWORD wait = WaitForSingleObject(hThread, 60000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 60s; worker was "
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
