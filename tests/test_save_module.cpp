// test_save_module.cpp
//
// Workstream M slice M.2 — TDD RED->GREEN integration test for the
// _SaveModule_New@20 flags (arg3) and password buffer (arg5) parity with
// TSDev.exe.
//
// CONFIRMED FACTS (Ghidra static analysis of TSDev.exe FUN_00452290 at
// 0x00452290, the unique caller of _SaveModule_New@20; see
// docs/RE/RE_M2_SAVEMODULE_NEW.md, UPDATE 2026-08-25 Corrections 1 and 2):
//
//   R-M.2.1 — The flags word (arg3) is computed by FUN_00452290 as a
//   per-item bitfield:
//     flags = (type_word & 0x7)              // bits 0-2: component category
//           | ((has_expdate  & 1) << 3)      // bit 3: has expiration date
//           | ((is_verified  & 1) << 4)      // bit 4: is verified
//           | ((is_protected & 1) << 5)      // bit 5: is protected
//           | ((is_protected_plus & 1) << 6) // bit 6: is protected-plus
//     TSDev ALWAYS forces bits 5 and 6 on (the else branch of FUN_00452290
//     calls SetProtectedPlus(true) + SetProtected(true) when IsProtected()
//     returns false). For a NEW item being saved BEFORE verify (not yet
//     verified, no expiration date), bits 3 and 4 are 0:
//       Strategy  (category 0): flags = 0x60
//       Indicator (category 2): flags = 0x62
//       Function  (category 4): flags = 0x64
//       ShowMe    (category 2): flags = 0x62
//       TradingApp (category 2): flags = 0x62
//
//   R-M.2.3 — The password buffer (arg5) is a 64-byte stack buffer
//   constructed by FUN_00452290 as:
//     memset(buffer, 0, 0x40);                  // 64 bytes zeroed
//     memcpy(buffer, "!p$Kn@aSus", 10);         // then 10-byte default copied in
//   "!p$Kn@aSus" is TSDev's INTERNAL DEFAULT password (at 0x00567b40 in
//   TSDev.exe), NOT a user secret. It is a hardcoded constant baked into
//   TSDev.exe, used as the encryption key material for items TSDev
//   force-protects. The CLI must replicate this fixed internal default
//   string, never accept user input for it.
//
//   R-M.2.4 — There is exactly ONE _SaveModule_New call per verify
//   operation (pre-verify); no post-verify save. The CLI's single
//   saveSource() before verifyModule() matches this ordering.
//
// This test exercises the onSaveModuleNewReady test hook (populated via
// makeTsDevLibForTest) which saveSource() invokes immediately BEFORE the
// _SaveModule_New@20 call. The callback captures the flags pointer,
// password buffer pointer, buffer size, component type, and progId into
// thread_local globals, then the test asserts:
//   - capture.flags is non-NULL
//   - *capture.flags == expected flags value (0x60 / 0x62 / 0x64)
//   - capture.passwordBuf is non-NULL
//   - capture.passwordBuf starts with "!p$Kn@aSus" (first 10 bytes)
//   - capture.passwordBuf bytes 10-63 are all zero
//   - saveSource returned true
//   - getSource round-trip contains the original source text
//
// HISTORICAL RED STATE (pre-M.2): the production code then passed NULL for
// both arg3 (flags) and arg5 (password). The hook received those NULL values,
// so the two non-NULL assertions failed. The current M.2 implementation passes
// the non-NULL flags word and 64-byte password buffer matching the TSDev values
// above. The historical RED→GREEN transition is retained in the slice handoff.
//
// Six insert/modify test cases + one batch-reuse case (adversarial review
// findings #1, #2, #4):
//   1. Strategy   (category 0) — expected insert flags 0x60
//   2. Indicator  (category 2) — expected insert flags 0x62
//   3. Function   (category 4) — expected insert flags 0x64
//   4. MODIFY Strategy — insert (0x60) -> verify -> modify save (0x70, bit 4
//      is_verified set). Proves the modify path sets flags bit 4.
//   5. ShowMe     (category 2) — expected insert flags 0x62
//   6. TradingApp (category 2) — expected insert flags 0x62
//   7. BATCH REUSE — 3 consecutive insert+save+verify+delete cycles on the
//      SAME shared TsDevLib, asserting flags=0x60 + password on EACH
//      iteration (proves stack buffers re-init per call in serve/batch mode).
//
// Each insert case inserts a disposable item, calls saveSource (which fires
// the hook), asserts the flags/password, calls verifyModule (compile +
// persist), round-trips via getSource (substring + length-tolerance check),
// then deletes the disposable item.
//
// REQUIRES TradeStation: this is an integration test that constructs a real
// TsDevLib (loads orlib20.dll, applies patches, calls OrlibInit, creates the
// hidden EDIT control) and inserts + verifies + deletes real warehouse
// entries. This is a library-level test; Desktop/TSDev running state is not a
// prerequisite because the test does not exercise the Desktop UI.
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
#include <windows.h>

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
// Thread-local capture state for the onSaveModuleNewReady hook.
//
// The hook runs on the worker thread (the same thread saveSource runs on)
// and writes into these globals. The test reads them after saveSource
// returns. thread_local ensures each worker thread has its own copy (the
// main thread's copy stays zeroed).
//
// The password buffer is copied into a local 64-byte array so the test can
// inspect it after saveSource returns (the production-side buffer is on the
// saveSource stack and would be invalid after the call returns).
// ---------------------------------------------------------------------------
struct SaveModuleCapture {
    bool                flagsPresent     = false;
    unsigned int        flagsValue       = 0;
    char                passwordBuf[64] = {};
    int                 passwordBufSize = 0;
    int                 componentType   = -1;
    int                 progId          = -1;
    bool                fired           = false;
};

thread_local SaveModuleCapture g_capture;

void onSaveModuleNewReadyHook(const unsigned int* flags,
                              const char* passwordBuf,
                              int passwordBufSize,
                              int componentType,
                              int progId) {
    g_capture.flagsPresent    = (flags != nullptr);
    g_capture.flagsValue      = flags ? *flags : 0u;
    g_capture.passwordBufSize = passwordBufSize;
    g_capture.componentType   = componentType;
    g_capture.progId          = progId;
    g_capture.fired           = true;
    // Copy up to 64 bytes of the password buffer so the test can inspect it
    // after saveSource returns. If passwordBuf is NULL (the historical pre-M.2
    // RED state), the buffer stays zeroed (matching the capture struct's
    // default initialization). Current production passes a non-NULL
    // initialized/populated buffer, as documented elsewhere.
    if (passwordBuf != nullptr && passwordBufSize > 0) {
        int n = (passwordBufSize < 64) ? passwordBufSize : 64;
        std::memcpy(g_capture.passwordBuf, passwordBuf, n);
    }
}

// ---------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// ---------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// Build a TsDevLibTestConfig with the real COM API and the
// onSaveModuleNewReady hook installed. throwAfterCom is false so the ctor
// proceeds with the full init sequence (loads orlib20, applies patches,
// OrlibInit, EDIT control).
TsDevLibTestConfig makeConfigWithHook() {
    TsDevLibTestConfig cfg;
    cfg.comApi               = realComApi();
    cfg.throwAfterCom        = false;
    cfg.onSaveModuleNewReady = &onSaveModuleNewReadyHook;
    return cfg;
}

// ---------------------------------------------------------------------------
// Test fixture: per-case state.
// ---------------------------------------------------------------------------
struct TestCase {
    ComponentType type;
    std::string   name;
    std::string   source;
    unsigned int  expectedFlags;  // (getCategory(type) & 0x7) | 0x60
};

// Minimal valid EasyLanguage source for each component type, mirroring the
// proven patterns from test_compiler_layout.cpp (Strategy, Indicator) and
// test_get_source.cpp (Function). In EasyLanguage `{ ... }` is a multi-line
// COMMENT block; the actual code must be OUTSIDE it. The brace text is
// arbitrary fixture source content only, not a metadata convention or lookup
// mechanism. Stored names, types, and ProgIds come from warehouse records/APIs.
// For Function, the function name in the source body MUST match the item name
// for EasyLanguage compiler syntax, independently of stored metadata.
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

// ShowMe studies can use Plot statements (like Indicators) but cannot issue
// order statements (Buy/Sell are Strategy-only). Pattern confirmed via
// tests/test_showme_e2e.ps1 (SM1) and tests/test_tsdev_client.py.
std::string makeShowMeSource(const std::string& name) {
    return std::string("{ ") + name + " }\r\n"
           "if Close > Open then\r\n"
           "    Plot1(High, \"Up bar\");\r\n";
}

// TradingApp minimal source — proven to compile in TVS-C.3 E2E and
// test_insert_tradingapp.cpp. "using elsystem;" is the minimal valid
// TradingApp source; the full Form-based source is a fallback only.
std::string makeTradingAppSource(const std::string& /*name*/) {
    return std::string("using elsystem;\r\n");
}

// The internal TSDev default password (NOT a user secret). See
// docs/RE/RE_M2_SAVEMODULE_NEW.md UPDATE 2026-08-25 Correction 2: this is
// the hardcoded constant at 0x00567b40 in TSDev.exe, used as the encryption
// key material for items TSDev force-protects.
const char* kInternalDefaultPassword = "!p$Kn@aSus";  // 10 bytes

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Run one test case against an ALREADY-CONSTRUCTED TsDevLib (passed by
// reference). The caller (runTest) constructs ONE TsDevLib for the whole
// worker thread and passes it to all three cases — this matches the
// operational invariant used by every other integration test in this
// codebase (see test_auto_cleanup.cpp, test_stress_batch.cpp,
// test_compiler_layout.cpp): a SINGLE TsDevLib per worker thread.
// Constructing a second TsDevLib on the same thread leaves orlib20 global
// state in a bad state after the first instance's destructor runs OrlibExit
// and the second instance's constructor runs OrlibInit again.
//
// The onSaveModuleNewReady hook is a function pointer that writes to
// thread_local globals, so it works correctly with a single shared instance
// across all cases (the capture is reset at the top of each case).
//
// Returns 0 on success, nonzero on failure. Updates g_failCount/g_passCount.
// ---------------------------------------------------------------------------
int runOneCase(TsDevLib& lib, const TestCase& tc, int caseNum) {
    const char* typeLabel =
        (tc.type == ComponentType::Strategy)   ? "Strategy"   :
        (tc.type == ComponentType::Indicator)  ? "Indicator"  :
        (tc.type == ComponentType::Function)   ? "Function"   :
        (tc.type == ComponentType::ShowMe)     ? "ShowMe"     :
        (tc.type == ComponentType::TradingApp) ? "TradingApp" : "Unknown";
    std::printf("\n=== Case %d: %s (name=%s, expectedFlags=0x%02X) ===\n",
                caseNum, typeLabel, tc.name.c_str(),
                static_cast<unsigned>(tc.expectedFlags));

    // Reset the capture so a stale value from a previous case cannot mask a
    // "hook did not fire" failure.
    g_capture = SaveModuleCapture{};

    // --- 1. TsDevLib already constructed by runTest (hook installed) ---
    std::printf("  [1] Using shared TsDevLib (test-only ctor, hook installed)\n");

    // --- 2. If the test item already exists, delete it first (re-run safe) ---
    int existingProgId = lib.findExistPos(tc.name, tc.type);
    if (existingProgId > 0) {
        std::printf("  [2] Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    tc.name.c_str(), existingProgId);
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
    }

    // --- 3. createEntry ---
    std::printf("  [3] createEntry(%s, \"%s\")...\n",
                typeLabel, tc.name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(tc.type, tc.name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("createEntry returned ProgId <= 0", 100 + caseNum * 10 + 2);
    }

    // --- 4. saveSource (fires the hook — captures flags + password) ---
    std::printf("  [4] saveSource(%s, %d, source)...\n", typeLabel, progId);
    bool saved = lib.saveSource(tc.type, progId, tc.source);
    std::printf("      saveSource = %s\n", saved ? "true" : "false");

    // --- 5. Assert the hook fired ---
    CHECK(g_capture.fired,
          "onSaveModuleNewReady hook fired during saveSource");
    if (!g_capture.fired) {
        lib.deleteEntry(tc.type, tc.name, /*flush=*/false);
        return fail("hook did not fire — cannot assert flags/password",
                    100 + caseNum * 10 + 5);
    }

    // --- 6. Assert the captured component type and progId ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "captured componentType==%d (expected %d) [case %d]",
                      g_capture.componentType,
                      static_cast<int>(tc.type), caseNum);
        CHECK(g_capture.componentType == static_cast<int>(tc.type), buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "captured progId==%d (expected %d) [case %d]",
                      g_capture.progId, progId, caseNum);
        CHECK(g_capture.progId == progId, buf);
    }

    // --- 7. Assert the flags were captured (RED: currently NULL/not present) ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "capture.flagsPresent [case %d, got %d]",
                      caseNum, static_cast<int>(g_capture.flagsPresent));
        CHECK(g_capture.flagsPresent, buf);
    }

    // --- 8. Assert capture.flagsValue == expected flags value ---
    // On RED this fails because flagsPresent is false (flagsValue stays 0);
    // on GREEN it verifies the exact flag word.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "capture.flagsValue==0x%02X (expected 0x%02X) [case %d]",
                      g_capture.flagsValue, tc.expectedFlags, caseNum);
        CHECK(g_capture.flagsValue == tc.expectedFlags, buf);
    }

    // --- 9. Assert the password buffer pointer is non-NULL (RED: NULL) ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "capture.passwordBufSize==64 [case %d, got %d]",
                      caseNum, g_capture.passwordBufSize);
        CHECK(g_capture.passwordBufSize == 64, buf);
    }

    // --- 10. Assert the password buffer starts with "!p$Kn@aSus" ---
    // The hook copies up to 64 bytes into g_capture.passwordBuf. On RED the
    // buffer is all-zero (hook received NULL), so this assertion fails.
    if (g_capture.passwordBufSize > 0 &&
        g_capture.passwordBuf[0] != '\0') {
        bool prefixOk = (std::memcmp(g_capture.passwordBuf,
                                     kInternalDefaultPassword, 10) == 0);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "passwordBuf starts with \"!p$Kn@aSus\" [case %d]",
                      caseNum);
        CHECK(prefixOk, buf);
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "passwordBuf starts with \"!p$Kn@aSus\" [case %d, "
                      "buffer empty/NULL — skipped]",
                      caseNum);
        CHECK(false, buf);
    }

    // --- 11. Assert password buffer bytes 10-63 are all zero ---
    // The 64-byte buffer is zeroed first, then the 10-byte password is
    // copied in, so bytes 10-63 must remain zero.
    if (g_capture.passwordBufSize >= 64) {
        bool tailZero = true;
        for (int i = 10; i < 64; ++i) {
            if (g_capture.passwordBuf[i] != 0) {
                tailZero = false;
                break;
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "passwordBuf bytes 10-63 all zero [case %d]", caseNum);
        CHECK(tailZero, buf);
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "passwordBuf bytes 10-63 all zero [case %d, "
                      "buffer size < 64 — skipped]",
                      caseNum);
        CHECK(false, buf);
    }

    // --- 12. Assert saveSource returned true ---
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "saveSource returned true [case %d, got %s]",
                      caseNum, saved ? "true" : "false");
        CHECK(saved, buf);
    }

    // --- 13. verifyModule (complete the insert — may succeed or fail) ---
    // The RED test is about the flags/password assertions above; verifyModule
    // is invoked to complete the insert path so the warehouse state is
    // consistent for the getSource round-trip and the cleanup. A verify
    // failure here does NOT fail this test (the flags/password assertions
    // are the RED signal), but is reported for diagnostics.
    std::printf("  [13] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(tc.type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr, "      verifyModule errors (diagnostic only):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }

    // --- 14. getSource round-trip — assert retrieved source contains the
    // original source text. The warehouse may normalize whitespace/line
    // endings, so we use substring checks on key tokens. ---
    std::printf("  [14] getSource(%s, \"%s\") — round-trip...\n",
                typeLabel, tc.name.c_str());
    std::string retrieved = lib.getSource(tc.type, tc.name);
    std::printf("      getSource returned %zu bytes\n", retrieved.size());
    // Pick a key substring per type: "Buy" for Strategy, "Plot1" for
    // Indicator/ShowMe, "elsystem" for TradingApp, the function name for
    // Function.
    const char* keySubstr =
        (tc.type == ComponentType::Strategy)   ? "Buy"        :
        (tc.type == ComponentType::Indicator)  ? "Plot1"      :
        (tc.type == ComponentType::ShowMe)     ? "Plot1"      :
        (tc.type == ComponentType::TradingApp) ? "elsystem"   :
        (tc.type == ComponentType::Function)   ? tc.name.c_str() : "";
    {
        char buf[160];
        bool rtOk = (!retrieved.empty() &&
                     retrieved.find(keySubstr) != std::string::npos);
        std::snprintf(buf, sizeof(buf),
                      "getSource round-trip contains \"%s\" [case %d]",
                      keySubstr, caseNum);
        CHECK(rtOk, buf);
    }

    // --- 14b. getSource round-trip length check (pragmatic byte-for-byte) ---
    // The warehouse may normalize line endings (\r\n -> \n) and trim trailing
    // whitespace, so an exact byte-for-byte compare is fragile. Instead assert
    // the retrieved length is within a few bytes of the original source length
    // (allowing for line-ending normalization: at most one byte per line, plus
    // a small margin). This catches gross round-trip corruption (truncation,
    // duplication, garbage) without being brittle to whitespace normalization.
    {
        // Count lines in the original source to bound the line-ending delta.
        int lineCount = 1;
        for (char c : tc.source) if (c == '\n') ++lineCount;
        // Allow: per-line \r\n->\n delta (lineCount bytes) + trailing-newline
        // trim (1) + a 4-byte safety margin for any other normalization.
        long maxDelta = lineCount + 1 + 4;
        long srcLen = static_cast<long>(tc.source.size());
        long gotLen = static_cast<long>(retrieved.size());
        long diff = gotLen - srcLen;
        bool lenOk = (!retrieved.empty() &&
                      diff >= -maxDelta && diff <= maxDelta);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "getSource round-trip length within tolerance "
                      "(src=%ld got=%ld diff=%ld maxDelta=%ld) [case %d]",
                      srcLen, gotLen, diff, maxDelta, caseNum);
        CHECK(lenOk, buf);
    }

    // --- 15. Cleanup: delete the disposable item ---
    std::printf("  [15] Cleanup: deleteEntry(%s, \"%s\")...\n",
                typeLabel, tc.name.c_str());
    if (!lib.deleteEntry(tc.type, tc.name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual artifact?)\n");
    }

    // Reset capture so it does not leak into the next case.
    g_capture = SaveModuleCapture{};

    return 0;
}

// ---------------------------------------------------------------------------
// runModifyCase — adversarial review finding #1: no modify case.
//
// Exercises the MODIFY path (saveSource with isVerified=true), which must set
// flags bit 4 (is_verified) per TSDev FUN_00452290. The flow:
//   1. createEntry (insert)        — saveSource #1 with isVerified=false
//   2. verifyModule                — compile + persist (item now verified)
//   3. saveSource #2 with isVerified=true  — the MODIFY save, fires the hook
//   4. Assert the SECOND hook capture: flags == 0x70 (0x60 | 0x10), password
//      buffer still "!p$Kn@aSus", bytes 10-63 still zero.
//   5. getSource round-trip after modify (substring + length tolerance).
//   6. Cleanup: deleteEntry.
//
// The KEY assertion is that the second saveSource call (the modify) captures
// flags=0x70, NOT 0x60 — proving bit 4 (is_verified) is set on the modify
// path. This is the production-code change the PM made (saveSource now takes
// isVerified; modifyEntry passes true).
//
// Returns 0 on success, nonzero on failure. Updates g_failCount/g_passCount.
// ---------------------------------------------------------------------------
int runModifyCase(TsDevLib& lib, int caseNum,
                  const std::string& name,
                  const std::string& insertSource,
                  const std::string& modifySource) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    // Insert (pre-verify) flags: (0 & 0x7) | 0x60 = 0x60.
    // Modify (post-verify) flags: 0x60 | 0x10 (bit 4 is_verified) = 0x70.
    const unsigned int expectedInsertFlags = 0x60;
    const unsigned int expectedModifyFlags = 0x70;

    std::printf("\n=== Case %d: MODIFY %s (name=%s, "
                "insertFlags=0x%02X, modifyFlags=0x%02X) ===\n",
                caseNum, typeLabel, name.c_str(),
                expectedInsertFlags, expectedModifyFlags);

    // --- 1. Pre-cleanup ---
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  [1] Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }

    // --- 2. createEntry ---
    std::printf("  [2] createEntry(%s, \"%s\")...\n", typeLabel, name.c_str());
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry = %d, errCode = 0x%X\n", progId, errCode);
    if (progId <= 0) {
        return fail("modify case: createEntry returned ProgId <= 0",
                    100 + caseNum * 10 + 2);
    }

    // --- 3. saveSource #1 (INSERT, isVerified=false) — fires hook ---
    g_capture = SaveModuleCapture{};
    std::printf("  [3] saveSource #1 INSERT (isVerified=false)...\n");
    bool saved1 = lib.saveSource(type, progId, insertSource, /*isVerified=*/false);
    std::printf("      saveSource #1 = %s\n", saved1 ? "true" : "false");
    CHECK(g_capture.fired,
          "modify: hook fired on INSERT saveSource");
    if (!g_capture.fired) {
        lib.deleteEntry(type, name, /*flush=*/false);
        return fail("modify: hook did not fire on insert save",
                    100 + caseNum * 10 + 3);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: INSERT flags==0x%02X (expected 0x%02X) [case %d]",
                      g_capture.flagsValue, expectedInsertFlags, caseNum);
        CHECK(g_capture.flagsValue == expectedInsertFlags, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: INSERT saveSource returned true [case %d]",
                      caseNum);
        CHECK(saved1, buf);
    }

    // --- 4. verifyModule (compile + persist — item is now verified) ---
    std::printf("  [4] verifyModule(%s, %d)...\n", typeLabel, progId);
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors (diagnostic only):\n%s\n",
                     errors.c_str());
    } else {
        std::printf("      verifyModule: no errors (compilation OK)\n");
    }

    // --- 5. saveSource #2 (MODIFY, isVerified=true) — fires hook ---
    // This is the KEY assertion: the second saveSource must capture flags=0x70
    // (bit 4 set), proving the modify path sets is_verified.
    g_capture = SaveModuleCapture{};
    std::printf("  [5] saveSource #2 MODIFY (isVerified=true)...\n");
    bool saved2 = lib.saveSource(type, progId, modifySource, /*isVerified=*/true);
    std::printf("      saveSource #2 = %s\n", saved2 ? "true" : "false");
    CHECK(g_capture.fired,
          "modify: hook fired on MODIFY saveSource");
    if (!g_capture.fired) {
        lib.deleteEntry(type, name, /*flush=*/false);
        return fail("modify: hook did not fire on modify save",
                    100 + caseNum * 10 + 5);
    }

    // KEY ASSERTION: modify flags == 0x70 (bit 4 set), NOT 0x60.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY flags==0x%02X (expected 0x%02X, "
                      "bit 4 is_verified set) [case %d]",
                      g_capture.flagsValue, expectedModifyFlags, caseNum);
        CHECK(g_capture.flagsValue == expectedModifyFlags, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY saveSource returned true [case %d]",
                      caseNum);
        CHECK(saved2, buf);
    }

    // Assert the modify's password buffer is still "!p$Kn@aSus".
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY passwordBufSize==64 [case %d, got %d]",
                      caseNum, g_capture.passwordBufSize);
        CHECK(g_capture.passwordBufSize == 64, buf);
    }
    if (g_capture.passwordBufSize > 0 && g_capture.passwordBuf[0] != '\0') {
        bool prefixOk = (std::memcmp(g_capture.passwordBuf,
                                     kInternalDefaultPassword, 10) == 0);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY passwordBuf starts with \"!p$Kn@aSus\" [case %d]",
                      caseNum);
        CHECK(prefixOk, buf);
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY passwordBuf starts with \"!p$Kn@aSus\" "
                      "[case %d, buffer empty/NULL]", caseNum);
        CHECK(false, buf);
    }
    if (g_capture.passwordBufSize >= 64) {
        bool tailZero = true;
        for (int i = 10; i < 64; ++i) {
            if (g_capture.passwordBuf[i] != 0) { tailZero = false; break; }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY passwordBuf bytes 10-63 all zero [case %d]",
                      caseNum);
        CHECK(tailZero, buf);
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "modify: MODIFY passwordBuf bytes 10-63 all zero "
                      "[case %d, buffer size < 64]", caseNum);
        CHECK(false, buf);
    }

    // --- 6. getSource round-trip after modify ---
    // After saveSource #2 (the modify save, isVerified=true), the EDIT
    // control holds the modify source. getSource reads from the EDIT control
    // (the in-memory source), so the retrieved source reflects the MODIFY
    // source, not the original insert source. We assert getSource returns a
    // non-empty result containing the modify source's key token ("Sell"),
    // proving the modify save correctly placed the new source and the item
    // is still retrievable.
    std::printf("  [6] getSource(%s, \"%s\") — round-trip after modify...\n",
                typeLabel, name.c_str());
    std::string retrieved = lib.getSource(type, name);
    std::printf("      getSource returned %zu bytes\n", retrieved.size());
    {
        char buf[160];
        bool rtOk = (!retrieved.empty() &&
                     retrieved.find("Sell") != std::string::npos);
        std::snprintf(buf, sizeof(buf),
                      "modify: getSource round-trip contains \"Sell\" (modify source) [case %d]",
                      caseNum);
        CHECK(rtOk, buf);
    }
    // Length-tolerance check (pragmatic byte-for-byte) against the MODIFY source.
    {
        int lineCount = 1;
        for (char c : modifySource) if (c == '\n') ++lineCount;
        long maxDelta = lineCount + 1 + 4;
        long srcLen = static_cast<long>(modifySource.size());
        long gotLen = static_cast<long>(retrieved.size());
        long diff = gotLen - srcLen;
        bool lenOk = (!retrieved.empty() &&
                      diff >= -maxDelta && diff <= maxDelta);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "modify: getSource round-trip length within tolerance "
                      "(src=%ld got=%ld diff=%ld maxDelta=%ld) [case %d]",
                      srcLen, gotLen, diff, maxDelta, caseNum);
        CHECK(lenOk, buf);
    }

    // --- 7. Cleanup: delete the disposable item ---
    std::printf("  [7] Cleanup: deleteEntry(%s, \"%s\")...\n",
                typeLabel, name.c_str());
    if (!lib.deleteEntry(type, name, /*flush=*/true)) {
        std::fprintf(stderr,
                     "  WARNING: deleteEntry returned false (residual artifact?)\n");
    }

    g_capture = SaveModuleCapture{};
    return 0;
}

// ---------------------------------------------------------------------------
// runBatchReuseCase — adversarial review finding #2: no batch/serve mode
// coverage.
//
// Proves the stack buffers (saveFlags, savePasswordBuf in saveSource) are
// correctly re-initialized on EVERY iteration within a single long-lived
// TsDevLib process — the batch/serve reuse pattern. Performs N consecutive
// insert+saveSource+verifyModule+deleteEntry cycles on Strategy items with
// distinct names, asserting the hook fires with correct flags (0x60) and
// password on EACH iteration.
//
// Uses the SAME shared TsDevLib instance (passed by reference) — this is the
// critical invariant: a single instance reused across many operations, as the
// serve-mode daemon and the batch CLI do.
//
// Returns 0 on success, nonzero on failure. Updates g_failCount/g_passCount.
// ---------------------------------------------------------------------------
int runBatchReuseCase(TsDevLib& lib, int caseNum,
                      const std::string& baseName,
                      const std::string& source) {
    const ComponentType type = ComponentType::Strategy;
    const char* typeLabel = "Strategy";
    const unsigned int expectedFlags = 0x60;
    const int kIterations = 3;

    std::printf("\n=== Case %d: BATCH REUSE %s (base=%s, %d iterations, "
                "expectedFlags=0x%02X) ===\n",
                caseNum, typeLabel, baseName.c_str(), kIterations,
                expectedFlags);

    for (int iter = 1; iter <= kIterations; ++iter) {
        // Unique per-iteration name with date + iteration suffix.
        char name[80];
        std::snprintf(name, sizeof(name), "%s_IT%d", baseName.c_str(), iter);
        std::printf("\n  --- Batch iteration %d/%d (name=%s) ---\n",
                    iter, kIterations, name);

        // Reset capture so a stale value cannot mask a "did not fire" failure.
        g_capture = SaveModuleCapture{};

        // Pre-cleanup.
        int existingProgId = lib.findExistPos(name, type);
        if (existingProgId > 0) {
            std::printf("    Pre-cleanup: deleting existing (ProgId=%d)\n",
                        existingProgId);
            lib.deleteEntry(type, name, /*flush=*/false);
        }

        // createEntry.
        int errCode = 0;
        int progId = lib.createEntry(type, name, errCode);
        std::printf("    createEntry = %d, errCode = 0x%X\n", progId, errCode);
        if (progId <= 0) {
            return fail("batch: createEntry returned ProgId <= 0",
                        100 + caseNum * 10 + iter);
        }

        // saveSource (fires hook).
        bool saved = lib.saveSource(type, progId, source, /*isVerified=*/false);
        std::printf("    saveSource = %s\n", saved ? "true" : "false");

        // Assert hook fired THIS iteration (not a stale value).
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: hook fired [case %d]", iter, caseNum);
            CHECK(g_capture.fired, buf);
        }
        if (!g_capture.fired) {
            lib.deleteEntry(type, name, /*flush=*/false);
            return fail("batch: hook did not fire on iteration",
                        100 + caseNum * 10 + iter);
        }

        // Assert flags == 0x60 THIS iteration (re-init check).
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: flags==0x%02X (expected 0x%02X) [case %d]",
                          iter, g_capture.flagsValue, expectedFlags, caseNum);
            CHECK(g_capture.flagsValue == expectedFlags, buf);
        }

        // Assert password buffer starts with "!p$Kn@aSus" THIS iteration.
        {
            bool prefixOk = (g_capture.passwordBufSize == 64 &&
                             g_capture.passwordBuf[0] != '\0' &&
                             std::memcmp(g_capture.passwordBuf,
                                         kInternalDefaultPassword, 10) == 0);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: passwordBuf starts with \"!p$Kn@aSus\" "
                          "[case %d]", iter, caseNum);
            CHECK(prefixOk, buf);
        }

        // Assert password buffer bytes 10-63 all zero THIS iteration.
        {
            bool tailZero = (g_capture.passwordBufSize >= 64);
            if (tailZero) {
                for (int i = 10; i < 64; ++i) {
                    if (g_capture.passwordBuf[i] != 0) { tailZero = false; break; }
                }
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: passwordBuf bytes 10-63 all zero [case %d]",
                          iter, caseNum);
            CHECK(tailZero, buf);
        }

        // Assert saveSource returned true.
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "batch it%d: saveSource returned true [case %d]",
                          iter, caseNum);
            CHECK(saved, buf);
        }

        // verifyModule (complete the insert — diagnostic only).
        std::string errors = lib.verifyModule(type, progId);
        if (!errors.empty()) {
            std::fprintf(stderr,
                         "    batch it%d: verifyModule errors (diagnostic):\n%s\n",
                         iter, errors.c_str());
        } else {
            std::printf("    batch it%d: verifyModule OK\n", iter);
        }

        // Cleanup THIS iteration.
        if (!lib.deleteEntry(type, name, /*flush=*/true)) {
            std::fprintf(stderr,
                         "    batch it%d: WARNING deleteEntry returned false\n",
                         iter);
        }

        // Reset capture between iterations.
        g_capture = SaveModuleCapture{};
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Worker thread entry point. Constructs ONE TsDevLib (with the
// onSaveModuleNewReady hook installed) for the whole worker thread and runs
// all test cases against it. This matches the operational invariant
// used by every other integration test in this codebase: a SINGLE TsDevLib
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
    std::printf("\n--- Constructing shared TsDevLib (test-only ctor, hook installed) ---\n");
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
    // TS_AUTO_M2_SAVE_<TYPE>_<YYYYMMDD> matches the convention in the task
    // spec and mirrors test_compiler_layout.cpp.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char stratName[64];
    char indName[64];
    char funcName[64];
    char showmeName[64];
    char tappName[64];
    char modName[64];
    char batchName[64];
    std::snprintf(stratName, sizeof(stratName),
                  "TS_AUTO_M2_SAVE_STRAT_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(indName, sizeof(indName),
                  "TS_AUTO_M2_SAVE_IND_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(funcName, sizeof(funcName),
                  "TS_AUTO_M2_SAVE_FUNC_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(showmeName, sizeof(showmeName),
                  "TS_AUTO_M2_SAVE_SHOWME_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(tappName, sizeof(tappName),
                  "TS_AUTO_M2_SAVE_TAPP_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(modName, sizeof(modName),
                  "TS_AUTO_M2_SAVE_MOD_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);
    std::snprintf(batchName, sizeof(batchName),
                  "TS_AUTO_M2_SAVE_BATCH_%04d%02d%02d",
                  lt.wYear, lt.wMonth, lt.wDay);

    // Expected flags per docs/RE/RE_M2_SAVEMODULE_NEW.md (UPDATE 2026-08-25,
    // Correction 1): (getCategory(type) & 0x7) | 0x60 for a pre-verify save
    // (bits 5,6 forced on; bits 3,4 = 0).
    // ShowMe and TradingApp both have category 2, so expected flags = 0x62.
    TestCase case1{
        ComponentType::Strategy,
        stratName,
        makeStrategySource(stratName),
        static_cast<unsigned int>((getCategory(ComponentType::Strategy) & 0x7) | 0x60)  // 0x60
    };
    TestCase case2{
        ComponentType::Indicator,
        indName,
        makeIndicatorSource(indName),
        static_cast<unsigned int>((getCategory(ComponentType::Indicator) & 0x7) | 0x60)  // 0x62
    };
    TestCase case3{
        ComponentType::Function,
        funcName,
        makeFunctionSource(funcName),
        static_cast<unsigned int>((getCategory(ComponentType::Function) & 0x7) | 0x60)  // 0x64
    };
    // Adversarial review finding #4: cover ShowMe + TradingApp (category 2).
    TestCase case5{
        ComponentType::ShowMe,
        showmeName,
        makeShowMeSource(showmeName),
        static_cast<unsigned int>((getCategory(ComponentType::ShowMe) & 0x7) | 0x60)  // 0x62
    };
    TestCase case6{
        ComponentType::TradingApp,
        tappName,
        makeTradingAppSource(tappName),
        static_cast<unsigned int>((getCategory(ComponentType::TradingApp) & 0x7) | 0x60)  // 0x62
    };

    // --- Cases 1-3: insert (Strategy / Indicator / Function) ---
    int rc1 = runOneCase(*lib, case1, 1);
    if (rc1 != 0) {
        st->exitCode = rc1;
        return 0;
    }

    int rc2 = runOneCase(*lib, case2, 2);
    if (rc2 != 0) {
        st->exitCode = rc2;
        return 0;
    }

    int rc3 = runOneCase(*lib, case3, 3);
    if (rc3 != 0) {
        st->exitCode = rc3;
        return 0;
    }

    // --- Case 4: MODIFY (adversarial review finding #1) ---
    // Insert a Strategy, verify it, then saveSource with isVerified=true and
    // assert the modify flags = 0x70 (bit 4 set).
    {
        std::string insertSrc = makeStrategySource(modName);
        // Modify source: a different valid Strategy body (Sell instead of Buy)
        // so the modify is observably different.
        std::string modifySrc = std::string("{ ") + modName + " }\r\n"
            "Sell (\"EnterShort\") next bar at Market;\r\n";
        int rc4 = runModifyCase(*lib, 4, modName, insertSrc, modifySrc);
        if (rc4 != 0) {
            st->exitCode = rc4;
            return 0;
        }
    }

    // --- Case 5: ShowMe insert (adversarial review finding #4) ---
    int rc5 = runOneCase(*lib, case5, 5);
    if (rc5 != 0) {
        st->exitCode = rc5;
        return 0;
    }

    // --- Case 6: TradingApp insert (adversarial review finding #4) ---
    int rc6 = runOneCase(*lib, case6, 6);
    if (rc6 != 0) {
        st->exitCode = rc6;
        return 0;
    }

    // --- Case 7: BATCH REUSE (adversarial review finding #2) ---
    // 3 consecutive insert+saveSource+verifyModule+deleteEntry cycles on the
    // SAME shared TsDevLib, asserting flags=0x60 + password on EACH iteration.
    {
        std::string batchSrc = makeStrategySource(batchName);
        int rc7 = runBatchReuseCase(*lib, 7, batchName, batchSrc);
        if (rc7 != 0) {
            st->exitCode = rc7;
            return 0;
        }
    }

    st->exitCode = (g_failCount == 0) ? 0 : 1;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_save_module: M.2 _SaveModule_New flags/password parity ===\n");
    std::printf("Asserts the flags word (arg3) and password buffer (arg5) passed to\n");
    std::printf("_SaveModule_New@20 by saveSource() match the TSDev.exe values\n");
    std::printf("(FUN_00452290; see docs/RE/RE_M2_SAVEMODULE_NEW.md).\n");
    std::printf("Expected: flags=(category&0x7)|0x60 (insert) | 0x70 (modify, bit 4 set),\n");
    std::printf("          password=\"!p$Kn@aSus\" (internal default).\n");
    std::printf("Desktop/TSDev running state is not a prerequisite for this library-level test.\n");
    std::printf("Cases: 3 insert (Strat/Ind/Func) + modify + ShowMe + TradingApp + batch-reuse.\n");

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

    std::printf("\n=== test_save_module: %d/%d assertions passed, %d failed ===\n",
                g_passCount, g_passCount + g_failCount, g_failCount);
    std::printf("=== test_save_module complete (exit %d) ===\n",
                st.exitCode);
    return st.exitCode;
}
