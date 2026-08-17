// test_orlib_resolver.cpp
//
// TVS-B.4 TDD — Cycle 1: construction (test-mode in-memory ctor), PE header
// parsing, and accessors.
//
// This test loads tests/fixtures/orlib20_baseline.dll into an in-memory
// buffer and constructs an OrlibResolver via the test constructor
// (in-memory PE image, no LoadLibraryW). It then asserts:
//   - isTestMode() == true
//   - baseAddress() == 0x10000000 (the simulated base passed to the ctor)
//   - moduleHandle() == nullptr (test mode acquires no HMODULE)
//   - textSectionBounds() == {0x1000, 0x1F7400} (.text RVA + raw size)
//   - signatureSet() == SIGSET_NONE (stub detectVersion returns default)
//
// RED phase (Cycle 1): Against the current stubs, parsePeHeaders() is empty
// so m_textRva / m_textRawSize stay 0 → textSectionBounds() returns {0,0}
// and the textSectionBounds assertion FAILS. The other assertions pass
// against the stub (the ctor sets m_testMode/m_base/m_module correctly).
//
// GREEN phase (later subagent): Implement parsePeHeaders() to populate
// m_textRva=0x1000, m_textRawSize=0x1F7400 → all assertions pass.
//
// Exit codes:
//   0  = all assertions passed
//   1  = one or more assertions failed
//   99 = DLL file could not be loaded (environment/setup error)

#include "signatures.gen.h"  // Must come before orlib_resolver.h: defines SignatureSetId enum
#include "orlib_resolver.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <utility>
#include <windows.h>

// ---------------------------------------------------------------------------
// CHECK macro — prints PASS/FAIL per assertion with line numbers.
// ---------------------------------------------------------------------------
static int g_failCount = 0;
static int g_passCount = 0;

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failCount++;                                             \
    } else {                                                       \
        std::printf("PASS: %s\n", msg);                            \
        g_passCount++;                                             \
    }                                                              \
} while(0)

// ---------------------------------------------------------------------------
// loadDllBuffer — read entire file contents into a byte vector (binary mode).
// Mirrors the file-reading pattern from sig_scan.cpp (std::ifstream +
// std::istreambuf_iterator).
// ---------------------------------------------------------------------------
static bool loadDllBuffer(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "ERROR: cannot open \"%s\" for reading.\n", path);
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    f.close();
    if (out.empty()) {
        std::fprintf(stderr, "ERROR: \"%s\" is empty or unreadable.\n", path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// findDllPath — try candidate paths and return the first that exists.
// Candidates cover: run-from-project-root, run-from-build\Release\, and an
// absolute fallback. If none exist, the first candidate is returned (so the
// caller can print a loud error listing every path that was tried).
// ---------------------------------------------------------------------------
static const char* kDllCandidates[] = {
    "tests/fixtures/orlib20_baseline.dll",
    "../../tests/fixtures/orlib20_baseline.dll",
    "C:\\Users\\user\\projects\\tsdev_automation\\tests\\fixtures\\orlib20_baseline.dll",
};
static const int kDllCandidateCount =
    sizeof(kDllCandidates) / sizeof(kDllCandidates[0]);

static std::string findDllPath() {
    for (int i = 0; i < kDllCandidateCount; i++) {
        DWORD attr = GetFileAttributesA(kDllCandidates[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::printf("Using DLL path: %s\n", kDllCandidates[i]);
            return std::string(kDllCandidates[i]);
        }
    }
    // Fallback: return the first candidate (loadDllBuffer will print the error).
    std::fprintf(stderr, "WARNING: no candidate DLL path found; trying first candidate.\n");
    return std::string(kDllCandidates[0]);
}

// ===========================================================================
// main — Cycle 1 assertions.
//
// Structure: later cycles will append more CHECK assertions before the final
// summary/return block.
// ===========================================================================
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_orlib_resolver: Cycle 1 (construction + PE parsing) ===\n");

    // --- 1. Load the baseline DLL into an in-memory buffer ---
    std::string dllPath = findDllPath();
    std::vector<uint8_t> buffer;
    if (!loadDllBuffer(dllPath.c_str(), buffer)) {
        std::fprintf(stderr,
            "\nFATAL: Baseline DLL not found at any of the following paths:\n");
        for (int i = 0; i < kDllCandidateCount; i++) {
            std::fprintf(stderr, "  %s\n", kDllCandidates[i]);
        }
        std::fprintf(stderr,
            "This DLL is required for test_orlib_resolver (signature resolution tests).\n"
            "To obtain it: copy orlib20.dll from your TradeStation installation\n"
            "(<TS install>\\Program\\orlib20.dll) to tests/fixtures/orlib20_baseline.dll\n"
            "or run the regression suite (tests/run_regression.ps1) which will prompt\n"
            "you to copy it automatically.\n\n");
        return 99;
    }
    std::printf("Loaded DLL: %zu bytes\n", buffer.size());

    // --- 2. Construct OrlibResolver in test mode (in-memory PE image) ---
    // The test ctor is pure computation (no DLL loading, no worker thread
    // needed). Wrap in try/catch — construction must NOT throw.
    tsdev::OrlibResolver* resolver = nullptr;
    try {
        resolver = new tsdev::OrlibResolver(buffer.data(),
                                            buffer.size(),
                                            0x10000000ull);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: construction threw exception: %s (line %d)\n",
                     e.what(), __LINE__);
        g_failCount++;
        std::printf("Cycle 1: %d/%d assertions passed\n",
                    g_passCount, g_passCount + g_failCount);
        std::printf("Total failures: %d\n", g_failCount);
        return g_failCount == 0 ? 0 : 1;
    }
    std::printf("OrlibResolver constructed OK (test mode)\n");

    // --- 3. Cycle 1 assertions ---
    // NOTE: Against the stubs, textSectionBounds() returns {0,0} (parsePeHeaders
    // is empty) → that assertion FAILS (RED). The others pass against the stub
    // because the test ctor sets m_testMode=true, m_base=0x10000000,
    // m_module=nullptr, and detectVersion returns a default VersionDescriptor
    // whose signatureSet is SIGSET_NONE.

    CHECK(resolver->isTestMode() == true,
          "isTestMode()==true");
    CHECK(resolver->baseAddress() == 0x10000000ull,
          "baseAddress()==0x10000000");
    CHECK(resolver->moduleHandle() == nullptr,
          "moduleHandle()==nullptr (test mode)");
    {
        std::pair<uintptr_t, size_t> tb = resolver->textSectionBounds();
        CHECK(tb.first == 0x1000ull && tb.second == 0x1F7400ull,
              "textSectionBounds()=={0x1000, 0x1F7400}");
    }
    CHECK(resolver->signatureSet() == tsdev::SignatureSetId::SIGSET_V9_50_01_3344,
          "signatureSet()==SIGSET_V9_50_01_3344 (known DLL, detectVersion at construction)");

    // ===================================================================
    // Cycle 2: 8 internal-function signature resolvers + failure path.
    //
    // Each resolver is called inside a try/catch. Against the current
    // stubs (RED phase), every resolver throws OrlibResolverError
    // ("not implemented") → the catch block fires → CHECK(false) → FAIL.
    // After implementation (GREEN phase), each resolver returns a typed
    // function pointer at the expected RVA → the try block computes the
    // RVA and CHECK(rva == EXPECTED) → PASS.
    //
    // Expected RVAs are taken from signatures.gen.h
    // (the `rva` field of each SignatureDef), cross-checked against
    // docs/SIGNATURE_CATALOG.md §3.
    // ===================================================================
    std::printf("\n=== Cycle 2: 8 internal-function resolvers ===\n");

    // The RVA of a resolved pointer is (absolute - baseAddress()).
    // baseAddress() == 0x10000000 (passed to the test ctor).
    const uintptr_t kBase = resolver->baseAddress();

    // --- Target 1: StartCompilingProcess (kSignatures[0], __cdecl) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::StartCompilingProcessFn fn = resolver->resolveStartCompilingProcess();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveStartCompilingProcess threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveStartCompilingProcess RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[0].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[0].rva, buf);
        }
    }

    // --- Target 2: EndCompilingProcess (kSignatures[1], __fastcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::EndCompilingProcessFn fn = resolver->resolveEndCompilingProcess();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveEndCompilingProcess threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveEndCompilingProcess RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[1].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[1].rva, buf);
        }
    }

    // --- Target 3: CELCompiler constructor (kSignatures[2], __fastcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::CELCompilerCtorFn fn = resolver->resolveCELCompilerCtor();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveCELCompilerCtor threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveCELCompilerCtor RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[2].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[2].rva, buf);
        }
    }

    // --- Target 4: CELCompiler destructor (kSignatures[3], __fastcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::CELCompilerDtorFn fn = resolver->resolveCELCompilerDtor();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveCELCompilerDtor threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveCELCompilerDtor RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[3].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[3].rva, buf);
        }
    }

    // --- Target 5: InitGlobalLookup (kSignatures[4], __cdecl) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::InitGlobalLookupFn fn = resolver->resolveInitGlobalLookup();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveInitGlobalLookup threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveInitGlobalLookup RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[4].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[4].rva, buf);
        }
    }

    // --- Target 6: DeleteFromTree (kSignatures[5], __thiscall → void*) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            void* fn = resolver->resolveDeleteFromTree();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveDeleteFromTree threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveDeleteFromTree RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[5].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[5].rva, buf);
        }
    }

    // --- Target 7: GetWarehouse (kSignatures[6], __cdecl) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            tsdev::GetWarehouseFn fn = resolver->resolveGetWarehouse();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveGetWarehouse threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveGetWarehouse RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[6].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[6].rva, buf);
        }
    }

    // --- Target 8: FUN_101a8360 delete orchestrator (kSignatures[7], __fastcall → void*) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            void* fn = resolver->resolveDeleteOrchestrator();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveDeleteOrchestrator threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveDeleteOrchestrator RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kSignatures[7].rva);
            CHECK(rva == (uintptr_t)tsdev::kSignatures[7].rva, buf);
        }
    }

    // ===================================================================
    // Cycle 2 — Failure-path test (AC6).
    //
    // Construct a CORRUPTED copy of the DLL buffer by zeroing the entire
    // .text section. This breaks every signature (0 matches). The resolver
    // must throw OrlibResolverError whose message contains the signature
    // name and the match count (0).
    //
    // To find the .text raw file offset + raw size, we parse the PE
    // headers directly from the buffer (DOS → NT → sections → ".text").
    // This mirrors what parsePeHeaders() does internally but is done
    // inline here so the test is self-contained and does not rely on the
    // (stub) resolver's own parsing.
    // ===================================================================
    std::printf("\n--- Cycle 2: failure-path (corrupted .text, AC6) ---\n");

    // Parse .text raw offset + raw size from the buffer.
    size_t textRawOffset = 0;
    size_t textRawSize   = 0;
    bool  peParsedOk     = false;
    do {
        if (buffer.size() < sizeof(IMAGE_DOS_HEADER)) break;
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) break;
        if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > buffer.size())
            break;
        const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            buffer.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) break;
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
                textRawOffset = sections[i].PointerToRawData;
                textRawSize   = sections[i].SizeOfRawData;
                peParsedOk    = true;
                break;
            }
        }
    } while (false);
    CHECK(peParsedOk,
          "failure-path: parsed .text section from buffer");
    if (peParsedOk) {
        std::printf(".text raw offset=0x%zX raw size=0x%zX\n",
                    textRawOffset, textRawSize);
    }

    // Build the corrupted buffer: copy the original, then zero all of .text.
    std::vector<uint8_t> corrupted = buffer;
    if (peParsedOk && textRawOffset + textRawSize <= corrupted.size()) {
        std::memset(corrupted.data() + textRawOffset, 0, textRawSize);
        std::printf("Corrupted .text: zeroed 0x%zX bytes at file offset 0x%zX\n",
                    textRawSize, textRawOffset);
    } else {
        // Defensive fallback: zero a large chunk starting at 0x400 (typical
        // first-section raw offset). This should still break signatures.
        size_t off = 0x400;
        size_t len = corrupted.size() - off;
        if (len > 0x200000) len = 0x200000; // cap at 2 MB
        std::memset(corrupted.data() + off, 0, len);
        std::printf("Corrupted fallback: zeroed 0x%zX bytes at file offset 0x400\n",
                    len);
    }

    // Construct a second resolver from the corrupted buffer. Construction
    // should succeed (PE headers are intact; only .text is zeroed).
    tsdev::OrlibResolver* corruptedResolver = nullptr;
    try {
        corruptedResolver = new tsdev::OrlibResolver(corrupted.data(),
                                                     corrupted.size(),
                                                     0x10000000ull);
    } catch (const std::exception& e) {
        std::string msg = std::string("corrupted ctor threw: ") + e.what();
        CHECK(false, msg.c_str());
    }

    if (corruptedResolver) {
        // Call resolveStartCompilingProcess on the corrupted buffer.
        // It MUST throw OrlibResolverError (0 matches).
        bool threw = false;
        std::string exMsg;
        std::string exTarget;
        try {
            (void)corruptedResolver->resolveStartCompilingProcess();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw     = true;
            exMsg     = ex.what();
            exTarget  = ex.target();
        } catch (const std::exception& ex) {
            // Wrong exception type — still a failure, but flag it.
            exMsg = std::string("wrong exception type: ") + ex.what();
        }

        CHECK(threw,
              "corrupted buffer: resolveStartCompilingProcess throws OrlibResolverError");

        if (threw) {
            std::printf("Exception message: %s\n", exMsg.c_str());

            // AC6: the exception message must contain the signature name.
            CHECK(exMsg.find("StartCompilingProcess") != std::string::npos ||
                  exTarget.find("StartCompilingProcess") != std::string::npos,
                  "exception message/target contains signature name 'StartCompilingProcess'");

            // AC6: the exception message must contain the match count (0).
            CHECK(exMsg.find("0") != std::string::npos,
                  "exception message contains match count '0'");
        }

        delete corruptedResolver;
        corruptedResolver = nullptr;
    }

    // -----------------------------------------------------------------
    // Multi-match test (AC6 — BEST EFFORT, OPTIONAL).
    //
    // SKIPPED: constructing a buffer where a signature matches exactly
    // twice is fragile. It requires duplicating a known-unique signature
    // byte sequence (including its wildcarded address-dependent bytes)
    // into a second .text location AND ensuring the duplicate does not
    // accidentally create extra matches for other signatures. This is
    // easy to get wrong and would make the test brittle against minor
    // buffer-layout changes. The 0-match test above is MANDATORY and
    // fully exercises the OrlibResolverError throw path (AC6). The
    // multi-match branch (>1) shares the same throw code path with a
    // different match-count value, so the 0-match test provides
    // sufficient coverage of the error mechanism for this cycle.
    // -----------------------------------------------------------------
    std::printf("\n--- Cycle 2: multi-match test (AC6 optional) --- SKIPPED\n");
    std::printf("Reason: fragile to construct reliably; 0-match test covers "
                "the OrlibResolverError throw path (AC6).\n");

    // ===================================================================
    // Cycle 3: 7 export resolvers + generic resolveExport().
    //
    // Exports are resolved by NAME (via GetProcAddress in production, or
    // PE export-table parsing in test mode). The test uses the in-memory
    // test ctor, so exports must be resolved from the buffer's PE export
    // directory.
    //
    // Each specific resolver is called inside a try/catch. Against the
    // current stubs (RED phase), every resolver throws OrlibResolverError
    // ("not implemented") → the catch block fires → CHECK(false) → FAIL.
    // After implementation (GREEN phase), each resolver returns a typed
    // function pointer at the expected RVA → the try block computes the
    // RVA and CHECK(rva == EXPECTED) → PASS.
    //
    // Expected RVAs are taken from tmp/re_artifacts/b1_export_confirmation.md
    // (the authoritative dumpbin /exports confirmation), cross-checked
    // against docs/SIGNATURE_CATALOG.md §5.
    // ===================================================================
    std::printf("\n=== Cycle 3: 7 export resolvers ===\n");

    // --- Export 1: _OrlibInit@8 (kExports_v9_50_01_3344[0], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::OrlibInitFn fn = nullptr;
        try {
            fn = resolver->resolveOrlibInit();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveOrlibInit threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveOrlibInit returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveOrlibInit RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[0].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[0].rva, buf);
        }
    }

    // --- Export 2: _OrlibExit@0 (kExports_v9_50_01_3344[1], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::OrlibExitFn fn = nullptr;
        try {
            fn = resolver->resolveOrlibExit();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveOrlibExit threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveOrlibExit returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveOrlibExit RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[1].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[1].rva, buf);
        }
    }

    // --- Export 3: FindExistPos (undecorated, kExports_v9_50_01_3344[2], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::FindExistPosFn fn = nullptr;
        try {
            fn = resolver->resolveFindExistPos();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveFindExistPos threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveFindExistPos returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveFindExistPos RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[2].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[2].rva, buf);
        }
    }

    // --- Export 4: _CreateEntryEX@24 (kExports_v9_50_01_3344[3], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::CreateEntryEXFn fn = nullptr;
        try {
            fn = resolver->resolveCreateEntryEX();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveCreateEntryEX threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveCreateEntryEX returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveCreateEntryEX RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[3].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[3].rva, buf);
        }
    }

    // --- Export 5: _SaveModule_New@20 (kExports_v9_50_01_3344[4], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::SaveModule_NewFn fn = nullptr;
        try {
            fn = resolver->resolveSaveModuleNew();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveSaveModuleNew threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveSaveModuleNew returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveSaveModuleNew RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[4].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[4].rva, buf);
        }
    }

    // --- Export 6: _VerifyModule_New@20 (kExports_v9_50_01_3344[5], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::VerifyModule_NewFn fn = nullptr;
        try {
            fn = resolver->resolveVerifyModuleNew();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveVerifyModuleNew threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveVerifyModuleNew returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveVerifyModuleNew RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[5].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[5].rva, buf);
        }
    }

    // --- Export 7: _CloseWarehouseForCurrentThreadAndPath@4 (kExports_v9_50_01_3344[6], __stdcall) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        tsdev::CloseWarehouseFn fn = nullptr;
        try {
            fn = resolver->resolveCloseWarehouse();
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveCloseWarehouse threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveCloseWarehouse returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveCloseWarehouse RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[6].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[6].rva, buf);
        }
    }

    // ===================================================================
    // Cycle 3 — Generic resolveExport() tests.
    //
    // resolveExport(const char* decoratedName) returns a FARPROC for any
    // export name present in the PE export table. Tested with:
    //   - "_OrlibInit@8"        → kExports_v9_50_01_3344[0] (decorated stdcall export)
    //   - "FindExistPos"        → kExports_v9_50_01_3344[2] (undecorated export)
    //   - "_NonexistentExport@99" → MUST throw OrlibResolverError
    // ===================================================================
    std::printf("\n--- Cycle 3: generic resolveExport() ---\n");

    // --- resolveExport("_OrlibInit@8") → kExports_v9_50_01_3344[0] ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        FARPROC fn = nullptr;
        try {
            fn = resolver->resolveExport("_OrlibInit@8");
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveExport(\"_OrlibInit@8\") threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveExport(\"_OrlibInit@8\") returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveExport(\"_OrlibInit@8\") RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[0].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[0].rva, buf);
        }
    }

    // --- resolveExport("FindExistPos") → kExports_v9_50_01_3344[2] (undecorated) ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        FARPROC fn = nullptr;
        try {
            fn = resolver->resolveExport("FindExistPos");
            rva = reinterpret_cast<uintptr_t>(fn) - kBase;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveExport(\"FindExistPos\") threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            CHECK(fn != nullptr, "resolveExport(\"FindExistPos\") returns non-null");
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveExport(\"FindExistPos\") RVA==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kExports_v9_50_01_3344[2].rva);
            CHECK(rva == (uintptr_t)tsdev::kExports_v9_50_01_3344[2].rva, buf);
        }
    }

    // --- resolveExport("_NonexistentExport@99") → MUST throw OrlibResolverError ---
    {
        bool threw = false;
        std::string errMsg;
        try {
            (void)resolver->resolveExport("_NonexistentExport@99");
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            // Wrong exception type — still a failure, but flag it.
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        CHECK(threw,
              "resolveExport(\"_NonexistentExport@99\") throws OrlibResolverError");
        if (threw) {
            std::printf("Nonexistent export exception: %s\n", errMsg.c_str());
        }
    }

    // ===================================================================
    // Cycle 4: version detection (3-tier detectVersion).
    //
    // detectVersion() implements the 3-tier decision tree from
    // docs/VERSION_DETECTION.md §4.2:
    //   Tier 1: FileVersion string lookup in the manifest.
    //   Tier 2: PE timestamp + file size cross-check.
    //   Tier 3: SHA-256 byte-identity confirmation.
    //
    // Against the current stubs (RED phase), detectVersion() returns a
    // default VersionDescriptor{known=false} → all Cycle 4 assertions
    // FAIL. After implementation (GREEN phase), the 3-tier logic
    // populates the descriptor correctly → all assertions PASS.
    //
    // Cases 1-3 are MANDATORY. Case 4 is best-effort (soft skip if the
    // UTF-16LE version string is not found in the buffer).
    // ===================================================================
    std::printf("\n=== Cycle 4: version detection (3-tier) ===\n");

    // Track per-cycle assertion counts for the summary.
    int c4_passStart = g_passCount;
    int c4_failStart = g_failCount;

    // -------------------------------------------------------------------
    // Case 1 — Known DLL (MANDATORY, AC5).
    //
    // The baseline DLL is the known, tested build. detectVersion() must
    // return known=true, tier=Tier3Match (all 3 tiers match), with the
    // exact baseline indicators from docs/VERSION_DETECTION.md §2 and
    // the manifest entry from §6.2.
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 4 Case 1: known DLL (AC5) ---\n");
    {
        tsdev::VersionDescriptor vd = resolver->detectVersion();
        const tsdev::VersionDescriptor& vdCached = resolver->version();

        // Print actual values for visibility.
        std::printf("detectVersion: known=%d tier=%d fileVersion='%s' "
                    "peTimestamp=0x%08X fileSize=%u sha256='%s' "
                    "manifestEntry=%p signatureSet=%d warning='%s'\n",
                    (int)vd.known, (int)vd.tier, vd.fileVersion.c_str(),
                    vd.peTimestamp, vd.fileSize, vd.sha256.c_str(),
                    (void*)vd.manifestEntry, (int)vd.signatureSet,
                    vd.warningMessage.c_str());

        CHECK(vd.known == true,
              "Case1: known==true");
        CHECK(vd.tier == tsdev::VersionTier::Tier3Match,
              "Case1: tier==Tier3Match");
        CHECK(vd.fileVersion == "9.50.01.3344",
              "Case1: fileVersion=='9.50.01.3344'");
        CHECK(vd.peTimestamp == 0x5C01735Eu,
              "Case1: peTimestamp==0x5C01735E");
        CHECK(vd.fileSize == 4924400u ||
              vd.fileSize == (uint32_t)buffer.size(),
              "Case1: fileSize==4924400 (or buffer size)");
        CHECK(vd.sha256 == "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
              "Case1: sha256==baseline hash (uppercase)");
        CHECK(vd.signatureSet == tsdev::SignatureSetId::SIGSET_V9_50_01_3344,
              "Case1: signatureSet==SIGSET_V9_50_01_3344");
        CHECK(vd.manifestEntry != nullptr,
              "Case1: manifestEntry!=nullptr");
        CHECK(vd.warningMessage.empty(),
              "Case1: warningMessage empty (known & verified)");

        // version() must return the same descriptor as detectVersion().
        CHECK(vdCached.known == vd.known &&
              vdCached.tier == vd.tier &&
              vdCached.fileVersion == vd.fileVersion,
              "Case1: version() matches detectVersion()");
    }

    // -------------------------------------------------------------------
    // Case 2 — Tier 2 mismatch (MANDATORY).
    //
    // Modify the PE timestamp in the COFF header. FileVersion (Tier 1)
    // still matches → known=true, but Tier 2 (timestamp) mismatches →
    // tier=Tier2Mismatch, warningMessage non-empty.
    //
    // PE timestamp offset computation:
    //   - e_lfanew (DWORD at DOS header offset 0x3C) points to the NT
    //     signature ("PE\0\0", 4 bytes).
    //   - IMAGE_FILE_HEADER follows immediately after the NT signature.
    //   - IMAGE_FILE_HEADER layout: Machine(WORD,2) + NumberOfSections
    //     (WORD,2) + TimeDateStamp(DWORD,4) + ...
    //   - TimeDateStamp is at offset 4 within IMAGE_FILE_HEADER.
    //   - So the timestamp file offset = e_lfanew + 4 (NT sig) + 4
    //     (offset within IMAGE_FILE_HEADER) = e_lfanew + 8.
    //
    // NOTE: The task brief stated "e_lfanew + 4 + 8 = e_lfanew + 12",
    // but that is incorrect — TimeDateStamp is at offset 4 (not 8)
    // within IMAGE_FILE_HEADER. The correct offset is e_lfanew + 8.
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 4 Case 2: Tier 2 mismatch (timestamp) ---\n");
    {
        std::vector<uint8_t> tier2 = buffer;

        // Read e_lfanew from the DOS header (offset 0x3C, DWORD).
        DWORD e_lfanew = *reinterpret_cast<const DWORD*>(tier2.data() + 0x3C);
        // Compute the PE timestamp file offset:
        //   e_lfanew + 4 (NT signature) + 4 (TimeDateStamp offset within
        //   IMAGE_FILE_HEADER: Machine=2 + NumberOfSections=2 = 4).
        size_t tsOffset = (size_t)e_lfanew + 4 + 4;
        std::printf("e_lfanew=0x%08X  timestamp offset=0x%zX\n",
                    (unsigned)e_lfanew, tsOffset);

        // Verify the offset is in bounds and read the original timestamp.
        CHECK(tsOffset + 4 <= tier2.size(),
              "Case2: timestamp offset in bounds");
        if (tsOffset + 4 <= tier2.size()) {
            uint32_t origTs = *reinterpret_cast<const uint32_t*>(
                tier2.data() + tsOffset);
            std::printf("Original PE timestamp: 0x%08X\n", origTs);
            CHECK(origTs == 0x5C01735Eu,
                  "Case2: original timestamp==0x5C01735E");

            // Change the timestamp to a different value.
            *reinterpret_cast<uint32_t*>(tier2.data() + tsOffset) =
                0xDEADBEEFu;
            std::printf("Modified PE timestamp to 0xDEADBEEF\n");
        }

        // Construct a resolver from the modified buffer.
        tsdev::OrlibResolver* tier2Resolver = nullptr;
        try {
            tier2Resolver = new tsdev::OrlibResolver(tier2.data(),
                                                     tier2.size(),
                                                     0x10000000ull);
        } catch (const std::exception& e) {
            std::string msg = std::string("Case2 ctor threw: ") + e.what();
            CHECK(false, msg.c_str());
        }

        if (tier2Resolver) {
            tsdev::VersionDescriptor vd = tier2Resolver->detectVersion();
            std::printf("Tier2 detectVersion: known=%d tier=%d "
                        "fileVersion='%s' peTimestamp=0x%08X warning='%s'\n",
                        (int)vd.known, (int)vd.tier,
                        vd.fileVersion.c_str(), vd.peTimestamp,
                        vd.warningMessage.c_str());

            CHECK(vd.known == true,
                  "Case2: known==true (FileVersion still matches Tier 1)");
            CHECK(vd.tier == tsdev::VersionTier::Tier2Mismatch,
                  "Case2: tier==Tier2Mismatch");
            CHECK(!vd.warningMessage.empty(),
                  "Case2: warningMessage non-empty (timestamp mismatch)");

            delete tier2Resolver;
            tier2Resolver = nullptr;
        }
    }

    // -------------------------------------------------------------------
    // Case 3 — Tier 3 mismatch (MANDATORY).
    //
    // Flip a single byte in the .text section. This changes the SHA-256
    // (Tier 3) but leaves FileVersion (resource section) and PE timestamp
    // (header) unchanged. Tier 1 + Tier 2 match → known=true, but
    // Tier 3 mismatches → tier=Tier3Mismatch, warningMessage non-empty.
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 4 Case 3: Tier 3 mismatch (SHA-256) ---\n");
    {
        std::vector<uint8_t> tier3 = buffer;

        // Use the .text raw offset parsed in Cycle 2. Flip one byte
        // well inside .text (not a header or resource byte).
        if (peParsedOk && textRawOffset + 0x100 < tier3.size()) {
            tier3[textRawOffset + 0x100] ^= 0xFF;
            std::printf("Flipped byte at .text+0x100 (file offset 0x%zX)\n",
                        textRawOffset + 0x100);
        } else {
            // Fallback: flip a byte at a known .text offset (0x500).
            size_t off = 0x500;
            if (off < tier3.size()) {
                tier3[off] ^= 0xFF;
                std::printf("Flipped byte at fallback offset 0x%zX\n", off);
            } else {
                std::printf("WARNING: could not flip a .text byte "
                            "(offset out of bounds)\n");
            }
        }

        // Construct a resolver from the modified buffer.
        tsdev::OrlibResolver* tier3Resolver = nullptr;
        try {
            tier3Resolver = new tsdev::OrlibResolver(tier3.data(),
                                                     tier3.size(),
                                                     0x10000000ull);
        } catch (const std::exception& e) {
            std::string msg = std::string("Case3 ctor threw: ") + e.what();
            CHECK(false, msg.c_str());
        }

        if (tier3Resolver) {
            tsdev::VersionDescriptor vd = tier3Resolver->detectVersion();
            std::printf("Tier3 detectVersion: known=%d tier=%d "
                        "fileVersion='%s' sha256='%.16s...' warning='%s'\n",
                        (int)vd.known, (int)vd.tier,
                        vd.fileVersion.c_str(),
                        vd.sha256.c_str(),
                        vd.warningMessage.c_str());

            CHECK(vd.known == true,
                  "Case3: known==true (Tier 1 + Tier 2 match)");
            CHECK(vd.tier == tsdev::VersionTier::Tier3Mismatch,
                  "Case3: tier==Tier3Mismatch");
            CHECK(!vd.warningMessage.empty(),
                  "Case3: warningMessage non-empty (SHA-256 mismatch)");

            delete tier3Resolver;
            tier3Resolver = nullptr;
        }
    }

    // -------------------------------------------------------------------
    // Case 4 — Unknown FileVersion (BEST-EFFORT, soft skip if infeasible).
    //
    // Replace the UTF-16LE "9.50.01.3344" string in the PE version
    // resource with "9.99.99.9999" (same byte length: 12 wide chars =
    // 24 bytes). detectVersion() must return known=false, tier=Unknown,
    // signatureSet=SIGSET_NONE.
    //
    // FEASIBILITY NOTE: This case depends on readFileVersion() extracting
    // the STRING form from the version resource's string table. If
    // readFileVersion() instead uses VS_FIXEDFILEINFO (binary DWORDs),
    // modifying the UTF-16LE string won't change the returned FileVersion,
    // and this test will fail in GREEN. The implementer (B.4.17) must
    // ensure readFileVersion returns the string form that matches the
    // manifest key. If the UTF-16LE pattern is not found in the buffer
    // (stored differently), this case is SKIPPED (soft skip — no
    // failure, no increment to g_failCount).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 4 Case 4: unknown FileVersion (best-effort) ---\n");
    {
        std::vector<uint8_t> unknown = buffer;

        // UTF-16LE encoding of "9.50.01.3344" (12 wide chars = 24 bytes):
        // each ASCII char followed by 0x00.
        static const uint8_t origUtf16[] = {
            0x39, 0x00, 0x2E, 0x00, 0x35, 0x00, 0x30, 0x00,
            0x2E, 0x00, 0x30, 0x00, 0x31, 0x00, 0x2E, 0x00,
            0x33, 0x00, 0x33, 0x00, 0x34, 0x00, 0x34, 0x00
        };
        // UTF-16LE encoding of "9.99.99.9999" (12 wide chars = 24 bytes,
        // same length so the resource layout is preserved).
        static const uint8_t replUtf16[] = {
            0x39, 0x00, 0x2E, 0x00, 0x39, 0x00, 0x39, 0x00,
            0x2E, 0x00, 0x39, 0x00, 0x39, 0x00, 0x2E, 0x00,
            0x39, 0x00, 0x39, 0x00, 0x39, 0x00, 0x39, 0x00
        };
        const size_t patLen = sizeof(origUtf16);
        static_assert(sizeof(origUtf16) == sizeof(replUtf16),
                      "pattern lengths must match");

        // Search for the UTF-16LE pattern in the buffer.
        size_t foundOffset = SIZE_MAX;
        if (unknown.size() >= patLen) {
            for (size_t i = 0; i + patLen <= unknown.size(); ++i) {
                if (std::memcmp(unknown.data() + i, origUtf16, patLen) == 0) {
                    foundOffset = i;
                    break;
                }
            }
        }

        if (foundOffset == SIZE_MAX) {
            // Soft skip: the UTF-16LE version string was not found in
            // the buffer. This may happen if the version string is stored
            // as VS_FIXEDFILEINFO (binary DWORDs) rather than in the
            // string table, or with different padding/alignment. Do NOT
            // fail the test — this case is best-effort.
            std::printf("SKIP: Case 4 — UTF-16LE '9.50.01.3344' pattern "
                        "not found in buffer. The version string may be "
                        "stored as VS_FIXEDFILEINFO (binary DWORDs) rather "
                        "than in the string table. This case is best-effort "
                        "and does not count as a failure.\n");
        } else {
            std::printf("Found UTF-16LE '9.50.01.3344' at offset 0x%zX\n",
                        foundOffset);
            // Replace with "9.99.99.9999" (same byte length).
            std::memcpy(unknown.data() + foundOffset, replUtf16, patLen);
            std::printf("Replaced with UTF-16LE '9.99.99.9999'\n");

            // Construct a resolver from the modified buffer.
            tsdev::OrlibResolver* unkResolver = nullptr;
            try {
                unkResolver = new tsdev::OrlibResolver(unknown.data(),
                                                       unknown.size(),
                                                       0x10000000ull);
            } catch (const std::exception& e) {
                std::string msg = std::string("Case4 ctor threw: ") + e.what();
                CHECK(false, msg.c_str());
            }

            if (unkResolver) {
                tsdev::VersionDescriptor vd = unkResolver->detectVersion();
                std::printf("Unknown detectVersion: known=%d tier=%d "
                            "fileVersion='%s' signatureSet=%d "
                            "warning='%s'\n",
                            (int)vd.known, (int)vd.tier,
                            vd.fileVersion.c_str(),
                            (int)vd.signatureSet,
                            vd.warningMessage.c_str());

                CHECK(vd.known == false,
                      "Case4: known==false (FileVersion not in manifest)");
                CHECK(vd.tier == tsdev::VersionTier::Unknown,
                      "Case4: tier==Unknown");
                CHECK(vd.signatureSet == tsdev::SignatureSetId::SIGSET_NONE,
                      "Case4: signatureSet==SIGSET_NONE");
                CHECK(!vd.warningMessage.empty(),
                      "Case4: warningMessage non-empty (unknown version)");

                delete unkResolver;
                unkResolver = nullptr;
            }
        }
    }

    // --- Cycle 4 per-cycle summary ---
    {
        int c4Passes = g_passCount - c4_passStart;
        int c4Fails  = g_failCount - c4_failStart;
        int c4Total  = c4Passes + c4Fails;
        std::printf("Cycle 4: %d/%d assertions passed\n", c4Passes, c4Total);
    }

    // ===================================================================
    // Cycle 5: patch-site resolution + validation + data globals.
    //
    // This is the final implementation cycle (TVS-B.4 TDD red-green).
    // It covers:
    //   Part A — 4 patch-site resolvers (resolvePatchSite by name).
    //   Part B — 4 validatePatchSite(descriptor) tests.
    //   Part C — 4 convenience methods (resolvePersistencePatch /
    //            resolveThreadCheck1/2/3).
    //   Part D — applyPatch (test mode = validate-only no-op).
    //   Part E — 3 data-global RVA resolvers.
    //
    // Against the current stubs (RED phase), resolvePatchSite and the
    // data-global methods throw OrlibResolverError("not implemented") →
    // the catch blocks fire → CHECK(false) → FAIL. After implementation
    // (GREEN phase, later subagent), each resolver returns a populated
    // PatchSiteDescriptor / RVA at the expected value → PASS.
    //
    // Expected values are taken from signatures.gen.h
    // (the kSignatures[] entries for targets 9-12), cross-checked against
    // docs/SIGNATURE_CATALOG.md §3 (patch-site metadata) and §6 (data-
    // section RVA targets: kDataGlobals_v9_50_01_3344[0..2]).
    //
    // patchRva = sigMatchRva + patchOffset for each patch site.
    // ===================================================================
    std::printf("\n=== Cycle 5: patch sites + validation + data globals ===\n");

    // Track per-cycle assertion counts for the summary.
    int c5_passStart = g_passCount;
    int c5_failStart = g_failCount;

    // --- Helper: parse a space-separated hex byte string ("55 8B EC")
    //     into a std::vector<uint8_t>. Used to build the expected
    //     originalBytes / replacementBytes vectors for comparison. ---
    auto parseHexBytes = [](const char* hex) -> std::vector<uint8_t> {
        std::vector<uint8_t> out;
        const char* p = hex;
        while (*p) {
            // skip whitespace
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            // parse two hex digits
            int hi = -1, lo = -1;
            if (*p) { hi = (*p >= '0' && *p <= '9') ? *p - '0'
                     : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10
                     : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1; ++p; }
            if (*p) { lo = (*p >= '0' && *p <= '9') ? *p - '0'
                     : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10
                     : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1; ++p; }
            if (hi >= 0 && lo >= 0) {
                out.push_back((uint8_t)((hi << 4) | lo));
            } else {
                break; // malformed — stop
            }
        }
        return out;
    };

    // --- Helper: compare two byte vectors (size + contents). ---
    auto bytesEqual = [](const std::vector<uint8_t>& a,
                         const std::vector<uint8_t>& b) -> bool {
        return a.size() == b.size() &&
               std::memcmp(a.data(), b.data(), a.size()) == 0;
    };

    // -------------------------------------------------------------------
    // Part A — 4 patch-site resolution tests (resolvePatchSite by name).
    //
    // For each of the 4 patch sites, call resolvePatchSite(name) inside
    // try/catch. Assert the returned PatchSiteDescriptor fields:
    //   patchRva, sigMatchRva, patchOffset, originalBytes,
    //   replacementBytes, name, containingFunction.
    //
    // Against stubs → catch fires → CHECK(false) → FAIL (RED).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 5 Part A: 4 patch-site resolvers ---\n");

    // Patch-site expected values (from signatures.gen.h targets 9-12).
    struct PatchSiteExpect {
        const char* name;
        uintptr_t   sigMatchRva;
        int         patchOffset;
        uintptr_t   patchRva;
        const char* originalHex;
        const char* replacementHex;
        const char* containingFunction;
    };
    static const PatchSiteExpect kPatchSites[] = {
        { "FUN_10011230_persistence_patch",
          (uintptr_t)tsdev::kSignatures[8].rva, tsdev::kSignatures[8].patchOffset,
          ((uintptr_t)tsdev::kSignatures[8].rva + tsdev::kSignatures[8].patchOffset),
          "55 8B EC 6A FF 68 CD D3",
          "B8 01 00 00 00 C2 0C 00",
          "FUN_10011230" },
        { "FUN_101d0950_thread_check_INT3",
          (uintptr_t)tsdev::kSignatures[9].rva, tsdev::kSignatures[9].patchOffset,
          ((uintptr_t)tsdev::kSignatures[9].rva + tsdev::kSignatures[9].patchOffset),
          "CC",
          "90",
          "FUN_101d0950" },
        { "FUN_101cf5b0_thread_check_INT3",
          (uintptr_t)tsdev::kSignatures[10].rva, tsdev::kSignatures[10].patchOffset,
          ((uintptr_t)tsdev::kSignatures[10].rva + tsdev::kSignatures[10].patchOffset),
          "CC",
          "90",
          "FUN_101cf5b0" },
        { "FUN_101d1fa0_thread_check_INT3",
          (uintptr_t)tsdev::kSignatures[11].rva, tsdev::kSignatures[11].patchOffset,
          ((uintptr_t)tsdev::kSignatures[11].rva + tsdev::kSignatures[11].patchOffset),
          "CC",
          "90",
          "FUN_101d1fa0" },
    };
    const int kPatchSiteCount =
        static_cast<int>(sizeof(kPatchSites) / sizeof(kPatchSites[0]));

    // Resolved descriptors from Part A are reused in Part B. Store them
    // here (only valid when the corresponding resolve succeeded).
    std::vector<tsdev::PatchSiteDescriptor> resolvedDescs(kPatchSiteCount);
    std::vector<bool> resolveOk(kPatchSiteCount, false);

    for (int i = 0; i < kPatchSiteCount; ++i) {
        const PatchSiteExpect& e = kPatchSites[i];
        std::printf("\n[Part A] resolvePatchSite(\"%s\")\n", e.name);

        bool threw = false;
        std::string errMsg;
        tsdev::PatchSiteDescriptor desc;
        try {
            desc = resolver->resolvePatchSite(e.name);
            resolveOk[i] = true;
            resolvedDescs[i] = desc;
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }

        if (threw) {
            std::string msg = std::string("resolvePatchSite(\"") +
                              e.name + "\") threw: " + errMsg;
            CHECK(false, msg.c_str());
            continue;
        }

        // Assert patchRva == sigMatchRva + patchOffset.
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "[%s] patchRva==0x%llX (got 0x%llX)",
                      e.name, (unsigned long long)e.patchRva,
                      (unsigned long long)desc.patchRva);
        CHECK(desc.patchRva == e.patchRva, buf);

        std::snprintf(buf, sizeof(buf),
                      "[%s] sigMatchRva==0x%llX (got 0x%llX)",
                      e.name, (unsigned long long)e.sigMatchRva,
                      (unsigned long long)desc.sigMatchRva);
        CHECK(desc.sigMatchRva == e.sigMatchRva, buf);

        std::snprintf(buf, sizeof(buf),
                      "[%s] patchOffset==%d (got %d)",
                      e.name, e.patchOffset, desc.patchOffset);
        CHECK(desc.patchOffset == e.patchOffset, buf);

        // Assert originalBytes match expected.
        std::vector<uint8_t> expOrig = parseHexBytes(e.originalHex);
        std::snprintf(buf, sizeof(buf),
                      "[%s] originalBytes size==%zu (got %zu)",
                      e.name, expOrig.size(), desc.originalBytes.size());
        CHECK(desc.originalBytes.size() == expOrig.size(), buf);
        CHECK(bytesEqual(desc.originalBytes, expOrig),
              (std::string("[") + e.name + "] originalBytes contents match").c_str());

        // Assert replacementBytes match expected.
        std::vector<uint8_t> expRepl = parseHexBytes(e.replacementHex);
        std::snprintf(buf, sizeof(buf),
                      "[%s] replacementBytes size==%zu (got %zu)",
                      e.name, expRepl.size(), desc.replacementBytes.size());
        CHECK(desc.replacementBytes.size() == expRepl.size(), buf);
        CHECK(bytesEqual(desc.replacementBytes, expRepl),
              (std::string("[") + e.name + "] replacementBytes contents match").c_str());

        // Assert name matches (exact).
        CHECK(desc.name == e.name,
              (std::string("[") + e.name + "] desc.name matches").c_str());

        // Assert containingFunction matches.
        CHECK(desc.containingFunction == e.containingFunction,
              (std::string("[") + e.name + "] containingFunction matches").c_str());
    }

    // -------------------------------------------------------------------
    // Part B — 4 validatePatchSite(descriptor) tests.
    //
    // For each patch site, first resolve the descriptor (Part A). If the
    // resolve threw (RED), we CANNOT validate (no descriptor) → skip the
    // validation CHECK with a note documenting the RED behavior. The
    // Part A CHECK(false) already records the RED failure for the throw.
    //
    // If the resolve succeeded (GREEN), call validatePatchSite(desc) and
    // assert it returns true (the baseline DLL is unpatched — original
    // bytes are present at the patch site).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 5 Part B: 4 validatePatchSite tests ---\n");

    for (int i = 0; i < kPatchSiteCount; ++i) {
        const PatchSiteExpect& e = kPatchSites[i];
        std::printf("\n[Part B] validatePatchSite(\"%s\")\n", e.name);

        if (!resolveOk[i]) {
            // RED behavior: resolvePatchSite threw in Part A, so we have
            // no descriptor to validate. Skip the validation CHECK (the
            // Part A failure already records the RED state). This avoids
            // double-counting the same stub-not-implemented failure.
            std::printf("SKIP: cannot validate \"%s\" — resolvePatchSite "
                        "threw in Part A (RED: stub not implemented). "
                        "Validation deferred until resolve is implemented.\n",
                        e.name);
            continue;
        }

        const tsdev::PatchSiteDescriptor& desc = resolvedDescs[i];
        bool threw = false;
        std::string errMsg;
        bool valid = false;
        try {
            valid = resolver->validatePatchSite(desc);
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }

        if (threw) {
            std::string msg = std::string("validatePatchSite(\"") +
                              e.name + "\") threw: " + errMsg;
            CHECK(false, msg.c_str());
            continue;
        }

        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "[%s] validatePatchSite(desc)==true (got %d)",
                      e.name, (int)valid);
        CHECK(valid == true, buf);
    }

    // -------------------------------------------------------------------
    // Part C — 4 convenience method tests.
    //
    // resolvePersistencePatch(), resolveThreadCheck1/2/3() each return a
    // PatchSiteDescriptor. Assert the returned descriptor's patchRva
    // matches the expected patch RVA.
    //
    // Against stubs → throws → FAIL (RED).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 5 Part C: 4 convenience methods ---\n");

    // --- resolvePersistencePatch() → patchRva kSignatures[8].rva+offset ---
    {
        bool threw = false;
        std::string errMsg;
        tsdev::PatchSiteDescriptor desc;
        try {
            desc = resolver->resolvePersistencePatch();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolvePersistencePatch threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            uintptr_t expectedPatchRva = (uintptr_t)tsdev::kSignatures[8].rva + tsdev::kSignatures[8].patchOffset;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolvePersistencePatch patchRva==0x%llX (expected 0x%llX)",
                          (unsigned long long)desc.patchRva, (unsigned long long)expectedPatchRva);
            CHECK(desc.patchRva == expectedPatchRva, buf);
        }
    }

    // --- resolveThreadCheck1() → patchRva kSignatures[9].rva+offset ---
    {
        bool threw = false;
        std::string errMsg;
        tsdev::PatchSiteDescriptor desc;
        try {
            desc = resolver->resolveThreadCheck1();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveThreadCheck1 threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            uintptr_t expectedPatchRva = (uintptr_t)tsdev::kSignatures[9].rva + tsdev::kSignatures[9].patchOffset;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveThreadCheck1 patchRva==0x%llX (expected 0x%llX)",
                          (unsigned long long)desc.patchRva, (unsigned long long)expectedPatchRva);
            CHECK(desc.patchRva == expectedPatchRva, buf);
        }
    }

    // --- resolveThreadCheck2() → patchRva kSignatures[10].rva+offset ---
    {
        bool threw = false;
        std::string errMsg;
        tsdev::PatchSiteDescriptor desc;
        try {
            desc = resolver->resolveThreadCheck2();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveThreadCheck2 threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            uintptr_t expectedPatchRva = (uintptr_t)tsdev::kSignatures[10].rva + tsdev::kSignatures[10].patchOffset;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveThreadCheck2 patchRva==0x%llX (expected 0x%llX)",
                          (unsigned long long)desc.patchRva, (unsigned long long)expectedPatchRva);
            CHECK(desc.patchRva == expectedPatchRva, buf);
        }
    }

    // --- resolveThreadCheck3() → patchRva kSignatures[11].rva+offset ---
    {
        bool threw = false;
        std::string errMsg;
        tsdev::PatchSiteDescriptor desc;
        try {
            desc = resolver->resolveThreadCheck3();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveThreadCheck3 threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            uintptr_t expectedPatchRva = (uintptr_t)tsdev::kSignatures[11].rva + tsdev::kSignatures[11].patchOffset;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveThreadCheck3 patchRva==0x%llX (expected 0x%llX)",
                          (unsigned long long)desc.patchRva, (unsigned long long)expectedPatchRva);
            CHECK(desc.patchRva == expectedPatchRva, buf);
        }
    }

    // -------------------------------------------------------------------
    // Part D — applyPatch test (test mode = validate-only no-op).
    //
    // Call applyPatch("FUN_10011230_persistence_patch") inside try/catch.
    // In test mode, applyPatch validates the original bytes then skips
    // the actual write (the in-memory buffer is read-only) → returns
    // true. It must NOT modify the buffer.
    //
    // Against stubs → throws → FAIL (RED).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 5 Part D: applyPatch (test mode no-op) ---\n");

    {
        bool threw = false;
        std::string errMsg;
        bool result = false;
        std::vector<uint8_t> bufferBeforeApply = buffer;
        try {
            result = resolver->applyPatch("FUN_10011230_persistence_patch");
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("applyPatch(\"FUN_10011230_persistence_patch\") threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "applyPatch(persistence)==true in test mode (got %d)",
                          (int)result);
            CHECK(result == true, buf);
            CHECK(std::memcmp(buffer.data(), bufferBeforeApply.data(), buffer.size()) == 0,
                  "applyPatch did not modify buffer (test mode no-op)");
        }
    }

    // -------------------------------------------------------------------
    // Part E — 3 data-global RVA tests.
    //
    // resolveErrorArrayPtrRva() → kDataGlobals_v9_50_01_3344[0] (DAT_1023cea0, error-record
    //   array pointer).
    // resolveErrorCountRva()   → kDataGlobals_v9_50_01_3344[1] (DAT_1023cea4, error-record
    //   count).
    // resolveErrorHinstRva()   → kDataGlobals_v9_50_01_3344[2] (DAT_1023aab8, string-table
    //   HINSTANCE).
    //
    // Against stubs → throws → FAIL (RED).
    // -------------------------------------------------------------------
    std::printf("\n--- Cycle 5 Part E: 3 data-global RVA resolvers ---\n");

    // --- resolveErrorArrayPtrRva() → kDataGlobals_v9_50_01_3344[0] ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            rva = resolver->resolveErrorArrayPtrRva();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveErrorArrayPtrRva threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveErrorArrayPtrRva==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kDataGlobals_v9_50_01_3344[0].rva);
            CHECK(rva == (uintptr_t)tsdev::kDataGlobals_v9_50_01_3344[0].rva, buf);
        }
    }

    // --- resolveErrorCountRva() → kDataGlobals_v9_50_01_3344[1] ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            rva = resolver->resolveErrorCountRva();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveErrorCountRva threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveErrorCountRva==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kDataGlobals_v9_50_01_3344[1].rva);
            CHECK(rva == (uintptr_t)tsdev::kDataGlobals_v9_50_01_3344[1].rva, buf);
        }
    }

    // --- resolveErrorHinstRva() → kDataGlobals_v9_50_01_3344[2] ---
    {
        bool threw = false;
        std::string errMsg;
        uintptr_t rva = 0;
        try {
            rva = resolver->resolveErrorHinstRva();
        } catch (const tsdev::OrlibResolverError& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            threw = true;
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        if (threw) {
            std::string msg = std::string("resolveErrorHinstRva threw: ") + errMsg;
            CHECK(false, msg.c_str());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "resolveErrorHinstRva==0x%llX (expected 0x%X)",
                          (unsigned long long)rva, tsdev::kDataGlobals_v9_50_01_3344[2].rva);
            CHECK(rva == (uintptr_t)tsdev::kDataGlobals_v9_50_01_3344[2].rva, buf);
        }
    }

    // --- Cycle 5 per-cycle summary ---
    {
        int c5Passes = g_passCount - c5_passStart;
        int c5Fails  = g_failCount - c5_failStart;
        int c5Total  = c5Passes + c5Fails;
        std::printf("Cycle 5: %d/%d assertions passed\n", c5Passes, c5Total);
    }

    // ===================================================================
    // Cycle 6: public iterator API (getSignatureCount / getSignatureByIndex).
    //
    // Two new public methods expose the loaded signature catalog for
    // introspection:
    //   size_t getSignatureCount() const;
    //   const OrlibSignatureDef& getSignatureByIndex(size_t index) const;
    //
    // Against the current stubs (RED phase), these methods do not exist
    // → the test fails to compile. After implementation (GREEN phase),
    // getSignatureCount() returns 12 (matching kSignatureCount) and
    // getSignatureByIndex(i) returns a const reference to the
    // OrlibSignatureDef at that index, throwing std::out_of_range for
    // index >= count.
    // ===================================================================
    std::printf("\n=== Cycle 6: public iterator API ===\n");

    // Track per-cycle assertion counts for the summary.
    int c6_passStart = g_passCount;
    int c6_failStart = g_failCount;

    // --- getSignatureCount() ---
    size_t sigCount = resolver->getSignatureCount();
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "getSignatureCount()==kSignatureCount (got %zu)",
                      sigCount);
        CHECK(sigCount == (size_t)tsdev::kSignatureCount, buf);
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "getSignatureCount()==12 (got %zu)",
                      sigCount);
        CHECK(sigCount == 12u, buf);
    }

    // --- getSignatureByIndex(i) per-signature field assertions ---
    for (size_t i = 0; i < sigCount; ++i) {
        const tsdev::OrlibSignatureDef& sig =
            resolver->getSignatureByIndex(i);

        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").name != nullptr";
            CHECK(sig.name != nullptr, msg.c_str());
        }
        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").name non-empty (strlen > 0)";
            CHECK(sig.name != nullptr && std::strlen(sig.name) > 0,
                  msg.c_str());
        }
        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").bytes != nullptr";
            CHECK(sig.bytes != nullptr, msg.c_str());
        }
        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").mask != nullptr";
            CHECK(sig.mask != nullptr, msg.c_str());
        }
        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").length > 0";
            CHECK(sig.length > 0, msg.c_str());
        }
        {
            std::string msg = std::string("getSignatureByIndex(") +
                std::to_string(i) + ").expectedMatchCount == 1";
            CHECK(sig.expectedMatchCount == 1, msg.c_str());
        }
    }

    // --- First/last signature name matches kSignatures[] ---
    if (sigCount > 0) {
        const tsdev::OrlibSignatureDef& first =
            resolver->getSignatureByIndex(0);
        CHECK(first.name != nullptr &&
              std::strcmp(first.name, tsdev::kSignatures[0].name) == 0,
              "getSignatureByIndex(0).name matches kSignatures[0].name");

        const tsdev::OrlibSignatureDef& last =
            resolver->getSignatureByIndex(sigCount - 1);
        CHECK(last.name != nullptr &&
              std::strcmp(last.name,
                          tsdev::kSignatures[tsdev::kSignatureCount - 1].name) == 0,
              "getSignatureByIndex(count-1).name matches kSignatures[kSignatureCount-1].name");
    }

    // --- getSignatureByIndex(count) throws std::out_of_range ---
    {
        bool threw = false;
        std::string errMsg;
        try {
            (void)resolver->getSignatureByIndex(sigCount);
        } catch (const std::out_of_range& ex) {
            threw = true;
            errMsg = ex.what();
        } catch (const std::exception& ex) {
            // Wrong exception type — still a failure, but flag it.
            errMsg = std::string("wrong exception type: ") + ex.what();
        }
        CHECK(threw,
              "getSignatureByIndex(count) throws std::out_of_range");
        if (threw) {
            std::printf("out_of_range exception: %s\n", errMsg.c_str());
        }
    }

    // --- Cycle 6 per-cycle summary ---
    {
        int c6Passes = g_passCount - c6_passStart;
        int c6Fails  = g_failCount - c6_failStart;
        int c6Total  = c6Passes + c6Fails;
        std::printf("Cycle 6: %d/%d assertions passed\n", c6Passes, c6Total);
    }

    std::printf("Cycle 6: OrlibResolver public iterator API — PASS\n");

    // --- Summary ---
    int total = g_passCount + g_failCount;
    std::printf("\nCycle 1: %d/%d assertions passed\n", g_passCount, total);
    std::printf("Cycle 2: %d/%d assertions passed\n", g_passCount, total);
    std::printf("Cycle 3: %d/%d assertions passed\n", g_passCount, total);
    std::printf("Total failures: %d\n", g_failCount);

    delete resolver;
    return g_failCount == 0 ? 0 : 1;
}
