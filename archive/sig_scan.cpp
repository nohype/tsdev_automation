// sig_scan.cpp
//
// Standalone signature uniqueness verification probe for orlib20.dll.
//
// Reads the DLL file from disk (does NOT load it into memory), parses the PE
// headers to locate the .text section, then scans .text raw bytes for each
// candidate signature in b1_signatures.h. Reports match counts, file offsets,
// and RVAs. Verifies that each signature matches exactly once at the expected
// RVA (uniqueness check).
//
// Build (from project root):
//   cmake -S . -B build -A Win32
//   cmake --build build --config Release --target sig_scan
// Run:
//   build\Release\sig_scan.exe [path_to_orlib20.dll]
//
// If no command-line argument is given, defaults to:
//   tmp/re_artifacts/orlib20_baseline.dll (relative to project root)
//
// Exit codes:
//   0 = all 12 signatures PASS (unique, matched at expected RVA)
//   1 = at least one signature FAILed

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
#include <string>
#include <windows.h>

#include "b1_signatures.h"

// ---------------------------------------------------------------------------
// Parsed hex pattern: parallel byte array + wildcard mask.
// ---------------------------------------------------------------------------
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    wildcards;  // true = wildcard (match any byte)
};

// Parse a space-separated hex pattern string ("55 8B ?? EC") into a Pattern.
// "??" tokens become wildcards; "XX" tokens are fixed bytes.
static Pattern parseHexPattern(const char* hex) {
    Pattern p;
    const char* s = hex;
    while (*s) {
        // Skip whitespace.
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        // Read a 2-char token.
        char tok[3] = {0};
        tok[0] = *s++;
        if (*s && *s != ' ' && *s != '\t') {
            tok[1] = *s++;
        }

        if (tok[0] == '?' && tok[1] == '?') {
            p.bytes.push_back(0x00);
            p.wildcards.push_back(true);
        } else {
            // Parse hex.
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int hi = hexVal(tok[0]);
            int lo = hexVal(tok[1]);
            if (hi < 0 || lo < 0) {
                std::fprintf(stderr, "ERROR: invalid hex token \"%s\" in pattern\n",
                             tok);
                return p;  // partial/empty pattern will likely fail to match
            }
            p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            p.wildcards.push_back(false);
        }
    }
    return p;
}

// Scan a byte buffer for matches of the given pattern (respecting wildcards).
// Returns a vector of offsets within [data, data+dataSize) where the pattern
// starts.
static std::vector<size_t> scanBuffer(const uint8_t* data, size_t dataSize,
                                      const Pattern& pat) {
    std::vector<size_t> matches;
    if (pat.bytes.empty() || pat.bytes.size() > dataSize)
        return matches;

    size_t patLen = pat.bytes.size();
    for (size_t i = 0; i + patLen <= dataSize; i++) {
        bool match = true;
        for (size_t j = 0; j < patLen; j++) {
            if (!pat.wildcards[j] && data[i + j] != pat.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match)
            matches.push_back(i);
    }
    return matches;
}

// Parse a hex byte string ("B8 01 00 00 00") into a byte vector (for
// originalBytes / replacementBytes fields).
static std::vector<uint8_t> parseHexBytes(const char* hex) {
    std::vector<uint8_t> result;
    if (!hex) return result;
    Pattern p = parseHexPattern(hex);
    result = p.bytes;  // no wildcards expected, but bytes are correct
    return result;
}

int main(int argc, char* argv[]) {
    std::printf("=== sig_scan: orlib20.dll signature uniqueness probe ===\n");
    std::printf("sizeof(void*) = %zu (must be 4 for 32-bit)\n", sizeof(void*));
    if (sizeof(void*) != 4) {
        std::printf("FATAL: probe is not 32-bit; PE32 parsing assumes 32-bit "
                    "optional header.\n");
        return 2;
    }

    // 1. Determine DLL path.
    std::string dllPath;
    if (argc >= 2) {
        dllPath = argv[1];
    } else {
        dllPath = "tmp/re_artifacts/orlib20_baseline.dll";
    }
    std::printf("Opening DLL: %s\n", dllPath.c_str());

    // 2. Read the entire file into memory.
    std::ifstream f(dllPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "FATAL: cannot open \"%s\" for reading.\n",
                     dllPath.c_str());
        return 2;
    }
    std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    f.close();
    std::printf("File size: %zu bytes\n", fileData.size());

    if (fileData.size() < sizeof(IMAGE_DOS_HEADER)) {
        std::fprintf(stderr, "FATAL: file too small for DOS header.\n");
        return 2;
    }

    // 3. Parse PE headers.
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(fileData.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        std::fprintf(stderr, "FATAL: bad DOS magic (0x%04X, expected 0x%04X).\n",
                     dos->e_magic, IMAGE_DOS_SIGNATURE);
        return 2;
    }

    DWORD peOffset = dos->e_lfanew;
    if (peOffset + sizeof(IMAGE_NT_HEADERS32) > fileData.size()) {
        std::fprintf(stderr, "FATAL: PE headers beyond end of file.\n");
        return 2;
    }

    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(fileData.data() + peOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        std::fprintf(stderr, "FATAL: bad PE signature (0x%08X).\n",
                     nt->Signature);
        return 2;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        std::fprintf(stderr, "FATAL: not a PE32 optional header "
                     "(magic=0x%04X, expected 0x%04X).\n",
                     nt->OptionalHeader.Magic,
                     IMAGE_NT_OPTIONAL_HDR32_MAGIC);
        return 2;
    }

    std::printf("PE signature OK. Machine=0x%04X, %u sections, "
                "image base=0x%08X.\n",
                nt->FileHeader.Machine, nt->FileHeader.NumberOfSections,
                nt->OptionalHeader.ImageBase);

    // 4. Find the .text section.
    const IMAGE_SECTION_HEADER* sections =
        IMAGE_FIRST_SECTION(nt);  // macro: first section after optional header

    const IMAGE_SECTION_HEADER* textSection = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {0};
        std::memcpy(name, sections[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            textSection = &sections[i];
            break;
        }
    }

    if (!textSection) {
        std::fprintf(stderr, "FATAL: .text section not found.\n");
        return 2;
    }

    DWORD textRVA     = textSection->VirtualAddress;
    DWORD textVSize   = textSection->Misc.VirtualSize;
    DWORD textRawOff  = textSection->PointerToRawData;
    DWORD textRawSize = textSection->SizeOfRawData;

    std::printf("\n--- .text section ---\n");
    std::printf("  RVA (virtual address): 0x%08X\n", textRVA);
    std::printf("  Virtual size:           0x%08X (%u bytes)\n",
                textVSize, textVSize);
    std::printf("  Raw offset (file):      0x%08X\n", textRawOff);
    std::printf("  Raw size:               0x%08X (%u bytes)\n",
                textRawSize, textRawSize);

    if (textRawOff + textRawSize > fileData.size()) {
        std::fprintf(stderr, "FATAL: .text raw data extends beyond file.\n");
        return 2;
    }

    const uint8_t* textData = fileData.data() + textRawOff;
    DWORD textScanSize = textRawSize;  // scan full raw size (>= virtual size)

    // 5. Scan each signature.
    std::printf("\n--- Signature scan results ---\n");
    int passCount = 0;
    std::vector<std::string> failures;

    for (int si = 0; si < kSignatureCount; si++) {
        const SignatureDef& sig = kSignatures[si];
        Pattern pat = parseHexPattern(sig.hexPattern);
        int patLen = static_cast<int>(pat.bytes.size());

        std::printf("\n[%2d] %s\n", si + 1, sig.name);
        std::printf("     Expected RVA: 0x%08X, pattern length: %d bytes\n",
                    sig.rva, patLen);

        if (patLen == 0) {
            std::printf("     FAIL: empty pattern (parse error)\n");
            failures.push_back(sig.name);
            continue;
        }

        std::vector<size_t> matches =
            scanBuffer(textData, textScanSize, pat);

        std::printf("     Match count: %d (expected %d)\n",
                    static_cast<int>(matches.size()), sig.expectedMatchCount);

        for (size_t mi = 0; mi < matches.size(); mi++) {
            size_t offInText = matches[mi];
            DWORD fileOff = textRawOff + static_cast<DWORD>(offInText);
            DWORD rva     = textRVA + static_cast<DWORD>(offInText);
            std::printf("     match[%zu]: file offset=0x%08X, RVA=0x%08X",
                        mi, fileOff, rva);
            if (rva == static_cast<DWORD>(sig.rva)) {
                std::printf("  <-- expected RVA");
            }
            std::printf("\n");
        }

        // PASS criteria: exactly 1 match AND match RVA == expected RVA.
        bool pass = false;
        if (static_cast<int>(matches.size()) == sig.expectedMatchCount &&
            sig.expectedMatchCount == 1) {
            DWORD matchRva = textRVA + static_cast<DWORD>(matches[0]);
            if (matchRva == static_cast<DWORD>(sig.rva)) {
                pass = true;
            }
        }

        if (pass) {
            std::printf("     PASS\n");
            passCount++;
        } else {
            std::printf("     FAIL");
            if (matches.size() != 1)
                std::printf(" (count=%zu, expected 1)", matches.size());
            else {
                DWORD matchRva = textRVA + static_cast<DWORD>(matches[0]);
                if (matchRva != static_cast<DWORD>(sig.rva))
                    std::printf(" (RVA=0x%08X, expected 0x%08X)",
                                matchRva, sig.rva);
            }
            std::printf("\n");
            failures.push_back(sig.name);
        }

        // For patch sites, also show patch info.
        if (sig.isPatchSite) {
            std::printf("     [PATCH SITE] containing function: %s\n",
                        sig.containingFunction ? sig.containingFunction : "?");
            std::printf("       patchOffset: %d (0x%X) from function entry\n",
                        sig.patchOffset, sig.patchOffset);
            std::printf("       original bytes:  %s\n",
                        sig.originalBytes ? sig.originalBytes : "?");
            std::printf("       replacement bytes: %s\n",
                        sig.replacementBytes ? sig.replacementBytes : "?");

            // Verify the byte at the patch offset within the matched function.
            if (pass && matches.size() == 1) {
                size_t patchFileOffInText = matches[0] + sig.patchOffset;
                if (patchFileOffInText < textScanSize) {
                    uint8_t actualByte = textData[patchFileOffInText];
                    std::printf("       byte at patch offset: 0x%02X\n",
                                actualByte);
                    // Parse original bytes and check first byte.
                    std::vector<uint8_t> origBytes =
                        parseHexBytes(sig.originalBytes);
                    if (!origBytes.empty()) {
                        if (actualByte == origBytes[0]) {
                            std::printf("       patch byte check: OK "
                                        "(matches original 0x%02X)\n",
                                        origBytes[0]);
                        } else {
                            std::printf("       patch byte check: WARNING "
                                        "(got 0x%02X, expected original "
                                        "0x%02X — may already be patched)\n",
                                        actualByte, origBytes[0]);
                        }
                    }
                }
            }
        }
    }

    // 6. Summary.
    std::printf("\n=== SUMMARY ===\n");
    std::printf("%d / %d signatures PASS\n", passCount, kSignatureCount);
    if (!failures.empty()) {
        std::printf("FAILURES:\n");
        for (const std::string& name : failures) {
            std::printf("  - %s\n", name.c_str());
        }
    }

    if (passCount == kSignatureCount) {
        std::printf("\nALL SIGNATURES UNIQUE AND VERIFIED.\n");
        return 0;
    } else {
        std::printf("\nSOME SIGNATURES FAILED — see above.\n");
        return 1;
    }
}
