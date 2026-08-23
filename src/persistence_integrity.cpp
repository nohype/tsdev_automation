// persistence_integrity.cpp
//
// TVS-L.9 GREEN: implementation of the shared persistence-integrity validator
// declared in src/persistence_integrity.h.
//
// This is the GREEN step that resolves the symbol the TDD RED test in
// tests/test_persistence_integrity.cpp links against. The verdict matrix
// encoded below is the EXACT matrix documented in the header; no behavior is
// invented beyond it.
//
// Implementation notes (kept deliberately minimal — no extra abstractions):
//   * SHA-256 is computed with the native Win32 bcrypt provider (the same
//     provider orlib_resolver.cpp and the RED test already use). No third-party
//     crypto. bcrypt is linked transitively via the tsdev_lib -> orlib_resolver
//     -> bcrypt PUBLIC chain (see CMakeLists.txt).
//   * Files are read with the ANSI (A) Win32 APIs into a flat byte buffer,
//     matching the std::string path parameters in IntegrityInput (no
//     wchar_t<->char truncation).
//   * "Regular file" requirement: a path that does not exist or is a directory
//     is rejected as InvalidInput before any hashing (the header's
//     "unreadable / unparseable input" row).
//   * The XML GUID is extracted by scanning the raw XML bytes for the first
//     brace-delimited GUID token of the form
//       {HHHHHHHH-HHHH-HHHH-HHHH-HHHHHHHHHHHH}
//     This matches BOTH the real TradeStation descriptor XML
//       (<symbols ... version="{...}">) AND the unit-test fixture
//       (<Guid>{...}</Guid>). No EasyLanguage source is parsed (HARD RULE 0:
//     metadata never comes from item source content). The GUID is the only
//     metadata read here, and it comes from the .xml descriptor artifact, not
//     from EL source.
//   * The mixed-endian GUID binary form is the standard Windows GUID layout
//     (Data1 LE, Data2 LE, Data3 LE, Data4 byte-preserved) — the form
//     orlib20/ELMDCore embeds in the .ell. The validator counts occurrences of
//     this 16-byte sequence in the post-op .ell and requires EXACTLY ONE for
//     guidInEll to be true (zero or >1 -> false).
//   * Pre-hash comparison is case-insensitive (the header permits upper or
//     lower hex). An EMPTY pre-hash means "unknown" and MUST yield
//     InvalidInput (the header is explicit; the implementation does not guess).

#include "persistence_integrity.h"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tsdev {

namespace {

// ---------------------------------------------------------------------------
// RAII guards for bcrypt handles (mirrors the project's existing style in
// orlib_resolver.cpp / the RED test, kept local to this TU).
// ---------------------------------------------------------------------------
struct AlgGuard {
    BCRYPT_ALG_HANDLE h;
    ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct HashGuard {
    BCRYPT_HASH_HANDLE h;
    ~HashGuard() { if (h) BCryptDestroyHash(h); }
};

// ---------------------------------------------------------------------------
// sha256File: compute the uppercase-hex SHA-256 of a regular file at `path`.
// Returns empty string on any failure (open / read / hash). The caller treats
// an empty result as "could not hash" -> InvalidInput.
// ---------------------------------------------------------------------------
std::string sha256File(const std::string& path) {
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return std::string();
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fileGuard{hFile};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status != 0) return std::string();
    AlgGuard algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashObjLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) return std::string();

    DWORD hashLen = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) return std::string();

    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                              nullptr, 0, 0);
    if (status != 0) return std::string();
    HashGuard hashGuard{hHash};

    // Feed the file in 64 KB chunks (same chunk size as orlib_resolver).
    const DWORD chunkSize = 65536;
    std::vector<uint8_t> chunk(chunkSize);
    for (;;) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, chunk.data(), chunkSize, &bytesRead, nullptr);
        if (!ok) return std::string();
        if (bytesRead == 0) break;  // EOF
        status = BCryptHashData(hHash, chunk.data(), bytesRead, 0);
        if (status != 0) return std::string();
    }

    std::vector<uint8_t> hash(hashLen);
    status = BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    if (status != 0) return std::string();

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
// isRegularFile: true iff `path` exists and is NOT a directory. Used to
// satisfy the "require regular files" precondition before hashing/parsing.
// ---------------------------------------------------------------------------
bool isRegularFile(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
    return true;
}

// ---------------------------------------------------------------------------
// readFileBytes: read an entire (regular) file into a byte buffer. Returns
// false on any I/O failure.
// ---------------------------------------------------------------------------
bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fileGuard{hFile};

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) return false;
    if (fileSize.QuadPart < 0) return false;
    // Cap at a sane upper bound (256 MB) to avoid unbounded allocation on a
    // malformed path; .ell/.xml descriptors are far smaller than this.
    if (static_cast<unsigned long long>(fileSize.QuadPart) > 256ull * 1024 * 1024) {
        return false;
    }
    out.resize(static_cast<size_t>(fileSize.QuadPart));
    if (out.empty()) return true;  // zero-byte file is a valid (read) result
    DWORD totalRead = 0;
    while (totalRead < out.size()) {
        DWORD toRead = static_cast<DWORD>(out.size() - totalRead);
        if (toRead > 65536) toRead = 65536;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, out.data() + totalRead, toRead, &bytesRead,
                           nullptr);
        if (!ok || bytesRead == 0) return false;
        totalRead += bytesRead;
    }
    return true;
}

// ---------------------------------------------------------------------------
// hexEqual: case-insensitive ASCII comparison of two hex strings. Used for
// pre/post hash comparison (the header permits upper or lower hex from the
// caller).
// ---------------------------------------------------------------------------
bool hexEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// parseGuidFromXml: scan the XML bytes for the FIRST brace-delimited GUID
// token {HHHHHHHH-HHHH-HHHH-HHHH-HHHHHHHHHHHH}. Returns true and fills `out`
// on success; returns false if no such token is found or it is malformed.
//
// This finds the GUID in BOTH the real TS descriptor XML
//   <symbols ... version="{A54C4F30-9217-4102-BD43-445D9AB5E7DA}">
// and the unit-test fixture
//   <Guid>{56E03EA7-1B8D-46E2-98F8-C31D4DEB73A5}</Guid>
// because both contain exactly one brace-delimited GUID token. No EasyLanguage
// source is parsed.
// ---------------------------------------------------------------------------
bool isHexDigit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool parseGuidFromXml(const std::vector<uint8_t>& xml, std::string& out) {
    if (xml.empty()) return false;
    const size_t n = xml.size();
    // A brace-delimited GUID token is 38 chars: '{' + 36 GUID chars + '}'.
    // The canonical text form is 8-4-4-4-12 hex digits with dashes.
    for (size_t i = 0; i + 38 <= n; ++i) {
        if (xml[i] != '{') continue;
        // Check the structural skeleton: positions of '-' and the closing '}'.
        const char* p = reinterpret_cast<const char*>(xml.data() + i);
        // Indices within the 38-char window: 0='{', 9='-', 14='-', 19='-',
        // 24='-', 37='}'. Hex digits everywhere else.
        if (p[9]  != '-' || p[14] != '-' || p[19] != '-' || p[24] != '-' ||
            p[37] != '}') {
            continue;
        }
        bool ok = true;
        for (size_t j = 1; j < 37; ++j) {
            if (j == 9 || j == 14 || j == 19 || j == 24) continue;
            if (!isHexDigit(p[j])) { ok = false; break; }
        }
        if (!ok) continue;
        out.assign(p, 38);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// parseGuidText: parse "{HHHHHHHH-HHHH-HHHH-HHHH-HHHHHHHHHHHH}" (with or
// without surrounding braces) into 5 numeric components. Returns false on
// malformed input.
// ---------------------------------------------------------------------------
struct GuidParts {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

bool parseGuidText(const std::string& s, GuidParts& out) {
    std::string g = s;
    if (!g.empty() && g.front() == '{') g.erase(g.begin());
    if (!g.empty() && g.back()  == '}') g.pop_back();
    unsigned d[11];
    int matched = std::sscanf(g.c_str(),
        "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d[0], &d[1], &d[2], &d[3], &d[4],
        &d[5], &d[6], &d[7], &d[8], &d[9], &d[10]);
    if (matched != 11) return false;
    out.data1 = static_cast<uint32_t>(d[0]);
    out.data2 = static_cast<uint16_t>(d[1]);
    out.data3 = static_cast<uint16_t>(d[2]);
    for (int i = 0; i < 8; ++i) out.data4[i] = static_cast<uint8_t>(d[3 + i]);
    return true;
}

// ---------------------------------------------------------------------------
// guidToMixedEndian: render a GUID as its 16-byte Windows binary layout
// (Data1 LE, Data2 LE, Data3 LE, Data4 byte-preserved) — the form embedded in
// the .ell by orlib20/ELMDCore.
// ---------------------------------------------------------------------------
std::vector<uint8_t> guidToMixedEndian(const GuidParts& p) {
    std::vector<uint8_t> b;
    b.reserve(16);
    b.push_back(static_cast<uint8_t>(p.data1 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data1 >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>(p.data2 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data2 >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(p.data3 & 0xFF));
    b.push_back(static_cast<uint8_t>((p.data3 >> 8) & 0xFF));
    for (int i = 0; i < 8; ++i) b.push_back(p.data4[i]);
    return b;
}

// ---------------------------------------------------------------------------
// countOccurrences: count overlapping occurrences of a 16-byte needle in a
// haystack. (GUID embeddings are fixed 16-byte windows; overlapping matches
// are not expected in practice but counting them is the conservative choice
// for the "exactly one" requirement.)
// ---------------------------------------------------------------------------
int countOccurrences(const std::vector<uint8_t>& haystack,
                     const std::vector<uint8_t>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return 0;
    const size_t n = haystack.size();
    const size_t m = needle.size();
    int count = 0;
    for (size_t i = 0; i + m <= n; ++i) {
        if (std::memcmp(haystack.data() + i, needle.data(), m) == 0) ++count;
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// validatePersistenceIntegrity: the GREEN implementation of the verdict
// matrix in persistence_integrity.h. See the header for the full matrix and
// the rationale; this function encodes it exactly.
// ---------------------------------------------------------------------------
IntegrityVerdict validatePersistenceIntegrity(const IntegrityInput& in) {
    // --- Pre-hash precondition: empty pre-hash means "unknown" -> InvalidInput.
    //     The header is explicit: the implementation MUST NOT guess.
    if (in.preEllSha256.empty() || in.preXmlSha256.empty()) {
        return IntegrityVerdict::InvalidInput;
    }

    // --- Require regular files (not missing, not directories).
    if (!isRegularFile(in.ellPath) || !isRegularFile(in.xmlPath)) {
        return IntegrityVerdict::InvalidInput;
    }

    // --- Compute post-op hashes.
    std::string postEllHash = sha256File(in.ellPath);
    if (postEllHash.empty()) return IntegrityVerdict::InvalidInput;
    std::string postXmlHash = sha256File(in.xmlPath);
    if (postXmlHash.empty()) return IntegrityVerdict::InvalidInput;

    // --- Determine ellChanged / xmlChanged (case-insensitive hex compare).
    bool ellChanged = !hexEqual(in.preEllSha256, postEllHash);
    bool xmlChanged = !hexEqual(in.preXmlSha256, postXmlHash);

    // --- Read the .ell bytes (for the GUID-occurrence search).
    std::vector<uint8_t> ellBytes;
    if (!readFileBytes(in.ellPath, ellBytes)) {
        return IntegrityVerdict::InvalidInput;
    }

    // --- Read the .xml bytes and parse the GUID out of it.
    std::vector<uint8_t> xmlBytes;
    if (!readFileBytes(in.xmlPath, xmlBytes)) {
        return IntegrityVerdict::InvalidInput;
    }
    std::string guidText;
    if (!parseGuidFromXml(xmlBytes, guidText)) {
        return IntegrityVerdict::InvalidInput;  // GUID could not be parsed
    }
    GuidParts parts;
    if (!parseGuidText(guidText, parts)) {
        return IntegrityVerdict::InvalidInput;
    }
    std::vector<uint8_t> me = guidToMixedEndian(parts);

    // --- guidInEll: the mixed-endian GUID occurs EXACTLY ONCE in the .ell.
    int occ = countOccurrences(ellBytes, me);
    bool guidInEll = (occ == 1);

    // --- Apply the verdict matrix from the header.
    //
    //   ellChanged | xmlChanged | guidInEll | sourceEqualProven | Verdict
    //   -----------+------------+-----------+-------------------+----------------------
    //      true    |    any     |   true    |       any         | VerifiedRefresh
    //      true    |    any     |   false   |       any         | FaultA_StaleEll (***)
    //      false   |   false    |   any     |       true        | NoOpAllowed
    //      false   |   false    |   any     |      false        | FaultA_StaleEll (*)
    //      false   |   true     |   any     |       true        | FaultA_StaleEll (**)
    //      false   |   true     |   any     |      false        | FaultA_StaleEll
    //
    // (The single uncovered combination — ellChanged=F, xmlChanged=T,
    //  guidInEll=T, sourceEqualProven=F — falls through to the last branch
    //  and is classified as FaultA_StaleEll: a stale .ell that the caller
    //  could not prove was a no-op. This is the conservative, fail-safe
    //  choice mandated by "require hash refresh unless caller proves no-op".)
    if (ellChanged) {
        if (guidInEll) return IntegrityVerdict::VerifiedRefresh;
        return IntegrityVerdict::FaultA_StaleEll;  // (***)
    }
    // ellChanged == false
    if (in.sourceEqualProven) {
        if (!xmlChanged) return IntegrityVerdict::NoOpAllowed;
        return IntegrityVerdict::FaultA_StaleEll;  // (**)
    }
    // ellChanged == false && sourceEqualProven == false: stale .ell that the
    // caller could not prove was a no-op -> reject (rows (*) and the
    // uncovered combination above).
    return IntegrityVerdict::FaultA_StaleEll;
}

} // namespace tsdev
