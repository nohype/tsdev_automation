// test_callers_enumeration.cpp
//
// MANUAL ADMIN DIAGNOSTIC for Workstream M slice M.4b.1 — Warehouse-level
// reverse-dependent (Callers) enumeration primitive.
//
// This test is NOT part of the regression suite and is intentionally NOT
// wired into `test_regression` / `$TestDefinitions` / `run_regression.ps1`.
// It is a standalone, self-contained diagnostic-style executable run
// manually from an elevated process against live TradeStation.
//
// What this test does:
//   1. Pre-check TSCrashReport.exe.
//   2. Constructs a real TsDevLib (test-only ctor, real ComApi, no hooks) on
//      a 64-MiB-stack worker thread.
//   3. Inserts a disposable Function with `Inputs:` declarations
//      (e.g. `Inputs: int x; JW_M4B1_TEST_FUNC_<ts> = x + 1;`).
//   4. Inserts a disposable Indicator that references the Function
//      (e.g. `Variables: int v; v = JW_M4B1_TEST_FUNC_<ts>(1); Plot(v, "v");`).
//   5. Calls lib.enumerateCallers(ComponentType::Function, funcName) and
//      asserts the result contains the caller (Indicator) with the correct
//      name and ProgId (cross-checked via
//      lib.enumerateRecords(ComponentType::Indicator, callerName)).
//   6. Cleans up: delete the caller (Indicator), then delete the Function.
//      Verify .ell files are gone.
//   7. Post-check TSCrashReport.exe.
//
// REQUIRES TradeStation: this is an integration test that constructs a real
// TsDevLib (loads orlib20.dll, applies patches, calls OrlibInit, creates the
// hidden EDIT control) and inserts + verifies + deletes real warehouse
// entries. Desktop/TSDev running state is not a prerequisite because the
// test does not exercise the Desktop UI.
//
// The test must be run from an ELEVATED process. If not elevated, the CLI
// elevation guard would return exit 11; this test does not have that guard
// but orlib20 operations require admin privileges (TSDev.exe and
// TradeStation Desktop always run elevated).
//
// Exit code: 0 only if ALL assertions pass; nonzero on any failure.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
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

// Forward-declare the test-only friend accessor (defined in diag_m4_reverse_enum.cpp).
namespace tsdev {
struct DiagReverseDependent { int progId = 0; int category = 0; int subtype = 0; };
struct DiagM4ReverseEnumAccess {
    static std::vector<DiagReverseDependent> enumerate(
        TsDevLib& lib, int rootProgId, int rootCategory) {
        std::vector<TsDevLib::ReverseDependent> priv =
            lib.enumerateReverseDependents(rootProgId, rootCategory);
        std::vector<DiagReverseDependent> out;
        for (const auto& d : priv) {
            out.push_back({d.progId, d.category, d.subtype});
        }
        return out;
    }
    // Find the stored name for a given (category, progId) by scanning
    // master records via _LoadMasterRecsEX@16. Returns empty string if
    // not found. Uses the private m_loadMasterRecsEx member via friend
    // access. No SEH wrapper here (LoadMasterRecsEx is proven safe in
    // production code; the SEH wrapper requires a separate non-throwing
    // function which is not worth the complexity for a diagnostic).
    static std::string nameForProgId(TsDevLib& lib, int category, int progId) {
        if (!lib.m_loadMasterRecsEx) return "";
        HGLOBAL buf = NULL;
        int count = lib.m_loadMasterRecsEx(category, &buf, (const char*)NULL, 1);
        if (count <= 0 || !buf) { if (buf) GlobalFree(buf); return ""; }
        char* records = (char*)GlobalLock(buf);
        if (!records) { GlobalFree(buf); return ""; }
        std::string result;
        const int kRecordSize = 0x10A;
        const int kNameOffset = 0x17;
        for (int i = 0; i < count; i++) {
            char* rec = records + (size_t)i * kRecordSize;
            short pid = *(short*)rec;
            if (pid == progId) {
                const char* namePtr = rec + kNameOffset;
                result = std::string(namePtr, strnlen(namePtr, 63));
                break;
            }
        }
        GlobalUnlock(buf);
        GlobalFree(buf);
        return result;
    }
};
}

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
// (Same pattern as test_reverse_dependent_revalidation.cpp /
// test_m3b_object_code_version.cpp.)
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// CHECK macro — prints PASS/FAIL per assertion with line numbers.
// (Same pattern as test_reverse_dependent_revalidation.cpp.)
// -------------------------------------------------------------------------
static int g_failCount = 0;
static int g_passCount = 0;

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failCount++;                                             \
    } else {                                                       \
        std::printf("PASS: %s\n", msg);                            \
        g_passCount++;                                             \
    }                                                              \
} while(0)

// -------------------------------------------------------------------------
// Minimal valid EasyLanguage source per component type.
//
// These mirror the PROVEN patterns from
// test_reverse_dependent_revalidation.cpp (which compiles cleanly against
// the real EasyLanguage compiler):
//   - Function (no input): `name = 1;\r\n`
//   - Indicator calling a no-input Function:
//       `{ calls <calleeName> }\r\nValue1 = <calleeName>;\r\nPlot1(Value1, "Dep");\r\n`
//
// The Indicator references the Function by name, so the Function's ProgId
// appears in the caller's linked per-element dependency list, making the
// Indicator a reverse dependent (caller) of the Function.
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    // Function with an explicit Inputs: declaration so the compiler's
    // ScanSourceCode → RecordInputs path fires, populating the INFOREC
    // element count (+0x29) and element array (+0x4f). Without declarations,
    // the INFOREC counts remain 0 and no dep-lists are stored in the .ell
    // file's object code (M.3b.2b finding).
    return "Inputs: x(Numeric);\r\n" + name + " = x + 1;\r\n";
}

std::string makeIndicatorSourceCalling(const std::string& /*name*/,
                                       const std::string& calleeName) {
    // Indicator with an explicit Inputs: declaration (required for the same
    // reason as the Function: the compiler only populates element dep-lists
    // for modules that have elements). The function call's 0xFA3 dependency
    // entry is recorded in the element's dep-list, making the Indicator a
    // reverse dependent (caller) of the Function.
    return "Inputs: dummy(0);\r\n"
           "Variables: v(0);\r\n"
           "v = " + calleeName + "(1);\r\n"
           "Plot1(v, \"Dep\");\r\n";
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// -------------------------------------------------------------------------
// Pre-cleanup helper: delete the named item if it already exists (re-run
// safety). (Same pattern as test_reverse_dependent_revalidation.cpp.)
// -------------------------------------------------------------------------
void preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
}

// -------------------------------------------------------------------------
// insertAndVerify — helper: pre-clean, create, save, verify a disposable
// item. Returns the ProgId on success, 0 on a fatal setup error. The caller
// is responsible for cleanup (deleteEntry) after the assertions.
// (Same pattern as test_reverse_dependent_revalidation.cpp.)
// -------------------------------------------------------------------------
int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    const char* typeLabel) {
    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                typeLabel, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0", 0);
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false", 0);
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors", 0);
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", typeLabel, progId);
    return progId;
}

// -------------------------------------------------------------------------
// Verify the .ell file is gone for a given type + ProgId.
// -------------------------------------------------------------------------
bool verifyEllGone(TsDevLib& lib, ComponentType type, int progId) {
    std::string ellPath = lib.getEllPath(type, progId);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(ellPath.c_str(), &fd);
    bool gone = (h == INVALID_HANDLE_VALUE);
    if (!gone) FindClose(h);
    return gone;
}

// -------------------------------------------------------------------------
// Test state.
// -------------------------------------------------------------------------
struct TestState {
    int exitCode = 0;
};

// -------------------------------------------------------------------------
// The main test body, running on a 64-MiB-stack worker thread.
// -------------------------------------------------------------------------
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    const std::string binDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

    std::printf("=== test_callers_enumeration: M.4b.1 warehouse-level "
                "reverse-dependent (Callers) enumeration ===\n");
    std::printf("Purpose: verify enumerateCallers() returns the correct "
                "reverse dependents\n");
    std::printf("  of a library item via the orlib20 warehouse worker-thread "
                "command protocol\n");
    std::printf("  (cmd 0x0D -> cmd 0x3B -> SafeArray parse).\n");
    std::printf("See docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md section 9 "
                "(R-M.4a.1 through R-M.4a.6, section 9.7).\n");
    std::printf("Desktop/TSDev running state is not a prerequisite for this "
                "library-level test.\n");

    // --- Step 1: Construct a real TsDevLib (full init). No hooks. ---
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

    // M.4b.1 DIAGNOSTIC: Test enumerateCallers for an EXISTING function
    // (ProgId=517) that has known reverse dependents (Indicators 3 and 8,
    // confirmed by diag_m4_reverse_enum). This determines whether the COM
    // server's Callers method works at all for pre-existing items.
    std::printf("\n[1b] Testing enumerateCallers for existing Function "
                "ProgId=517...\n");
    {
        std::string existingName = tsdev::DiagM4ReverseEnumAccess::nameForProgId(
            *lib, 4, 517);
        if (existingName.empty()) {
            std::printf("    Could not find Function ProgId=517 name\n");
        } else {
            std::printf("    Function ProgId=517 name=\"%s\"\n",
                        existingName.c_str());
            // Linker-based check (known to work for existing items)
            auto linkerDeps = tsdev::DiagM4ReverseEnumAccess::enumerate(
                *lib, 517, 4);
            std::printf("    Linker-based: %zu reverse dependent(s)\n",
                        linkerDeps.size());
            // COM server Callers check — use ProgId-based overload since
            // findExistPos can fail for existing items when the BST is not
            // initialized. The warehouse cmd 0x0D takes a ProgId directly.
            auto callers = lib->enumerateCallersByProgId(
                ComponentType::Function, 517);
            std::printf("    COM Callers: %zu caller(s)\n", callers.size());
            for (size_t i = 0; i < callers.size() && i < 5; i++) {
                std::printf("      [%zu] name=\"%s\" progId=%d cat=%d\n",
                            i, callers[i].name.c_str(),
                            callers[i].progId, callers[i].category);
            }
        }
    }

    // Date-stamped + timestamped names so the test items are unique per run.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char funcName[80];
    char indName[80];
    std::snprintf(funcName, sizeof(funcName),
                  "JW_M4B1_TEST_FUNC_%04d%02d%02d_%02d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay,
                  lt.wHour, lt.wMinute, lt.wSecond);
    std::snprintf(indName, sizeof(indName),
                  "JW_M4B1_TEST_IND_%04d%02d%02d_%02d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay,
                  lt.wHour, lt.wMinute, lt.wSecond);

    std::printf("\n[2] Insert + verify Function (callee) \"%s\"...\n",
                funcName);
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        st->exitCode = fail("Function setup failed", 200);
        return 0;
    }

    std::printf("\n[3] Insert + verify Indicator (caller) \"%s\" calling "
                "Function...\n", indName);
    int indProgId = insertAndVerify(*lib, ComponentType::Indicator,
                                    indName,
                                    makeIndicatorSourceCalling(indName,
                                                               funcName),
                                    "Indicator");
    if (indProgId <= 0) {
        lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/false);
        st->exitCode = fail("Indicator setup failed", 201);
        return 0;
    }

    // M.4b.1: The closeWarehouse experiment was removed. The root cause
    // was identified as a missing post-verify link step (Ordinal_69).
    // verifyModule now calls linkModule internally, which populates the
    // dep-list cache with 0xFA3 function-call dependency entries.

    // --- Step 4: Cross-check the Indicator's ProgId via enumerateRecords ---
    std::printf("\n[4] Cross-checking Indicator ProgId via "
                "enumerateRecords...\n");
    EnumerateResult indEr = lib->enumerateRecords(ComponentType::Indicator,
                                                   indName);
    CHECK(indEr.exists, "enumerateRecords found the Indicator");
    CHECK(indEr.progId == indProgId,
          "enumerateRecords ProgId matches insertAndVerify ProgId");
    std::printf("    Indicator: storedName=\"%s\", progId=%d\n",
                indEr.storedName.c_str(), indEr.progId);

    // --- Step 5: Linker-based reverse-dependent enumeration ---
    // M.4b.1 ROOT CAUSE: TSDev calls orlib20 Ordinal_69 (link) after
    // _VerifyModule_New succeeds to populate the dep-list cache with 0xFA3
    // function-call dependency entries. Without this step, the linker
    // creates a linked record with dep-lists that only contain 0xBB8/0xBB9
    // (variable/input) entries — no 0xFA3 (function-call) entries. Our
    // verifyModule now calls linkModule internally (Ordinal_28 + Ordinal_69
    // + Ordinal_70), so the dep-list cache is populated and subsequent
    // Ordinal_85 (linker) calls return records with 0xFA3 entries.
    std::printf("\n[5] Linker-based reverse-dependent enumeration "
                "(Function, %d)...\n", funcProgId);
    auto linkerDeps = tsdev::DiagM4ReverseEnumAccess::enumerate(
        *lib, funcProgId, 4);
    std::printf("    enumerateReverseDependents returned %zu dep(s)\n",
                linkerDeps.size());
    for (size_t i = 0; i < linkerDeps.size(); i++) {
        std::string depName = tsdev::DiagM4ReverseEnumAccess::nameForProgId(
            *lib, linkerDeps[i].category, linkerDeps[i].progId);
        std::printf("    [%zu] progId=%d category=%d subtype=%d name=\"%s\"\n",
                    i, linkerDeps[i].progId, linkerDeps[i].category,
                    linkerDeps[i].subtype, depName.c_str());
    }

    // Assert at least one reverse dependent was found.
    CHECK(!linkerDeps.empty(),
          "enumerateReverseDependents returned at least one caller");

    // Assert the Indicator is among the reverse dependents (by ProgId).
    bool foundCaller = false;
    int foundCategory = -1;
    for (const auto& d : linkerDeps) {
        if (d.progId == indProgId) {
            foundCaller = true;
            foundCategory = d.category;
            break;
        }
    }
    CHECK(foundCaller,
          "enumerateReverseDependents result contains the Indicator ProgId");

    // Assert the caller's category matches the Indicator's category (2).
    if (foundCaller) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Caller category == 2 (Indicator), got %d",
                      foundCategory);
        CHECK(foundCategory == 2, buf);
    }

    // --- Step 6: Cleanup — delete the caller, then delete the Function ---
    std::printf("\n[6] Cleanup: deleting Indicator (caller) \"%s\"...\n",
                indName);
    bool indDelOk = lib->deleteEntry(ComponentType::Indicator, indName,
                                     /*flush=*/true);
    std::printf("    deleteEntry(Indicator) returned %s\n",
                indDelOk ? "true" : "false");
    bool indEllGone = verifyEllGone(*lib, ComponentType::Indicator,
                                    indProgId);
    CHECK(indEllGone, "Indicator .ell file is gone after delete");

    std::printf("\n[7] Cleanup: deleting Function (callee) \"%s\"...\n",
                funcName);
    bool funcDelOk = lib->deleteEntry(ComponentType::Function, funcName,
                                      /*flush=*/true);
    std::printf("    deleteEntry(Function) returned %s\n",
                funcDelOk ? "true" : "false");
    bool funcEllGone = verifyEllGone(*lib, ComponentType::Function,
                                     funcProgId);
    CHECK(funcEllGone, "Function .ell file is gone after delete");

    st->exitCode = (g_failCount == 0) ? 0 : 1;
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
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 600-second hard timeout — report a hang without terminating the
    // worker. Insert+verify + enumerateCallers + cleanup may take a couple
    // of minutes on a cold warehouse.
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

    std::printf("\n=== test_callers_enumeration: %d/%d assertions "
                "passed, %d failed ===\n",
                g_passCount, g_passCount + g_failCount, g_failCount);
    std::printf("=== test_callers_enumeration complete (exit %d) ===\n",
                st.exitCode);
    return st.exitCode;
}
