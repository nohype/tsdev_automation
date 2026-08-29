// diag_m4_subtype.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: read master_record[+0xe9] (the subtype byte) for CLI-created and
// existing TSDev items, to verify the theory that
//   FUN_101d4280(category, master_record[+0xe9]) -> version field
//   - For category 0 (Strategy): returns 0x200 when +0xe9 is 2 or 3,
//                                 returns 0 when +0xe9 is 0 or 1.
//   - For other categories: unknown (parallel Ghidra subagent investigating).
//
// We need the actual +0xe9 value for:
//   - CLI-created items (expected: 0, since CLI passes subtype=0 to
//     CreateEntryEX).
//   - Existing TSDev-created items (some have version=0x200, so +0xe9 should
//     be 2 or 3 if the theory holds).
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB
//      stack worker thread.
//   2. Resolves Ordinal_28, Ordinal_69, Ordinal_70, _LoadMasterRecsEX@16.
//   3. Inserts + verifies a Function "TS_AUTO_M4_SUB_FUNC" with source
//      `TS_AUTO_M4_SUB_FUNC = 1;`. Reads +0xe9 via Ordinal_28(1, progId, 0,
//      buf). Prints "CLI Function TS_AUTO_M4_SUB_FUNC ProgId=X +0xe9=Y".
//   4. Inserts + verifies a Strategy "TS_AUTO_M4_SUB_STRAT" with source
//      `Buy next bar at Market;`. Reads +0xe9 via Ordinal_28(0, progId, 0,
//      buf). Prints "CLI Strategy TS_AUTO_M4_SUB_STRAT ProgId=X +0xe9=Y".
//   5. Enumerates EXISTING items (read-only, no modification):
//        - First 10 existing Strategies (category 0) via
//          _LoadMasterRecsEX@16(0, &buf, NULL, 1). For each, reads +0xe9
//          via Ordinal_28(0, progId, 0, buf). Prints
//          "Existing Strategy ProgId=X +0xe9=Y".
//        - First 10 existing Functions (category 4, built-in; user-created
//          Functions are now category 1) via
//          _LoadMasterRecsEX@16(4, &buf, NULL, 1). Same pattern.
//        - First 10 existing Indicators (category 2) via
//          _LoadMasterRecsEX@16(2, &buf, NULL, 1). Same pattern.
//      IMPORTANT: Do NOT modify any existing items. Read-only.
//   6. Hex dump comparison: for one existing Strategy and one existing
//      Function, print a hex dump of bytes 0xe0-0xf5 from the master record
//      (via Ordinal_28), so we can compare the subtype/version area between
//      CLI-created and existing items.
//   7. Cleanup: delete TS_AUTO_M4_SUB_FUNC and TS_AUTO_M4_SUB_STRAT.
//      Verify their .ell files are gone.
//   8. Check for TSCrashReport.exe before and after.
//
// All orlib20 calls are SEH-protected (__try/__except). Its 120-second limit
// is an operational timeout, not a measured completion time or M.4 acceptance
// evidence.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

using namespace tsdev;
using diag_m4_process::ProcessInspectionResult;

namespace {

ProcessInspectionResult reportCrashReporter(const char* phase) {
    std::printf("[%s] Checking for TSCrashReport.exe...\n", phase);
    const ProcessInspectionResult result =
        diag_m4_process::inspectProcessByName(L"TSCrashReport.exe");
    if (result == ProcessInspectionResult::Found) {
        std::printf("[%s] TSCrashReport.exe IS running (crash report detected)\n",
                    phase);
    } else if (result == ProcessInspectionResult::NotFound) {
        std::printf("[%s] TSCrashReport.exe NOT running (no crash report detected)\n",
                    phase);
    } else {
        std::fprintf(stderr,
                     "[%s] FATAL: process enumeration inspection failure for "
                     "TSCrashReport.exe\n",
                     phase);
    }
    return result;
}

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args, RET 0x10.
//   uint (int category, int progId, int flag, void* outBuf)
//   Writes a 0x10A-byte partial copy into outBuf (includes +0xe9).
//   Returns 1 if record found and copied, 0 otherwise.
//
// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args (caller cleans 12 bytes).
//   void* (char category, int progId, int flag)
//   Returns live record pointer (refcounted — release via Ordinal_70).
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args, RET 0x08.
//   int (void* rec, int flag)
//
// _LoadMasterRecsEX@16 (ord 568): __stdcall, 4 args, RET 0x10.
//   int (int category, HGLOBAL* outBuf, const char* path, int filterMode)
//   Returns count; allocates HGLOBAL of count * 0x10A bytes.
// -------------------------------------------------------------------------
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef void* (__cdecl  *Ordinal69Fn)(char category, int progId, int flag);
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef int   (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                              const char* path,
                                              int filterMode);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct Ord28SehResult {
    unsigned int result;  // 0xFFFFFFFF if SEH
};

bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try {
        out->result = fn(category, progId, flag, outBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord69SehResult {
    void* rec;
};

bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               Ord69SehResult* out) {
    out->rec = nullptr;
    __try {
        out->rec = fn(category, progId, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Ord70SehResult {
    int result;
};

bool callOrd70(Ordinal70Fn fn, void* rec, int flag, Ord70SehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(rec, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct LoadMasterRecsExSehResult {
    int result;  // -1 if SEH
};

bool callLoadMasterRecsEx(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                          const char* path, int filterMode,
                          LoadMasterRecsExSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(category, outBuf, path, filterMode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers (POD-only).
// -------------------------------------------------------------------------
bool sehReadByte(const void* p, unsigned char* out) {
    *out = 0;
    __try { *out = *(const unsigned char*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehReadShort(const void* p, short* out) {
    *out = 0;
    __try { *out = *(const short*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool sehMemcpy(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// -------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// EasyLanguage source builders.
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

// Simplest possible Strategy source — a single market order.
std::string makeStrategySourceSimple() {
    return std::string("Buy next bar at Market;\r\n");
}

// Fallback Strategy source (proven to compile in prior diags).
std::string makeStrategySourceValue1(const std::string& funcName) {
    return std::string("{ calls ") + funcName + " }\r\n"
           "Value1 = " + funcName + ";\r\n"
           "Buy (\"EnterLong\") next bar at Market;\r\n";
}

void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
}

void preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
}

int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    const char* label) {
    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                label, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0");
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false");
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors");
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", label, progId);
    return progId;
}

// -------------------------------------------------------------------------
// Read +0xe9 from a master record via Ordinal_28(category, progId, 0, buf).
// Returns the byte value, or -1 on failure (SEH, not found, or NULL fn).
// -------------------------------------------------------------------------
int readSubtypeByteViaOrd28(Ordinal28Fn ord28, int category, int progId,
                            unsigned char* outBuf0x10A) {
    if (!ord28) return -1;
    Ord28SehResult r;
    if (!callOrd28(ord28, category, progId, /*flag=*/0, outBuf0x10A, &r)) {
        return -2;  // SEH
    }
    if (r.result != 1) {
        return -3;  // not found / not copied
    }
    unsigned char b = 0;
    if (!sehReadByte(outBuf0x10A + 0xe9, &b)) {
        return -4;  // SEH reading byte
    }
    return (int)b;
}

// -------------------------------------------------------------------------
// Hex dump of a byte buffer (canonical 16-byte rows with ASCII gutter).
// -------------------------------------------------------------------------
void hexDumpRange(const unsigned char* buf, size_t start, size_t end,
                  const char* label) {
    if (start >= end) return;
    size_t n = end - start;
    std::printf("\n  --- HEX DUMP: %s (offsets 0x%02zx-0x%02zx, %zu bytes) "
                "---\n", label, start, end - 1, n);
    // Round start down to 16-byte boundary for canonical alignment.
    size_t base = start & ~((size_t)0xF);
    size_t endAligned = (end + 0xF) & ~((size_t)0xF);
    for (size_t i = base; i < endAligned; i += 16) {
        std::printf("  %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            size_t off = i + j;
            if (off >= start && off < end) {
                std::printf("%02x ", buf[off]);
            } else if (off < endAligned) {
                std::printf("   ");
            }
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16; j++) {
            size_t off = i + j;
            if (off >= start && off < end) {
                unsigned char c = buf[off];
                std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
            } else {
                std::printf(" ");
            }
        }
        std::printf("|\n");
    }
}

// -------------------------------------------------------------------------
// Enumerate existing records in a category via _LoadMasterRecsEX@16.
// Returns a vector of (progId, subtype, name) for the first `maxCount`
// records. Read-only — does NOT modify any record.
// -------------------------------------------------------------------------
struct ExistingRec {
    short progId = 0;
    unsigned char subType = 0;
    char name[64] = {};  // inline long name at +0x17 (null-terminated)
};

std::vector<ExistingRec> enumerateExisting(LoadMasterRecsExFn loadFn,
                                           int category, int maxCount,
                                           const char* catLabel) {
    std::vector<ExistingRec> result;
    if (!loadFn) {
        std::printf("  [_LoadMasterRecsEX@16 NOT resolved for %s]\n",
                    catLabel);
        return result;
    }

    HGLOBAL buf = NULL;
    LoadMasterRecsExSehResult seh;
    if (!callLoadMasterRecsEx(loadFn, category, &buf,
                              (const char*)NULL, /*filterMode=*/1, &seh)) {
        std::printf("  [_LoadMasterRecsEX@16 raised SEH for %s]\n",
                    catLabel);
        return result;
    }
    int count = seh.result;
    std::printf("  [_LoadMasterRecsEX@16(%d) returned count=%d, buf=%p]\n",
                category, count, (void*)buf);
    if (count <= 0 || buf == NULL) {
        if (buf) GlobalFree(buf);
        return result;
    }

    char* records = (char*)GlobalLock(buf);
    if (!records) {
        std::printf("  [GlobalLock FAILED for %s buf]\n", catLabel);
        GlobalFree(buf);
        return result;
    }

    const int kRecordSize   = 0x10A;
    const int kNameOffset   = 0x17;
    const int kProgIdOffset = 0x00;
    const int kSubTypeOffset = 0xE9;

    int collected = 0;
    for (int i = 0; i < count && collected < maxCount; i++) {
        char* rec = records + (size_t)i * kRecordSize;
        ExistingRec er;
        if (!sehReadShort(rec + kProgIdOffset, &er.progId)) continue;
        if (er.progId <= 0) continue;
        if (!sehReadByte(rec + kSubTypeOffset, &er.subType)) continue;
        // Copy inline name (null-terminated, max 63 bytes to fit our buffer).
        const char* namePtr = rec + kNameOffset;
        size_t nameLen = 0;
        while (nameLen < 63 && namePtr[nameLen] != '\0') {
            er.name[nameLen] = namePtr[nameLen];
            nameLen++;
        }
        er.name[nameLen] = '\0';
        result.push_back(er);
        collected++;
    }

    GlobalUnlock(buf);
    GlobalFree(buf);
    return result;
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
    int funcProgId = 0;
    int stratProgId = 0;
    int funcSubType = -100;
    int stratSubType = -100;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName  = "TS_AUTO_M4_SUB_FUNC";
    const std::string stratName = "TS_AUTO_M4_SUB_STRAT";

    std::printf("=== diag_m4_subtype: master_record[+0xe9] subtype-byte "
                "diagnostic ===\n");
    std::printf("Purpose: read +0xe9 for CLI-created and existing items to "
                "verify the\n");
    std::printf("  FUN_101d4280(category, +0xe9) -> version theory "
                "(Strategy: 0x200 when\n");
    std::printf("  +0xe9 is 2 or 3, 0 when +0xe9 is 0 or 1).\n");

    // --- Step 1: construct a real TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib construction threw: %s\n",
                     e.what());
        st->exitCode = 100;
        return 0;
    }
    if (!lib) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest returned null\n");
        st->exitCode = 101;
        return 0;
    }
    std::printf("    TsDevLib constructed OK.\n");

    // --- Step 2: resolve Ordinal_28, Ordinal_69, Ordinal_70,
    //     _LoadMasterRecsEX@16. ---
    std::printf("\n[2] Resolving Ordinal_28 (ord 28), Ordinal_69 (ord 69), "
                "Ordinal_70 (ord 70), _LoadMasterRecsEX@16...\n");
    Ordinal28Fn ord28 = nullptr;
    Ordinal69Fn ord69 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    LoadMasterRecsExFn loadMasterRecsEx = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        loadMasterRecsEx = reinterpret_cast<LoadMasterRecsExFn>(
            resolver.resolveExport("_LoadMasterRecsEX@16"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    Resolved: Ordinal_28=%p Ordinal_69=%p Ordinal_70=%p "
                "_LoadMasterRecsEX@16=%p\n",
                (void*)ord28, (void*)ord69, (void*)ord70,
                (void*)loadMasterRecsEx);
    if (!ord28) {
        std::fprintf(stderr, "FATAL: Ordinal_28 not resolved — cannot read "
                     "+0xe9. Aborting.\n");
        st->exitCode = 103;
        return 0;
    }

    // --- Step 3: insert + verify the Function, then read +0xe9. ---
    std::printf("\n[3] Insert + verify Function \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) {
        std::fprintf(stderr, "  [WARN] Function insert/verify failed — "
                     "skipping CLI Function +0xe9 read.\n");
    } else {
        st->funcProgId = funcProgId;
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/1,
                                         funcProgId, ord28buf);
        st->funcSubType = sv;
        std::printf("    CLI Function TS_AUTO_M4_SUB_FUNC ProgId=%d "
                    "+0xe9=%d (0x%02X)\n",
                    funcProgId, sv, (unsigned)sv);
        // Hex dump 0xe0-0xf5 for the CLI-created Function.
        if (sv >= 0) {
            hexDumpRange(ord28buf, 0xe0, 0xf5,
                         "CLI Function TS_AUTO_M4_SUB_FUNC (Ord28 copy)");
        }
    }

    // --- Step 4: insert + verify the Strategy, then read +0xe9. ---
    std::printf("\n[4] Insert + verify Strategy \"%s\"...\n",
                stratName.c_str());
    // Try the simplest Strategy source first; fall back to a form that
    // calls the Function (proven to compile in prior diags).
    int stratProgId = insertAndVerify(*lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceSimple(),
                                      "Strategy");
    if (stratProgId <= 0 && funcProgId > 0) {
        std::printf("  >> Simple Strategy source failed; trying fallback "
                    "(Value1 = FUNC; + Buy)...\n");
        stratProgId = insertAndVerify(*lib, ComponentType::Strategy,
                                      stratName,
                                      makeStrategySourceValue1(funcName),
                                      "Strategy");
    }
    if (stratProgId <= 0) {
        std::fprintf(stderr, "  [WARN] Strategy insert/verify failed — "
                     "skipping CLI Strategy +0xe9 read.\n");
    } else {
        st->stratProgId = stratProgId;
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/0,
                                         stratProgId, ord28buf);
        st->stratSubType = sv;
        std::printf("    CLI Strategy TS_AUTO_M4_SUB_STRAT ProgId=%d "
                    "+0xe9=%d (0x%02X)\n",
                    stratProgId, sv, (unsigned)sv);
        // Hex dump 0xe0-0xf5 for the CLI-created Strategy.
        if (sv >= 0) {
            hexDumpRange(ord28buf, 0xe0, 0xf5,
                         "CLI Strategy TS_AUTO_M4_SUB_STRAT (Ord28 copy)");
        }
    }

    // --- Step 5: enumerate EXISTING items (read-only) and read +0xe9. ---
    std::printf("\n[5] Enumerating EXISTING items (READ-ONLY — no "
                "modification)...\n");

    // 5a. Existing Strategies (category 0).
    std::printf("\n  --- 5a. Existing Strategies (category 0) ---\n");
    auto existingStrats = enumerateExisting(loadMasterRecsEx, /*cat=*/0,
                                            /*maxCount=*/10, "Strategy");
    for (const auto& er : existingStrats) {
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/0,
                                         er.progId, ord28buf);
        std::printf("    Existing Strategy ProgId=%d +0xe9=%d (0x%02X)  "
                    "[enum subtype=%d (0x%02X), name=\"%s\"]\n",
                    er.progId, sv, (unsigned)sv,
                    er.subType, er.subType, er.name);
    }
    if (existingStrats.empty()) {
        std::printf("    [no existing Strategies enumerated]\n");
    }

    // 5b. Existing built-in Functions (category 4); user-created Functions
    //     are now category 1.
    std::printf("\n  --- 5b. Existing Functions (category 4, built-in) ---\n");
    auto existingFuncs = enumerateExisting(loadMasterRecsEx, /*cat=*/4,
                                            /*maxCount=*/10, "Function");
    for (const auto& er : existingFuncs) {
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/4,
                                         er.progId, ord28buf);
        std::printf("    Existing Function ProgId=%d +0xe9=%d (0x%02X)  "
                    "[enum subtype=%d (0x%02X), name=\"%s\"]\n",
                    er.progId, sv, (unsigned)sv,
                    er.subType, er.subType, er.name);
    }
    if (existingFuncs.empty()) {
        std::printf("    [no existing Functions enumerated]\n");
    }

    // 5c. Existing Indicators (category 2).
    std::printf("\n  --- 5c. Existing Indicators (category 2) ---\n");
    auto existingInds = enumerateExisting(loadMasterRecsEx, /*cat=*/2,
                                           /*maxCount=*/10, "Indicator");
    for (const auto& er : existingInds) {
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/2,
                                         er.progId, ord28buf);
        std::printf("    Existing Indicator ProgId=%d +0xe9=%d (0x%02X)  "
                    "[enum subtype=%d (0x%02X), name=\"%s\"]\n",
                    er.progId, sv, (unsigned)sv,
                    er.subType, er.subType, er.name);
    }
    if (existingInds.empty()) {
        std::printf("    [no existing Indicators enumerated]\n");
    }

    // --- Step 6: Hex dump comparison of 0xe0-0xf5 for one existing
    //      Strategy and one existing Function (vs CLI-created above). ---
    std::printf("\n[6] Hex dump comparison (0xe0-0xf5) for one existing "
                "Strategy and one existing Function...\n");
    if (!existingStrats.empty()) {
        const auto& er = existingStrats[0];
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/0,
                                         er.progId, ord28buf);
        if (sv >= 0) {
            char label[128];
            std::snprintf(label, sizeof(label),
                          "Existing Strategy ProgId=%d name=\"%s\" "
                          "(Ord28 copy)", er.progId, er.name);
            hexDumpRange(ord28buf, 0xe0, 0xf5, label);
        } else {
            std::printf("  [Could not read Ord28 copy for existing Strategy "
                        "ProgId=%d (sv=%d)]\n", er.progId, sv);
        }
    } else {
        std::printf("  [No existing Strategy available for hex dump]\n");
    }
    if (!existingFuncs.empty()) {
        const auto& er = existingFuncs[0];
        unsigned char ord28buf[0x10A] = {};
        int sv = readSubtypeByteViaOrd28(ord28, /*category=*/4,
                                         er.progId, ord28buf);
        if (sv >= 0) {
            char label[128];
            std::snprintf(label, sizeof(label),
                          "Existing Function ProgId=%d name=\"%s\" "
                          "(Ord28 copy)", er.progId, er.name);
            hexDumpRange(ord28buf, 0xe0, 0xf5, label);
        } else {
            std::printf("  [Could not read Ord28 copy for existing Function "
                        "ProgId=%d (sv=%d)]\n", er.progId, sv);
        }
    } else {
        std::printf("  [No existing Function available for hex dump]\n");
    }

    // --- Step 7: cleanup. Delete CLI-created items and verify .ell gone. ---
    std::printf("\n[7] Cleanup: delete CLI-created items...\n");
    const std::string binDir = "C:\\Program Files (x86)\\TradeStation 9.5\\"
                               "MyWork\\Bin";
    if (st->stratProgId > 0) {
        std::printf("  Deleting Strategy \"%s\" (ProgId=%d)...\n",
                    stratName.c_str(), st->stratProgId);
        bool delOk = lib->deleteEntry(ComponentType::Strategy, stratName,
                                      /*flush=*/true);
        std::printf("    deleteEntry returned %s\n", delOk ? "true" : "false");
        // Verify .ell file is gone.
        std::string ell = binDir + "\\" + stratName + ".ell";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(ell.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("    [OK] .ell file gone: %s\n", ell.c_str());
        } else {
            std::printf("    [WARN] .ell file STILL EXISTS: %s\n",
                        ell.c_str());
            FindClose(h);
        }
    } else {
        std::printf("  [Strategy was not created — nothing to delete]\n");
    }
    if (st->funcProgId > 0) {
        std::printf("  Deleting Function \"%s\" (ProgId=%d)...\n",
                    funcName.c_str(), st->funcProgId);
        bool delOk = lib->deleteEntry(ComponentType::Function, funcName,
                                      /*flush=*/true);
        std::printf("    deleteEntry returned %s\n", delOk ? "true" : "false");
        std::string ell = binDir + "\\" + funcName + ".ell";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(ell.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("    [OK] .ell file gone: %s\n", ell.c_str());
        } else {
            std::printf("    [WARN] .ell file STILL EXISTS: %s\n",
                        ell.c_str());
            FindClose(h);
        }
    } else {
        std::printf("  [Function was not created — nothing to delete]\n");
    }

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("CLI Function TS_AUTO_M4_SUB_FUNC: ProgId=%d +0xe9=%d "
                "(0x%02X)\n",
                st->funcProgId, st->funcSubType,
                (unsigned)st->funcSubType);
    std::printf("CLI Strategy TS_AUTO_M4_SUB_STRAT: ProgId=%d +0xe9=%d "
                "(0x%02X)\n",
                st->stratProgId, st->stratSubType,
                (unsigned)st->stratSubType);
    std::printf("Existing Strategies enumerated: %zu\n",
                existingStrats.size());
    std::printf("Existing Functions enumerated: %zu\n",
                existingFuncs.size());
    std::printf("Existing Indicators enumerated: %zu\n",
                existingInds.size());
    std::printf("\n=== DIAGNOSIS ===\n");
    std::printf("  If CLI-created items have +0xe9=0 and existing items with "
                "version=0x200\n");
    std::printf("  have +0xe9=2 or 3, the FUN_101d4280 theory is "
                "CONFIRMED for category 0.\n");
    std::printf("  Compare the hex dumps of 0xe0-0xf5 between CLI-created and "
                "existing items\n");
    std::printf("  to see the full subtype/version area.\n");

    st->exitCode = 0;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was terminated.\n");
        return 97;
    }

    // Run on a 64 MB-stack worker thread (EasyLanguage compiler / hidden
    // EDIT control need this, and TsDevLib is thread-affine).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 120-second hard timeout — report a hang without terminating the worker.
    // Insert+verify of 2 items + enumeration of 3 categories + Ord28 reads +
    // hex dumps may take longer than 90 seconds.
    DWORD wait = WaitForSingleObject(hThread, 120000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 120s; worker was "
                     "not forcibly terminated and no process cleanup was "
                     "attempted. Manual exact-PID recovery is required.\n");
        CloseHandle(hThread);
        return 98;
    }
    if (wait == WAIT_FAILED) {
        std::fprintf(stderr,
                     "\nFATAL: waiting for diagnostic worker failed: %lu; "
                     "no thread or process was forcibly terminated.\n",
                     GetLastError());
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    if (reportCrashReporter("POST") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be inspected after the "
                     "diagnostic; no process cleanup was attempted.\n");
        return 97;
    }

    return st.exitCode;
}
