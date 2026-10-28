// test_cache_cli_units.cpp
//
// Pure unit test for the --backup-cache / --restore-cache cache-phase
// helpers in src/tsdev_cli.cpp:
//
//   - bool parseCacheBackupPolicy(const std::string&, CacheBackupPolicy&);
//   - std::string cacheElapsedFormat(double elapsedSecs);
//   - std::string cacheProgressLine(op, done, total, elapsedSecs, fileBase);
//   - std::string cacheSpinnerLine(op, tick, elapsedSecs);
//   - std::string cachePipeProgressLine(op, done, total, elapsedSecs, jsonMode);
//   - std::string formatBytesHuman(uint64_t bytes);
//   - BackupAction decideBackupAction(const BackupInfo&, CacheBackupPolicy,
//     bool interactive);
//   - std::string existingBackupDetailLine(const std::wstring&,
//     const BackupInfo&);
//   - wchar_t cachePromptChoice(const std::wstring& line);
//   - bool backupCapturedLiveSource(const CacheOpResult&);
//   - const char* liveSourceSuffix(const CacheOpResult&);
//   - void applyLiveSourceJson(ordered_json&, const CacheOpResult&);
//   - ordered_json cacheErrorJson(op, exitCode, message);
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and performs no filesystem I/O (the
// renderer emitter and runCachePhase driver are NOT exercised here; only
// the pure builders are). The body runs on a 64 MB-stack worker thread only
// to match the project's test-harness convention.
//
// Linkage mechanism: identical to test_pbar.cpp — the helpers live inside
// tsdev_cli.cpp's anonymous namespace (internal linkage), so the test
// #defines TSDEV_CLI_TEST and #includes the .cpp directly (same TU; the
// file's main() is excluded by the TSDEV_CLI_TEST guard).
//
// Pinned formats:
//   cacheProgressLine: "<OP> [<30-cell bar>] <pct>% <M:SS|H:MM:SS>[ - <file>]"
//     (bar/pct geometry identical to pbarLine; never wider than 79 cells).
//   cacheSpinnerLine:  "<OP> <| / - \> <elapsed>" (ASCII only).
//   cachePipeProgressLine human: "<OP> PROGRESS pct=<n> bytes=<n> total=<n>
//     elapsed_secs=<n>"; JSON mode: {"event":"cache_backup_progress"|
//     "cache_restore_progress","bytes":..,"total":..,"pct":..,"elapsed_secs":..}.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
#define TSDEV_CLI_TEST

// Include the CLI source directly so the anonymous-namespace internals are
// visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

const std::string kFull   = "#";  // filled bar cell
const std::string kShade  = "-";  // empty bar cell
const std::string kEmDash = "-";  // separator dash

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Test-side display-cell width under the pinned rule (UTF-8 continuation
// bytes don't count) — same restatement as test_pbar.cpp.
size_t cellWidth(const std::string& s) {
    size_t cells = 0;
    for (unsigned char b : s)
        if ((b & 0xC0) != 0x80) ++cells;
    return cells;
}

std::string rep(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_cache_cli_units: cache-phase helpers (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: parseCacheBackupPolicy accepts all four values,
    // case-insensitively, and rejects unknown ones without touching `out`.
    // ------------------------------------------------------------------
    {
        CacheBackupPolicy p = CacheBackupPolicy::Abort;
        if (!parseCacheBackupPolicy("prompt", p) ||
            p != CacheBackupPolicy::Prompt)
            return ec = fail("check1: 'prompt'", 10), 0;
        if (!parseCacheBackupPolicy("Overwrite", p) ||
            p != CacheBackupPolicy::Overwrite)
            return ec = fail("check1: 'Overwrite' case-insensitive", 11), 0;
        if (!parseCacheBackupPolicy("SKIP", p) ||
            p != CacheBackupPolicy::Skip)
            return ec = fail("check1: 'SKIP'", 12), 0;
        if (!parseCacheBackupPolicy("abort", p) ||
            p != CacheBackupPolicy::Abort)
            return ec = fail("check1: 'abort'", 13), 0;
        if (parseCacheBackupPolicy("bogus", p) ||
            parseCacheBackupPolicy("", p) ||
            p != CacheBackupPolicy::Abort)
            return ec = fail("check1: invalid value not rejected cleanly",
                             14), 0;
        std::fprintf(stdout, "    PASS: policy parse\n");
    }

    // ------------------------------------------------------------------
    // Check 2: cacheElapsedFormat — M:SS below 1h, H:MM:SS at/above,
    // negatives clamp to 0:00, fractional seconds truncate.
    // ------------------------------------------------------------------
    {
        if (cacheElapsedFormat(0.0)    != "0:00")     return ec = fail("check2: 0", 20), 0;
        if (cacheElapsedFormat(7.9)    != "0:07")     return ec = fail("check2: 7.9", 21), 0;
        if (cacheElapsedFormat(65.0)   != "1:05")     return ec = fail("check2: 65", 22), 0;
        if (cacheElapsedFormat(3599.0) != "59:59")    return ec = fail("check2: 3599", 23), 0;
        if (cacheElapsedFormat(3600.0) != "1:00:00")  return ec = fail("check2: 3600", 24), 0;
        if (cacheElapsedFormat(3723.0) != "1:02:03")  return ec = fail("check2: 3723", 25), 0;
        if (cacheElapsedFormat(-5.0)   != "0:00")     return ec = fail("check2: negative", 26), 0;
        std::fprintf(stdout, "    PASS: elapsed format\n");
    }

    // ------------------------------------------------------------------
    // Check 3: cacheProgressLine geometry.
    //   cacheProgressLine("BACKUP", 450461696, 1964185600, 7.2, "")
    //   filled = floor(30*450461696/1964185600) = 6; pct = 22
    //   Expected: BACKUP [█x6░x24]  22% 0:07
    // ------------------------------------------------------------------
    {
        std::string line =
            cacheProgressLine("BACKUP", 450461696ULL, 1964185600ULL, 7.2, "");
        std::fprintf(stdout, "  [3] %s\n", line.c_str());
        const std::string want =
            "BACKUP [" + rep(kFull, 6) + rep(kShade, 24) + "]  22% 0:07";
        if (line != want) {
            return ec = fail("check3: determinate line bytes wrong", 30), 0;
        }
        std::fprintf(stdout, "    PASS: determinate bar exact\n");
    }

    // ------------------------------------------------------------------
    // Check 4: file suffix + truncation. A long basename is truncated on a
    // code-point boundary; the whole line stays <= 79 cells; a suffix that
    // cannot fit at all is dropped.
    //   cacheProgressLine("BACKUP", 5, 10, 3.0, <name>)
    //   prefix "BACKUP [...]  50% 0:03" = 49 cells; budget for " — <file>"
    //   is 30 cells -> " — " + 27 file cells.
    // ------------------------------------------------------------------
    {
        std::string line = cacheProgressLine("BACKUP", 5, 10, 3.0, "f.el");
        const std::string want =
            "BACKUP [" + rep(kFull, 15) + rep(kShade, 15) +
            "]  50% 0:03 " + kEmDash + " f.el";
        if (line != want) {
            return ec = fail("check4a: file suffix bytes wrong", 40), 0;
        }

        const std::string longName(60, 'x');
        line = cacheProgressLine("BACKUP", 5, 10, 3.0, longName);
        std::fprintf(stdout, "  [4] %s\n", line.c_str());
        const std::string wantTrunc =
            "BACKUP [" + rep(kFull, 15) + rep(kShade, 15) +
            "]  50% 0:03 " + kEmDash + " " + rep("x", 27);
        if (line != wantTrunc || cellWidth(line) != 79) {
            return ec = fail("check4b: truncation wrong", 41), 0;
        }

        // 76-cell prefix leaves budget 3 -> suffix dropped entirely.
        line = cacheProgressLine(rep("O", 33), 5, 10, 3.0, "f.el");
        const std::string wantDrop =
            rep("O", 33) + " [" + rep(kFull, 15) + rep(kShade, 15) +
            "]  50% 0:03";
        if (line != wantDrop) {
            return ec = fail("check4c: suffix not dropped on tiny budget",
                             42), 0;
        }
        std::fprintf(stdout, "    PASS: suffix append/truncate/drop\n");
    }

    // ------------------------------------------------------------------
    // Check 5: cacheProgressLine total==0 -> all shade, pct 0; done>total
    // clamps the bar but pct stays formulaic. Every rendered line is one
    // physical line of <=79 cells with no trailing space (sweep).
    // ------------------------------------------------------------------
    {
        std::string line = cacheProgressLine("RESTORE", 0, 0, 0.0, "");
        const std::string want =
            "RESTORE [" + rep(kShade, 30) + "]   0% 0:00";
        if (line != want) {
            return ec = fail("check5a: total==0 line wrong", 50), 0;
        }
        line = cacheProgressLine("BACKUP", 15, 10, 0.0, "");
        const std::string wantOver =
            "BACKUP [" + rep(kFull, 30) + "] 150% 0:00";
        if (line != wantOver) {
            return ec = fail("check5b: done>total line wrong", 51), 0;
        }
        for (uint64_t t : {0ULL, 1ULL, 10ULL, 1000000ULL}) {
            for (uint64_t d : {0ULL, t / 2, t, t * 2}) {
                const std::string l =
                    cacheProgressLine("BACKUP", d, t, 61.0, "some-file.el");
                if (l.empty() || l.find('\n') != std::string::npos ||
                    l.find('\r') != std::string::npos ||
                    cellWidth(l) > 79 || l.back() == ' ') {
                    return ec = fail("check5c: line invariant violated", 52),
                           0;
                }
            }
        }
        std::fprintf(stdout, "    PASS: edge geometry + invariant sweep\n");
    }

    // ------------------------------------------------------------------
    // Check 6: cacheSpinnerLine cycles | / - \ and is ASCII-only.
    // ------------------------------------------------------------------
    {
        if (cacheSpinnerLine("BACKUP", 0, 7.0) != "BACKUP | 0:07")
            return ec = fail("check6: tick 0", 60), 0;
        if (cacheSpinnerLine("BACKUP", 1, 7.0) != "BACKUP / 0:07")
            return ec = fail("check6: tick 1", 61), 0;
        if (cacheSpinnerLine("BACKUP", 2, 7.0) != "BACKUP - 0:07")
            return ec = fail("check6: tick 2", 62), 0;
        if (cacheSpinnerLine("BACKUP", 3, 7.0) != "BACKUP \\ 0:07")
            return ec = fail("check6: tick 3", 63), 0;
        if (cacheSpinnerLine("BACKUP", 4, 7.0) != "BACKUP | 0:07")
            return ec = fail("check6: tick wraps", 64), 0;
        std::fprintf(stdout, "    PASS: spinner ticks\n");
    }

    // ------------------------------------------------------------------
    // Check 7: cachePipeProgressLine — human key=value line and NDJSON.
    // ------------------------------------------------------------------
    {
        const std::string human = cachePipeProgressLine(
            "BACKUP", 450461696ULL, 1964185600ULL, 7.9, false);
        const std::string want =
            "BACKUP PROGRESS pct=22 bytes=450461696 total=1964185600 "
            "elapsed_secs=7";
        if (human != want) {
            return ec = fail("check7a: human PROGRESS line wrong", 70), 0;
        }
        const std::string js = cachePipeProgressLine(
            "RESTORE", 10ULL, 100ULL, 3.0, true);
        const nlohmann::json j = nlohmann::json::parse(js);
        if (!j.is_object() || j["event"] != "cache_restore_progress" ||
            j["bytes"] != 10 || j["total"] != 100 || j["pct"] != 10 ||
            j["elapsed_secs"] != 3) {
            return ec = fail("check7b: JSON PROGRESS line wrong", 71), 0;
        }
        const std::string jsB = cachePipeProgressLine(
            "BACKUP", 0ULL, 0ULL, 0.0, true);
        const nlohmann::json jb = nlohmann::json::parse(jsB);
        if (jb["event"] != "cache_backup_progress" || jb["pct"] != 0) {
            return ec = fail("check7c: JSON event name/pct wrong", 72), 0;
        }
        std::fprintf(stdout, "    PASS: pipe progress lines\n");
    }

    // ------------------------------------------------------------------
    // Check 8: formatBytesHuman.
    // ------------------------------------------------------------------
    {
        if (formatBytesHuman(0)          != "0 B")      return ec = fail("check8: 0", 80), 0;
        if (formatBytesHuman(512)        != "512 B")    return ec = fail("check8: 512", 81), 0;
        if (formatBytesHuman(1024)       != "1.00 KiB") return ec = fail("check8: 1 KiB", 82), 0;
        if (formatBytesHuman(1964185600ULL) != "1.83 GiB")
            return ec = fail("check8: GiB", 83), 0;
        std::fprintf(stdout, "    PASS: human bytes\n");
    }

    // ------------------------------------------------------------------
    // Check 9: decideBackupAction — the F4 decision seam
    // (exists/complete/policy/interactive -> action).
    // ------------------------------------------------------------------
    {
        BackupInfo info{};
        info.exists   = true;
        info.complete = true;
        info.created  = "2025-01-02 03:04:05";
        info.files    = 3;
        info.bytes    = 1024;
        info.source   = L"C:\\TS";

        if (decideBackupAction(info, CacheBackupPolicy::Prompt, true) !=
            BackupAction::Prompt)
            return ec = fail("check9: complete+prompt+interactive", 90), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Prompt, false) !=
            BackupAction::Skip)
            return ec = fail("check9: complete+prompt+non-interactive", 91), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Overwrite, false) !=
            BackupAction::Overwrite)
            return ec = fail("check9: complete+overwrite", 92), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Skip, true) !=
            BackupAction::Skip)
            return ec = fail("check9: complete+skip", 93), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Abort, true) !=
            BackupAction::Cancel)
            return ec = fail("check9: complete+abort", 94), 0;

        // exists && !complete: never Prompt/Skip — overwrite regardless of
        // policy EXCEPT abort -> Cancel.
        info.complete = false;
        if (decideBackupAction(info, CacheBackupPolicy::Prompt, true) !=
            BackupAction::Overwrite)
            return ec = fail("check9: incomplete+prompt+interactive", 95), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Prompt, false) !=
            BackupAction::Overwrite)
            return ec = fail("check9: incomplete+prompt+non-interactive",
                             96), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Skip, false) !=
            BackupAction::Overwrite)
            return ec = fail("check9: incomplete+skip", 97), 0;
        if (decideBackupAction(info, CacheBackupPolicy::Abort, false) !=
            BackupAction::Cancel)
            return ec = fail("check9: incomplete+abort", 98), 0;

        // !exists: any policy -> Overwrite (proceed silently).
        info.exists = false;
        info.complete = true;
        for (CacheBackupPolicy p :
             {CacheBackupPolicy::Prompt, CacheBackupPolicy::Overwrite,
              CacheBackupPolicy::Skip, CacheBackupPolicy::Abort}) {
            if (decideBackupAction(info, p, false) !=
                BackupAction::Overwrite)
                return ec = fail("check9: !exists", 99), 0;
        }
        std::fprintf(stdout, "    PASS: decideBackupAction\n");
    }

    // ------------------------------------------------------------------
    // Check 10: existingBackupDetailLine — exact bytes, source= appended
    // only when present.
    // ------------------------------------------------------------------
    {
        BackupInfo info{};
        info.exists   = true;
        info.complete = true;
        info.created  = "2025-01-02 03:04:05";
        info.files    = 42;
        info.bytes    = 1024;
        info.source   = L"C:\\Program Files (x86)\\TradeStation 9.5";

        const std::string with =
            existingBackupDetailLine(L"C:\\bk", info);
        const std::string wantWith =
            "path=C:\\bk\\cache created=2025-01-02 03:04:05 files=42 "
            "size=1.00 KiB "
            "source=C:\\Program Files (x86)\\TradeStation 9.5";
        if (with != wantWith)
            return ec = fail("check10: detail line with source", 100), 0;

        info.source.clear();
        const std::string without =
            existingBackupDetailLine(L"C:\\bk", info);
        const std::string wantWithout =
            "path=C:\\bk\\cache created=2025-01-02 03:04:05 files=42 "
            "size=1.00 KiB";
        if (without != wantWithout)
            return ec = fail("check10: detail line without source", 101), 0;
        std::fprintf(stdout, "    PASS: existingBackupDetailLine\n");
    }

    // ------------------------------------------------------------------
    // Check 11: cachePromptChoice — first non-whitespace char wins,
    // case-insensitive; empty/whitespace-only line (bare Enter) is 's';
    // anything else is L'\0' (invalid -> re-prompt).
    // ------------------------------------------------------------------
    {
        if (cachePromptChoice(L"o\r\n") != L'o')
            return ec = fail("check11: 'o'", 110), 0;
        if (cachePromptChoice(L"O\r\n") != L'o')
            return ec = fail("check11: 'O' case-insensitive", 111), 0;
        if (cachePromptChoice(L"s\r\n") != L's')
            return ec = fail("check11: 's'", 112), 0;
        if (cachePromptChoice(L"C\r\n") != L'c')
            return ec = fail("check11: 'C'", 113), 0;
        if (cachePromptChoice(L"\r\n") != L's')
            return ec = fail("check11: bare Enter", 114), 0;
        if (cachePromptChoice(L"") != L's')
            return ec = fail("check11: empty line", 115), 0;
        if (cachePromptChoice(L"   \r\n") != L's')
            return ec = fail("check11: whitespace-only line", 116), 0;
        if (cachePromptChoice(L"  o junk\r\n") != L'o')
            return ec = fail("check11: leading whitespace + key", 117), 0;
        if (cachePromptChoice(L"\tc\r\n") != L'c')
            return ec = fail("check11: tab + key", 118), 0;
        if (cachePromptChoice(L"x\r\n") != L'\0')
            return ec = fail("check11: invalid key", 119), 0;
        if (cachePromptChoice(L"quit\r\n") != L'\0')
            return ec = fail("check11: invalid word", 120), 0;
        std::fprintf(stdout, "    PASS: cachePromptChoice\n");
    }

    // ------------------------------------------------------------------
    // Check 12: torn-capture disclosure seams — backupCapturedLiveSource,
    // liveSourceSuffix, applyLiveSourceJson (additive-only keys).
    // ------------------------------------------------------------------
    {
        CacheOpResult res{};
        res.usedVss = false;
        res.usedBackupSemantics = false;
        if (backupCapturedLiveSource(res))
            return ec = fail("check12: false when neither flag", 121), 0;
        if (std::string(liveSourceSuffix(res)) != "")
            return ec = fail("check12: empty suffix when false", 122), 0;
        {
            ordered_json c;
            c["path"] = "x";
            applyLiveSourceJson(c, res);
            if (c.contains("live_source") || c.contains("used_vss"))
                return ec = fail("check12: keys must be omitted when false",
                                 123), 0;
        }

        res.usedBackupSemantics = true;
        if (!backupCapturedLiveSource(res))
            return ec = fail("check12: backup-semantics -> live", 124), 0;
        if (std::string(liveSourceSuffix(res)) != " live_source=true")
            return ec = fail("check12: suffix bytes", 125), 0;
        {
            ordered_json c;
            applyLiveSourceJson(c, res);
            if (!c.contains("live_source") || c["live_source"] != true ||
                c.contains("used_vss"))
                return ec = fail("check12: backup-semantics json", 126), 0;
        }

        res.usedVss = true;
        {
            ordered_json c;
            applyLiveSourceJson(c, res);
            if (!c.contains("live_source") || c["live_source"] != true ||
                !c.contains("used_vss") || c["used_vss"] != true)
                return ec = fail("check12: vss json", 127), 0;
        }
        std::fprintf(stdout, "    PASS: live-source disclosure\n");
    }

    // ------------------------------------------------------------------
    // Check 13: cacheErrorJson — the single stdout error object emitted by
    // every runCachePhase failure path under --json. CliResult shape:
    // {"op":<phase>,"success":false,"exit_code":<n>,
    //  "error":{"code":<from exit code>,"message":<msg>}}.
    // ------------------------------------------------------------------
    {
        const nlohmann::json j5 = cacheErrorJson(
            "restore-cache", 5, "no cache backup found at C:\\bk");
        if (!j5.is_object() || j5["op"] != "restore-cache" ||
            j5["success"] != false || j5["exit_code"] != 5 ||
            j5["error"]["code"] != "not_found" ||
            j5["error"]["message"] != "no cache backup found at C:\\bk") {
            return ec = fail("check13: exit-5 object shape", 130), 0;
        }
        const nlohmann::json j16 = cacheErrorJson(
            "backup-cache", 16, "cache backup failed: x y (err=5)");
        if (j16["op"] != "backup-cache" || j16["exit_code"] != 16 ||
            j16["error"]["code"] != "cache_io_error" ||
            j16["error"]["message"] != "cache backup failed: x y (err=5)") {
            return ec = fail("check13: exit-16 object shape", 131), 0;
        }
        const nlohmann::json j17 = cacheErrorJson(
            "backup-cache+restore-cache", 17, "cache backup cancelled");
        if (j17["op"] != "backup-cache+restore-cache" ||
            j17["exit_code"] != 17 ||
            j17["error"]["code"] != "cache_cancelled" ||
            j17["error"]["message"] != "cache backup cancelled") {
            return ec = fail("check13: exit-17 object shape", 132), 0;
        }
        std::fprintf(stdout, "    PASS: cacheErrorJson\n");
    }

    std::fprintf(stdout, "test_cache_cli_units: ALL PASS\n");
    return 0;
}

} // namespace

int main() {
    TestState st;
    HANDLE h = CreateThread(nullptr, 64 * 1024 * 1024, runTest, &st, 0,
                            nullptr);
    if (!h) {
        std::fprintf(stderr, "test_cache_cli_units: CreateThread failed\n");
        return 90;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return st.exitCode;
}
