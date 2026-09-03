// test_no_hardcoded_rvas.cpp
//
// AC1 verification: tsdev_lib.cpp must contain ZERO hardcoded RVA constants.
// This test reads src/tsdev_lib.cpp from disk and checks that none of the
// forbidden RVA hex constants appear in the file.
//
// The forbidden set is DERIVED FROM signatures.gen.h (the generated header),
// which is itself produced from signatures/v9.50.01.3344.json by sigcodegen.py.
// This is "Option A" from docs/archive/ARCHITECTURE_REDESIGN.md §7.4: the single source
// of truth becomes JSON -> generated header -> test. There is no longer a
// hand-maintained forbidden-constants array in this file.
//
// For each signature entry:
//   - if isPatchSite is false: forbid the signature's match-point rva.
//   - if isPatchSite is true:  forbid (rva + patchOffset), the actual patch
//     site address (per TVS-E.3, the signature rva is the match point, not the
//     patch site).
// For each data global: forbid its rva.
// Exports are NOT included (they are resolved by name via GetProcAddress; their
// RVAs were never hardcoded in tsdev_lib.cpp).
//
// The test locates src/tsdev_lib.cpp relative to the executable's location.
// Build output lives in build\Release\, so the repo root is two levels up
// from the executable directory (..\..\src\tsdev_lib.cpp). A command-line
// argument may optionally override the path.
//
// Exit codes:
//   0  = PASS -- none of the forbidden constants found in tsdev_lib.cpp
//   1  = FAIL -- at least one forbidden RVA constant still present
//   2  = ERROR -- the file could not be opened/read

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <windows.h>

#include "signatures.gen.h"

namespace {

// A forbidden RVA: the lowercase hex substring to search for and the symbolic
// name (from the generated header) used in diagnostics.
using ForbiddenEntry = std::pair<std::string, std::string>;

// Build the forbidden-constants set from signatures.gen.h.
//
// Signatures: for non-patch-site entries forbid the match-point rva; for
// patch-site entries forbid (rva + patchOffset), the actual patch address.
// Data globals: forbid each rva. Exports are intentionally excluded.
std::vector<ForbiddenEntry> buildForbidden() {
    std::vector<ForbiddenEntry> out;

    for (int i = 0; i < tsdev::kSignatureCount; ++i) {
        const tsdev::SignatureDef& sig = tsdev::kSignatures[i];
        unsigned int value = sig.isPatchSite
            ? static_cast<unsigned int>(sig.rva + sig.patchOffset)
            : static_cast<unsigned int>(sig.rva);

        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", value);
        out.emplace_back(std::string(hex), std::string(sig.name));
    }

    for (int i = 0; i < tsdev::kDataGlobalCount_v9_50_01_3344; ++i) {
        const tsdev::DataGlobalDef& dg = tsdev::kDataGlobals_v9_50_01_3344[i];
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", static_cast<unsigned int>(dg.rva));
        out.emplace_back(std::string(hex), std::string(dg.name));
    }

    return out;
}

// Case-insensitive substring search. Returns true if needle appears in haystack.
// Uses std::string::find with a lowercase copy of the haystack. The forbidden
// hex strings are already lowercase (%x in buildForbidden).
bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    if (haystack.size() < needle.size()) return false;

    std::string hayLower;
    hayLower.reserve(haystack.size());
    for (char c : haystack) {
        hayLower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    }
    return hayLower.find(needle) != std::string::npos;
}

// Resolve the path to src/tsdev_lib.cpp.
// 1. If argv[1] is provided, use it directly.
// 2. Otherwise, derive from the executable path: <exe_dir>\..\..\src\tsdev_lib.cpp
std::string resolveTargetPath(int argc, char** argv) {
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        return argv[1];
    }

    wchar_t exePathW[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // Fallback: use a relative path from the current working directory.
        return "src\\tsdev_lib.cpp";
    }

    // Convert wide path to narrow (UTF-8-ish; paths here are ASCII-safe).
    char exePathA[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, exePathW, -1, exePathA, MAX_PATH, nullptr, nullptr);

    // Strip the executable filename to get the directory.
    std::string dir(exePathA);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
    }

    // Navigate up two levels (build\Release\ -> repo root) then into src\.
    return dir + "\\..\\..\\src\\tsdev_lib.cpp";
}

} // namespace

int main(int argc, char** argv) {
    std::string targetPath = resolveTargetPath(argc, argv);

    // Read the entire file into a string.
    std::ifstream ifs(targetPath, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        std::fprintf(stderr, "ERROR: could not open file: %s\n", targetPath.c_str());
        return 2;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string content = oss.str();
    ifs.close();

    if (content.empty()) {
        std::fprintf(stderr, "ERROR: file is empty or unreadable: %s\n", targetPath.c_str());
        return 2;
    }

    // Forbidden constants are derived from signatures.gen.h (JSON -> header).
    std::vector<ForbiddenEntry> forbidden = buildForbidden();

    int failCount = 0;
    for (const auto& entry : forbidden) {
        if (containsCaseInsensitive(content, entry.first)) {
            std::fprintf(stderr,
                "FAIL: tsdev_lib.cpp still contains forbidden RVA constant %s (%s)\n",
                entry.first.c_str(), entry.second.c_str());
            ++failCount;
        }
    }

    if (failCount > 0) {
        std::fprintf(stderr,
            "\n%d forbidden RVA constant(s) still present in tsdev_lib.cpp (path: %s)\n"
            "Forbidden set derived from signatures.gen.h (%zu entries).\n",
            failCount, targetPath.c_str(), forbidden.size());
        return 1;
    }

    std::printf("PASS: tsdev_lib.cpp contains zero hardcoded RVA constants\n"
                "(forbidden set derived from signatures.gen.h, %zu entries)\n",
                forbidden.size());
    return 0;
}
