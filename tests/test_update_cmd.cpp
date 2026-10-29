// test_update_cmd.cpp
//
// Unit test for the self-update CLI decision surface in src/tsdev_cli.cpp
// (design tmp/UPDATE_DESIGN.md):
//   - parseUpdateArgs(): the update-local flag grammar -- 0-or-1 positional
//     (check | latest | <semver>), --yes/-y and --force in any position,
//     --update-url non-repeatable, >1 positional/unknown token -> !ok.
//   - decideUpdateAction(): every spec decision path -- check verdicts,
//     empty feed, dev-build/non-bundle refusals, not-found/no-asset, the
//     capability-loss confirm trigger, kMin boundary, decline/force paths.
//   - kMinSelfUpdateVersion constant.
//
// This is a PURE UNIT TEST -- no orlib20, no network, no filesystem
// mutation. decideUpdateAction/parseUpdateArgs are pure; the feed and
// filesystem are modeled by the UpdateDecisionInput fields.
//
// Linkage: the helpers live inside an anonymous namespace in
// src/tsdev_cli.cpp (internal linkage), so the test #includes the .cpp
// directly (same TU) and #defines TSDEV_CLI_TEST to exclude its main() --
// the same mechanism used by test_pbar/test_version_stamp.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#define TSDEV_CLI_TEST
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

using tsdev::update::Semver;
using tsdev::update::parseSemver;

// Convenience: a fully-specified "typical" input -- current v0.1.5,
// non-dev, latest==target v0.1.6 with asset, bundle layout, interactive,
// no flags. Callers tweak fields per case.
UpdateDecisionInput baseInput() {
    UpdateDecisionInput in;
    in.current = parseSemver("0.1.5");
    in.devBuild = false;
    in.checkOnly = false;
    in.explicitTarget = false;
    in.feedEmpty = false;
    in.targetFound = true;
    in.target = parseSemver("0.1.6");
    in.targetHasAsset = true;
    in.bundleLayout = true;
    in.yes = false;
    in.force = false;
    in.interactive = true;
    in.minSelfUpdate = parseSemver(kMinSelfUpdateVersion);
    return in;
}

int expectAction(const char* label, const UpdateDecisionInput& in,
                 UpdateAction want, int code) {
    const UpdateAction got = decideUpdateAction(in);
    if (got != want) {
        std::fprintf(stderr, "\n[FAIL] %s: action=%d expected %d\n",
                     label, (int)got, (int)want);
        return code;
    }
    std::fprintf(stdout, "    %-58s -> %d PASS\n", label, (int)got);
    return 0;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    // ------------------------------------------------------------------
    // Check 1: parseUpdateArgs grammar.
    // ------------------------------------------------------------------
    {
        auto ok = [](std::initializer_list<const char*> v) {
            return parseUpdateArgs(
                std::vector<std::string>(v.begin(), v.end()));
        };
        UpdateArgs ua = ok({});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Latest) {
            ec = fail("check1: bare update must be Kind::Latest", 10);
            return 0;
        }
        ua = ok({"check"});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Check) {
            ec = fail("check1: update check", 11); return 0;
        }
        ua = ok({"CHECK"});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Check) {
            ec = fail("check1: check is case-insensitive", 12); return 0;
        }
        ua = ok({"latest"});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Latest) {
            ec = fail("check1: update latest", 13); return 0;
        }
        ua = ok({"v0.1.5"});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Version ||
            ua.version != "v0.1.5") {
            ec = fail("check1: update v0.1.5", 14); return 0;
        }
        ua = ok({"0.1.5"});
        if (!ua.ok || ua.kind != UpdateArgs::Kind::Version) {
            ec = fail("check1: update 0.1.5", 15); return 0;
        }
        // Flags legal in ANY position.
        ua = ok({"--yes", "v0.1.5"});
        if (!ua.ok || !ua.yes || ua.kind != UpdateArgs::Kind::Version) {
            ec = fail("check1: --yes before positional", 16); return 0;
        }
        ua = ok({"v0.1.5", "-y", "--force"});
        if (!ua.ok || !ua.yes || !ua.force) {
            ec = fail("check1: flags after positional", 17); return 0;
        }
        ua = ok({"--force", "check"});
        if (!ua.ok || !ua.force || ua.kind != UpdateArgs::Kind::Check) {
            ec = fail("check1: --force check accepted", 18); return 0;
        }
        // --update-url space + equals forms; non-repeatable.
        ua = ok({"check", "--update-url", "http://x"});
        if (!ua.ok || ua.updateUrl != "http://x") {
            ec = fail("check1: --update-url space form", 19); return 0;
        }
        ua = ok({"--update-url=http://y"});
        if (!ua.ok || ua.updateUrl != "http://y") {
            ec = fail("check1: --update-url equals form", 20); return 0;
        }
        ua = ok({"--update-url=a", "--update-url=b"});
        if (ua.ok) {
            ec = fail("check1: --update-url must be non-repeatable", 21);
            return 0;
        }
        // Rejections.
        if (parseUpdateArgs({"foo"}).ok) {
            ec = fail("check1: update foo", 22); return 0;
        }
        if (parseUpdateArgs({"check", "extra"}).ok) {
            ec = fail("check1: update check extra", 23); return 0;
        }
        if (parseUpdateArgs({"check", "v0.1.5"}).ok) {
            ec = fail("check1: update check v0.1.5", 24); return 0;
        }
        if (parseUpdateArgs({"0.1.5.1"}).ok) {
            ec = fail("check1: four-component version", 25); return 0;
        }
        if (parseUpdateArgs({"0.2.0_remote-control"}).ok) {
            ec = fail("check1: non-semver tag", 26); return 0;
        }
        if (parseUpdateArgs({"--help"}).ok) {
            ec = fail("check1: --help is an unknown token", 27); return 0;
        }
        if (parseUpdateArgs({"--update-url"}).ok) {
            ec = fail("check1: --update-url needs a value", 28); return 0;
        }
        std::fprintf(stdout, "    parseUpdateArgs grammar PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 2: kMinSelfUpdateVersion constant.
    // ------------------------------------------------------------------
    {
        const Semver kMin = parseSemver(kMinSelfUpdateVersion);
        if (!kMin.valid || kMin.major != 0 || kMin.minor != 1 ||
            kMin.patch != 6) {
            ec = fail("check2: kMinSelfUpdateVersion must be 0.1.6", 30);
            return 0;
        }
        std::fprintf(stdout, "    kMinSelfUpdateVersion=%s PASS\n",
                     kMinSelfUpdateVersion);
    }

    // ------------------------------------------------------------------
    // Check 3: check-form verdicts.
    // ------------------------------------------------------------------
    {
        UpdateDecisionInput in = baseInput();
        in.checkOnly = true;
        if ((ec = expectAction("check: update available", in,
                               UpdateAction::CheckAvailable, 40)) != 0)
            return 0;
        in.target = parseSemver("0.1.5");  // latest == current
        if ((ec = expectAction("check: up to date", in,
                               UpdateAction::VerdictUpToDate, 41)) != 0)
            return 0;
        in.target = parseSemver("0.1.4");  // current ahead
        if ((ec = expectAction("check: ahead", in,
                               UpdateAction::VerdictAhead, 42)) != 0)
            return 0;
        in.target = parseSemver("0.1.6");
        in.feedEmpty = true; in.targetFound = false;
        if ((ec = expectAction("check: empty feed", in,
                               UpdateAction::VerdictEmpty, 43)) != 0)
            return 0;
        in = baseInput(); in.checkOnly = true; in.devBuild = true;
        if ((ec = expectAction("check: dev build verdict", in,
                               UpdateAction::VerdictDev, 44)) != 0)
            return 0;
    }

    // ------------------------------------------------------------------
    // Check 4: bare-update verdicts + failures.
    // ------------------------------------------------------------------
    {
        UpdateDecisionInput in = baseInput();
        if ((ec = expectAction("update: apply to newer latest", in,
                               UpdateAction::Apply, 50)) != 0) return 0;
        in.target = parseSemver("0.1.5");
        if ((ec = expectAction("update: already latest", in,
                               UpdateAction::VerdictUpToDate, 51)) != 0)
            return 0;
        in.target = parseSemver("0.1.4");
        if ((ec = expectAction("update: ahead of latest", in,
                               UpdateAction::VerdictAhead, 52)) != 0)
            return 0;
        in = baseInput(); in.feedEmpty = true; in.targetFound = false;
        if ((ec = expectAction("update: empty feed verdict", in,
                               UpdateAction::VerdictEmpty, 53)) != 0)
            return 0;
        in.explicitTarget = true;
        if ((ec = expectAction("update <v>: not found on empty feed", in,
                               UpdateAction::FailNotFound, 54)) != 0)
            return 0;
        in = baseInput(); in.targetFound = false; in.explicitTarget = true;
        if ((ec = expectAction("update <v>: absent release", in,
                               UpdateAction::FailNotFound, 55)) != 0)
            return 0;
        in = baseInput(); in.targetHasAsset = false;
        if ((ec = expectAction("update: release without bundle asset", in,
                               UpdateAction::FailNoAsset, 56)) != 0)
            return 0;
    }

    // ------------------------------------------------------------------
    // Check 5: dev-build / non-bundle refusals and --force overrides.
    // ------------------------------------------------------------------
    {
        UpdateDecisionInput in = baseInput();
        in.devBuild = true;
        if ((ec = expectAction("dev build apply refused", in,
                               UpdateAction::RefuseDev, 60)) != 0) return 0;
        in.force = true;
        if ((ec = expectAction("dev build + --force applies", in,
                               UpdateAction::Apply, 61)) != 0) return 0;
        in = baseInput(); in.bundleLayout = false;
        if ((ec = expectAction("non-bundle dir refused", in,
                               UpdateAction::RefuseNonBundle, 62)) != 0)
            return 0;
        in.force = true;
        if ((ec = expectAction("non-bundle + --force applies", in,
                               UpdateAction::Apply, 63)) != 0) return 0;
        // dev + non-bundle both refused; dev gate fires first.
        in = baseInput(); in.devBuild = true; in.bundleLayout = false;
        if ((ec = expectAction("dev+nonbundle: dev gate first", in,
                               UpdateAction::RefuseDev, 64)) != 0) return 0;
    }

    // ------------------------------------------------------------------
    // Check 6: same-version / downgrade / capability-loss confirm.
    // ------------------------------------------------------------------
    {
        UpdateDecisionInput in = baseInput();
        in.explicitTarget = true;
        in.target = parseSemver("0.1.5");  // == current
        if ((ec = expectAction("update <same v>: AlreadyCurrent", in,
                               UpdateAction::AlreadyCurrent, 70)) != 0)
            return 0;
        in.force = true;
        // target==current==0.1.5 < kMin (0.1.6) -> capability gate fires.
        if ((ec = expectAction("update <same v> --force (below kMin)", in,
                               UpdateAction::NeedsConfirm, 71)) != 0)
            return 0;
        in.yes = true;
        if ((ec = expectAction("... with --yes applies", in,
                               UpdateAction::Apply, 72)) != 0) return 0;
        // Downgrade to a still-capable version: no prompt, straight Apply.
        in = baseInput(); in.explicitTarget = true;
        in.current = parseSemver("0.1.10");
        in.target = parseSemver("0.1.9");  // >= kMin, < current
        if ((ec = expectAction("downgrade to capable version applies", in,
                               UpdateAction::Apply, 73)) != 0) return 0;
        // kMin boundary: target == kMinSelfUpdateVersion is NOT a
        // capability-loss confirm.
        in.target = parseSemver(kMinSelfUpdateVersion);
        if ((ec = expectAction("target==kMin is not a confirm", in,
                               UpdateAction::Apply, 74)) != 0) return 0;
        // target < kMin -> confirm paths.
        in.target = parseSemver("0.1.5");
        if ((ec = expectAction("target<kMin interactive prompts", in,
                               UpdateAction::NeedsConfirm, 75)) != 0)
            return 0;
        in.interactive = false;
        if ((ec = expectAction("target<kMin non-interactive refuses", in,
                               UpdateAction::RefuseConfirm, 76)) != 0)
            return 0;
        in.yes = true;
        if ((ec = expectAction("target<kMin + --yes applies", in,
                               UpdateAction::Apply, 77)) != 0) return 0;
        // --force does NOT pre-answer the confirmation (4.3).
        in.yes = false; in.force = true;
        if ((ec = expectAction("--force does not pre-answer confirm", in,
                               UpdateAction::RefuseConfirm, 78)) != 0)
            return 0;
    }

    // ------------------------------------------------------------------
    // Check 7: explicit-target compare directions + upgrade < kMin.
    // ------------------------------------------------------------------
    {
        UpdateDecisionInput in = baseInput();
        in.explicitTarget = true;
        in.current = parseSemver("0.1.0");
        in.target = parseSemver("0.1.5");  // upgrade but still < kMin
        if ((ec = expectAction("upgrade to <kMin still prompts", in,
                               UpdateAction::NeedsConfirm, 80)) != 0)
            return 0;
        in.target = parseSemver("0.1.7");
        if ((ec = expectAction("explicit upgrade applies", in,
                               UpdateAction::Apply, 81)) != 0) return 0;
        // Bare update with target<current -> Ahead even when target<kMin.
        in = baseInput();
        in.current = parseSemver("0.2.0");
        in.target = parseSemver("0.1.5");
        if ((ec = expectAction("bare update ahead (target<kMin moot)", in,
                               UpdateAction::VerdictAhead, 82)) != 0)
            return 0;
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper -- POD-only so __try is legal.
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
                 "=== test_update_cmd: update decision surface (unit) ===\n");

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
                 "\n=== test_update_cmd complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
