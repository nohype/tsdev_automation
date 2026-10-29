// update_feed.cpp -- self-update release feed (see update_feed.h).
//
// WinHTTP with WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (WPAD + system proxy,
// zero-config for corporate users), 30/30/60 s connect/send/receive
// timeouts, default redirect following (the asset browser_download_url
// redirects to GitHub's CDN), and a 4 MB cap on the API response.

#include "update_feed.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <vector>

namespace tsdev {
namespace update {

// --- Semver --------------------------------------------------------------

Semver parseSemver(const std::string& s) {
    Semver v;
    size_t i = 0;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) i = 1;
    int comps[3] = {0, 0, 0};
    for (int c = 0; c < 3; ++c) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            return v;  // empty component -> invalid
        }
        long long val = 0;
        while (i < s.size() &&
               std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + (s[i] - '0');
            if (val > 99999) return v;  // component bound
            ++i;
        }
        comps[c] = static_cast<int>(val);
        if (c < 2) {
            if (i >= s.size() || s[i] != '.') return v;
            ++i;
        }
    }
    if (i != s.size()) return v;  // trailing junk -> invalid
    v.major = comps[0];
    v.minor = comps[1];
    v.patch = comps[2];
    v.valid = true;
    return v;
}

int compareSemver(const Semver& a, const Semver& b) {
    if (a.valid != b.valid) return a.valid ? 1 : -1;
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// --- Feed parse --------------------------------------------------------------

bool parseReleasesJson(const std::string& jsonText,
                       std::vector<ReleaseInfo>& out) {
    out.clear();
    nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
    if (!j.is_array()) return false;
    for (const auto& rel : j) {
        if (!rel.is_object()) return false;
        // Wrong-typed fields (e.g. "tag_name": 123, "size": "abc") make
        // rel.value()/a.value() throw nlohmann::json::type_error. An
        // untrusted feed must never crash the process, so any type error
        // fails the whole parse -> FeedError::MalformedJson -> exit 25.
        try {
            const bool draft = rel.value("draft", false);
            const bool prerelease = rel.value("prerelease", false);
            if (draft || prerelease) continue;
            const std::string tag = rel.value("tag_name", "");
            const Semver ver = parseSemver(tag);
            if (!ver.valid) continue;  // non-semver tags are invisible
            ReleaseInfo info;
            info.tag = tag;
            info.version = ver;
            auto ait = rel.find("assets");
            if (ait != rel.end() && ait->is_array()) {
                for (const auto& a : *ait) {
                    if (!a.is_object()) continue;
                    AssetInfo asset;
                    asset.name = a.value("name", "");
                    asset.url  = a.value("browser_download_url", "");
                    asset.size = a.value("size", (uint64_t)0);
                    auto dit = a.find("digest");
                    if (dit != a.end() && dit->is_string()) {
                        const std::string d = dit->get<std::string>();
                        const std::string prefix = "sha256:";
                        if (d.rfind(prefix, 0) == 0 &&
                            d.size() == prefix.size() + 64) {
                            asset.sha256 = d.substr(prefix.size());
                            asset.has_digest = true;
                        }
                    }
                    info.assets.push_back(std::move(asset));
                }
            }
            out.push_back(std::move(info));
        } catch (const nlohmann::json::exception&) {
            return false;
        }
    }
    return true;
}

const ReleaseInfo* selectLatest(const std::vector<ReleaseInfo>& releases) {
    const ReleaseInfo* best = nullptr;
    for (const ReleaseInfo& r : releases) {
        if (!best || compareSemver(r.version, best->version) > 0) best = &r;
    }
    return best;
}

const ReleaseInfo* findRelease(const std::vector<ReleaseInfo>& releases,
                               const std::string& versionArg) {
    const Semver want = parseSemver(versionArg);
    if (!want.valid) return nullptr;
    for (const ReleaseInfo& r : releases) {
        if (compareSemver(r.version, want) == 0) return &r;
    }
    return nullptr;
}

const AssetInfo* pickBundleAsset(const ReleaseInfo& release) {
    const std::string want = "tsdev_automation_" + release.tag + ".zip";
    for (const AssetInfo& a : release.assets) {
        if (a.name == want) return &a;
    }
    return nullptr;
}

void publishedSemverRange(const std::vector<ReleaseInfo>& releases,
                          Semver& lo, Semver& hi) {
    if (releases.empty()) return;
    lo = releases.front().version;
    hi = lo;
    for (const ReleaseInfo& r : releases) {
        if (compareSemver(r.version, lo) < 0) lo = r.version;
        if (compareSemver(r.version, hi) > 0) hi = r.version;
    }
}

// --- HTTP layer --------------------------------------------------------------

namespace {

struct SessionGuard {
    HINTERNET h = nullptr;
    ~SessionGuard() { if (h) WinHttpCloseHandle(h); }
};

// Narrow -> wide for WinHttpCrackUrl input.
std::wstring toWide(const std::string& s) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()),
                                        nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], len);
    return out;
}

// Query a single response header by name ("" when absent).
std::wstring queryHeader(HINTERNET hRequest, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, name, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::wstring();
    }
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, name, &buf[0],
                             &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::wstring();
    }
    buf.resize(size / sizeof(wchar_t));
    while (!buf.empty() && (buf.back() == L'\0' || buf.back() == L'\r' ||
                          buf.back() == L'\n' || buf.back() == L' ')) {
        buf.pop_back();
    }
    return buf;
}

long parseLongHeader(const std::wstring& s) {
    if (s.empty()) return -1;
    wchar_t* end = nullptr;
    const long v = std::wcstol(s.c_str(), &end, 10);
    return (end != s.c_str()) ? v : -1;
}

// Shared GET: when outPath is non-null the body streams to the file;
// otherwise it accumulates into result.body capped at capBytes.
HttpResult httpGet(const std::string& url, const std::string& userAgent,
                   const std::wstring* outPath, size_t capBytes) {
    HttpResult res;
    const std::wstring wurl = toWide(url);
    const std::wstring wua  = toWide(userAgent);

    SessionGuard session;
    session.h = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { res.error = HttpError::Unreachable; return res; }
    // resolve / connect / send / receive timeouts (ms).
    WinHttpSetTimeouts(session.h, 30000, 30000, 30000, 60000);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.error = HttpError::Unreachable;
        return res;
    }

    SessionGuard connect;
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h) { res.error = HttpError::Unreachable; return res; }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    SessionGuard request;
    request.h = WinHttpOpenRequest(
        connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) { res.error = HttpError::Unreachable; return res; }

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        res.error = HttpError::Unreachable;
        return res;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        res.httpStatus = static_cast<int>(status);
    }

    if (status == 403) {
        const std::wstring remaining =
            queryHeader(request.h, L"X-RateLimit-Remaining");
        if (remaining == L"0") {
            res.error = HttpError::RateLimited;
            long reset = parseLongHeader(
                queryHeader(request.h, L"X-RateLimit-Reset"));
            if (reset >= 0) {
                const long now =
                    static_cast<long>(std::time(nullptr));
                res.retryAfterSecs = (reset > now) ? (reset - now) : 0;
            }
            if (res.retryAfterSecs < 0) {
                res.retryAfterSecs = parseLongHeader(
                    queryHeader(request.h, L"Retry-After"));
            }
            return res;
        }
    }
    if (status < 200 || status >= 300) {
        res.error = HttpError::HttpStatus;
        return res;
    }

    // Body: to file or to the capped string.
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (outPath) {
        hFile = CreateFileW(outPath->c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            res.error = HttpError::Io;
            return res;
        }
    }
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fg{hFile};

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            res.error = HttpError::Unreachable;
            break;
        }
        if (avail == 0) break;  // end of body
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buf.data(), avail, &read)) {
            res.error = HttpError::Unreachable;
            break;
        }
        if (read == 0) continue;
        if (outPath) {
            DWORD written = 0;
            if (!WriteFile(hFile, buf.data(), read, &written, nullptr) ||
                written != read) {
                res.error = HttpError::Io;
                break;
            }
        } else {
            if (res.body.size() + read > capBytes) {
                res.error = HttpError::TooLarge;
                break;
            }
            res.body.append(buf.data(), read);
        }
    }

    if (outPath && res.error != HttpError::None) {
        fg.h = INVALID_HANDLE_VALUE;
        CloseHandle(hFile);
        DeleteFileW(outPath->c_str());  // remove the partial download
    }
    return res;
}

} // namespace

HttpResult httpGetToString(const std::string& url,
                           const std::string& userAgent,
                           size_t capBytes) {
    return httpGet(url, userAgent, nullptr, capBytes);
}

HttpResult httpGetToFile(const std::string& url,
                         const std::string& userAgent,
                         const std::wstring& outPath) {
    return httpGet(url, userAgent, &outPath, 0);
}

// --- Feed fetch --------------------------------------------------------------

FeedResult fetchReleases(const std::string& apiBase,
                         const std::string& userAgent) {
    FeedResult fr;
    std::string base = apiBase;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string url = base + "/releases?per_page=100";

    const HttpResult http = httpGetToString(url, userAgent, 4 * 1024 * 1024);
    fr.httpStatus = http.httpStatus;
    fr.retryAfterSecs = http.retryAfterSecs;
    switch (http.error) {
        case HttpError::None:        break;
        case HttpError::Unreachable: fr.error = FeedError::Unreachable; return fr;
        case HttpError::RateLimited: fr.error = FeedError::RateLimited; return fr;
        case HttpError::TooLarge:
        case HttpError::HttpStatus:
        case HttpError::Io:          fr.error = FeedError::HttpStatus; return fr;
    }

    if (!parseReleasesJson(http.body, fr.releases)) {
        fr.error = FeedError::MalformedJson;
        fr.releases.clear();
        return fr;
    }
    return fr;
}

// --- SHA-256 -----------------------------------------------------------------

std::string sha256BytesHex(const void* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) {
        return std::string();
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashObjLen),
                          sizeof(DWORD), &cbResult, 0) != 0 ||
        hashObjLen == 0) {
        return std::string();
    }
    DWORD hashLen = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLen),
                          sizeof(DWORD), &cbResult, 0) != 0 ||
        hashLen == 0) {
        return std::string();
    }

    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                         nullptr, 0, 0) != 0) {
        return std::string();
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};

    if (len > 0 &&
        BCryptHashData(hHash,
                       reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(len), 0) != 0) {
        return std::string();
    }
    std::vector<uint8_t> hash(hashLen);
    if (BCryptFinishHash(hHash, hash.data(), hashLen, 0) != 0) {
        return std::string();
    }

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(hashLen * 2);
    for (BYTE b : hash) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

bool sha256FileHex(const std::wstring& path, std::string& outHex) {
    outHex.clear();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fg{hFile};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) {
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } ag{hAlg};

    DWORD hashObjLen = 0, cbResult = 0, hashLen = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashObjLen),
                          sizeof(DWORD), &cbResult, 0) != 0 ||
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLen),
                          sizeof(DWORD), &cbResult, 0) != 0 ||
        hashObjLen == 0 || hashLen == 0) {
        return false;
    }
    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                         nullptr, 0, 0) != 0) {
        return false;
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hg{hHash};

    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()),
                      &n, nullptr)) {
            return false;
        }
        if (n == 0) break;
        if (BCryptHashData(hHash, buf.data(), n, 0) != 0) return false;
    }
    std::vector<uint8_t> hash(hashLen);
    if (BCryptFinishHash(hHash, hash.data(), hashLen, 0) != 0) return false;

    static const char* hex = "0123456789abcdef";
    outHex.reserve(hashLen * 2);
    for (BYTE b : hash) {
        outHex.push_back(hex[b >> 4]);
        outHex.push_back(hex[b & 0x0F]);
    }
    return true;
}

} // namespace update
} // namespace tsdev
