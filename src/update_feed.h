// update_feed.h -- self-update release feed (design docs/UPDATE_DESIGN.md).
//
// Pure-Win32 module (WinHTTP + BCrypt + nlohmann/json; no orlib20, no TS).
// Two layers:
//   1. Pure helpers -- semver parse/compare, releases-JSON parse, release
//      selection, sha256 -- fully unit-testable without a network.
//   2. Network layer -- WinHTTP GET to a string (API cap) or to a file
//      (streaming download). Error surface distinguishes unreachable /
//      HTTP-error(status) / rate-limited (retry_after_secs) / too-large.
//
// The release feed expects the GitHub REST releases-array shape
// (GET <base>/releases?per_page=100). Draft + prerelease entries and
// non-semver tags are invisible to the feed (skipped at parse).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tsdev {
namespace update {

// --- Semver --------------------------------------------------------------

// Parsed ^[vV]?\d+\.\d+\.\d+$ version. Components are bounded 0..99999
// (overflow-proof for this project's range). valid==false means the input
// did not match the strict grammar.
struct Semver {
    int  major = 0;
    int  minor = 0;
    int  patch = 0;
    bool valid = false;
};

// Strict parse: optional leading v/V, exactly three all-digit components
// separated by dots, each 0..99999. Anything else -> valid==false.
Semver parseSemver(const std::string& s);

// Numeric three-component compare: -1 a<b, 0 equal, +1 a>b. Callers only
// compare valid semvers; an invalid side sorts below any valid side.
int compareSemver(const Semver& a, const Semver& b);

// --- Feed types ------------------------------------------------------------

struct AssetInfo {
    std::string name;      // e.g. tsdev_automation_v0.1.6.zip
    std::string url;       // browser_download_url, used verbatim
    uint64_t    size = 0;  // bytes
    std::string sha256;    // digest field minus the "sha256:" prefix
    bool        has_digest = false;  // digest present AND well-formed
};

struct ReleaseInfo {
    std::string tag;       // tag_name verbatim (e.g. "v0.1.6")
    Semver      version;   // parsed tag (always valid for kept releases)
    bool        draft = false;
    bool        prerelease = false;
    std::vector<AssetInfo> assets;
};

// Parse the GitHub releases-array JSON. Keeps ONLY releases that are
// non-draft, non-prerelease, AND carry a semver-parseable tag_name -- the
// rest are invisible to "latest" and to explicit-version lookups. Returns
// false when the text is not a JSON array (malformed feed).
bool parseReleasesJson(const std::string& jsonText,
                       std::vector<ReleaseInfo>& out);

// Max semver over the (already filtered) releases; nullptr when empty.
const ReleaseInfo* selectLatest(const std::vector<ReleaseInfo>& releases);

// Find a release whose tag parses to the same version as `versionArg`
// (accepts the v-prefix on either side). nullptr when absent.
const ReleaseInfo* findRelease(const std::vector<ReleaseInfo>& releases,
                               const std::string& versionArg);

// The install-bundle asset `tsdev_automation_<tag>.zip` on a release.
const AssetInfo* pickBundleAsset(const ReleaseInfo& release);

// Min/max published semver (releases must be non-empty). Used for the
// "Published releases run from vX to vY" not-found clause.
void publishedSemverRange(const std::vector<ReleaseInfo>& releases,
                          Semver& lo, Semver& hi);

// --- HTTP layer (WinHTTP) --------------------------------------------------

enum class HttpError {
    None,
    Unreachable,   // DNS/connect/send/receive/read failure
    HttpStatus,    // non-2xx status (status in httpStatus)
    RateLimited,   // 403 + X-RateLimit-Remaining: 0 (retryAfterSecs set)
    TooLarge,      // body exceeded the cap
    Io,            // local file write failure (to-file variant)
};

struct HttpResult {
    HttpError error = HttpError::None;
    int       httpStatus = 0;      // status when a response was received
    long      retryAfterSecs = -1; // rate-limit retry hint (>=0 when set)
    std::string body;              // response bytes (to-string variant)
};

// GET `url` into a string, capped at capBytes (>cap -> TooLarge).
// `userAgent` is required (GitHub rejects UA-less requests).
HttpResult httpGetToString(const std::string& url,
                           const std::string& userAgent,
                           size_t capBytes);

// GET `url` streamed to `outPath` (trunc/create). No byte cap. On failure
// the partial file is removed.
HttpResult httpGetToFile(const std::string& url,
                         const std::string& userAgent,
                         const std::wstring& outPath);

// --- Feed fetch ------------------------------------------------------------

enum class FeedError {
    None,
    Unreachable,    // service could not be contacted at all
    HttpStatus,     // non-2xx response (status in httpStatus)
    RateLimited,    // 403 + X-RateLimit-Remaining: 0
    MalformedJson,  // response was not a parseable releases array
};

struct FeedResult {
    FeedError error = FeedError::None;
    int       httpStatus = 0;
    long      retryAfterSecs = -1;
    std::vector<ReleaseInfo> releases;
};

// GET <apiBase>/releases?per_page=100 (first page only; apiBase may carry a
// trailing '/'). apiBase = full API base replacement (--update-url /
// TSDEV_UPDATE_URL contract; the GitHub-shape JSON is still expected).
FeedResult fetchReleases(const std::string& apiBase,
                         const std::string& userAgent);

// --- SHA-256 (BCrypt/CNG) ---------------------------------------------------

// Lowercase-hex SHA-256 of the file at `path`. False on any failure.
bool sha256FileHex(const std::wstring& path, std::string& outHex);

// Lowercase-hex SHA-256 of a byte buffer (used by the apply module for the
// install-dir mutex name).
std::string sha256BytesHex(const void* data, size_t len);

} // namespace update
} // namespace tsdev
