// test_m3b_object_code_version.cpp
//
// MANUAL ADMIN DIAGNOSTIC for Workstream M slice M.3b.2c (Phase 3: fix RED
// test + parameterize). This test is NOT part of the regression suite and is
// intentionally NOT wired into `test_regression` / `$TestDefinitions` /
// `run_regression.ps1`. It is a standalone, self-contained diagnostic-style
// executable run manually from an elevated process.
//
// M.3b.2b finding (the M.3b "defect" was a TEST ARTIFACT):
//   The original RED test pinned a "defect" where CLI-compiled items had
//   EMPTY element arrays (elemCount=0, elemArrayPtr=NULL) when the linker
//   read them. The M.3b.2b dynamic investigation proved this was a TEST
//   ARTIFACT: the test used a declaration-free Function source
//   (`TS_AUTO_M3B_VER_FUNC = 1;\r\n`) which legitimately has elemCount=0
//   because there are no `Inputs:`/`Variables:`/`Array:` declarations to
//   trigger the compiler's `Record*` count-populator functions. CLI-compiled
//   items WITH declarations produce non-zero elemCount and non-NULL
//   elemArrayPtr. See docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md §12-13.
//
// What this test now does:
//   - Loops over all 5 supported component types (Strategy, Indicator,
//     Function, TradingApp, ShowMe), closing the §13.6 sample-size gap.
//   - Uses declaration-rich sources for 4 types (Strategy, Indicator,
//     Function, ShowMe) and asserts elemCount > 0 AND elemArrayPtr != NULL.
//   - For TradingApp (which uses `using elsystem;` with no
//     `Inputs:`/`Variables:`/`Array:` declarations), elemCount == 0 is
//     EXPECTED and confirms the no-declarations pattern; elemArrayPtr == NULL
//     is also expected. This is a PASS condition, not a failure.
//
// Abstraction level (why this stays a manual diagnostic, not regression):
//   The test inspects orlib20 internal buffer layouts at the linker /
//   ordinal-85 abstraction level (record+0x29 elemCount, record+0x4f
//   elemArrayPtr). There is no higher-level API for the "element arrays
//   populated" property, so the test cannot be expressed against a stable
//   public contract. M.4b will replace the linker-based approach with
//   warehouse-level APIs; until then this remains a manual admin diagnostic.
//
// Reference: docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md §12-13.
//
// Flow:
//   1. Pre-check TSCrashReport.exe.
//   2. Construct a real TsDevLib (test-only ctor, real ComApi, no hooks) on a
//      64-MiB-stack worker thread.
//   3. Resolve the linker (orlib20 ordinal 85, FUN_101a3340), Ordinal_28
//      (FUN_101a03f0, master-record partial copy), and Ordinal_70
//      (FUN_1015df30, refcount-dec/free) via OrlibResolver.
//   4. For each of the 5 supported component types (Strategy, Indicator,
//      Function, TradingApp, ShowMe):
//      a. Insert + verify a disposable item with a type-appropriate source
//         (declaration-rich for 4 types; `using elsystem;` for TradingApp).
//      b. Call Ordinal_28(linkerType, progId, 0, buf) to get the partial master
//         record (0x10A bytes). linkerType = expandedWhType (the enum value),
//         NOT the category (low 8 bits). For Strategy/Indicator/Function,
//         expanded whType == category. For TradingApp (5122) and ShowMe (1282),
//         expanded whType != category (2); passing category=2 causes the
//         linker/Ordinal_28 to return NULL because the warehouse lookup fails.
//         This is a finding for M.4: the production enumerateReverseDependents()
//         iterates categories 0-7 and calls the linker with the category, which
//         fails for composite types (TradingApp/ShowMe). M.4b's warehouse-level
//         approach should bypass this.
//      c. Call the linker (ordinal 85) with (type=linkerType, id=progId,
//         errorFlag=0, partialBuf) to get the linked record. The linker
//         signature and call pattern match the existing diagnostics
//         (manual-ESP inline-asm wrapper robust to stdcall RET 0x10 /
//         fastcall RET 0x8, ECX=type, EDX=id).
//      d. Read record+0x29 (elemCount) and record+0x4f (elemArrayPtr) via
//         SEH-protected readers.
//      e. ASSERT:
//           - declaration-rich types: elemCount > 0 AND elemArrayPtr != NULL.
//           - TradingApp: elemCount == 0 (expected no-declarations pattern).
//      f. Release the linked record via Ordinal_70(rec, 0).
//      g. Clean up: delete the disposable item, verify its .ell file is gone
//         via FindFirstFileA. The .ell naming convention is
//         `elmodule_<expandedWhType>_<progId>.ell` in `MyWork\Bin`, where
//         expandedWhType = static_cast<int>(type).
//   5. Print a per-type summary table.
//   6. Post-check TSCrashReport.exe.
//
// All orlib20 calls are SEH-protected (__try/__except) using POD-only structs
// (MSVC C2712). The 600-second hard timeout is an operational safety bound,
// not a measured completion time or acceptance evidence.

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

// -------------------------------------------------------------------------
// reportCrashReporter — inspect (do NOT terminate) TSCrashReport.exe.
// Mirrors diag_m4_subtype.cpp / diag_m4_reverse_enum.cpp exactly.
// -------------------------------------------------------------------------
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
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// (Same pattern as diag_m4_subtype / diag_m4_objcode.)
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args, RET 0x10.
//   uint (int category, int progId, int flag, void* outBuf)
//   Writes a 0x10A-byte partial copy into outBuf.
//   Returns 1 if record found and copied, 0 otherwise.
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args, RET 0x08.
//   int (void* rec, int flag)  — refcount-dec / free.
//
// Linker FUN_101a3340 (ordinal 85). The brief specifies __stdcall with 4
//   stack args (type, id, errorFlag, partialBuf). We use the manual-ESP
//   inline-asm wrapper so the call is robust to either RET 0x10 (stdcall)
//   or RET 0x8 (fastcall) cleanup, AND we load ECX=type, EDX=id so the call
//   is valid under BOTH conventions. (Mirrors diag_m4_objcode.cpp exactly.)
// -------------------------------------------------------------------------
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef void* (__stdcall *LinkerStdcallFn)(int type, int id, int errorFlag,
                                           void* partialBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
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

struct IntSehResult {
    int result;
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

// callLinkerManual — calls the linker with ALL 4 args on the stack
// (as __stdcall would) AND loads ECX=type, EDX=id (as __fastcall would),
// using inline assembly so we can manually restore ESP regardless of whether
// the callee did RET 0x10 (stdcall, popped 16) or RET 0x8 (fastcall, popped
// 8). This makes the call robust to calling-convention mismatch.
//
// NOTE: This function is NOT itself SEH-protected (inline asm + SEH in the
// same function is rejected by MSVC C2712/C2181). The caller wraps it.
// (Mirrors diag_m4_objcode.cpp exactly.)
void* callLinkerManual(void* fn, int whType, int whId, int whErrFlag,
                       void* whPartialBuf) {
    void* retVal = nullptr;
    __asm {
        mov  esi, esp
        push whPartialBuf
        push whErrFlag
        push whId
        push whType
        mov  ecx, whType
        mov  edx, whId
        call fn
        mov  retVal, eax
        mov  esp, esi
    }
    return retVal;
}

struct LinkerSehResult {
    void* ptr;
    bool seh;
};

bool callLinkerSafe(void* fn, int type, int id, int errorFlag,
                    void* partialBuf, LinkerSehResult* out) {
    out->ptr = nullptr;
    out->seh = false;
    __try {
        out->ptr = callLinkerManual(fn, type, id, errorFlag, partialBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->seh = true;
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers (POD-only). Mirrors diag_m4_objcode.cpp.
// -------------------------------------------------------------------------
bool sehReadInt(const void* p, int* out) {
    *out = 0;
    __try { *out = *(const int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadPtr(const void* p, void** out) {
    *out = nullptr;
    __try { *out = *(void* const*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
}

// -------------------------------------------------------------------------
// Per-type test case table. Source is built per-type below.
// expectElemCountPositive=true for declaration-rich sources; false for
// TradingApp (`using elsystem;` with no Inputs:/Variables:/Array:
// declarations → elemCount=0 expected).
// -------------------------------------------------------------------------
struct TypeTestCase {
    ComponentType type;
    const char* name;
    const char* label;
    bool expectElemCountPositive;
};

const TypeTestCase kTypeCases[] = {
    { ComponentType::Strategy,   "TS_AUTO_M3B_VER_STRAT", "Strategy",   true  },
    { ComponentType::Indicator,  "TS_AUTO_M3B_VER_IND",   "Indicator",  true  },
    { ComponentType::Function,   "TS_AUTO_M3B_VER_FUNC",  "Function",   true  },
    { ComponentType::TradingApp, "TS_AUTO_M3B_VER_TA",    "TradingApp", false },
    { ComponentType::ShowMe,     "TS_AUTO_M3B_VER_SM",    "ShowMe",     true  },
};

// -------------------------------------------------------------------------
// EasyLanguage source builder — type-appropriate sources with \r\n line
// endings (EasyLanguage conventions). Declaration-rich for 4 types;
// `using elsystem;` (no declarations) for TradingApp.
// -------------------------------------------------------------------------
std::string makeSource(const TypeTestCase& tc) {
    switch (tc.type) {
        case ComponentType::Strategy:
            return "Inputs: int x(0);\r\nif x > 0 then Buy next bar at market;\r\n";
        case ComponentType::Indicator:
            return "Inputs: int x(0);\r\nPlot1(x, \"MyPlot\");\r\n";
        case ComponentType::Function:
            return std::string("Inputs: x(Numeric);\r\n") +
                   tc.name + std::string(" = x + 1;\r\n");
        case ComponentType::TradingApp:
            return "using elsystem;\r\n";
        case ComponentType::ShowMe:
            return "Inputs: int x(0);\r\nPlot1(Close, \"MyPlot\");\r\n";
        default:
            return "";
    }
}

// -------------------------------------------------------------------------
// insertAndVerify — pre-cleanup, createEntry, saveSource, verifyModule.
// Mirrors diag_m4_subtype.cpp / diag_m4_objcode.cpp exactly.
// Returns ProgId > 0 on success, 0 on failure (with cleanup already done).
// -------------------------------------------------------------------------
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
// Release a live record via Ordinal_70. Mirrors diag_m4_objcode.cpp.
// -------------------------------------------------------------------------
void releaseRec(Ordinal70Fn ord70, const char* label, void* rec) {
    if (!rec) return;
    if (ord70) {
        IntSehResult r;
        bool ok = callOrd70(ord70, rec, 0, &r);
        std::printf("    [release %s] Ordinal_70(rec=%p, 0): %s, ret=%d\n",
                    label, rec, ok ? "OK" : "SEH", r.result);
    } else {
        std::printf("    [release %s] Ordinal_70 NOT resolved — LEAKING "
                    "rec=%p\n", label, rec);
    }
}

// -------------------------------------------------------------------------
// Per-type diagnostic result (POD-only).
// -------------------------------------------------------------------------
struct TypeResult {
    ComponentType type = ComponentType::Function;
    const char* label = "";
    int progId = 0;
    bool linkerReturnedRec = false;
    bool linkerSeh = false;
    int  elemCount = -1;          // record+0x29
    void* elemArrayPtr = nullptr; // record+0x4f
    bool elemCountSeh = false;
    bool elemArrayPtrSeh = false;
    bool elemCountOk = false;     // > 0 (decl-rich) or == 0 (TradingApp)
    bool elemArrayPtrOk = false;  // != NULL (decl-rich) or == NULL (TradingApp)
    bool ellGone = false;
    bool pass = false;
};

// -------------------------------------------------------------------------
// Diagnostic / test state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
    std::vector<TypeResult> results;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string binDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

    std::printf("=== test_m3b_object_code_version: MANUAL ADMIN DIAGNOSTIC "
                "(M.3b.2c) ===\n");
    std::printf("Purpose: verify CLI-compiled items have populated element "
                "arrays when the\n");
    std::printf("  linker reads them, across all 5 supported component "
                "types. Declaration-rich\n");
    std::printf("  sources (Strategy/Indicator/Function/ShowMe) assert "
                "elemCount > 0 and\n");
    std::printf("  elemArrayPtr != NULL. TradingApp (`using elsystem;`, no "
                "declarations)\n");
    std::printf("  asserts elemCount == 0 (expected no-declarations "
                "pattern).\n");
    std::printf("  M.3b.2b finding: the original M.3b 'defect' was a TEST "
                "ARTIFACT.\n");
    std::printf("  Reference: docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md "
                "§12-13.\n");

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

    // --- Step 2: resolve Ordinal_28, Ordinal_70, and the linker (ord 85).
    // ---
    std::printf("\n[2] Resolving Ordinal_28 (ord 28), Ordinal_70 (ord 70), "
                "linker (ord 85, FUN_101a3340)...\n");
    Ordinal28Fn ord28 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    void* ord85Raw = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) ord85Raw = (void*)ord85;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    Resolved: Ord28=%p Ord70=%p Linker(85)=%p\n",
                (void*)ord28, (void*)ord70, ord85Raw);
    if (!ord28) {
        std::fprintf(stderr, "FATAL: Ordinal_28 not resolved — cannot get "
                     "partial master record. Aborting.\n");
        st->exitCode = 103;
        return 0;
    }
    if (!ord70) {
        std::fprintf(stderr, "WARN: Ordinal_70 not resolved — linker rec "
                     "will LEAK if returned.\n");
    }
    if (!ord85Raw) {
        std::fprintf(stderr, "FATAL: linker (ordinal 85) not resolved — "
                     "cannot run diagnostic. Aborting.\n");
        st->exitCode = 104;
        return 0;
    }

    // --- Steps 3-6: loop over all 5 supported component types. ---
    const int kNumCases = sizeof(kTypeCases) / sizeof(kTypeCases[0]);
    st->results.reserve(kNumCases);

    for (int ti = 0; ti < kNumCases; ++ti) {
        const TypeTestCase& tc = kTypeCases[ti];
        const std::string itemName = tc.name;
        const int category = getCategory(tc.type);
        const int expandedWhType = static_cast<int>(tc.type);
        // The linker (ordinal 85) and Ordinal_28 take the EXPANDED whType
        // (the enum value), NOT the category (low 8 bits). For
        // Strategy/Indicator/Function, expanded whType == category. For
        // TradingApp (5122) and ShowMe (1282), expanded whType != category
        // (2); passing category=2 causes the linker to return NULL because
        // the warehouse lookup fails. This is a finding for M.4: the
        // production enumerateReverseDependents() iterates categories 0-7
        // and calls the linker with the category, which fails for composite
        // types (TradingApp/ShowMe). M.4b's warehouse-level approach should
        // bypass this.
        const int linkerType = expandedWhType;

        std::printf("\n=== [%d/%d] %s (category=%d, expandedWhType=%d, "
                    "linkerType=%d, expectElemCountPositive=%s) ===\n",
                    ti + 1, kNumCases, tc.label, category, expandedWhType,
                    linkerType, tc.expectElemCountPositive ? "true" : "false");

        TypeResult tr;
        tr.type = tc.type;
        tr.label = tc.label;

        // --- Step 3: insert + verify the disposable item. ---
        std::printf("\n[3] Insert + verify %s \"%s\"...\n",
                    tc.label, itemName.c_str());
        std::string source = makeSource(tc);
        int progId = insertAndVerify(*lib, tc.type, itemName, source,
                                     tc.label);
        if (progId <= 0) {
            std::fprintf(stderr, "FATAL: %s insert/verify failed — cannot "
                         "run linker diagnostic without a compiled item.\n",
                         tc.label);
            tr.pass = false;
            st->results.push_back(tr);
            continue;
        }
        tr.progId = progId;
        std::printf("    %s ProgId = %d\n", tc.label, progId);

        // --- Step 4: get the partial master record via
        //     Ordinal_28(linkerType, progId, 0, buf). The linker takes this
        //     partial buffer as its 4th arg. linkerType = expandedWhType
        //     (the enum value), NOT the category (low 8 bits). For
        //     Strategy/Indicator/Function, expanded whType == category.
        //     For TradingApp (5122) and ShowMe (1282), expanded whType !=
        //     category (2); passing category=2 causes the linker to return
        //     NULL because the warehouse lookup fails.
        //     (Mirrors diag_m4_objcode.cpp step 6, extended for all 5 types.)
        // ---
        std::printf("\n[4] Calling Ordinal_28(%d, %d, 0, buf) for the "
                    "partial master record (0x10A bytes)...\n",
                    linkerType, progId);
        unsigned char partialBuf[0x10A] = {};
        bool havePartial = false;
        {
            Ord28SehResult r;
            bool ok = callOrd28(ord28, /*category=*/linkerType, progId,
                                /*flag=*/0, partialBuf, &r);
            std::printf("    Ordinal_28(%d, %d, 0, buf): %s, ret=%u\n",
                        linkerType, progId, ok ? "OK" : "SEH", r.result);
            if (ok && r.result == 1) {
                havePartial = true;
                void* sa = nullptr;
                int ec = 0;
                sehReadPtr(partialBuf + 0x26, &sa);
                sehReadInt(partialBuf + 0x29, &ec);
                std::printf("    Partial copy +0x26 SafeArrayPtr=%p, "
                            "+0x29 elemCount=%d\n", sa, ec);
            } else {
                std::fprintf(stderr, "WARN: Ordinal_28 did not return 1 — "
                             "linker will be called with partialBuf=NULL.\n");
            }
        }

        // --- Step 5: call the linker (ordinal 85) to get the linked record.
        //     The linker takes the EXPANDED whType (the enum value), NOT the
        //     category (low 8 bits). For Strategy/Indicator/Function,
        //     expanded whType == category. For TradingApp (5122) and ShowMe
        //     (1282), expanded whType != category (2); passing category=2
        //     causes the linker to return NULL.
        //     (Mirrors diag_m4_objcode.cpp step 7, extended for all 5 types.)
        // ---
        std::printf("\n[5] Calling linker (ordinal 85, FUN_101a3340) with "
                    "(type=%d, id=%d, errorFlag=0, partialBuf=%p)...\n",
                    linkerType, progId,
                    havePartial ? (void*)partialBuf : nullptr);
        void* linkerRec = nullptr;
        {
            LinkerSehResult r;
            bool ok = callLinkerSafe(ord85Raw, /*type=*/linkerType, progId,
                                     /*errorFlag=*/0,
                                     havePartial ? partialBuf : nullptr, &r);
            std::printf("    linker(%d, %d, 0, partialBuf): %s, rec=%p\n",
                        linkerType, progId,
                        ok ? "OK (no SEH)" : "SEH raised", r.ptr);
            tr.linkerSeh = r.seh;
            if (ok && r.ptr) {
                linkerRec = r.ptr;
                tr.linkerReturnedRec = true;
            } else if (!ok) {
                std::fprintf(stderr, "    [linker raised SEH — no record to "
                             "inspect]\n");
            } else {
                std::fprintf(stderr, "    [linker returned NULL — no record "
                             "to inspect]\n");
            }
        }

        // --- Step 6: read record+0x29 (elemCount) and record+0x4f
        //     (elemArrayPtr) via SEH-protected readers, then assert per the
        //     expected pattern. Declaration-rich types require elemCount > 0
        //     AND elemArrayPtr != NULL. TradingApp requires elemCount == 0
        //     (no-declarations pattern); elemArrayPtr == NULL is expected.
        // ---
        std::printf("\n[6] ASSERT %s: %s\n", tc.label,
                    tc.expectElemCountPositive
                        ? "record+0x29 (elemCount) > 0 AND "
                          "record+0x4f (elemArrayPtr) != NULL"
                        : "record+0x29 (elemCount) == 0 "
                          "(no-declarations pattern)");

        if (!linkerRec) {
            std::fprintf(stderr, "  [ASSERT] CANNOT evaluate: linker returned "
                         "no record (rec=NULL or SEH).\n");
            std::fprintf(stderr, "  [ASSERT] Treating as FAIL for %s.\n",
                         tc.label);
        } else {
            int ec = 0;
            void* eap = nullptr;
            bool ecSeh = !sehReadInt((const unsigned char*)linkerRec + 0x29,
                                     &ec);
            bool eapSeh = !sehReadPtr((const unsigned char*)linkerRec + 0x4f,
                                      &eap);
            tr.elemCount = ec;
            tr.elemArrayPtr = eap;
            tr.elemCountSeh = ecSeh;
            tr.elemArrayPtrSeh = eapSeh;

            std::printf("    record+0x29 (elemCount, int)    = %d (0x%X)  "
                        "[%s%s]\n",
                        ec, (unsigned)ec,
                        ecSeh ? "SEH reading" : (ec > 0 ? "> 0" : "== 0"),
                        ecSeh ? "" : "");
            std::printf("    record+0x4f (elemArrayPtr, ptr) = %p  "
                        "[%s%s]\n",
                        eap,
                        eapSeh ? "SEH reading" : (eap ? "NON-NULL" : "NULL"),
                        eapSeh ? "" : "");

            if (tc.expectElemCountPositive) {
                tr.elemCountOk = (!ecSeh) && (ec > 0);
                tr.elemArrayPtrOk = (!eapSeh) && (eap != nullptr);
            } else {
                // TradingApp: no declarations → elemCount == 0 expected.
                tr.elemCountOk = (!ecSeh) && (ec == 0);
                // elemArrayPtr == NULL is also expected; treat as OK.
                tr.elemArrayPtrOk = (!eapSeh) && (eap == nullptr);
            }

            std::printf("    [ASSERT elemCount %s]      : %s\n",
                        tc.expectElemCountPositive ? "> 0" : "== 0",
                        tr.elemCountOk ? "PASS" : "FAIL");
            std::printf("    [ASSERT elemArrayPtr %s]: %s\n",
                        tc.expectElemCountPositive ? "!= NULL" : "== NULL",
                        tr.elemArrayPtrOk ? "PASS" : "FAIL");
        }

        // --- Step 7: release the linked record via Ordinal_70(rec, 0). ---
        std::printf("\n[7] Releasing linked record via Ordinal_70(rec, "
                    "0)...\n");
        releaseRec(ord70, tc.label, linkerRec);

        // --- Step 8: clean up — delete the disposable item and verify its
        //     .ell file is gone. The .ell naming convention is
        //     `elmodule_<expandedWhType>_<progId>.ell` in `MyWork\Bin`,
        //     where expandedWhType = static_cast<int>(type).
        //     (Mirrors diag_m4_subtype.cpp step 7.)
        // ---
        std::printf("\n[8] Cleanup: delete %s \"%s\" (ProgId=%d)...\n",
                    tc.label, itemName.c_str(), progId);
        bool delOk = lib->deleteEntry(tc.type, itemName, /*flush=*/true);
        std::printf("    deleteEntry returned %s\n",
                    delOk ? "true" : "false");

        char ellPath[MAX_PATH];
        std::snprintf(ellPath, MAX_PATH, "%s\\elmodule_%d_%d.ell",
                      binDir.c_str(), expandedWhType, progId);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(ellPath, &fd);
        bool ellGone = (h == INVALID_HANDLE_VALUE);
        tr.ellGone = ellGone;
        if (ellGone) {
            std::printf("    [OK] .ell file gone: %s\n", ellPath);
        } else {
            std::printf("    [WARN] .ell file STILL EXISTS: %s\n", ellPath);
            FindClose(h);
        }

        // Per-type verdict: both element assertions must hold. .ell cleanup
        // is reported but does not gate the per-type pass (matches the
        // original test's treatment of .ell as a diagnostic, not a hard
        // gate).
        tr.pass = tr.elemCountOk && tr.elemArrayPtrOk;
        std::printf("\n  [%s] OVERALL: %s\n", tc.label,
                    tr.pass ? "PASS" : "FAIL");

        st->results.push_back(tr);
    }

    // --- Final verdict: per-type summary table. ---
    std::printf("\n=== FINAL VERDICT (per-type summary) ===\n");
    std::printf("  %-10s %-7s %-7s %-12s %-14s %-8s %-6s\n",
                "Type", "ProgId", "Linker", "elemCount", "elemArrayPtr",
                ".ell", "Result");
    bool allPass = true;
    for (const auto& tr : st->results) {
        std::printf("  %-10s %-7d %-7s %-12d %-14p %-8s %-6s\n",
                    tr.label,
                    tr.progId,
                    tr.linkerReturnedRec ? "YES"
                        : (tr.linkerSeh ? "SEH" : "NO"),
                    tr.elemCount,
                    tr.elemArrayPtr,
                    tr.ellGone ? "gone" : "EXISTS",
                    tr.pass ? "PASS" : "FAIL");
        if (!tr.pass) allPass = false;
    }
    std::printf("\n  OVERALL: %s\n", allPass ? "PASS" : "FAIL");
    std::printf("  (Manual admin diagnostic — NOT in regression suite. "
                "M.3b.2c.)\n");

    st->exitCode = allPass ? 0 : 1;
    std::printf("\n=== TEST COMPLETE ===\n");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the test; no process was terminated.\n");
        return 97;
    }

    // Run on a 64 MB-stack worker thread (TsDevLib is thread-affine and
    // orlib20 / EasyLanguage compiler calls need a large stack).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 600-second hard timeout — report a hang without terminating the
    // worker. Insert+verify + Ordinal_28 + linker call + cleanup may take
    // a couple of minutes on a cold warehouse.
    DWORD wait = WaitForSingleObject(hThread, 600000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: test timed out after 600s; worker was "
                     "not forcibly terminated and no process cleanup was "
                     "attempted. Manual exact-PID recovery is required.\n");
        CloseHandle(hThread);
        return 98;
    }
    if (wait == WAIT_FAILED) {
        std::fprintf(stderr,
                     "\nFATAL: waiting for test worker failed: %lu; "
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
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected after the test; no process cleanup was "
                     "attempted.\n");
        return 97;
    }

    return st.exitCode;
}
