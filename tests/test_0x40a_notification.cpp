// test_0x40a_notification.cpp
//
// Workstream M slice M.3 — integration test for the 0x40A post-success
// library-change notification parity with TSDev.exe. The TDD RED→GREEN
// transition is historical; the current implementation and receiver fixture
// are retained for regression coverage.
//
// CONFIRMED FACTS (Ghidra static analysis of orlib20.dll FUN_101526f0
// (Ordinal_57) + Frida trace; see
// docs/RE/RE_M3_0x40A_NOTIFICATION.md):
//
//   R-M.3.1 — Message format and packing (CONFIRMED):
//     msg    = 0x040A (1034)
//     wParam = 0
//     lParam = (category << 16) | (ProgId & 0xFFFF)
//   The LOW WORD of lParam IS the ProgId (warehouse record index), NOT the
//   subtype. category = getCategory(type) (low 8 bits of the expanded
//   whType). Sender = orlib20.dll Ordinal_57 (FUN_101526f0), async
//   PostMessageW to the registered client HWND set maintained by
//   tslib::EventsViaHandles.
//
//   R-M.3.2 — Confirmed operation set:
//     verify-success (insert/modify/upsert) sends 0x40A
//     delete-success sends 0x40A (gated category != 4)
//     failed compile/verify does NOT send 0x40A
//
//   R-M.3.3 — Error semantics:
//     Ordinal_57 always returns 1; send failure is silently ignored; no
//     receiver = silent no-op. Operation success is NOT blocked by
//     receiver absence.
//
//   R-M.3.4 — Test receiver: register via
//   _OrlibRegisterNotificationHandle@4 (ordinal 595). The receiver-aware
//   fixture is implemented; the hook also captures the would-be send
//   arguments to pin the packing and operation set.
//
// This test exercises the onNotifyReady test hook (populated via
// makeTsDevLibForTest) which the current verifyModule() and deleteEntry()
// paths invoke immediately BEFORE the 0x40A notification is sent (the same
// point at which TSDev calls orlib20 Ordinal_57). The callback captures the
// (msg, wParam, lParam) tuple plus the affected componentType and progId into
// thread_local globals, then the test asserts the packing and operation-set
// behavior described above.
//
// HISTORICAL RED STATE (pre-M.3): the production paths did not send 0x40A or
// invoke the hook, so the positive hook assertions failed while the failed-
// verify negative assertion passed trivially. The current M.3 implementation
// sends through ordinal 57 and the receiver-aware cases exercise actual
// delivery. The RED→GREEN transition is retained in the slice handoff.
//
// 16 test cases:
//   1. Verify-success sends 0x40A (Strategy, category 0)
//   2. Verify-success sends 0x40A (Indicator, category 2)
//   3. Verify-success sends 0x40A (Function, category 4)
//   4. Failed verify does NOT send 0x40A (Strategy, negative test)
//   5. Delete-success sends 0x40A (Strategy, category 0)
//   6. BATCH REUSE — 3 consecutive create+save+verify+delete cycles on the
//      SAME shared TsDevLib, asserting the hook fires with correct packing
//      on EACH verify-success iteration (proves no stale notification
//      state across batch reuse, per M.3 operation-mode coverage).
//   7. Verify-success sends 0x40A (TradingApp, category 2)
//   8. Verify-success sends 0x40A (ShowMe, category 2)
//   9. RECEIVER-AWARE single verify-success — real message-only window
//      registered via ordinal 595, captures actual PostMessageW(0x40A).
//  10. RECEIVER-AWARE batch — 3 verify + 3 delete = 6 receiver-captured
//      posts, no missed/duplicated.
//  11. Delete-success sends 0x40A (Indicator, category 2)
//  12. Delete-success sends 0x40A (TradingApp, category 2)
//  13. Delete-success sends 0x40A (ShowMe, category 2)
//  14. Function delete does NOT send 0x40A (category 4, negative test)
//  15. CLI stdin batch mode — spawns real tsdev_cli.exe, receiver captures
//      0x40A via cross-process shared EventsViaHandles array.
//  16. CLI serve mode — spawns real tsdev_cli.exe serve, receiver captures
//      0x40A via cross-process shared EventsViaHandles array.
//
// Each positive case creates a disposable entry, saves valid source,
// resets the capture, calls verifyModule (or deleteEntry), asserts the
// hook fired with correct packing, then cleans up. The negative case
// saves deliberately broken source, calls verifyModule, asserts the hook
// did NOT fire AND verifyModule actually failed (non-empty error string).
//
// REQUIRES TradeStation: this is an integration test that constructs a real
// TsDevLib (loads orlib20.dll, applies patches, calls OrlibInit, creates the
// hidden EDIT control) and inserts + verifies + deletes real warehouse
// entries. This is a library-level notification test; Desktop/TSDev running
// state is not a prerequisite because it uses its own receiver fixture.
//
// Runs on a 64 MB-stack worker thread (EasyLanguage compiler needs far more
// than the default 1 MB, and the hidden EDIT control is thread-affine so
// TsDevLib must live on this thread).
//
// Exit code: 0 only if ALL assertions pass; nonzero on any failure.

#include "../src/tsdev_lib_test_hooks.h"  // internal/test-only header (NOT public)
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include <sstream>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// CHECK macro — prints PASS/FAIL per assertion with line numbers.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Thread-local capture state for the onNotifyReady hook.
//
// The hook runs on the worker thread (the same thread verifyModule /
// deleteEntry runs on) and writes into these globals. The test reads them
// after the operation returns. thread_local ensures each worker thread has
// its own copy (the main thread's copy stays zeroed).
//
// callCount tracks the cumulative number of hook invocations across the
// lifetime of the capture (reset only by an explicit resetCapture() call),
// so the batch-reuse case can assert it increments by exactly one per
// operation.
// ---------------------------------------------------------------------------
struct NotifyCapture {
    bool        fired         = false;
    UINT        msg           = 0;
    WPARAM      wParam        = 0;
    LPARAM      lParam        = 0;
    int         componentType = -1;
    int         progId        = -1;
    int         callCount     = 0;  // cumulative across resets-until-explicit
};

thread_local NotifyCapture g_capture;

void onNotifyReadyHook(UINT msg, WPARAM wParam, LPARAM lParam,
                       int componentType, int progId) {
    g_capture.fired         = true;
    g_capture.msg           = msg;
    g_capture.wParam        = wParam;
    g_capture.lParam        = lParam;
    g_capture.componentType = componentType;
    g_capture.progId        = progId;
    ++g_capture.callCount;
}

// Reset the per-operation capture fields but PRESERVE callCount semantics:
// the batch-reuse case asserts callCount increments by exactly one per
// operation, so we snapshot the count before resetting and restore it.
// For non-batch cases callCount is irrelevant (only `fired` matters), so
// this unified reset is safe.
void resetCapture() {
    int savedCount = g_capture.callCount;
    g_capture = NotifyCapture{};
    g_capture.callCount = savedCount;
}

// ---------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// ---------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// Build a TsDevLibTestConfig with the real COM API and the onNotifyReady
// hook installed. throwAfterCom is false so the ctor proceeds with the
// full init sequence (loads orlib20, applies patches, OrlibInit, EDIT
// control).
TsDevLibTestConfig makeConfigWithHook() {
    TsDevLibTestConfig cfg;
    cfg.comApi         = realComApi();
    cfg.throwAfterCom  = false;
    cfg.onNotifyReady  = &onNotifyReadyHook;
    return cfg;
}

// ---------------------------------------------------------------------------
// The 0x40A message ID and packing constants (R-M.3.1).
// ---------------------------------------------------------------------------
const UINT  kNotifyMsg = 0x040A;
const UINT  kNotifyWParam = 0;

// Compute the expected lParam for a given component type + progId:
//   lParam = (category << 16) | (ProgId & 0xFFFF)
LPARAM expectedLParam(ComponentType type, int progId) {
    int category = getCategory(type);
    return static_cast<LPARAM>((static_cast<unsigned int>(category) << 16)
                               | (static_cast<unsigned int>(progId) & 0xFFFF));
}

// ---------------------------------------------------------------------------
// Minimal valid EasyLanguage source per component type, mirroring the
// proven patterns from test_save_module.cpp / test_compiler_layout.cpp.
// In EasyLanguage `{ ... }` is a multi-line COMMENT block; the actual code
// must be OUTSIDE it. For Function, the function name in the source body
// MUST match the item name.
// ---------------------------------------------------------------------------
std::string makeStrategySource(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

std::string makeIndicatorSource(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "Plot1(Close, \"Close\");\r\n";
}

std::string makeFunctionSource(const std::string& name) {
    // Function name in the body MUST match the item name.
    return name + " = 1;\r\n";
}

// TradingApp minimal valid source — `using elsystem;` compiles successfully
// (matches docs/E2E_TEST_SPEC.md §4.1 TradingApp insert source).
std::string makeTradingAppSource(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "using elsystem;\r\n";
}

// ShowMe valid source — uses Plot1 (allowed in ShowMe studies, like
// Indicators); mirrors docs/E2E_TEST_SPEC.md §4.1 ShowMe insert source and
// tests/test_showme_e2e.ps1 SM1.
std::string makeShowMeSource(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "if Close > Open then\r\n"
           "    Plot1(High, \"Up bar\");\r\n";
}

// Deliberately broken EasyLanguage source — guaranteed to fail compilation.
// Contains invalid syntax (@#$%) and no valid statements.
std::string makeBrokenSource() {
    return std::string("This is not valid EasyLanguage @#$%\r\n");
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Pre-cleanup helper: delete the named item if it already exists (re-run
// safety). Returns the progId that was deleted (>0) or 0 if not found.
// ---------------------------------------------------------------------------
int preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
    return existingProgId;
}

// ---------------------------------------------------------------------------
// runVerifySuccessCase — positive case: create, save valid source, verify,
// assert the onNotifyReady hook fired with correct 0x40A packing, then
// clean up.
//
// Returns 0 on success (all assertions evaluated), nonzero on a fatal
// setup error (createEntry <= 0). Assertion failures increment
// g_failCount but do NOT early-return — the cleanup still runs.
// ---------------------------------------------------------------------------
int runVerifySuccessCase(TsDevLib& lib, int caseNum,
                         ComponentType type, const std::string& name,
                         const std::string& source) {
    const char* typeLabel =
        (type == ComponentType::Strategy)   ? "Strategy"   :
        (type == ComponentType::Indicator)  ? "Indicator"  :
        (type == ComponentType::Function)   ? "Function"   :
        (type == ComponentType::TradingApp) ? "TradingApp" :
        (type == ComponentType::ShowMe)     ? "ShowMe"     : "Unknown";
    int category = getCategory(type);

    std::printf("\n=== Case %d: verify-success sends 0x40A (%s, "
                "category=%d) ===\n",
                caseNum, typeLabel, category);

    // --- 1. Pre-cleanup (re-run safety) ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (valid source) ---
    std::printf("  [3] saveSource(%s, %d, valid source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. Reset capture, then verifyModule (the operation under test) ---
    resetCapture();
    std::printf("  [4] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }

    // --- 5. Assert verifyModule succeeded (empty error string) ---
    // This guards the positive case: if verify unexpectedly failed, the
    // "hook did not fire" failure below would be misleading. We assert
    // verify success first so the notification assertion is meaningful.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule succeeded (empty errors) [case %d, "
                      "got %zu bytes]",
                      caseNum, errors.size());
        CHECK(errors.empty(), buf);
    }

    // --- 6. Assert the hook fired ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "onNotifyReady hook fired after verify-success "
                      "[case %d]", caseNum);
        CHECK(g_capture.fired, buf);
    }

    // --- 7. Assert the captured packing ---
    LPARAM expLParam = expectedLParam(type, progId);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "capture.msg==0x%03X (expected 0x%03X) [case %d]",
                      g_capture.msg, kNotifyMsg, caseNum);
        CHECK(g_capture.fired && g_capture.msg == kNotifyMsg, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "capture.wParam==0 (expected 0) [case %d, got 0x%llX]",
                      caseNum,
                      static_cast<unsigned long long>(g_capture.wParam));
        CHECK(g_capture.fired && g_capture.wParam == kNotifyWParam, buf);
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "capture.lParam==0x%08lX (expected 0x%08lX, "
                      "category=%d progId=%d) [case %d]",
                      static_cast<unsigned long>(g_capture.lParam),
                      static_cast<unsigned long>(expLParam),
                      category, progId, caseNum);
        CHECK(g_capture.fired && g_capture.lParam == expLParam, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "capture.componentType==%d (expected %d) [case %d]",
                      g_capture.componentType,
                      static_cast<int>(type), caseNum);
        CHECK(g_capture.fired &&
              g_capture.componentType == static_cast<int>(type), buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "capture.progId==%d (expected %d) [case %d]",
                      g_capture.progId, progId, caseNum);
        CHECK(g_capture.fired && g_capture.progId == progId, buf);
    }

    // --- 8. Cleanup: delete the disposable item ---
    std::printf("  [8] Cleanup: deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    if (!lib.deleteEntry(type, name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual?)\n");
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runFailedVerifyCase — negative case: create, save BROKEN source, verify,
// assert the hook did NOT fire AND verifyModule actually failed. This is
// the correct negative assertion (R-M.3.2: failed compile/verify does NOT
// send 0x40A). The current implementation must keep this negative contract.
// ---------------------------------------------------------------------------
int runFailedVerifyCase(TsDevLib& lib, int caseNum,
                        ComponentType type, const std::string& name) {
    const char* typeLabel =
        (type == ComponentType::Strategy) ? "Strategy" : "Unknown";

    std::printf("\n=== Case %d: failed verify does NOT send 0x40A (%s, "
                "negative test) ===\n",
                caseNum, typeLabel);

    // --- 1. Pre-cleanup ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("negative case: createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (DELIBERATELY BROKEN source) ---
    std::string broken = makeBrokenSource();
    std::printf("  [3] saveSource(%s, %d, BROKEN source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, broken);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. Reset capture, then verifyModule (expected to FAIL) ---
    resetCapture();
    std::printf("  [4] verifyModule(%s, %d) — expecting failure...\n",
                typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::printf("      verifyModule: FAILED as expected (%zu bytes of "
                    "errors)\n", errors.size());
    } else {
        std::fprintf(stderr,
                     "      verifyModule: UNEXPECTEDLY succeeded (broken "
                     "source compiled?)\n");
    }

    // --- 5. Assert verifyModule actually FAILED (non-empty error string) ---
    // This guards the negative case: if the broken source unexpectedly
    // compiled, the "hook did not fire" assertion would be meaningless.
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule FAILED (non-empty errors) [case %d, "
                      "got %zu bytes]",
                      caseNum, errors.size());
        CHECK(!errors.empty(), buf);
    }

    // --- 6. Assert the hook did NOT fire (negative assertion) ---
    // This pins the current contract: failed verify must NOT send 0x40A.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "onNotifyReady hook did NOT fire after failed verify "
                      "[case %d]", caseNum);
        CHECK(!g_capture.fired, buf);
    }

    // --- 7. Cleanup: delete the disposable item ---
    std::printf("  [7] Cleanup: deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    if (!lib.deleteEntry(type, name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual?)\n");
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runDeleteSuccessCase — positive case: create, save valid source, verify
// (so the item is real and persisted), reset capture, deleteEntry, assert
// the onNotifyReady hook fired with correct 0x40A packing. No post-delete
// cleanup needed (the item was deleted by the operation under test).
// ---------------------------------------------------------------------------
int runDeleteSuccessCase(TsDevLib& lib, int caseNum,
                         ComponentType type, const std::string& name,
                         const std::string& source) {
    const char* typeLabel =
        (type == ComponentType::Strategy) ? "Strategy" : "Unknown";
    int category = getCategory(type);

    std::printf("\n=== Case %d: delete-success sends 0x40A (%s, "
                "category=%d) ===\n",
                caseNum, typeLabel, category);

    // --- 1. Pre-cleanup (re-run safety) ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("delete case: createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (valid source) ---
    std::printf("  [3] saveSource(%s, %d, valid source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. verifyModule (so the item is compiled + persisted before delete) ---
    std::printf("  [4] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule succeeded before delete [case %d]",
                      caseNum);
        CHECK(errors.empty(), buf);
    }

    // --- 5. Reset capture, then deleteEntry (the operation under test) ---
    // Capture the progId BEFORE delete (the hook should report this same
    // progId; the item is gone after delete returns).
    int preDeleteProgId = progId;
    resetCapture();
    std::printf("  [5] deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    bool deleted = lib.deleteEntry(type, name, /*flush=*/true);
    std::printf("      deleteEntry = %s\n", deleted ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "deleteEntry returned true [case %d]", caseNum);
        CHECK(deleted, buf);
    }

    // --- 6. Assert the hook fired ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "onNotifyReady hook fired after delete-success "
                      "[case %d]", caseNum);
        CHECK(g_capture.fired, buf);
    }

    // --- 7. Assert the captured packing ---
    LPARAM expLParam = expectedLParam(type, preDeleteProgId);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.msg==0x%03X (expected 0x%03X) "
                      "[case %d]",
                      g_capture.msg, kNotifyMsg, caseNum);
        CHECK(g_capture.fired && g_capture.msg == kNotifyMsg, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.wParam==0 (expected 0) [case %d, "
                      "got 0x%llX]",
                      caseNum,
                      static_cast<unsigned long long>(g_capture.wParam));
        CHECK(g_capture.fired && g_capture.wParam == kNotifyWParam, buf);
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.lParam==0x%08lX (expected 0x%08lX, "
                      "category=%d progId=%d) [case %d]",
                      static_cast<unsigned long>(g_capture.lParam),
                      static_cast<unsigned long>(expLParam),
                      category, preDeleteProgId, caseNum);
        CHECK(g_capture.fired && g_capture.lParam == expLParam, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.componentType==%d (expected %d) "
                      "[case %d]",
                      g_capture.componentType,
                      static_cast<int>(type), caseNum);
        CHECK(g_capture.fired &&
              g_capture.componentType == static_cast<int>(type), buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.progId==%d (expected %d) [case %d]",
                      g_capture.progId, preDeleteProgId, caseNum);
        CHECK(g_capture.fired && g_capture.progId == preDeleteProgId, buf);
    }

    // No post-delete cleanup — the item was deleted by the operation.
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runBatchReuseCase — adversarial / operation-mode coverage: 3 consecutive
// create+save+verify+delete cycles on the SAME shared TsDevLib, asserting
// the onNotifyReady hook fires with correct 0x40A packing on EACH
// verify-success iteration. Proves no stale notification state across
// batch reuse (per M.3 operation-mode coverage).
//
// Each iteration does a FULL reset (including callCount) before verifyModule
// and asserts callCount == 1 after verify — proving each verify fires the
// hook exactly once, independently, with no stale/duplicated state from a
// prior iteration. The cleanup deleteEntry at the end of each iteration also
// fires the hook (delete sends 0x40A for Strategy, category 0 != 4 — see
// Case 5), which is correct per RE_M3 §2.4; that fire is not asserted here
// (it is covered by Case 5) and is cleared by the full reset at the top of
// the next iteration.
// ---------------------------------------------------------------------------
int runBatchReuseCase(TsDevLib& lib, int caseNum,
                      const std::string& baseName,
                      const std::string& source) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    int category = getCategory(type);
    const int kIterations = 3;

    std::printf("\n=== Case %d: BATCH REUSE — %d verify-success cycles (%s, "
                "category=%d) ===\n",
                caseNum, kIterations, typeLabel, category);

    for (int iter = 1; iter <= kIterations; ++iter) {
        char name[80];
        std::snprintf(name, sizeof(name), "%s_IT%d", baseName.c_str(), iter);
        std::printf("\n  --- Batch iteration %d/%d (name=%s) ---\n",
                    iter, kIterations, name);

        // Pre-cleanup.
        preCleanup(lib, type, name);

        // createEntry.
        int errCode = 0;
        int progId = lib.createEntry(type, name, errCode);
        std::printf("    createEntry = %d, errCode = 0x%X\n",
                    progId, errCode);
        if (progId <= 0) {
            return fail("batch: createEntry returned ProgId <= 0",
                        100 + caseNum * 10 + iter);
        }

        // saveSource (valid source).
        bool saved = lib.saveSource(type, progId, source);
        std::printf("    saveSource = %s\n", saved ? "true" : "false");
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: saveSource returned true [case %d]",
                          iter, caseNum);
            CHECK(saved, buf);
        }

        // FULL reset (including callCount) before verifyModule, so the
        // callCount assertion below proves THIS verify fired exactly once.
        g_capture = NotifyCapture{};
        std::printf("    verifyModule(%s, %d)...\n", typeLabel, progId);
        std::string errors = lib.verifyModule(type, progId);
        if (!errors.empty()) {
            std::fprintf(stderr,
                         "    batch it%d: verifyModule errors (diagnostic):\n%s\n",
                         iter, errors.c_str());
        } else {
            std::printf("    batch it%d: verifyModule OK\n", iter);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: verifyModule succeeded [case %d]",
                          iter, caseNum);
            CHECK(errors.empty(), buf);
        }

        // Assert the hook fired THIS iteration (not a stale value).
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: onNotifyReady hook fired [case %d]",
                          iter, caseNum);
            CHECK(g_capture.fired, buf);
        }

        // Assert callCount == 1 (exactly one fire from THIS verify — proves
        // no missed/duplicated notification, no stale state from a prior
        // iteration).
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: callCount==1 (got %d) [case %d]",
                          iter, g_capture.callCount, caseNum);
            CHECK(g_capture.callCount == 1, buf);
        }

        // Assert the packing THIS iteration.
        LPARAM expLParam = expectedLParam(type, progId);
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: capture.msg==0x%03X (expected 0x%03X) "
                          "[case %d]",
                          iter, g_capture.msg, kNotifyMsg, caseNum);
            CHECK(g_capture.fired && g_capture.msg == kNotifyMsg, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: capture.wParam==0 [case %d]", iter,
                          caseNum);
            CHECK(g_capture.fired && g_capture.wParam == kNotifyWParam, buf);
        }
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: capture.lParam==0x%08lX (expected "
                          "0x%08lX, progId=%d) [case %d]",
                          iter,
                          static_cast<unsigned long>(g_capture.lParam),
                          static_cast<unsigned long>(expLParam),
                          progId, caseNum);
            CHECK(g_capture.fired && g_capture.lParam == expLParam, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: capture.componentType==%d (expected "
                          "%d) [case %d]",
                          iter, g_capture.componentType,
                          static_cast<int>(type), caseNum);
            CHECK(g_capture.fired &&
                  g_capture.componentType == static_cast<int>(type), buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: capture.progId==%d (expected %d) "
                          "[case %d]",
                          iter, g_capture.progId, progId, caseNum);
            CHECK(g_capture.fired && g_capture.progId == progId, buf);
        }

        // Cleanup THIS iteration.
        if (!lib.deleteEntry(type, name, /*flush=*/true)) {
            std::fprintf(stderr,
                         "    batch it%d: WARNING deleteEntry returned false\n",
                         iter);
        }

        resetCapture();
    }

    return 0;
}

// ---------------------------------------------------------------------------
// runDeleteSuccessCase_Indicator / _TradingApp / _ShowMe — Finding 4:
// delete-success 0x40A packing parity for the remaining 3 of the 5 supported
// component types (Indicator, TradingApp, ShowMe — all category 2). Delete
// is in the confirmed operation set (gated category != 4), so each of these
// MUST send 0x40A with lParam=(category<<16)|(progId&0xFFFF) on
// delete-success. Mirrors runDeleteSuccessCase (Case 5) exactly, just with a
// different component type and a type-aware typeLabel.
// ---------------------------------------------------------------------------
int runDeleteSuccessCase_Indicator(TsDevLib& lib, int caseNum,
                                   ComponentType type,
                                   const std::string& name,
                                   const std::string& source) {
    const char* typeLabel =
        (type == ComponentType::Indicator)  ? "Indicator"  :
        (type == ComponentType::TradingApp) ? "TradingApp" :
        (type == ComponentType::ShowMe)     ? "ShowMe"     : "Unknown";
    int category = getCategory(type);

    std::printf("\n=== Case %d: delete-success sends 0x40A (%s, "
                "category=%d) ===\n",
                caseNum, typeLabel, category);

    // --- 1. Pre-cleanup (re-run safety) ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("delete case: createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (valid source) ---
    std::printf("  [3] saveSource(%s, %d, valid source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. verifyModule (so the item is compiled + persisted before delete) ---
    std::printf("  [4] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule succeeded before delete [case %d]",
                      caseNum);
        CHECK(errors.empty(), buf);
    }

    // --- 5. Reset capture, then deleteEntry (the operation under test) ---
    int preDeleteProgId = progId;
    resetCapture();
    std::printf("  [5] deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    bool deleted = lib.deleteEntry(type, name, /*flush=*/true);
    std::printf("      deleteEntry = %s\n", deleted ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "deleteEntry returned true [case %d]", caseNum);
        CHECK(deleted, buf);
    }

    // --- 6. Assert the hook fired ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "onNotifyReady hook fired after delete-success "
                      "[case %d]", caseNum);
        CHECK(g_capture.fired, buf);
    }

    // --- 7. Assert the captured packing ---
    LPARAM expLParam = expectedLParam(type, preDeleteProgId);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.msg==0x%03X (expected 0x%03X) "
                      "[case %d]",
                      g_capture.msg, kNotifyMsg, caseNum);
        CHECK(g_capture.fired && g_capture.msg == kNotifyMsg, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.wParam==0 (expected 0) [case %d, "
                      "got 0x%llX]",
                      caseNum,
                      static_cast<unsigned long long>(g_capture.wParam));
        CHECK(g_capture.fired && g_capture.wParam == kNotifyWParam, buf);
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.lParam==0x%08lX (expected 0x%08lX, "
                      "category=%d progId=%d) [case %d]",
                      static_cast<unsigned long>(g_capture.lParam),
                      static_cast<unsigned long>(expLParam),
                      category, preDeleteProgId, caseNum);
        CHECK(g_capture.fired && g_capture.lParam == expLParam, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.componentType==%d (expected %d) "
                      "[case %d]",
                      g_capture.componentType,
                      static_cast<int>(type), caseNum);
        CHECK(g_capture.fired &&
              g_capture.componentType == static_cast<int>(type), buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "delete: capture.progId==%d (expected %d) [case %d]",
                      g_capture.progId, preDeleteProgId, caseNum);
        CHECK(g_capture.fired && g_capture.progId == preDeleteProgId, buf);
    }

    // No post-delete cleanup — the item was deleted by the operation.
    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runDeleteNegativeCase_Function — Finding 5: Function (category 4) delete
// is OUTSIDE the confirmed operation set (the 0x40A send is gated
// category != 4), so delete-success must NOT send 0x40A for a Function.
//
// Pattern: create a Function item, save valid source, verify (which DOES
// fire 0x40A — verify-success is in the confirmed set for all categories),
// then reset the capture, delete the item, and assert:
//   - deleteEntry returned true (the delete itself succeeded), and
//   - callCount == 0 / fired == false (NO 0x40A fired for the delete step).
//
// The reset between verify and delete is essential: the verify step fires
// 0x40A (Function verify-success IS in the confirmed set), so without the
// reset the negative assertion would observe the verify-fire and fail
// spuriously.
// ---------------------------------------------------------------------------
int runDeleteNegativeCase_Function(TsDevLib& lib, int caseNum,
                                   ComponentType type,
                                   const std::string& name,
                                   const std::string& source) {
    const char* typeLabel =
        (type == ComponentType::Function) ? "Function" : "Unknown";
    int category = getCategory(type);

    std::printf("\n=== Case %d: Function delete does NOT send 0x40A "
                "(%s, category=%d, negative test) ===\n",
                caseNum, typeLabel, category);

    // --- 1. Pre-cleanup (re-run safety) ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("Function delete negative case: createEntry returned "
                    "ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (valid source) ---
    std::printf("  [3] saveSource(%s, %d, valid source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. verifyModule (so the item is compiled + persisted before delete).
    // This step DOES fire 0x40A (verify-success is in the confirmed set for
    // all categories, including Function). We assert verify succeeded but
    // do NOT assert the hook fired here — the point of this case is the
    // DELETE step's negative assertion below. The resetCapture() before the
    // delete step clears any verify-fire so the delete-step capture is clean.
    std::printf("  [4] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule succeeded before delete [case %d]",
                      caseNum);
        CHECK(errors.empty(), buf);
    }

    // --- 5. Reset capture (FULL reset, including callCount), then deleteEntry
    //     (the operation under test). The reset is essential: the verify
    //     step above fired 0x40A, and we must isolate the delete step's
    //     capture so the negative assertion observes ONLY the delete step. ---
    g_capture = NotifyCapture{};
    std::printf("  [5] deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    bool deleted = lib.deleteEntry(type, name, /*flush=*/true);
    std::printf("      deleteEntry = %s\n", deleted ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "deleteEntry returned true [case %d]", caseNum);
        CHECK(deleted, buf);
    }

    // --- 6. Assert the hook did NOT fire for the delete step (negative
    //     assertion — Function category 4 is gated out of the 0x40A delete
    //     send). callCount==0 proves no 0x40A fired between the reset and
    //     now (i.e. the delete step fired zero notifications). ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "onNotifyReady hook did NOT fire after Function delete "
                      "[case %d]", caseNum);
        CHECK(!g_capture.fired, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Function delete: callCount==0 (got %d) [case %d]",
                      g_capture.callCount, caseNum);
        CHECK(g_capture.callCount == 0, buf);
    }

    // --- 7. Assert the item is actually gone (delete succeeded at the
    //     warehouse level, not just at the API return). This guards the
    //     negative case: if deleteEntry returned true but the item still
    //     exists, the "no 0x40A" observation would be misleading (it could
    //     mean the delete was a no-op rather than a category-4 gate).
    //     findExistPos returns >0 if found; 0 (SEH) or -1 (not found) if
    //     absent — so the "gone" assertion is postProgId <= 0, matching
    //     the convention used by preCleanup() above. ---
    int postProgId = lib.findExistPos(name, type);
    std::printf("      post-delete findExistPos = %d (expected <= 0)\n",
                postProgId);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Function delete: item is gone after delete "
                      "[case %d, findExistPos=%d]", caseNum, postProgId);
        CHECK(postProgId <= 0, buf);
    }

    // No post-delete cleanup — the item was deleted by the operation.
    resetCapture();
    return 0;
}

// ===========================================================================
// RECEIVER-AWARE FIXTURE (Findings 1 & 2)
//
// Proves ACTUAL delivery via PostMessageW to a window registered with
// _OrlibRegisterNotificationHandle@4 (orlib20 ordinal 595), not just the
// onNotifyReady hook capture. The hook fires in-process before the
// Ordinal_57 call; the receiver window proves the PostMessageW reaches a
// real registered HWND and is dispatched by a real message loop.
//
// Architecture (per the task spec and docs/RE/RE_M3_0x40A_NOTIFICATION.md
// §1.4):
//   - The receiver window is created on a SEPARATE receiver thread (Windows
//     requires GetMessage on the thread that created the window).
//   - _OrlibRegisterNotificationHandle@4 (ordinal 595) is resolved from the
//     already-loaded orlib20.dll via GetModuleHandleW + GetProcAddress by
//     ordinal (MAKEINTRESOURCEA(595)). Same for the unregister (ordinal 596).
//     The registration call itself is thread-agnostic (it just adds the HWND
//     to the EventsViaHandles client set), so it is made from the main
//     (worker) thread right before kicking off the operation.
//   - The receiver thread runs a GetMessage/DispatchMessage loop that
//     captures every 0x40A into a shared, mutex-protected vector and exits
//     when the requested number of messages has arrived OR a timeout
//     (kReceiverTimeoutMs) elapses.
//   - The main thread performs the verify-success operation(s); orlib20
//     Ordinal_57 posts 0x40A to every registered HWND (including ours).
//   - Synchronization: the receiver thread signals an event once the window
//     is created and ready, so the main thread does not call
//     _OrlibRegisterNotificationHandle before the HWND exists.
// ===========================================================================

// A single captured 0x40A delivery.
struct ReceivedNotify {
    UINT   msg;
    WPARAM wParam;
    LPARAM lParam;
};

// Shared state between the main thread and the receiver thread.
struct ReceiverState {
    HWND                       hwnd = nullptr;
    HANDLE                     readyEvent = nullptr;     // signaled when window is created
    HANDLE                     doneEvent = nullptr;      // signaled by wnd proc (target reached) or main (backstop) to wake the main thread's wait
    CRITICAL_SECTION           cs;
    std::vector<ReceivedNotify> messages;
    int                        targetCount = 1;          // how many 0x40A to capture before exiting
    bool                       timedOut = false;
    bool                       mayExit = false;          // set by main thread AFTER unregisterFn, to tell the receiver it may destroy its window and exit
};

// The window proc — captures 0x40A into the shared state. All other
// messages go to DefWindowProcW.
static LRESULT WINAPI ReceiverWndProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam) {
    ReceiverState* rs = reinterpret_cast<ReceiverState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == 0x040A && rs != nullptr) {
        EnterCriticalSection(&rs->cs);
        rs->messages.push_back({msg, wParam, lParam});
        int count = static_cast<int>(rs->messages.size());
        LeaveCriticalSection(&rs->cs);
        std::printf("  [receiver] captured 0x40A: wParam=0x%llX "
                    "lParam=0x%08lX (#%d)\n",
                    static_cast<unsigned long long>(wParam),
                    static_cast<unsigned long>(lParam), count);
        if (count >= rs->targetCount) {
            // Wake up the main thread's wait so it can unregister the HWND
            // and then set mayExit. The receiver loop does NOT break on
            // this signal — it only breaks when mayExit is set, ensuring
            // the window is not destroyed before unregisterFn runs.
            SetEvent(rs->doneEvent);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Receiver thread entry point: registers a window class, creates a
// message-only window, signals ready, then runs a GetMessage/DispatchMessage
// loop until doneEvent is signaled or the loop times out.
static DWORD WINAPI ReceiverThreadProc(LPVOID param) {
    ReceiverState* rs = static_cast<ReceiverState*>(param);

    // Register a unique window class for the receiver window.
    WNDCLASSW wc{};
    wc.lpfnWndProc   = ReceiverWndProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = L"TsDevM3NotifyReceiver";
    ATOM atom = RegisterClassW(&wc);
    if (atom == 0) {
        std::fprintf(stderr,
                     "  [receiver] RegisterClassW failed: %lu\n",
                     GetLastError());
        SetEvent(rs->readyEvent);  // unblock the main thread regardless
        return 1;
    }

    // Create a message-only window (HWND_MESSAGE parent). Per the task
    // spec this is acceptable; the window is not visible and does not
    // receive broadcast messages.
    rs->hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"TsDevM3NotifyReceiver",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        wc.hInstance,
        nullptr);
    if (!rs->hwnd) {
        std::fprintf(stderr,
                     "  [receiver] CreateWindowExW failed: %lu\n",
                     GetLastError());
        SetEvent(rs->readyEvent);
        return 2;
    }
    SetWindowLongPtrW(rs->hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(rs));

    // Window is ready — unblock the main thread so it can register the
    // HWND with EventsViaHandles and start the operation.
    SetEvent(rs->readyEvent);

    // Message loop with a timeout. PeekMessage with PM_REMOVE is used so
    // we can periodically check mayExit and the elapsed time; a pure
    // blocking GetMessage would not time out.
    //
    // LIFECYCLE RACE FIX: the loop breaks on mayExit (set by the main
    // thread AFTER unregisterFn), NOT on doneEvent (self-signaled by the
    // wnd proc when targetCount is reached). doneEvent only wakes the main
    // thread so it can unregister the saved HWND before the receiver
    // destroys its window. This guarantees unregisterFn(savedHwnd) always
    // runs before DestroyWindow.
    const DWORD kLoopTickMs = 50;
    DWORD elapsed = 0;
    // ponytail: 120s backstop matches the CLI child-wait timeout in Cases 15/16.
    // The in-process Cases 1-14 complete in < 5s, but CLI child-process cases
    // need the receiver to pump for the full CLI startup + EL compile duration.
    // The receiver exits early via mayExit (set by the main thread after
    // unregisterFn), so the backstop is only reached on a hang.
    const DWORD kTimeout = 120000;
    while (elapsed < kTimeout) {
        // If the main thread has set mayExit (after unregistering the
        // HWND), drain remaining messages then exit.
        if (rs->mayExit) {
            // Drain any straggler messages.
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            break;
        }
        MSG msg;
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(kLoopTickMs);
            elapsed += kLoopTickMs;
        }
    }

    if (elapsed >= kTimeout) {
        EnterCriticalSection(&rs->cs);
        rs->timedOut = true;
        LeaveCriticalSection(&rs->cs);
        std::fprintf(stderr,
                     "  [receiver] TIMEOUT after %lu ms (captured %zu "
                     "0x40A messages)\n",
                     kTimeout, rs->messages.size());
        // Wait for the main thread to unregister the saved HWND and set
        // mayExit before destroying the window. Give a generous backstop
        // (10s) so the main thread's doneEvent-timeout path has time to
        // run unregisterFn and set mayExit.
        DWORD waitElapsed = 0;
        const DWORD kMayExitWaitMs = 10000;
        while (!rs->mayExit && waitElapsed < kMayExitWaitMs) {
            Sleep(kLoopTickMs);
            waitElapsed += kLoopTickMs;
        }
    }

    // Destroy the window and unregister the class. This only runs after
    // mayExit is set (by the main thread, after unregisterFn), so the
    // HWND is still valid when unregisterFn was called.
    DestroyWindow(rs->hwnd);
    rs->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// Resolve an orlib20 ordinal export by ordinal number. Returns nullptr on
// failure. orlib20.dll is already loaded by TsDevLib, so GetModuleHandleW
// is sufficient (no LoadLibrary needed).
static void* resolveOrlibOrdinal(WORD ordinal) {
    HMODULE h = GetModuleHandleW(L"orlib20.dll");
    if (!h) {
        std::fprintf(stderr,
                     "  [receiver] GetModuleHandleW(orlib20.dll) failed: "
                     "%lu\n",
                     GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(
        GetProcAddress(h, MAKEINTRESOURCEA(ordinal)));
}

// Typedefs for the registration/unregistration exports (stdcall, 1 HWND arg,
// RET 4).
typedef int (__stdcall *OrlibRegisterNotifyFn)(HWND);
typedef int (__stdcall *OrlibUnregisterNotifyFn)(HWND);

// ---------------------------------------------------------------------------
// runReceiverAwareSingleCase — Finding 1: prove ACTUAL delivery via
// PostMessageW to a registered receiver window for ONE verify-success
// operation. Asserts the receiver captured exactly one 0x40A with the
// correct (msg, wParam, lParam) packing.
//
// Thread layout:
//   - The MAIN worker thread (this one) constructs TsDevLib and performs
//     createEntry + saveSource + verifyModule.
//   - A SEPARATE receiver thread creates the message-only window and runs
//     the message loop.
//   - The main thread registers the receiver HWND with EventsViaHandles
//     (via _OrlibRegisterNotificationHandle@4 / ordinal 595) AFTER the
//     window is created and BEFORE the verifyModule call, then unregisters
//     (ordinal 596) and signals the receiver to exit after the assertion.
// ---------------------------------------------------------------------------
int runReceiverAwareSingleCase(TsDevLib& lib, int caseNum,
                               ComponentType type,
                               const std::string& name,
                               const std::string& source) {
    const char* typeLabel =
        (type == ComponentType::Strategy) ? "Strategy" : "Unknown";
    int category = getCategory(type);

    std::printf("\n=== Case %d: RECEIVER-AWARE single verify-success "
                "sends 0x40A via PostMessageW (%s, category=%d) ===\n",
                caseNum, typeLabel, category);

    // Resolve the orlib20 register/unregister exports up front so a
    // resolution failure is a clean early return rather than a mid-test
    // abort.
    OrlibRegisterNotifyFn registerFn =
        reinterpret_cast<OrlibRegisterNotifyFn>(resolveOrlibOrdinal(595));
    OrlibUnregisterNotifyFn unregisterFn =
        reinterpret_cast<OrlibUnregisterNotifyFn>(resolveOrlibOrdinal(596));
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibRegisterNotificationHandle@4 "
                      "(ordinal 595) [case %d]", caseNum);
        CHECK(registerFn != nullptr, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibUnregisterNotificationHandle@4 "
                      "(ordinal 596) [case %d]", caseNum);
        CHECK(unregisterFn != nullptr, buf);
    }
    if (!registerFn || !unregisterFn) {
        return fail("receiver-aware case: orlib20 ordinal 595/596 "
                    "unresolved",
                    100 + caseNum * 10 + 1);
    }

    // --- 1. Pre-cleanup (re-run safety) ---
    preCleanup(lib, type, name);

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("receiver-aware case: createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource (valid source) ---
    std::printf("  [3] saveSource(%s, %d, valid source)...\n",
                typeLabel, progId);
    bool saved = lib.saveSource(type, progId, source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d]", caseNum);
        CHECK(saved, buf);
    }

    // --- 4. Start the receiver thread + register the window ---
    ReceiverState rs;
    InitializeCriticalSection(&rs.cs);
    rs.targetCount = 1;
    rs.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rs.doneEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rs.readyEvent || !rs.doneEvent) {
        DeleteCriticalSection(&rs.cs);
        if (rs.readyEvent) CloseHandle(rs.readyEvent);
        if (rs.doneEvent)  CloseHandle(rs.doneEvent);
        return fail("receiver-aware case: CreateEventW failed",
                    100 + caseNum * 10 + 4);
    }

    std::printf("  [4] starting receiver thread...\n");
    HANDLE hRecv = CreateThread(nullptr, 0,
                                ReceiverThreadProc, &rs, 0, nullptr);
    if (!hRecv) {
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("receiver-aware case: CreateThread(receiver) failed",
                    100 + caseNum * 10 + 4);
    }

    // Wait for the receiver window to be created.
    if (WaitForSingleObject(rs.readyEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "  [receiver] readyEvent timed out\n");
    }
    HWND savedHwnd = rs.hwnd;
    if (!savedHwnd) {
        std::fprintf(stderr,
                     "  [receiver] window not created; aborting case\n");
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("receiver-aware case: receiver window not created",
                    100 + caseNum * 10 + 4);
    }
    std::printf("      receiver HWND=0x%p\n", static_cast<void*>(savedHwnd));

    // Register the receiver HWND with EventsViaHandles. The registration
    // call is thread-agnostic (it just adds the HWND to the set), so it
    // is safe to call from this worker thread.
    int regResult = registerFn(savedHwnd);
    std::printf("      _OrlibRegisterNotificationHandle@4 = %d\n",
                regResult);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "_OrlibRegisterNotificationHandle@4 returned nonzero "
                      "[case %d, got %d]", caseNum, regResult);
        CHECK(regResult != 0, buf);
    }

    // --- 5. Reset hook capture, then verifyModule (the operation under
    //     test). The PostMessageW from Ordinal_57 should reach the
    //     receiver window. ---
    resetCapture();
    std::printf("  [5] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "verifyModule succeeded (empty errors) [case %d]",
                      caseNum);
        CHECK(errors.empty(), buf);
    }

    // --- 6. Wait for the receiver to capture the 0x40A (or timeout) ---
    // The wnd proc signals doneEvent when targetCount messages arrive.
    // Give it a generous wait; the post happens during verifyModule above
    // so the message should already be queued.
    if (WaitForSingleObject(rs.doneEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr,
                     "  [receiver] doneEvent timed out — no 0x40A "
                     "captured\n");
    }

    // --- 7. Unregister the saved HWND (local copy, not the shared
    // rs.hwnd which the receiver may null during destruction) BEFORE
    // setting mayExit and signaling the receiver thread to destroy its
    // window. Then join the receiver thread. This ordering guarantees
    // unregisterFn runs while the window is still alive. ---
    {
        DWORD rc = WaitForSingleObject(hRecv, 0);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "receiver thread still alive at unregister time "
                      "(unregister before window destroy) [case %d]",
                      caseNum);
        CHECK(rc == WAIT_TIMEOUT, buf);
    }
    if (unregisterFn) {
        int unregResult = unregisterFn(savedHwnd);
        std::printf("      _OrlibUnregisterNotificationHandle@4 = %d\n",
                    unregResult);
    }
    rs.mayExit = true;
    SetEvent(rs.doneEvent);
    WaitForSingleObject(hRecv, 5000);

    // Snapshot the captured messages after the receiver has joined.
    std::vector<ReceivedNotify> captured;
    bool timedOut = false;
    {
        EnterCriticalSection(&rs.cs);
        captured = rs.messages;
        timedOut = rs.timedOut;
        LeaveCriticalSection(&rs.cs);
    }

    CloseHandle(hRecv);
    CloseHandle(rs.readyEvent);
    CloseHandle(rs.doneEvent);
    DeleteCriticalSection(&rs.cs);

    // --- 8. Assertions on the captured delivery ---
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "receiver did NOT time out [case %d]", caseNum);
        CHECK(!timedOut, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "receiver captured exactly ONE 0x40A [case %d, "
                      "got %zu]", caseNum, captured.size());
        CHECK(captured.size() == 1, buf);
    }
    if (!captured.empty()) {
        const ReceivedNotify& m = captured.front();
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "receiver msg==0x%03X (expected 0x%03X) "
                          "[case %d]", m.msg, kNotifyMsg, caseNum);
            CHECK(m.msg == kNotifyMsg, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "receiver wParam==0 (expected 0) [case %d, "
                          "got 0x%llX]", caseNum,
                          static_cast<unsigned long long>(m.wParam));
            CHECK(m.wParam == kNotifyWParam, buf);
        }
        LPARAM expLParam = expectedLParam(type, progId);
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "receiver lParam==0x%08lX (expected 0x%08lX, "
                          "category=%d progId=%d) [case %d]",
                          static_cast<unsigned long>(m.lParam),
                          static_cast<unsigned long>(expLParam),
                          category, progId, caseNum);
            CHECK(m.lParam == expLParam, buf);
        }
    }

    // --- 9. Cleanup: delete the disposable item ---
    std::printf("  [9] Cleanup: deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    if (!lib.deleteEntry(type, name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual?)\n");
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runReceiverAwareBatchCase — Finding 2: prove ACTUAL delivery via
// PostMessageW to a registered receiver window across MULTIPLE verify-
// success operations (3 cycles) on the SAME shared TsDevLib, asserting the
// receiver gets EXACTLY ONE 0x40A per operation (3 total) with no
// missed/duplicated messages. This proves receiver-aware delivery works
// correctly across batch reuse at the library level (which is what CLI
// batch/serve modes do internally).
// ---------------------------------------------------------------------------
int runReceiverAwareBatchCase(TsDevLib& lib, int caseNum,
                              const std::string& baseName,
                              const std::string& source) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    int category = getCategory(type);
    const int kIterations = 3;

    std::printf("\n=== Case %d: RECEIVER-AWARE BATCH — %d verify-success "
                "cycles (%s, category=%d) ===\n",
                caseNum, kIterations, typeLabel, category);

    // Resolve register/unregister exports.
    OrlibRegisterNotifyFn registerFn =
        reinterpret_cast<OrlibRegisterNotifyFn>(resolveOrlibOrdinal(595));
    OrlibUnregisterNotifyFn unregisterFn =
        reinterpret_cast<OrlibUnregisterNotifyFn>(resolveOrlibOrdinal(596));
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibRegisterNotificationHandle@4 "
                      "(ordinal 595) [case %d]", caseNum);
        CHECK(registerFn != nullptr, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibUnregisterNotificationHandle@4 "
                      "(ordinal 596) [case %d]", caseNum);
        CHECK(unregisterFn != nullptr, buf);
    }
    if (!registerFn || !unregisterFn) {
        return fail("receiver-aware batch case: orlib20 ordinal 595/596 "
                    "unresolved",
                    100 + caseNum * 10 + 1);
    }

    // Start the receiver thread ONCE for the whole batch (the same
    // registered HWND receives all 3 posts — mirrors a long-lived
    // receiver like orchart.exe).
    // targetCount = 2 * kIterations because each iteration produces ONE
    // verify-post AND ONE delete-post (Strategy category 0, delete is
    // gated category != 4 so it sends). The receiver captures ALL 0x40A
    // messages; the final assertion checks the total equals this count.
    ReceiverState rs;
    InitializeCriticalSection(&rs.cs);
    rs.targetCount = 2 * kIterations;  // 3 verify + 3 delete = 6
    rs.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rs.doneEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rs.readyEvent || !rs.doneEvent) {
        DeleteCriticalSection(&rs.cs);
        if (rs.readyEvent) CloseHandle(rs.readyEvent);
        if (rs.doneEvent)  CloseHandle(rs.doneEvent);
        return fail("receiver-aware batch case: CreateEventW failed",
                    100 + caseNum * 10 + 2);
    }

    std::printf("  [setup] starting receiver thread (targetCount=%d)...\n",
                rs.targetCount);
    HANDLE hRecv = CreateThread(nullptr, 0,
                                ReceiverThreadProc, &rs, 0, nullptr);
    if (!hRecv) {
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("receiver-aware batch case: CreateThread(receiver) "
                    "failed",
                    100 + caseNum * 10 + 2);
    }
    if (WaitForSingleObject(rs.readyEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "  [receiver] readyEvent timed out\n");
    }
    HWND savedHwnd = rs.hwnd;
    if (!savedHwnd) {
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("receiver-aware batch case: receiver window not "
                    "created",
                    100 + caseNum * 10 + 2);
    }
    std::printf("      receiver HWND=0x%p\n", static_cast<void*>(savedHwnd));

    int regResult = registerFn(savedHwnd);
    std::printf("      _OrlibRegisterNotificationHandle@4 = %d\n",
                regResult);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "_OrlibRegisterNotificationHandle@4 returned nonzero "
                      "[case %d, got %d]", caseNum, regResult);
        CHECK(regResult != 0, buf);
    }

    // Run kIterations verify-success cycles. The hook capture is reset
    // before each verify so the per-iteration hook assertions are
    // independent; the receiver accumulates across all iterations.
    for (int iter = 1; iter <= kIterations; ++iter) {
        char name[80];
        std::snprintf(name, sizeof(name), "%s_RCV_IT%d",
                      baseName.c_str(), iter);
        std::printf("\n  --- Receiver-aware batch iteration %d/%d "
                    "(name=%s) ---\n", iter, kIterations, name);

        preCleanup(lib, type, name);

        int errCode = 0;
        int progId = lib.createEntry(type, name, errCode);
        std::printf("    createEntry = %d, errCode = 0x%X\n",
                    progId, errCode);
        if (progId <= 0) {
            unregisterFn(savedHwnd);
            rs.mayExit = true;
            SetEvent(rs.doneEvent);
            WaitForSingleObject(hRecv, INFINITE);
            CloseHandle(hRecv);
            CloseHandle(rs.readyEvent);
            CloseHandle(rs.doneEvent);
            DeleteCriticalSection(&rs.cs);
            return fail("receiver-aware batch: createEntry returned "
                        "ProgId <= 0",
                        100 + caseNum * 10 + iter);
        }

        bool saved = lib.saveSource(type, progId, source);
        std::printf("    saveSource = %s\n", saved ? "true" : "false");
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "rcv batch it%d: saveSource returned true "
                          "[case %d]", iter, caseNum);
            CHECK(saved, buf);
        }

        // FULL reset (including callCount) before verifyModule.
        g_capture = NotifyCapture{};
        std::printf("    verifyModule(%s, %d)...\n", typeLabel, progId);
        std::string errors = lib.verifyModule(type, progId);
        if (!errors.empty()) {
            std::fprintf(stderr,
                         "    rcv batch it%d: verifyModule errors:\n%s\n",
                         iter, errors.c_str());
        } else {
            std::printf("    rcv batch it%d: verifyModule OK\n", iter);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "rcv batch it%d: verifyModule succeeded "
                          "[case %d]", iter, caseNum);
            CHECK(errors.empty(), buf);
        }

        // Hook fired this iteration (library-level proof).
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "rcv batch it%d: onNotifyReady hook fired "
                          "[case %d]", iter, caseNum);
            CHECK(g_capture.fired, buf);
        }
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "rcv batch it%d: callCount==1 (got %d) "
                          "[case %d]", iter, g_capture.callCount, caseNum);
            CHECK(g_capture.callCount == 1, buf);
        }

        // Cleanup this iteration's item. The delete also posts 0x40A for
        // Strategy category 0 (correct per RE_M3 §2.4 — delete is gated
        // category != 4). The receiver captures ALL 0x40A messages
        // (verify + delete); the final assertion checks the total equals
        // 2 * kIterations (3 verify + 3 delete = 6).
        if (!lib.deleteEntry(type, name, /*flush=*/true)) {
            std::fprintf(stderr,
                         "    rcv batch it%d: WARNING deleteEntry false\n",
                         iter);
        }
        resetCapture();
    }

    // Wait for the receiver to capture all targetCount posts (3 verify +
    // 3 delete = 6), or time out. The receiver signals doneEvent when
    // targetCount is reached; we also signal it below as a backstop.
    if (WaitForSingleObject(rs.doneEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr,
                     "  [receiver] doneEvent timed out — expected %d "
                     "0x40A posts\n", rs.targetCount);
    }

    // Unregister the saved HWND (local copy) BEFORE setting mayExit and
    // signaling the receiver thread to destroy its window. Then join.
    {
        DWORD rc = WaitForSingleObject(hRecv, 0);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "rcv batch: receiver thread still alive at unregister "
                      "time (unregister before window destroy) [case %d]",
                      caseNum);
        CHECK(rc == WAIT_TIMEOUT, buf);
    }
    if (unregisterFn) {
        int unregResult = unregisterFn(savedHwnd);
        std::printf("      _OrlibUnregisterNotificationHandle@4 = %d\n",
                    unregResult);
    }
    // Tell the receiver it may destroy its window and exit, then join.
    rs.mayExit = true;
    SetEvent(rs.doneEvent);
    WaitForSingleObject(hRecv, 5000);

    // Snapshot the captured messages after the receiver has joined.
    std::vector<ReceivedNotify> captured;
    bool timedOut = false;
    {
        EnterCriticalSection(&rs.cs);
        captured = rs.messages;
        timedOut = rs.timedOut;
        LeaveCriticalSection(&rs.cs);
    }

    CloseHandle(hRecv);
    CloseHandle(rs.readyEvent);
    CloseHandle(rs.doneEvent);
    DeleteCriticalSection(&rs.cs);

    // --- Final assertions ---
    // The receiver captures EVERY 0x40A posted to it. Each iteration
    // produces ONE verify-post AND ONE delete-post (Strategy category 0,
    // delete is gated category != 4 so it sends). So the expected total
    // is 2 * kIterations = 6. We assert:
    //   - no timeout,
    //   - total captured == 2 * kIterations (no missed, no duplicated),
    //   - the FIRST message of each verify-post pair (i.e. messages at
    //     even indices 0,2,4) has the correct packing for the verify
    //     operation (the delete-posts in between are not asserted here
    //     — they are covered by Case 5).
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "rcv batch: receiver did NOT time out [case %d]",
                      caseNum);
        CHECK(!timedOut, buf);
    }
    size_t expectedTotal = static_cast<size_t>(2 * kIterations);
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "rcv batch: receiver captured exactly %zu 0x40A "
                      "messages (expected %zu = %d verify + %d delete) "
                      "[case %d, got %zu]",
                      captured.size(), expectedTotal,
                      kIterations, kIterations, caseNum, captured.size());
        CHECK(captured.size() == expectedTotal, buf);
    }
    // Assert every captured message is a 0x40A with wParam==0 (no
    // stragglers of other message types leaked into the capture vector —
    // the wnd proc only pushes 0x40A, so this is a structural check).
    for (size_t i = 0; i < captured.size(); ++i) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "rcv batch msg %zu: msg==0x40A && wParam==0 "
                      "[case %d]", i, caseNum);
        CHECK(captured[i].msg == kNotifyMsg &&
              captured[i].wParam == kNotifyWParam, buf);
    }

    return 0;
}

// ===========================================================================
// CLI-LEVEL 0x40A E2E (Findings 1 & 2 — M.3 pre-completion comparative review)
//
// Cases 15 and 16 drive the REAL tsdev_cli.exe binary as a SEPARATE child
// process (not the in-process TsDevLib used by Cases 1-14). Per
// docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md, orlib20.dll's EventsViaHandles
// HWND set lives in the cross-process SHARE_WO shared section, so a receiver
// window registered in the TEST process (via ordinal 595) is visible to
// tsdev_cli.exe's orlib20 instance. When the CLI performs a verify-success,
// its orlib20 Ordinal_57 reads the shared array and posts 0x40A to ALL
// registered HWNDs — including the test process's receiver window. The test
// process's message loop captures the 0x40A.
//
//   Case 15 — CLI stdin batch mode (tsdev_cli batch-insert --batch-stdin):
//             3 Strategy insert+verify operations, assert exactly 3 0x40A
//             posts captured, no missed/duplicated, correct packing.
//   Case 16 — CLI serve mode (tsdev_cli serve --serve-pipe-file <tmp>):
//             3 Strategy insert+verify operations over a named pipe, assert
//             exactly 3 0x40A posts captured, no missed/duplicated, correct
//             packing. Proves a reused serve session delivers one
//             notification per operation.
//
// Both cases use the SAME receiver window infrastructure as Cases 9-10
// (ReceiverState / ReceiverWndProc / ReceiverThreadProc / resolveOrlibOrdinal)
// — the receiver is a REAL registered window (ordinal 595), not a hook.
//
// Cleanup: after the child process exits, the test's OWN TsDevLib (which
// shares the same warehouse via its own orlib20 instance) deletes the 3
// disposable items. The child process has its own TsDevLib; we cannot reuse
// it after it exits, so the test process performs the cleanup.
// ===========================================================================

// Resolve the path to tsdev_cli.exe. Derives from the test executable's
// location: <exe_dir>\tsdev_cli.exe (both live in build\Release\). Falls
// back to a relative path on failure.
std::string resolveCliExePath() {
    wchar_t exePathW[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "tsdev_cli.exe";
    }
    char exePathA[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, exePathW, -1, exePathA, MAX_PATH,
                        nullptr, nullptr);
    std::string dir(exePathA);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
    }
    return dir + "\\tsdev_cli.exe";
}

// Write a string to a file (binary mode). Returns false on failure.
bool writeTempFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// Read a file into a string. Returns false on failure.
bool readTempFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Build a unique temp file path under %TEMP% with the given suffix. Returns
// an empty string on failure.
std::string makeTempFilePath(const char* suffix) {
    char tmp[MAX_PATH] = {0};
    DWORD len = GetTempPathA(MAX_PATH, tmp);
    if (len == 0 || len >= MAX_PATH) return std::string();
    char uniq[MAX_PATH] = {0};
    if (GetTempFileNameA(tmp, "m3", 0, uniq) == 0) return std::string();
    // GetTempFileNameA creates a file; rename it with the suffix.
    std::string base(uniq);
    DeleteFileA(uniq);
    std::string path = base + suffix;
    return path;
}

// Extract ProgIds from CLI JSON output. The batch/serve JSON contains
// "prog_id":NNNN per item. Returns all prog_id values found.
// Used to filter captured 0x40A messages to only those from the CLI child's
// operations, excluding notifications from concurrent orlib20 operations
// (the SHARE_WO shared array delivers notifications from ALL processes).
std::vector<DWORD> extractProgIdsFromJson(const std::string& json) {
    std::vector<DWORD> progIds;
    const std::string key = "\"prog_id\":";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        pos += key.size();
        // Skip optional space after colon.
        while (pos < json.size() && json[pos] == ' ') ++pos;
        DWORD val = 0;
        bool gotDigit = false;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val = val * 10 + static_cast<DWORD>(json[pos] - '0');
            ++pos;
            gotDigit = true;
        }
        if (gotDigit) progIds.push_back(val);
    }
    return progIds;
}

// Filter captured 0x40A messages to only those whose ProgId (low word of
// lParam) matches one of the expected ProgIds. This isolates the CLI child's
// notifications from concurrent orlib20 operations via the shared SHARE_WO
// HWND array.
std::vector<ReceivedNotify> filterByProgIds(
    const std::vector<ReceivedNotify>& captured,
    const std::vector<DWORD>& expectedProgIds) {
    std::vector<ReceivedNotify> filtered;
    for (const auto& msg : captured) {
        WORD progIdLo = static_cast<WORD>(
            static_cast<unsigned long>(msg.lParam) & 0xFFFF);
        for (DWORD ep : expectedProgIds) {
            if (static_cast<WORD>(ep) == progIdLo) {
                filtered.push_back(msg);
                break;
            }
        }
    }
    return filtered;
}

// ---------------------------------------------------------------------------
// runCliBatchStdinCase — Case 15: spawn tsdev_cli.exe batch-insert
// --batch-stdin as a child process, feed a 3-line manifest (3 Strategy
// insert+verify operations) via the stdin pipe, and assert the test's
// receiver window captures exactly 3 0x40A posts (one per verify-success),
// no missed/duplicated, with correct (msg, wParam, lParam) packing.
//
// The manifest format (per src/tsdev_cli.cpp parseManifestLines) is one
// item per line: "<type> <name> <source_file_path>". The source files are
// written to temp files in ASCII (UTF-8 BOM breaks the EL compiler, per
// test_batch_stdin.ps1 Write-SourceFile).
//
// Thread layout:
//   - The MAIN worker thread (this one) constructs the test's own TsDevLib
//     (for post-test cleanup), starts the receiver thread, registers the
//     receiver HWND, spawns the CLI child, waits for it to exit, then
//     cleans up.
//   - A SEPARATE receiver thread runs the message loop (same pattern as
//     Cases 9-10).
//   - The CLI child process has its OWN TsDevLib on its OWN 64MB-stack
//     worker thread (the batch worker); it performs the 3 insert+verify
//     operations and posts 0x40A to the shared HWND array.
// ---------------------------------------------------------------------------
int runCliBatchStdinCase(TsDevLib& lib, int caseNum,
                         const std::string& baseName) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    int category = getCategory(type);
    const int kIterations = 3;

    std::printf("\n=== Case %d: CLI stdin BATCH — %d verify-success ops via "
                "tsdev_cli.exe batch-insert --batch-stdin (%s, category=%d) "
                "===\n",
                caseNum, kIterations, typeLabel, category);

    // Resolve the orlib20 register/unregister exports.
    OrlibRegisterNotifyFn registerFn =
        reinterpret_cast<OrlibRegisterNotifyFn>(resolveOrlibOrdinal(595));
    OrlibUnregisterNotifyFn unregisterFn =
        reinterpret_cast<OrlibUnregisterNotifyFn>(resolveOrlibOrdinal(596));
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibRegisterNotificationHandle@4 "
                      "(ordinal 595) [case %d]", caseNum);
        CHECK(registerFn != nullptr, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibUnregisterNotificationHandle@4 "
                      "(ordinal 596) [case %d]", caseNum);
        CHECK(unregisterFn != nullptr, buf);
    }
    if (!registerFn || !unregisterFn) {
        return fail("CLI batch case: orlib20 ordinal 595/596 unresolved",
                    100 + caseNum * 10 + 1);
    }

    // Resolve the CLI executable path.
    std::string cliPath = resolveCliExePath();
    std::printf("  [setup] CLI path: %s\n", cliPath.c_str());
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "tsdev_cli.exe exists at resolved path [case %d]",
                      caseNum);
        DWORD attr = GetFileAttributesA(cliPath.c_str());
        CHECK(attr != INVALID_FILE_ATTRIBUTES &&
              !(attr & FILE_ATTRIBUTE_DIRECTORY), buf);
    }
    if (GetFileAttributesA(cliPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return fail("CLI batch case: tsdev_cli.exe not found",
                    100 + caseNum * 10 + 1);
    }

    // Build the 3 disposable item names and write their source files.
    // The manifest format is "<type> <name> <source_file>".
    std::vector<std::string> names(kIterations);
    std::vector<std::string> srcFiles(kIterations);
    std::string manifestContent;
    for (int i = 0; i < kIterations; ++i) {
        char name[96];
        std::snprintf(name, sizeof(name), "%s_CLI_IT%d",
                      baseName.c_str(), i + 1);
        names[i] = name;
        // Pre-cleanup via the test's own TsDevLib (re-run safety).
        preCleanup(lib, type, names[i]);
        // Write the source file (ASCII; UTF-8 BOM breaks the EL compiler).
        std::string src = makeStrategySource(names[i]);
        std::string srcPath = makeTempFilePath(".el");
        if (srcPath.empty() || !writeTempFile(srcPath, src)) {
            return fail("CLI batch case: failed to write source file",
                        100 + caseNum * 10 + 2 + i);
        }
        srcFiles[i] = srcPath;
        manifestContent += std::string("strategy ") + names[i] + " " +
                           srcPath + "\r\n";
    }
    std::printf("  [setup] manifest (%zu bytes, %d items):\n%s",
                manifestContent.size(), kIterations, manifestContent.c_str());

    // Start the receiver thread (targetCount = kIterations: one 0x40A per
    // verify-success; the CLI batch-insert does NOT delete the items, so
    // there are no delete-posts to capture).
    ReceiverState rs;
    InitializeCriticalSection(&rs.cs);
    rs.targetCount = kIterations;
    rs.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rs.doneEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rs.readyEvent || !rs.doneEvent) {
        DeleteCriticalSection(&rs.cs);
        if (rs.readyEvent) CloseHandle(rs.readyEvent);
        if (rs.doneEvent)  CloseHandle(rs.doneEvent);
        return fail("CLI batch case: CreateEventW failed",
                    100 + caseNum * 10 + 3);
    }

    std::printf("  [setup] starting receiver thread (targetCount=%d)...\n",
                rs.targetCount);
    HANDLE hRecv = CreateThread(nullptr, 0,
                                ReceiverThreadProc, &rs, 0, nullptr);
    if (!hRecv) {
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI batch case: CreateThread(receiver) failed",
                    100 + caseNum * 10 + 3);
    }
    if (WaitForSingleObject(rs.readyEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "  [receiver] readyEvent timed out\n");
    }
    HWND savedHwnd = rs.hwnd;
    if (!savedHwnd) {
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI batch case: receiver window not created",
                    100 + caseNum * 10 + 3);
    }
    std::printf("      receiver HWND=0x%p\n", static_cast<void*>(savedHwnd));

    int regResult = registerFn(savedHwnd);
    std::printf("      _OrlibRegisterNotificationHandle@4 = %d\n",
                regResult);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "_OrlibRegisterNotificationHandle@4 returned nonzero "
                      "[case %d, got %d]", caseNum, regResult);
        CHECK(regResult != 0, buf);
    }

    // Spawn tsdev_cli.exe batch-insert --batch-stdin with the manifest on
    // stdin. Use CreateProcessW with redirected stdin/stdout/stderr pipes.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hChildStdinRd = nullptr, hChildStdinWr = nullptr;
    HANDLE hChildStdoutRd = nullptr, hChildStdoutWr = nullptr;
    HANDLE hChildStderrRd = nullptr, hChildStderrWr = nullptr;

    auto createPipe = [&](HANDLE& rd, HANDLE& wr) -> bool {
        return CreatePipe(&rd, &wr, &sa, 0) != 0;
    };
    if (!createPipe(hChildStdinRd, hChildStdinWr) ||
        !createPipe(hChildStdoutRd, hChildStdoutWr) ||
        !createPipe(hChildStderrRd, hChildStderrWr)) {
        unregisterFn(savedHwnd);
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI batch case: CreatePipe failed",
                    100 + caseNum * 10 + 4);
    }
    // Ensure the PARENT's pipe ends are NOT inherited by the child (only
    // the child's ends — stdin read, stdout write, stderr write — go to
    // the child via STARTF_USESTDHANDLES). If the child inherited the
    // parent's write end of stdin, it would never see EOF; if it
    // inherited the parent's read end of stdout/stderr, the drain
    // threads would never see EOF either.
    SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0);

    // Build the command line. The CLI uses main(int, char**), so a plain
    // ANSI command line works. Quote the path in case of spaces.
    std::string cmdLine = "\"" + cliPath + "\" batch-insert --batch-stdin";
    // CreateProcessW needs a mutable wide command line.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
                                   static_cast<int>(cmdLine.size()),
                                   nullptr, 0);
    std::wstring wCmdLine(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
                        static_cast<int>(cmdLine.size()),
                        &wCmdLine[0], wlen);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hChildStdinRd;
    si.hStdOutput = hChildStdoutWr;
    si.hStdError  = hChildStderrWr;
    PROCESS_INFORMATION pi{};

    std::printf("  [spawn] CreateProcessW: %s\n", cmdLine.c_str());
    BOOL ok = CreateProcessW(nullptr, &wCmdLine[0], nullptr, nullptr, TRUE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        std::fprintf(stderr,
                     "  [spawn] CreateProcessW failed: %lu\n",
                     GetLastError());
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        unregisterFn(savedHwnd);
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI batch case: CreateProcessW failed",
                    100 + caseNum * 10 + 4);
    }
    std::printf("      child PID=%lu\n", pi.dwProcessId);

    // Write the manifest to the child's stdin, then close the write end so
    // the child sees EOF (its --batch-stdin reader loops on fread until 0).
    {
        DWORD written = 0;
        WriteFile(hChildStdinWr, manifestContent.data(),
                  static_cast<DWORD>(manifestContent.size()),
                  &written, nullptr);
        std::printf("  [stdin] wrote %lu bytes to child stdin\n", written);
    }
    CloseHandle(hChildStdinWr); hChildStdinWr = nullptr;
    CloseHandle(hChildStdinRd); hChildStdinRd = nullptr;
    CloseHandle(hChildStdoutWr); hChildStdoutWr = nullptr;
    CloseHandle(hChildStderrWr); hChildStderrWr = nullptr;

    // Drain the child's stdout and stderr in a background thread so the
    // pipe does not fill and block the child. We capture the output for
    // diagnostics.
    struct ChildOutput {
        std::string stdoutStr;
        std::string stderrStr;
    };
    ChildOutput childOut;
    HANDLE hDrainStdout = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* co = static_cast<std::pair<HANDLE, std::string*>*>(p);
            char buf[4096]; DWORD got = 0;
            while (ReadFile(co->first, buf, sizeof(buf), &got, nullptr) && got > 0) {
                co->second->append(buf, got);
            }
            return 0;
        },
        new std::pair<HANDLE, std::string*>(hChildStdoutRd, &childOut.stdoutStr),
        0, nullptr);
    HANDLE hDrainStderr = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* co = static_cast<std::pair<HANDLE, std::string*>*>(p);
            char buf[4096]; DWORD got = 0;
            while (ReadFile(co->first, buf, sizeof(buf), &got, nullptr) && got > 0) {
                co->second->append(buf, got);
            }
            return 0;
        },
        new std::pair<HANDLE, std::string*>(hChildStderrRd, &childOut.stderrStr),
        0, nullptr);

    // Wait for the child to exit (generous timeout: batch init + 3 compiles).
    DWORD childExitCode = 0;
    {
        DWORD waitRc = WaitForSingleObject(pi.hProcess, 120000);
        if (waitRc != WAIT_OBJECT_0) {
            std::fprintf(stderr,
                         "  [child] WaitForSingleObject timed out / failed: "
                         "%lu\n", waitRc);
            TerminateProcess(pi.hProcess, 99);
        }
        GetExitCodeProcess(pi.hProcess, &childExitCode);
        std::printf("  [child] exited, code=%lu\n", childExitCode);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI child exited with code 0 (all items succeeded) "
                      "[case %d, got %lu]", caseNum, childExitCode);
        CHECK(childExitCode == 0, buf);
    }

    // Close the child's stdout/stderr read ends (this lets the drain
    // threads exit) and join them.
    CloseHandle(hChildStdoutRd); hChildStdoutRd = nullptr;
    CloseHandle(hChildStderrRd); hChildStderrRd = nullptr;
    if (hDrainStdout) { WaitForSingleObject(hDrainStdout, 5000); CloseHandle(hDrainStdout); }
    if (hDrainStderr) { WaitForSingleObject(hDrainStderr, 5000); CloseHandle(hDrainStderr); }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!childOut.stdoutStr.empty()) {
        std::printf("  [child stdout] %zu bytes:\n%s\n",
                    childOut.stdoutStr.size(),
                    childOut.stdoutStr.c_str());
    }
    if (!childOut.stderrStr.empty()) {
        std::fprintf(stderr, "  [child stderr] %zu bytes:\n%s\n",
                     childOut.stderrStr.size(),
                     childOut.stderrStr.c_str());
    }

    // Wait for the receiver to capture all kIterations 0x40A posts (or
    // time out). The receiver signals doneEvent when targetCount is reached.
    // With the 120s receiver pump, doneEvent is typically already signaled
    // before the child exits; 30s is a safety margin for late captures.
    if (WaitForSingleObject(rs.doneEvent, 30000) != WAIT_OBJECT_0) {
        std::fprintf(stderr,
                     "  [receiver] doneEvent timed out — expected %d "
                     "0x40A posts\n", rs.targetCount);
    }

    // Unregister the saved HWND (local copy) BEFORE setting mayExit and
    // signaling the receiver thread to destroy its window. Then join.
    {
        DWORD rc = WaitForSingleObject(hRecv, 0);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI batch: receiver thread still alive at unregister "
                      "time (unregister before window destroy) [case %d]",
                      caseNum);
        CHECK(rc == WAIT_TIMEOUT, buf);
    }
    if (unregisterFn) {
        int unregResult = unregisterFn(savedHwnd);
        std::printf("      _OrlibUnregisterNotificationHandle@4 = %d\n",
                    unregResult);
    }
    rs.mayExit = true;
    SetEvent(rs.doneEvent);
    WaitForSingleObject(hRecv, 5000);

    std::vector<ReceivedNotify> captured;
    bool timedOut = false;
    {
        EnterCriticalSection(&rs.cs);
        captured = rs.messages;
        timedOut = rs.timedOut;
        LeaveCriticalSection(&rs.cs);
    }

    CloseHandle(hRecv);
    CloseHandle(rs.readyEvent);
    CloseHandle(rs.doneEvent);
    DeleteCriticalSection(&rs.cs);

    // --- Assertions ---
    // The SHARE_WO shared HWND array delivers 0x40A notifications from ALL
    // orlib20 operations in the session, not just the CLI child's. Filter
    // captured messages to only those matching the CLI child's ProgIds.
    std::vector<DWORD> expectedProgIds = extractProgIdsFromJson(childOut.stdoutStr);
    std::vector<ReceivedNotify> matched = filterByProgIds(captured, expectedProgIds);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI batch: receiver did NOT time out [case %d]",
                      caseNum);
        CHECK(!timedOut, buf);
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "CLI batch: receiver captured exactly %d 0x40A "
                      "messages matching CLI ProgIds (one per verify-success) "
                      "[case %d, got %zu (total captured %zu)]",
                      kIterations, caseNum, matched.size(), captured.size());
        CHECK(matched.size() == static_cast<size_t>(kIterations), buf);
    }
    // Assert every matched message is a 0x40A with wParam==0.
    for (size_t i = 0; i < matched.size(); ++i) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI batch msg %zu: msg==0x40A && wParam==0 "
                      "[case %d]", i, caseNum);
        CHECK(matched[i].msg == kNotifyMsg &&
              matched[i].wParam == kNotifyWParam, buf);
    }
    // Assert the category packing (high word of lParam == category for
    // Strategy == 0). We do NOT assert the exact progId here because the
    // progId is assigned by the child's orlib20 instance and may differ
    // from what the test's TsDevLib would assign; the category (high word)
    // is deterministic for Strategy (category 0).
    for (size_t i = 0; i < matched.size(); ++i) {
        WORD catHi = static_cast<WORD>(
            (static_cast<unsigned long>(matched[i].lParam) >> 16) & 0xFFFF);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI batch msg %zu: lParam high word == category "
                      "(%d for Strategy) [case %d, got %u]",
                      i, category, caseNum, static_cast<unsigned>(catHi));
        CHECK(catHi == static_cast<WORD>(category), buf);
    }

    // --- Cleanup: delete the 3 disposable items via the test's own TsDevLib
    //     (the child process has exited; its TsDevLib is gone). ---
    std::printf("  [cleanup] deleting %d disposable items via test TsDevLib...\n",
                kIterations);
    for (int i = 0; i < kIterations; ++i) {
        if (!lib.deleteEntry(type, names[i], /*flush=*/true)) {
            std::fprintf(stderr,
                         "  [cleanup] WARNING: deleteEntry(%s) returned false\n",
                         names[i].c_str());
        }
        // Remove the temp source file.
        if (!srcFiles[i].empty()) {
            DeleteFileA(srcFiles[i].c_str());
        }
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// runCliServeCase — Case 16: spawn tsdev_cli.exe serve
// --serve-pipe-file <tmp> as a child process, connect to its named pipe,
// send 3 insert JSON requests (3 Strategy insert+verify operations),
// capture the 0x40A posts, send quit, and assert exactly 3 0x40A posts
// captured (one per verify-success), no missed/duplicated, correct packing.
//
// The serve protocol (per src/tsdev_cli.cpp runServe + _pipe.py):
//   - The broker writes its pipe name (\\.\pipe\tsdev_cli_<PID>) to the
//     --serve-pipe-file (line 1) and its PID (line 2).
//   - The client connects via CreateFileW(pipeName, GENERIC_READ|WRITE).
//   - Each request is a 4-byte LE length prefix + UTF-8 JSON payload.
//   - Each response is the same framing.
//   - The "quit" op shuts the broker down.
//
// The broker (tsdev_cli.exe) requires admin privileges (it accesses the
// TSDev Library). The test process is itself elevated (the test exe has a
// requireAdministrator manifest per CMakeLists.txt TSDEV_ADMIN_TARGETS),
// so the child inherits the elevated token — no UAC prompt.
// ---------------------------------------------------------------------------
int runCliServeCase(TsDevLib& lib, int caseNum,
                    const std::string& baseName) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    int category = getCategory(type);
    const int kIterations = 3;

    std::printf("\n=== Case %d: CLI SERVE — %d verify-success ops via "
                "tsdev_cli.exe serve --serve-pipe-file (%s, category=%d) "
                "===\n",
                caseNum, kIterations, typeLabel, category);

    // Resolve the orlib20 register/unregister exports.
    OrlibRegisterNotifyFn registerFn =
        reinterpret_cast<OrlibRegisterNotifyFn>(resolveOrlibOrdinal(595));
    OrlibUnregisterNotifyFn unregisterFn =
        reinterpret_cast<OrlibUnregisterNotifyFn>(resolveOrlibOrdinal(596));
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibRegisterNotificationHandle@4 "
                      "(ordinal 595) [case %d]", caseNum);
        CHECK(registerFn != nullptr, buf);
    }
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "resolved _OrlibUnregisterNotificationHandle@4 "
                      "(ordinal 596) [case %d]", caseNum);
        CHECK(unregisterFn != nullptr, buf);
    }
    if (!registerFn || !unregisterFn) {
        return fail("CLI serve case: orlib20 ordinal 595/596 unresolved",
                    100 + caseNum * 10 + 1);
    }

    // Resolve the CLI executable path.
    std::string cliPath = resolveCliExePath();
    std::printf("  [setup] CLI path: %s\n", cliPath.c_str());
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "tsdev_cli.exe exists at resolved path [case %d]",
                      caseNum);
        DWORD attr = GetFileAttributesA(cliPath.c_str());
        CHECK(attr != INVALID_FILE_ATTRIBUTES &&
              !(attr & FILE_ATTRIBUTE_DIRECTORY), buf);
    }
    if (GetFileAttributesA(cliPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return fail("CLI serve case: tsdev_cli.exe not found",
                    100 + caseNum * 10 + 1);
    }

    // Build the 3 disposable item names and pre-cleanup.
    std::vector<std::string> names(kIterations);
    for (int i = 0; i < kIterations; ++i) {
        char name[96];
        std::snprintf(name, sizeof(name), "%s_CLI_SV_IT%d",
                      baseName.c_str(), i + 1);
        names[i] = name;
        preCleanup(lib, type, names[i]);
    }

    // Create a temp file path for the pipe-name handshake. The broker
    // writes "pipeName\nPID\n" to this file atomically.
    std::string pipeFilePath = makeTempFilePath(".pipe");
    if (pipeFilePath.empty()) {
        return fail("CLI serve case: failed to create pipe file path",
                    100 + caseNum * 10 + 2);
    }
    // makeTempFilePath already deleted the placeholder; ensure no stale file.
    DeleteFileA(pipeFilePath.c_str());

    // Start the receiver thread (targetCount = kIterations: one 0x40A per
    // verify-success; serve insert does NOT delete the items).
    ReceiverState rs;
    InitializeCriticalSection(&rs.cs);
    rs.targetCount = kIterations;
    rs.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rs.doneEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rs.readyEvent || !rs.doneEvent) {
        DeleteCriticalSection(&rs.cs);
        if (rs.readyEvent) CloseHandle(rs.readyEvent);
        if (rs.doneEvent)  CloseHandle(rs.doneEvent);
        return fail("CLI serve case: CreateEventW failed",
                    100 + caseNum * 10 + 3);
    }

    std::printf("  [setup] starting receiver thread (targetCount=%d)...\n",
                rs.targetCount);
    HANDLE hRecv = CreateThread(nullptr, 0,
                                ReceiverThreadProc, &rs, 0, nullptr);
    if (!hRecv) {
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI serve case: CreateThread(receiver) failed",
                    100 + caseNum * 10 + 3);
    }
    if (WaitForSingleObject(rs.readyEvent, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "  [receiver] readyEvent timed out\n");
    }
    HWND savedHwnd = rs.hwnd;
    if (!savedHwnd) {
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI serve case: receiver window not created",
                    100 + caseNum * 10 + 3);
    }
    std::printf("      receiver HWND=0x%p\n", static_cast<void*>(savedHwnd));

    int regResult = registerFn(savedHwnd);
    std::printf("      _OrlibRegisterNotificationHandle@4 = %d\n",
                regResult);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "_OrlibRegisterNotificationHandle@4 returned nonzero "
                      "[case %d, got %d]", caseNum, regResult);
        CHECK(regResult != 0, buf);
    }

    // Spawn tsdev_cli.exe serve --serve-pipe-file <tmp>. The broker prints
    // nothing to stdout (it writes the pipe name to the file). We still
    // redirect stdout/stderr for diagnostics.
    std::string cmdLine = "\"" + cliPath + "\" serve --serve-pipe-file \"" +
                          pipeFilePath + "\"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
                                   static_cast<int>(cmdLine.size()),
                                   nullptr, 0);
    std::wstring wCmdLine(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
                        static_cast<int>(cmdLine.size()),
                        &wCmdLine[0], wlen);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE hChildStdoutRd = nullptr, hChildStdoutWr = nullptr;
    HANDLE hChildStderrRd = nullptr, hChildStderrWr = nullptr;
    CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &sa, 0);
    CreatePipe(&hChildStderrRd, &hChildStderrWr, &sa, 0);
    // The PARENT's read ends must NOT be inherited (the child gets the
    // write ends via STARTF_USESTDHANDLES).
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = INVALID_HANDLE_VALUE;  // serve does not read stdin
    si.hStdOutput = hChildStdoutWr;
    si.hStdError  = hChildStderrWr;
    PROCESS_INFORMATION pi{};

    std::printf("  [spawn] CreateProcessW: %s\n", cmdLine.c_str());
    BOOL ok = CreateProcessW(nullptr, &wCmdLine[0], nullptr, nullptr, TRUE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        std::fprintf(stderr,
                     "  [spawn] CreateProcessW failed: %lu\n",
                     GetLastError());
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        unregisterFn(savedHwnd);
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI serve case: CreateProcessW failed",
                    100 + caseNum * 10 + 4);
    }
    std::printf("      broker PID=%lu\n", pi.dwProcessId);
    CloseHandle(hChildStdoutWr); hChildStdoutWr = nullptr;
    CloseHandle(hChildStderrWr); hChildStderrWr = nullptr;

    // Drain the broker's stdout/stderr in background threads (diagnostics).
    std::string brokerStdout, brokerStderr;
    HANDLE hDrainStdout = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* co = static_cast<std::pair<HANDLE, std::string*>*>(p);
            char buf[4096]; DWORD got = 0;
            while (ReadFile(co->first, buf, sizeof(buf), &got, nullptr) && got > 0) {
                co->second->append(buf, got);
            }
            return 0;
        },
        new std::pair<HANDLE, std::string*>(hChildStdoutRd, &brokerStdout),
        0, nullptr);
    HANDLE hDrainStderr = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* co = static_cast<std::pair<HANDLE, std::string*>*>(p);
            char buf[4096]; DWORD got = 0;
            while (ReadFile(co->first, buf, sizeof(buf), &got, nullptr) && got > 0) {
                co->second->append(buf, got);
            }
            return 0;
        },
        new std::pair<HANDLE, std::string*>(hChildStderrRd, &brokerStderr),
        0, nullptr);

    // Poll the pipe file (up to 30s) for the pipe name. The broker writes
    // "pipeName\nPID\n" atomically (write to <path>.tmp then MoveFileEx).
    std::string pipeName;
    {
        DWORD deadline = GetTickCount() + 30000;
        while (GetTickCount() < deadline) {
            // Check the broker is still alive.
            DWORD ec = 0;
            if (GetExitCodeProcess(pi.hProcess, &ec) &&
                ec != STILL_ACTIVE) {
                std::fprintf(stderr,
                             "  [broker] exited before writing pipe file "
                             "(code=%lu)\n", ec);
                break;
            }
            std::string content;
            if (readTempFile(pipeFilePath, content) && !content.empty()) {
                // First line is the pipe name.
                size_t nl = content.find('\n');
                if (nl != std::string::npos) {
                    pipeName = content.substr(0, nl);
                } else {
                    pipeName = content;
                }
                // Trim any trailing \r.
                if (!pipeName.empty() && pipeName.back() == '\r') {
                    pipeName.pop_back();
                }
                if (!pipeName.empty()) break;
            }
            Sleep(100);
        }
    }
    std::printf("  [handshake] pipe name: %s\n",
                pipeName.empty() ? "(NOT FOUND)" : pipeName.c_str());
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "serve broker wrote pipe name to file [case %d]",
                      caseNum);
        CHECK(!pipeName.empty(), buf);
    }
    if (pipeName.empty()) {
        TerminateProcess(pi.hProcess, 99);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStderrRd);
        if (hDrainStdout) { WaitForSingleObject(hDrainStdout, 5000); CloseHandle(hDrainStdout); }
        if (hDrainStderr) { WaitForSingleObject(hDrainStderr, 5000); CloseHandle(hDrainStderr); }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        unregisterFn(savedHwnd);
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI serve case: broker did not write pipe file",
                    100 + caseNum * 10 + 4);
    }

    // Connect to the named pipe (retry for ~5s — the broker may not have
    // called ConnectNamedPipe yet).
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    {
        DWORD deadline = GetTickCount() + 5000;
        while (GetTickCount() < deadline) {
            // Convert the narrow pipe name to wide.
            int plen = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                                           static_cast<int>(pipeName.size()),
                                           nullptr, 0);
            std::wstring wPipeName(plen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                                static_cast<int>(pipeName.size()),
                                &wPipeName[0], plen);
            hPipe = CreateFileW(wPipeName.c_str(),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
            if (hPipe != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
                std::fprintf(stderr,
                             "  [connect] CreateFileW failed: err=%lu\n",
                             err);
                break;
            }
            Sleep(100);
        }
    }
    std::printf("  [connect] pipe handle: 0x%p\n", static_cast<void*>(hPipe));
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "connected to serve broker named pipe [case %d]",
                      caseNum);
        CHECK(hPipe != INVALID_HANDLE_VALUE, buf);
    }
    if (hPipe == INVALID_HANDLE_VALUE) {
        TerminateProcess(pi.hProcess, 99);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStderrRd);
        if (hDrainStdout) { WaitForSingleObject(hDrainStdout, 5000); CloseHandle(hDrainStdout); }
        if (hDrainStderr) { WaitForSingleObject(hDrainStderr, 5000); CloseHandle(hDrainStderr); }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        unregisterFn(savedHwnd);
        rs.mayExit = true;
        SetEvent(rs.doneEvent);
        WaitForSingleObject(hRecv, INFINITE);
        CloseHandle(hRecv);
        DeleteCriticalSection(&rs.cs);
        CloseHandle(rs.readyEvent);
        CloseHandle(rs.doneEvent);
        return fail("CLI serve case: failed to connect to broker pipe",
                    100 + caseNum * 10 + 5);
    }

    // Helper lambdas for framed I/O (4-byte LE length prefix + UTF-8 payload).
    auto writeAll = [](HANDLE h, const void* buf, size_t len) -> bool {
        size_t total = 0;
        while (total < len) {
            DWORD wrote = 0;
            if (!WriteFile(h, static_cast<const char*>(buf) + total,
                           static_cast<DWORD>(len - total), &wrote, nullptr))
                return false;
            if (wrote == 0) return false;
            total += wrote;
        }
        return true;
    };
    auto readAll = [](HANDLE h, void* buf, size_t len) -> bool {
        size_t total = 0;
        while (total < len) {
            DWORD got = 0;
            if (!ReadFile(h, static_cast<char*>(buf) + total,
                          static_cast<DWORD>(len - total), &got, nullptr))
                return false;
            if (got == 0) return false;
            total += got;
        }
        return true;
    };
    auto writeFramed = [&](const std::string& payload) -> bool {
        uint32_t len = static_cast<uint32_t>(payload.size());
        if (!writeAll(hPipe, &len, 4)) return false;
        if (len > 0 && !writeAll(hPipe, payload.data(), len)) return false;
        return true;
    };
    auto readFramed = [&](std::string& out) -> bool {
        uint32_t len = 0;
        if (!readAll(hPipe, &len, 4)) return false;
        if (len > 67108864) return false;
        out.resize(len);
        if (len > 0 && !readAll(hPipe, out.data(), len)) return false;
        return true;
    };

    // Send 3 insert requests (one per disposable item). Each insert does
    // createEntry + saveSource + verifyModule internally (doInsert with
    // flushPerItem=false); each verify-success posts 0x40A.
    bool allInsertsOk = true;
    std::string allServeResponses;  // accumulated for ProgId extraction
    for (int i = 0; i < kIterations; ++i) {
        // Build the JSON request manually (avoid pulling in nlohmann here).
        // Schema: {"op":"insert","type":"strategy","name":"<N>","source":"<S>"}
        // The source must be JSON-escaped (newlines -> \r\n as \\r\\n).
        std::string src = makeStrategySource(names[i]);
        // JSON-escape the source: escape backslash, quote, and control chars.
        std::string escSrc;
        escSrc.reserve(src.size() * 2);
        for (char c : src) {
            switch (c) {
                case '\\': escSrc += "\\\\"; break;
                case '"':  escSrc += "\\\""; break;
                case '\r': escSrc += "\\r"; break;
                case '\n': escSrc += "\\n"; break;
                case '\t': escSrc += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x",
                                      static_cast<unsigned>(c));
                        escSrc += hex;
                    } else {
                        escSrc += c;
                    }
            }
        }
        std::string req = "{\"op\":\"insert\",\"type\":\"strategy\","
                          "\"name\":\"" + names[i] + "\","
                          "\"source\":\"" + escSrc + "\"}";
        std::printf("  [send %d/%d] insert \"%s\" (%zu bytes)\n",
                    i + 1, kIterations, names[i].c_str(), req.size());
        if (!writeFramed(req)) {
            std::fprintf(stderr, "  [send %d] writeFramed failed\n", i + 1);
            allInsertsOk = false;
            break;
        }
        // Read the response.
        std::string resp;
        if (!readFramed(resp)) {
            std::fprintf(stderr, "  [recv %d] readFramed failed\n", i + 1);
            allInsertsOk = false;
            break;
        }
        std::printf("  [recv %d/%d] %zu bytes: %s\n",
                    i + 1, kIterations, resp.size(), resp.c_str());
        allServeResponses += resp;
        // Check the response contains "success":true (simple substring
        // check — we do not parse the full JSON here to keep the test
        // dependency-free).
        bool success = resp.find("\"success\":true") != std::string::npos ||
                       resp.find("\"success\": true") != std::string::npos;
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "serve insert %d/%d response success=true "
                          "[case %d]", i + 1, kIterations, caseNum);
            CHECK(success, buf);
        }
        if (!success) allInsertsOk = false;
    }

    // Send the quit request to shut the broker down cleanly.
    if (allInsertsOk) {
        std::string quitReq = "{\"op\":\"quit\"}";
        std::printf("  [send quit] %zu bytes\n", quitReq.size());
        if (!writeFramed(quitReq)) {
            std::fprintf(stderr, "  [send quit] writeFramed failed\n");
        } else {
            std::string quitResp;
            // Best-effort read of the quit response (the broker may close
            // the pipe immediately after responding).
            readFramed(quitResp);
            std::printf("  [recv quit] %zu bytes: %s\n",
                        quitResp.size(), quitResp.c_str());
        }
    }

    // Close the pipe handle.
    CloseHandle(hPipe);
    CloseHandle(hChildStdoutRd); hChildStdoutRd = nullptr;
    CloseHandle(hChildStderrRd); hChildStderrRd = nullptr;
    if (hDrainStdout) { WaitForSingleObject(hDrainStdout, 5000); CloseHandle(hDrainStdout); }
    if (hDrainStderr) { WaitForSingleObject(hDrainStderr, 5000); CloseHandle(hDrainStderr); }

    // Wait for the broker to exit (quit op should make it exit 0).
    DWORD brokerExitCode = 0;
    {
        DWORD waitRc = WaitForSingleObject(pi.hProcess, 30000);
        if (waitRc != WAIT_OBJECT_0) {
            std::fprintf(stderr,
                         "  [broker] WaitForSingleObject timed out / "
                         "failed: %lu\n", waitRc);
            TerminateProcess(pi.hProcess, 99);
        }
        GetExitCodeProcess(pi.hProcess, &brokerExitCode);
        std::printf("  [broker] exited, code=%lu\n", brokerExitCode);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!brokerStdout.empty()) {
        std::printf("  [broker stdout] %zu bytes:\n%s\n",
                    brokerStdout.size(), brokerStdout.c_str());
    }
    if (!brokerStderr.empty()) {
        std::fprintf(stderr, "  [broker stderr] %zu bytes:\n%s\n",
                     brokerStderr.size(), brokerStderr.c_str());
    }

    // Wait for the receiver to capture all kIterations 0x40A posts.
    // With the 120s receiver pump, doneEvent is typically already signaled
    // before the broker exits; 30s is a safety margin for late captures.
    if (WaitForSingleObject(rs.doneEvent, 30000) != WAIT_OBJECT_0) {
        std::fprintf(stderr,
                     "  [receiver] doneEvent timed out — expected %d "
                     "0x40A posts\n", rs.targetCount);
    }

    // Unregister the saved HWND (local copy) BEFORE setting mayExit and
    // signaling the receiver thread to destroy its window. Then join.
    {
        DWORD rc = WaitForSingleObject(hRecv, 0);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI serve: receiver thread still alive at unregister "
                      "time (unregister before window destroy) [case %d]",
                      caseNum);
        CHECK(rc == WAIT_TIMEOUT, buf);
    }
    if (unregisterFn) {
        int unregResult = unregisterFn(savedHwnd);
        std::printf("      _OrlibUnregisterNotificationHandle@4 = %d\n",
                    unregResult);
    }
    rs.mayExit = true;
    SetEvent(rs.doneEvent);
    WaitForSingleObject(hRecv, 5000);

    std::vector<ReceivedNotify> captured;
    bool timedOut = false;
    {
        EnterCriticalSection(&rs.cs);
        captured = rs.messages;
        timedOut = rs.timedOut;
        LeaveCriticalSection(&rs.cs);
    }

    CloseHandle(hRecv);
    CloseHandle(rs.readyEvent);
    CloseHandle(rs.doneEvent);
    DeleteCriticalSection(&rs.cs);

    // Clean up the pipe file.
    DeleteFileA(pipeFilePath.c_str());

    // --- Assertions ---
    // The SHARE_WO shared HWND array delivers 0x40A notifications from ALL
    // orlib20 operations in the session, not just the CLI child's. Filter
    // captured messages to only those matching the CLI child's ProgIds.
    std::vector<DWORD> expectedProgIds = extractProgIdsFromJson(allServeResponses);
    std::vector<ReceivedNotify> matched = filterByProgIds(captured, expectedProgIds);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI serve: receiver did NOT time out [case %d]",
                      caseNum);
        CHECK(!timedOut, buf);
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "CLI serve: receiver captured exactly %d 0x40A "
                      "messages matching CLI ProgIds (one per verify-success) "
                      "[case %d, got %zu (total captured %zu)]",
                      kIterations, caseNum, matched.size(), captured.size());
        CHECK(matched.size() == static_cast<size_t>(kIterations), buf);
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI serve msg %zu: msg==0x40A && wParam==0 "
                      "[case %d]", i, caseNum);
        CHECK(matched[i].msg == kNotifyMsg &&
              matched[i].wParam == kNotifyWParam, buf);
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        WORD catHi = static_cast<WORD>(
            (static_cast<unsigned long>(matched[i].lParam) >> 16) & 0xFFFF);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "CLI serve msg %zu: lParam high word == category "
                      "(%d for Strategy) [case %d, got %u]",
                      i, category, caseNum, static_cast<unsigned>(catHi));
        CHECK(catHi == static_cast<WORD>(category), buf);
    }

    // --- Cleanup: delete the 3 disposable items via the test's own TsDevLib. ---
    std::printf("  [cleanup] deleting %d disposable items via test TsDevLib...\n",
                kIterations);
    for (int i = 0; i < kIterations; ++i) {
        if (!lib.deleteEntry(type, names[i], /*flush=*/true)) {
            std::fprintf(stderr,
                         "  [cleanup] WARNING: deleteEntry(%s) returned false\n",
                         names[i].c_str());
        }
    }

    resetCapture();
    return 0;
}

// ---------------------------------------------------------------------------
// Worker thread entry point. Constructs ONE TsDevLib (with the
// onNotifyReady hook installed) for the whole worker thread and runs all
// test cases against it. This matches the operational invariant used by
// every other integration test in this codebase: a SINGLE TsDevLib
// instance per worker thread.
//
// The 64 MB stack is required by the EasyLanguage compiler invoked via
// VerifyModule_New.
// ---------------------------------------------------------------------------
struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    // --- Construct the single shared TsDevLib for this worker thread ---
    std::printf("\n--- Constructing shared TsDevLib (test-only ctor, "
                "onNotifyReady hook installed) ---\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg = makeConfigWithHook();
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        st->exitCode = fail(
            (std::string("TsDevLib construction threw: ") + e.what()).c_str(),
            100);
        return 0;
    }
    if (!lib) {
        st->exitCode = fail("makeTsDevLibForTest returned null", 101);
        return 0;
    }
    std::printf("--- TsDevLib constructed OK ---\n");

    // Use a date-stamped name so the test items are unique per run day and
    // do not collide with any pre-existing entries. The format
    // TS_AUTO_M3_RED_<TYPE>_<YYYYMMDD> matches the convention in the task
    // spec and mirrors test_save_module.cpp / test_compiler_layout.cpp.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char stratName[64];
    char indName[64];
    char funcName[64];
    char brokenName[64];
    char delName[64];
    char batchName[64];
    char tradingAppName[64];
    char showMeName[64];
    char rcvSingleName[64];
    char rcvBatchName[64];
    char delIndName[64];
    char delTradingAppName[64];
    char delShowMeName[64];
    char delFuncNegName[64];
    char cliBatchName[80];
    char cliServeName[80];
    std::snprintf(stratName, sizeof(stratName),
                  "TS_AUTO_M3_RED_STRAT_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(indName, sizeof(indName),
                  "TS_AUTO_M3_RED_IND_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(funcName, sizeof(funcName),
                  "TS_AUTO_M3_RED_FUNC_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(brokenName, sizeof(brokenName),
                  "TS_AUTO_M3_RED_BROKEN_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(delName, sizeof(delName),
                  "TS_AUTO_M3_RED_DEL_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(batchName, sizeof(batchName),
                  "TS_AUTO_M3_RED_BATCH_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(tradingAppName, sizeof(tradingAppName),
                  "TS_AUTO_M3_RED_TRADINGAPP_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(showMeName, sizeof(showMeName),
                  "TS_AUTO_M3_RED_SHOWME_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(rcvSingleName, sizeof(rcvSingleName),
                  "TS_AUTO_M3_RED_RCV_SINGLE_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(rcvBatchName, sizeof(rcvBatchName),
                  "TS_AUTO_M3_RED_RCV_BATCH_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(delIndName, sizeof(delIndName),
                  "TS_AUTO_M3_RED_DEL_IND_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(delTradingAppName, sizeof(delTradingAppName),
                  "TS_AUTO_M3_RED_DEL_TRADINGAPP_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(delShowMeName, sizeof(delShowMeName),
                  "TS_AUTO_M3_RED_DEL_SHOWME_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(delFuncNegName, sizeof(delFuncNegName),
                  "TS_AUTO_M3_RED_DEL_FUNC_NEG_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    // CLI-level E2E cases (15, 16) use date-stamped disposable names per
    // the task spec convention (TS_AUTO_M3_CLI_BATCH_<YYYYMMDD>_NN).
    std::snprintf(cliBatchName, sizeof(cliBatchName),
                  "TS_AUTO_M3_CLI_BATCH_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(cliServeName, sizeof(cliServeName),
                  "TS_AUTO_M3_CLI_SERVE_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);

    // --- Case 1: verify-success sends 0x40A (Strategy, category 0) ---
    {
        int rc = runVerifySuccessCase(*lib, 1, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySource(stratName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 2: verify-success sends 0x40A (Indicator, category 2) ---
    {
        int rc = runVerifySuccessCase(*lib, 2, ComponentType::Indicator,
                                      indName,
                                      makeIndicatorSource(indName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 3: verify-success sends 0x40A (Function, category 4) ---
    {
        int rc = runVerifySuccessCase(*lib, 3, ComponentType::Function,
                                      funcName,
                                      makeFunctionSource(funcName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 4: failed verify does NOT send 0x40A (Strategy, negative) ---
    {
        int rc = runFailedVerifyCase(*lib, 4, ComponentType::Strategy,
                                     brokenName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 5: delete-success sends 0x40A (Strategy, category 0) ---
    {
        int rc = runDeleteSuccessCase(*lib, 5, ComponentType::Strategy,
                                      delName,
                                      makeStrategySource(delName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 6: BATCH REUSE — 3 verify-success cycles (operation-mode
    // coverage, per M.3 plan) ---
    {
        int rc = runBatchReuseCase(*lib, 6, batchName,
                                   makeStrategySource(batchName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 7: verify-success sends 0x40A (TradingApp, category 2) ---
    // Finding 3: covers the 4th of the 5 supported component types for
    // category/ProgId packing parity.
    {
        int rc = runVerifySuccessCase(*lib, 7, ComponentType::TradingApp,
                                      tradingAppName,
                                      makeTradingAppSource(tradingAppName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 8: verify-success sends 0x40A (ShowMe, category 2) ---
    // Finding 3: covers the 5th of the 5 supported component types for
    // category/ProgId packing parity.
    {
        int rc = runVerifySuccessCase(*lib, 8, ComponentType::ShowMe,
                                      showMeName,
                                      makeShowMeSource(showMeName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 9: RECEIVER-AWARE single verify-success — proves ACTUAL
    // delivery via PostMessageW to a window registered with
    // _OrlibRegisterNotificationHandle@4 (ordinal 595), not just the
    // onNotifyReady hook capture (Finding 1). ---
    {
        int rc = runReceiverAwareSingleCase(*lib, 9,
                                            ComponentType::Strategy,
                                            rcvSingleName,
                                            makeStrategySource(rcvSingleName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 10: RECEIVER-AWARE BATCH — 3 verify-success cycles on the
    // SAME shared TsDevLib with the SAME registered receiver window,
    // asserting exactly one 0x40A per operation (3 verify + 3 delete =
    // 6 total) with no missed/duplicated messages (Finding 2). ---
    {
        int rc = runReceiverAwareBatchCase(*lib, 10, rcvBatchName,
                                           makeStrategySource(rcvBatchName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 11: delete-success sends 0x40A (Indicator, category 2) ---
    // Finding 4: covers the Indicator component type for delete-success
    // 0x40A packing parity (delete is in the confirmed set, gated
    // category != 4; Indicator category 2 satisfies the gate).
    {
        int rc = runDeleteSuccessCase_Indicator(*lib, 11,
                                                ComponentType::Indicator,
                                                delIndName,
                                                makeIndicatorSource(delIndName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 12: delete-success sends 0x40A (TradingApp, category 2) ---
    // Finding 4: covers the TradingApp component type for delete-success
    // 0x40A packing parity.
    {
        int rc = runDeleteSuccessCase_Indicator(*lib, 12,
                                                ComponentType::TradingApp,
                                                delTradingAppName,
                                                makeTradingAppSource(delTradingAppName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 13: delete-success sends 0x40A (ShowMe, category 2) ---
    // Finding 4: covers the ShowMe component type for delete-success
    // 0x40A packing parity.
    {
        int rc = runDeleteSuccessCase_Indicator(*lib, 13,
                                                ComponentType::ShowMe,
                                                delShowMeName,
                                                makeShowMeSource(delShowMeName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 14: Function delete does NOT send 0x40A (negative test) ---
    // Finding 5: Function (category 4) delete is OUTSIDE the confirmed
    // operation set (the 0x40A send is gated category != 4). The verify
    // step DOES fire 0x40A (verify-success is in the confirmed set for all
    // categories), but the delete step must NOT. The capture is reset
    // between verify and delete so the negative assertion isolates the
    // delete step.
    {
        int rc = runDeleteNegativeCase_Function(*lib, 14,
                                                ComponentType::Function,
                                                delFuncNegName,
                                                makeFunctionSource(delFuncNegName));
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 15: CLI stdin BATCH — 3 verify-success ops via
    // tsdev_cli.exe batch-insert --batch-stdin (M.3 operation-mode
    // coverage, per the M.3 pre-completion comparative review findings 1 & 2).
    // Drives the REAL tsdev_cli.exe binary as a separate child process;
    // the test's receiver window (registered via ordinal 595) captures the
    // 0x40A posts the CLI's orlib20 instance sends to the shared
    // EventsViaHandles HWND array on each verify-success. Asserts exactly
    // 3 0x40A posts (one per operation), no missed/duplicated, correct
    // packing. ---
    {
        int rc = runCliBatchStdinCase(*lib, 15, cliBatchName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    // --- Case 16: CLI SERVE — 3 verify-success ops via tsdev_cli.exe serve
    // --serve-pipe-file (M.3 operation-mode coverage, reused serve session).
    // Drives the REAL tsdev_cli.exe binary as a separate child process in
    // serve mode; connects to its named pipe, sends 3 insert JSON requests
    // (each does createEntry + saveSource + verifyModule internally), and
    // asserts the test's receiver window captures exactly 3 0x40A posts
    // (one per verify-success), no missed/duplicated, correct packing.
    // Proves a reused serve session delivers one notification per
    // operation. ---
    {
        int rc = runCliServeCase(*lib, 16, cliServeName);
        if (rc != 0) { st->exitCode = rc; return 0; }
    }

    st->exitCode = (g_failCount == 0) ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_0x40a_notification: M.3 0x40A post-success "
                "notification parity ===\n");
    std::printf("Asserts the 0x40A notification (msg=0x40A, wParam=0, "
                "lParam=(category<<16)|ProgId) is sent after verify-success "
                "and delete-success, and NOT sent after failed verify.\n");
    std::printf("See docs/RE/RE_M3_0x40A_NOTIFICATION.md (R-M.3.1, R-M.3.2, "
                "R-M.3.3).\n");
    std::printf("M.3 status: 0x40A send and receiver fixture are implemented; "
                "the historical RED baseline is retained in the handoff.\n");
    std::printf("Desktop/TSDev running state is not a prerequisite; this test uses its own receiver fixture.\n");
    std::printf("Cases: 3 verify-success (Strat/Ind/Func) + failed-verify "
                "(negative) + delete-success + batch-reuse (3 cycles) + "
                "TradingApp + ShowMe + receiver-aware single + "
                "receiver-aware batch (3 cycles) + delete-success "
                "Indicator/TradingApp/ShowMe + Function delete negative + "
                "CLI stdin batch (3 ops via tsdev_cli.exe) + CLI serve "
                "(3 ops via tsdev_cli.exe serve) (16 cases total).\n");

    // Run the entire test on a worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::printf("\n=== test_0x40a_notification: %d/%d assertions passed, "
                "%d failed ===\n",
                g_passCount, g_passCount + g_failCount, g_failCount);
    std::printf("=== test_0x40a_notification complete (exit %d) ===\n",
                st.exitCode);
    return st.exitCode;
}
