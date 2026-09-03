// test_directory_batch.cpp
//
// Unit test for parseDirectory() — the directory-enumeration logic that the
// `batch-<op>-dir` CLI subcommands (TVS-I.2) rely on. parseDirectory() walks a
// directory with std::filesystem::directory_iterator, filters regular files by
// accepted extensions (default: .el, .txt; overridable), extracts the file
// stem as the item name, and (for insert/modify/upsert) reads the file content
// as the EL source. For delete (needsSource=false) the source is left empty.
//
// TVS-I.2 unit test for the implemented parseDirectory() helper in
// src/tsdev_cli.cpp. The test exercises the production directory-enumeration
// logic directly in the same translation unit.
//
// Linkage mechanism:
//   parseDirectory() and BatchItem live inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so they are NOT accessible from a
//   separate translation unit. The test therefore #includes the .cpp directly
//   so it is part of the SAME translation unit. This pulls in tsdev_cli.cpp's
//   main(), which would conflict with this test's own main(); the test
//   #defines TSDEV_CLI_TEST before the include and the production source wraps
//   its main() in #ifndef TSDEV_CLI_TEST / #endif so the CLI main is excluded.
//
// The test is a pure unit test of parseDirectory()'s filesystem logic: it does
// NOT construct TsDevLib, does NOT touch orlib20, and does NOT need a 64 MB
// worker thread. It creates a temp directory tree with std::filesystem, writes
// temp .el/.txt files, calls parseDirectory(), and asserts on the resulting
// BatchItem vector. Temp dirs are cleaned up at the end.
//
// Exit code: 0 on success, nonzero on failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// tsdev_cli.cpp provides the corresponding #ifndef TSDEV_CLI_TEST guard.
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (BatchItem, parseDirectory, readFile, parseType, CliResult) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Test helpers.
// ---------------------------------------------------------------------------

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Create a unique temp directory under the system temp dir. Returns the path.
// Uses a random suffix to avoid collisions with parallel/previous runs.
fs::path makeTempDir(const char* tag) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 999999);
    fs::path base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 8; ++attempt) {
        fs::path p = base / (std::string("tsdev_dirbatch_") + tag + "_" +
                             std::to_string(dist(gen)));
        std::error_code ec;
        if (fs::create_directory(p, ec)) return p;
    }
    throw std::runtime_error("failed to create unique temp directory");
}

// Write content to a file (binary mode). Throws on failure.
void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("failed to open for write: " + path.string());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) throw std::runtime_error("failed to write: " + path.string());
}

// Find a BatchItem by name in a vector. Returns pointer or nullptr.
const BatchItem* findItem(const std::vector<BatchItem>& items,
                          const std::string& name) {
    for (const auto& it : items) {
        if (it.name == name) return &it;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Test 1: Extension filtering — mixed .el and .txt, default extensions.
// A directory with alpha.el, beta.txt, and gamma.md (non-matching). With the
// default extensions {".el", ".txt"}, only alpha and beta should be picked up;
// gamma.md must be excluded.
// ---------------------------------------------------------------------------
int testExtensionFilteringDefault() {
    std::fprintf(stdout, "\n[1] Extension filtering (default .el/.txt)...\n");
    fs::path dir = makeTempDir("extdef");
    try {
        writeFile(dir / "alpha.el",  "{ alpha }\r\nBuy next bar at Market;\r\n");
        writeFile(dir / "beta.txt",  "{ beta }\r\nSell next bar at Market;\r\n");
        writeFile(dir / "gamma.md",  "# not an EL file\r\n");

        std::vector<BatchItem> items;
        std::vector<std::string> noExts;  // empty → defaults
        parseDirectory(static_cast<int>(ComponentType::Strategy),
                       "strategy", dir.string(), noExts,
                       /*needsSource=*/true, items);

        if (items.size() != 2) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[1] expected 2 items (alpha, beta), got %zu",
                          items.size());
            fs::remove_all(dir);
            return fail(buf, 101);
        }
        const BatchItem* a = findItem(items, "alpha");
        const BatchItem* b = findItem(items, "beta");
        if (!a || !b) {
            fs::remove_all(dir);
            return fail("[1] missing alpha or beta item", 102);
        }
        if (findItem(items, "gamma")) {
            fs::remove_all(dir);
            return fail("[1] gamma.md should have been excluded", 103);
        }
        // Source should be read for needsSource=true.
        if (a->source.empty() || b->source.empty()) {
            fs::remove_all(dir);
            return fail("[1] source text should be non-empty for needsSource=true",
                        104);
        }
        std::fprintf(stdout, "    PASS: 2 items (alpha, beta), gamma excluded, "
                     "sources read\n");
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 199);
    }
    fs::remove_all(dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 2: Extension override — only .el specified.
// Same directory as test 1 (alpha.el, beta.txt, gamma.md). With extensions
// {".el"} only, only alpha should be picked up; beta.txt must be excluded.
// ---------------------------------------------------------------------------
int testExtensionOverride() {
    std::fprintf(stdout, "\n[2] Extension override (only .el)...\n");
    fs::path dir = makeTempDir("extovr");
    try {
        writeFile(dir / "alpha.el",  "{ alpha }\r\nBuy next bar at Market;\r\n");
        writeFile(dir / "beta.txt",  "{ beta }\r\nSell next bar at Market;\r\n");

        std::vector<BatchItem> items;
        std::vector<std::string> onlyEl = {".el"};
        parseDirectory(static_cast<int>(ComponentType::Strategy),
                       "strategy", dir.string(), onlyEl,
                       /*needsSource=*/true, items);

        if (items.size() != 1) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[2] expected 1 item (alpha only), got %zu",
                          items.size());
            fs::remove_all(dir);
            return fail(buf, 201);
        }
        if (!findItem(items, "alpha")) {
            fs::remove_all(dir);
            return fail("[2] missing alpha item", 202);
        }
        if (findItem(items, "beta")) {
            fs::remove_all(dir);
            return fail("[2] beta.txt should have been excluded by .el-only filter",
                        203);
        }
        std::fprintf(stdout, "    PASS: 1 item (alpha), beta.txt excluded\n");
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 299);
    }
    fs::remove_all(dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 3: Stem extraction — my.strategy.el → stem = "my.strategy".
// std::filesystem::path::stem() strips only the LAST extension, so a file with
// multiple dots keeps the intermediate dots in the stem.
// ---------------------------------------------------------------------------
int testStemExtraction() {
    std::fprintf(stdout, "\n[3] Stem extraction (my.strategy.el)...\n");
    fs::path dir = makeTempDir("stem");
    try {
        writeFile(dir / "my.strategy.el", "{ my.strategy }\r\nBuy;\r\n");
        writeFile(dir / "plain.el",        "{ plain }\r\nSell;\r\n");

        std::vector<BatchItem> items;
        std::vector<std::string> noExts;  // defaults
        parseDirectory(static_cast<int>(ComponentType::Strategy),
                       "strategy", dir.string(), noExts,
                       /*needsSource=*/true, items);

        const BatchItem* ms = findItem(items, "my.strategy");
        if (!ms) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[3] expected item with stem \"my.strategy\", got %zu items",
                          items.size());
            fs::remove_all(dir);
            return fail(buf, 301);
        }
        const BatchItem* pl = findItem(items, "plain");
        if (!pl) {
            fs::remove_all(dir);
            return fail("[3] missing plain item", 302);
        }
        std::fprintf(stdout, "    PASS: my.strategy.el -> stem \"%s\"\n",
                     ms->name.c_str());
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 399);
    }
    fs::remove_all(dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 4: Empty directory → 0 items.
// A freshly-created directory with no files should yield an empty BatchItem
// vector. (The caller — cmdBatchDir — is responsible for emitting the
// "no files matching" error; parseDirectory itself just returns 0 items.)
// ---------------------------------------------------------------------------
int testEmptyDir() {
    std::fprintf(stdout, "\n[4] Empty directory -> 0 items...\n");
    fs::path dir = makeTempDir("empty");
    try {
        std::vector<BatchItem> items;
        std::vector<std::string> noExts;  // defaults
        parseDirectory(static_cast<int>(ComponentType::Strategy),
                       "strategy", dir.string(), noExts,
                       /*needsSource=*/true, items);

        if (!items.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[4] expected 0 items for empty dir, got %zu",
                          items.size());
            fs::remove_all(dir);
            return fail(buf, 401);
        }
        std::fprintf(stdout, "    PASS: empty dir -> 0 items\n");
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 499);
    }
    fs::remove_all(dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 5: Subdirectory skipping — only regular files, no recursion.
// Create a .el file at the top level AND a .el file inside a subdirectory.
// Only the top-level file should be picked up; the subdirectory's file must
// be skipped (parseDirectory uses directory_iterator, not recursive_directory_
// iterator, and filters via is_regular_file()).
// ---------------------------------------------------------------------------
int testSubdirSkipping() {
    std::fprintf(stdout, "\n[5] Subdirectory skipping (no recursion)...\n");
    fs::path dir = makeTempDir("subdir");
    try {
        writeFile(dir / "top.el", "{ top }\r\nBuy;\r\n");
        fs::create_directory(dir / "nested");
        writeFile(dir / "nested" / "deep.el", "{ deep }\r\nSell;\r\n");

        std::vector<BatchItem> items;
        std::vector<std::string> noExts;  // defaults
        parseDirectory(static_cast<int>(ComponentType::Strategy),
                       "strategy", dir.string(), noExts,
                       /*needsSource=*/true, items);

        if (items.size() != 1) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[5] expected 1 item (top only), got %zu",
                          items.size());
            fs::remove_all(dir);
            return fail(buf, 501);
        }
        if (!findItem(items, "top")) {
            fs::remove_all(dir);
            return fail("[5] missing top item", 502);
        }
        if (findItem(items, "deep")) {
            fs::remove_all(dir);
            return fail("[5] nested/deep.el should have been skipped "
                        "(no recursion)", 503);
        }
        std::fprintf(stdout, "    PASS: 1 item (top), nested/deep.el skipped\n");
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 599);
    }
    fs::remove_all(dir);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 6: Source reading — needsSource=true reads file; needsSource=false
// (delete) leaves source empty.
// Create alpha.el with known content. Call parseDirectory twice: once with
// needsSource=true (insert/modify/upsert) — source must equal the file content;
// once with needsSource=false (delete) — source must be empty.
// ---------------------------------------------------------------------------
int testSourceReading() {
    std::fprintf(stdout, "\n[6] Source reading (needsSource true vs false)...\n");
    fs::path dir = makeTempDir("src");
    const std::string content =
        "{ alpha }\r\nBuy (\"EnterLong\") next bar at Market;\r\n";
    try {
        writeFile(dir / "alpha.el", content);

        // needsSource = true (insert/modify/upsert): source is read from file.
        {
            std::vector<BatchItem> items;
            std::vector<std::string> noExts;
            parseDirectory(static_cast<int>(ComponentType::Strategy),
                           "strategy", dir.string(), noExts,
                           /*needsSource=*/true, items);
            const BatchItem* a = findItem(items, "alpha");
            if (!a) {
                fs::remove_all(dir);
                return fail("[6] missing alpha item (needsSource=true)", 601);
            }
            if (a->source != content) {
                fs::remove_all(dir);
                return fail("[6] source text mismatch for needsSource=true", 602);
            }
            std::fprintf(stdout, "    [6a] PASS: needsSource=true -> source read "
                         "(%zu bytes)\n", a->source.size());
        }

        // needsSource = false (delete): source is empty.
        {
            std::vector<BatchItem> items;
            std::vector<std::string> noExts;
            parseDirectory(static_cast<int>(ComponentType::Strategy),
                           "strategy", dir.string(), noExts,
                           /*needsSource=*/false, items);
            const BatchItem* a = findItem(items, "alpha");
            if (!a) {
                fs::remove_all(dir);
                return fail("[6] missing alpha item (needsSource=false)", 603);
            }
            if (!a->source.empty()) {
                fs::remove_all(dir);
                return fail("[6] source should be empty for needsSource=false "
                            "(delete)", 604);
            }
            std::fprintf(stdout, "    [6b] PASS: needsSource=false -> source empty\n");
        }
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        return fail(e.what(), 699);
    }
    fs::remove_all(dir);
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_directory_batch: parseDirectory() logic (TVS-I.2) ===\n");

    int rc = 0;
    int stepRc;

    stepRc = testExtensionFilteringDefault();  if (stepRc) rc = stepRc;
    stepRc = testExtensionOverride();          if (stepRc) rc = stepRc;
    stepRc = testStemExtraction();             if (stepRc) rc = stepRc;
    stepRc = testEmptyDir();                   if (stepRc) rc = stepRc;
    stepRc = testSubdirSkipping();             if (stepRc) rc = stepRc;
    stepRc = testSourceReading();              if (stepRc) rc = stepRc;

    if (rc == 0) {
        std::fprintf(stdout,
                     "\nPASS: parseDirectory() directory-batch logic verified — "
                     "extension filtering, default vs override, stem extraction, "
                     "empty dir, subdirectory skipping, source reading\n");
    }

    std::fprintf(stdout,
                 "\n=== test_directory_batch complete (exit %d) ===\n", rc);
    return rc;
}
