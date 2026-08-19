// orlib_resolver.cpp
//
// TVS-B.4 — Full implementation of OrlibResolver.
//
// This file provides the complete implementation of EVERY method declared in
// orlib_resolver.h: PE parsing, signature scanning, version detection (3-tier),
// export resolution, patch-site resolution/validation/application, convenience
// resolvers, and data-section RVA resolution. All 45 methods are implemented
// (Cycles 1-5).
//
// The version manifest (kOrlibVersionManifest[]) is constexpr data copied
// verbatim from docs/VERSION_DETECTION.md §6.2 — it is safe to include now
// because it is pure data, not logic.

// orlib_resolver.h is included via signatures.gen.h (below), which defines the
// SignatureSetId enum BEFORE including orlib_resolver.h. This ordering is
// required because orlib_resolver.h forward-declares SignatureSetId and uses
// it as a by-value struct member in OrlibVersionManifestEntry.

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
#include <bcrypt.h>

// Link version.lib for GetFileVersionInfoW / VerQueryValueW (Cycle 4).
#pragma comment(lib, "version.lib")

// signatures.gen.h is the build-time-generated header (from committed JSON via
// tools/sigcodegen.py). It defines SignatureDef / kSignatures[] (the
// signatures/v9.50.01.3344.json catalog), the SignatureSetId enum, and the
// version manifest arrays. It includes orlib_resolver.h after defining the
// enum so that OrlibVersionManifestEntry (which has a SignatureSetId member
// by value) can use it. This replaces the old tmp/re_artifacts/b1_signatures.h
// dependency.
#include "signatures.gen.h"

namespace tsdev {

// ===========================================================================
// Anonymous-namespace helpers: hex parsing + signature catalog construction
// ===========================================================================
namespace {

/// \brief Convert a hex character to its numeric value (0-15), or -1 if invalid.
int hexCharVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// \brief Parse a space-separated hex pattern string into parallel byte + mask
///        vectors.
///
/// "??" tokens → mask byte 0x00 (wildcard), bytes byte 0x00.
/// "XX" tokens → mask byte 0xFF (fixed), bytes byte = parsed hex value.
void parseHexPatternWithMask(const char* hex,
                             std::vector<uint8_t>& bytes,
                             std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    if (!hex) return;

    const char* s = hex;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        char tok[3] = {0};
        tok[0] = *s++;
        if (*s && *s != ' ' && *s != '\t') {
            tok[1] = *s++;
        }

        if (tok[0] == '?' && tok[1] == '?') {
            bytes.push_back(0x00);
            mask.push_back(0x00);
        } else {
            int hi = hexCharVal(tok[0]);
            int lo = hexCharVal(tok[1]);
            if (hi < 0 || lo < 0) return;  // parse error → partial result
            bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            mask.push_back(0xFF);
        }
    }
}

/// \brief Parse a hex byte string (no wildcards) into a byte vector.
/// Returns an empty vector if hex is null.
std::vector<uint8_t> parseHexBytes(const char* hex) {
    std::vector<uint8_t> result;
    if (!hex) return result;
    std::vector<uint8_t> bytes, mask;
    parseHexPatternWithMask(hex, bytes, mask);
    result = bytes;  // no wildcards expected; bytes are correct
    return result;
}

/// \brief Convert a wide string to a narrow (UTF-8 / ANSI) string for
///        diagnostic messages.
std::string wideToNarrow(const wchar_t* wstr) {
    if (!wstr) return "";
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    // WideCharToMultiByte includes the null terminator in len; shrink it.
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

/// \brief Backing storage for one signature's byte arrays (bytes, mask,
///        originalBytes, originalBytesMask, replacementBytes). Stored in a
///        static vector so the raw const uint8_t* pointers in
///        OrlibSignatureDef remain valid for the program lifetime.
struct SigBackingData {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> originalBytesMask;
    std::vector<uint8_t> replacementBytes;
};

/// \brief Build the OrlibSignatureDef catalog from kSignatures[] (13 entries).
///
/// The backing byte data is stored in function-local static vectors so the
/// pointers in the returned OrlibSignatureDef objects remain valid
/// indefinitely. Thread-safe via C++ static-local initialization guarantee.
const std::vector<OrlibSignatureDef>& buildSignatureCatalog() {
    static std::vector<SigBackingData> backing;
    static std::vector<OrlibSignatureDef> catalog;

    if (!catalog.empty()) return catalog;  // already built (cached)

    backing.resize(static_cast<size_t>(kSignatureCount));
    catalog.resize(static_cast<size_t>(kSignatureCount));

    for (int i = 0; i < kSignatureCount; i++) {
        const SignatureDef& src = kSignatures[i];
        SigBackingData& bd = backing[static_cast<size_t>(i)];

        parseHexPatternWithMask(src.hexPattern, bd.bytes, bd.mask);
        bd.originalBytes       = parseHexBytes(src.originalBytes);
        bd.originalBytesMask   = parseHexBytes(src.originalBytesMask);
        bd.replacementBytes    = parseHexBytes(src.replacementBytes);

        OrlibSignatureDef& dst = catalog[static_cast<size_t>(i)];
        dst.name                = src.name;
        dst.expectedRva         = src.rva;
        dst.bytes               = bd.bytes.data();
        dst.mask                = bd.mask.data();
        dst.length              = static_cast<int>(bd.bytes.size());
        dst.expectedMatchCount  = src.expectedMatchCount;
        dst.isPatchSite         = src.isPatchSite;
        dst.originalBytes       = bd.originalBytes.data();
        dst.originalBytesLen    = static_cast<int>(bd.originalBytes.size());
        dst.originalBytesMask   = bd.originalBytesMask.empty() ? nullptr : bd.originalBytesMask.data();
        dst.originalBytesMaskLen = static_cast<int>(bd.originalBytesMask.size());
        dst.replacementBytes    = bd.replacementBytes.data();
        dst.replacementBytesLen = static_cast<int>(bd.replacementBytes.size());
        dst.patchOffset         = src.patchOffset;
        dst.containingFunction  = src.containingFunction;
    }

    return catalog;
}

/// \brief Convert an RVA to a raw file offset within an in-memory PE image
///        buffer (test mode only).
///
/// Iterates the section headers (DOS header → e_lfanew → NT headers →
/// IMAGE_FIRST_SECTION for NumberOfSections). For the section whose virtual
/// range contains the RVA, returns
///   PointerToRawData + (rva - VirtualAddress).
/// The virtual range bound is the larger of Misc.VirtualSize and
/// SizeOfRawData so that data spanning the raw-mapped tail (where
/// VirtualSize < SizeOfRawData) is still resolved. Returns 0 if no section
/// contains the RVA or the headers are invalid/truncated; callers must
/// bounds-check the result before dereferencing.
uint32_t rvaToFileOffset(const uint8_t* image, size_t imageSize, uint32_t rva) {
    if (!image || imageSize < sizeof(IMAGE_DOS_HEADER)) return 0;
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    DWORD peOff = dos->e_lfanew;
    if (peOff + sizeof(IMAGE_NT_HEADERS32) > imageSize) return 0;
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + peOff);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER& s = sections[i];
        DWORD va = s.VirtualAddress;
        DWORD vsize = s.Misc.VirtualSize;
        if (vsize < s.SizeOfRawData) vsize = s.SizeOfRawData;
        if (rva >= va && rva < va + vsize) {
            return s.PointerToRawData + (rva - va);
        }
    }
    return 0;
}

/// \brief Resolve an RVA to a readable byte pointer in either mode.
///
/// TEST mode:       converts RVA → file offset via rvaToFileOffset, then
///                  returns m_image + fileOffset (bounds-checked against
///                  m_imageSize; returns nullptr if out of bounds).
/// PRODUCTION mode: returns (const uint8_t*)(m_base + rva) directly (the
///                  loaded module is mapped at its RVAs; the caller trusts
///                  the RVA is valid since it came from signature resolution).
///
/// \param needed  Number of bytes the caller intends to read starting at the
///                returned pointer. Used for bounds-checking in test mode.
/// \return Pointer to the bytes, or nullptr if out of bounds (test mode).
const uint8_t* rvaToPtr(bool testMode,
                        const uint8_t* image, size_t imageSize,
                        uintptr_t base, uint32_t rva, size_t needed) {
    if (testMode) {
        uint32_t off = rvaToFileOffset(image, imageSize, rva);
        if (off == 0 || static_cast<size_t>(off) + needed > imageSize) {
            return nullptr;
        }
        return image + off;
    }
    return reinterpret_cast<const uint8_t*>(base) + rva;
}

/// \brief Read a little-endian uint32_t from a byte pointer (no alignment
///        requirement — safe for x86 unaligned reads).
uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ---------------------------------------------------------------------------
// Cycle 4 helpers: version resource extraction + SHA-256 hex formatting
// ---------------------------------------------------------------------------

/// \brief Extract the FileVersion STRING (not FIXEDFILEINFO) from a PE file's
///        version resource via GetFileVersionInfoW / VerQueryValueW.
///
/// Queries \\VarFileInfo\\Translation to obtain the language-codepage pair,
/// then queries \\StringFileInfo\\<lang><codepage>\\FileVersion to get the
/// wide string. Converts to narrow via wideToNarrow.
///
/// \param path Wide path to the PE file (on-disk or temp file).
/// \return Narrow FileVersion string (e.g. "9.50.01.3344").
/// \throws OrlibResolverError if any version-API call fails.
std::string extractFileVersionFromPath(const wchar_t* path) {
    DWORD handle = 0;
    DWORD verSize = GetFileVersionInfoSizeW(path, &handle);
    if (verSize == 0) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "GetFileVersionInfoSizeW returned 0");
    }
    std::vector<uint8_t> verData(verSize);
    if (!GetFileVersionInfoW(path, 0, verSize, verData.data())) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "GetFileVersionInfoW failed");
    }

    // Query the translation table (lang-codepage pairs).
    struct LangCodepage {
        WORD language;
        WORD codePage;
    } *translate = nullptr;
    UINT translateLen = 0;
    if (!VerQueryValueW(verData.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&translate), &translateLen)) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "VerQueryValueW Translation failed");
    }
    if (translate == nullptr || translateLen < sizeof(LangCodepage)) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "Translation table empty or too small");
    }

    // Build the StringFileInfo sub-block path:
    //   \StringFileInfo\<%04x%04x>\FileVersion
    wchar_t subBlock[64];
    swprintf_s(subBlock, 64, L"\\StringFileInfo\\%04x%04x\\FileVersion",
               translate[0].language, translate[0].codePage);

    wchar_t* fileVersion = nullptr;
    UINT fvLen = 0;
    if (!VerQueryValueW(verData.data(), subBlock,
                        reinterpret_cast<LPVOID*>(&fileVersion), &fvLen)) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "VerQueryValueW FileVersion failed");
    }
    if (fileVersion == nullptr || fvLen == 0) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "FileVersion string is empty");
    }

    return wideToNarrow(fileVersion);
}

/// \brief RAII guard that deletes a temp file on destruction.
struct TempFileGuard {
    const wchar_t* path;
    bool owns;
    TempFileGuard(const wchar_t* p) : path(p), owns(true) {}
    ~TempFileGuard() { if (owns && path) DeleteFileW(path); }
    void release() { owns = false; }
};

/// \brief Convert a raw byte array to an uppercase hex string.
/// \param data Pointer to the bytes.
/// \param len  Number of bytes.
/// \return Uppercase hex string (2 * len chars, no spaces).
std::string bytesToUpperHex(const uint8_t* data, size_t len) {
    static const char hexChars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(hexChars[(data[i] >> 4) & 0xF]);
        result.push_back(hexChars[data[i] & 0xF]);
    }
    return result;
}

/// \brief Case-insensitive ASCII string comparison (for SHA-256 hex).
bool iequalAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'f') ca -= 32;
        if (cb >= 'a' && cb <= 'f') cb -= 32;
        if (ca != cb) return false;
    }
    return true;
}

} // anonymous namespace

// ===========================================================================
// Construction / destruction
// ===========================================================================

OrlibResolver::OrlibResolver(const wchar_t* dllPath) {
    m_testMode   = false;
    m_module     = nullptr;
    m_base       = 0;
    m_image      = nullptr;
    m_imageSize  = 0;
    m_version    = VersionDescriptor{};

    if (!dllPath) {
        throw OrlibResolverError("LoadLibraryW failed", "(null path)",
                                 "dllPath is null");
    }

    m_module = LoadLibraryW(dllPath);
    if (!m_module) {
        DWORD err = GetLastError();
        throw OrlibResolverError("LoadLibraryW failed",
                                 wideToNarrow(dllPath),
                                 "GetLastError=" + std::to_string(err));
    }

    m_base = reinterpret_cast<uintptr_t>(m_module);

    parsePeHeaders();
    m_signatures = buildSignatureCatalog();

    // detectVersion() is called at construction (caches in m_version).
    // Wrap in try/catch — construction must never throw due to version
    // detection failure (detectVersion itself never throws, but this is
    // belt-and-suspenders). On exception, m_version stays default unknown.
    try {
        detectVersion();
    } catch (...) {
        m_version = VersionDescriptor{};
    }
}

OrlibResolver::OrlibResolver(const uint8_t* image, size_t size, uintptr_t base) {
    m_testMode   = true;
    m_image      = image;
    m_imageSize  = size;
    m_base       = base;
    m_module     = nullptr;
    m_version    = VersionDescriptor{};

    parsePeHeaders();
    m_signatures = buildSignatureCatalog();

    // detectVersion() is called at construction (caches in m_version).
    // Wrap in try/catch — construction must never throw due to version
    // detection failure (detectVersion itself never throws, but this is
    // belt-and-suspenders). On exception, m_version stays default unknown.
    try {
        detectVersion();
    } catch (...) {
        m_version = VersionDescriptor{};
    }
}

OrlibResolver::~OrlibResolver() {
    // No FreeLibrary in production; nothing to release in test mode.
}

// ===========================================================================
// Version detection
// ===========================================================================

VersionDescriptor OrlibResolver::detectVersion() {
    // detectVersion() NEVER throws. All internal errors are caught and
    // returned as a VersionDescriptor with known=false, tier=Unknown, and
    // a warningMessage describing the error.
    try {
        // --- Tier 1: FileVersion lookup in the manifest ---
        std::string fv = readFileVersion();

        const OrlibVersionManifestEntry* entry = nullptr;
        for (size_t i = 0; i < kOrlibVersionManifestCount; i++) {
            if (fv == kOrlibVersionManifest[i].fileVersion) {
                entry = &kOrlibVersionManifest[i];
                break;
            }
        }

        if (!entry) {
            // FileVersion not in manifest → unknown version.
            VersionDescriptor vd;
            vd.known          = false;
            vd.tier           = VersionTier::Unknown;
            vd.fileVersion    = fv;
            vd.peTimestamp    = 0;
            vd.fileSize       = 0;
            vd.sha256         = "";
            vd.manifestEntry  = nullptr;
            vd.signatureSet   = SIGSET_NONE;
            vd.warningMessage = "UNTESTED orlib20.dll version '" + fv +
                                "' — signatures may not match; proceeding "
                                "at your own risk";
            m_version = vd;
            return m_version;
        }

        // --- Tier 2: PE timestamp + file size cross-check ---
        // Read the PE timestamp directly from the image (test: m_image,
        // production: m_base). The timestamp is at:
        //   e_lfanew + 4 (NT signature) + 4 (TimeDateStamp offset within
        //   IMAGE_FILE_HEADER: Machine=2 + NumberOfSections=2 = 4).
        const BYTE* basePtr = m_testMode
            ? reinterpret_cast<const BYTE*>(m_image)
            : reinterpret_cast<const BYTE*>(m_base);
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(basePtr);
        DWORD peOff = dos->e_lfanew;
        const IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(basePtr + peOff);
        uint32_t actualTs = nt->FileHeader.TimeDateStamp;

        // Read the file size.
        uint32_t actualSize = 0;
        if (m_testMode) {
            actualSize = static_cast<uint32_t>(m_imageSize);
        } else {
            wchar_t path[MAX_PATH];
            DWORD pathLen = GetModuleFileNameW(m_module, path, MAX_PATH);
            if (pathLen > 0 && pathLen < MAX_PATH) {
                HANDLE hFile = CreateFileW(path, GENERIC_READ,
                                           FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER fileSize;
                    if (GetFileSizeEx(hFile, &fileSize)) {
                        actualSize = static_cast<uint32_t>(
                            fileSize.QuadPart);
                    }
                    CloseHandle(hFile);
                }
            }
        }

        if (actualTs != entry->peTimestamp ||
            actualSize != entry->fileSize) {
            // Tier 2 mismatch: known but rebuilt.
            char warning[512];
            std::snprintf(warning, sizeof(warning),
                          "orlib20.dll Tier 2 mismatch: expected "
                          "timestamp=0x%08X size=%u, got timestamp=0x%08X "
                          "size=%u",
                          entry->peTimestamp, entry->fileSize,
                          actualTs, actualSize);
            VersionDescriptor vd;
            vd.known          = true;
            vd.tier           = VersionTier::Tier2Mismatch;
            vd.fileVersion    = fv;
            vd.peTimestamp    = actualTs;
            vd.fileSize       = actualSize;
            vd.sha256         = "";
            vd.manifestEntry  = entry;
            vd.signatureSet   = entry->signatureSet;
            vd.warningMessage = warning;
            m_version = vd;
            return m_version;
        }

        // --- Tier 3: SHA-256 cross-check ---
        std::string hash;
        try {
            hash = computeSha256();
        } catch (const OrlibResolverError& e) {
            // SHA-256 computation failed, but Tier 2 matched.
            VersionDescriptor vd;
            vd.known          = true;
            vd.tier           = VersionTier::Tier2Match;
            vd.fileVersion    = fv;
            vd.peTimestamp    = actualTs;
            vd.fileSize       = actualSize;
            vd.sha256         = "";
            vd.manifestEntry  = entry;
            vd.signatureSet   = entry->signatureSet;
            vd.warningMessage = std::string("SHA-256 computation failed: ") +
                                e.what();
            m_version = vd;
            return m_version;
        }

        // Case-insensitive comparison (both should be uppercase, but be safe).
        if (iequalAscii(hash, entry->sha256)) {
            // Tier 3 match: known & verified (byte-identical).
            VersionDescriptor vd;
            vd.known          = true;
            vd.tier           = VersionTier::Tier3Match;
            vd.fileVersion    = fv;
            vd.peTimestamp    = actualTs;
            vd.fileSize       = actualSize;
            vd.sha256         = hash;
            vd.manifestEntry  = entry;
            vd.signatureSet   = entry->signatureSet;
            vd.warningMessage = "";
            m_version = vd;
            return m_version;
        }

        // Tier 3 mismatch: known but byte-differs.
        std::string warnMsg =
            "orlib20.dll Tier 3 mismatch: SHA-256 differs from manifest "
            "(expected " + std::string(entry->sha256) +
            ", got " + hash + ")";
        VersionDescriptor vd;
        vd.known          = true;
        vd.tier           = VersionTier::Tier3Mismatch;
        vd.fileVersion    = fv;
        vd.peTimestamp    = actualTs;
        vd.fileSize       = actualSize;
        vd.sha256         = hash;
        vd.manifestEntry  = entry;
        vd.signatureSet   = entry->signatureSet;
        vd.warningMessage = warnMsg;
        m_version = vd;
        return m_version;

    } catch (const OrlibResolverError& e) {
        VersionDescriptor vd;
        vd.known          = false;
        vd.tier           = VersionTier::Unknown;
        vd.fileVersion    = "";
        vd.peTimestamp    = 0;
        vd.fileSize       = 0;
        vd.sha256         = "";
        vd.manifestEntry  = nullptr;
        vd.signatureSet   = SIGSET_NONE;
        vd.warningMessage = std::string("Version detection error: ") +
                            e.what();
        m_version = vd;
        return m_version;
    } catch (...) {
        VersionDescriptor vd;
        vd.known          = false;
        vd.tier           = VersionTier::Unknown;
        vd.fileVersion    = "";
        vd.peTimestamp    = 0;
        vd.fileSize       = 0;
        vd.sha256         = "";
        vd.manifestEntry  = nullptr;
        vd.signatureSet   = SIGSET_NONE;
        vd.warningMessage = "Version detection error: unknown exception";
        m_version = vd;
        return m_version;
    }
}

const VersionDescriptor& OrlibResolver::version() const {
    return m_version;
}

// ===========================================================================
// Generic signature resolution
// ===========================================================================

uintptr_t OrlibResolver::resolveBySignature(const std::string& name,
                                            const OrlibSignatureDef& signature) {
    std::vector<uintptr_t> matches = scanTextSection(signature);

    if (matches.empty()) {
        // AC6: the what() message (whatArg + ": target='" + target + "' " +
        // detail) must contain BOTH the signature name (in target) and the
        // match count "0" (in detail).
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "0 matches found in .text (scanned 0x%zX bytes, "
                      "signature length %d)",
                      m_textRawSize, signature.length);
        throw OrlibResolverError("signature no-match", name, detail);
    }

    if (matches.size() > 1) {
        // Multi-match: include the count and all match RVAs in the detail.
        std::string detail = std::to_string(matches.size()) +
                             " matches at RVAs:";
        for (size_t i = 0; i < matches.size(); i++) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " 0x%llX",
                          static_cast<unsigned long long>(matches[i]));
            detail += buf;
        }
        throw OrlibResolverError("signature multi-match", name, detail);
    }

    return matches[0];
}

void* OrlibResolver::resolveAddressBySignature(const std::string& name,
                                               const OrlibSignatureDef& signature) {
    return reinterpret_cast<void*>(
        baseAddress() + resolveBySignature(name, signature));
}

// ===========================================================================
// Signature catalog access
// ===========================================================================

size_t OrlibResolver::getSignatureCount() const {
    return m_signatures.size();
}

const OrlibSignatureDef& OrlibResolver::getSignatureByIndex(size_t index) const {
    if (index >= m_signatures.size()) {
        throw std::out_of_range("OrlibResolver::getSignatureByIndex: index " +
            std::to_string(index) + " out of range (count=" +
            std::to_string(m_signatures.size()) + ")");
    }
    return m_signatures[index];
}

// ===========================================================================
// Specific internal-function resolvers (8 CALL_TARGETs)
// ===========================================================================

StartCompilingProcessFn OrlibResolver::resolveStartCompilingProcess() {
    const OrlibSignatureDef& sig = findSignature("StartCompilingProcess");
    uintptr_t rva = resolveBySignature("StartCompilingProcess", sig);
    return reinterpret_cast<StartCompilingProcessFn>(baseAddress() + rva);
}

EndCompilingProcessFn OrlibResolver::resolveEndCompilingProcess() {
    const OrlibSignatureDef& sig = findSignature("EndCompilingProcess");
    uintptr_t rva = resolveBySignature("EndCompilingProcess", sig);
    return reinterpret_cast<EndCompilingProcessFn>(baseAddress() + rva);
}

CELCompilerCtorFn OrlibResolver::resolveCELCompilerCtor() {
    const OrlibSignatureDef& sig = findSignature("CELCompiler_ctor");
    uintptr_t rva = resolveBySignature("CELCompiler_ctor", sig);
    return reinterpret_cast<CELCompilerCtorFn>(baseAddress() + rva);
}

CELCompilerDtorFn OrlibResolver::resolveCELCompilerDtor() {
    const OrlibSignatureDef& sig = findSignature("CELCompiler_dtor");
    uintptr_t rva = resolveBySignature("CELCompiler_dtor", sig);
    return reinterpret_cast<CELCompilerDtorFn>(baseAddress() + rva);
}

InitGlobalLookupFn OrlibResolver::resolveInitGlobalLookup() {
    const OrlibSignatureDef& sig = findSignature("InitGlobalLookup");
    uintptr_t rva = resolveBySignature("InitGlobalLookup", sig);
    return reinterpret_cast<InitGlobalLookupFn>(baseAddress() + rva);
}

void* OrlibResolver::resolveDeleteFromTree() {
    const OrlibSignatureDef& sig = findSignature("DeleteFromTree");
    uintptr_t rva = resolveBySignature("DeleteFromTree", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

GetWarehouseFn OrlibResolver::resolveGetWarehouse() {
    const OrlibSignatureDef& sig = findSignature("GetWarehouse");
    uintptr_t rva = resolveBySignature("GetWarehouse", sig);
    return reinterpret_cast<GetWarehouseFn>(baseAddress() + rva);
}

void* OrlibResolver::resolveDeleteOrchestrator() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_101a8360_delete_orchestrator");
    uintptr_t rva =
        resolveBySignature("FUN_101a8360_delete_orchestrator", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

void* OrlibResolver::resolveBuildTypeArray() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_101d43a0_build_type_array");
    uintptr_t rva =
        resolveBySignature("FUN_101d43a0_build_type_array", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

void* OrlibResolver::resolveCelProgramWriterCtor() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_1000dd30_celprogwriter_ctor");
    uintptr_t rva =
        resolveBySignature("FUN_1000dd30_celprogwriter_ctor", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

void* OrlibResolver::resolveFindItemGetName() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_101a42d0_find_item_get_name");
    uintptr_t rva =
        resolveBySignature("FUN_101a42d0_find_item_get_name", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

void* OrlibResolver::resolveBstSearchWrapper() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_101d05e0_bst_search_wrapper");
    uintptr_t rva =
        resolveBySignature("FUN_101d05e0_bst_search_wrapper", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

void* OrlibResolver::resolveVerbatimNameGetter() {
    const OrlibSignatureDef& sig =
        findSignature("FUN_101c36d0_verbatim_name_getter");
    uintptr_t rva =
        resolveBySignature("FUN_101c36d0_verbatim_name_getter", sig);
    return reinterpret_cast<void*>(baseAddress() + rva);
}

// ===========================================================================
// Export resolution (8 exports + generic)
// ===========================================================================

OrlibInitFn OrlibResolver::resolveOrlibInit() {
    return reinterpret_cast<OrlibInitFn>(resolveExport("_OrlibInit@8"));
}

OrlibExitFn OrlibResolver::resolveOrlibExit() {
    return reinterpret_cast<OrlibExitFn>(resolveExport("_OrlibExit@0"));
}

FindExistPosFn OrlibResolver::resolveFindExistPos() {
    // Exported UNDECORATED (no @N suffix), but still __stdcall.
    return reinterpret_cast<FindExistPosFn>(resolveExport("FindExistPos"));
}

CreateEntryEXFn OrlibResolver::resolveCreateEntryEX() {
    return reinterpret_cast<CreateEntryEXFn>(resolveExport("_CreateEntryEX@24"));
}

SaveModule_NewFn OrlibResolver::resolveSaveModuleNew() {
    return reinterpret_cast<SaveModule_NewFn>(resolveExport("_SaveModule_New@20"));
}

VerifyModule_NewFn OrlibResolver::resolveVerifyModuleNew() {
    return reinterpret_cast<VerifyModule_NewFn>(resolveExport("_VerifyModule_New@20"));
}

CloseWarehouseFn OrlibResolver::resolveCloseWarehouse() {
    return reinterpret_cast<CloseWarehouseFn>(
        resolveExport("_CloseWarehouseForCurrentThreadAndPath@4"));
}

FARPROC OrlibResolver::resolveExport(const char* decoratedName) {
    if (!decoratedName) {
        throw OrlibResolverError("null export name", "(null)", "");
    }

    if (m_testMode) {
        return resolveExportFromBuffer(decoratedName);
    }

    // Production mode: use GetProcAddress on the loaded module.
    FARPROC proc = GetProcAddress(m_module, decoratedName);
    if (!proc) {
        DWORD err = GetLastError();
        char detail[64];
        std::snprintf(detail, sizeof(detail), "GetLastError=%lu",
                      static_cast<unsigned long>(err));
        throw OrlibResolverError("export not found", decoratedName, detail);
    }
    return proc;
}

// ===========================================================================
// Patch-site resolution & validation (5 PATCH_SITEs)
// ===========================================================================

PatchSiteDescriptor OrlibResolver::resolvePatchSite(const std::string& name) {
    const OrlibSignatureDef& sig = findSignature(name);
    uintptr_t sigMatchRva = resolveBySignature(name, sig);
    uintptr_t patchRva =
        sigMatchRva + static_cast<uintptr_t>(sig.patchOffset);

    PatchSiteDescriptor desc;
    desc.name              = name;
    desc.patchRva          = patchRva;
    desc.sigMatchRva       = sigMatchRva;
    desc.patchOffset       = sig.patchOffset;
    desc.originalBytes     = std::vector<uint8_t>(
        sig.originalBytes, sig.originalBytes + sig.originalBytesLen);
    desc.replacementBytes  = std::vector<uint8_t>(
        sig.replacementBytes, sig.replacementBytes + sig.replacementBytesLen);
    desc.containingFunction =
        sig.containingFunction ? std::string(sig.containingFunction)
                               : std::string();
    return desc;
}

bool OrlibResolver::validatePatchSite(const std::string& name,
                                      uintptr_t rva,
                                      const std::vector<uint8_t>& expectedBytes,
                                      const std::vector<uint8_t>& mask) {
    const size_t len = expectedBytes.size();
    if (len == 0) return true;  // nothing to validate

    const uint8_t* ptr = rvaToPtr(m_testMode, m_image, m_imageSize, m_base,
                                  static_cast<uint32_t>(rva), len);
    if (!ptr) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "RVA 0x%llX is outside the image bounds",
                      static_cast<unsigned long long>(rva));
        throw OrlibResolverError("patch-site read out of bounds", name,
                                 detail);
    }

    for (size_t j = 0; j < len; j++) {
        // If mask is shorter than expectedBytes, treat missing entries as
        // 0xFF (must match).
        uint8_t maskByte = (j < mask.size()) ? mask[j] : 0xFF;
        if (maskByte == 0xFF) {
            if (ptr[j] != expectedBytes[j]) {
                return false;  // mismatch on a fixed byte
            }
        }
        // maskByte == 0x00 → wildcard, skip
    }
    return true;
}

bool OrlibResolver::validatePatchSite(const PatchSiteDescriptor& desc) {
    // Derive the validation mask from the patch-site name:
    //   - persistence patch: bytes 6-7 are the low 2 bytes of the SEH
    //     handler address (image-base-relative, ASLR-dependent) → wildcarded.
    //     This matches kPersistenceOriginalMask in tsdev_lib.cpp.
    //   - thread-check INT3 patches: single byte 0xCC, no wildcards.
    std::vector<uint8_t> mask;
    if (desc.name.find("persistence") != std::string::npos) {
        mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    } else {
        // Thread-check: full-FF mask (no wildcards).
        mask.assign(desc.originalBytes.size(), 0xFF);
    }
    return validatePatchSite(desc.name, desc.patchRva, desc.originalBytes,
                             mask);
}

bool OrlibResolver::applyPatch(const std::string& name) {
    PatchSiteDescriptor desc = resolvePatchSite(name);

    // Step 1: validate original bytes — throw on mismatch.
    if (!validatePatchSite(desc)) {
        const uint8_t* actualPtr = rvaToPtr(
            m_testMode, m_image, m_imageSize, m_base,
            static_cast<uint32_t>(desc.patchRva), desc.originalBytes.size());
        std::string expectedHex = bytesToUpperHex(
            desc.originalBytes.data(), desc.originalBytes.size());
        std::string actualHex;
        if (actualPtr) {
            actualHex = bytesToUpperHex(actualPtr, desc.originalBytes.size());
        } else {
            actualHex = "<unreadable>";
        }
        throw OrlibResolverError("patch-site validation failed", name,
                                 "expected: " + expectedHex +
                                 " actual: " + actualHex);
    }

    // Step 2: idempotency — if replacement bytes are already present, skip.
    const uint8_t* curPtr = rvaToPtr(
        m_testMode, m_image, m_imageSize, m_base,
        static_cast<uint32_t>(desc.patchRva), desc.replacementBytes.size());
    if (curPtr && std::memcmp(curPtr, desc.replacementBytes.data(),
                              desc.replacementBytes.size()) == 0) {
        return true;  // already patched
    }

    // Step 3: TEST mode — no write (buffer is read-only). Validation passed.
    if (m_testMode) {
        return true;
    }

    // Step 4: PRODUCTION mode — VirtualProtect + memcpy + restore.
    void* patchAddr = reinterpret_cast<void*>(m_base + desc.patchRva);
    DWORD oldProtect = 0;
    if (!VirtualProtect(patchAddr, desc.replacementBytes.size(),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD err = GetLastError();
        char detail[96];
        std::snprintf(detail, sizeof(detail),
                      "VirtualProtect failed GetLastError=%lu",
                      static_cast<unsigned long>(err));
        throw OrlibResolverError("applyPatch failed", name, detail);
    }
    std::memcpy(patchAddr, desc.replacementBytes.data(),
                desc.replacementBytes.size());
    DWORD dummy = 0;
    VirtualProtect(patchAddr, desc.replacementBytes.size(),
                   oldProtect, &dummy);
    return true;
}

// ===========================================================================
// Specific patch-site convenience methods (4)
// ===========================================================================

PatchSiteDescriptor OrlibResolver::resolvePersistencePatch() {
    return resolvePatchSite("FUN_10011230_persistence_patch");
}

PatchSiteDescriptor OrlibResolver::resolveThreadCheck1() {
    return resolvePatchSite("FUN_101d0950_thread_check_INT3");
}

PatchSiteDescriptor OrlibResolver::resolveThreadCheck2() {
    return resolvePatchSite("FUN_101cf5b0_thread_check_INT3");
}

PatchSiteDescriptor OrlibResolver::resolveThreadCheck3() {
    return resolvePatchSite("FUN_101d1fa0_thread_check_INT3");
}

// ===========================================================================
// Data-section RVA resolution (3 data globals)
// ===========================================================================

// ---------------------------------------------------------------------------
// Data-global extraction strategy (PM DECISION R5 — instruction-offset
// extraction):
//
//   The 3 .data-section globals are resolved by finding the referencing CODE
//   function via signature scan or export-name resolution, then extracting
//   the 4-byte absolute address operand from a known instruction within
//   that function. The absolute address is image-base-relative; subtracting
//   m_base yields the RVA.
//
//   Evidence: tmp/re_artifacts/b1_ghidra_evidence.md §Target 1.
//
//   StartCompilingProcess references two of the globals:
//     - offset 0x19: 8B 0D <imm32>  → MOV ECX,[<error array ptr>]
//       imm32 at function-offset 0x1B (opcode is 2 bytes: 8B 0D).
//     - offset 0x21: A3 <imm32>     → MOV [<error count>],EAX
//       imm32 at function-offset 0x22 (opcode is 1 byte: A3).
//
//   The third global (HINSTANCE for string table) is
//   referenced by _CreateEntryEX@24 (resolved export) at
//   instruction offset 0x6C: MOV ECX,[imm32] (opcode 8B 0D), imm32 at
//   function-offset 0x6E. Resolved by export name (not
//   signature) — see resolveErrorHinstRva below. The data-global RVAs
//   are recorded in signatures/v9.50.01.3344.json. Evidence:
//   tmp/re_artifacts/b4_5_hinst_xref.md.
// ---------------------------------------------------------------------------

uintptr_t OrlibResolver::resolveErrorArrayPtrRva() {
    // Resolve StartCompilingProcess (Target 1) by signature.
    const OrlibSignatureDef& sig = findSignature("StartCompilingProcess");
    uintptr_t funcRva = resolveBySignature("StartCompilingProcess", sig);

    // Instruction at funcRva + 0x19: MOV ECX,[imm32] → opcode 8B 0D (2 bytes),
    // followed by the 4-byte absolute address operand at funcRva + 0x1B.
    const uint32_t kOpcodeOffset = 0x19;
    const size_t   kReadLen      = 6;  // 2 opcode + 4 imm32

    const uint8_t* instr = rvaToPtr(m_testMode, m_image, m_imageSize, m_base,
                                    static_cast<uint32_t>(funcRva) + kOpcodeOffset,
                                    kReadLen);
    if (!instr) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "cannot read instruction at RVA 0x%llX+0x%X",
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorArrayPtrRva", detail);
    }

    // Verify opcode: 8B 0D (MOV ECX, [imm32]).
    if (instr[0] != 0x8B || instr[1] != 0x0D) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "unexpected opcode 0x%02X 0x%02X at RVA 0x%llX+0x%X "
                      "(expected 8B 0D)",
                      instr[0], instr[1],
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorArrayPtrRva", detail);
    }

    uint32_t absAddr = readLe32(instr + 2);  // imm32 at offset 2 within instr
    return static_cast<uintptr_t>(absAddr) - m_base;
}

uintptr_t OrlibResolver::resolveErrorCountRva() {
    // Resolve StartCompilingProcess (Target 1) by signature.
    const OrlibSignatureDef& sig = findSignature("StartCompilingProcess");
    uintptr_t funcRva = resolveBySignature("StartCompilingProcess", sig);

    // Instruction at funcRva + 0x21: MOV [imm32],EAX → opcode A3 (1 byte),
    // followed by the 4-byte absolute address operand at funcRva + 0x22.
    const uint32_t kOpcodeOffset = 0x21;
    const size_t   kReadLen      = 5;  // 1 opcode + 4 imm32

    const uint8_t* instr = rvaToPtr(m_testMode, m_image, m_imageSize, m_base,
                                    static_cast<uint32_t>(funcRva) + kOpcodeOffset,
                                    kReadLen);
    if (!instr) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "cannot read instruction at RVA 0x%llX+0x%X",
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorCountRva", detail);
    }

    // Verify opcode: A3 (MOV [imm32], EAX).
    if (instr[0] != 0xA3) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "unexpected opcode 0x%02X at RVA 0x%llX+0x%X "
                      "(expected A3)",
                      instr[0],
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorCountRva", detail);
    }

    uint32_t absAddr = readLe32(instr + 1);  // imm32 at offset 1 within instr
    return static_cast<uintptr_t>(absAddr) - m_base;
}

uintptr_t OrlibResolver::resolveErrorHinstRva() {
    // Resolve _CreateEntryEX@24 (export) by name, then extract the data
    // reference at offset 0x6C.
    FARPROC proc = resolveExport("_CreateEntryEX@24");
    uintptr_t funcRva = reinterpret_cast<uintptr_t>(proc) - m_base;

    // Instruction at funcRva + 0x6C: MOV ECX,[imm32] → opcode 8B 0D (2 bytes),
    // followed by the 4-byte absolute address operand at funcRva + 0x6E.
    const uint32_t kOpcodeOffset = 0x6C;
    const size_t   kReadLen      = 6;  // 2 opcode + 4 imm32

    const uint8_t* instr = rvaToPtr(m_testMode, m_image, m_imageSize, m_base,
                                    static_cast<uint32_t>(funcRva) + kOpcodeOffset,
                                    kReadLen);
    if (!instr) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "cannot read instruction at RVA 0x%llX+0x%X",
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorHinstRva", detail);
    }

    // Verify opcode: 8B 0D (MOV ECX, [imm32]).
    if (instr[0] != 0x8B || instr[1] != 0x0D) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "unexpected opcode 0x%02X 0x%02X at RVA 0x%llX+0x%X "
                      "(expected 8B 0D)",
                      instr[0], instr[1],
                      static_cast<unsigned long long>(funcRva), kOpcodeOffset);
        throw OrlibResolverError("data-global extraction failed",
                                 "resolveErrorHinstRva", detail);
    }

    uint32_t absAddr = readLe32(instr + 2);  // imm32 at offset 2 within instr
    return static_cast<uintptr_t>(absAddr) - m_base;
}

// ===========================================================================
// Accessors
// ===========================================================================

uintptr_t OrlibResolver::baseAddress() const {
    return m_base;
}

HMODULE OrlibResolver::moduleHandle() const {
    return m_module;
}

std::pair<uintptr_t, size_t> OrlibResolver::textSectionBounds() const {
    return {m_textRva, m_textRawSize};
}

bool OrlibResolver::isTestMode() const {
    return m_testMode;
}

SignatureSetId OrlibResolver::signatureSet() const {
    return m_version.signatureSet;
}

// ===========================================================================
// Protected virtuals (test seams)
// ===========================================================================

std::vector<uintptr_t> OrlibResolver::scanTextSection(const OrlibSignatureDef& sig) {
    std::vector<uintptr_t> matches;

    // Edge cases: invalid / impossible signatures yield no matches.
    if (sig.length <= 0) return matches;
    if (static_cast<size_t>(sig.length) > m_textRawSize) return matches;

    // Determine the .text bytes pointer.
    // - TEST mode:       .text bytes are at file offset m_textRawOffset into
    //                    the raw image buffer (m_image).
    // - PRODUCTION mode: .text bytes are at (m_base + m_textRva) in the
    //                    loaded module (the section is mapped at its RVA).
    const uint8_t* textBytes = nullptr;
    if (m_testMode) {
        textBytes = m_image + m_textRawOffset;
    } else {
        textBytes = reinterpret_cast<const uint8_t*>(m_base) + m_textRva;
    }

    const size_t scanSize = m_textRawSize;
    const size_t sigLen   = static_cast<size_t>(sig.length);

    // Scan every valid start offset. The last valid start is scanSize - sigLen
    // (inclusive), so the loop condition is (i + sigLen <= scanSize).
    for (size_t i = 0; i + sigLen <= scanSize; i++) {
        bool match = true;
        for (size_t j = 0; j < sigLen; j++) {
            // mask 0xFF = fixed byte (must match); 0x00 = wildcard (skip).
            if (sig.mask[j] == 0xFF) {
                if (textBytes[i + j] != sig.bytes[j]) {
                    match = false;
                    break;
                }
            }
        }
        if (match) {
            // Match RVA = .text RVA + offset within .text.
            matches.push_back(static_cast<uintptr_t>(m_textRva + i));
        }
    }

    return matches;
}

void OrlibResolver::parsePeHeaders() {
    // Determine the base pointer for PE parsing.
    // - Test mode:       m_image (raw file bytes; headers at file offsets).
    // - Production mode: m_base  (loaded module; headers at RVAs, but the
    //                    NT headers are at e_lfanew offset from the base in
    //                    both cases since the headers region is mapped 1:1).
    const BYTE* basePtr = m_testMode
        ? reinterpret_cast<const BYTE*>(m_image)
        : reinterpret_cast<const BYTE*>(m_base);

    if (!basePtr) {
        throw OrlibResolverError("invalid image pointer", "parsePeHeaders",
                                 m_testMode ? "test mode: null image buffer"
                                            : "production mode: null base");
    }

    // --- DOS header ---
    if (m_testMode && m_imageSize < sizeof(IMAGE_DOS_HEADER)) {
        throw OrlibResolverError("image too small for DOS header",
                                 "parsePeHeaders", "");
    }

    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(basePtr);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        throw OrlibResolverError("invalid DOS signature", "parsePeHeaders",
                                 "expected 0x5A4D");
    }

    // --- NT headers ---
    DWORD peOffset = dos->e_lfanew;
    if (m_testMode && peOffset + sizeof(IMAGE_NT_HEADERS32) > m_imageSize) {
        throw OrlibResolverError("PE headers beyond end of image buffer",
                                 "parsePeHeaders", "");
    }

    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(basePtr + peOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        throw OrlibResolverError("invalid PE signature", "parsePeHeaders",
                                 "expected 0x4550");
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        throw OrlibResolverError("not a PE32 image", "parsePeHeaders",
                                 "optional header magic is not 0x10B");
    }

    // --- Find the .text section ---
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    bool foundText = false;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        // Section name is 8 bytes, not necessarily null-terminated.
        if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
            m_textRva      = sections[i].VirtualAddress;
            m_textVsize    = sections[i].Misc.VirtualSize;
            m_textRawSize  = sections[i].SizeOfRawData;
            m_textRawOffset = sections[i].PointerToRawData;
            foundText = true;
            break;
        }
    }
    if (!foundText) {
        throw OrlibResolverError(".text section not found", "parsePeHeaders",
                                 "");
    }

    // --- Export directory (used in Cycle 3) ---
    m_exportDirRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    m_exportDirSize =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
}

std::string OrlibResolver::readFileVersion() {
    if (m_testMode) {
        // Test mode (PM DECISION R3 — TEMP-FILE approach):
        // Write m_image to a temp file, then use GetFileVersionInfoW on it.
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        DWORD pathLen = GetTempPathW(MAX_PATH, tempPath);
        if (pathLen == 0) {
            throw OrlibResolverError("readFileVersion failed", "",
                                     "GetTempPathW returned 0");
        }
        if (!GetTempFileNameW(tempPath, L"orl", 0, tempFile)) {
            throw OrlibResolverError("readFileVersion failed", "",
                                     "GetTempFileNameW failed");
        }

        // RAII guard ensures the temp file is deleted even on exception.
        TempFileGuard guard(tempFile);

        // Write the in-memory image buffer to the temp file.
        HANDLE hTemp = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTemp == INVALID_HANDLE_VALUE) {
            throw OrlibResolverError("readFileVersion failed", "",
                                     "CreateFileW(temp) failed");
        }
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(hTemp, m_image,
                            static_cast<DWORD>(m_imageSize),
                            &bytesWritten, nullptr);
        CloseHandle(hTemp);
        if (!ok || bytesWritten != static_cast<DWORD>(m_imageSize)) {
            throw OrlibResolverError("readFileVersion failed", "",
                                     "WriteFile(temp) failed");
        }

        // Extract the FileVersion string from the temp file.
        return extractFileVersionFromPath(tempFile);
        // guard's destructor deletes the temp file.
    }

    // Production mode: get the DLL path from the loaded module handle.
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(m_module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        throw OrlibResolverError("readFileVersion failed", "",
                                 "GetModuleFileNameW failed");
    }
    return extractFileVersionFromPath(path);
}

std::string OrlibResolver::computeSha256() {
    // --- BCrypt SHA-256 setup ---
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0);
    if (status != 0) {
        throw OrlibResolverError("computeSha256 failed", "",
                                 "BCryptOpenAlgorithmProvider failed");
    }
    // RAII guard for the algorithm provider.
    struct AlgGuard {
        BCRYPT_ALG_HANDLE& h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    // Query the hash object length and hash output length.
    DWORD hashObjLen = 0, cbResult = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashObjLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) {
        throw OrlibResolverError("computeSha256 failed", "",
                                 "BCryptGetProperty(OBJECT_LENGTH) failed");
    }
    DWORD hashLen = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLen),
                               sizeof(DWORD), &cbResult, 0);
    if (status != 0) {
        throw OrlibResolverError("computeSha256 failed", "",
                                 "BCryptGetProperty(HASH_LENGTH) failed");
    }

    // Create the hash object.
    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen,
                              nullptr, 0, 0);
    if (status != 0) {
        throw OrlibResolverError("computeSha256 failed", "",
                                 "BCryptCreateHash failed");
    }
    // RAII guard for the hash handle.
    struct HashGuard {
        BCRYPT_HASH_HANDLE& h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};

    // Feed data in 64 KB chunks to avoid huge single BCryptHashData calls.
    const size_t chunkSize = 65536;

    if (m_testMode) {
        // Test mode: hash the in-memory buffer (m_image, m_imageSize bytes).
        const uint8_t* data = m_image;
        size_t remaining = m_imageSize;
        size_t offset = 0;
        while (remaining > 0) {
            size_t toHash = (remaining < chunkSize) ? remaining : chunkSize;
            status = BCryptHashData(hHash,
                                    const_cast<PUCHAR>(data + offset),
                                    static_cast<ULONG>(toHash), 0);
            if (status != 0) {
                throw OrlibResolverError("computeSha256 failed", "",
                                         "BCryptHashData(buffer) failed");
            }
            offset += toHash;
            remaining -= toHash;
        }
    } else {
        // Production mode: hash the on-disk file (not the in-memory image).
        // The manifest SHA-256 is of the on-disk file (§8.5).
        wchar_t path[MAX_PATH];
        DWORD pathLen = GetModuleFileNameW(m_module, path, MAX_PATH);
        if (pathLen == 0 || pathLen >= MAX_PATH) {
            throw OrlibResolverError("computeSha256 failed", "",
                                     "GetModuleFileNameW failed");
        }
        HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            throw OrlibResolverError("computeSha256 failed", "",
                                     "CreateFileW failed");
        }
        struct FileGuard {
            HANDLE h;
            ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        } fileGuard{hFile};

        std::vector<uint8_t> chunk(chunkSize);
        for (;;) {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(hFile, chunk.data(),
                               static_cast<DWORD>(chunkSize),
                               &bytesRead, nullptr);
            if (!ok) {
                throw OrlibResolverError("computeSha256 failed", "",
                                         "ReadFile failed");
            }
            if (bytesRead == 0) break;  // EOF
            status = BCryptHashData(hHash, chunk.data(), bytesRead, 0);
            if (status != 0) {
                throw OrlibResolverError("computeSha256 failed", "",
                                         "BCryptHashData(file) failed");
            }
        }
    }

    // Finish the hash and convert to uppercase hex.
    std::vector<uint8_t> hash(hashLen);
    status = BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    if (status != 0) {
        throw OrlibResolverError("computeSha256 failed", "",
                                 "BCryptFinishHash failed");
    }

    return bytesToUpperHex(hash.data(), hash.size());
}

// ===========================================================================
// Private methods
// ===========================================================================

const OrlibSignatureDef& OrlibResolver::findSignature(const std::string& name) const {
    for (const auto& sig : m_signatures) {
        if (name == sig.name) {
            return sig;
        }
    }
    throw OrlibResolverError("signature not found in catalog", name, "");
}

FARPROC OrlibResolver::resolveExportFromBuffer(const char* decoratedName) {
    // TEST mode only: walk the PE export directory in the in-memory buffer.
    if (!m_testMode || !m_image) {
        throw OrlibResolverError("resolveExportFromBuffer requires test mode",
                                 decoratedName ? decoratedName : "(null)",
                                 "not in test mode or null image buffer");
    }
    if (!decoratedName) {
        throw OrlibResolverError("null export name", "(null)", "");
    }
    if (m_exportDirRva == 0) {
        throw OrlibResolverError("no export directory", decoratedName, "");
    }

    // --- Locate the export directory in the raw file buffer ---
    uint32_t expOff = rvaToFileOffset(m_image, m_imageSize,
                                      static_cast<uint32_t>(m_exportDirRva));
    if (expOff == 0 ||
        expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > m_imageSize) {
        throw OrlibResolverError("export directory out of bounds",
                                 decoratedName, "");
    }
    const IMAGE_EXPORT_DIRECTORY* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(m_image + expOff);

    const DWORD numNames      = exp->NumberOfNames;
    const DWORD numFuncs      = exp->NumberOfFunctions;
    const DWORD addrNames     = exp->AddressOfNames;
    const DWORD addrOrdinals  = exp->AddressOfNameOrdinals;
    const DWORD addrFunctions = exp->AddressOfFunctions;

    // --- Locate & bounds-check the three export tables ---
    uint32_t namesOff = rvaToFileOffset(m_image, m_imageSize, addrNames);
    if (addrNames == 0 || namesOff == 0 ||
        namesOff + static_cast<size_t>(sizeof(DWORD)) * numNames > m_imageSize) {
        throw OrlibResolverError("export name table out of bounds",
                                 decoratedName, "");
    }
    const DWORD* nameRvas =
        reinterpret_cast<const DWORD*>(m_image + namesOff);

    uint32_t ordOff = rvaToFileOffset(m_image, m_imageSize, addrOrdinals);
    if (addrOrdinals == 0 || ordOff == 0 ||
        ordOff + static_cast<size_t>(sizeof(WORD)) * numNames > m_imageSize) {
        throw OrlibResolverError("export ordinal table out of bounds",
                                 decoratedName, "");
    }
    const WORD* ordinals =
        reinterpret_cast<const WORD*>(m_image + ordOff);

    uint32_t funcOff = rvaToFileOffset(m_image, m_imageSize, addrFunctions);
    if (addrFunctions == 0 || funcOff == 0 ||
        funcOff + static_cast<size_t>(sizeof(DWORD)) * numFuncs > m_imageSize) {
        throw OrlibResolverError("export function table out of bounds",
                                 decoratedName, "");
    }
    const DWORD* funcRvas =
        reinterpret_cast<const DWORD*>(m_image + funcOff);

    // --- Walk the name pointer array, comparing each name with decoratedName ---
    const size_t dlen = std::strlen(decoratedName);
    for (DWORD i = 0; i < numNames; i++) {
        DWORD nameRva = nameRvas[i];
        uint32_t nameOff = rvaToFileOffset(m_image, m_imageSize, nameRva);
        if (nameOff == 0) {
            continue;  // unreadable name entry — skip
        }
        // Need dlen + 1 bytes (name + null terminator) within the buffer.
        if (nameOff + dlen + 1 > m_imageSize) {
            continue;  // name not fully contained or not null-terminated in buffer
        }
        const char* name =
            reinterpret_cast<const char*>(m_image + nameOff);
        // Exact, case-sensitive match including the null terminator.
        if (std::memcmp(name, decoratedName, dlen + 1) != 0) {
            continue;
        }

        // Match found at index i → look up the ordinal → function RVA.
        WORD ordinal = ordinals[i];
        if (ordinal >= numFuncs) {
            throw OrlibResolverError("export ordinal out of range",
                                     decoratedName, "");
        }
        DWORD funcRva = funcRvas[ordinal];
        // Return the absolute (simulated) address: base + RVA.
        return reinterpret_cast<FARPROC>(m_base + funcRva);
    }

    // No matching name found.
    throw OrlibResolverError("export not found", decoratedName, "");
}

} // namespace tsdev
