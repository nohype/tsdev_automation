// test_recycle_whserver_flag.cpp
//
// Pure unit test for the --recycle-whserver / --no-recycle-whserver flag
// surface in src/tsdev_cli.cpp (TVS R2 -- flag plumbing only; the recycle
// engine call sites are R3; tri-state authorization addendum):
//
//   - bool isBatchSubcommand(const std::string&)             -- shared
//     batch-* SSOT list (both flag validations)
//   - bool isRecycleWhserverSubcommand(const std::string&)   -- Section 6.1 set
//   - bool validateRecycleWhserver(RecycleMode, authorizedViaEnv,
//         vasGuardEnabled, batchControl, subcommand, CliResult&)
//                                                     -- Section 6.1 refusals
//   - bool envPresenceEnabled(const char*)                    -- env mirror
//     presence semantics (set-and-not-"0")
//   - bool resolveRecycleMode(flagSeen, noFlagSeen, RecycleMode&, err)
//     -- the tri-state precedence matrix (flags beat env, negative env
//     wins env-vs-env, default RescueOnly, both flags -> usage error)
//   - const char kRecycleWhserverWarning[] + emitRecycleWhserverWarning()
//     -- Section 6.3 warning text (Full mode), emitted once per process
//     lifetime
//   - kRecycleRescueDefaultNote / kRecycleControlSuppressedNote + their
//     emitters -- the one-line RescueOnly NOTEs (default / suppressed by
//     --batch-progress-control), each once per process lifetime
//
// This is a PURE UNIT TEST -- it does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and performs no privileged I/O (the only
// environment mutation is SetEnvironmentVariableA on the test's own
// TSDEV_*_WHSERVER probe vars, restored afterwards; the only fd
// redirection is a tmpfile() capture of stderr, restored afterwards).
// The body runs on a 64 MB-stack worker thread only to match the project's
// test-harness convention.
//
// Linkage mechanism: identical to test_pbar.cpp -- the helpers live inside
// tsdev_cli.cpp's anonymous namespace (internal linkage), so the test
// #defines TSDEV_CLI_TEST and #includes the .cpp directly (same TU; the
// file's main() is excluded by the TSDEV_CLI_TEST guard).
//
// Checks:
//   1. isRecycleWhserverSubcommand: all 8 batch-* subcommands + serve pass
//      (case-insensitive); every single-op subcommand, "", and unknown
//      tokens fail.
//   1b. isBatchSubcommand: exactly the 8 batch-* names.
//   2. validateRecycleWhserver: RescueOnly and Off NEVER refuse (any other
//      combination -- the conflicts are scoped to explicit Full
//      authorization); Full + batch-* passes; Full + serve passes; Full +
//      guard disabled -> exit-1 refusal; Full + --batch-progress-control
//      -> exit-1 refusal; Full + a single-op/empty subcommand -> exit-1
//      refusal. Refusal order: vasGuard wins over batchControl wins over
//      subcommand. When authorizedViaEnv is set the refusal names
//      TSDEV_RECYCLE_WHSERVER and states the remedy (unset env / drop
//      flag) instead of misattributing an explicit flag.
//   3. envPresenceEnabled: unset -> false, "" -> false, "0" -> false, "1"
//      -> true, arbitrary -> true.
//   3b. resolveRecycleMode: the tri-state precedence matrix (env reads
//      inside the helper) -- both flags -> false + error; an explicit
//      flag beats the opposite-polarity env; env-vs-env conflict -> the
//      negative arm wins; TSDEV_RECYCLE_WHSERVER presence -> Full;
//      TSDEV_NO_RECYCLE_WHSERVER presence -> Off; neither -> RescueOnly;
//      an env "0" only neutralizes its own arm. (main()'s post-scan calls
//      this helper; the real TSDEV_* vars are snapshotted and restored.)
//   4. emitRecycleWhserverWarning: emits kRecycleWhserverWarning verbatim
//      to stderr exactly once (second call emits nothing -- the once-per-
//      process-lifetime guard); the text is the short user-facing warning
//      (ASCII, <= 5 lines, names the flag/server/resume file/docs).
//   4b. The RescueOnly NOTEs: each emitter writes its one-line NOTE
//      verbatim exactly once (second call emits nothing), ASCII only,
//      and the two texts are distinct.
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
#include <io.h>
#include <fcntl.h>
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
                 "=== test_recycle_whserver_flag: --recycle-whserver flag "
                 "surface (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: isRecycleWhserverSubcommand — the §6.1 valid set is exactly
    // the 8 batch-* subcommands + serve, case-insensitive.
    // ------------------------------------------------------------------
    {
        const char* valid[] = {
            "batch-insert", "batch-modify", "batch-delete", "batch-upsert",
            "batch-insert-dir", "batch-modify-dir", "batch-delete-dir",
            "batch-upsert-dir", "serve",
            "BATCH-INSERT", "Serve",  // case-insensitive (iequals)
        };
        for (const char* s : valid) {
            if (!isRecycleWhserverSubcommand(s)) {
                ec = fail("check1: valid subcommand rejected", 10);
                return 0;
            }
        }
        const char* invalid[] = {
            "", "insert", "modify", "delete", "upsert", "exists",
            "get-source", "dependents", "batch", "batch-insertx",
            "--batch-stdin",
        };
        for (const char* s : invalid) {
            if (isRecycleWhserverSubcommand(s)) {
                ec = fail("check1: invalid subcommand accepted", 11);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: subcommand set\n");
    }

    // ------------------------------------------------------------------
    // Check 1b: isBatchSubcommand — the shared SSOT batch-* list both
    // flag validations use (--batch-progress-control and
    // --recycle-whserver). Exactly the 8 batch-* names; "serve" and every
    // single-op/unknown token fail.
    // ------------------------------------------------------------------
    {
        const char* valid[] = {
            "batch-insert", "batch-modify", "batch-delete", "batch-upsert",
            "batch-insert-dir", "batch-modify-dir", "batch-delete-dir",
            "batch-upsert-dir",
            "BATCH-MODIFY-DIR",  // case-insensitive (iequals)
        };
        for (const char* s : valid) {
            if (!isBatchSubcommand(s)) {
                ec = fail("check1b: batch-* subcommand rejected", 12);
                return 0;
            }
        }
        const char* invalid[] = {
            "", "serve", "insert", "modify", "delete", "upsert", "exists",
            "get-source", "dependents", "batch", "batch-insertx",
            "--batch-stdin",
        };
        for (const char* s : invalid) {
            if (isBatchSubcommand(s)) {
                ec = fail("check1b: non-batch subcommand accepted", 13);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: shared batch-* subcommand set\n");
    }

    // ------------------------------------------------------------------
    // Check 2: validateRecycleWhserver -- every Section 6.1 arm, scoped to
    // EXPLICIT Full authorization (the RescueOnly default and an explicit
    // Off opt-out are never refused).
    // ------------------------------------------------------------------
    {
        CliResult r;

        // RescueOnly (the default) and Off: always pass regardless of the
        // other inputs -- the conflicts cannot produce broken states in
        // these modes (the recycle is inert outside the floor rescue /
        // disabled entirely).
        if (!validateRecycleWhserver(tsdev::RecycleMode::RescueOnly, false,
                                     false, true, "", r) ||
            !validateRecycleWhserver(tsdev::RecycleMode::Off, false,
                                     false, true, "", r) ||
            !validateRecycleWhserver(tsdev::RecycleMode::RescueOnly, true,
                                     false, true, "insert", r) ||
            !validateRecycleWhserver(tsdev::RecycleMode::Off, false,
                                     true, true, "bogus", r)) {
            ec = fail("check2: non-Full mode was refused", 20);
            return 0;
        }

        // Full + valid subcommands pass.
        if (!validateRecycleWhserver(tsdev::RecycleMode::Full, false, true,
                                     false, "batch-insert", r) ||
            !validateRecycleWhserver(tsdev::RecycleMode::Full, false, true,
                                     false, "batch-upsert-dir", r) ||
            !validateRecycleWhserver(tsdev::RecycleMode::Full, false, true,
                                     false, "serve", r)) {
            ec = fail("check2: authorized + valid subcommand refused", 21);
            return 0;
        }

        // Guard disabled -> refusal naming --no-whserver-vas-guard.
        r = CliResult();
        if (validateRecycleWhserver(tsdev::RecycleMode::Full, false, false,
                                    false, "batch-insert", r)) {
            ec = fail("check2: vas-guard conflict not refused", 22);
            return 0;
        }
        if (r.exitCode != 1 ||
            r.errorMessage.find("--no-whserver-vas-guard") ==
                std::string::npos ||
            r.humanStderr.empty()) {
            ec = fail("check2: vas-guard refusal shape wrong", 23);
            return 0;
        }

        // --batch-progress-control -> refusal naming the control flag.
        r = CliResult();
        if (validateRecycleWhserver(tsdev::RecycleMode::Full, false, true,
                                    true, "batch-insert", r)) {
            ec = fail("check2: progress-control conflict not refused", 24);
            return 0;
        }
        if (r.exitCode != 1 ||
            r.errorMessage.find("--batch-progress-control") ==
                std::string::npos ||
            r.humanStderr.empty()) {
            ec = fail("check2: progress-control refusal shape wrong", 25);
            return 0;
        }

        // Single-op / empty / unknown subcommand -> refusal.
        for (const char* s : {"insert", "exists", "", "bogus"}) {
            r = CliResult();
            if (validateRecycleWhserver(tsdev::RecycleMode::Full, false,
                                        true, false, s, r)) {
                ec = fail("check2: single-op subcommand not refused", 26);
                return 0;
            }
            if (r.exitCode != 1 || r.humanStderr.empty()) {
                ec = fail("check2: subcommand refusal shape wrong", 27);
                return 0;
            }
        }

        // Refusal order: vasGuard conflict is reported before the
        // batchControl conflict, which precedes the subcommand check.
        r = CliResult();
        if (validateRecycleWhserver(tsdev::RecycleMode::Full, false, false,
                                    true, "insert", r) ||
            r.errorMessage.find("--no-whserver-vas-guard") ==
                std::string::npos) {
            ec = fail("check2: refusal order (guard first) wrong", 28);
            return 0;
        }
        r = CliResult();
        if (validateRecycleWhserver(tsdev::RecycleMode::Full, false, true,
                                    true, "insert", r) ||
            r.errorMessage.find("--batch-progress-control") ==
                std::string::npos) {
            ec = fail("check2: refusal order (control before subcommand) "
                      "wrong", 29);
            return 0;
        }

        // Env-sourced authorization (authorizedViaEnv=true): each refusal
        // arm must name TSDEV_RECYCLE_WHSERVER and state the remedy rather
        // than misattributing an explicit --recycle-whserver flag.
        for (int arm = 0; arm < 3; arm++) {
            r = CliResult();
            const bool refused = !validateRecycleWhserver(
                tsdev::RecycleMode::Full, /*authorizedViaEnv=*/true,
                /*vasGuardEnabled=*/arm != 0,
                /*batchControl=*/arm == 1,
                /*subcommand=*/arm == 2 ? "insert" : "batch-insert", r);
            if (!refused || r.exitCode != 1 ||
                r.errorMessage.find("TSDEV_RECYCLE_WHSERVER") ==
                    std::string::npos ||
                r.errorMessage.find("unset the env var") ==
                    std::string::npos ||
                r.humanStderr.find("TSDEV_RECYCLE_WHSERVER") ==
                    std::string::npos) {
                ec = fail("check2: env-sourced refusal misattributes "
                          "source", 35 + arm);
                return 0;
            }
        }
        // Flag-sourced refusals do NOT name the env var.
        r = CliResult();
        if (validateRecycleWhserver(tsdev::RecycleMode::Full, false, false,
                                    false, "batch-insert", r) ||
            r.errorMessage.find("TSDEV_RECYCLE_WHSERVER") !=
                std::string::npos) {
            ec = fail("check2: flag-sourced refusal names env var", 38);
            return 0;
        }
        std::fprintf(stdout, "    PASS: validateRecycleWhserver arms\n");
    }

    // ------------------------------------------------------------------
    // Check 3: envPresenceEnabled — set-and-not-"0" semantics on a
    // test-private variable (real TSDEV_RECYCLE_WHSERVER is left alone).
    // ------------------------------------------------------------------
    {
        const char* var = "TSDEV_RECYCLE_WHSERVER_TEST";
        // Snapshot any pre-existing value so it is restored.
        char saved[256] = {};
        DWORD savedLen = GetEnvironmentVariableA(var, saved, sizeof(saved));
        const bool hadSaved = savedLen > 0 && savedLen < sizeof(saved);

        SetEnvironmentVariableA(var, nullptr);  // unset
        if (envPresenceEnabled(var)) {
            ec = fail("check3: unset -> true", 30);
            return 0;
        }
        SetEnvironmentVariableA(var, "");
        if (envPresenceEnabled(var)) {
            ec = fail("check3: empty -> true", 31);
            return 0;
        }
        SetEnvironmentVariableA(var, "0");
        if (envPresenceEnabled(var)) {
            ec = fail("check3: \"0\" -> true", 32);
            return 0;
        }
        SetEnvironmentVariableA(var, "1");
        if (!envPresenceEnabled(var)) {
            ec = fail("check3: \"1\" -> false", 33);
            return 0;
        }
        SetEnvironmentVariableA(var, "yes");
        if (!envPresenceEnabled(var)) {
            ec = fail("check3: arbitrary value -> false", 34);
            return 0;
        }
        // Restore.
        if (hadSaved) SetEnvironmentVariableA(var, saved);
        else          SetEnvironmentVariableA(var, nullptr);
        std::fprintf(stdout, "    PASS: env presence semantics\n");
    }

    // ------------------------------------------------------------------
    // Check 3b: resolveRecycleMode -- the tri-state precedence matrix
    // (the env reads are inside the helper, so the whole rule set is
    // pinnable in this TU). Uses the REAL TSDEV_RECYCLE_WHSERVER and
    // TSDEV_NO_RECYCLE_WHSERVER vars, snapshotted and restored.
    // ------------------------------------------------------------------
    {
        const char* varOn = "TSDEV_RECYCLE_WHSERVER";
        const char* varOff = "TSDEV_NO_RECYCLE_WHSERVER";
        char savedOn[256] = {};
        char savedOff[256] = {};
        DWORD savedOnLen = GetEnvironmentVariableA(varOn, savedOn,
                                                   sizeof(savedOn));
        DWORD savedOffLen = GetEnvironmentVariableA(varOff, savedOff,
                                                    sizeof(savedOff));
        const bool hadOn = savedOnLen > 0 && savedOnLen < sizeof(savedOn);
        const bool hadOff = savedOffLen > 0 && savedOffLen < sizeof(savedOff);
        auto restore = [&]() {
            if (hadOn) SetEnvironmentVariableA(varOn, savedOn);
            else       SetEnvironmentVariableA(varOn, nullptr);
            if (hadOff) SetEnvironmentVariableA(varOff, savedOff);
            else        SetEnvironmentVariableA(varOff, nullptr);
        };
        using tsdev::RecycleMode;
        RecycleMode m = RecycleMode::Full;  // sentinel != any expected default
        std::string err;

        // (a) both polarity flags -> usage error (false + err), whatever
        // the env vars say. The message names both flags (finding
        // "--no-recycle-whserver" implies the full pinned sentence).
        SetEnvironmentVariableA(varOn, nullptr);
        SetEnvironmentVariableA(varOff, nullptr);
        if (resolveRecycleMode(true, true, m, err) ||
            err.find("--no-recycle-whserver") == std::string::npos ||
            err.find("cannot be combined") == std::string::npos) {
            restore();
            ec = fail("check3b: both flags not a usage error", 50);
            return 0;
        }

        // (b) explicit flag beats the opposite-polarity env.
        SetEnvironmentVariableA(varOff, "1");
        if (!resolveRecycleMode(true, false, m, err) ||
            m != RecycleMode::Full) {
            restore();
            ec = fail("check3b: flag did not beat negative env", 51);
            return 0;
        }
        SetEnvironmentVariableA(varOn, "1");
        SetEnvironmentVariableA(varOff, nullptr);
        if (!resolveRecycleMode(false, true, m, err) ||
            m != RecycleMode::Off) {
            restore();
            ec = fail("check3b: negative flag did not beat positive env", 52);
            return 0;
        }

        // (c) env-vs-env conflict -> the negative arm wins.
        SetEnvironmentVariableA(varOn, "1");
        SetEnvironmentVariableA(varOff, "1");
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::Off) {
            restore();
            ec = fail("check3b: env conflict did not resolve negative", 53);
            return 0;
        }

        // (d) single env arms; "0" neutralizes only its own arm.
        SetEnvironmentVariableA(varOn, "1");
        SetEnvironmentVariableA(varOff, nullptr);
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::Full) {
            restore();
            ec = fail("check3b: positive env alone not Full", 54);
            return 0;
        }
        SetEnvironmentVariableA(varOn, nullptr);
        SetEnvironmentVariableA(varOff, "1");
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::Off) {
            restore();
            ec = fail("check3b: negative env alone not Off", 55);
            return 0;
        }
        // "0" neutralizes its own arm only: positive "0" + negative set
        // -> Off (the negative arm still applies).
        SetEnvironmentVariableA(varOn, "0");
        SetEnvironmentVariableA(varOff, "1");
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::Off) {
            restore();
            ec = fail("check3b: positive \"0\" did not neutralize its arm",
                      56);
            return 0;
        }
        // TSDEV_RECYCLE_WHSERVER="0" alone does NOT opt out of the
        // RescueOnly default (it only neutralizes the env arm).
        SetEnvironmentVariableA(varOn, "0");
        SetEnvironmentVariableA(varOff, nullptr);
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::RescueOnly) {
            restore();
            ec = fail("check3b: positive env \"0\" opted out of the default",
                      57);
            return 0;
        }
        // Neither flag nor env -> the RescueOnly default.
        SetEnvironmentVariableA(varOn, nullptr);
        SetEnvironmentVariableA(varOff, nullptr);
        if (!resolveRecycleMode(false, false, m, err) ||
            m != RecycleMode::RescueOnly) {
            restore();
            ec = fail("check3b: default is not RescueOnly", 58);
            return 0;
        }
        restore();
        std::fprintf(stdout, "    PASS: tri-state precedence matrix\n");
    }

    // ------------------------------------------------------------------
    // Check 4: emitRecycleWhserverWarning — verbatim §6.3 bytes on stderr,
    // emitted exactly once (the second call is a no-op).
    // Redirect C stderr onto a binary tmpfile like test_pbar check 16.
    // ------------------------------------------------------------------
    {
        FILE* tmp = std::tmpfile();
        if (!tmp) {
            ec = fail("check4: tmpfile() failed", 40);
            return 0;
        }
        int savedFd = _dup(_fileno(stderr));
        if (savedFd < 0) {
            std::fclose(tmp);
            ec = fail("check4: _dup(stderr) failed", 41);
            return 0;
        }
        if (_dup2(_fileno(tmp), _fileno(stderr)) != 0) {
            _close(savedFd);
            std::fclose(tmp);
            ec = fail("check4: _dup2(tmp, stderr) failed", 42);
            return 0;
        }
        _setmode(_fileno(stderr), _O_BINARY);

        emitRecycleWhserverWarning();
        emitRecycleWhserverWarning();  // must not re-emit
        std::fflush(stderr);

        if (_dup2(savedFd, _fileno(stderr)) != 0) {
            _close(savedFd);
            std::fclose(tmp);
            ec = fail("check4: stderr restore failed", 43);
            return 0;
        }
        _close(savedFd);
        _setmode(_fileno(stderr), _O_TEXT);

        std::string got;
        std::fseek(tmp, 0, SEEK_SET);
        char buf[512];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            got.append(buf, n);
        std::fclose(tmp);

        if (got != kRecycleWhserverWarning) {
            ec = fail("check4: warning bytes wrong or emitted twice", 44);
            return 0;
        }
        // The warning is a short user-facing text (engineering detail
        // lives in docs/BUNDLE_README.md): it must name the flag, state
        // what it authorizes, note the batch can still stop safely when
        // another TradeStation program uses the server, and point at the
        // resume file + docs.
        const std::string w = kRecycleWhserverWarning;
        if (w.find("--recycle-whserver") == std::string::npos ||
            w.find("WHServer.exe") == std::string::npos ||
            w.find("memory runs low") == std::string::npos ||
            w.find("another TradeStation program") == std::string::npos ||
            w.find("resume file") == std::string::npos ||
            w.find("docs/BUNDLE_README.md") == std::string::npos) {
            ec = fail("check4: warning misses pinned content", 45);
            return 0;
        }
        // Strictly ASCII and short: every byte < 0x80 and no more than
        // five emitted lines.
        const bool asciiOnly = std::all_of(
            w.begin(), w.end(),
            [](char c) { return static_cast<unsigned char>(c) <= 0x7F; });
        size_t newlines = 0;
        for (char c : w) if (c == '\n') ++newlines;
        if (!asciiOnly || newlines > 5) {
            ec = fail("check4: warning not short ASCII text", 46);
            return 0;
        }
        std::fprintf(stdout, "    PASS: verbatim warning emitted once\n");
    }

    // ------------------------------------------------------------------
    // Check 4b: the RescueOnly NOTEs -- each emitter writes its one-line
    // NOTE verbatim to stderr exactly once (the second call is a no-op),
    // ASCII only, and the two texts are distinct. Same dup2'd-tmpfile
    // capture as check 4.
    // ------------------------------------------------------------------
    {
        auto capture = [&](void (*emit)(), const char* text,
                           const char* what, int code) -> int {
            FILE* tmp = std::tmpfile();
            if (!tmp) return fail(what, code);
            int savedFd = _dup(_fileno(stderr));
            if (savedFd < 0) {
                std::fclose(tmp);
                return fail(what, code + 1);
            }
            if (_dup2(_fileno(tmp), _fileno(stderr)) != 0) {
                _close(savedFd);
                std::fclose(tmp);
                return fail(what, code + 2);
            }
            _setmode(_fileno(stderr), _O_BINARY);
            emit();
            emit();  // must not re-emit
            std::fflush(stderr);
            if (_dup2(savedFd, _fileno(stderr)) != 0) {
                _close(savedFd);
                std::fclose(tmp);
                return fail(what, code + 3);
            }
            _close(savedFd);
            _setmode(_fileno(stderr), _O_TEXT);
            std::string got;
            std::fseek(tmp, 0, SEEK_SET);
            char buf[512];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
                got.append(buf, n);
            std::fclose(tmp);
            if (got != text) return fail(what, code + 4);
            const bool asciiOnly = std::all_of(
                got.begin(), got.end(),
                [](char c) {
                    return static_cast<unsigned char>(c) <= 0x7F;
                });
            if (!asciiOnly || got.find('\n', got.find('\n') + 1)
                    != std::string::npos) {
                return fail(what, code + 5);  // not one ASCII line
            }
            return 0;
        };
        int bad = capture(emitRecycleRescueDefaultNote,
                          kRecycleRescueDefaultNote,
                          "check4b: default NOTE bytes wrong", 60);
        if (bad) { ec = bad; return 0; }
        bad = capture(emitRecycleControlSuppressedNote,
                      kRecycleControlSuppressedNote,
                      "check4b: suppression NOTE bytes wrong", 70);
        if (bad) { ec = bad; return 0; }
        // The two NOTEs are distinct texts (a reader must be able to tell
        // the suppressed run from the default one).
        if (std::string(kRecycleRescueDefaultNote) ==
                std::string(kRecycleControlSuppressedNote)) {
            ec = fail("check4b: NOTE texts collide", 80);
            return 0;
        }
        // Content pins: the default NOTE names the default state and both
        // opt-out levers; the suppression NOTE names the suppressing flag.
        const std::string d = kRecycleRescueDefaultNote;
        if (d.find("on by default") == std::string::npos ||
            d.find("--no-recycle-whserver") == std::string::npos ||
            d.find("TSDEV_NO_RECYCLE_WHSERVER=1") == std::string::npos) {
            ec = fail("check4b: default NOTE misses pinned content", 81);
            return 0;
        }
        const std::string s = kRecycleControlSuppressedNote;
        if (s.find("--batch-progress-control") == std::string::npos ||
            s.find("suppressed") == std::string::npos) {
            ec = fail("check4b: suppression NOTE misses pinned content", 82);
            return 0;
        }
        std::fprintf(stdout, "    PASS: RescueOnly NOTEs emitted once\n");
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
                 "=== test_recycle_whserver_flag (unit) ===\n");

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
                 "\n=== test_recycle_whserver_flag complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
