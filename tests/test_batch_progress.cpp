// test_batch_progress.cpp
//
// Unit test for the batch-progress event renderers and the aggregate batch
// exit-code helper (TVS A), plus the TVS C cooperative stdin batch-control
// helpers (decision reader, control-error renderer, cancelled marker).
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises these helpers in src/tsdev_cli.cpp:
//   - std::string batchItemStartedLine(const std::string& op, size_t index,
//         size_t total, const std::string& type, const std::string& name,
//         bool jsonOutput);
//   - std::string batchItemCompletedLine(const std::string& op, size_t index,
//         size_t total, const CliResult& result, bool jsonOutput);
//   - std::string batchCompletedLine(const std::string& op, size_t completed,
//         size_t total, size_t successes, size_t failures, size_t notRun,
//         bool cancelled, bool closeFailed, int exitCode, bool jsonOutput);
//   - int batchAggregateExitCode(bool batchStarted, bool anyFailed,
//         bool cancelled);
//   - enum class BatchControlDecision { Continue, Cancel, Error };   (TVS C)
//   - BatchControlDecision readBatchControlDecision(FILE* input,
//         std::string& error);                                      (TVS C)
//   - std::string batchControlErrorLine(const std::string& message,
//         bool jsonOutput);                                         (TVS C)
//   - CliResult cancelledBatchItemResult(const std::string& op,
//         const BatchItem& item);                                   (TVS C)
//
// Line contract (both renderers): the returned string is exactly ONE line —
// it contains no '\n' or '\r' anywhere and no trailing newline (production
// emission appends '\n' + fflush(stderr) later). JSON mode emits one compact
// object per the approved schemas:
//   {"event":"batch_item_started","batch_operation":"...","index":1,
//    "total":3,"type":"...","name":"..."}
//   {"event":"batch_item_completed","batch_operation":"...","index":1,
//    "total":3,"type":"...","name":"...","success":false,"exit_code":8,
//    "result":<exact cliResultToJson object>}
//   {"event":"batch_completed","batch_operation":"...","completed":2,
//    "total":3,"successes":1,"failures":1,"not_run":1,"cancelled":true,
//    "close_failed":false,"exit_code":3}
// Human mode emits one canonical line per event — the explicit tokens
// START / SUCCESS / FAILED / COMPLETE / CANCELLED, a 1-based "[index/total]"
// on item lines, and ordered key=value fields carrying the equivalent data:
//   BATCH START [i/n] op=<op> type="<type>" name="<name>"
//   BATCH SUCCESS [i/n] op=<op> type="<type>" name="<name>" exit_code=<n>
//   BATCH FAILED [i/n] op=<op> type="<type>" name="<name>" exit_code=<n>
//         message="<errorMessage>"
//   BATCH COMPLETE op=<op> completed=N total=N successes=N
//         failures=N not_run=N cancelled=<bool> close_failed=<bool>
//         exit_code=N   (the optional CANCELLED token is inserted
//         immediately after COMPLETE when cancelled=true:
//         "BATCH COMPLETE CANCELLED op=<op> ...")
// CR/LF/quote/backslash inside type/name (and message) are escaped
// (\" \\ \n \r) so one event remains one physical line.
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   The helpers live inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so they are NOT accessible from a
//   separate translation unit. The only way to reach them is to #include the
//   .cpp directly so the test is part of the SAME translation unit. This
//   pulls in tsdev_cli.cpp's int main(); the test #defines TSDEV_CLI_TEST
//   before the include so main() is excluded by the TSDEV_CLI_TEST guard in
//   src/tsdev_cli.cpp.
//
// Checks:
//   1. JSON batch_item_started: exact member set {event, batch_operation,
//      index, total, type, name}, correct types, one physical line.
//   2. JSON batch_item_completed (failure result): member set incl. success,
//      exit_code, and "result" equal to cliResultToJson(r) verbatim.
//   3. JSON batch_item_completed (success result): success:true, exit_code:0,
//      "result" equal to cliResultToJson(r) verbatim.
//   4. JSON batch_completed: exact member set incl. not_run, cancelled,
//      close_failed, exit_code — boolean fields are JSON booleans, counters
//      are JSON integers. Covered for a cancelled+close-failed batch and a
//      clean batch.
//   5. Human item lines: exact canonical START / SUCCESS / FAILED lines,
//      including the result details (exit_code, message on failure).
//   6. Human batch_completed: exact canonical COMPLETE line; the cancelled
//      form inserts the CANCELLED token and keeps the same ordered
//      key/value fields.
//   7. Escaping: a name containing '"' + LF + CR + '\' renders its escaped
//      form inside the canonical human line for BOTH the started and
//      completed renderers, and round-trips verbatim through JSON mode.
//   8. batchAggregateExitCode: all 8 (batchStarted, anyFailed, cancelled)
//      combinations — !batchStarted -> 1, cancelled -> 3 (dominates),
//      anyFailed -> 2, clean -> 0.
//   9. (TVS C) readBatchControlDecision: exactly "continue\n" -> Continue
//      and "cancel\n" -> Cancel; one stream yields one decision per call;
//      EOF/read error/any other token (blank, uppercase, spaces, CRLF,
//      garbage) -> Error with a nonempty diagnostic.
//  10. (TVS C) batchControlErrorLine: JSON exact member set {event,
//      message, action} with action=="cancel"; human exact
//      "BATCH CONTROL ERROR: <message>; cancelling before next item" with
//      JSON-safe one-line escaping of the message.
//  11. (TVS C) cancelledBatchItemResult: preserves op/type/name,
//      success=false, exitCode=12, exact errorMessage, serialized
//      error.code=="cancelled" (errorCodeString(12,op)=="cancelled").
//  12. (TVS C) cancel composition: attempted prefix + markers stays full
//      length in the emitted array, and cancelled+anyFailed aggregates to
//      exit 3 (batchCompletedLine: completed=attempted, not_run=markers).
//  15. (R6) bounded batch-control wait: runBoundedControlRead pass-through
//      for Continue/Cancel/Error+diagnostic; a blocked reader yields Error
//      after the bound (orphan-wait containment); FILE* end-to-end via
//      readBatchControlDecisionBounded (timeoutSecs=0 -> direct read,
//      >0 -> bounded, EOF still Error); parseTimeoutSecs contract.
//  16. (P4 gaps a/g-1) zombie pre-check + enum-failure seams:
//      whserverZombiePid intersection; whserverZombieAbortLine /
//      whserverEnumFailedLine renderers; the zombie refusal's exit-14
//      serialization; kZombieAbortMarkerMessage + abort_reason
//      "whserver_zombie" on the summary.
//
// NOT pinned here (no same-TU seam exists): the --batch-progress-control
// argument wiring lives inside tsdev_cli.cpp's main(), which is excluded
// under TSDEV_CLI_TEST, so the "batch-only flag / decisions-only vs framed
// stdin routing" characterization cannot be exercised without main() and is
// covered by the TVS C live harness instead.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (the batch-progress helpers, cliResultToJson, CliResult,
// ordered_json, etc.) are visible.
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

// Helper: returns true when `line` is exactly one physical line — non-empty,
// no '\n'/'\r' anywhere, hence no trailing newline either.
bool isSingleLine(const std::string& line) {
    return !line.empty() &&
           line.find('\n') == std::string::npos &&
           line.find('\r') == std::string::npos;
}

// Helper (TVS C checks): returns a binary-mode FILE* positioned at the start
// of an anonymous pipe pre-filled with `bytes`; once those bytes are consumed
// the reader sees EOF. This feeds the stdin-shaped control protocol exact
// byte sequences — including immediate EOF — without touching the real stdin
// or the filesystem. The FILE* owns the pipe read handle; fclose() releases
// it. Returns nullptr on setup failure.
FILE* fileFromBytes(const std::string& bytes) {
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return nullptr;
    if (!bytes.empty()) {
        DWORD wrote = 0;
        if (!WriteFile(writeEnd, bytes.data(),
                       static_cast<DWORD>(bytes.size()), &wrote, nullptr) ||
            wrote != bytes.size()) {
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return nullptr;
        }
    }
    // Closing the write end makes the reader see EOF once the buffered
    // bytes are drained.
    CloseHandle(writeEnd);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(readEnd),
                             _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(readEnd);
        return nullptr;
    }
    FILE* f = _fdopen(fd, "rb");
    if (!f) {
        _close(fd);  // _close releases the underlying OS handle
        return nullptr;
    }
    return f;
}

// Helper (TVS C checks): drains the remainder of `f` into a string — used to
// assert the byte-exact manifest/decision boundary on the input stream.
std::string drainFile(FILE* f) {
    std::string out;
    char buf[256];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    return out;
}

// Injected readers (R6 checks): runBoundedControlRead takes the reader as
// a function pointer so the real helper-thread + bounded-wait machinery
// can be exercised without stdin.
BatchControlDecision readerReturnsContinue(void*, std::string& err) {
    err = "reader-side diagnostic";  // passes through verbatim
    return BatchControlDecision::Continue;
}
BatchControlDecision readerReturnsCancel(void*, std::string& err) {
    err.clear();
    return BatchControlDecision::Cancel;
}
BatchControlDecision readerReturnsError(void*, std::string& err) {
    err = "injected read failure";
    return BatchControlDecision::Error;
}
// Blocks longer than any bound the test arms — models the orphaned
// control channel (dead parent, open pipe: fgetc never returns).
BatchControlDecision readerBlocksForever(void* ctx, std::string& err) {
    err.clear();
    Sleep(*static_cast<const DWORD*>(ctx));
    return BatchControlDecision::Continue;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_batch_progress: batch progress helpers (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: JSON batch_item_started.
    //   batchItemStartedLine("insert", 1, 3, "strategy", "MyStrat", true)
    // Expected: one physical line parsing to exactly
    //   {"event":"batch_item_started","batch_operation":"insert","index":1,
    //    "total":3,"type":"strategy","name":"MyStrat"}
    // ------------------------------------------------------------------
    {
        std::string line =
            batchItemStartedLine("insert", 1, 3, "strategy", "MyStrat", true);
        std::fprintf(stdout, "  [1] json started: %s\n", line.c_str());

        if (!isSingleLine(line)) {
            ec = fail("check1: line is not exactly one physical line", 10);
            return 0;
        }
        ordered_json j = ordered_json::parse(line);
        if (j.size() != 6) {
            ec = fail("check1: unexpected member count (want exactly 6)", 11);
            return 0;
        }
        if (!j.contains("event") ||
            j["event"].get<std::string>() != "batch_item_started") {
            ec = fail("check1: event != \"batch_item_started\"", 12);
            return 0;
        }
        if (!j.contains("batch_operation") ||
            j["batch_operation"].get<std::string>() != "insert") {
            ec = fail("check1: batch_operation != \"insert\"", 13);
            return 0;
        }
        if (!j.contains("index") || !j["index"].is_number_integer() ||
            j["index"].get<size_t>() != 1) {
            ec = fail("check1: index != 1 (integer)", 14);
            return 0;
        }
        if (!j.contains("total") || !j["total"].is_number_integer() ||
            j["total"].get<size_t>() != 3) {
            ec = fail("check1: total != 3 (integer)", 15);
            return 0;
        }
        if (!j.contains("type") ||
            j["type"].get<std::string>() != "strategy") {
            ec = fail("check1: type != \"strategy\"", 16);
            return 0;
        }
        if (!j.contains("name") ||
            j["name"].get<std::string>() != "MyStrat") {
            ec = fail("check1: name != \"MyStrat\"", 17);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: json batch_item_started schema exact\n");
    }

    // ------------------------------------------------------------------
    // Check 2: JSON batch_item_completed (failure result).
    //   CliResult: op="insert", typeStr="strategy", name="MyStrat",
    //   success=false, exitCode=8, progId=42,
    //   errorMessage="compilation FAILED", compileErrors="Line 1: error"
    //   batchItemCompletedLine("insert", 2, 3, r, true)
    // Expected: member set {event, batch_operation, index, total, type, name,
    //   success, exit_code, result}; success:false, exit_code:8, and
    //   "result" is the EXACT cliResultToJson(r) object.
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op            = "insert";
        r.typeStr       = "strategy";
        r.name          = "MyStrat";
        r.success       = false;
        r.exitCode      = 8;
        r.progId        = 42;
        r.errorMessage  = "compilation FAILED";
        r.compileErrors = "Line 1: error";

        std::string line = batchItemCompletedLine("insert", 2, 3, r, true);
        std::fprintf(stdout, "  [2] json completed (fail): %s\n",
                     line.c_str());

        if (!isSingleLine(line)) {
            ec = fail("check2: line is not exactly one physical line", 20);
            return 0;
        }
        ordered_json j = ordered_json::parse(line);
        if (j.size() != 9) {
            ec = fail("check2: unexpected member count (want exactly 9)", 21);
            return 0;
        }
        if (!j.contains("event") ||
            j["event"].get<std::string>() != "batch_item_completed") {
            ec = fail("check2: event != \"batch_item_completed\"", 22);
            return 0;
        }
        if (!j.contains("batch_operation") ||
            j["batch_operation"].get<std::string>() != "insert") {
            ec = fail("check2: batch_operation != \"insert\"", 23);
            return 0;
        }
        if (!j.contains("index") || j["index"].get<size_t>() != 2) {
            ec = fail("check2: index != 2", 24);
            return 0;
        }
        if (!j.contains("total") || j["total"].get<size_t>() != 3) {
            ec = fail("check2: total != 3", 25);
            return 0;
        }
        if (!j.contains("type") ||
            j["type"].get<std::string>() != "strategy") {
            ec = fail("check2: type != \"strategy\"", 26);
            return 0;
        }
        if (!j.contains("name") ||
            j["name"].get<std::string>() != "MyStrat") {
            ec = fail("check2: name != \"MyStrat\"", 27);
            return 0;
        }
        if (!j.contains("success") || !j["success"].is_boolean() ||
            j["success"].get<bool>() != false) {
            ec = fail("check2: success != false (boolean)", 28);
            return 0;
        }
        if (!j.contains("exit_code") || !j["exit_code"].is_number_integer() ||
            j["exit_code"].get<int>() != 8) {
            ec = fail("check2: exit_code != 8 (integer)", 29);
            return 0;
        }
        // The "result" member must be the exact cliResultToJson object —
        // the same element schema the final array and serve responses use.
        if (!j.contains("result") || !j["result"].is_object() ||
            j["result"] != cliResultToJson(r)) {
            ec = fail("check2: result != exact cliResultToJson object", 30);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: json batch_item_completed (failure) exact\n");
    }

    // ------------------------------------------------------------------
    // Check 3: JSON batch_item_completed (success result).
    //   CliResult: op="insert", typeStr="strategy", name="MyStrat",
    //   success=true, exitCode=0, progId=42, ellPath="C:\\path\\file.ell"
    //   batchItemCompletedLine("insert", 1, 3, r, true)
    // Expected: success:true, exit_code:0, "result" == cliResultToJson(r).
    // ------------------------------------------------------------------
    {
        CliResult r;
        r.op       = "insert";
        r.typeStr  = "strategy";
        r.name     = "MyStrat";
        r.success  = true;
        r.exitCode = 0;
        r.progId   = 42;
        r.ellPath  = "C:\\path\\file.ell";

        std::string line = batchItemCompletedLine("insert", 1, 3, r, true);
        std::fprintf(stdout, "  [3] json completed (ok): %s\n", line.c_str());

        if (!isSingleLine(line)) {
            ec = fail("check3: line is not exactly one physical line", 40);
            return 0;
        }
        ordered_json j = ordered_json::parse(line);
        if (!j.contains("event") ||
            j["event"].get<std::string>() != "batch_item_completed") {
            ec = fail("check3: event != \"batch_item_completed\"", 41);
            return 0;
        }
        if (!j.contains("success") || j["success"].get<bool>() != true) {
            ec = fail("check3: success != true", 42);
            return 0;
        }
        if (!j.contains("exit_code") || j["exit_code"].get<int>() != 0) {
            ec = fail("check3: exit_code != 0", 43);
            return 0;
        }
        if (!j.contains("result") || !j["result"].is_object() ||
            j["result"] != cliResultToJson(r)) {
            ec = fail("check3: result != exact cliResultToJson object", 44);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: json batch_item_completed (success) exact\n");
    }

    // ------------------------------------------------------------------
    // Check 4: JSON batch_completed.
    //   (a) batchCompletedLine("delete", 2, 3, 1, 1, 1,
    //                          cancelled=true, closeFailed=true, 3, true)
    //       -> exact member set {event, batch_operation, completed, total,
    //          successes, failures, not_run, cancelled, close_failed,
    //          exit_code} with cancelled:true, close_failed:true, exit_code:3.
    //   (b) batchCompletedLine("insert", 3, 3, 3, 0, 0,
    //                          cancelled=false, closeFailed=false, 0, true)
    //       -> cancelled:false, close_failed:false, not_run:0, exit_code:0.
    // ------------------------------------------------------------------
    {
        std::string line = batchCompletedLine("delete", 2, 3, 1, 1, 1,
                                              /*cancelled=*/true,
                                              /*closeFailed=*/true,
                                              /*exitCode=*/3, true);
        std::fprintf(stdout, "  [4a] json batch_completed: %s\n",
                     line.c_str());

        if (!isSingleLine(line)) {
            ec = fail("check4a: line is not exactly one physical line", 50);
            return 0;
        }
        ordered_json j = ordered_json::parse(line);
        if (j.size() != 10) {
            ec = fail("check4a: unexpected member count (want exactly 10)",
                      51);
            return 0;
        }
        if (!j.contains("event") ||
            j["event"].get<std::string>() != "batch_completed") {
            ec = fail("check4a: event != \"batch_completed\"", 52);
            return 0;
        }
        if (!j.contains("batch_operation") ||
            j["batch_operation"].get<std::string>() != "delete") {
            ec = fail("check4a: batch_operation != \"delete\"", 53);
            return 0;
        }
        if (!j.contains("completed") || !j["completed"].is_number_integer() ||
            j["completed"].get<size_t>() != 2) {
            ec = fail("check4a: completed != 2 (integer)", 54);
            return 0;
        }
        if (!j.contains("total") || j["total"].get<size_t>() != 3) {
            ec = fail("check4a: total != 3", 55);
            return 0;
        }
        if (!j.contains("successes") || j["successes"].get<size_t>() != 1) {
            ec = fail("check4a: successes != 1", 56);
            return 0;
        }
        if (!j.contains("failures") || j["failures"].get<size_t>() != 1) {
            ec = fail("check4a: failures != 1", 57);
            return 0;
        }
        if (!j.contains("not_run") || !j["not_run"].is_number_integer() ||
            j["not_run"].get<size_t>() != 1) {
            ec = fail("check4a: not_run != 1 (integer)", 58);
            return 0;
        }
        if (!j.contains("cancelled") || !j["cancelled"].is_boolean() ||
            j["cancelled"].get<bool>() != true) {
            ec = fail("check4a: cancelled != true (boolean)", 59);
            return 0;
        }
        if (!j.contains("close_failed") || !j["close_failed"].is_boolean() ||
            j["close_failed"].get<bool>() != true) {
            ec = fail("check4a: close_failed != true (boolean)", 60);
            return 0;
        }
        if (!j.contains("exit_code") || j["exit_code"].get<int>() != 3) {
            ec = fail("check4a: exit_code != 3", 61);
            return 0;
        }

        std::string line2 = batchCompletedLine("insert", 3, 3, 3, 0, 0,
                                               /*cancelled=*/false,
                                               /*closeFailed=*/false,
                                               /*exitCode=*/0, true);
        std::fprintf(stdout, "  [4b] json batch_completed: %s\n",
                     line2.c_str());
        if (!isSingleLine(line2)) {
            ec = fail("check4b: line is not exactly one physical line", 62);
            return 0;
        }
        ordered_json j2 = ordered_json::parse(line2);
        if (!j2.contains("cancelled") ||
            j2["cancelled"].get<bool>() != false) {
            ec = fail("check4b: cancelled != false", 63);
            return 0;
        }
        if (!j2.contains("close_failed") ||
            j2["close_failed"].get<bool>() != false) {
            ec = fail("check4b: close_failed != false", 64);
            return 0;
        }
        if (!j2.contains("not_run") || j2["not_run"].get<size_t>() != 0) {
            ec = fail("check4b: not_run != 0", 65);
            return 0;
        }
        if (!j2.contains("exit_code") || j2["exit_code"].get<int>() != 0) {
            ec = fail("check4b: exit_code != 0", 66);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: json batch_completed schema exact\n");
    }

    // ------------------------------------------------------------------
    // Check 5: Human item lines (jsonOutput=false) — exact canonical form.
    //   (a) batchItemStartedLine("insert", 1, 3, "strategy", "MyStrat", false)
    //       == "BATCH START [1/3] op=insert type=\"strategy\"
    //           name=\"MyStrat\""
    //   (b) batchItemCompletedLine with a success result (exit 0)
    //       == "BATCH SUCCESS [2/3] op=insert type=\"strategy\"
    //           name=\"MyStrat\" exit_code=0"
    //   (c) batchItemCompletedLine with a failure result (exit 8,
    //       errorMessage "compilation FAILED")
    //       == "BATCH FAILED [2/3] op=insert type=\"strategy\"
    //           name=\"MyStrat\" exit_code=8 message=\"compilation FAILED\""
    //   Exact equality also proves each line is one physical line with no
    //   trailing newline.
    // ------------------------------------------------------------------
    {
        std::string started =
            batchItemStartedLine("insert", 1, 3, "strategy", "MyStrat", false);
        std::fprintf(stdout, "  [5a] human started: %s\n", started.c_str());
        if (started != "BATCH START [1/3] op=insert type=\"strategy\" "
                       "name=\"MyStrat\"") {
            ec = fail("check5a: line != canonical START line", 70);
            return 0;
        }

        CliResult ok;
        ok.op       = "insert";
        ok.typeStr  = "strategy";
        ok.name     = "MyStrat";
        ok.success  = true;
        ok.exitCode = 0;
        ok.progId   = 42;
        std::string succ = batchItemCompletedLine("insert", 2, 3, ok, false);
        std::fprintf(stdout, "  [5b] human completed (ok): %s\n",
                     succ.c_str());
        if (succ != "BATCH SUCCESS [2/3] op=insert type=\"strategy\" "
                    "name=\"MyStrat\" exit_code=0") {
            ec = fail("check5b: line != canonical SUCCESS line", 71);
            return 0;
        }

        CliResult bad;
        bad.op           = "insert";
        bad.typeStr      = "strategy";
        bad.name         = "MyStrat";
        bad.success      = false;
        bad.exitCode     = 8;
        bad.errorMessage = "compilation FAILED";
        std::string failed =
            batchItemCompletedLine("insert", 2, 3, bad, false);
        std::fprintf(stdout, "  [5c] human completed (fail): %s\n",
                     failed.c_str());
        if (failed != "BATCH FAILED [2/3] op=insert type=\"strategy\" "
                      "name=\"MyStrat\" exit_code=8 "
                      "message=\"compilation FAILED\"") {
            ec = fail("check5c: line != canonical FAILED line", 72);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: human item lines match canonical form\n");
    }

    // ------------------------------------------------------------------
    // Check 6: Human batch_completed — exact canonical form.
    //   (a) clean batch ==
    //       "BATCH COMPLETE op=insert completed=3 total=3 successes=3
    //        failures=0 not_run=0 cancelled=false close_failed=false
    //        exit_code=0"
    //   (b) cancelled batch == "BATCH COMPLETE CANCELLED" followed by the
    //       same ordered key/value fields (cancelled=true, exit_code=3).
    // ------------------------------------------------------------------
    {
        std::string clean = batchCompletedLine("insert", 3, 3, 3, 0, 0,
                                               /*cancelled=*/false,
                                               /*closeFailed=*/false,
                                               /*exitCode=*/0, false);
        std::fprintf(stdout, "  [6a] human batch_completed: %s\n",
                     clean.c_str());
        if (clean != "BATCH COMPLETE op=insert completed=3 total=3 "
                     "successes=3 failures=0 not_run=0 cancelled=false "
                     "close_failed=false exit_code=0") {
            ec = fail("check6a: line != canonical COMPLETE line", 90);
            return 0;
        }

        std::string cx = batchCompletedLine("delete", 2, 3, 1, 1, 1,
                                            /*cancelled=*/true,
                                            /*closeFailed=*/false,
                                            /*exitCode=*/3, false);
        std::fprintf(stdout,
                     "  [6b] human batch_completed (cancelled): %s\n",
                     cx.c_str());
        if (cx != "BATCH COMPLETE CANCELLED op=delete completed=2 total=3 "
                  "successes=1 failures=1 not_run=1 cancelled=true "
                  "close_failed=false exit_code=3") {
            ec = fail("check6b: line != canonical COMPLETE CANCELLED line",
                      91);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: human batch_completed matches canonical form\n");
    }

    // ------------------------------------------------------------------
    // Check 7: Escaping — one event stays one physical line, through BOTH
    //   the started and completed renderers.
    //   name = A"q<LF>B<CR>C\d  (quote, LF, CR, backslash)
    //   (a) human started: exact canonical line carrying the escaped form
    //       A\"q\nB\rC\\d inside the quoted name="..." field.
    //   (b) human completed (FAILED): the same escaped form inside the
    //       canonical FAILED line.
    //   (c) JSON started: no raw control characters; parsed "name"
    //       round-trips verbatim.
    //   (d) JSON completed: same, plus result."name" round-trips verbatim.
    // ------------------------------------------------------------------
    {
        const std::string nasty = "A\"q\nB\rC\\d";

        std::string hs =
            batchItemStartedLine("insert", 1, 2, "function", nasty, false);
        std::fprintf(stdout, "  [7a] human started escaped: %s\n",
                     hs.c_str());
        if (hs != "BATCH START [1/2] op=insert type=\"function\" "
                  "name=\"A\\\"q\\nB\\rC\\\\d\"") {
            ec = fail("check7a: line != canonical escaped START line", 100);
            return 0;
        }

        CliResult bad;
        bad.op           = "insert";
        bad.typeStr      = "function";
        bad.name         = nasty;
        bad.success      = false;
        bad.exitCode     = 8;
        bad.errorMessage = "compilation FAILED";
        std::string hc = batchItemCompletedLine("insert", 1, 2, bad, false);
        std::fprintf(stdout, "  [7b] human completed escaped: %s\n",
                     hc.c_str());
        if (hc != "BATCH FAILED [1/2] op=insert type=\"function\" "
                  "name=\"A\\\"q\\nB\\rC\\\\d\" exit_code=8 "
                  "message=\"compilation FAILED\"") {
            ec = fail("check7b: line != canonical escaped FAILED line", 101);
            return 0;
        }

        std::string js =
            batchItemStartedLine("insert", 1, 2, "function", nasty, true);
        std::fprintf(stdout, "  [7c] json started escaped: %s\n", js.c_str());
        if (!isSingleLine(js)) {
            ec = fail("check7c: raw CR/LF leaked into the JSON line", 102);
            return 0;
        }
        ordered_json jn = ordered_json::parse(js);
        if (!jn.contains("name") ||
            jn["name"].get<std::string>() != nasty) {
            ec = fail("check7c: JSON name did not round-trip verbatim", 103);
            return 0;
        }

        std::string jc = batchItemCompletedLine("insert", 1, 2, bad, true);
        std::fprintf(stdout, "  [7d] json completed escaped: %s\n",
                     jc.c_str());
        if (!isSingleLine(jc)) {
            ec = fail("check7d: raw CR/LF leaked into the JSON line", 104);
            return 0;
        }
        ordered_json jcj = ordered_json::parse(jc);
        if (!jcj.contains("name") ||
            jcj["name"].get<std::string>() != nasty ||
            !jcj.contains("result") ||
            jcj["result"]["name"].get<std::string>() != nasty) {
            ec = fail("check7d: JSON name/result.name not verbatim", 105);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: escaping keeps one event one physical line\n");
    }

    // ------------------------------------------------------------------
    // Check 8: batchAggregateExitCode — every (batchStarted, anyFailed,
    // cancelled) combination.
    //   !batchStarted -> 1 (pre-run usage/manifest/dir failures unchanged);
    //   cancelled -> 3 (dominates: cancelled + anyFailed is still 3);
    //   anyFailed -> 2; clean run -> 0.
    // ------------------------------------------------------------------
    {
        struct AggCase {
            bool batchStarted;
            bool anyFailed;
            bool cancelled;
            int  expected;
        };
        const AggCase cases[] = {
            {false, false, false, 1},
            {false, false, true,  1},
            {false, true,  false, 1},
            {false, true,  true,  1},
            {true,  false, false, 0},
            {true,  false, true,  3},  // cancelled dominates
            {true,  true,  false, 2},
            {true,  true,  true,  3},  // cancelled + failed -> still 3
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const AggCase& c = cases[i];
            int got = batchAggregateExitCode(c.batchStarted, c.anyFailed,
                                             c.cancelled);
            std::fprintf(stdout,
                         "  [8.%u] batchAggregateExitCode(started=%d, "
                         "failed=%d, cancelled=%d) = %d (want %d)\n",
                         static_cast<unsigned>(i), c.batchStarted,
                         c.anyFailed, c.cancelled, got, c.expected);
            if (got != c.expected) {
                ec = fail("check8: wrong aggregate exit code", 110);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: batchAggregateExitCode all 8 combinations\n");
    }

    // ------------------------------------------------------------------
    // Check 9 (TVS C): readBatchControlDecision — the between-items stdin
    //   decision reader.
    //   Accepts EXACTLY "continue\n" -> BatchControlDecision::Continue and
    //   "cancel\n" -> BatchControlDecision::Cancel. EOF, a read error, or
    //   ANY other token (blank line, uppercase, leading/trailing spaces,
    //   CRLF — the approved design admits only LF — garbage) ->
    //   BatchControlDecision::Error with a NON-EMPTY diagnostic in `error`.
    //   (a) "continue\n" -> Continue.
    //   (b) "cancel\n" -> Cancel.
    //   (c) one stream carrying "continue\ncancel\n" yields Continue then
    //       Cancel across two successive calls (exactly one decision line
    //       is consumed per call).
    //   (d) rejection table: every malformed input -> Error + nonempty
    //       error string.
    // ------------------------------------------------------------------
    {
        FILE* f = fileFromBytes("continue\n");
        if (!f) {
            ec = fail("check9a: pipe setup failed", 120);
            return 0;
        }
        std::string err = "SENTINEL";
        BatchControlDecision d = readBatchControlDecision(f, err);
        std::fclose(f);
        std::fprintf(stdout, "  [9a] \"continue\\n\" -> decision\n");
        if (d != BatchControlDecision::Continue) {
            ec = fail("check9a: \"continue\\n\" != Continue", 121);
            return 0;
        }

        f = fileFromBytes("cancel\n");
        if (!f) {
            ec = fail("check9b: pipe setup failed", 122);
            return 0;
        }
        err = "SENTINEL";
        d = readBatchControlDecision(f, err);
        std::fclose(f);
        std::fprintf(stdout, "  [9b] \"cancel\\n\" -> decision\n");
        if (d != BatchControlDecision::Cancel) {
            ec = fail("check9b: \"cancel\\n\" != Cancel", 123);
            return 0;
        }

        f = fileFromBytes("continue\ncancel\n");
        if (!f) {
            ec = fail("check9c: pipe setup failed", 124);
            return 0;
        }
        err.clear();
        BatchControlDecision d1 = readBatchControlDecision(f, err);
        BatchControlDecision d2 = readBatchControlDecision(f, err);
        std::fclose(f);
        std::fprintf(stdout,
                     "  [9c] \"continue\\ncancel\\n\" -> two decisions\n");
        if (d1 != BatchControlDecision::Continue ||
            d2 != BatchControlDecision::Cancel) {
            ec = fail("check9c: sequential decisions != Continue,Cancel",
                      125);
            return 0;
        }

        // Rejection table — every input must yield Error and a nonempty
        // diagnostic. "" is immediate EOF; "continue"/"cancel" without the
        // LF hit EOF mid-token (a channel anomaly, not a clean decision).
        const char* badInputs[] = {
            "",                 // EOF: no bytes at all
            "continue",         // EOF before the line terminator
            "cancel",           // EOF before the line terminator
            "\n",               // blank line
            "CONTINUE\n",       // uppercase
            "CANCEL\n",         // uppercase
            " continue\n",      // leading space
            "continue \n",      // trailing space
            "continue\r\n",     // CRLF (design admits LF only)
            "cancel\r\n",       // CRLF
            "junk\n",           // unrecognized token
            "continue cancel\n" // two tokens on one line
        };
        for (size_t i = 0; i < sizeof(badInputs) / sizeof(badInputs[0]);
             ++i) {
            f = fileFromBytes(badInputs[i]);
            if (!f) {
                ec = fail("check9d: pipe setup failed", 126);
                return 0;
            }
            err.clear();
            d = readBatchControlDecision(f, err);
            std::fclose(f);
            std::fprintf(stdout, "  [9d.%u] bad token -> Error, "
                         "error=\"%s\"\n", static_cast<unsigned>(i),
                         err.c_str());
            if (d != BatchControlDecision::Error) {
                ec = fail("check9d: malformed input did not yield Error",
                          127);
                return 0;
            }
            if (err.empty()) {
                ec = fail("check9d: malformed input left error empty", 128);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: decision reader accepts exactly "
                     "continue\\n / cancel\\n, rejects all else\n");
    }

    // ------------------------------------------------------------------
    // Check 10 (TVS C): batchControlErrorLine — the batch_control_error
    //   event renderer.
    //   (a) JSON: one physical line parsing to EXACTLY
    //       {"event":"batch_control_error","message":<msg>,
    //        "action":"cancel"} — exactly three members.
    //   (b) human: exactly
    //       "BATCH CONTROL ERROR: <message>; cancelling before next item".
    //   (c) escaping: a message containing '"' + LF + CR + '\' stays on
    //       ONE physical line in both renderers; the human <message> slot
    //       carries the JSON one-line escaped form (\" \\ \n \r — same
    //       escaping jsonQuote applies, without the surrounding quotes),
    //       and the JSON "message" member round-trips verbatim.
    // ------------------------------------------------------------------
    {
        const std::string msg = "unexpected EOF while reading decision";
        std::string line = batchControlErrorLine(msg, true);
        std::fprintf(stdout, "  [10a] json control error: %s\n",
                     line.c_str());
        if (!isSingleLine(line)) {
            ec = fail("check10a: line is not exactly one physical line",
                      130);
            return 0;
        }
        ordered_json j = ordered_json::parse(line);
        if (j.size() != 3) {
            ec = fail("check10a: unexpected member count (want exactly 3)",
                      131);
            return 0;
        }
        if (!j.contains("event") ||
            j["event"].get<std::string>() != "batch_control_error") {
            ec = fail("check10a: event != \"batch_control_error\"", 132);
            return 0;
        }
        if (!j.contains("message") ||
            j["message"].get<std::string>() != msg) {
            ec = fail("check10a: message did not round-trip verbatim", 133);
            return 0;
        }
        if (!j.contains("action") ||
            j["action"].get<std::string>() != "cancel") {
            ec = fail("check10a: action != \"cancel\"", 134);
            return 0;
        }

        std::string human = batchControlErrorLine(msg, false);
        std::fprintf(stdout, "  [10b] human control error: %s\n",
                     human.c_str());
        if (human != "BATCH CONTROL ERROR: " + msg +
                     "; cancelling before next item") {
            ec = fail("check10b: line != canonical BATCH CONTROL ERROR "
                      "line", 135);
            return 0;
        }

        const std::string nasty = "a\"b\nc\rd\\e";
        std::string hesc = batchControlErrorLine(nasty, false);
        std::fprintf(stdout, "  [10c] human control error escaped: %s\n",
                     hesc.c_str());
        if (!isSingleLine(hesc)) {
            ec = fail("check10c: raw CR/LF leaked into the human line",
                      136);
            return 0;
        }
        // The <message> slot is verbatim text rendered JSON-safe on one
        // line: the same escaping jsonQuote applies, minus the surrounding
        // quotes.
        std::string quoted = jsonQuote(nasty);
        std::string escapedMsg = quoted.substr(1, quoted.size() - 2);
        if (hesc != "BATCH CONTROL ERROR: " + escapedMsg +
                    "; cancelling before next item") {
            ec = fail("check10c: message not JSON-escaped on one line",
                      137);
            return 0;
        }

        std::string jesc = batchControlErrorLine(nasty, true);
        if (!isSingleLine(jesc)) {
            ec = fail("check10c: raw CR/LF leaked into the JSON line", 138);
            return 0;
        }
        ordered_json jn = ordered_json::parse(jesc);
        if (!jn.contains("message") ||
            jn["message"].get<std::string>() != nasty) {
            ec = fail("check10c: JSON message did not round-trip verbatim",
                      139);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: batch_control_error renderer matches "
                     "canonical form\n");
    }

    // ------------------------------------------------------------------
    // Check 11 (TVS C): cancelledBatchItemResult — the synthetic not-run
    //   marker CliResult for items never attempted after a cancel.
    //   Preserves the original op/type/name verbatim; success=false;
    //   exitCode=12; errorMessage is EXACTLY
    //   "Batch cancelled before this item was attempted". The serialized
    //   form (cliResultToJson — the same schema the final array uses)
    //   carries error.code "cancelled", i.e. errorCodeString(12,op) ==
    //   "cancelled" for any op.
    // ------------------------------------------------------------------
    {
        BatchItem item;
        item.typeStr = "strategy";
        item.name    = "MyStrat";
        item.sourceFile = "C:\\src\\my strat.el";

        CliResult m = cancelledBatchItemResult("insert", item);
        if (m.op != "insert" || m.typeStr != "strategy" ||
            m.name != "MyStrat") {
            ec = fail("check11: marker did not preserve op/type/name", 140);
            return 0;
        }
        if (m.success != false || m.exitCode != 12) {
            ec = fail("check11: marker success/exitCode != false/12", 141);
            return 0;
        }
        if (m.errorMessage !=
            "Batch cancelled before this item was attempted") {
            ec = fail("check11: marker errorMessage mismatch", 142);
            return 0;
        }

        ordered_json j = cliResultToJson(m);
        if (!j.contains("success") || j["success"].get<bool>() != false ||
            !j.contains("exit_code") || j["exit_code"].get<int>() != 12) {
            ec = fail("check11: serialized success/exit_code mismatch",
                      143);
            return 0;
        }
        if (!j.contains("op") || j["op"].get<std::string>() != "insert" ||
            !j.contains("type") ||
            j["type"].get<std::string>() != "strategy" ||
            !j.contains("name") ||
            j["name"].get<std::string>() != "MyStrat") {
            ec = fail("check11: serialized op/type/name not preserved",
                      144);
            return 0;
        }
        if (!j.contains("error") || !j["error"].is_object() ||
            !j["error"].contains("code") ||
            j["error"]["code"].get<std::string>() != "cancelled") {
            ec = fail("check11: serialized error.code != \"cancelled\"",
                      145);
            return 0;
        }
        if (!j["error"].contains("message") ||
            j["error"]["message"].get<std::string>() !=
                "Batch cancelled before this item was attempted") {
            ec = fail("check11: serialized error.message mismatch", 146);
            return 0;
        }
        if (errorCodeString(12, "insert") != "cancelled" ||
            errorCodeString(12, "delete") != "cancelled") {
            ec = fail("check11: errorCodeString(12, op) != \"cancelled\"",
                      147);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: cancelled marker preserves identity with "
                     "exit 12 / cancelled\n");
    }

    // ------------------------------------------------------------------
    // Check 12 (TVS C): cancel composition — one attempted item plus
    //   cancelledBatchItemResult markers for the remaining items keeps the
    //   results vector (and hence the emitted stdout array) FULL LENGTH,
    //   and the aggregate exit is 3 because cancelled dominates anyFailed.
    //   The approved design has no separate sequence helper, so this pins
    //   the marker + cliResultToJson + batchAggregateExitCode +
    //   batchCompletedLine composition directly.
    //   Scenario: 3-item insert batch; item 1 attempted and failed
    //   (exit 8); items 2-3 are not-run markers.
    //   (a) serialized array length == total (3); element 0 keeps exit 8;
    //       elements 1-2 are exit-12/cancelled markers.
    //   (b) batchAggregateExitCode(started, anyFailed, cancelled) == 3.
    //   (c) batchCompletedLine reports completed=1 (attempted),
    //       not_run=2 (markers), cancelled=true, exit_code=3 — and
    //       completed + not_run == total.
    // ------------------------------------------------------------------
    {
        const size_t total = 3;

        std::vector<CliResult> results;
        CliResult attempted;
        attempted.op           = "insert";
        attempted.typeStr      = "strategy";
        attempted.name         = "Done";
        attempted.success      = false;
        attempted.exitCode     = 8;
        attempted.errorMessage = "compilation FAILED";
        results.push_back(attempted);

        BatchItem i2;
        i2.typeStr = "strategy";
        i2.name    = "Skipped1";
        BatchItem i3;
        i3.typeStr = "function";
        i3.name    = "Skipped2";
        results.push_back(cancelledBatchItemResult("insert", i2));
        results.push_back(cancelledBatchItemResult("insert", i3));

        if (results.size() != total) {
            ec = fail("check12a: prefix+markers != full length", 150);
            return 0;
        }
        // Serialize exactly like emitBatchOutput: one cliResultToJson per
        // result, one element per input item.
        ordered_json arr = ordered_json::array();
        for (const CliResult& r : results) {
            arr.push_back(cliResultToJson(r));
        }
        if (arr.size() != total) {
            ec = fail("check12a: emitted array != full length", 151);
            return 0;
        }
        if (arr[0]["exit_code"].get<int>() != 8 ||
            arr[0]["name"].get<std::string>() != "Done") {
            ec = fail("check12a: attempted prefix element mutated", 152);
            return 0;
        }
        for (size_t i = 1; i < total; ++i) {
            if (arr[i]["exit_code"].get<int>() != 12 ||
                arr[i]["success"].get<bool>() != false ||
                arr[i]["error"]["code"].get<std::string>() != "cancelled") {
                ec = fail("check12a: marker element != exit-12 cancelled",
                          153);
                return 0;
            }
        }

        if (batchAggregateExitCode(/*batchStarted=*/true,
                                   /*anyFailed=*/true,
                                   /*cancelled=*/true) != 3) {
            ec = fail("check12b: cancelled+anyFailed aggregate != 3", 154);
            return 0;
        }

        std::string done = batchCompletedLine(
            "insert", /*completed=*/1, total, /*successes=*/0,
            /*failures=*/1, /*notRun=*/2, /*cancelled=*/true,
            /*closeFailed=*/false, /*exitCode=*/3, true);
        std::fprintf(stdout, "  [12c] json batch_completed: %s\n",
                     done.c_str());
        ordered_json dj = ordered_json::parse(done);
        if (!dj.contains("completed") ||
            dj["completed"].get<size_t>() != 1 ||
            !dj.contains("not_run") || dj["not_run"].get<size_t>() != 2 ||
            !dj.contains("cancelled") ||
            dj["cancelled"].get<bool>() != true ||
            !dj.contains("exit_code") || dj["exit_code"].get<int>() != 3) {
            ec = fail("check12c: batch_completed counters wrong on cancel",
                      155);
            return 0;
        }
        if (dj["completed"].get<size_t>() + dj["not_run"].get<size_t>() !=
            total) {
            ec = fail("check12c: completed + not_run != total", 156);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: cancelled batch stays full length and "
                     "aggregates to exit 3\n");
    }

    // ------------------------------------------------------------------
    // Check 13 (TVS-1b): WHServer VAS-guard abort path —
    //   (a) batchAggregateExitCode(started, anyFailed, cancelled=false,
    //       aborted=true) == 14; aborted dominates anyFailed.
    //   (b) batchCompletedLine with aborted=true + abortReason renders the
    //       ABORTED token and aborted/abort_reason fields (human + JSON);
    //       a non-aborted call stays byte-identical (already pinned by
    //       checks 4/6 — re-asserted implicitly here via JSON members).
    //   (c) cancelledBatchItemResult with the abort message keeps exit 12
    //       and carries the abort reason verbatim.
    //   (d) whserverVasAbortLine / whserverVasMeasureFailedLine render one
    //       physical line in both modes.
    //   (e) parseMbThreshold accepts "0"/"5000", rejects "x"/"-1"/""/"1.5".
    // ------------------------------------------------------------------
    {
        if (batchAggregateExitCode(/*batchStarted=*/true,
                                   /*anyFailed=*/true,
                                   /*cancelled=*/false,
                                   /*aborted=*/true) != 14) {
            ec = fail("check13a: aborted aggregate != 14", 160);
            return 0;
        }
        if (batchAggregateExitCode(/*batchStarted=*/true,
                                   /*anyFailed=*/false,
                                   /*cancelled=*/false,
                                   /*aborted=*/true) != 14) {
            ec = fail("check13a: clean aborted aggregate != 14", 161);
            return 0;
        }

        const std::string reason = "whserver_vas_free=123MB";
        std::string hl = batchCompletedLine(
            "upsert", /*completed=*/1, /*total=*/3, /*successes=*/1,
            /*failures=*/0, /*notRun=*/2, /*cancelled=*/false,
            /*closeFailed=*/false, /*exitCode=*/14, false,
            /*aborted=*/true, reason);
        std::fprintf(stdout, "  [13b] human aborted line: %s\n",
                     hl.c_str());
        if (hl != "BATCH COMPLETE ABORTED op=upsert completed=1 total=3 "
                  "successes=1 failures=0 not_run=2 cancelled=false "
                  "aborted=true abort_reason=whserver_vas_free=123MB "
                  "close_failed=false exit_code=14") {
            ec = fail("check13b: human ABORTED line != canonical", 162);
            return 0;
        }
        std::string jl = batchCompletedLine(
            "upsert", 1, 3, 1, 0, 2, false, false, 14, true,
            true, reason);
        std::fprintf(stdout, "  [13b] json aborted line: %s\n",
                     jl.c_str());
        ordered_json aj = ordered_json::parse(jl);
        if (!aj.contains("aborted") || aj["aborted"].get<bool>() != true ||
            !aj.contains("abort_reason") ||
            aj["abort_reason"].get<std::string>() != reason ||
            !aj.contains("exit_code") || aj["exit_code"].get<int>() != 14) {
            ec = fail("check13b: JSON aborted fields wrong", 163);
            return 0;
        }
        // Non-aborted JSON carries NO aborted members (byte-identical
        // contract preserved — exact member count already pinned in
        // check 4a).
        if (aj.contains("not_run") != true) {
            ec = fail("check13b: JSON missing not_run", 164);
            return 0;
        }

        BatchItem item;
        item.typeStr = "function";
        item.name    = "NeverRan";
        CliResult am = cancelledBatchItemResult("upsert", item,
                                                kVasAbortMarkerMessage);
        if (am.exitCode != 12 || am.success != false ||
            am.errorMessage !=
                "Batch aborted (WHServer VAS headroom) before this item "
                "was attempted") {
            ec = fail("check13c: abort marker wrong", 165);
            return 0;
        }

        tsdev::WhVasStats st;
        st.totalFreeBytes        = 123ULL * 1024 * 1024;
        st.largestFreeBlockBytes = 40ULL * 1024 * 1024;
        st.privateBytes          = 0;
        std::string ahl = whserverVasAbortLine(4321, st, 256, 64, false);
        std::string ajl = whserverVasAbortLine(4321, st, 256, 64, true);
        std::string mhl = whserverVasMeasureFailedLine(4321, false);
        std::string mjl = whserverVasMeasureFailedLine(4321, true);
        std::fprintf(stdout, "  [13d] abort human: %s\n", ahl.c_str());
        std::fprintf(stdout, "  [13d] abort json: %s\n", ajl.c_str());
        if (!isSingleLine(ahl) || !isSingleLine(ajl) ||
            !isSingleLine(mhl) || !isSingleLine(mjl)) {
            ec = fail("check13d: renderer emitted multi-line output", 166);
            return 0;
        }
        if (ahl.find("BATCH ABORTED") != 0 ||
            ahl.find("pid=4321") == std::string::npos ||
            ahl.find("free=123MB") == std::string::npos ||
            ahl.find("largest=40MB") == std::string::npos) {
            ec = fail("check13d: human abort line content wrong", 167);
            return 0;
        }
        ordered_json abj = ordered_json::parse(ajl);
        if (abj["event"].get<std::string>() != "batch_aborted" ||
            abj["action"].get<std::string>() != "abort" ||
            abj["free_mb"].get<uint64_t>() != 123 ||
            abj["largest_free_mb"].get<uint64_t>() != 40) {
            ec = fail("check13d: JSON abort line content wrong", 168);
            return 0;
        }
        ordered_json mbj = ordered_json::parse(mjl);
        if (mbj["event"].get<std::string>() !=
                "whserver_vas_measure_failed" ||
            mbj["pid"].get<unsigned long>() != 4321UL) {
            ec = fail("check13d: JSON measure_failed line wrong", 169);
            return 0;
        }

        unsigned tv = 0;
        if (!parseMbThreshold("0", tv) || tv != 0 ||
            !parseMbThreshold("5000", tv) || tv != 5000 ||
            parseMbThreshold("x", tv) || parseMbThreshold("-1", tv) ||
            parseMbThreshold("", tv) || parseMbThreshold("1.5", tv) ||
            parseMbThreshold("1048576", tv)) {
            ec = fail("check13e: parseMbThreshold contract wrong", 170);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: VAS-guard abort path renders exit 14 + "
                     "truthful fields\n");
    }

    // ------------------------------------------------------------------
    // Check 14 (TVS-1c): serve-path VAS-guard refusal —
    //   (a) serveVasGuardRefuse returns false for a disabled guard, for a
    //       non-mutating op (exists/get-source are never VAS-gated), and
    //       for a mutating op with zero thresholds (breach requires a
    //       value STRICTLY below the threshold; 0 can never breach) — all
    //       deterministic, no WHServer dependency.
    //   (b) The refusal CliResult serializes like a normal op failure:
    //       exit_code=14, error.code="whserver_vas_abort" via
    //       errorCodeString, message carried verbatim — the contract the
    //       Python client aborts on.
    //   (c) R1: the attached-site deferred recycle boundary (engine NOT
    //       run — +7 m_nLock lasts the process's lifetime) serializes on
    //       the same exit-18 "whserver_recycled" frame the client
    //       respawns on.
    // ------------------------------------------------------------------
    {
        ServeRequest req;
        req.op      = "insert";
        req.typeStr = "function";
        req.name    = "F";

        VasGuardConfig disabled;
        disabled.enabled = false;
        VasGuardWarns warned;
        CliResult refusal;
        refusal.exitCode = -777;  // sentinel: must stay untouched
        if (serveVasGuardRefuse(req, disabled, warned, false, refusal) ||
            refusal.exitCode != -777) {
            ec = fail("check14a: disabled guard refused", 171);
            return 0;
        }

        ServeRequest ro;
        ro.op      = "exists";
        ro.typeStr = "function";
        ro.name    = "F";
        VasGuardConfig enabled;
        if (serveVasGuardRefuse(ro, enabled, warned, false, refusal)) {
            ec = fail("check14a: non-mutating op was VAS-gated", 172);
            return 0;
        }

        // Enabled guard + a mutating op but impossible thresholds (0):
        // every WHServer pid either fails to measure (warn-once, no
        // refusal) or reports free MB >= 0 -> never breaches. Deterministic
        // whether or not WHServer is running — the one environmental input
        // that CAN still refuse is a real crash-frozen WHServer on this
        // machine (the zombie pre-check fires regardless of thresholds;
        // genuinely absent in test environments).
        VasGuardConfig zero;
        zero.freeThresholdMB    = 0;
        zero.largestThresholdMB = 0;
        warned = VasGuardWarns();
        if (serveVasGuardRefuse(req, zero, warned, false, refusal)) {
            ec = fail("check14a: zero thresholds refused", 173);
            return 0;
        }

        // Refusal serialization: the exact CliResult shape
        // serveVasGuardRefuse fills on breach -> cliResultToJson must carry
        // exit_code=14 and error.code "whserver_vas_abort".
        CliResult ref;
        ref.op = "insert";
        ref.typeStr = "function";
        ref.name = "F";
        ref.success = false;
        ref.exitCode = 14;
        ref.errorMessage =
            "WHServer VAS headroom below threshold: free=10MB largest=4MB "
            "(whserver_vas_free=10MB); refusing insert before dispatch";
        ordered_json rj = cliResultToJson(ref);
        if (rj["exit_code"].get<int>() != 14 ||
            rj["success"].get<bool>() != false ||
            rj["error"]["code"].get<std::string>() != "whserver_vas_abort" ||
            rj["error"]["message"].get<std::string>() != ref.errorMessage ||
            rj["op"].get<std::string>() != "insert" ||
            rj["name"].get<std::string>() != "F") {
            ec = fail("check14b: refusal serialization wrong", 174);
            return 0;
        }

        // R1: the attached-site deferred recycle boundary serializes on
        // the same exit-18 frame the client respawns on — exit_code=18,
        // error.code "whserver_recycled" via errorCodeString (the frame
        // is the contract; the message says WHY deferred).
        CliResult def;
        def.op = "insert";
        def.typeStr = "function";
        def.name = "F";
        def.success = false;
        def.exitCode = 18;
        def.errorMessage =
            "WHServer recycle deferred (whserver_vas_free=10MB); "
            "resubmit on the respawned broker";
        ordered_json dj = cliResultToJson(def);
        if (dj["exit_code"].get<int>() != 18 ||
            dj["success"].get<bool>() != false ||
            dj["error"]["code"].get<std::string>() != "whserver_recycled" ||
            dj["error"]["message"].get<std::string>() != def.errorMessage) {
            ec = fail("check14c: deferred refusal serialization wrong",
                      175);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: serve VAS-guard refusal gate + exit-14 "
                     "serialization + R1 defer frame\n");
    }

    // ------------------------------------------------------------------
    // Check 15 (R6): bounded batch-control wait (design §5) — the decision
    //   read runs on a helper thread under a bounded wait; a stalled/open
    //   channel yields Error (cancel) instead of an unbounded orphan wait.
    //   runBoundedControlRead is exercised with INJECTED readers so the
    //   real thread + WaitForSingleObject machinery is covered without
    //   stdin; readBatchControlDecisionBounded is covered end-to-end with
    //   the FILE* pipe helper.
    //   (a) reader completing inside the bound -> decision AND error pass
    //       through verbatim.
    //   (b) reader blocked past the bound -> Error + nonempty diagnostic
    //       naming the bound (fail-closed cancel).
    //   (c) injected reader error -> Error + injected diagnostic verbatim.
    //   (d) readBatchControlDecisionBounded: timeoutSecs=0 -> plain
    //       blocking read (documented escape hatch); >0 -> bounded read;
    //       EOF channel still yields Error (EOF-as-cancel preserved).
    //   (e) parseTimeoutSecs shared contract.
    // ------------------------------------------------------------------
    {
        std::string err;
        BatchControlDecision d = runBoundedControlRead(
            readerReturnsContinue, nullptr, 5000, err);
        if (d != BatchControlDecision::Continue ||
            err != "reader-side diagnostic") {
            ec = fail("check15a: Continue pass-through broken", 180);
            return 0;
        }
        d = runBoundedControlRead(readerReturnsCancel, nullptr, 5000, err);
        if (d != BatchControlDecision::Cancel || !err.empty()) {
            ec = fail("check15a: Cancel pass-through broken", 181);
            return 0;
        }

        // Orphaned channel: reader blocks 60 s; the 150 ms bound must fire.
        DWORD blockMs = 60000;
        err.clear();
        const DWORD t0 = GetTickCount();
        d = runBoundedControlRead(readerBlocksForever, &blockMs, 150, err);
        const DWORD waited = GetTickCount() - t0;
        if (d != BatchControlDecision::Error) {
            ec = fail("check15b: bound expiry did not yield Error", 182);
            return 0;
        }
        if (err.empty() ||
            err.find("exceeded") == std::string::npos) {
            ec = fail("check15b: bound-expiry diagnostic missing", 183);
            return 0;
        }
        if (waited > 10000) {
            ec = fail("check15b: wait was not bounded", 184);
            return 0;
        }
        std::fprintf(stdout,
                     "  [15b] blocked reader -> Error after %lu ms: %s\n",
                     static_cast<unsigned long>(waited), err.c_str());

        err.clear();
        d = runBoundedControlRead(readerReturnsError, nullptr, 5000, err);
        if (d != BatchControlDecision::Error || err != "injected read failure") {
            ec = fail("check15c: injected error pass-through broken", 185);
            return 0;
        }

        // FILE* end-to-end: 0 disables the bound (plain blocking read).
        FILE* f = fileFromBytes("cancel\n");
        if (!f) {
            ec = fail("check15d: pipe setup failed", 186);
            return 0;
        }
        err.clear();
        d = readBatchControlDecisionBounded(f, 0, err);
        std::fclose(f);
        if (d != BatchControlDecision::Cancel) {
            ec = fail("check15d: timeoutSecs=0 did not read directly", 187);
            return 0;
        }
        // Bounded path: decision within the bound.
        f = fileFromBytes("continue\n");
        if (!f) {
            ec = fail("check15d: pipe setup failed", 188);
            return 0;
        }
        err.clear();
        d = readBatchControlDecisionBounded(f, 30, err);
        std::fclose(f);
        if (d != BatchControlDecision::Continue) {
            ec = fail("check15d: bounded read broke Continue", 189);
            return 0;
        }
        // EOF-as-cancel preserved through the bounded path.
        f = fileFromBytes("");
        if (!f) {
            ec = fail("check15d: pipe setup failed", 190);
            return 0;
        }
        err.clear();
        d = readBatchControlDecisionBounded(f, 30, err);
        std::fclose(f);
        if (d != BatchControlDecision::Error || err.empty()) {
            ec = fail("check15d: EOF did not yield Error", 191);
            return 0;
        }

        unsigned tv = 0;
        if (!parseTimeoutSecs("0", tv) || tv != 0 ||
            !parseTimeoutSecs("900", tv) || tv != 900 ||
            !parseTimeoutSecs("86400", tv) || tv != 86400 ||
            parseTimeoutSecs("86401", tv) || parseTimeoutSecs("-1", tv) ||
            parseTimeoutSecs("", tv) || parseTimeoutSecs("1.5", tv) ||
            parseTimeoutSecs("x", tv)) {
            ec = fail("check15e: parseTimeoutSecs contract wrong", 192);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: bounded control wait -- pass-through, "
                     "bound expiry (cancel), EOF-as-cancel, 0=unbounded\n");
    }

    // ------------------------------------------------------------------
    // Check 16 (P4 gaps a/g-1): zombie pre-check + enum-failure warning
    // seams —
    //   (a) whserverZombiePid: the first WHServer pid that parents a
    //       TSCrashReport.exe, or 0 — intersection order, non-matching
    //       parents, empty sets.
    //   (b) whserverZombieAbortLine / whserverEnumFailedLine render one
    //       physical line in both modes; JSON carries the honest fields
    //       (batch_aborted + reason "whserver_zombie"; the enum_failed
    //       event names).
    //   (c) the zombie refusal CliResult serializes like a normal op
    //       failure on the SHARED exit-14 guard class: exit_code=14,
    //       error.code="whserver_vas_abort" via errorCodeString (the
    //       Python client's batch-abort contract), and the message
    //       carries the whserver_zombie token + the frozen pid verbatim
    //       from whserverZombieRefusalMessage.
    //   (d) kZombieAbortMarkerMessage keeps exit 12 and stays distinct
    //       from the VAS-abort marker; batchCompletedLine carries
    //       abort_reason="whserver_zombie" verbatim.
    // ------------------------------------------------------------------
    {
        // (a) pure intersection decision.
        if (whserverZombiePid({111, 222}, {222, 333}) != 222 ||
            whserverZombiePid({111, 222}, {333}) != 0 ||
            whserverZombiePid({}, {111}) != 0 ||
            whserverZombiePid({111}, {}) != 0 ||
            whserverZombiePid({111, 222}, {999, 222}) != 222 ||
            whserverZombiePid({222, 111}, {222}) != 222) {
            ec = fail("check16a: whserverZombiePid intersection wrong", 200);
            return 0;
        }

        // (b) renderer shape — single physical lines, honest fields.
        std::string zhl = whserverZombieAbortLine(4321, false);
        std::string zjl = whserverZombieAbortLine(4321, true);
        std::string weh = whserverEnumFailedLine("whserver", false);
        std::string wej = whserverEnumFailedLine("whserver", true);
        std::string ceh = whserverEnumFailedLine("crashreport", false);
        std::string cej = whserverEnumFailedLine("crashreport", true);
        std::fprintf(stdout, "  [16b] zombie human: %s\n", zhl.c_str());
        std::fprintf(stdout, "  [16b] zombie json: %s\n", zjl.c_str());
        if (!isSingleLine(zhl) || !isSingleLine(zjl) ||
            !isSingleLine(weh) || !isSingleLine(wej) ||
            !isSingleLine(ceh) || !isSingleLine(cej)) {
            ec = fail("check16b: renderer emitted multi-line output", 201);
            return 0;
        }
        if (zhl.find("BATCH ABORTED") != 0 ||
            zhl.find("pid=4321") == std::string::npos ||
            zhl.find("zombie") == std::string::npos ||
            zhl.find("TSCrashReport") == std::string::npos ||
            zhl.find("free=") != std::string::npos) {  // no fake VAS numbers
            ec = fail("check16b: human zombie line content wrong", 202);
            return 0;
        }
        ordered_json zbj = ordered_json::parse(zjl);
        if (zbj["event"].get<std::string>() != "batch_aborted" ||
            zbj["reason"].get<std::string>() != "whserver_zombie" ||
            zbj["pid"].get<unsigned long>() != 4321UL ||
            zbj["action"].get<std::string>() != "abort") {
            ec = fail("check16b: JSON zombie line content wrong", 203);
            return 0;
        }
        ordered_json wbj = ordered_json::parse(wej);
        ordered_json cbj = ordered_json::parse(cej);
        if (wbj["event"].get<std::string>() != "whserver_enum_failed" ||
            cbj["event"].get<std::string>() != "crashreport_enum_failed" ||
            weh.find("whserver.enum_failed") == std::string::npos ||
            ceh.find("crashreport.enum_failed") == std::string::npos ||
            weh.find("; continuing)") == std::string::npos ||
            ceh.find("; continuing)") == std::string::npos) {
            ec = fail("check16b: enum_failed warning shape wrong", 204);
            return 0;
        }

        // (c) zombie refusal shape — same exit-14 class as the VAS
        // breach (the Python client aborts on exit 14 + error.code
        // "whserver_vas_abort"); the honest reason lives in the message.
        CliResult zref;
        zref.op = "insert";
        zref.typeStr = "function";
        zref.name = "F";
        zref.success = false;
        zref.exitCode = 14;
        zref.errorMessage = whserverZombieRefusalMessage("insert", 4321);
        ordered_json zrj = cliResultToJson(zref);
        if (zrj["exit_code"].get<int>() != 14 ||
            zrj["success"].get<bool>() != false ||
            zrj["error"]["code"].get<std::string>() !=
                "whserver_vas_abort" ||
            zrj["error"]["message"].get<std::string>() !=
                zref.errorMessage ||
            zref.errorMessage.find("(whserver_zombie)") ==
                std::string::npos ||
            zref.errorMessage.find("pid=4321") == std::string::npos ||
            zref.errorMessage.find("refusing insert before dispatch") ==
                std::string::npos) {
            ec = fail("check16c: zombie refusal serialization wrong", 205);
            return 0;
        }

        // (d) zombie marker + aborted summary carry the honest reason.
        BatchItem zitem;
        zitem.typeStr = "function";
        zitem.name    = "NeverRanZombie";
        CliResult zm = cancelledBatchItemResult("insert", zitem,
                                                kZombieAbortMarkerMessage);
        if (zm.exitCode != 12 || zm.success != false ||
            zm.errorMessage !=
                "Batch aborted (WHServer crash-frozen zombie) before this "
                "item was attempted" ||
            std::string(kZombieAbortMarkerMessage) ==
                kVasAbortMarkerMessage) {
            ec = fail("check16d: zombie abort marker wrong", 206);
            return 0;
        }
        std::string zsum = batchCompletedLine(
            "insert", /*completed=*/1, /*total=*/3, /*successes=*/1,
            /*failures=*/0, /*notRun=*/2, /*cancelled=*/false,
            /*closeFailed=*/false, /*exitCode=*/14, false,
            /*aborted=*/true, /*abortReason=*/"whserver_zombie");
        std::string zsumj = batchCompletedLine(
            "insert", 1, 3, 1, 0, 2, false, false, 14, true,
            true, "whserver_zombie");
        ordered_json zsj = ordered_json::parse(zsumj);
        if (zsum.find("abort_reason=whserver_zombie") ==
                std::string::npos ||
            zsj["abort_reason"].get<std::string>() != "whserver_zombie" ||
            zsj["exit_code"].get<int>() != 14) {
            ec = fail("check16d: zombie abort summary wrong", 207);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: zombie pre-check + enum-failure seams "
                     "render exit-14 refusal + honest fields\n");
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
                 "=== test_batch_progress: batch progress helpers (unit) ===\n");

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
                 "\n=== test_batch_progress complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
