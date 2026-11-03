// test_update_cmd.cpp
//
// Unit test for the self-update CLI decision surface in src/tsdev_cli.cpp
// (design docs/UPDATE_DESIGN.md):
//   - parseUpdateArgs(): the update-local flag grammar -- 0-or-1 positional
//     (check | latest | <semver>), --yes/-y and --force in any position,
//     --update-url non-repeatable, >1 positional/unknown token -> !ok.
//   - decideUpdateAction(): every spec decision path -- check verdicts,
//     empty feed, dev-build/non-bundle refusals, not-found/no-asset, the
//     capability-loss confirm trigger, kMin boundary, decline/force paths.
//   - capableDowngradeNotice(): the 4.2 notice scope (kMin <= target <
//     current only) and the errTooMany grammar flag.
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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Walk up from startDir looking for docs/UPDATE_DESIGN.md in the SOURCE
// tree (the committed file, never the build dir). Returns "" if not found.
std::string walkUpForUpdateDesign(const std::string& startDir) {
    std::string cur = startDir;
    for (int up = 0; up < 9; ++up) {
        const std::string cand = cur + "\\docs\\UPDATE_DESIGN.md";
        if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return cand;
        }
        const size_t slash = cur.find_last_of("\\/");
        if (slash == std::string::npos || slash <= 2) break;
        cur = cur.substr(0, slash);
    }
    return std::string();
}

// Resolve docs/UPDATE_DESIGN.md from the source tree, anchoring at the
// test executable's directory first, then the CWD. Deterministic pure
// file read -- RequiresTs=false compatible (no TS, no network).
std::string resolveUpdateDesignPath() {
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
        std::string dir(exe);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string found =
                walkUpForUpdateDesign(dir.substr(0, slash));
            if (!found.empty()) return found;
        }
    }
    char cwd[MAX_PATH] = {0};
    if (GetCurrentDirectoryA(MAX_PATH, cwd) > 0) {
        const std::string found = walkUpForUpdateDesign(cwd);
        if (!found.empty()) return found;
    }
    return std::string();
}

bool readWholeFileA(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
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

    // ------------------------------------------------------------------
    // Check 8: capable-downgrade notice scope (4.2) + too-many-args flag.
    // ------------------------------------------------------------------
    {
        auto expectNotice = [](const char* label,
                               const UpdateDecisionInput& in,
                               bool want, int code) {
            const bool got = capableDowngradeNotice(in);
            if (got != want) {
                std::fprintf(stderr,
                             "\n[FAIL] %s: notice=%d expected %d\n",
                             label, (int)got, (int)want);
                return code;
            }
            std::fprintf(stdout, "    %-58s -> %d PASS\n", label,
                         (int)got);
            return 0;
        };
        // (a) Downgrade to a still-capable target (kMin <= target <
        // current) flags the notice.
        UpdateDecisionInput in = baseInput();
        in.explicitTarget = true;
        in.current = parseSemver("0.1.10");
        in.target = parseSemver("0.1.9");
        if ((ec = expectNotice("downgrade to capable target -> notice", in,
                               true, 90)) != 0) return 0;
        // Boundary: target == kMin still counts as capable.
        in.target = parseSemver(kMinSelfUpdateVersion);
        if ((ec = expectNotice("downgrade to kMin boundary -> notice", in,
                               true, 91)) != 0) return 0;
        // (b) Downgrade below kMin -> NO capable-downgrade notice; the
        // capability-loss confirm path owns that case.
        in = baseInput();
        in.explicitTarget = true;
        in.yes = true;              // pre-answer the confirm gate
        in.target = parseSemver("0.1.4");  // < kMin, < current 0.1.5
        if ((ec = expectAction("downgrade below kMin + --yes applies", in,
                               UpdateAction::Apply, 92)) != 0) return 0;
        if ((ec = expectNotice("downgrade below kMin -> NO notice", in,
                               false, 93)) != 0) return 0;
        // Non-downgrade / dev-build cases never flag the notice.
        in = baseInput();           // target 0.1.6 > current 0.1.5
        if ((ec = expectNotice("upgrade -> no notice", in,
                               false, 94)) != 0) return 0;
        in.devBuild = true;
        in.current = parseSemver("0.1.10");
        in.target = parseSemver("0.1.9");
        if ((ec = expectNotice("dev build -> no notice", in,
                               false, 95)) != 0) return 0;
        // (c) >1 positional is flagged as a distinct grammar error.
        const UpdateArgs tm = parseUpdateArgs({"check", "v0.1.5"});
        if (tm.ok || !tm.errTooMany) {
            ec = fail("check8: 'check v0.1.5' must set errTooMany", 96);
            return 0;
        }
        const UpdateArgs bt = parseUpdateArgs({"foo"});
        if (bt.ok || bt.errTooMany) {
            ec = fail("check8: 'foo' is a bad token, not errTooMany", 97);
            return 0;
        }
        std::fprintf(stdout,
                     "    capableDowngradeNotice + errTooMany PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 9: shipped download-failure message pin (RC-24).
    //   kUpdateDownloadFailedMsg (used verbatim by cmdUpdate's fail(27,
    //   ...) arm) must equal BOTH the hardcoded `want` below AND the
    //   docs/UPDATE_DESIGN.md section 5 message table row, read live from
    //   the committed source tree. Editing either side without the other
    //   turns this check red.
    // ------------------------------------------------------------------
    {
        const std::string want =
            "the update download failed (the update service could not "
            "deliver the file). Nothing was changed. Try again; if it "
            "repeats, reinstall manually.";
        if (std::string(kUpdateDownloadFailedMsg) != want) {
            ec = fail("check9: download-fail message != want literal",
                      100);
            return 0;
        }
        // Spec-file pin: the Sec 5 "| Download fetch fails |" row carries
        // `tsdev_cli: ERROR <msg>`; the <msg> part must equal the constant
        // byte-for-byte.
        const std::string specPath = resolveUpdateDesignPath();
        if (specPath.empty()) {
            ec = fail("check9: docs/UPDATE_DESIGN.md not found "
                      "(exe dir / CWD walk-up)", 101);
            return 0;
        }
        std::string spec;
        if (!readWholeFileA(specPath.c_str(), spec)) {
            ec = fail("check9: cannot read docs/UPDATE_DESIGN.md", 101);
            return 0;
        }
        const std::string rowMark = "| Download fetch fails |";
        const size_t row = spec.find(rowMark);
        if (row == std::string::npos) {
            ec = fail("check9: download-fail row missing from spec", 102);
            return 0;
        }
        const size_t rowEnd = spec.find('\n', row);
        const std::string rowText = spec.substr(
            row, rowEnd == std::string::npos ? std::string::npos
                                             : rowEnd - row);
        const size_t q1 = rowText.find('`');
        const size_t q2 = (q1 == std::string::npos)
            ? std::string::npos : rowText.find('`', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) {
            ec = fail("check9: download-fail row has no quoted message",
                      102);
            return 0;
        }
        const std::string cell = rowText.substr(q1 + 1, q2 - q1 - 1);
        const std::string cliPrefix = "tsdev_cli: ERROR ";
        if (cell.compare(0, cliPrefix.size(), cliPrefix) != 0) {
            ec = fail("check9: download-fail row missing CLI prefix",
                      102);
            return 0;
        }
        const std::string specMsg = cell.substr(cliPrefix.size());
        if (std::string(kUpdateDownloadFailedMsg) != specMsg) {
            ec = fail("check9: download-fail message != spec file row",
                      103);
            return 0;
        }
        std::fprintf(stdout,
                      "    download-failure message pin PASS\n");
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
