// test_recycle_whserver_flag.cpp
//
// Pure unit test for the --recycle-whserver flag surface in
// src/tsdev_cli.cpp (TVS R2 — flag plumbing only; the recycle engine call
// sites are R3):
//
//   - bool isBatchSubcommand(const std::string&)             — shared
//     batch-* SSOT list (both flag validations)
//   - bool isRecycleWhserverSubcommand(const std::string&)   — §6.1 set
//   - bool validateRecycleWhserver(authorized, authorizedViaEnv,
//         vasGuardEnabled, batchControl, subcommand, CliResult&)
//                                                           — §6.1 refusals
//   - bool envPresenceEnabled(const char*)                    — env mirror
//     presence semantics (set-and-not-"0")
//   - const char kRecycleWhserverWarning[] + emitRecycleWhserverWarning()
//     -- §6.3 warning text, emitted once per process lifetime
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and performs no privileged I/O (the only
// environment mutation is SetEnvironmentVariableA on the test's own
// TSDEV_RECYCLE_WHSERVER probe var, restored afterwards; the only fd
// redirection is a tmpfile() capture of stderr, restored afterwards).
// The body runs on a 64 MB-stack worker thread only to match the project's
// test-harness convention.
//
// Linkage mechanism: identical to test_pbar.cpp — the helpers live inside
// tsdev_cli.cpp's anonymous namespace (internal linkage), so the test
// #defines TSDEV_CLI_TEST and #includes the .cpp directly (same TU; the
// file's main() is excluded by the TSDEV_CLI_TEST guard).
//
// Checks:
//   1. isRecycleWhserverSubcommand: all 8 batch-* subcommands + serve pass
//      (case-insensitive); every single-op subcommand, "", and unknown
//      tokens fail.
//   2. validateRecycleWhserver: not-authorized always passes (any other
//      combination); authorized + batch-* passes; authorized + serve
//      passes; authorized + guard disabled -> exit-1 refusal;
//      authorized + --batch-progress-control -> exit-1 refusal; authorized
//      + a single-op/empty subcommand -> exit-1 refusal. Refusal order:
//      vasGuard wins over batchControl wins over subcommand. When
//      authorizedViaEnv is set the refusal names TSDEV_RECYCLE_WHSERVER
//      and states the remedy (unset env / drop flag) instead of
//      misattributing an explicit flag.
//   3. envPresenceEnabled: unset -> false, "" -> false, "0" -> false, "1"
//      -> true, arbitrary -> true.
//   3b. resolveRecycleAuthorized: the OR-ed flag/env composition itself
//      (env read inside the helper) — flag alone -> true, env alone ->
//      true, neither -> false, env "0" never vetoes an explicit flag.
//      (main()'s pre-scan calls this helper; the env var is the real
//      TSDEV_RECYCLE_WHSERVER, snapshotted and restored.)
//   4. emitRecycleWhserverWarning: emits kRecycleWhserverWarning verbatim
//      to stderr exactly once (second call emits nothing — the once-per-
//      process-lifetime guard); the text is the short user-facing warning
//      (ASCII, <= 5 lines, names the flag/server/resume file/docs).
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
    // Check 2: validateRecycleWhserver — every §6.1 arm.
    // ------------------------------------------------------------------
    {
        CliResult r;

        // Not authorized: always passes regardless of the other inputs.
        if (!validateRecycleWhserver(false, false, false, true, "", r)) {
            ec = fail("check2: not-authorized should pass", 20);
            return 0;
        }

        // Authorized + valid subcommands pass.
        if (!validateRecycleWhserver(true, false, true, false, "batch-insert", r) ||
            !validateRecycleWhserver(true, false, true, false, "batch-upsert-dir", r) ||
            !validateRecycleWhserver(true, false, true, false, "serve", r)) {
            ec = fail("check2: authorized + valid subcommand refused", 21);
            return 0;
        }

        // Guard disabled -> refusal naming --no-whserver-vas-guard.
        r = CliResult();
        if (validateRecycleWhserver(true, false, false, false, "batch-insert", r)) {
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
        if (validateRecycleWhserver(true, false, true, true, "batch-insert", r)) {
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
            if (validateRecycleWhserver(true, false, true, false, s, r)) {
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
        if (validateRecycleWhserver(true, false, false, true, "insert", r) ||
            r.errorMessage.find("--no-whserver-vas-guard") ==
                std::string::npos) {
            ec = fail("check2: refusal order (guard first) wrong", 28);
            return 0;
        }
        r = CliResult();
        if (validateRecycleWhserver(true, false, true, true, "insert", r) ||
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
                true, /*authorizedViaEnv=*/true,
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
        if (validateRecycleWhserver(true, false, false, false,
                                    "batch-insert", r) ||
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
    // Check 3b: resolveRecycleAuthorized — the OR-ed flag/env composition
    // itself (the env read is inside the helper, so the precedence rule is
    // pinnable in this TU). Uses the REAL TSDEV_RECYCLE_WHSERVER var,
    // snapshotted and restored.
    // ------------------------------------------------------------------
    {
        const char* var = "TSDEV_RECYCLE_WHSERVER";
        char saved[256] = {};
        DWORD savedLen = GetEnvironmentVariableA(var, saved, sizeof(saved));
        const bool hadSaved = savedLen > 0 && savedLen < sizeof(saved);

        SetEnvironmentVariableA(var, nullptr);          // env unset
        if (!resolveRecycleAuthorized(true)) {          // flag alone
            ec = fail("check3b: flag alone -> false", 50);
            return 0;
        }
        if (resolveRecycleAuthorized(false)) {          // neither
            ec = fail("check3b: neither flag nor env -> true", 51);
            return 0;
        }
        SetEnvironmentVariableA(var, "1");              // env alone
        if (!resolveRecycleAuthorized(false)) {
            ec = fail("check3b: env alone -> false", 52);
            return 0;
        }
        SetEnvironmentVariableA(var, "0");              // env "0" + flag
        if (!resolveRecycleAuthorized(true)) {          // env "0" no veto
            ec = fail("check3b: env \"0\" vetoed the flag", 53);
            return 0;
        }
        if (resolveRecycleAuthorized(false)) {          // env "0" alone
            ec = fail("check3b: env \"0\" alone -> true", 54);
            return 0;
        }
        // Restore.
        if (hadSaved) SetEnvironmentVariableA(var, saved);
        else          SetEnvironmentVariableA(var, nullptr);
        std::fprintf(stdout, "    PASS: flag/env OR composition\n");
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
