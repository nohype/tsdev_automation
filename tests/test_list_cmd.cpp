// tests/test_list_cmd.cpp
//
// Unit tests for the CLI-level `list <type>` output shaping in
// src/tsdev_cli.cpp:
//   - listHumanStdout(names) : the pure human-mode stdout renderer (one
//     stored name per line; an empty result renders NOTHING - empty string).
//   - cliResultToJson(r) "list" success shape: {"op":"list","type":<type>,
//     "success":true,"exit_code":0,"names":[...]} - "names" is ALWAYS
//     present on success (empty array when the category has zero items) and
//     entries keep the given order (no sorting, no dedup).
//   - cliResultToJson(r) "list" failure shape: the standard error object,
//     with NO "names" key.
//   - sortNamesForOutput(names) : THE canonical "list" output ordering,
//     applied exactly once in doList before the names reach the renderers:
//     case-insensitive ASCII comparison (primary key = the name lowercased
//     with ASCII tolower semantics), STABLE ties (names equal ignoring case
//     keep their source order), NO dedup. The renderers above stay
//     order-preserving passthroughs (check 1/3 pin that they do NOT
//     re-sort).
//
// The main() dispatch arg-validation for `list` is wrapped in the
// #ifndef TSDEV_CLI_TEST guard in src/tsdev_cli.cpp and cannot be reached
// from a unit-test TU (and cmdList/runList/doList need a live TsDevLib: TS +
// elevation). This test therefore covers the pure output-shaping helpers
// only.
//
// This is a PURE UNIT TEST - it does NOT load orlib20.dll, does NOT touch the
// warehouse, and runs under -NoTs. listHumanStdout() and cliResultToJson()
// live inside an anonymous namespace in src/tsdev_cli.cpp (internal linkage),
// so the test #includes the .cpp directly (same TU) to reach them. This pulls
// in tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST before the
// include so main() is excluded by the TSDEV_CLI_TEST guard in
// src/tsdev_cli.cpp.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (listHumanStdout, sortNamesForOutput, cliResultToJson, CliResult,
// ordered_json, etc.) are
// visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Comma-joined names for failure diagnostics ("[a,b,c]").
std::string joinNames(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += v[i];
    }
    s += "]";
    return s;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_list_cmd: list output shaping (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: listHumanStdout with several names -> one name per line,
    //   input order preserved (no sorting, no dedup).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "Zeta", "Alpha", "Alpha" };
        const std::string out = listHumanStdout(names);
        std::fprintf(stdout, "  [1] listHumanStdout(3 names): %s\n",
                     out.c_str());
        if (out != "Zeta\nAlpha\nAlpha\n") {
            ec = fail("check1: names not rendered one-per-line in order", 10);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 2: listHumanStdout with ZERO names -> empty string (an empty
    //   result prints NOTHING).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names;
        const std::string out = listHumanStdout(names);
        std::fprintf(stdout, "  [2] listHumanStdout(0 names): \"%s\"\n",
                     out.c_str());
        if (!out.empty()) {
            ec = fail("check2: empty result must render NOTHING", 20);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 3: cliResultToJson list success (3 names): exact pinned shape
    //   {"op":"list","type":"strategy","success":true,"exit_code":0,
    //    "names":["Zeta","Alpha","Alpha"]}.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "strategy";
        r.success  = true;
        r.exitCode = 0;
        r.names    = { "Zeta", "Alpha", "Alpha" };

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [3] list success (3 names): %s\n",
                     j.dump().c_str());
        const std::string want =
            "{\"op\":\"list\",\"type\":\"strategy\",\"success\":true,"
            "\"exit_code\":0,\"names\":[\"Zeta\",\"Alpha\",\"Alpha\"]}";
        if (j.dump() != want) {
            ec = fail("check3: list success JSON shape mismatch", 30);
            return 0;
        }
        if (j.contains("name")) {
            ec = fail("check3: list JSON must not carry a name key", 31);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 4: cliResultToJson list success (0 names): "names" is STILL
    //   present and is an empty array.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "showme";
        r.success  = true;
        r.exitCode = 0;

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [4] list success (0 names): %s\n",
                     j.dump().c_str());
        if (!j.contains("names") || !j["names"].is_array() ||
            !j["names"].empty()) {
            ec = fail("check4: names must be an empty array when zero items",
                      40);
            return 0;
        }
        const std::string want =
            "{\"op\":\"list\",\"type\":\"showme\",\"success\":true,"
            "\"exit_code\":0,\"names\":[]}";
        if (j.dump() != want) {
            ec = fail("check4: empty-names JSON shape mismatch", 41);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 5: cliResultToJson list failure (exit 4): the standard error
    //   object with code/message and NO "names" key.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "list";
        r.typeStr  = "indicator";
        r.success  = false;
        r.exitCode = 4;
        r.errorMessage = "init failed: could not enumerate items";

        ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [5] list failure (exit 4): %s\n",
                     j.dump().c_str());
        if (j.contains("names")) {
            ec = fail("check5: failure JSON must not carry a names key", 50);
            return 0;
        }
        if (!j.contains("error") || !j["error"].is_object()) {
            ec = fail("check5: failure JSON missing error object", 51);
            return 0;
        }
        if (j["error"]["code"].get<std::string>() != "init_failure") {
            ec = fail("check5: error.code != \"init_failure\"", 52);
            return 0;
        }
        if (j["error"]["message"].get<std::string>() !=
            "init failed: could not enumerate items") {
            ec = fail("check5: error.message mismatch", 53);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 6: sortNamesForOutput mixed case -> case-insensitive ASCII
    //   order with STABLE ties ("Alpha" before "ALPHA": source order kept
    //   for names equal ignoring case).
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "beta", "Alpha", "ALPHA", "zeta" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "Alpha", "ALPHA", "beta",
                                                "zeta" };
        std::fprintf(stdout, "  [6] sortNamesForOutput(mixed case): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check6: mixed-case sort order mismatch", 60);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 7: sortNamesForOutput equal-ignoring-case run -> stays in
    //   source order (stable ties, no reordering among "aB"/"Ab"/"AB").
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "aB", "Ab", "AB" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "aB", "Ab", "AB" };
        std::fprintf(stdout, "  [7] sortNamesForOutput(equal-ignoring-case "
                             "run): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check7: stable tie order not preserved", 70);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 8: sortNamesForOutput duplicates -> preserved (NO dedup);
    //   the three "dup"-keyed names keep source order among themselves.
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "Dup", "dup", "Other", "Dup" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "Dup", "dup", "Dup",
                                                "Other" };
        std::fprintf(stdout, "  [8] sortNamesForOutput(duplicates): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check8: duplicates not preserved in stable order", 80);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 9: sortNamesForOutput on an empty vector and a single element
    //   -> unchanged.
    // ------------------------------------------------------------------
    {
        std::vector<std::string> empty;
        sortNamesForOutput(empty);
        std::vector<std::string> one = { "Solo" };
        sortNamesForOutput(one);
        std::fprintf(stdout, "  [9] sortNamesForOutput(0/1 names): %s %s\n",
                     joinNames(empty).c_str(), joinNames(one).c_str());
        if (!empty.empty()) {
            ec = fail("check9: empty vector must stay empty", 90);
            return 0;
        }
        if (one != std::vector<std::string>{ "Solo" }) {
            ec = fail("check9: single element must stay unchanged", 91);
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // Check 10: sortNamesForOutput ASCII punctuation/digit ordering ->
    //   byte order after ASCII-lowercasing ('%' 0x25 < '1' 0x31 < 'a'
    //   0x61): ["%Chg","alpha","10x"] -> ["%Chg","10x","alpha"].
    // ------------------------------------------------------------------
    {
        std::vector<std::string> names = { "%Chg", "alpha", "10x" };
        sortNamesForOutput(names);
        const std::vector<std::string> want = { "%Chg", "10x", "alpha" };
        std::fprintf(stdout, "  [10] sortNamesForOutput(punct/digit): %s\n",
                     joinNames(names).c_str());
        if (names != want) {
            std::fprintf(stderr, "  got %s want %s\n",
                         joinNames(names).c_str(), joinNames(want).c_str());
            ec = fail("check10: ASCII byte order mismatch", 100);
            return 0;
        }
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper - POD-only (no C++ objects with destructors) so __try is legal.
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
                 "=== test_list_cmd: list output shaping (unit) ===\n");

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
                 "\n=== test_list_cmd complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}