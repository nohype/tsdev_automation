// test_serve_dispatch.cpp
//
// Unit test (RED-GREEN TDD) for TVS-K.3: serve request dispatch layer.
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// only exercises the new parseServeRequest() helper in src/tsdev_cli.cpp:
//   - parseServeRequest(const ordered_json& req, ServeRequest& out)
//       -> CliResult (usage-error CliResult on malformed request).
//
// Per the TVS-K.3 plan, dispatchServeRequest() is NOT unit-tested here (it
// needs a real TsDevLib, which is concrete and thread-affine — not mockable).
// The dispatch switch is validated via the K.5/K.6a E2E integration path with
// the real TsDevLib. This test focuses exclusively on parseServeRequest()
// (pure logic, no TsDevLib).
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   parseServeRequest() and the ServeRequest struct will live inside an
//   anonymous namespace in src/tsdev_cli.cpp (internal linkage), so they are
//   NOT accessible from a separate translation unit. The only way to reach
//   them is to #include the .cpp directly so the test is part of the SAME
//   translation unit. This pulls in tsdev_cli.cpp's int main(); the test
//   #defines TSDEV_CLI_TEST before the include so main() is excluded (main()
//   is wrapped in #ifndef TSDEV_CLI_TEST at line 1741 of tsdev_cli.cpp).
//
// TDD RED step:
//   - parseServeRequest() and ServeRequest do NOT exist yet in tsdev_cli.cpp.
//   This test is the RED step — it is expected to FAIL TO COMPILE until the
//   implementer (Green step) adds the ServeRequest struct and
//   parseServeRequest() function.
//
// Checks (valid parsing):
//   1. Valid insert: {"op":"insert","type":"strategy","name":"Test",
//      "source":"{ Test } Plot1(Close,\"C\");"} -> op=="insert",
//      type==ComponentType::Strategy, typeStr=="strategy", name=="Test",
//      source contains the EL source.
//   2. Valid exists: {"op":"exists","type":"indicator","name":"MovAvg",
//      "case_insensitive":true} -> op=="exists",
//      type==ComponentType::Indicator, caseInsensitive==true, source empty.
//   3. Valid get-source: {"op":"get-source","type":"function","name":"MyFunc",
//      "output_path":"C:\\tmp\\out.el"} -> op=="get-source",
//      outputPath=="C:\\tmp\\out.el".
//   4. Valid quit: {"op":"quit"} -> op=="quit".
//
// Checks (error cases — malformed requests return a usage-error CliResult):
//   5. Missing op field: {} -> exitCode==1, error.code=="usage_error",
//      error.message contains "op".
//   6. Unknown op: {"op":"foobar"} -> exitCode==1, error.code=="usage_error",
//      error.message contains "unknown op".
//   7. Missing type field: {"op":"insert","name":"Test","source":"..."} ->
//      exitCode==1, error.message contains "type".
//   8. Missing name field: {"op":"insert","type":"strategy","source":"..."} ->
//      exitCode==1, error.message contains "name".
//   9. Missing source field (insert): {"op":"insert","type":"strategy",
//      "name":"Test"} -> exitCode==1, error.message contains "source".
//  10. Unknown type: {"op":"insert","type":"foobar","name":"Test",
//      "source":"..."} -> exitCode==2, error.code=="unknown_type".
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in #ifndef TSDEV_CLI_TEST at line 1741 of tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (parseServeRequest, ServeRequest, errorCodeString, CliResult, ordered_json,
// etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Helper: returns true if `haystack` contains `needle` (case-sensitive).
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_serve_dispatch: parseServeRequest() (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: Valid insert request.
    //   {"op":"insert","type":"strategy","name":"Test",
    //    "source":"{ Test } Plot1(Close,\"C\");"}
    // Expected: parsing succeeds (CliResult.success == true, exitCode == 0),
    //   out.op=="insert", out.type==ComponentType::Strategy,
    //   out.typeStr=="strategy", out.name=="Test", out.source contains the
    //   EL source text.
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\","
            "\"source\":\"{ Test } Plot1(Close,\\\"C\\\");\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [1] valid insert: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check1: parse should succeed (success/exit 0)", 10);
            return 0;
        }
        if (out.op != "insert") {
            ec = fail("check1: out.op != \"insert\"", 11);
            return 0;
        }
        if (out.type != ComponentType::Strategy) {
            ec = fail("check1: out.type != ComponentType::Strategy", 12);
            return 0;
        }
        if (out.typeStr != "strategy") {
            ec = fail("check1: out.typeStr != \"strategy\"", 13);
            return 0;
        }
        if (out.name != "Test") {
            ec = fail("check1: out.name != \"Test\"", 14);
            return 0;
        }
        if (!contains(out.source, "Plot1")) {
            ec = fail("check1: out.source missing EL source text", 15);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: valid insert parsed correctly\n");
    }

    // ------------------------------------------------------------------
    // Check 2: Valid exists request.
    //   {"op":"exists","type":"indicator","name":"MovAvg",
    //    "case_insensitive":true}
    // Expected: out.op=="exists", out.type==ComponentType::Indicator,
    //   out.caseInsensitive==true, out.source is empty.
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"exists\",\"type\":\"indicator\",\"name\":\"MovAvg\","
            "\"case_insensitive\":true}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [2] valid exists: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check2: parse should succeed (success/exit 0)", 20);
            return 0;
        }
        if (out.op != "exists") {
            ec = fail("check2: out.op != \"exists\"", 21);
            return 0;
        }
        if (out.type != ComponentType::Indicator) {
            ec = fail("check2: out.type != ComponentType::Indicator", 22);
            return 0;
        }
        if (out.typeStr != "indicator") {
            ec = fail("check2: out.typeStr != \"indicator\"", 23);
            return 0;
        }
        if (out.name != "MovAvg") {
            ec = fail("check2: out.name != \"MovAvg\"", 24);
            return 0;
        }
        if (out.caseInsensitive != true) {
            ec = fail("check2: out.caseInsensitive != true", 25);
            return 0;
        }
        if (!out.source.empty()) {
            ec = fail("check2: out.source should be empty for exists", 26);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: valid exists parsed correctly\n");
    }

    // ------------------------------------------------------------------
    // Check 3: Valid get-source request.
    //   {"op":"get-source","type":"function","name":"MyFunc",
    //    "output_path":"C:\\tmp\\out.el"}
    // Expected: out.op=="get-source",
    //   out.outputPath=="C:\\tmp\\out.el".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"get-source\",\"type\":\"function\",\"name\":\"MyFunc\","
            "\"output_path\":\"C:\\\\tmp\\\\out.el\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [3] valid get-source: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check3: parse should succeed (success/exit 0)", 30);
            return 0;
        }
        if (out.op != "get-source") {
            ec = fail("check3: out.op != \"get-source\"", 31);
            return 0;
        }
        if (out.type != ComponentType::Function) {
            ec = fail("check3: out.type != ComponentType::Function", 32);
            return 0;
        }
        if (out.typeStr != "function") {
            ec = fail("check3: out.typeStr != \"function\"", 33);
            return 0;
        }
        if (out.name != "MyFunc") {
            ec = fail("check3: out.name != \"MyFunc\"", 34);
            return 0;
        }
        if (out.outputPath != "C:\\tmp\\out.el") {
            ec = fail("check3: out.outputPath != \"C:\\\\tmp\\\\out.el\"", 35);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: valid get-source parsed correctly\n");
    }

    // ------------------------------------------------------------------
    // Check 4: Valid quit request.
    //   {"op":"quit"}
    // Expected: out.op=="quit". (No type/name/source required for quit.)
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse("{\"op\":\"quit\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [4] valid quit: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check4: parse should succeed (success/exit 0)", 40);
            return 0;
        }
        if (out.op != "quit") {
            ec = fail("check4: out.op != \"quit\"", 41);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: valid quit parsed correctly\n");
    }

    // ------------------------------------------------------------------
    // Check 5: Missing op field.
    //   {}
    // Expected: CliResult with exitCode==1, error.code=="usage_error",
    //   error.message contains "op".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse("{}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [5] missing op: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check5: parse should fail (success==false)", 50);
            return 0;
        }
        if (r.exitCode != 1) {
            ec = fail("check5: exitCode != 1 (usage_error)", 51);
            return 0;
        }
        if (errorCodeString(r.exitCode, r.op) != "usage_error") {
            ec = fail("check5: error.code != \"usage_error\"", 52);
            return 0;
        }
        if (!contains(r.errorMessage, "op")) {
            ec = fail("check5: error.message missing \"op\"", 53);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: missing op rejected with usage_error\n");
    }

    // ------------------------------------------------------------------
    // Check 6: Unknown op.
    //   {"op":"foobar"}
    // Expected: CliResult with exitCode==1, error.code=="usage_error",
    //   error.message contains "unknown op".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse("{\"op\":\"foobar\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [6] unknown op: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check6: parse should fail (success==false)", 60);
            return 0;
        }
        if (r.exitCode != 1) {
            ec = fail("check6: exitCode != 1 (usage_error)", 61);
            return 0;
        }
        if (errorCodeString(r.exitCode, r.op) != "usage_error") {
            ec = fail("check6: error.code != \"usage_error\"", 62);
            return 0;
        }
        if (!contains(r.errorMessage, "unknown op")) {
            ec = fail("check6: error.message missing \"unknown op\"", 63);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: unknown op rejected with usage_error\n");
    }

    // ------------------------------------------------------------------
    // Check 7: Missing type field (insert).
    //   {"op":"insert","name":"Test","source":"..."}
    // Expected: CliResult with exitCode==1, error.message contains "type".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"name\":\"Test\",\"source\":\"...\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [7] missing type: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check7: parse should fail (success==false)", 70);
            return 0;
        }
        if (r.exitCode != 1) {
            ec = fail("check7: exitCode != 1 (usage_error)", 71);
            return 0;
        }
        if (!contains(r.errorMessage, "type")) {
            ec = fail("check7: error.message missing \"type\"", 72);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: missing type rejected with usage_error\n");
    }

    // ------------------------------------------------------------------
    // Check 8: Missing name field (insert).
    //   {"op":"insert","type":"strategy","source":"..."}
    // Expected: CliResult with exitCode==1, error.message contains "name".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"source\":\"...\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [8] missing name: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check8: parse should fail (success==false)", 80);
            return 0;
        }
        if (r.exitCode != 1) {
            ec = fail("check8: exitCode != 1 (usage_error)", 81);
            return 0;
        }
        if (!contains(r.errorMessage, "name")) {
            ec = fail("check8: error.message missing \"name\"", 82);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: missing name rejected with usage_error\n");
    }

    // ------------------------------------------------------------------
    // Check 9: Missing source field (insert).
    //   {"op":"insert","type":"strategy","name":"Test"}
    // Expected: CliResult with exitCode==1, error.message contains "source".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [9] missing source: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check9: parse should fail (success==false)", 90);
            return 0;
        }
        if (r.exitCode != 1) {
            ec = fail("check9: exitCode != 1 (usage_error)", 91);
            return 0;
        }
        if (!contains(r.errorMessage, "source")) {
            ec = fail("check9: error.message missing \"source\"", 92);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: missing source rejected with usage_error\n");
    }

    // ------------------------------------------------------------------
    // Check 10: Unknown type.
    //   {"op":"insert","type":"foobar","name":"Test","source":"..."}
    // Expected: CliResult with exitCode==2, error.code=="unknown_type".
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"foobar\",\"name\":\"Test\","
            "\"source\":\"...\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [10] unknown type: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (r.success) {
            ec = fail("check10: parse should fail (success==false)", 100);
            return 0;
        }
        if (r.exitCode != 2) {
            ec = fail("check10: exitCode != 2 (unknown_type)", 101);
            return 0;
        }
        if (errorCodeString(r.exitCode, r.op) != "unknown_type") {
            ec = fail("check10: error.code != \"unknown_type\"", 102);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: unknown type rejected with unknown_type\n");
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
                 "=== test_serve_dispatch: parseServeRequest() (unit) ===\n");

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
                 "\n=== test_serve_dispatch complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
