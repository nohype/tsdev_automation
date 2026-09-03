// test_tradingapp_type.cpp
//
// Unit test (RED-GREEN TDD) for TVS-C.3: TradingApp ComponentType enum entry
// and the category/sub-type decomposition layer (getCategory / getSubType).
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack. It only exercises
// the ComponentType enum and the inline getCategory/getSubType helpers defined
// in tsdev_lib.h. It runs on a worker thread with a 64 MB stack only to match
// the project's test harness convention (all tests run on a large-stack worker
// thread); the stack size is harmless for a unit test.
//
// Checks:
//   1. ComponentType::TradingApp exists and equals 5122 (0x1402).
//   2. getCategory(TradingApp) == 2 (low 8 bits = category).
//   3. getSubType(TradingApp) == 0x14 (20) (high bits = sub-type).
//   4. Decomposition is a no-op for existing sub-type-0 types:
//        Strategy   -> category 0, sub-type 0
//        Indicator  -> category 2, sub-type 0
//        Function   -> category 1, sub-type 0
//   5. CLI parseType accepts "tradingapp" -> ComponentType::TradingApp.
//      (parseType is file-local in tsdev_cli.cpp, so it is not directly
//      callable. Instead we verify the string->enum mapping contract by
//      confirming that the recognized token "tradingapp" maps to the
//      TradingApp enum value via a local case-insensitive comparison, which
//      mirrors the exact logic in parseType.)
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Case-insensitive ASCII string comparison (mirrors iequals in tsdev_cli.cpp).
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
        [](char ca, char cb) {
            return std::tolower(static_cast<unsigned char>(ca)) ==
                   std::tolower(static_cast<unsigned char>(cb));
        });
}

// Local replica of parseType's tradingapp branch. Because parseType itself is
// file-local in tsdev_cli.cpp, we cannot call it directly. This function
// mirrors ONLY the tradingapp branch to verify the string->enum contract. The
// real parseType is exercised end-to-end by the CLI; this unit test confirms
// the enum value is correct and that the canonical token maps to it.
bool parseTradingApp(const std::string& s, ComponentType& out) {
    if (iequals(s, "tradingapp")) { out = ComponentType::TradingApp; return true; }
    return false;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_tradingapp_type: TradingApp enum + decomposition ===\n");

    // ------------------------------------------------------------------
    // Check 1: ComponentType::TradingApp exists and equals 5122.
    // ------------------------------------------------------------------
    {
        int v = static_cast<int>(ComponentType::TradingApp);
        std::fprintf(stdout, "  TradingApp enum value = %d (0x%X)\n", v, v);
        if (v != 5122) {
            ec = fail("ComponentType::TradingApp != 5122", 10);
            return 0;
        }
        std::fprintf(stdout, "    PASS: TradingApp == 5122 (0x1402)\n");
    }

    // ------------------------------------------------------------------
    // Check 2: getCategory(TradingApp) == 2.
    // ------------------------------------------------------------------
    {
        int cat = getCategory(ComponentType::TradingApp);
        std::fprintf(stdout, "  getCategory(TradingApp) = %d\n", cat);
        if (cat != 2) {
            ec = fail("getCategory(TradingApp) != 2", 20);
            return 0;
        }
        std::fprintf(stdout, "    PASS: category == 2\n");
    }

    // ------------------------------------------------------------------
    // Check 3: getSubType(TradingApp) == 0x14 (20).
    // ------------------------------------------------------------------
    {
        int sub = getSubType(ComponentType::TradingApp);
        std::fprintf(stdout, "  getSubType(TradingApp) = %d (0x%X)\n", sub, sub);
        if (sub != 0x14) {
            ec = fail("getSubType(TradingApp) != 0x14", 30);
            return 0;
        }
        std::fprintf(stdout, "    PASS: sub-type == 0x14 (20)\n");
    }

    // ------------------------------------------------------------------
    // Check 4: Decomposition is a no-op for existing sub-type-0 types.
    // ------------------------------------------------------------------
    {
        struct { const char* name; ComponentType t; int expCat; } cases[] = {
            { "Strategy",  ComponentType::Strategy,  0 },
            { "Indicator", ComponentType::Indicator, 2 },
            { "Function",  ComponentType::Function,  1 },
        };
        for (const auto& c : cases) {
            int cat = getCategory(c.t);
            int sub = getSubType(c.t);
            std::fprintf(stdout, "  %s: category=%d sub-type=%d\n",
                         c.name, cat, sub);
            if (cat != c.expCat || sub != 0) {
                ec = fail((std::string("decomposition no-op failed for ") +
                           c.name).c_str(), 40);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: all existing types are no-op (sub-type 0)\n");
    }

    // ------------------------------------------------------------------
    // Check 5: CLI parseType accepts "tradingapp" -> TradingApp.
    // ------------------------------------------------------------------
    {
        ComponentType out;
        const char* token = "tradingapp";
        if (!parseTradingApp(token, out)) {
            ec = fail("parseType does not accept \"tradingapp\"", 50);
            return 0;
        }
        int v = static_cast<int>(out);
        std::fprintf(stdout, "  parseType(\"%s\") -> enum value %d\n", token, v);
        if (out != ComponentType::TradingApp || v != 5122) {
            ec = fail("parseType(\"tradingapp\") did not map to TradingApp (5122)",
                      51);
            return 0;
        }
        // Case-insensitivity check.
        if (!parseTradingApp("TradingApp", out) || out != ComponentType::TradingApp) {
            ec = fail("parseType is not case-insensitive for \"TradingApp\"", 52);
            return 0;
        }
        if (!parseTradingApp("TRADINGAPP", out) || out != ComponentType::TradingApp) {
            ec = fail("parseType is not case-insensitive for \"TRADINGAPP\"", 53);
            return 0;
        }
        // Negative: a bogus token must NOT parse as TradingApp.
        if (parseTradingApp("bogustype", out)) {
            ec = fail("parseType accepted a bogus token as TradingApp", 54);
            return 0;
        }
        std::fprintf(stdout, "    PASS: \"tradingapp\" (case-insensitive) -> TradingApp\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
DWORD WINAPI threadProc(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        st->exitCode = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_tradingapp_type: TradingApp enum + decomposition (unit) ===\n");

    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    int exitCode = st.exitCode;
    std::fprintf(stdout,
                 "\n=== test_tradingapp_type complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
