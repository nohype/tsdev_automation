// test_serve_dispatch.cpp
//
// Unit test for TVS-K.3: serve request dispatch layer.
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises the implemented parseServeRequest() helper in src/tsdev_cli.cpp:
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
//   parseServeRequest() and the ServeRequest struct live inside an anonymous
//   namespace in src/tsdev_cli.cpp (internal linkage), so they are NOT
//   accessible from a separate translation unit. The only way to reach them is
//   to #include the .cpp directly so the test is part of the SAME translation
//   unit. This pulls in tsdev_cli.cpp's int main(); the test #defines
//   TSDEV_CLI_TEST before the include so main() is excluded by the
//   TSDEV_CLI_TEST guard in src/tsdev_cli.cpp.
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
// Checks (TVS A batch metadata):
//   These exercise the optional nested "batch" member and the
//   addServeProgress() response enrichment. The ServeRequest batch fields
//   asserted here are: bool hasBatch; std::string batchId;
//   size_t batchIndex; size_t batchTotal (populated only for tagged
//   insert/modify/delete/upsert requests).
//  11. Tagged insert accepted: batch {"id":"b-1","index":1,"total":3} ->
//      parse succeeds, hasBatch==true, batchId/batchIndex/batchTotal set.
//  12. Tagged modify/delete/upsert accepted (boundary index==total).
//  13. "batch" on non-mutating ops (exists/get-source/quit) -> rejected
//      exitCode==1 through the existing parse-failure path.
//  14. Malformed "batch" rejected exitCode==1: non-object (incl. null),
//      missing or extra member, empty/null/non-string id, index/total
//      0/negative/null/bool/fraction/string, index>total.
//  15. Untagged request unchanged: parses with hasBatch==false.
//  16. addServeProgress(ordered_json& response, const ServeRequest& req):
//      tagged req -> response gains exactly one new top-level member
//      "progress" equal to
//      {"batch_id":id,"index":i,"total":t,"phase":"completed"}; untagged req
//      -> response byte-identical (no "progress", no other envelope).
//  17. Tagged parse failures keep the progress data: a tagged request that
//      fails AFTER 'batch' validated (unknown type / missing source) leaves
//      out.hasBatch/batchIndex/batchTotal populated and the failure
//      CliResult carrying op/typeStr/name as far as validation reached;
//      taggedParseFailureLines renders the exact started/completed pair
//      (human + JSON). Invalid 'batch' metadata itself -> hasBatch==false
//      -> empty pair (untagged failure, no progress).
//  18. 32-bit index/total boundaries: 4294967295 (UINT32_MAX) accepted when
//      index<=total; 4294967296 rejected for index and for total (the
//      binary is built Win32, size_t is 32-bit).
//
// Checks (serve cache ops — backup-cache/restore-cache):
//  19. Both ops parse with no type/name/source (quit-style early-out);
//      absent "policy" -> out.policy "abort" on backup-cache (serve
//      default; the broker has no console to prompt), empty on
//      restore-cache.
//  20. "policy" on backup-cache: skip/overwrite/abort accepted
//      (case-insensitive, verbatim into out.policy); "prompt" rejected
//      exit 1 (broker has no console); unknown/non-string values rejected.
//  21. "policy" rejected exit 1 on non-backup-cache ops (quit/restore-cache/
//      exists/insert).
//  22. "batch" metadata rejected exit 1 on cache ops (mutating-op gate —
//      cache ops are not WHServer-mutating).
//  23. CliResult::extraPayload merge: success CliResult carrying the
//      standalone --json payload serializes its members at top level via
//      cliResultToJson (the seam dispatchServeRequest returns through).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
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

    // ------------------------------------------------------------------
    // Check 11 (TVS A): tagged insert request accepted.
    //   {"op":"insert","type":"strategy","name":"Test","source":"x",
    //    "batch":{"id":"b-1","index":1,"total":3}}
    // Expected: parse succeeds; out.hasBatch==true, out.batchId=="b-1",
    //   out.batchIndex==1, out.batchTotal==3.
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b-1\",\"index\":1,"
            "\"total\":3}}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [11] tagged insert: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check11: tagged insert should parse", 110);
            return 0;
        }
        if (!out.hasBatch) {
            ec = fail("check11: out.hasBatch != true", 111);
            return 0;
        }
        if (out.batchId != "b-1") {
            ec = fail("check11: out.batchId != \"b-1\"", 112);
            return 0;
        }
        if (out.batchIndex != 1 || out.batchTotal != 3) {
            ec = fail("check11: out.batchIndex/batchTotal != 1/3", 113);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: tagged insert parsed with batch metadata\n");
    }

    // ------------------------------------------------------------------
    // Check 12 (TVS A): batch metadata accepted on all mutating ops
    //   (insert covered by check 11). Boundary case index==total is valid.
    //   {"op":<modify|delete|upsert>,...,"batch":{"id":"b-2","index":3,
    //    "total":3}}
    // Expected: parse succeeds; out.hasBatch==true, batchIndex==3,
    //   batchTotal==3.
    // ------------------------------------------------------------------
    {
        const char* ops[] = {"modify", "delete", "upsert"};
        for (int i = 0; i < 3; ++i) {
            const std::string op = ops[i];
            const std::string sourceMember =
                (op == "delete") ? "" : ",\"source\":\"x\"";
            ordered_json req = ordered_json::parse(
                std::string("{\"op\":\"") + op +
                "\",\"type\":\"strategy\",\"name\":\"T\"" + sourceMember +
                ",\"batch\":{\"id\":\"b-2\",\"index\":3,\"total\":3}}");

            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [12] tagged %s: %s\n", op.c_str(),
                         cliResultToJson(r).dump().c_str());

            if (!r.success || r.exitCode != 0) {
                ec = fail("check12: tagged mutating op should parse", 120);
                return 0;
            }
            if (!out.hasBatch || out.batchId != "b-2" ||
                out.batchIndex != 3 || out.batchTotal != 3) {
                ec = fail("check12: batch metadata not populated", 121);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: tagged modify/delete/upsert accepted\n");
    }

    // ------------------------------------------------------------------
    // Check 13 (TVS A): "batch" is accepted only on
    //   insert/modify/delete/upsert. On non-mutating ops the whole request
    //   is rejected through the existing parse-failure path (exitCode==1).
    //   exists / get-source / quit each carry a valid batch object.
    // ------------------------------------------------------------------
    {
        const char* reqs[] = {
            "{\"op\":\"exists\",\"type\":\"indicator\",\"name\":\"X\","
            "\"batch\":{\"id\":\"b\",\"index\":1,\"total\":3}}",
            "{\"op\":\"get-source\",\"type\":\"function\",\"name\":\"X\","
            "\"batch\":{\"id\":\"b\",\"index\":1,\"total\":3}}",
            "{\"op\":\"quit\","
            "\"batch\":{\"id\":\"b\",\"index\":1,\"total\":3}}",
        };
        for (int i = 0; i < 3; ++i) {
            ordered_json req = ordered_json::parse(reqs[i]);
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [13.%d] batch on non-mutating op: %s\n",
                         i, cliResultToJson(r).dump().c_str());
            if (r.success) {
                ec = fail("check13: batch on non-mutating op must reject",
                          130);
                return 0;
            }
            if (r.exitCode != 1) {
                ec = fail("check13: exitCode != 1", 131);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: batch on non-mutating ops rejected\n");
    }

    // ------------------------------------------------------------------
    // Check 14 (TVS A): malformed "batch" metadata rejects the whole
    //   request (exitCode==1). Each fragment below is substituted verbatim
    //   for the "batch" member of an otherwise-valid tagged insert request.
    // ------------------------------------------------------------------
    {
        struct BadBatch { const char* desc; const char* json; };
        const BadBatch bad[] = {
            {"non-object (bool)",     "true"},
            {"non-object (string)",   "\"abc\""},
            {"non-object (array)",    "[1,2,3]"},
            {"non-object (null)",     "null"},
            {"non-object (number)",   "7"},
            {"missing id",            "{\"index\":1,\"total\":3}"},
            {"missing index",         "{\"id\":\"b\",\"total\":3}"},
            {"missing total",         "{\"id\":\"b\",\"index\":1}"},
            {"extra member",          "{\"id\":\"b\",\"index\":1,\"total\":3,"
                                      "\"seq\":0}"},
            {"empty id",              "{\"id\":\"\",\"index\":1,\"total\":3}"},
            {"null id",               "{\"id\":null,\"index\":1,\"total\":3}"},
            {"bool id",               "{\"id\":true,\"index\":1,\"total\":3}"},
            {"non-string id",         "{\"id\":7,\"index\":1,\"total\":3}"},
            {"null index",            "{\"id\":\"b\",\"index\":null,\"total\":3}"},
            {"index 0",               "{\"id\":\"b\",\"index\":0,\"total\":3}"},
            {"index negative",        "{\"id\":\"b\",\"index\":-1,\"total\":3}"},
            {"total 0",               "{\"id\":\"b\",\"index\":1,\"total\":0}"},
            {"total negative",        "{\"id\":\"b\",\"index\":1,\"total\":-1}"},
            {"null total",            "{\"id\":\"b\",\"index\":1,\"total\":null}"},
            {"index > total",         "{\"id\":\"b\",\"index\":4,\"total\":3}"},
            {"index bool",            "{\"id\":\"b\",\"index\":true,\"total\":3}"},
            {"index fraction",        "{\"id\":\"b\",\"index\":1.5,\"total\":3}"},
            {"index string",          "{\"id\":\"b\",\"index\":\"1\",\"total\":3}"},
            {"total bool",            "{\"id\":\"b\",\"index\":1,\"total\":true}"},
            {"total fraction",        "{\"id\":\"b\",\"index\":1,\"total\":3.5}"},
            {"total string",          "{\"id\":\"b\",\"index\":1,\"total\":\"3\"}"},
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            ordered_json req = ordered_json::parse(
                std::string("{\"op\":\"insert\",\"type\":\"strategy\","
                            "\"name\":\"T\",\"source\":\"x\",\"batch\":") +
                bad[i].json + "}");
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [14.%u] %s: %s\n",
                         static_cast<unsigned>(i), bad[i].desc,
                         cliResultToJson(r).dump().c_str());
            if (r.success) {
                ec = fail("check14: malformed batch must reject", 140);
                return 0;
            }
            if (r.exitCode != 1) {
                ec = fail("check14: exitCode != 1", 141);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: malformed batch metadata rejected\n");
    }

    // ------------------------------------------------------------------
    // Check 15 (TVS A): untagged request unchanged — a valid insert
    //   with no "batch" member parses exactly as before and reports
    //   hasBatch==false.
    // ------------------------------------------------------------------
    {
        ordered_json req = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\","
            "\"source\":\"x\"}");

        ServeRequest out;
        CliResult r = parseServeRequest(req, out);
        std::fprintf(stdout, "  [15] untagged insert: %s\n",
                     cliResultToJson(r).dump().c_str());

        if (!r.success || r.exitCode != 0) {
            ec = fail("check15: untagged insert should parse", 150);
            return 0;
        }
        if (out.hasBatch) {
            ec = fail("check15: out.hasBatch != false on untagged request",
                      151);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: untagged request unchanged (hasBatch false)\n");
    }

    // ------------------------------------------------------------------
    // Check 16 (TVS A):
    //   addServeProgress(ordered_json& response, const ServeRequest& req)
    //   enriches the ordinary response frame for tagged requests only.
    //   (a) tagged req -> response gains exactly ONE new top-level member
    //       "progress" equal to {"batch_id":"b-9","index":2,"total":3,
    //       "phase":"completed"}; every other member is unchanged (no other
    //       top-level envelope/frame fields).
    //   (b) untagged req -> response is byte-identical (no "progress").
    // ------------------------------------------------------------------
    {
        ordered_json reqJson = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b-9\",\"index\":2,"
            "\"total\":3}}");
        ServeRequest tagged;
        CliResult pr = parseServeRequest(reqJson, tagged);
        if (!pr.success || !tagged.hasBatch) {
            ec = fail("check16: tagged setup request failed to parse", 160);
            return 0;
        }

        CliResult res;
        res.op       = "insert";
        res.typeStr  = "strategy";
        res.name     = "Test";
        res.success  = true;
        res.exitCode = 0;
        res.progId   = 42;

        ordered_json resp = cliResultToJson(res);
        ordered_json orig = resp;
        addServeProgress(resp, tagged);

        if (!resp.contains("progress") || !resp["progress"].is_object() ||
            resp["progress"].size() != 4) {
            ec = fail("check16a: progress missing or not a 4-member object",
                      161);
            return 0;
        }
        const ordered_json& prog = resp["progress"];
        if (!prog.contains("batch_id") ||
            prog["batch_id"].get<std::string>() != "b-9") {
            ec = fail("check16a: progress.batch_id != \"b-9\"", 162);
            return 0;
        }
        if (!prog.contains("index") || !prog["index"].is_number_integer() ||
            prog["index"].get<size_t>() != 2) {
            ec = fail("check16a: progress.index != 2 (integer)", 163);
            return 0;
        }
        if (!prog.contains("total") || !prog["total"].is_number_integer() ||
            prog["total"].get<size_t>() != 3) {
            ec = fail("check16a: progress.total != 3 (integer)", 164);
            return 0;
        }
        if (!prog.contains("phase") ||
            prog["phase"].get<std::string>() != "completed") {
            ec = fail("check16a: progress.phase != \"completed\"", 165);
            return 0;
        }
        resp.erase("progress");
        if (resp != orig) {
            ec = fail("check16a: addServeProgress altered other top-level "
                      "members", 166);
            return 0;
        }

        // (b) untagged: the response must be byte-identical.
        ordered_json plainJson = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"Test\","
            "\"source\":\"x\"}");
        ServeRequest plain;
        CliResult pr2 = parseServeRequest(plainJson, plain);
        if (!pr2.success || plain.hasBatch) {
            ec = fail("check16b: untagged setup request failed to parse",
                      167);
            return 0;
        }
        ordered_json resp2 = cliResultToJson(res);
        const std::string before = resp2.dump();
        addServeProgress(resp2, plain);
        if (resp2.contains("progress") || resp2.dump() != before) {
            ec = fail("check16b: untagged response not byte-identical", 168);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: addServeProgress enriches tagged only\n");
    }

    // ------------------------------------------------------------------
    // Check 17 (TVS A): tagged parse failures keep the progress data the
    //   serve loop needs. When 'batch' validated but a later step fails,
    //   out.hasBatch/batchIndex/batchTotal are populated and the failure
    //   CliResult carries op/typeStr/name as far as validation reached —
    //   taggedParseFailureLines(r, out, ...) must render the exact
    //   started/completed pair. When 'batch' itself is invalid,
    //   hasBatch==false and the helper returns an empty pair (untagged
    //   failure -> no progress).
    // ------------------------------------------------------------------
    {
        // (a) tagged request, unknown type: fails exit 2 AFTER batch
        //   metadata was accepted. r has op/typeStr; r.name stays empty
        //   (validation stopped at the type step).
        ordered_json reqJson = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"bogus\",\"name\":\"T\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b-f\",\"index\":2,"
            "\"total\":3}}");
        ServeRequest out;
        CliResult r = parseServeRequest(reqJson, out);
        std::fprintf(stdout, "  [17a] tagged unknown type: %s\n",
                     cliResultToJson(r).dump().c_str());
        if (r.success || r.exitCode != 2) {
            ec = fail("check17a: tagged unknown type should fail exit 2",
                      170);
            return 0;
        }
        if (!out.hasBatch || out.batchId != "b-f" ||
            out.batchIndex != 2 || out.batchTotal != 3) {
            ec = fail("check17a: batch metadata not populated on parse "
                      "failure", 171);
            return 0;
        }
        if (r.op != "insert" || r.typeStr != "bogus" || !r.name.empty()) {
            ec = fail("check17a: CliResult op/typeStr/name not as expected",
                      172);
            return 0;
        }
        {
            const auto lines = taggedParseFailureLines(r, out, false);
            const std::string expStart =
                "BATCH START [2/3] op=insert type=\"bogus\" name=\"\"";
            const std::string expDone =
                "BATCH FAILED [2/3] op=insert type=\"bogus\" name=\"\""
                " exit_code=2 message=\"unknown type 'bogus'\"";
            if (lines.first != expStart || lines.second != expDone) {
                ec = fail("check17a: human progress pair mismatch", 173);
                return 0;
            }
            const auto jlines = taggedParseFailureLines(r, out, true);
            ordered_json js = ordered_json::parse(jlines.first);
            ordered_json jc = ordered_json::parse(jlines.second);
            if (js["event"] != "batch_item_started" ||
                js["batch_operation"] != "insert" ||
                js["index"] != 2 || js["total"] != 3 ||
                js["type"] != "bogus" || js["name"] != "") {
                ec = fail("check17a: JSON started event mismatch", 174);
                return 0;
            }
            if (jc["event"] != "batch_item_completed" ||
                jc["success"] != false || jc["exit_code"] != 2 ||
                jc["result"] != cliResultToJson(r)) {
                ec = fail("check17a: JSON completed event mismatch", 175);
                return 0;
            }
        }

        // (b) tagged request, missing source: fails exit 1 after type and
        //   name were validated, so r carries all three fields.
        ordered_json reqJson2 = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"batch\":{\"id\":\"b-s\",\"index\":1,\"total\":2}}");
        ServeRequest out2;
        CliResult r2 = parseServeRequest(reqJson2, out2);
        std::fprintf(stdout, "  [17b] tagged missing source: %s\n",
                     cliResultToJson(r2).dump().c_str());
        if (r2.success || r2.exitCode != 1) {
            ec = fail("check17b: tagged missing source should fail exit 1",
                      176);
            return 0;
        }
        if (!out2.hasBatch || out2.batchIndex != 1 || out2.batchTotal != 2) {
            ec = fail("check17b: batch metadata not populated on parse "
                      "failure", 177);
            return 0;
        }
        if (r2.op != "insert" || r2.typeStr != "strategy" ||
            r2.name != "T") {
            ec = fail("check17b: CliResult op/typeStr/name not as expected",
                      178);
            return 0;
        }
        {
            const auto lines = taggedParseFailureLines(r2, out2, false);
            const std::string expStart =
                "BATCH START [1/2] op=insert type=\"strategy\" name=\"T\"";
            const std::string expDone =
                "BATCH FAILED [1/2] op=insert type=\"strategy\" name=\"T\""
                " exit_code=1 message=\"op 'insert' requires 'source'\"";
            if (lines.first != expStart || lines.second != expDone) {
                ec = fail("check17b: human progress pair mismatch", 179);
                return 0;
            }
        }

        // (c) invalid 'batch' metadata itself: hasBatch stays false, so
        //   the helper returns an empty pair — an untagged parse failure
        //   emits no progress.
        ordered_json reqJson3 = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b\",\"index\":4,"
            "\"total\":3}}");
        ServeRequest out3;
        CliResult r3 = parseServeRequest(reqJson3, out3);
        std::fprintf(stdout, "  [17c] invalid batch metadata: %s\n",
                     cliResultToJson(r3).dump().c_str());
        if (r3.success || r3.exitCode != 1) {
            ec = fail("check17c: invalid batch should fail exit 1", 180);
            return 0;
        }
        if (out3.hasBatch) {
            ec = fail("check17c: hasBatch must stay false on invalid "
                      "metadata", 181);
            return 0;
        }
        {
            const auto lines = taggedParseFailureLines(r3, out3, false);
            if (!lines.first.empty() || !lines.second.empty()) {
                ec = fail("check17c: untagged failure must render no "
                          "progress", 182);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: tagged parse failures keep progress data\n");
    }

    // ------------------------------------------------------------------
    // Check 18 (TVS A): 32-bit index/total boundaries. This binary is
    //   built Win32 (size_t 32-bit), so 4294967295 (UINT32_MAX) is the
    //   largest accepted value; 4294967296 must reject for either member.
    // ------------------------------------------------------------------
    {
        ordered_json reqJson = ordered_json::parse(
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b-max\",\"index\":"
            "4294967295,\"total\":4294967295}}");
        ServeRequest out;
        CliResult r = parseServeRequest(reqJson, out);
        std::fprintf(stdout, "  [18a] UINT32_MAX boundary: %s\n",
                     cliResultToJson(r).dump().c_str());
        if (!r.success || r.exitCode != 0) {
            ec = fail("check18a: index==total==4294967295 should parse",
                      190);
            return 0;
        }
        if (!out.hasBatch || out.batchIndex != 4294967295u ||
            out.batchTotal != 4294967295u) {
            ec = fail("check18a: 4294967295 not stored in size_t fields",
                      191);
            return 0;
        }
        const char* over[] = {
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b\",\"index\":"
            "4294967296,\"total\":4294967296}}",
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"source\":\"x\",\"batch\":{\"id\":\"b\",\"index\":1,"
            "\"total\":4294967296}}",
        };
        for (int i = 0; i < 2; ++i) {
            ordered_json rq = ordered_json::parse(over[i]);
            ServeRequest o;
            CliResult rr = parseServeRequest(rq, o);
            std::fprintf(stdout, "  [18b.%d] 2^32 rejected: %s\n", i,
                         cliResultToJson(rr).dump().c_str());
            if (rr.success || rr.exitCode != 1) {
                ec = fail("check18b: 4294967296 must reject exit 1", 192);
                return 0;
            }
            if (o.hasBatch) {
                ec = fail("check18b: hasBatch must stay false on overflow",
                          193);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: 32-bit index/total boundaries enforced\n");
    }

    // ------------------------------------------------------------------
    // Check 19: Serve cache ops parse with no type/name/source.
    //   {"op":"backup-cache"} and {"op":"restore-cache"}
    // Expected: parse succeeds, out.op set, absent "policy" resolves to
    //   out.policy "abort" on backup-cache (the serve default — the broker
    //   has no console) and stays empty on restore-cache; no type/name
    //   required (same early-out pattern as quit).
    // ------------------------------------------------------------------
    {
        const char* ops[] = {"backup-cache", "restore-cache"};
        const char* wantPolicy[] = {"abort", ""};
        for (int i = 0; i < 2; ++i) {
            ordered_json req = ordered_json::parse(
                std::string("{\"op\":\"") + ops[i] + "\"}");
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [19.%d] %s: %s\n", i, ops[i],
                         cliResultToJson(r).dump().c_str());
            if (!r.success || r.exitCode != 0) {
                ec = fail("check19: cache op should parse", 200 + i);
                return 0;
            }
            if (out.op != ops[i]) {
                ec = fail("check19: out.op mismatch", 202 + i);
                return 0;
            }
            if (out.policy != wantPolicy[i]) {
                ec = fail("check19: out.policy default mismatch", 204 + i);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: cache ops parse with no type/name/source\n");
    }

    // ------------------------------------------------------------------
    // Check 20: 'policy' field on backup-cache.
    //   skip/overwrite/abort accepted (case-insensitive); "prompt" rejected
    //   exit 1 (broker has no console); unknown value rejected; non-string
    //   rejected. Accepted values land verbatim in out.policy.
    // ------------------------------------------------------------------
    {
        const char* ok[] = {"skip", "overwrite", "abort", "Overwrite"};
        for (int i = 0; i < 4; ++i) {
            ordered_json req = ordered_json::parse(
                std::string("{\"op\":\"backup-cache\",\"policy\":\"") +
                ok[i] + "\"}");
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [20a.%d] policy=%s: %s\n", i, ok[i],
                         cliResultToJson(r).dump().c_str());
            if (!r.success || r.exitCode != 0) {
                ec = fail("check20a: valid policy should parse", 210 + i);
                return 0;
            }
            if (out.policy != ok[i]) {
                ec = fail("check20a: out.policy mismatch", 214 + i);
                return 0;
            }
        }
        // "prompt" is deliberately rejected: the broker has no console.
        {
            ordered_json req = ordered_json::parse(
                "{\"op\":\"backup-cache\",\"policy\":\"prompt\"}");
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [20b] policy=prompt: %s\n",
                         cliResultToJson(r).dump().c_str());
            if (r.success || r.exitCode != 1) {
                ec = fail("check20b: policy=prompt must reject exit 1", 218);
                return 0;
            }
            if (!contains(r.errorMessage, "policy")) {
                ec = fail("check20b: message missing 'policy'", 219);
                return 0;
            }
        }
        const char* bad[] = {
            "{\"op\":\"backup-cache\",\"policy\":\"bogus\"}",
            "{\"op\":\"backup-cache\",\"policy\":7}",
            "{\"op\":\"backup-cache\",\"policy\":true}",
            "{\"op\":\"backup-cache\",\"policy\":null}",
        };
        for (int i = 0; i < 4; ++i) {
            ordered_json req = ordered_json::parse(bad[i]);
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [20c.%d] bad policy: %s\n", i,
                         cliResultToJson(r).dump().c_str());
            if (r.success || r.exitCode != 1) {
                ec = fail("check20c: invalid policy must reject exit 1",
                          220 + i);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: policy validated (skip/overwrite/abort; "
                     "prompt rejected)\n");
    }

    // ------------------------------------------------------------------
    // Check 21: 'policy' rejected on non-backup-cache ops.
    //   quit, restore-cache, exists, insert -> exitCode==1.
    // ------------------------------------------------------------------
    {
        const char* bad[] = {
            "{\"op\":\"quit\",\"policy\":\"skip\"}",
            "{\"op\":\"restore-cache\",\"policy\":\"skip\"}",
            "{\"op\":\"exists\",\"type\":\"strategy\",\"name\":\"T\","
            "\"policy\":\"skip\"}",
            "{\"op\":\"insert\",\"type\":\"strategy\",\"name\":\"T\","
            "\"source\":\"x\",\"policy\":\"skip\"}",
        };
        for (int i = 0; i < 4; ++i) {
            ordered_json req = ordered_json::parse(bad[i]);
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [21.%d] policy on non-cache op: %s\n", i,
                         cliResultToJson(r).dump().c_str());
            if (r.success || r.exitCode != 1) {
                ec = fail("check21: policy on non-cache op must reject",
                          230 + i);
                return 0;
            }
            if (!contains(r.errorMessage, "policy")) {
                ec = fail("check21: message missing 'policy'", 234 + i);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: 'policy' rejected on non-backup-cache ops\n");
    }

    // ------------------------------------------------------------------
    // Check 22: 'batch' metadata rejected on the cache ops (they are not
    //   WHServer-mutating, so the mutating-op gate refuses them).
    //   {"op":"backup-cache","batch":{...valid...}} -> exitCode==1.
    // ------------------------------------------------------------------
    {
        const char* bad[] = {
            "{\"op\":\"backup-cache\",\"batch\":{\"id\":\"b\",\"index\":1,"
            "\"total\":2}}",
            "{\"op\":\"restore-cache\",\"batch\":{\"id\":\"b\",\"index\":1,"
            "\"total\":2}}",
        };
        for (int i = 0; i < 2; ++i) {
            ordered_json req = ordered_json::parse(bad[i]);
            ServeRequest out;
            CliResult r = parseServeRequest(req, out);
            std::fprintf(stdout, "  [22.%d] batch on cache op: %s\n", i,
                         cliResultToJson(r).dump().c_str());
            if (r.success || r.exitCode != 1) {
                ec = fail("check22: batch tag on cache op must reject",
                          240 + i);
                return 0;
            }
            if (out.hasBatch) {
                ec = fail("check22: hasBatch must stay false", 242 + i);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: 'batch' metadata rejected on cache ops\n");
    }

    // ------------------------------------------------------------------
    // Check 23: CliResult::extraPayload merge — the response-frame seam for
    //   the serve cache ops. A success CliResult carrying the standalone
    //   --json payload object must serialize its members at top level via
    //   cliResultToJson (op/success/exit_code identical). This is the pure
    //   seam dispatchServeRequest returns through; dispatchServeRequest
    //   itself needs a real TsDevLib/cache layout (not unit-testable).
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op = "backup-cache";
        r.success = true;
        r.exitCode = 0;
        r.extraPayload["op"] = "backup-cache";
        r.extraPayload["success"] = true;
        r.extraPayload["exit_code"] = 0;
        ordered_json cache;
        cache["path"] = "C:\\Backup\\cache";
        cache["files"] = 3;
        cache["bytes"] = 42;
        cache["elapsed_secs"] = 0;
        cache["created"] = "2026-01-01T00:00:00Z";
        cache["live_source"] = true;
        r.extraPayload["cache"] = cache;
        const ordered_json j = cliResultToJson(r);
        std::fprintf(stdout, "  [23] extraPayload merge: %s\n",
                     j.dump().c_str());
        if (!j.contains("cache") || !j["cache"].is_object()) {
            ec = fail("check23: response missing 'cache' payload", 250);
            return 0;
        }
        if (j["cache"]["files"] != 3 ||
            j["cache"]["live_source"] != true) {
            ec = fail("check23: payload members not merged verbatim", 251);
            return 0;
        }
        if (j["op"] != "backup-cache" || j["success"] != true ||
            j["exit_code"] != 0) {
            ec = fail("check23: op/success/exit_code corrupted", 252);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: extraPayload merged into response frame\n");
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
