// sigtool.cpp
//
// Standalone developer tool for verifying orlib20.dll signatures.
//
// Subcommands:
//   sigtool verify --dll <path> [--signatures <json>] [--verbose]
//   sigtool verify-patch --dll <path> [--signatures <json>]
//   sigtool info --dll <path>
//   sigtool scan --dll <path> --pattern "55 8B EC ?? ??"
//   sigtool manifest --dll <path>
//
// Exit codes:
//   0 = all checks passed
//   1 = at least one check failed
//   2 = error (file not found, invalid PE, invalid JSON, usage error)
//
// Build (from project root):
//   cmake -S . -B build -A Win32
//   cmake --build build --config Release --target sigtool

#include <nlohmann/json.hpp>
#include "signatures.gen.h"  // for kSignatures, kSignatureCount, etc.
#include "orlib_resolver.h"  // for OrlibResolver

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;
using tsdev::OrlibResolver;
using tsdev::OrlibResolverError;
using tsdev::OrlibSignatureDef;
using tsdev::VersionDescriptor;
using tsdev::VersionTier;

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void PrintUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  sigtool verify --dll <path> [--signatures <json>] [--verbose]\n"
        "  sigtool verify-patch --dll <path> [--signatures <json>]\n"
        "  sigtool info --dll <path>\n"
        "  sigtool scan --dll <path> --pattern \"55 8B EC ?? ??\"\n"
        "  sigtool manifest --dll <path>\n");
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

/// \brief Read a file into a byte vector. Returns false on failure.
static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

/// \brief Parse a single hex character to its numeric value, or -1 on error.
static int HexCharVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// \brief Parse a space-separated hex pattern ("55 8B EC ?? ??") into parallel
///        byte + mask vectors. "??" → wildcard (mask 0x00). Returns false on
///        parse error.
static bool ParseHexPattern(const std::string& pattern,
                            std::vector<uint8_t>& bytes,
                            std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    const char* s = pattern.c_str();
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
            int hi = HexCharVal(tok[0]);
            int lo = HexCharVal(tok[1]);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            mask.push_back(0xFF);
        }
    }
    return !bytes.empty();
}

/// \brief Parse a hex byte string (no wildcards) into a byte vector.
static bool ParseHexBytes(const std::string& hex, std::vector<uint8_t>& out) {
    std::vector<uint8_t> mask;
    if (!ParseHexPattern(hex, out, mask)) return false;
    return true;
}

/// \brief Parse PE headers (DOS → NT → sections) to find the .text section.
///        Returns false on invalid PE.
static bool FindTextSection(const std::vector<uint8_t>& image,
                            uint32_t& textRva,
                            uint32_t& textRawOffset,
                            uint32_t& textRawSize) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;

    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    DWORD peOffset = dos->e_lfanew;
    if (peOffset + sizeof(IMAGE_NT_HEADERS32) > image.size()) return false;

    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + peOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {0};
        std::memcpy(name, sections[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            textRva       = sections[i].VirtualAddress;
            textRawOffset = sections[i].PointerToRawData;
            textRawSize   = sections[i].SizeOfRawData;
            return true;
        }
    }
    return false;
}

/// \brief Brute-force scan a byte buffer for all matches of the given pattern.
///        Appends match file offsets to the vector.
static void ScanPattern(const uint8_t* data, size_t dataLen,
                        const uint8_t* bytes, const uint8_t* mask,
                        int patternLen,
                        std::vector<uint32_t>& matchOffsets) {
    if (patternLen <= 0 ||
        static_cast<size_t>(patternLen) > dataLen) return;

    for (size_t i = 0; i + patternLen <= dataLen; i++) {
        bool match = true;
        for (int j = 0; j < patternLen; j++) {
            if (mask[j] == 0xFF && data[i + j] != bytes[j]) {
                match = false;
                break;
            }
        }
        if (match)
            matchOffsets.push_back(static_cast<uint32_t>(i));
    }
}

/// \brief Parse the PE export directory from an in-memory image and return
///        the function count and named count. Returns false if no export dir.
static bool ParseExportCounts(const std::vector<uint8_t>& image,
                              uint32_t& funcCount,
                              uint32_t& namedCount) {
    funcCount  = 0;
    namedCount = 0;

    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    DWORD peOffset = dos->e_lfanew;
    if (peOffset + sizeof(IMAGE_NT_HEADERS32) > image.size()) return false;
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + peOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    DWORD exportRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD exportSize =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (exportRva == 0 || exportSize == 0) return false;

    // Convert export RVA to file offset via section table.
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    DWORD exportOff = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD sRva  = sections[i].VirtualAddress;
        DWORD sVsz  = sections[i].Misc.VirtualSize;
        DWORD sRaw  = sections[i].PointerToRawData;
        DWORD sRawSz = sections[i].SizeOfRawData;
        if (exportRva >= sRva && exportRva < sRva + sVsz) {
            exportOff = sRaw + (exportRva - sRva);
            break;
        }
        // Use raw size as fallback bound.
        (void)sRawSz;
    }
    if (exportOff == 0 ||
        exportOff + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size())
        return false;

    const IMAGE_EXPORT_DIRECTORY* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.data() + exportOff);
    funcCount  = exp->NumberOfFunctions;
    namedCount = exp->NumberOfNames;
    return true;
}

// ---------------------------------------------------------------------------
// JSON signature parsing (for --signatures flag)
// ---------------------------------------------------------------------------

/// \brief Owns the byte/mask vectors so the pointers in OrlibSignatureDef
///        remain valid while the ParsedSig is alive.
struct ParsedSig {
    std::string name;
    std::string containingFunction;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> originalBytesMask;
    std::vector<uint8_t> replacementBytes;
    OrlibSignatureDef def;
};

/// \brief Parse a hex string value from JSON into a byte vector.
static bool JsonHexToBytes(const json& val, std::vector<uint8_t>& out) {
    if (!val.is_string()) return false;
    return ParseHexBytes(val.get<std::string>(), out);
}

/// \brief Parse a signature JSON file into a vector of ParsedSig.
///        Returns false on parse error.
static bool ParseSignaturesFromJson(const std::string& jsonPath,
                                    std::vector<ParsedSig>& out) {
    std::ifstream f(jsonPath);
    if (!f) {
        std::fprintf(stderr, "ERROR: cannot open signatures JSON: %s\n",
                     jsonPath.c_str());
        return false;
    }

    json root;
    try {
        f >> root;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: invalid JSON in %s: %s\n",
                     jsonPath.c_str(), e.what());
        return false;
    }

    if (!root.contains("signatures") || !root["signatures"].is_array()) {
        std::fprintf(stderr, "ERROR: JSON missing \"signatures\" array.\n");
        return false;
    }

    const json& sigs = root["signatures"];
    out.clear();

    for (const auto& entry : sigs) {
        ParsedSig ps;
        ps.def = OrlibSignatureDef{};

        if (!entry.contains("name") || !entry["name"].is_string()) {
            std::fprintf(stderr, "ERROR: signature entry missing \"name\".\n");
            return false;
        }
        ps.name = entry["name"].get<std::string>();
        ps.def.name = ps.name.c_str();

        // expectedRva: hex string (e.g. "0xRRRRRR"), sourced from
        // signatures/v9.50.01.3344.json (diagnostic only).
        if (entry.contains("expectedRva") && entry["expectedRva"].is_string()) {
            std::string rvaStr = entry["expectedRva"].get<std::string>();
            unsigned int rvaVal = 0;
            if (sscanf(rvaStr.c_str(), "%x", &rvaVal) == 1 ||
                sscanf(rvaStr.c_str(), "0x%x", &rvaVal) == 1) {
                ps.def.expectedRva = static_cast<int>(rvaVal);
            }
        }

        // hexPattern → bytes + mask
        if (!entry.contains("hexPattern") || !entry["hexPattern"].is_string()) {
            std::fprintf(stderr,
                "ERROR: signature \"%s\" missing \"hexPattern\".\n",
                ps.name.c_str());
            return false;
        }
        std::string hexPattern = entry["hexPattern"].get<std::string>();
        if (!ParseHexPattern(hexPattern, ps.bytes, ps.mask)) {
            std::fprintf(stderr,
                "ERROR: signature \"%s\" has invalid hexPattern.\n",
                ps.name.c_str());
            return false;
        }
        ps.def.bytes = ps.bytes.data();
        ps.def.mask  = ps.mask.data();
        ps.def.length = static_cast<int>(ps.bytes.size());

        // expectedMatchCount
        if (entry.contains("expectedMatchCount") &&
            entry["expectedMatchCount"].is_number_integer()) {
            ps.def.expectedMatchCount =
                entry["expectedMatchCount"].get<int>();
        } else {
            ps.def.expectedMatchCount = 1;
        }

        // isPatchSite
        if (entry.contains("isPatchSite") && entry["isPatchSite"].is_boolean()) {
            ps.def.isPatchSite = entry["isPatchSite"].get<bool>();
        } else if (entry.contains("targetType") &&
                   entry["targetType"].is_string()) {
            ps.def.isPatchSite =
                (entry["targetType"].get<std::string>() == "patch_site");
        } else {
            ps.def.isPatchSite = false;
        }

        // Patch-site fields (originalBytes, originalBytesMask,
        // replacementBytes, patchOffset, containingFunction)
        if (ps.def.isPatchSite) {
            if (entry.contains("patch") && entry["patch"].is_object()) {
                const json& patch = entry["patch"];
                if (patch.contains("offset") && patch["offset"].is_number_integer())
                    ps.def.patchOffset = patch["offset"].get<int>();
                if (patch.contains("originalBytes"))
                    JsonHexToBytes(patch["originalBytes"], ps.originalBytes);
                if (patch.contains("originalBytesMask"))
                    JsonHexToBytes(patch["originalBytesMask"], ps.originalBytesMask);
                if (patch.contains("replacementBytes"))
                    JsonHexToBytes(patch["replacementBytes"], ps.replacementBytes);
            } else {
                // Flat fields (no nested "patch" object)
                if (entry.contains("originalBytes"))
                    JsonHexToBytes(entry["originalBytes"], ps.originalBytes);
                if (entry.contains("originalBytesMask"))
                    JsonHexToBytes(entry["originalBytesMask"], ps.originalBytesMask);
                if (entry.contains("replacementBytes"))
                    JsonHexToBytes(entry["replacementBytes"], ps.replacementBytes);
                if (entry.contains("patchOffset") &&
                    entry["patchOffset"].is_number_integer())
                    ps.def.patchOffset = entry["patchOffset"].get<int>();
            }

            ps.def.originalBytes =
                ps.originalBytes.empty() ? nullptr : ps.originalBytes.data();
            ps.def.originalBytesLen =
                static_cast<int>(ps.originalBytes.size());
            ps.def.originalBytesMask =
                ps.originalBytesMask.empty() ? nullptr
                                             : ps.originalBytesMask.data();
            ps.def.originalBytesMaskLen =
                static_cast<int>(ps.originalBytesMask.size());
            ps.def.replacementBytes =
                ps.replacementBytes.empty() ? nullptr
                                            : ps.replacementBytes.data();
            ps.def.replacementBytesLen =
                static_cast<int>(ps.replacementBytes.size());

            if (entry.contains("containingFunction") &&
                entry["containingFunction"].is_string()) {
                ps.containingFunction =
                    entry["containingFunction"].get<std::string>();
                ps.def.containingFunction = ps.containingFunction.c_str();
            }
        }

        out.push_back(std::move(ps));
    }

    // After move, re-link pointers (the vectors may have moved). We need to
    // fix up the pointers to point at the moved-to storage.
    for (auto& ps : out) {
        ps.def.name = ps.name.c_str();
        ps.def.bytes = ps.bytes.data();
        ps.def.mask  = ps.mask.data();
        if (!ps.originalBytes.empty())
            ps.def.originalBytes = ps.originalBytes.data();
        if (!ps.originalBytesMask.empty())
            ps.def.originalBytesMask = ps.originalBytesMask.data();
        if (!ps.replacementBytes.empty())
            ps.def.replacementBytes = ps.replacementBytes.data();
        if (!ps.containingFunction.empty())
            ps.def.containingFunction = ps.containingFunction.c_str();
    }

    return true;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

/// \brief Construct an OrlibResolver from a DLL file read into memory.
///        Returns nullptr on error (message printed to stderr).
static std::unique_ptr<OrlibResolver> MakeResolver(
    const std::string& dllPath, std::vector<uint8_t>& image) {
    if (!ReadFile(dllPath, image)) {
        std::fprintf(stderr, "ERROR: cannot read DLL: %s\n", dllPath.c_str());
        return nullptr;
    }
    try {
        return std::make_unique<OrlibResolver>(image.data(), image.size(),
                                               0x10000000);
    } catch (const OrlibResolverError& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return nullptr;
    }
}

/// \brief sigtool verify — scan all signatures, report PASS/FAIL.
static int CmdVerify(const std::string& dllPath,
                     const std::string& signaturesPath,
                     bool verbose) {
    std::vector<uint8_t> image;
    auto resolver = MakeResolver(dllPath, image);
    if (!resolver) return 2;

    int passCount = 0;
    int failCount = 0;
    int totalCount = 0;

    if (signaturesPath.empty()) {
        // Default mode: use the committed signature set.
        totalCount = static_cast<int>(resolver->getSignatureCount());
        for (int i = 0; i < totalCount; i++) {
            const OrlibSignatureDef& sig =
                resolver->getSignatureByIndex(static_cast<size_t>(i));
            try {
                uintptr_t rva =
                    resolver->resolveBySignature(sig.name, sig);
                passCount++;
                if (verbose) {
                    std::printf("[PASS] %s: match RVA=0x%08X, "
                                "expected RVA=0x%08X, match count=1\n",
                                sig.name,
                                static_cast<unsigned int>(rva),
                                static_cast<unsigned int>(sig.expectedRva));
                }
            } catch (const OrlibResolverError& e) {
                failCount++;
                if (verbose) {
                    std::printf("[FAIL] %s: %s\n", sig.name, e.detail().c_str());
                }
            }
        }
    } else {
        // --signatures mode: parse JSON.
        std::vector<ParsedSig> parsedSigs;
        if (!ParseSignaturesFromJson(signaturesPath, parsedSigs))
            return 2;
        totalCount = static_cast<int>(parsedSigs.size());
        for (const auto& ps : parsedSigs) {
            try {
                uintptr_t rva =
                    resolver->resolveBySignature(ps.def.name, ps.def);
                passCount++;
                if (verbose) {
                    std::printf("[PASS] %s: match RVA=0x%08X, "
                                "expected RVA=0x%08X, match count=1\n",
                                ps.def.name,
                                static_cast<unsigned int>(rva),
                                static_cast<unsigned int>(ps.def.expectedRva));
                }
            } catch (const OrlibResolverError& e) {
                failCount++;
                if (verbose) {
                    std::printf("[FAIL] %s: %s\n",
                                ps.def.name, e.detail().c_str());
                }
            }
        }
    }

    if (verbose) {
        std::printf("\n%d/%d signatures PASS", passCount, totalCount);
        if (failCount > 0)
            std::printf(", %d FAIL", failCount);
        std::printf("\n");
    } else {
        std::printf("%d/%d signatures PASS", passCount, totalCount);
        if (failCount > 0)
            std::printf(", %d FAIL", failCount);
        std::printf("\n");
    }

    return (failCount == 0) ? 0 : 1;
}

/// \brief sigtool verify-patch — validate patch-site original bytes.
static int CmdVerifyPatch(const std::string& dllPath,
                          const std::string& signaturesPath) {
    std::vector<uint8_t> image;
    auto resolver = MakeResolver(dllPath, image);
    if (!resolver) return 2;

    int passCount = 0;
    int failCount = 0;
    int totalCount = 0;

    // Lambda to process a single signature def.
    auto processSig = [&](const OrlibSignatureDef& sig) {
        if (!sig.isPatchSite) return;

        totalCount++;

        try {
            uintptr_t matchRva =
                resolver->resolveBySignature(sig.name, sig);
            uintptr_t patchRva =
                matchRva + static_cast<uintptr_t>(sig.patchOffset);

            std::vector<uint8_t> expectedBytes(
                sig.originalBytes,
                sig.originalBytes + sig.originalBytesLen);
            std::vector<uint8_t> mask;
            if (sig.originalBytesMask && sig.originalBytesMaskLen > 0) {
                mask.assign(sig.originalBytesMask,
                            sig.originalBytesMask + sig.originalBytesMaskLen);
            }

            bool ok = resolver->validatePatchSite(sig.name, patchRva,
                                                  expectedBytes, mask);
            if (ok) {
                passCount++;
                std::printf("[PASS] %s: patch RVA=0x%08X, "
                            "original bytes match (%d bytes)\n",
                            sig.name,
                            static_cast<unsigned int>(patchRva),
                            sig.originalBytesLen);
            } else {
                failCount++;
                std::printf("[FAIL] %s: patch RVA=0x%08X, "
                            "original bytes MISMATCH\n",
                            sig.name,
                            static_cast<unsigned int>(patchRva));
            }
        } catch (const OrlibResolverError& e) {
            failCount++;
            std::printf("[FAIL] %s: %s\n", sig.name, e.detail().c_str());
        }
    };

    if (signaturesPath.empty()) {
        // Default mode: use the committed signature set.
        size_t count = resolver->getSignatureCount();
        for (size_t i = 0; i < count; i++) {
            processSig(resolver->getSignatureByIndex(i));
        }
    } else {
        // --signatures mode: parse JSON.
        std::vector<ParsedSig> parsedSigs;
        if (!ParseSignaturesFromJson(signaturesPath, parsedSigs))
            return 2;
        for (const auto& ps : parsedSigs) {
            processSig(ps.def);
        }
    }

    std::printf("\n%d/%d patch sites PASS", passCount, totalCount);
    if (failCount > 0)
        std::printf(", %d FAIL", failCount);
    std::printf("\n");

    return (failCount == 0) ? 0 : 1;
}

/// \brief sigtool info — print DLL version indicators.
static int CmdInfo(const std::string& dllPath) {
    std::vector<uint8_t> image;
    auto resolver = MakeResolver(dllPath, image);
    if (!resolver) return 2;

    VersionDescriptor vd = resolver->detectVersion();

    std::printf("FileVersion: %s\n", vd.fileVersion.c_str());
    std::printf("SHA-256: %s\n", vd.sha256.c_str());
    std::printf("PE timestamp: 0x%08X\n",
                static_cast<unsigned int>(vd.peTimestamp));
    std::printf("File size: %u bytes\n",
                static_cast<unsigned int>(vd.fileSize));

    // .text section info — parse PE headers directly from the buffer.
    uint32_t textRva = 0, textRawOff = 0, textRawSize = 0, textVsize = 0;
    if (FindTextSection(image, textRva, textRawOff, textRawSize)) {
        // Get virtual size from the section header.
        if (image.size() >= sizeof(IMAGE_DOS_HEADER)) {
            const IMAGE_DOS_HEADER* dos =
                reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
            DWORD peOffset = dos->e_lfanew;
            const IMAGE_NT_HEADERS32* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                    image.data() + peOffset);
            const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                char name[9] = {0};
                std::memcpy(name, sections[i].Name, 8);
                if (std::strcmp(name, ".text") == 0) {
                    textVsize = sections[i].Misc.VirtualSize;
                    break;
                }
            }
        }
        std::printf(".text section: RVA=0x%08X, vsize=0x%08X, "
                    "rawSize=0x%08X\n",
                    textRva, textVsize, textRawSize);
    } else {
        std::printf(".text section: not found\n");
    }

    // Export info — parse export directory directly.
    uint32_t funcCount = 0, namedCount = 0;
    if (ParseExportCounts(image, funcCount, namedCount)) {
        std::printf("Exports: %u functions, %u named\n",
                    funcCount, namedCount);
    } else {
        std::printf("Exports: export directory not found\n");
    }

    return 0;
}

/// \brief sigtool scan — scan an ad-hoc hex pattern, report all match RVAs.
static int CmdScan(const std::string& dllPath, const std::string& pattern) {
    std::vector<uint8_t> image;
    if (!ReadFile(dllPath, image)) {
        std::fprintf(stderr, "ERROR: cannot read DLL: %s\n", dllPath.c_str());
        return 2;
    }

    std::vector<uint8_t> bytes, mask;
    if (!ParseHexPattern(pattern, bytes, mask)) {
        std::fprintf(stderr, "ERROR: invalid hex pattern: %s\n",
                     pattern.c_str());
        return 2;
    }

    uint32_t textRva = 0, textRawOff = 0, textRawSize = 0;
    if (!FindTextSection(image, textRva, textRawOff, textRawSize)) {
        std::fprintf(stderr, "ERROR: invalid PE or .text section not found.\n");
        return 2;
    }

    if (textRawOff + textRawSize > image.size()) {
        std::fprintf(stderr, "ERROR: .text raw data extends beyond file.\n");
        return 2;
    }

    const uint8_t* textData = image.data() + textRawOff;
    std::vector<uint32_t> matchOffsets;
    ScanPattern(textData, textRawSize, bytes.data(), mask.data(),
                static_cast<int>(bytes.size()), matchOffsets);

    std::printf("Pattern: %s\n", pattern.c_str());
    std::printf("Matches: %zu\n", matchOffsets.size());

    for (size_t i = 0; i < matchOffsets.size(); i++) {
        uint32_t offInText = matchOffsets[i];
        uint32_t fileOff   = textRawOff + offInText;
        uint32_t rva       = textRva + offInText;
        std::printf("  match[%zu]: file offset=0x%08X, RVA=0x%08X\n",
                    i, fileOff, rva);
    }

    return (matchOffsets.size() >= 1) ? 0 : 1;
}

/// \brief sigtool manifest — look up FileVersion in the version manifest.
static int CmdManifest(const std::string& dllPath) {
    std::vector<uint8_t> image;
    auto resolver = MakeResolver(dllPath, image);
    if (!resolver) return 2;

    VersionDescriptor vd = resolver->detectVersion();

    std::printf("Version: %s\n", vd.fileVersion.c_str());
    std::printf("Known: %s\n", vd.known ? "yes" : "no");

    const char* tierStr = "Unknown";
    switch (vd.tier) {
        case VersionTier::Unknown:      tierStr = "Unknown"; break;
        case VersionTier::Tier1Match:   tierStr = "Tier1Match"; break;
        case VersionTier::Tier2Match:   tierStr = "Tier2Match"; break;
        case VersionTier::Tier2Mismatch: tierStr = "Tier2Mismatch"; break;
        case VersionTier::Tier3Match:   tierStr = "Tier3Match"; break;
        case VersionTier::Tier3Mismatch: tierStr = "Tier3Mismatch"; break;
    }
    std::printf("Tier: %s\n", tierStr);

    // Signature set ID as a number.
    std::printf("Signature set: %u\n",
                static_cast<unsigned int>(vd.signatureSet));

    if (!vd.warningMessage.empty()) {
        std::printf("Warning: %s\n", vd.warningMessage.c_str());
    }

    return vd.known ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Main / argument parsing
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    std::string subcommand = argv[1];
    std::string dllPath;
    std::string signaturesPath;
    std::string pattern;
    bool verbose = false;

    // Parse flags starting from argv[2].
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dll" && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (arg == "--signatures" && i + 1 < argc) {
            signaturesPath = argv[++i];
        } else if (arg == "--pattern" && i + 1 < argc) {
            pattern = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::fprintf(stderr, "ERROR: unknown or incomplete flag: %s\n",
                         arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    // Dispatch.
    if (subcommand == "verify") {
        if (dllPath.empty()) {
            std::fprintf(stderr, "ERROR: --dll is required for verify.\n");
            PrintUsage();
            return 2;
        }
        return CmdVerify(dllPath, signaturesPath, verbose);

    } else if (subcommand == "verify-patch") {
        if (dllPath.empty()) {
            std::fprintf(stderr,
                "ERROR: --dll is required for verify-patch.\n");
            PrintUsage();
            return 2;
        }
        return CmdVerifyPatch(dllPath, signaturesPath);

    } else if (subcommand == "info") {
        if (dllPath.empty()) {
            std::fprintf(stderr, "ERROR: --dll is required for info.\n");
            PrintUsage();
            return 2;
        }
        return CmdInfo(dllPath);

    } else if (subcommand == "scan") {
        if (dllPath.empty()) {
            std::fprintf(stderr, "ERROR: --dll is required for scan.\n");
            PrintUsage();
            return 2;
        }
        if (pattern.empty()) {
            std::fprintf(stderr, "ERROR: --pattern is required for scan.\n");
            PrintUsage();
            return 2;
        }
        return CmdScan(dllPath, pattern);

    } else if (subcommand == "manifest") {
        if (dllPath.empty()) {
            std::fprintf(stderr, "ERROR: --dll is required for manifest.\n");
            PrintUsage();
            return 2;
        }
        return CmdManifest(dllPath);

    } else {
        std::fprintf(stderr, "ERROR: unknown subcommand: %s\n",
                     subcommand.c_str());
        PrintUsage();
        return 2;
    }
}
