// test_persistence_integrity.cpp
//
// Unit test (RED-GREEN TDD) for TVS-L.9: a SHARED persistence-integrity
// validator that classifies the cross-store (.ell vs .xml) consistency of a
// modify/upsert operation.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, does NOT construct TsDevLib, and does NOT need a
// 64 MB stack. It builds in-memory fixture bytes for the `.ell` and `.xml`
// artifacts, writes them to temp files in %TEMP%, and calls the validator
// declared in src/persistence_integrity.h.
//
// TDD RED step:
//   src/persistence_integrity.h DECLARES validatePersistenceIntegrity() but
//   there is NO src/persistence_integrity.cpp yet. This test calls the symbol,
//   so the test executable FAILS TO LINK (unresolved external) until the GREEN
//   step adds the implementation. That link failure IS the RED signal. No
//   assertion can run until the symbol resolves; the test is written so that
//   once the GREEN implementation exists, the three fixture scenarios below
//   drive the verdict matrix in the header and the assertions pass.
//
// Why this test exists (the defect it targets):
//   Fault A (docs/RE/RE_L8_STALE_WRITE_FAULT_A.md): a modify/upsert reports
//   `exit 0 / success=true` while the `.ell` is left STALE (SHA-256 unchanged)
//   and the `.xml` is rewritten with a NEW GUID that has ZERO occurrences in
//   the stale `.ell`. The item is runtime-unlinkable but reported as success.
//   The existing `.ell`/`.xml` existence checks do NOT catch this. A shared
//   validator that combines (a) ELL hash change, (b) XML hash change, (c) new
//   XML GUID present in the ELL, and (d) source-equal-proven precondition is
//   required to distinguish a real refresh from Fault A and from a legitimate
//   no-op. This test pins that contract.
//
// Fixture scenarios (each writes pre-op + post-op temp files):
//
//   (a) VerifiedRefresh — successful refresh:
//       pre-ELL: bytes WITHOUT the new GUID.
//       post-ELL: bytes WITH the new GUID embedded EXACTLY ONCE in its
//                 mixed-endian binary form, plus extra filler so the SHA-256
//                 differs from pre-ELL.
//       post-XML: minimal XML containing the new GUID text.
//       pre-XML: minimal XML containing an OLD GUID (so XML hash differs).
//       sourceEqualProven = false.
//       Expected verdict: VerifiedRefresh.
//
//   (b) FaultA_StaleEll — stale ELL + new XML GUID (Fault A):
//       pre-ELL == post-ELL (bytes identical, so ELL SHA-256 UNCHANGED), and
//       the ELL does NOT contain the new GUID.
//       post-XML: minimal XML containing the new GUID (XML hash CHANGED).
//       pre-XML: minimal XML containing an OLD GUID.
//       sourceEqualProven = false.
//       Expected verdict: FaultA_StaleEll.
//
//   (c) NoOpAllowed — identical-source no-op:
//       pre-ELL == post-ELL (ELL SHA-256 unchanged), pre-XML == post-XML
//       (XML SHA-256 unchanged, SAME GUID in both).
//       sourceEqualProven = TRUE (the caller proved source-equality first).
//       Expected verdict: NoOpAllowed.
//       This is the case that a naive "unchanged ELL hash == Fault A" check
//       would FALSELY reject; the sourceEqualProven precondition is what lets
//       the validator accept it as a no-op rather than a verified refresh.
//
// The mixed-endian GUID binary form is the standard Windows GUID layout:
//   Data1 (4 bytes, little-endian) | Data2 (2 bytes, LE) | Data3 (2 bytes, LE)
//   | Data4 (8 bytes, byte-preserved)
// This is the form orlib20/ELMDCore embeds in the `.ell`, so the validator (and
// this test's fixture builder) use it for the ELL GUID-occurrence check.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.
// (Until the GREEN step links the validator, the executable does not build, so
//  no exit code is produced — that is the RED.)

#include "../src/persistence_integrity.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// SHA-256 helper (bcrypt). Returns uppercase hex. Mirrors the project's
// existing computeSha256() style in src/orlib_resolver.cpp but operates on an
// in-memory buffer (no DLL load).
// ---------------------------------------------------------------------------
std::string sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status != 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashObjLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) throw std::runtime_error("BCryptGetProperty(OBJECT_LENGTH) failed");

    DWORD hashLen = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) throw std::runtime_error("BCryptGetProperty(HASH_LENGTH) failed");

    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                              nullptr, 0, 0);
    if (status != 0) throw std::runtime_error("BCryptCreateHash failed");
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};

    if (!data.empty()) {
        status = BCryptHashData(hHash, const_cast<PUCHAR>(data.data()),
                                static_cast<ULONG>(data.size()), 0);
        if (status != 0) throw std::runtime_error("BCryptHashData failed");
    }

    std::vector<uint8_t> hash(hashLen);
    status = BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    if (status != 0) throw std::runtime_error("BCryptFinishHash failed");

    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// GUID helpers.
//
// parseGuid: parse "{HHHHHHHH-HHHH-HHHH-HHHH-HHHHHHHHHHHH}" or the braceless
// form into 5 numeric components. Throws on malformed input.
//
// guidToMixedEndian: render a GUID as its 16-byte Windows binary layout
// (Data1 LE, Data2 LE, Data3 LE, Data4 byte-preserved) — the form embedded in
// the `.ell`.
// ---------------------------------------------------------------------------
struct GuidParts {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

GuidParts parseGuid(const std::string& s) {
    // Accept optional surrounding braces.
    std::string g = s;
    if (!g.empty() && g.front() == '{') g.erase(g.begin());
    if (!g.empty() && g.back()  == '}') g.pop_back();

    GuidParts p{};
    unsigned d[11];
    // Format: 8-4-4-4-12 hex digits with dashes at positions 8,13,18,23.
    int matched = std::sscanf(g.c_str(),
        "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d[0], &d[1], &d[2], &d[3], &d[4],
        &d[5], &d[6], &d[7], &d[8], &d[9], &d[10]);
    if (matched != 11) throw std::runtime_error("malformed GUID: " + s);
    p.data1 = static_cast<uint32_t>(d[0]);
    p.data2 = static_cast<uint16_t>(d[1]);
    p.data3 = static_cast<uint16_t>(d[2]);
    for (int i = 0; i < 8; ++i) p.data4[i] = static_cast<uint8_t>(d[3 + i]);
    return p;
}

std::vector<uint8_t> guidToMixedEndian(const GuidParts& p) {
    std::vector<uint8_t> b;
    b.reserve(16);
    // Data1 little-endian
    b.push_back(static_cast<uint8_t>(p.data1 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 24) & 0xFF));
    // Data2 little-endian
    b.push_back(static_cast<uint8_t>(p.data2 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data2 >> 8) & 0xFF));
    // Data3 little-endian
    b.push_back(static_cast<uint8_t>(p.data3 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data3 >> 8) & 0xFF));
    // Data4 byte-preserved
    for (int i = 0; i < 8; ++i) b.push_back(p.data4[i]);
    return b;
}

// ---------------------------------------------------------------------------
// Minimal XML fixture containing a GUID in a <Guid>...</Guid> element. The
// validator (GREEN) parses the GUID out of the XML; this fixture gives it
// exactly one GUID to find.
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeXmlFixture(const std::string& guidWithBraces) {
    std::string xml = "<?xml version=\"1.0\"?>\n"
                      "<EasyLanguageModule>\n"
                      "  <Name>TS_AUTO_L9_FIXTURE</Name>\n"
                      "  <Guid>" + guidWithBraces + "</Guid>\n"
                      "</EasyLanguageModule>\n";
    return std::vector<uint8_t>(xml.begin(), xml.end());
}

// ---------------------------------------------------------------------------
// ELL fixture builder. Produces a deterministic blob of filler bytes, with the
// mixed-endian GUID optionally embedded EXACTLY ONCE at a known offset. The
// filler differs by `seed` so two calls with different seeds yield different
// SHA-256s (used to make pre/post ELL hashes differ for VerifiedRefresh).
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeEllFixture(uint32_t seed,
                                    const std::vector<uint8_t>* mixedEndianGuid) {
    std::vector<uint8_t> b;
    b.reserve(256);
    // 128 bytes of deterministic filler derived from seed.
    for (int i = 0; i < 128; ++i) {
        uint8_t v = static_cast<uint8_t>((seed * 2654435761u) >> (i % 8));
        b.push_back(v);
    }
    if (mixedEndianGuid && !mixedEndianGuid->empty()) {
        // Embed the 16-byte mixed-endian GUID once.
        b.insert(b.end(), mixedEndianGuid->begin(), mixedEndianGuid->end());
    }
    // 64 more filler bytes (different per seed) to perturb the hash.
    for (int i = 0; i < 64; ++i) {
        uint8_t v = static_cast<uint8_t>((seed * 40503u + i * 7u));
        b.push_back(v);
    }
    return b;
}

// ---------------------------------------------------------------------------
// Temp directory RAII. Creates a unique subdirectory under %TEMP% and removes
// it (recursively) on destruction. Uses ANSI (A) Win32 APIs so the path stays
// in a std::string, matching the validator's std::string path parameters
// without any wchar_t<->char truncation. %TEMP% under normal Windows accounts
// is ASCII-safe; this test does not need to support non-ASCII temp paths.
// ---------------------------------------------------------------------------
struct TempDir {
    std::string path;
    bool ok = false;
    TempDir() {
        char tmp[MAX_PATH];
        DWORD len = GetTempPathA(MAX_PATH, tmp);
        if (len == 0 || len >= MAX_PATH) return;
        std::string base(tmp, len);
        char uniq[MAX_PATH];
        if (GetTempFileNameA(base.c_str(), "l9", 0, uniq) == 0) return;
        // GetTempFileNameA creates a FILE; delete it and make a directory there.
        DeleteFileA(uniq);
        if (!CreateDirectoryA(uniq, nullptr)) return;
        path = uniq;
        ok = true;
    }
    ~TempDir() {
        if (!ok || path.empty()) return;
        // Best-effort recursive removal via rmdir /s /q.
        std::string cmd = "cmd /c rmdir /s /q \"" + path + "\"";
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); }
        if (pi.hThread)  { CloseHandle(pi.hThread); }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct G { HANDLE h; ~G() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } g{h};
    DWORD written = 0;
    if (!WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr)) return false;
    return written == bytes.size();
}

std::string join(const std::string& dir, const std::string& leaf) {
    if (!dir.empty() && dir.back() != '\\') return dir + "\\" + leaf;
    return dir + leaf;
}

const char* verdictStr(tsdev::IntegrityVerdict v) {
    switch (v) {
        case tsdev::IntegrityVerdict::VerifiedRefresh:  return "VerifiedRefresh";
        case tsdev::IntegrityVerdict::NoOpAllowed:      return "NoOpAllowed";
        case tsdev::IntegrityVerdict::FaultA_StaleEll:  return "FaultA_StaleEll";
        case tsdev::IntegrityVerdict::InvalidInput:     return "InvalidInput";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Run the three fixture scenarios. Returns 0 only if all verdicts match.
// ---------------------------------------------------------------------------
int runTests() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_persistence_integrity: validatePersistenceIntegrity() ===\n");

    TempDir tmp;
    if (!tmp.ok) {
        return fail("could not create temp dir", 1);
    }
    std::fprintf(stdout, "  temp dir: %s\n", tmp.path.c_str());

    // Test GUIDs.
    const std::string newGuid = "{56E03EA7-1B8D-46E2-98F8-C31D4DEB73A5}";
    const std::string oldGuid = "{11111111-2222-3333-4444-555555555555}";
    auto newGuidMe = guidToMixedEndian(parseGuid(newGuid));

    int ec = 0;

    // -------------------------------------------------------------------
    // Scenario (a): VerifiedRefresh.
    //   pre-ELL:  filler seed 1, NO new GUID.
    //   post-ELL: filler seed 2, new GUID embedded ONCE (mixed-endian).
    //   pre-XML:  old GUID.   post-XML: new GUID.
    //   sourceEqualProven = false.
    //   Expected: VerifiedRefresh.
    // -------------------------------------------------------------------
    {
        auto preEll  = makeEllFixture(1, nullptr);
        auto postEll = makeEllFixture(2, &newGuidMe);
        auto preXml  = makeXmlFixture(oldGuid);
        auto postXml = makeXmlFixture(newGuid);

        std::string preEllPath  = join(tmp.path, "a_pre.ell");
        std::string postEllPath = join(tmp.path, "a_post.ell");
        std::string preXmlPath  = join(tmp.path, "a_pre.xml");
        std::string postXmlPath = join(tmp.path, "a_post.xml");
        if (!writeFile(preEllPath, preEll)  || !writeFile(postEllPath, postEll) ||
            !writeFile(preXmlPath, preXml)  || !writeFile(postXmlPath, postXml)) {
            ec = fail("scenario (a): failed to write fixtures", 10);
            return ec;
        }

        std::string preEllHash = sha256Hex(preEll);
        std::string preXmlHash = sha256Hex(preXml);

        // Sanity: the fixture really did change both hashes and embed the GUID.
        std::string postEllHash = sha256Hex(postEll);
        std::string postXmlHash = sha256Hex(postXml);
        if (preEllHash == postEllHash) {
            ec = fail("scenario (a): ELL hash did not change (bad fixture)", 11);
            return ec;
        }
        if (preXmlHash == postXmlHash) {
            ec = fail("scenario (a): XML hash did not change (bad fixture)", 12);
            return ec;
        }
        // Confirm GUID occurs exactly once in post-ELL (mixed-endian).
        {
            int occ = 0;
            for (size_t i = 0; i + 16 <= postEll.size(); ++i) {
                if (std::memcmp(postEll.data() + i, newGuidMe.data(), 16) == 0) ++occ;
            }
            if (occ != 1) {
                ec = fail("scenario (a): GUID occurrence != 1 in fixture ELL", 13);
                return ec;
            }
        }

        tsdev::IntegrityInput in;
        in.ellPath = postEllPath;
        in.xmlPath = postXmlPath;
        in.preEllSha256 = preEllHash;
        in.preXmlSha256 = preXmlHash;
        in.sourceEqualProven = false;

        tsdev::IntegrityVerdict v = tsdev::validatePersistenceIntegrity(in);
        std::fprintf(stdout, "  [a] VerifiedRefresh: got %s\n", verdictStr(v));
        if (v != tsdev::IntegrityVerdict::VerifiedRefresh) {
            ec = fail("scenario (a): expected VerifiedRefresh", 14);
            return ec;
        }
        std::fprintf(stdout, "    PASS: scenario (a) VerifiedRefresh\n");
    }

    // -------------------------------------------------------------------
    // Scenario (b): FaultA_StaleEll.
    //   pre-ELL == post-ELL (stale, NO new GUID) -> ELL hash UNCHANGED.
    //   pre-XML: old GUID.   post-XML: new GUID -> XML hash CHANGED.
    //   sourceEqualProven = false.
    //   Expected: FaultA_StaleEll.
    // -------------------------------------------------------------------
    {
        auto staleEll = makeEllFixture(3, nullptr);  // no new GUID
        auto preXml   = makeXmlFixture(oldGuid);
        auto postXml  = makeXmlFixture(newGuid);

        std::string ellPath  = join(tmp.path, "b.ell");
        std::string preXmlPath  = join(tmp.path, "b_pre.xml");
        std::string postXmlPath = join(tmp.path, "b_post.xml");
        if (!writeFile(ellPath, staleEll) ||
            !writeFile(preXmlPath, preXml) || !writeFile(postXmlPath, postXml)) {
            ec = fail("scenario (b): failed to write fixtures", 20);
            return ec;
        }

        std::string preEllHash = sha256Hex(staleEll); // pre == post
        std::string preXmlHash = sha256Hex(preXml);

        // Sanity: ELL hash unchanged by construction, XML hash changed.
        if (sha256Hex(staleEll) != preEllHash) {
            ec = fail("scenario (b): ELL hash unexpectedly changed", 21);
            return ec;
        }
        if (sha256Hex(postXml) == preXmlHash) {
            ec = fail("scenario (b): XML hash did not change (bad fixture)", 22);
            return ec;
        }
        // Confirm new GUID is ABSENT from stale ELL.
        {
            bool found = false;
            for (size_t i = 0; i + 16 <= staleEll.size(); ++i) {
                if (std::memcmp(staleEll.data() + i, newGuidMe.data(), 16) == 0) {
                    found = true; break;
                }
            }
            if (found) {
                ec = fail("scenario (b): new GUID unexpectedly present in stale ELL", 23);
                return ec;
            }
        }

        tsdev::IntegrityInput in;
        in.ellPath = ellPath;
        in.xmlPath = postXmlPath;
        in.preEllSha256 = preEllHash;
        in.preXmlSha256 = preXmlHash;
        in.sourceEqualProven = false;

        tsdev::IntegrityVerdict v = tsdev::validatePersistenceIntegrity(in);
        std::fprintf(stdout, "  [b] FaultA_StaleEll: got %s\n", verdictStr(v));
        if (v != tsdev::IntegrityVerdict::FaultA_StaleEll) {
            ec = fail("scenario (b): expected FaultA_StaleEll", 24);
            return ec;
        }
        std::fprintf(stdout, "    PASS: scenario (b) FaultA_StaleEll\n");
    }

    // -------------------------------------------------------------------
    // Scenario (c): NoOpAllowed.
    //   pre-ELL == post-ELL (unchanged), pre-XML == post-XML (unchanged,
    //   SAME GUID in both).
    //   sourceEqualProven = TRUE.
    //   Expected: NoOpAllowed.
    //   This is the case a naive "unchanged ELL == Fault A" check would
    //   wrongly reject; the sourceEqualProven precondition is what allows it.
    // -------------------------------------------------------------------
    {
        auto ell = makeEllFixture(4, nullptr);
        auto xml = makeXmlFixture(newGuid);  // same GUID pre and post

        std::string ellPath = join(tmp.path, "c.ell");
        std::string xmlPath = join(tmp.path, "c.xml");
        if (!writeFile(ellPath, ell) || !writeFile(xmlPath, xml)) {
            ec = fail("scenario (c): failed to write fixtures", 30);
            return ec;
        }

        std::string preEllHash = sha256Hex(ell);
        std::string preXmlHash = sha256Hex(xml);

        tsdev::IntegrityInput in;
        in.ellPath = ellPath;
        in.xmlPath = xmlPath;
        in.preEllSha256 = preEllHash;
        in.preXmlSha256 = preXmlHash;
        in.sourceEqualProven = true;

        tsdev::IntegrityVerdict v = tsdev::validatePersistenceIntegrity(in);
        std::fprintf(stdout, "  [c] NoOpAllowed: got %s\n", verdictStr(v));
        if (v != tsdev::IntegrityVerdict::NoOpAllowed) {
            ec = fail("scenario (c): expected NoOpAllowed", 34);
            return ec;
        }
        std::fprintf(stdout, "    PASS: scenario (c) NoOpAllowed\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

} // namespace

int main() {
    try {
        int ec = runTests();
        std::fprintf(stdout,
                     "\n=== test_persistence_integrity complete (exit %d) ===\n", ec);
        if (ec == 0) std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
        else         std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
        return ec;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\n[FATAL] unhandled exception: %s\n", e.what());
        return 99;
    }
}
