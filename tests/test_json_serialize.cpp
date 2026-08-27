// test_json_serialize.cpp
//
// Unit test for TVS-K.2: the reusable cliResultToJson() serialization helper
// extracted from emitOutput().
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises the implemented cliResultToJson() helper in src/tsdev_cli.cpp:
//   - cliResultToJson(const CliResult& r) : returns ordered_json matching the
//                                            EXACT structure emitOutput()'s
//                                            JSON branch currently produces.
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   cliResultToJson() lives inside an anonymous namespace in src/tsdev_cli.cpp
//   (internal linkage), so it is NOT accessible from a separate translation
//   unit. The only way to reach it is to #include the .cpp directly so the test
//   is part of the SAME translation unit. This pulls in tsdev_cli.cpp's int
//   main(); the test #defines TSDEV_CLI_TEST before the include so main() is
//   excluded by the TSDEV_CLI_TEST guard in src/tsdev_cli.cpp.
//
// Checks:
//   1. Success insert: CliResult with op="insert", typeStr="strategy",
//      name="Test", success=true, exitCode=0, progId=42,
//      ellPath="C:\\path\\file.ell" -> JSON has keys op, type, name, success,
//      exit_code, prog_id, ell_path with matching values; NO error object.
//   2. Compile error: CliResult with op="insert", success=false, exitCode=8,
//      errorMessage="compilation FAILED", compileErrors="Line 1: error",
//      progId=42 -> JSON has error.code="compile_error", error.message,
//      error.compile_errors, error.prog_id=42.
//   3. Exists found: CliResult with op="exists", exists=true,
//      storedName="TestStrat", progId=42, success=true, exitCode=0 -> JSON
//      has exists=true, stored_name="TestStrat", prog_id=42.
//   4. Get-source: CliResult with op="get-source", sourceText="source code
//      here", success=true, exitCode=0 -> JSON has source="source code here".
//   5. Not found: CliResult with op="exists", exists=false, success=true,
//      exitCode=0, progId=0 -> JSON has exists=false, stored_name="" (empty),
//      and NO prog_id field (prog_id omitted when progId == 0).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (cliResultToJson, errorCodeString, CliResult, ordered_json, etc.) are
// visible.
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

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_json_serialize: cliResultToJson() (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: Success insert.
    //   op="insert", typeStr="strategy", name="Test", success=true,
    //   exitCode=0, progId=42, ellPath="C:\\path\\file.ell"
    // Expected JSON keys: op, type, name, success, exit_code, prog_id,
    //   ell_path. NO error object.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op        = "insert";
        r.typeStr   = "strategy";
        r.name      = "Test";
        r.success   = true;
        r.exitCode  = 0;
        r.progId    = 42;
        r.ellPath   = "C:\\path\\file.ell";

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [1] success insert: %s\n",
                     j.dump().c_str());

        if (!j.contains("op") || j["op"].get<std::string>() != "insert") {
            ec = fail("check1: op != \"insert\"", 10);
            return 0;
        }
        if (!j.contains("type") || j["type"].get<std::string>() != "strategy") {
            ec = fail("check1: type != \"strategy\"", 11);
            return 0;
        }
        if (!j.contains("name") || j["name"].get<std::string>() != "Test") {
            ec = fail("check1: name != \"Test\"", 12);
            return 0;
        }
        if (!j.contains("success") || j["success"].get<bool>() != true) {
            ec = fail("check1: success != true", 13);
            return 0;
        }
        if (!j.contains("exit_code") || j["exit_code"].get<int>() != 0) {
            ec = fail("check1: exit_code != 0", 14);
            return 0;
        }
        if (!j.contains("prog_id") || j["prog_id"].get<int>() != 42) {
            ec = fail("check1: prog_id != 42", 15);
            return 0;
        }
        if (!j.contains("ell_path") ||
            j["ell_path"].get<std::string>() != "C:\\path\\file.ell") {
            ec = fail("check1: ell_path != \"C:\\\\path\\\\file.ell\"", 16);
            return 0;
        }
        if (j.contains("error")) {
            ec = fail("check1: unexpected error object on success", 17);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: success insert JSON structure correct\n");
    }

    // ------------------------------------------------------------------
    // Check 2: Compile error.
    //   op="insert", success=false, exitCode=8,
    //   errorMessage="compilation FAILED", compileErrors="Line 1: error",
    //   progId=42
    // Expected JSON: error.code="compile_error", error.message,
    //   error.compile_errors, error.prog_id=42.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op            = "insert";
        r.success       = false;
        r.exitCode      = 8;
        r.progId        = 42;
        r.errorMessage  = "compilation FAILED";
        r.compileErrors = "Line 1: error";

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [2] compile error: %s\n",
                     j.dump().c_str());

        if (!j.contains("success") || j["success"].get<bool>() != false) {
            ec = fail("check2: success != false", 20);
            return 0;
        }
        if (!j.contains("exit_code") || j["exit_code"].get<int>() != 8) {
            ec = fail("check2: exit_code != 8", 21);
            return 0;
        }
        if (!j.contains("error")) {
            ec = fail("check2: missing error object", 22);
            return 0;
        }
        ordered_json err = j["error"];
        if (!err.contains("code") ||
            err["code"].get<std::string>() != "compile_error") {
            ec = fail("check2: error.code != \"compile_error\"", 23);
            return 0;
        }
        if (!err.contains("message") ||
            err["message"].get<std::string>() != "compilation FAILED") {
            ec = fail("check2: error.message != \"compilation FAILED\"", 24);
            return 0;
        }
        if (!err.contains("compile_errors") ||
            err["compile_errors"].get<std::string>() != "Line 1: error") {
            ec = fail("check2: error.compile_errors != \"Line 1: error\"", 25);
            return 0;
        }
        if (!err.contains("prog_id") || err["prog_id"].get<int>() != 42) {
            ec = fail("check2: error.prog_id != 42", 26);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: compile error JSON structure correct\n");
    }

    // ------------------------------------------------------------------
    // Check 3: Exists found.
    //   op="exists", exists=true, storedName="TestStrat", progId=42,
    //   success=true, exitCode=0
    // Expected JSON: exists=true, stored_name="TestStrat", prog_id=42.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op        = "exists";
        r.exists    = true;
        r.storedName = "TestStrat";
        r.progId    = 42;
        r.success   = true;
        r.exitCode  = 0;

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [3] exists found: %s\n",
                     j.dump().c_str());

        if (!j.contains("exists") || j["exists"].get<bool>() != true) {
            ec = fail("check3: exists != true", 30);
            return 0;
        }
        if (!j.contains("stored_name") ||
            j["stored_name"].get<std::string>() != "TestStrat") {
            ec = fail("check3: stored_name != \"TestStrat\"", 31);
            return 0;
        }
        if (!j.contains("prog_id") || j["prog_id"].get<int>() != 42) {
            ec = fail("check3: prog_id != 42", 32);
            return 0;
        }
        if (j.contains("error")) {
            ec = fail("check3: unexpected error object on success", 33);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: exists found JSON structure correct\n");
    }

    // ------------------------------------------------------------------
    // Check 4: Get-source.
    //   op="get-source", sourceText="source code here", success=true,
    //   exitCode=0
    // Expected JSON: source="source code here". No prog_id (progId == 0).
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op         = "get-source";
        r.sourceText = "source code here";
        r.success    = true;
        r.exitCode   = 0;

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [4] get-source: %s\n",
                     j.dump().c_str());

        if (!j.contains("source") ||
            j["source"].get<std::string>() != "source code here") {
            ec = fail("check4: source != \"source code here\"", 40);
            return 0;
        }
        if (j.contains("prog_id")) {
            ec = fail("check4: unexpected prog_id (progId == 0)", 41);
            return 0;
        }
        if (j.contains("error")) {
            ec = fail("check4: unexpected error object on success", 42);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: get-source JSON structure correct\n");
    }

    // ------------------------------------------------------------------
    // Check 5: Not found (exists op, exists=false, progId=0).
    //   op="exists", exists=false, success=true, exitCode=0, progId=0
    // Expected JSON: exists=false, stored_name="" (empty string, always
    //   emitted for exists op), NO prog_id field (omitted when progId == 0).
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op        = "exists";
        r.exists    = false;
        r.success   = true;
        r.exitCode  = 0;
        r.progId    = 0;

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [5] not found: %s\n",
                     j.dump().c_str());

        if (!j.contains("exists") || j["exists"].get<bool>() != false) {
            ec = fail("check5: exists != false", 50);
            return 0;
        }
        if (!j.contains("stored_name") ||
            j["stored_name"].get<std::string>() != "") {
            ec = fail("check5: stored_name != \"\" (empty)", 51);
            return 0;
        }
        if (j.contains("prog_id")) {
            ec = fail("check5: unexpected prog_id (progId == 0)", 52);
            return 0;
        }
        if (j.contains("error")) {
            ec = fail("check5: unexpected error object on success", 53);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: not found JSON structure correct\n");
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
                 "=== test_json_serialize: cliResultToJson() (unit) ===\n");

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
                 "\n=== test_json_serialize complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
