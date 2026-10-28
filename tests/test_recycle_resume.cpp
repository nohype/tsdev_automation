// test_recycle_resume.cpp
//
// Pure unit test for the R4 batch resume contract in src/tsdev_cli.cpp
// (design tmp/RECYCLE_ENGINE_DESIGN.md §4.1):
//
//   - whserverRecycleEventLine(rec, json, pid, free_mb, index, total,
//         manifest)                                   — full field set
//   - writeResumeManifest(dir, items, firstIndex, needsSource,
//         outPath, err)                               — §4.1 step-2 writer
//   - cancelledBatchItemResult(op, item, message)     — marker shape
//   - batchCompletedLine(..., aborted, abortReason)   — recycle summary
//   - RecycleConfig tri-state defaults + mode->vasGuardAction wiring
//     (check 16, tri-state authorization addendum)
//
// Round-trip guarantee under test: a resume manifest written by
// writeResumeManifest is re-read by parseManifest() with the SAME schema,
// so resubmitting `tsdev_cli batch-<op> <resume-file>` re-attempts exactly
// the not-run tail. A tail containing a preValid=false item is REFUSED
// outright (fail-closed — a fabricated placeholder name must never become
// a dispatchable line); representable preValid items round-trip and
// reproduce the same pre-validation failure instead of being dropped.
//
// This is a PURE UNIT TEST — no orlib20, no TradeStation contact. The only
// filesystem mutation is a scratch directory + files under %TEMP%,
// deleted afterwards.
//
// Linkage mechanism: identical to test_recycle_whserver_flag.cpp — the
// helpers live inside tsdev_cli.cpp's anonymous namespace (internal
// linkage), so the test #defines TSDEV_CLI_TEST and #includes the .cpp
// directly (same TU; the file's main() is excluded by the guard).

#define TSDEV_CLI_TEST

// Include the CLI source directly so the anonymous-namespace internals are
// visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

struct TestState {
    int exitCode = 0;
};

// Write a small scratch file; returns its path.
std::string writeScratch(const std::filesystem::path& dir,
                         const char* name, const char* content) {
    const std::filesystem::path p = dir / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content, static_cast<std::streamsize>(std::strlen(content)));
    return p.string();
}

// Scripted IRecycleSampler for the R2 defer-boundary checks
// (recycleDeferredResult): only zombiePoll is consumed (the pre-close
// corpse gate); census/death streams are never reached and return
// stream-exhausted. `zombies` scripts the gate polls — the default is a
// generous clean stream (snapshotOk + crashSnapshotOk, no zombie).
struct DeferSampler : tsdev::IRecycleSampler {
    std::vector<tsdev::RecycleZombiePoll> zombies;
    size_t zi = 0;
    int    zombiePolls = 0;

    DeferSampler() {
        tsdev::RecycleZombiePoll z;
        z.snapshotOk = true;
        z.crashSnapshotOk = true;
        zombies.assign(4, z);
    }

    bool censusPoll(tsdev::CensusPoll&) override { return false; }
    bool zombiePoll(tsdev::RecycleZombiePoll& out) override {
        ++zombiePolls;
        if (zi >= zombies.size()) return false;
        out = zombies[zi++];
        return true;
    }
    bool deathPoll(const std::vector<DWORD>&,
                   tsdev::RecycleDeathPoll&) override { return false; }
    double nowSecs() override { return 0.0; }
    void   sleepMs(unsigned) override {}
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_recycle_resume: R4 batch resume contract "
                 "(unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: whserverRecycleEventLine — JSON full field set for a
    // between-items exit-18 boundary (recycled) and exit-19 (refused).
    // ------------------------------------------------------------------
    {
        tsdev::RecycleResult rec;
        rec.outcome  = tsdev::RecycleOutcome::Recycled;
        rec.exitCode = 18;
        rec.reason   = "whserver_recycled";
        rec.detail   = "pid=4242 gone";

        const std::string j = whserverRecycleEventLine(
            rec, /*jsonOutput=*/true, /*whPid=*/4242, /*freeMB=*/10,
            /*boundaryIndex=*/3, /*total=*/5,
            /*manifestPath=*/R"(C:\tmp\tsdev_resume_1_2.manifest)");
        const char* want[] = {
            "\"event\":\"whserver_recycled\"",
            "\"reason\":\"whserver_recycled\"",
            "\"exit_code\":18",
            "\"pid\":4242",
            "\"free_mb\":10",
            "\"index\":3",
            "\"total\":5",
            "\"manifest\":\"C:\\\\tmp\\\\tsdev_resume_1_2.manifest\"",
        };
        for (const char* w : want) {
            if (j.find(w) == std::string::npos) {
                ec = fail("check1: recycled JSON event missing field", 10);
                return 0;
            }
        }

        rec.outcome  = tsdev::RecycleOutcome::RefuseCensus;
        rec.exitCode = 19;
        rec.reason   = "census_lock";
        rec.detail   = "pid=4242 lock=1";
        const std::string jr = whserverRecycleEventLine(
            rec, /*jsonOutput=*/true, /*whPid=*/4242, /*freeMB=*/10,
            /*boundaryIndex=*/3, /*total=*/5,
            /*manifestPath=*/"m.manifest");
        if (jr.find("\"event\":\"whserver_recycle_refused\"") ==
                std::string::npos ||
            jr.find("\"reason\":\"census_lock\"") == std::string::npos ||
            jr.find("\"exit_code\":19") == std::string::npos) {
            ec = fail("check1: refused JSON event wrong", 11);
            return 0;
        }
        // census_lock human wording: the detail carries a plain-English
        // lead-in (another TradeStation program is using the server) so
        // a bare token never reaches the user unexplained; the engine's
        // diagnostic detail stays appended. The reason token itself is
        // byte-pinned above.
        if (jr.find("another TradeStation program") ==
                std::string::npos ||
            jr.find("pid=4242 lock=1") == std::string::npos) {
            ec = fail("check1: census_lock detail wording wrong", 13);
            return 0;
        }

        // Minimal call (serve path / pre-flight continue): only the base
        // fields exist — no pid/free_mb/index/manifest members.
        const std::string jmin = whserverRecycleEventLine(rec, true);
        if (jmin.find("\"pid\"") != std::string::npos ||
            jmin.find("\"free_mb\"") != std::string::npos ||
            jmin.find("\"index\"") != std::string::npos ||
            jmin.find("\"manifest\"") != std::string::npos) {
            ec = fail("check1: minimal JSON event carries extra fields", 12);
            return 0;
        }
        std::fprintf(stdout, "    PASS: JSON event field set\n");
    }

    // ------------------------------------------------------------------
    // Check 2: whserverRecycleEventLine — human rendering carries the
    // same fields.
    // ------------------------------------------------------------------
    {
        tsdev::RecycleResult rec;
        rec.outcome  = tsdev::RecycleOutcome::Recycled;
        rec.exitCode = 18;
        rec.reason   = "whserver_recycled";
        const std::string h = whserverRecycleEventLine(
            rec, /*jsonOutput=*/false, /*whPid=*/4242, /*freeMB=*/10,
            /*boundaryIndex=*/3, /*total=*/5, /*manifestPath=*/"m.manifest");
        const char* want[] = {
            "tsdev_cli: whserver.recycled",
            "reason=whserver_recycled",
            "exit=18",
            "pid=4242",
            "free_mb=10",
            "index=3",
            "total=5",
            "manifest=m.manifest",
        };
        for (const char* w : want) {
            if (h.find(w) == std::string::npos) {
                ec = fail("check2: human event missing field", 20);
                return 0;
            }
        }
        const std::string href = whserverRecycleEventLine(
            rec, false, 0, 0, 0, 0, "");
        rec.outcome = tsdev::RecycleOutcome::RefuseCensus;
        const std::string hmin = whserverRecycleEventLine(rec, false);
        if (hmin.find("whserver.recycle_refused") == std::string::npos) {
            ec = fail("check2: human refused event wrong", 21);
            return 0;
        }
        (void)href;
        std::fprintf(stdout, "    PASS: human event rendering\n");
    }

    // ------------------------------------------------------------------
    // Check 3: writeResumeManifest round-trip (needsSource insert op) —
    // parseManifest re-reads the exact tail.
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);
        if (fsEc) {
            ec = fail("check3: scratch dir create failed", 30);
            return 0;
        }
        const std::string srcA =
            writeScratch(dir, "a.el", "variables: int x(0);\nx = 1;\n");
        const std::string srcB =
            writeScratch(dir, "b.el", "variables: int y(0);\ny = 2;\n");

        std::vector<BatchItem> items;
        BatchItem i1; i1.typeStr = "function"; i1.name = "Done1";
        i1.sourceFile = srcA; items.push_back(i1);
        BatchItem i2; i2.typeStr = "function"; i2.name = "Tail1";
        i2.sourceFile = srcA; items.push_back(i2);
        BatchItem i3; i3.typeStr = "indicator"; i3.name = "Tail2";
        i3.sourceFile = srcB; items.push_back(i3);

        std::string path, err;
        if (!writeResumeManifest(dir.string(), items, /*firstIndex=*/1,
                                 /*needsSource=*/true, path, err)) {
            ec = fail("check3: writeResumeManifest failed", 31);
            return 0;
        }
        const std::string fname =
            std::filesystem::path(path).filename().string();
        if (fname.rfind("tsdev_resume_", 0) != 0 ||
            fname.size() < 10 ||
            fname.compare(fname.size() - 9, 9, ".manifest") != 0) {
            ec = fail("check3: resume filename shape wrong", 32);
            return 0;
        }
        std::vector<BatchItem> back;
        if (!parseManifest(path, /*needsSource=*/true, back)) {
            ec = fail("check3: parseManifest could not re-read", 33);
            return 0;
        }
        if (back.size() != 2 ||
            back[0].typeStr != "function" || back[0].name != "Tail1" ||
            back[0].sourceFile != srcA || !back[0].preValid ||
            back[0].source.empty() ||
            back[1].typeStr != "indicator" || back[1].name != "Tail2" ||
            back[1].sourceFile != srcB || !back[1].preValid) {
            ec = fail("check3: round-trip items mismatch", 34);
            return 0;
        }
        std::filesystem::remove(path, fsEc);
        std::fprintf(stdout, "    PASS: needsSource round-trip\n");
    }

    // ------------------------------------------------------------------
    // Check 4: delete-op round-trip (2-token lines) + failure-reproducing
    // items (unknown type / empty name / missing source_file).
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4b_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);

        std::vector<BatchItem> items;
        BatchItem d1; d1.typeStr = "strategy"; d1.name = "Del1";
        items.push_back(d1);
        BatchItem bad; bad.typeStr = "bogus"; bad.name = "X";
        items.push_back(bad);
        BatchItem noname; noname.typeStr = "indicator"; noname.name = "";
        items.push_back(noname);

        std::string path, err;
        if (!writeResumeManifest(dir.string(), items, 0,
                                 /*needsSource=*/false, path, err)) {
            ec = fail("check4: writeResumeManifest(delete) failed", 40);
            return 0;
        }
        std::vector<BatchItem> back;
        if (!parseManifest(path, /*needsSource=*/false, back) ||
            back.size() != 3) {
            ec = fail("check4: delete round-trip size wrong", 41);
            return 0;
        }
        if (!back[0].preValid || back[0].typeStr != "strategy" ||
            back[0].name != "Del1") {
            ec = fail("check4: delete round-trip item wrong", 42);
            return 0;
        }
        // The unknown-type item reproduces the SAME pre-validation
        // failure (exit 2, preValid=false) instead of being dropped.
        if (back[1].preValid || back[1].result.exitCode != 2 ||
            back[1].typeStr != "bogus") {
            ec = fail("check4: unknown-type item not reproduced", 43);
            return 0;
        }
        // The empty-name item writes "<type>" alone -> same
        // "name must not be empty" failure on re-parse.
        if (back[2].preValid || back[2].result.exitCode != 2 ||
            back[2].name != "") {
            ec = fail("check4: empty-name item not reproduced", 44);
            return 0;
        }
        std::filesystem::remove(path, fsEc);

        // A needsSource item whose source_file was absent on the original
        // line reproduces "manifest line missing <source_file>" (exit 1).
        std::vector<BatchItem> items2;
        BatchItem nosrc; nosrc.typeStr = "indicator"; nosrc.name = "N1";
        items2.push_back(nosrc);
        std::string path2;
        if (!writeResumeManifest(dir.string(), items2, 0,
                                 /*needsSource=*/true, path2, err)) {
            ec = fail("check4: writeResumeManifest(nosrc) failed", 45);
            return 0;
        }
        std::vector<BatchItem> back2;
        if (!parseManifest(path2, /*needsSource=*/true, back2) ||
            back2.size() != 1 || back2[0].preValid ||
            back2[0].result.exitCode != 1 ||
            back2[0].result.errorMessage.find("missing <source_file>") ==
                std::string::npos) {
            ec = fail("check4: missing-source item not reproduced", 46);
            return 0;
        }
        std::filesystem::remove(path2, fsEc);
        std::filesystem::remove_all(dir, fsEc);
        std::fprintf(stdout, "    PASS: delete round-trip + failure "
                             "reproduction\n");
    }

    // ------------------------------------------------------------------
    // Check 5: writeResumeManifest failure surface — an unwritable dir
    // returns false with a diagnostic and no path (fail-closed, never a
    // partial/phantom manifest).
    // ------------------------------------------------------------------
    {
        std::vector<BatchItem> items;
        BatchItem it; it.typeStr = "function"; it.name = "X";
        it.sourceFile = "a.el"; items.push_back(it);
        std::string path, err;
        if (writeResumeManifest("Z:\\tsdev_r4_no_such_dir_zzz\\sub",
                                items, 0, true, path, err)) {
            ec = fail("check5: unwritable dir accepted", 50);
            return 0;
        }
        if (err.empty() || !path.empty()) {
            ec = fail("check5: failure carries no diagnostic", 51);
            return 0;
        }
        std::fprintf(stdout, "    PASS: writer fail-closed\n");
    }

    // ------------------------------------------------------------------
    // Check 6: recycle marker shape — cancelledBatchItemResult with the
    // pinned recycle messages (exit 12 / "cancelled", verbatim message,
    // op/type/name preserved).
    // ------------------------------------------------------------------
    {
        BatchItem it; it.typeStr = "function"; it.name = "Tail9";
        CliResult r1 = cancelledBatchItemResult("insert", it,
                                                kRecycledMarkerMessage);
        CliResult r2 = cancelledBatchItemResult("insert", it,
                                                kRecycleRefusedMarkerMessage);
        if (r1.exitCode != 12 || r1.success ||
            r1.errorMessage != "WHServer recycled; resubmit remaining items" ||
            r1.op != "insert" || r1.typeStr != "function" ||
            r1.name != "Tail9" ||
            r2.errorMessage !=
                "WHServer recycle refused before this item was attempted") {
            ec = fail("check6: recycle marker shape wrong", 60);
            return 0;
        }
        // The pinned messages stay distinct from the VAS-abort marker so
        // the array says WHY.
        if (std::string(kRecycledMarkerMessage) == kVasAbortMarkerMessage ||
            std::string(kRecycleRefusedMarkerMessage) ==
                kVasAbortMarkerMessage) {
            ec = fail("check6: marker messages collide", 61);
            return 0;
        }
        std::fprintf(stdout, "    PASS: recycle markers\n");
    }

    // ------------------------------------------------------------------
    // Check 7: batch_completed on a recycle exit — the aborted summary
    // carries aborted/abort_reason + the recycle exit code.
    // ------------------------------------------------------------------
    {
        const std::string j = batchCompletedLine(
            "insert", /*completed=*/2, /*total=*/5, /*successes=*/1,
            /*failures=*/1, /*notRun=*/3, /*cancelled=*/false,
            /*closeFailed=*/false, /*exitCode=*/18, /*jsonOutput=*/true,
            /*aborted=*/true, /*abortReason=*/"whserver_vas_free=10MB");
        const char* want[] = {
            "\"event\":\"batch_completed\"",
            "\"completed\":2",
            "\"not_run\":3",
            "\"aborted\":true",
            "\"abort_reason\":\"whserver_vas_free=10MB\"",
            "\"exit_code\":18",
        };
        for (const char* w : want) {
            if (j.find(w) == std::string::npos) {
                ec = fail("check7: aborted summary missing field", 70);
                return 0;
            }
        }
        const std::string h = batchCompletedLine(
            "insert", 2, 5, 1, 1, 3, false, false, 18, false,
            true, "whserver_vas_free=10MB");
        if (h.find("BATCH COMPLETE ABORTED") == std::string::npos ||
            h.find("abort_reason=whserver_vas_free=10MB") ==
                std::string::npos ||
            h.find("exit_code=18") == std::string::npos) {
            ec = fail("check7: human aborted summary wrong", 71);
            return 0;
        }
        std::fprintf(stdout, "    PASS: aborted batch_completed\n");
    }

    // ------------------------------------------------------------------
    // Check 8: whitespace names round-trip through the quoted-name form —
    // dir batches derive the name verbatim from the file stem, so a
    // spaced stem must resume as the SAME component (insert case: the
    // item re-attempts under its real name; delete case: the resumed
    // delete hits the intended component, not a whitespace-truncated
    // one). An unterminated quote must fail pre-validation, never
    // silently re-tokenize.
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4c_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);
        const std::string src =
            writeScratch(dir, "my item.el", "variables: int z(0);\nz = 3;\n");

        // insert/modify form (3 tokens, quoted name).
        std::vector<BatchItem> items;
        BatchItem i1; i1.typeStr = "indicator"; i1.name = "my item";
        i1.sourceFile = src; items.push_back(i1);
        std::string path, err;
        if (!writeResumeManifest(dir.string(), items, 0,
                                 /*needsSource=*/true, path, err)) {
            ec = fail("check8: spaced-name write failed", 80);
            return 0;
        }
        {
            std::ifstream mf(path, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(mf)),
                                      std::istreambuf_iterator<char>());
            if (content.find("indicator \"my item\"") == std::string::npos) {
                ec = fail("check8: name not quoted in manifest", 81);
                return 0;
            }
        }
        std::vector<BatchItem> back;
        if (!parseManifest(path, /*needsSource=*/true, back) ||
            back.size() != 1 || !back[0].preValid ||
            back[0].name != "my item" || back[0].sourceFile != src) {
            ec = fail("check8: spaced insert item did not round-trip", 82);
            return 0;
        }
        std::filesystem::remove(path, fsEc);

        // delete form (2 tokens, quoted name).
        std::vector<BatchItem> ditems;
        BatchItem d1; d1.typeStr = "strategy"; d1.name = "del me now";
        ditems.push_back(d1);
        std::string dpath;
        if (!writeResumeManifest(dir.string(), ditems, 0,
                                 /*needsSource=*/false, dpath, err)) {
            ec = fail("check8: spaced delete write failed", 83);
            return 0;
        }
        std::vector<BatchItem> dback;
        if (!parseManifest(dpath, /*needsSource=*/false, dback) ||
            dback.size() != 1 || !dback[0].preValid ||
            dback[0].name != "del me now") {
            ec = fail("check8: spaced delete item did not round-trip", 84);
            return 0;
        }
        std::filesystem::remove(dpath, fsEc);

        // Unterminated quote -> pre-validation failure (fail-closed).
        std::vector<BatchItem> malformed;
        parseManifestLines("function \"broken name C:/x.el\n",
                           /*needsSource=*/true, malformed);
        if (malformed.size() != 1 || malformed[0].preValid ||
            malformed[0].result.exitCode != 2 ||
            malformed[0].result.errorMessage.find("unterminated") ==
                std::string::npos) {
            ec = fail("check8: unterminated quote not refused", 85);
            return 0;
        }
        // Junk fused after the closing quote is equally fail-closed:
        // `delete "a"b` must NOT silently retarget to a delete of name
        // `a` — a quote-leading name token that is not a well-formed
        // quoted token is a pre-validation error, never a partial
        // reinterpretation.
        std::vector<BatchItem> malformed2;
        parseManifestLines("strategy \"a\"b\n",
                           /*needsSource=*/false, malformed2);
        if (malformed2.size() != 1 || malformed2[0].preValid ||
            malformed2[0].result.exitCode != 2 ||
            malformed2[0].result.errorMessage.find("malformed") ==
                std::string::npos) {
            ec = fail("check8: junk-after-quote not refused", 86);
            return 0;
        }
        // And the same fused junk on a needsSource line.
        std::vector<BatchItem> malformed3;
        parseManifestLines("function \"a\"b C:/x.el\n",
                           /*needsSource=*/true, malformed3);
        if (malformed3.size() != 1 || malformed3[0].preValid ||
            malformed3[0].result.exitCode != 2) {
            ec = fail("check8: junk-after-quote (source form) not "
                      "refused", 87);
            return 0;
        }
        std::filesystem::remove_all(dir, fsEc);
        std::fprintf(stdout, "    PASS: whitespace-name round-trip "
                             "(quoted) + malformed-quote refusal\n");
    }

    // ------------------------------------------------------------------
    // Check 9: the writer refuses ONLY truly unrepresentable names
    // fail-closed — a line break, a leading '"', or '"' combined with
    // whitespace (unquoted it would split, quoted it would terminate
    // early). A bare '"' inside a whitespace-free name IS representable
    // unquoted (it re-parses as a plain token) and must round-trip, not
    // be refused. Refusal happens BEFORE any file exists.
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4d_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);

        // Representable: 'has"quote' (no whitespace, not quote-leading)
        // is written UNQUOTED and re-parses to the same name.
        std::vector<BatchItem> items;
        BatchItem ok; ok.typeStr = "function"; ok.name = "Fine";
        items.push_back(ok);
        BatchItem q; q.typeStr = "function"; q.name = "has\"quote";
        items.push_back(q);
        std::string path, err;
        if (!writeResumeManifest(dir.string(), items, 0,
                                 /*needsSource=*/false, path, err)) {
            ec = fail("check9: representable quote-in-name refused", 90);
            return 0;
        }
        std::vector<BatchItem> back;
        if (!parseManifest(path, /*needsSource=*/false, back) ||
            back.size() != 2 || !back[1].preValid ||
            back[1].name != "has\"quote") {
            ec = fail("check9: quote-in-name did not round-trip", 91);
            return 0;
        }
        std::filesystem::remove(path, fsEc);

        // Unrepresentable: '"' combined with whitespace. (path is
        // cleared first — the writer only sets outPath on success.)
        items[1].name = "has \"quote";
        path.clear();
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check9: space+quote name write accepted", 92);
            return 0;
        }
        if (err.find("has \"quote") == std::string::npos ||
            !path.empty()) {
            ec = fail("check9: refusal does not name the item", 93);
            return 0;
        }
        // Unrepresentable: quote-LEADING (would re-parse as a quoted
        // token even without whitespace).
        items[1].name = "\"leading";
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check9: leading-quote name write accepted", 94);
            return 0;
        }
        // A line break is equally unrepresentable.
        items[1].name = "has\nbreak";
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check9: line-break name write accepted", 95);
            return 0;
        }
        // No manifest may have been created by any refusal.
        bool anyManifest = false;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().filename().string().rfind("tsdev_resume_", 0) == 0) {
                anyManifest = true;
            }
        }
        if (anyManifest) {
            ec = fail("check9: partial manifest left behind", 96);
            return 0;
        }
        std::filesystem::remove_all(dir, fsEc);
        std::fprintf(stdout, "    PASS: unrepresentable name fail-closed\n");
    }

    // ------------------------------------------------------------------
    // Check 10: '\v'/'\f' are manifest whitespace too — kManifestWs is the
    // C-locale std::isspace set (" \t\n\v\f\r"), shared by trim(), token
    // splitting, and the writer's quoting/refusal predicates. A name
    // containing '\v'/'\f' must be written QUOTED and re-parse to the
    // SAME component (an unquoted '\v' at a token edge would silently
    // retarget a resumed delete); '"' combined with '\v'/'\f' is refused
    // exactly like '"'+space.
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4e_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);
        const std::string src =
            writeScratch(dir, "s.el", "variables: int w(0);\nw = 4;\n");

        // Interior '\v', leading '\f', trailing '\v' — all quoted, all
        // preserved verbatim through the quoted-name form.
        std::vector<BatchItem> items;
        BatchItem v1; v1.typeStr = "indicator"; v1.name = "a\vb";
        v1.sourceFile = src; items.push_back(v1);
        BatchItem v2; v2.typeStr = "indicator"; v2.name = "\fc";
        v2.sourceFile = src; items.push_back(v2);
        BatchItem v3; v3.typeStr = "indicator"; v3.name = "d\v";
        v3.sourceFile = src; items.push_back(v3);
        std::string path, err;
        if (!writeResumeManifest(dir.string(), items, 0,
                                 /*needsSource=*/true, path, err)) {
            ec = fail("check10: \\v/\\f-name write failed", 100);
            return 0;
        }
        {
            std::ifstream mf(path, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(mf)),
                                      std::istreambuf_iterator<char>());
            if (content.find("indicator \"a\vb\"") == std::string::npos ||
                content.find("indicator \"\fc\"") == std::string::npos ||
                content.find("indicator \"d\v\"") == std::string::npos) {
                ec = fail("check10: \\v/\\f names not quoted", 101);
                return 0;
            }
        }
        std::vector<BatchItem> back;
        if (!parseManifest(path, /*needsSource=*/true, back) ||
            back.size() != 3 ||
            !back[0].preValid || back[0].name != "a\vb" ||
            back[0].sourceFile != src ||
            !back[1].preValid || back[1].name != "\fc" ||
            !back[2].preValid || back[2].name != "d\v") {
            ec = fail("check10: \\v/\\f names did not round-trip", 102);
            return 0;
        }
        std::filesystem::remove(path, fsEc);

        // delete form (2 tokens): a '\v'-name must round-trip quoted —
        // never re-parse to a whitespace-truncated different component.
        std::vector<BatchItem> ditems;
        BatchItem d1; d1.typeStr = "strategy"; d1.name = "a\vb";
        ditems.push_back(d1);
        std::string dpath;
        if (!writeResumeManifest(dir.string(), ditems, 0,
                                 /*needsSource=*/false, dpath, err)) {
            ec = fail("check10: \\v delete write failed", 103);
            return 0;
        }
        std::vector<BatchItem> dback;
        if (!parseManifest(dpath, /*needsSource=*/false, dback) ||
            dback.size() != 1 || !dback[0].preValid ||
            dback[0].name != "a\vb") {
            ec = fail("check10: \\v delete item retargeted", 104);
            return 0;
        }
        std::filesystem::remove(dpath, fsEc);

        // '"' combined with '\v'/'\f' refuses fail-closed, exactly like
        // '"'+space (unquoted it would split at the '\v'; quoted the '"'
        // terminates the token early) — and '\r'/'\n' refusal is
        // unchanged.
        items[1].name = "a\"\vb";
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check10: quote+\\v name write accepted", 105);
            return 0;
        }
        items[1].name = "a\"\fb";
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check10: quote+\\f name write accepted", 106);
            return 0;
        }
        items[1].name = "a\rb";
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check10: \\r name write accepted", 107);
            return 0;
        }
        std::filesystem::remove_all(dir, fsEc);
        std::fprintf(stdout, "    PASS: \\v/\\f whitespace round-trip "
                             "+ refusal\n");
    }

    // ------------------------------------------------------------------
    // Check 11: the writer NEVER emits a dispatchable line for a
    // preValid=false tail item — refuse-on-preValid-false (fail-closed).
    // The canonical case is the non-ANSI-filename placeholder item from
    // parseDirectory (a name containing a space would otherwise be
    // written QUOTED and a resumed batch-delete-dir would dispatch a
    // delete against a fabricated name). Malformed-quote items
    // (unterminated quote / junk after the closing quote) are also
    // preValid=false — the same refusal covers them, so a resubmission
    // can never silently lose the original diagnostic.
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4f_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);

        // Mirror the parseDirectory catch item shape verbatim: a spaced
        // placeholder name + exit-3 pre-validation failure.
        std::vector<BatchItem> items;
        BatchItem good; good.typeStr = "function"; good.name = "Fine";
        items.push_back(good);
        BatchItem ph; ph.type = ComponentType::Function;
        ph.typeStr = "function";
        ph.name = "<unrepresentable filename: \xE6\x96\x87.txt>";
        ph.preValid = false;
        ph.result.typeStr = "function";
        ph.result.name = ph.name;
        ph.result.exitCode = 3;
        ph.result.errorMessage =
            "failed to convert the directory entry path to a narrow "
            "string (non-ANSI filename)";
        items.push_back(ph);

        std::string path, err;
        if (writeResumeManifest(dir.string(), items, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check11: preValid-false tail write accepted", 110);
            return 0;
        }
        if (err.empty() || !path.empty()) {
            ec = fail("check11: refusal carries no diagnostic", 111);
            return 0;
        }
        // The refusal must identify the item, never emit a manifest.
        if (err.find("pre-validation") == std::string::npos) {
            ec = fail("check11: refusal does not say why", 112);
            return 0;
        }
        bool anyManifest = false;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().filename().string().rfind("tsdev_resume_", 0) ==
                0) {
                anyManifest = true;
            }
        }
        if (anyManifest) {
            ec = fail("check11: partial manifest left behind", 113);
            return 0;
        }

        // Malformed-quote items ARE preValid=false (unterminated quote
        // and junk-after-quote paths in parseManifestLines) — the same
        // refusal covers them.
        std::vector<BatchItem> tail;
        BatchItem g2; g2.typeStr = "function"; g2.name = "Fine";
        tail.push_back(g2);
        std::vector<BatchItem> mq;
        parseManifestLines("function \"broken name\n",
                           /*needsSource=*/false, mq);
        if (mq.size() != 1 || mq[0].preValid) {
            ec = fail("check11: unterminated quote not preValid=false",
                      114);
            return 0;
        }
        tail.push_back(mq[0]);
        if (writeResumeManifest(dir.string(), tail, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check11: malformed-quote tail write accepted",
                      115);
            return 0;
        }
        std::vector<BatchItem> mq2;
        parseManifestLines("strategy \"a\"b\n",
                           /*needsSource=*/false, mq2);
        if (mq2.size() != 1 || mq2[0].preValid) {
            ec = fail("check11: junk-after-quote not preValid=false",
                      116);
            return 0;
        }
        tail[1] = mq2[0];
        if (writeResumeManifest(dir.string(), tail, 0,
                                /*needsSource=*/false, path, err)) {
            ec = fail("check11: junk-after-quote tail write accepted",
                      117);
            return 0;
        }
        // firstIndex skips already-attempted items: a preValid=false item
        // BELOW the boundary must not refuse — only the tail matters.
        std::vector<BatchItem> split;
        split.push_back(mq[0]);      // attempted (firstIndex excludes it)
        BatchItem g3; g3.typeStr = "strategy"; g3.name = "Tail";
        split.push_back(g3);
        if (!writeResumeManifest(dir.string(), split, 1,
                                 /*needsSource=*/false, path, err)) {
            ec = fail("check11: attempted preValid item refused", 118);
            return 0;
        }
        std::filesystem::remove(path, fsEc);
        std::filesystem::remove_all(dir, fsEc);
        std::fprintf(stdout, "    PASS: preValid-false tail refuse "
                             "(placeholder + malformed-quote)\n");
    }

    // ------------------------------------------------------------------
    // Check 12: pin kManifestWs <-> isManifestWs equivalence — sweep all
    // 256 char values through the production seam (isManifestWs is the
    // C-locale std::isspace predicate trim() and token splitting actually
    // call). A drift would silently retarget quoted/unquoted names. Also
    // pin kManifestLineBreaks as a strict subset (the never-representable
    // set — a line break ends the record).
    // ------------------------------------------------------------------
    {
        for (int c = 0; c < 256; ++c) {
            const char ch = static_cast<char>(c);
            const bool inSet =
                (ch != '\0') &&
                std::strchr(kManifestWs, ch) != nullptr;
            const bool isSpace = isManifestWs(ch);
            if (inSet != isSpace) {
                ec = fail("check12: kManifestWs diverges from "
                          "isManifestWs", 120);
                return 0;
            }
        }
        for (const char* p = kManifestLineBreaks; *p; ++p) {
            if (std::strchr(kManifestWs, *p) == nullptr) {
                ec = fail("check12: line-break char not in kManifestWs",
                          121);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: kManifestWs == isManifestWs "
                             "(256-char sweep)\n");
    }

    // ------------------------------------------------------------------
    // Check 13: parseDirectory pre-validation —
    //   (a) an EMPTY .el file is a preValid=false exit-3 item with the
    //       same "source file is empty" text parseManifestLines and
    //       cmdInsert use (previously it dispatched as preValid=true);
    //   (b) a filename the ANSI code page cannot represent becomes the
    //       catch-branch placeholder item (preValid=false, exit 3,
    //       "<unrepresentable filename: ...>") instead of throwing out
    //       of the scan — pinned via a real wide-named file when the
    //       system's narrow conversion provably fails on the probe
    //       char (skipped honestly otherwise).
    //   (c) a delete-op dir ignores content entirely: an empty file
    //       stays preValid=true (no read happens).
    // ------------------------------------------------------------------
    {
        char tmpBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmpBuf);
        const std::filesystem::path dir =
            std::filesystem::path(tmpBuf) /
            ("tsdev_r4g_" + std::to_string(GetCurrentProcessId()));
        std::error_code fsEc;
        std::filesystem::create_directories(dir, fsEc);
        writeScratch(dir, "ok.el", "variables: int q(0);\nq = 5;\n");
        writeScratch(dir, "empty.el", "");
        writeScratch(dir, "empty.txt", "");

        // (a)+(c): needsSource=true marks the empty files preValid=false
        // ("source file is empty", exit 3); needsSource=false ignores
        // content and they stay preValid=true.
        std::vector<BatchItem> items;
        if (!parseDirectory(static_cast<int>(ComponentType::Indicator),
                            "indicator", dir.string(), {}, /*needsSource=*/true,
                            items)) {
            ec = fail("check13: parseDirectory open failed", 130);
            return 0;
        }
        const BatchItem* okItem = nullptr;
        int emptyItems = 0;
        for (const BatchItem& it : items) {
            if (it.name == "ok") {
                okItem = &it;
            } else if (it.name == "empty") {
                ++emptyItems;
                if (it.preValid || it.result.exitCode != 3 ||
                    it.result.errorMessage != "source file is empty") {
                    ec = fail("check13: empty .el not a preValid=false "
                              "exit-3 'source file is empty' item", 131);
                    return 0;
                }
            }
        }
        if (okItem == nullptr || !okItem->preValid || emptyItems != 2) {
            ec = fail("check13: empty-source pre-validation wrong", 132);
            return 0;
        }
        std::vector<BatchItem> delItems;
        if (!parseDirectory(static_cast<int>(ComponentType::Strategy),
                            "strategy", dir.string(), {}, /*needsSource=*/false,
                            delItems)) {
            ec = fail("check13: parseDirectory(delete) open failed", 133);
            return 0;
        }
        for (const BatchItem& it : delItems) {
            if (!it.preValid) {
                ec = fail("check13: delete-op dir item not preValid",
                          134);
                return 0;
            }
        }

        // (b): the non-ANSI-filename catch. U+1F600 is not representable
        // in any ANSI code page; if the system's narrow conversion still
        // accepts it (e.g. a UTF-8 ACP), the catch is unprovable here and
        // the check marks itself SKIP honestly instead of failing.
        const std::wstring wideName = L"wide_\U0001F600.el";
        bool narrowWouldThrow = false;
        try {
            (void)std::filesystem::path(dir.wstring() + L"\\" + wideName)
                .string();
        } catch (const std::exception&) {
            narrowWouldThrow = true;
        }
        if (narrowWouldThrow) {
            const std::filesystem::path wfile = dir.wstring() +
                L"\\" + wideName;
            {
                std::ofstream wf(wfile, std::ios::binary | std::ios::trunc);
                wf << "variables: int r(0);\nr = 6;\n";
            }
            std::vector<BatchItem> items2;
            if (!parseDirectory(static_cast<int>(ComponentType::Indicator),
                                "indicator", dir.string(), {},
                                /*needsSource=*/true, items2)) {
                ec = fail("check13: parseDirectory wide scan failed", 135);
                return 0;
            }
            bool sawPlaceholder = false;
            for (const BatchItem& it : items2) {
                if (it.name.find("<unrepresentable filename:") == 0) {
                    sawPlaceholder = true;
                    if (it.preValid || it.result.exitCode != 3 ||
                        it.result.errorMessage.find("non-ANSI filename") ==
                            std::string::npos) {
                        ec = fail("check13: placeholder item shape wrong",
                                  136);
                        return 0;
                    }
                }
            }
            if (!sawPlaceholder) {
                ec = fail("check13: wide filename did not produce the "
                          "catch-branch placeholder item", 137);
                return 0;
            }
            std::filesystem::remove(wfile, fsEc);
            std::fprintf(stdout, "    PASS: empty-source pre-validation + "
                                 "non-ANSI filename catch\n");
        } else {
            std::fprintf(stdout, "    PASS: empty-source pre-validation "
                                 "(non-ANSI filename catch SKIPPED: narrow "
                                 "conversion accepts the probe char on "
                                 "this system)\n");
        }
        std::filesystem::remove_all(dir, fsEc);
    }

    // ------------------------------------------------------------------
    // Check 14 (R1+R2): the attached-site DEFER boundary — the engine may
    // run only never-attached (an attached process holds +7 m_nLock for
    // its whole lifetime; closeWarehouse does NOT release it — only
    // GRACEFUL process teardown does). R2: the deferred exit-18 boundary
    // releases the +7 by running the §5 close stage itself
    // (boundedCloseWarehouse on the calling/session-owning thread) then
    // taking the GRACEFUL teardown path — `delete lib` + worker return +
    // ExitProcess(18), never TerminateProcess (an abrupt exit orphans
    // the +7 for >=minutes and the resumed census deadlocks on
    // census_lock — live-proven 2026-10-21). A close that fails or
    // outlives the bound REFUSES the defer on the §5 close-stage family
    // (exit 19 + terminateSelf — the session may still be open).
    // recycleDeferredResult takes an injectable close seam + sampler
    // (zombie pre-check) + bound, so every arm is pinned here without
    // orlib20. Pin the shared site gate, the granted/refused shapes, and
    // the event-name/exit-code pairs the Python resume path matches on.
    // ------------------------------------------------------------------
    {
        // The shared site gate: attached => defer (no engine run);
        // never-attached => the engine may run in-process.
        if (!recycleSiteDefers(/*ownSessionExists=*/true) ||
            recycleSiteDefers(/*ownSessionExists=*/false)) {
            ec = fail("check14: recycleSiteDefers mapping wrong", 140);
            return 0;
        }

        // GRANTED: a scripted close that returns true inside the bound
        // -> Recycled-shaped exit-18 result (the resume boundary) that
        // is NOW safe-to-delete-lib (the §5 post-close teardown class —
        // the graceful exit releases the +7), never terminateSelf.
        {
            DeferSampler sampler;
            int closeCalls = 0;
            const tsdev::RecycleResult def = recycleDeferredResult(
                [&] { ++closeCalls; return true; }, &sampler);
            if (def.outcome != tsdev::RecycleOutcome::Recycled ||
                def.exitCode != 18 ||
                def.terminateSelf ||
                !def.safeToDeleteLib ||
                def.reason != "recycle_deferred_own_session" ||
                def.detail.empty() ||
                closeCalls != 1 ||
                sampler.zombiePolls != 1) {
                ec = fail("check14: granted defer result shape wrong", 141);
                return 0;
            }

            // Contract pin: the defer event keeps the whserver_recycled
            // name — _resume_manifest_path matches ONLY that name (and
            // whserver_recycle_refused) for the manifest member, and
            // exit_code 18 drives both the direct resume and the serve
            // respawn. reason says WHY (deferred, not performed).
            const std::string j = whserverRecycleEventLine(
                def, /*jsonOutput=*/true, /*whPid=*/4242, /*freeMB=*/10,
                /*boundaryIndex=*/2, /*total=*/5,
                /*manifestPath=*/"m.manifest");
            if (j.find("\"event\":\"whserver_recycled\"") ==
                    std::string::npos ||
                j.find("\"reason\":\"recycle_deferred_own_session\"") ==
                    std::string::npos ||
                j.find("\"exit_code\":18") == std::string::npos ||
                j.find("\"manifest\":\"m.manifest\"") ==
                    std::string::npos) {
                ec = fail("check14: deferred JSON event breaks the "
                          "resume contract", 142);
                return 0;
            }
            const std::string h = whserverRecycleEventLine(def, false);
            if (h.find("whserver.recycled") == std::string::npos ||
                h.find("reason=recycle_deferred_own_session") ==
                    std::string::npos ||
                h.find("exit=18") == std::string::npos) {
                ec = fail("check14: deferred human event wrong", 143);
                return 0;
            }
        }

        // REFUSED: the close ran and returned false -> the §5 close-stage
        // family: RefuseCloseFailed + exit 19 + terminateSelf (the own
        // session may still be open — never `delete lib`/ExitProcess
        // into it). The event renders whserver_recycle_refused — the
        // other name _resume_manifest_path matches.
        {
            DeferSampler sampler;
            const tsdev::RecycleResult ref = recycleDeferredResult(
                [] { return false; }, &sampler);
            if (ref.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
                ref.exitCode != 19 ||
                !ref.terminateSelf ||
                ref.safeToDeleteLib ||
                ref.reason != "close_failed") {
                ec = fail("check14: refused defer (close false) wrong",
                          144);
                return 0;
            }
            const std::string j = whserverRecycleEventLine(ref, true);
            if (j.find("\"event\":\"whserver_recycle_refused\"") ==
                    std::string::npos ||
                j.find("\"reason\":\"close_failed\"") ==
                    std::string::npos ||
                j.find("\"exit_code\":19") == std::string::npos) {
                ec = fail("check14: refused JSON event wrong", 145);
                return 0;
            }
        }

        // The P4-(b) zombie pre-check runs BEFORE the defer close: a
        // crash-frozen server would wedge the close into the bound —
        // refuse early (the close never ran, so the session may still
        // be open -> terminateSelf).
        {
            DeferSampler sampler;
            tsdev::RecycleZombiePoll z;
            z.snapshotOk = true;
            z.crashSnapshotOk = true;
            z.whPids = {6060};
            z.crashParentPids = {6060};
            sampler.zombies = {z};
            int closeCalls = 0;
            const tsdev::RecycleResult ref = recycleDeferredResult(
                [&] { ++closeCalls; return true; }, &sampler);
            if (ref.outcome != tsdev::RecycleOutcome::RefuseCloseFailed ||
                ref.exitCode != 19 ||
                !ref.terminateSelf ||
                ref.reason != "zombie_present_close" ||
                closeCalls != 0) {
                ec = fail("check14: zombie defer -> zombie_present_close",
                          146);
                return 0;
            }
        }
        // An inconclusive gate (exhausted stream) fails closed the same
        // way — never proceeds on unknown zombie state.
        {
            DeferSampler sampler;
            sampler.zombies.clear();
            const tsdev::RecycleResult ref = recycleDeferredResult(
                [] { return true; }, &sampler);
            if (ref.reason != "zombie_unverifiable_close" ||
                ref.exitCode != 19 || !ref.terminateSelf) {
                ec = fail("check14: inconclusive zombie gate -> "
                          "zombie_unverifiable_close", 147);
                return 0;
            }
        }

        // A hanging close fires the bound: production's default action
        // is stderr + TerminateProcess(19); the injected stub records
        // the fire and unblocks the scripted close — the late return is
        // never trusted -> close_timeout on the same teardown contract.
        {
            DeferSampler sampler;
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev == nullptr) {
                ec = fail("check14: CreateEvent for close_timeout", 148);
                return 0;
            }
            bool abortFired = false;
            const tsdev::RecycleResult ref = recycleDeferredResult(
                [&] { WaitForSingleObject(ev, INFINITE); return true; },
                &sampler, /*closeBoundSecs=*/0.2,
                [&] { abortFired = true; SetEvent(ev); });
            CloseHandle(ev);
            if (!abortFired ||
                ref.reason != "close_timeout" ||
                ref.exitCode != 19 || !ref.terminateSelf ||
                ref.safeToDeleteLib) {
                ec = fail("check14: bound fire -> close_timeout "
                          "terminateSelf(19)", 149);
                return 0;
            }
        }
        // closeBoundSecs<=0 refuses at the gate — the close never ran.
        {
            DeferSampler sampler;
            int closeCalls = 0;
            const tsdev::RecycleResult ref = recycleDeferredResult(
                [&] { ++closeCalls; return true; }, &sampler,
                /*closeBoundSecs=*/0.0);
            if (ref.reason != "close_bound_invalid" ||
                ref.exitCode != 19 || !ref.terminateSelf ||
                closeCalls != 0) {
                ec = fail("check14: boundSecs<=0 -> close_bound_invalid",
                          150);
                return 0;
            }
        }
        // A fast close never fires the bound action.
        {
            DeferSampler sampler;
            bool abortFired = false;
            const tsdev::RecycleResult def = recycleDeferredResult(
                [] { return true; }, &sampler, /*closeBoundSecs=*/5.0,
                [&] { abortFired = true; });
            if (abortFired || def.exitCode != 18) {
                ec = fail("check14: fast defer close fired the bound",
                          151);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: R1+R2 defer boundary -- granted "
                             "graceful-exit-18 vs refused section-5 close-stage "
                             "shapes + contract\n");
    }

    // -------------------------------------------------------------------
    // Check 15: the two-threshold continuation policy (field fix) --
    // VasGuardConfig floor defaults, the once-per-run notice renderer,
    // and the census_lock-above-floor refuse classification.
    // -------------------------------------------------------------------
    {
        // Defaults pin: trigger pair unchanged; floor pair is the new
        // hard-abort band.
        VasGuardConfig cfg;
        if (!cfg.enabled || cfg.freeThresholdMB != 256 ||
            cfg.largestThresholdMB != 64 ||
            cfg.abortFreeMB != 64 || cfg.abortLargestFreeMB != 32) {
            ec = fail("check15: VasGuardConfig defaults drifted", 160);
            return 0;
        }
        VasGuardWarns w;
        if (w.continued || w.measure || w.whEnum || w.crashEnum) {
            ec = fail("check15: VasGuardWarns latches not default-false",
                      161);
            return 0;
        }

        // The abort-floor flags/env reuse parseMbThreshold's strict
        // contract: "0".."1048575" decimal, nothing else. The caller drops
        // invalid env values (keeps the default); the parse itself just
        // answers valid-or-not.
        {
            unsigned v = 0;
            if (!parseMbThreshold("0", v) || v != 0 ||
                !parseMbThreshold("64", v) || v != 64 ||
                !parseMbThreshold("32", v) || v != 32 ||
                !parseMbThreshold("1048575", v) || v != 1048575 ||
                parseMbThreshold("", v) ||
                parseMbThreshold("-1", v) ||
                parseMbThreshold("12.5", v) ||
                parseMbThreshold("bogus", v) ||
                parseMbThreshold(" 64", v) ||
                parseMbThreshold("1048576", v)) {
                ec = fail("check15: parseMbThreshold contract drifted",
                          169);
                return 0;
            }
        }

        // Refuse classification: ONLY census_lock above the floor
        // continues; everything else stays fail-closed.
        if (!vasRefuseAllowsContinue("census_lock", false)) {
            ec = fail("check15: census_lock above floor not a continue",
                      162);
            return 0;
        }
        if (vasRefuseAllowsContinue("census_lock", true) ||
            vasRefuseAllowsContinue("attach_failed", false) ||
            vasRefuseAllowsContinue("zombie_present_close", false) ||
            vasRefuseAllowsContinue("census_unverifiable", false) ||
            vasRefuseAllowsContinue("whserver_recycled", false) ||
            vasRefuseAllowsContinue("", false) ||
            vasRefuseAllowsContinue(nullptr, false)) {
            ec = fail("check15: non-census_lock/below-floor refuse "
                      "allowed a continue", 163);
            return 0;
        }

        // Cause names are stable strings (the JSON `cause` field values).
        if (std::strcmp(vasContinueCauseName(VasContinueCause::NotAuthorized),
                        "recycle_not_authorized") != 0 ||
            std::strcmp(vasContinueCauseName(VasContinueCause::ForeignClients),
                        "foreign_clients") != 0 ||
            std::strcmp(vasContinueCauseName(VasContinueCause::CensusLock),
                        "census_lock") != 0) {
            ec = fail("check15: continue cause names drifted", 164);
            return 0;
        }

        // Human line: exact pinned text for each cause (the user's
        // approved wording). All output must be pure ASCII.
        const std::string base = whserverVasContinueLine(
            4242, 246, 135, 256, VasContinueCause::NotAuthorized, false);
        const std::string expectBase =
            "tsdev_cli: NOTE WHServer memory low (free=246MB largest=135MB); "
            "continuing -- the server can keep working (the 256MB restart "
            "threshold is not a danger level)";
        if (base != expectBase) {
            ec = fail("check15: human continue line (not_authorized) "
                      "wrong text", 165);
            return 0;
        }
        const std::string expectSuffix =
            " -- another TradeStation program is using the server so it "
            "cannot be restarted";
        if (whserverVasContinueLine(4242, 246, 135, 256,
                                    VasContinueCause::ForeignClients,
                                    false) != expectBase + expectSuffix ||
            whserverVasContinueLine(4242, 246, 135, 256,
                                    VasContinueCause::CensusLock,
                                    false) != expectBase + expectSuffix) {
            ec = fail("check15: human continue line (foreign/census) "
                      "suffix wrong", 166);
            return 0;
        }
        for (const std::string& s :
             {base, expectBase + expectSuffix}) {
            for (unsigned char c : s) {
                if (c > 126 || c < 32) {
                    ec = fail("check15: continue line not pure ASCII", 167);
                    return 0;
                }
            }
        }

        // JSON line: the whserver_vas_continue event -- a diagnostics-lane
        // object that deliberately matches NEITHER pinned recycle event
        // name (client resume semantics untouched).
        const std::string j = whserverVasContinueLine(
            4242, 246, 135, 256, VasContinueCause::CensusLock, true);
        if (j.find("\"event\":\"whserver_vas_continue\"") ==
                std::string::npos ||
            j.find("\"pid\":4242") == std::string::npos ||
            j.find("\"free_mb\":246") == std::string::npos ||
            j.find("\"largest_free_mb\":135") == std::string::npos ||
            j.find("\"cause\":\"census_lock\"") == std::string::npos ||
            j.find("\"action\":\"continue\"") == std::string::npos ||
            j.find("whserver_recycled") != std::string::npos ||
            j.find("whserver_recycle_refused") != std::string::npos) {
            ec = fail("check15: JSON continue event wrong fields", 168);
            return 0;
        }
        std::fprintf(stdout, "    PASS: two-threshold continuation policy "
                             "(config defaults, census_lock classification, "
                             "notice renderings)\n");
    }

    // -------------------------------------------------------------------
    // Check 16: the tri-state recycle-authorization config (design
    // addendum) -- RecycleConfig defaults to RescueOnly (the recycle is
    // reserved for the abort-floor rescue) with no env attribution, and
    // the mode feeds vasGuardAction exactly as pinned in
    // test_whserver_health group 7 (wired here through the config type
    // the batch/serve guards actually hold).
    // -------------------------------------------------------------------
    {
        // Defaults pin: RescueOnly, no env attribution.
        RecycleConfig cfg;
        if (cfg.mode != tsdev::RecycleMode::RescueOnly ||
            cfg.authorizedViaEnv) {
            ec = fail("check16: RecycleConfig defaults drifted", 170);
            return 0;
        }

        // The config's mode drives the guard decision: below the abort
        // floor the default config attempts the rescue; an Off config
        // hard-aborts; a Full config attempts in the band too (latch
        // permitting) while RescueOnly never does.
        auto act = [](const RecycleConfig& rc, uint64_t freeMB,
                      uint64_t largestMB, bool foreign) {
            return tsdev::vasGuardAction(
                freeMB, largestMB, /*triggerTotalMB=*/256,
                /*triggerContigMB=*/64, /*floorTotalMB=*/64,
                /*floorContigMB=*/32, foreign, rc.mode);
        };
        if (act(RecycleConfig(), 10, 200, false) !=
                tsdev::VasGuardAction::AttemptRecycle) {
            ec = fail("check16: default config did not rescue below floor",
                      171);
            return 0;
        }
        if (act(RecycleConfig(), 246, 135, false) !=
                tsdev::VasGuardAction::Continue) {
            ec = fail("check16: default config attempted a band recycle",
                      172);
            return 0;
        }
        RecycleConfig off; off.mode = tsdev::RecycleMode::Off;
        if (act(off, 10, 200, false) != tsdev::VasGuardAction::Abort) {
            ec = fail("check16: Off config did not hard-abort below floor",
                      173);
            return 0;
        }
        RecycleConfig full; full.mode = tsdev::RecycleMode::Full;
        if (act(full, 246, 135, false) !=
                tsdev::VasGuardAction::AttemptRecycle) {
            ec = fail("check16: Full config did not attempt in the band",
                      174);
            return 0;
        }
        std::fprintf(stdout, "    PASS: tri-state recycle config pins\n");
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
    std::fprintf(stdout, "=== test_recycle_resume (unit) ===\n");

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
                 "\n=== test_recycle_resume complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
