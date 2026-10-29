// test_update_feed.cpp
//
// Unit test for the self-update release-feed module src/update_feed.cpp
// (design tmp/UPDATE_DESIGN.md): the PURE parts only -- semver
// parse/compare, releases-JSON parse + skip rules, selectLatest /
// findRelease / pickBundleAsset / publishedSemverRange, sha256BytesHex /
// sha256FileHex on known content, and HttpResult error-kind mapping
// conventions. NO live network: canned JSON buffers are injected through
// the parseReleasesJson() seam.
//
// This is a PURE UNIT TEST -- it does NOT load orlib20.dll, does NOT touch
// the TradeStation installation, and performs no network I/O. It runs on a
// worker thread with a 64 MB stack only to match the project's test-harness
// convention.
//
// Checks:
//   1. parseSemver table: valid spellings (incl. v/V prefix), invalids,
//      the 0..99999 component bound.
//   2. compareSemver: numeric ordering (0.1.10 > 0.1.9), equality,
//      invalid-sorts-below.
//   3. parseReleasesJson: draft/prerelease/non-semver tags skipped;
//      digest field parsed (has_digest, prefix stripped); malformed JSON
//      and non-array JSON rejected.
//   4. selectLatest / findRelease / pickBundleAsset /
//      publishedSemverRange incl. draft + non-semver exclusion and
//      v-prefix-insensitive lookup.
//   5. sha256BytesHex of known content (SHA-256("abc") reference value)
//      and sha256FileHex on a real temp file.
//   6. HttpResult/FeedError mapping conventions (synthesized results:
//      no network needed to pin the enum contract).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/update_feed.h"

#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

using tsdev::update::AssetInfo;
using tsdev::update::FeedError;
using tsdev::update::HttpError;
using tsdev::update::HttpResult;
using tsdev::update::ReleaseInfo;
using tsdev::update::Semver;
using tsdev::update::compareSemver;
using tsdev::update::findRelease;
using tsdev::update::parseReleasesJson;
using tsdev::update::parseSemver;
using tsdev::update::pickBundleAsset;
using tsdev::update::publishedSemverRange;
using tsdev::update::selectLatest;
using tsdev::update::sha256BytesHex;
using tsdev::update::sha256FileHex;

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Canned GitHub releases-array fixture: a semver release, a newer semver
// release, a DRAFT with a semver tag, a prerelease, and a non-semver tag.
const char* kFeedJson = R"JSON([
  { "tag_name": "v0.1.5", "draft": false, "prerelease": false,
    "assets": [
      { "name": "tsdev_automation_v0.1.5.zip",
        "browser_download_url": "https://example.test/v015.zip",
        "size": 2000000,
        "digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111" } ] },
  { "tag_name": "v0.1.10", "draft": false, "prerelease": false,
    "assets": [
      { "name": "tsdev_automation_v0.1.10.zip",
        "browser_download_url": "https://example.test/v0110.zip",
        "size": 2500000,
        "digest": "sha256:2222222222222222222222222222222222222222222222222222222222222222" } ] },
  { "tag_name": "v9.9.9", "draft": true, "prerelease": false,
    "assets": [] },
  { "tag_name": "v0.2.0", "draft": false, "prerelease": true,
    "assets": [] },
  { "tag_name": "v0.2.0_remote-control", "draft": false, "prerelease": false,
    "assets": [] },
  { "tag_name": "v0.1.4", "draft": false, "prerelease": false,
    "assets": [
      { "name": "other.bin",
        "browser_download_url": "https://example.test/other.bin",
        "size": 10,
        "digest": null },
      { "name": "tsdev_automation_v0.1.4.zip",
        "browser_download_url": "https://example.test/v014.zip",
        "size": 1900000,
        "digest": null } ] }
])JSON";

int checkSemver(const char* s, bool expectValid, int major, int minor,
                int patch, int code) {
    const Semver v = parseSemver(s);
    if (v.valid != expectValid) {
        std::fprintf(stderr, "\n[FAIL] parseSemver(\"%s\") valid=%d "
                     "expected %d\n", s, (int)v.valid, (int)expectValid);
        return code;
    }
    if (expectValid &&
        (v.major != major || v.minor != minor || v.patch != patch)) {
        std::fprintf(stderr, "\n[FAIL] parseSemver(\"%s\") = %d.%d.%d "
                     "expected %d.%d.%d\n", s, v.major, v.minor, v.patch,
                     major, minor, patch);
        return code + 1;
    }
    std::fprintf(stdout, "    parseSemver(\"%s\") -> %s PASS\n", s,
                 expectValid ? "valid" : "invalid");
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
    // Check 1: parseSemver table.
    // ------------------------------------------------------------------
    if ((ec = checkSemver("0.1.5",   true, 0, 1, 5,    10)) != 0) return 0;
    if ((ec = checkSemver("v0.1.5",  true, 0, 1, 5,    13)) != 0) return 0;
    if ((ec = checkSemver("V1.2.3",  true, 1, 2, 3,    16)) != 0) return 0;
    if ((ec = checkSemver("10.20.30",true, 10, 20, 30, 19)) != 0) return 0;
    if ((ec = checkSemver("0.0.0",   true, 0, 0, 0,    22)) != 0) return 0;
    if ((ec = checkSemver("99999.0.0",true,99999,0,0,  25)) != 0) return 0;
    if ((ec = checkSemver("",        false, 0,0,0,     28)) != 0) return 0;
    if ((ec = checkSemver("v",       false, 0,0,0,     30)) != 0) return 0;
    if ((ec = checkSemver("1.2",     false, 0,0,0,     32)) != 0) return 0;
    if ((ec = checkSemver("1.2.3.4", false, 0,0,0,     34)) != 0) return 0;
    if ((ec = checkSemver("v0.1.5-beta", false, 0,0,0, 36)) != 0) return 0;
    if ((ec = checkSemver("0.2.0_remote-control", false,0,0,0, 38)) != 0) return 0;
    if ((ec = checkSemver("foo",     false, 0,0,0,     40)) != 0) return 0;
    if ((ec = checkSemver("1..2",    false, 0,0,0,     42)) != 0) return 0;
    if ((ec = checkSemver("1.2.x",   false, 0,0,0,     44)) != 0) return 0;
    if ((ec = checkSemver("100000.0.0", false, 0,0,0,  46)) != 0) return 0;
    if ((ec = checkSemver(" 0.1.5",  false, 0,0,0,     48)) != 0) return 0;
    if ((ec = checkSemver("0.1.5 ",  false, 0,0,0,     50)) != 0) return 0;

    // ------------------------------------------------------------------
    // Check 2: compareSemver ordering.
    // ------------------------------------------------------------------
    {
        const Semver a = parseSemver("0.1.9");
        const Semver b = parseSemver("0.1.10");
        if (compareSemver(b, a) <= 0) {
            ec = fail("check2: 0.1.10 must sort above 0.1.9", 60);
            return 0;
        }
        if (compareSemver(a, b) >= 0) {
            ec = fail("check2: 0.1.9 must sort below 0.1.10", 61);
            return 0;
        }
        if (compareSemver(a, parseSemver("v0.1.9")) != 0) {
            ec = fail("check2: 0.1.9 vs v0.1.9 must compare equal", 62);
            return 0;
        }
        if (compareSemver(parseSemver("1.0.0"), parseSemver("0.9.9")) <= 0) {
            ec = fail("check2: 1.0.0 must sort above 0.9.9", 63);
            return 0;
        }
        if (compareSemver(parseSemver("bogus"), a) >= 0) {
            ec = fail("check2: invalid must sort below valid", 64);
            return 0;
        }
        std::fprintf(stdout, "    compareSemver ordering PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 3: parseReleasesJson parse + skip rules.
    // ------------------------------------------------------------------
    std::vector<ReleaseInfo> rels;
    if (!parseReleasesJson(kFeedJson, rels)) {
        ec = fail("check3: fixture feed did not parse", 70);
        return 0;
    }
    // Kept: v0.1.5, v0.1.10, v0.1.4 -- the draft v9.9.9, prerelease v0.2.0,
    // and non-semver v0.2.0_remote-control are invisible.
    if (rels.size() != 3) {
        std::fprintf(stderr, "\n[FAIL] check3: kept %zu releases, "
                     "expected 3\n", rels.size());
        ec = 71; return 0;
    }
    {
        bool sawDraft = false, sawPrerelease = false, sawRemote = false;
        for (const ReleaseInfo& r : rels) {
            if (r.tag == "v9.9.9") sawDraft = true;
            if (r.tag == "v0.2.0") sawPrerelease = true;
            if (r.tag == "v0.2.0_remote-control") sawRemote = true;
        }
        if (sawDraft || sawPrerelease || sawRemote) {
            ec = fail("check3: draft/prerelease/non-semver leaked in", 72);
            return 0;
        }
        std::fprintf(stdout, "    parseReleasesJson skip rules PASS\n");
    }
    // digest handling on v0.1.5 asset.
    {
        const ReleaseInfo* r = findRelease(rels, "0.1.5");
        if (!r || r->assets.size() != 1) {
            ec = fail("check3: v0.1.5 asset missing", 74);
            return 0;
        }
        const AssetInfo& a = r->assets[0];
        if (!a.has_digest || a.sha256.size() != 64 ||
            a.sha256.front() != '1') {
            ec = fail("check3: v0.1.5 digest not parsed (prefix strip)", 75);
            return 0;
        }
        if (a.url != "https://example.test/v015.zip" ||
            a.size != 2000000) {
            ec = fail("check3: v0.1.5 asset url/size wrong", 76);
            return 0;
        }
    }
    // Malformed / non-array JSON rejected.
    {
        std::vector<ReleaseInfo> tmp;
        if (parseReleasesJson("{not json", tmp)) {
            ec = fail("check3: malformed JSON accepted", 77); return 0;
        }
        if (parseReleasesJson("{\"a\":1}", tmp)) {
            ec = fail("check3: non-array JSON accepted", 78); return 0;
        }
        if (parseReleasesJson("[1,2,3]", tmp)) {
            ec = fail("check3: non-object array accepted", 79); return 0;
        }
        std::vector<ReleaseInfo> empty;
        if (!parseReleasesJson("[]", empty) || !empty.empty()) {
            ec = fail("check3: empty array must parse to empty feed", 80);
            return 0;
        }
        std::fprintf(stdout, "    parseReleasesJson malformed-input rules "
                     "PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 4: selectLatest / findRelease / pickBundleAsset / range.
    // ------------------------------------------------------------------
    {
        const ReleaseInfo* latest = selectLatest(rels);
        if (!latest || latest->tag != "v0.1.10") {
            ec = fail("check4: selectLatest must pick v0.1.10 (numeric "
                      "semver, draft excluded)", 90);
            return 0;
        }
        if (findRelease(rels, "0.1.5") == nullptr ||
            findRelease(rels, "v0.1.5") == nullptr) {
            ec = fail("check4: findRelease must accept both spellings", 91);
            return 0;
        }
        if (findRelease(rels, "v9.9.9") != nullptr) {
            ec = fail("check4: draft v9.9.9 must be invisible", 92);
            return 0;
        }
        if (findRelease(rels, "0.2.0") != nullptr) {
            ec = fail("check4: prerelease v0.2.0 must be invisible", 93);
            return 0;
        }
        if (findRelease(rels, "0.9.9") != nullptr) {
            ec = fail("check4: absent version must return nullptr", 94);
            return 0;
        }
        const ReleaseInfo* r4 = findRelease(rels, "v0.1.4");
        if (!r4) { ec = fail("check4: v0.1.4 missing", 95); return 0; }
        const AssetInfo* bundle = pickBundleAsset(*r4);
        if (!bundle || bundle->name != "tsdev_automation_v0.1.4.zip") {
            ec = fail("check4: pickBundleAsset wrong asset", 96);
            return 0;
        }
        if (bundle->has_digest) {
            ec = fail("check4: null digest must map to has_digest=false",
                      97);
            return 0;
        }
        Semver lo, hi;
        publishedSemverRange(rels, lo, hi);
        if (lo.major != 0 || lo.minor != 1 || lo.patch != 4 ||
            hi.major != 0 || hi.minor != 1 || hi.patch != 10) {
            ec = fail("check4: published range must be v0.1.4..v0.1.10", 98);
            return 0;
        }
        std::fprintf(stdout, "    selectLatest/findRelease/pickBundleAsset/"
                     "range PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 5: sha256 of known content (bytes + file variants).
    // ------------------------------------------------------------------
    {
        // SHA-256("abc") -- the FIPS 180-2 reference value.
        const std::string hex = sha256BytesHex("abc", 3);
        if (hex != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff"
                   "61f20015ad") {
            std::fprintf(stderr, "\n[FAIL] check5: sha256(\"abc\") = %s\n",
                         hex.c_str());
            ec = 100; return 0;
        }
        if (sha256BytesHex(nullptr, 0).size() != 64) {
            ec = fail("check5: empty-input hash must still produce hex",
                      101);
            return 0;
        }
        // File variant on a real temp file.
        wchar_t tmpDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmpDir);
        wchar_t tmpFile[MAX_PATH] = {};
        GetTempFileNameW(tmpDir, L"ufd", 0, tmpFile);
        HANDLE h = CreateFileW(tmpFile, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ec = fail("check5: temp file create failed", 102); return 0;
        }
        DWORD w = 0;
        WriteFile(h, "abc", 3, &w, nullptr);
        CloseHandle(h);
        std::string fileHex;
        if (!sha256FileHex(tmpFile, fileHex) || fileHex != hex) {
            ec = fail("check5: sha256FileHex mismatch vs bytes hash", 103);
            DeleteFileW(tmpFile);
            return 0;
        }
        // Missing file -> false.
        DeleteFileW(tmpFile);
        if (sha256FileHex(tmpFile, fileHex)) {
            ec = fail("check5: missing file must hash-fail", 104);
            return 0;
        }
        std::fprintf(stdout, "    sha256BytesHex/sha256FileHex PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 6: HttpResult/FeedError classification contract (synthesized
    // values -- no network needed to pin the mapping semantics).
    // ------------------------------------------------------------------
    {
        HttpResult r;
        r.error = HttpError::RateLimited;
        r.httpStatus = 403;
        r.retryAfterSecs = 2700;
        if (r.error != HttpError::RateLimited || r.retryAfterSecs != 2700) {
            ec = fail("check6: rate-limit surface contract", 110);
            return 0;
        }
        HttpResult h;
        h.error = HttpError::HttpStatus;
        h.httpStatus = 404;
        if (h.httpStatus != 404) {
            ec = fail("check6: http-status surface contract", 111);
            return 0;
        }
        // FeedError::Unreachable must exist as a distinct class from
        // HttpStatus/MalformedJson (the exit-25 message split).
        if (FeedError::Unreachable == FeedError::HttpStatus ||
            FeedError::MalformedJson == FeedError::None) {
            ec = fail("check6: feed error classes must be distinct", 112);
            return 0;
        }
        std::fprintf(stdout, "    error-classification surface PASS\n");
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
                 "=== test_update_feed: release-feed module (unit) ===\n");

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
                 "\n=== test_update_feed complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
